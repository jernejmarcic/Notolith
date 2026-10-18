// Headless performance harness for noteAppC. Jernej's requirement is that
// this app measurably beats Obsidian while staying CPU-only, and "nothing
// can be tuned before it is measured" - this tool only measures, on the
// offscreen QPA platform, the same way tools/shot.cpp does.
//
//   noteAppC-bench [--json] [--notes N] [--phase NAME] [vault]
//
// With no vault argument, one is generated with tools/gen_bench_vault.py
// into a temporary directory that is deleted on exit, so nothing gets
// committed by accident. A supplied vault is opened read-only; the note
// files this tool needs for the size-tiered measurements are written to a
// separate temporary directory, never into the vault itself.

#include <QApplication>
#include <QBuffer>
#include <QCommandLineParser>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <QTextStream>
#include <QWheelEvent>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include <QSurfaceFormat>

#include "Index.h"
#include "Indexer.h"
#include "Vault.h"
#include "MainWindow.h"

#ifdef NOTEAPPC_HAVE_EPUB
#include "EpubView.h"
#include <archive.h>
#include <archive_entry.h>
#endif
#include "MarkdownEditor.h"
#include "SettingsPage.h"
#include "TagsPane.h"

#ifdef NOTEAPPC_HAVE_PDFVIEW
#include "PdfDocument.h"
#include "PdfPagesArea.h"
#endif

using namespace noteAppC;

namespace {

// --- content generators -----------------------------------------------
// Same shape as generateNote() in tests/test_editor.cpp, so the numbers
// here and the budget asserted in the test are talking about the same kind
// of note. Kept as its own small copy rather than a shared header: it is
// eight lines, and test binaries and this tool build separately.

QString buildNote(int kilobytes)
{
    QString text;
    int block = 0;
    while (text.size() < kilobytes * 1024) {
        text += QStringLiteral(
                    "## Section %1\n\nA paragraph with **bold**, *italic*, "
                    "a [[Wikilink %1]], a #tag%1 and `code`. It runs long enough "
                    "to wrap across a line or two in the editor.\n\n"
                    "- a list item\n- another one\n\n")
                    .arg(block++);
    }
    return text;
}

/// A note of exactly `lines` lines: the scroll benchmark cares how many
/// blocks there are to page through, not how many bytes they take.
QString buildLongNote(int lines)
{
    QString text;
    for (int i = 0; i < lines; ++i) {
        if (i % 20 == 0)
            text += QStringLiteral("## Section %1\n\n").arg(i / 20);
        text += QStringLiteral("Line %1 of the scroll test, with **bold**, "
                               "a [[Note %1]] and `code`.\n")
                    .arg(i);
    }
    return text;
}

/// One fenced C++ listing of `lines` lines under a heading: the case a
/// list split does not help, since a fence is one block however long it is.
QString buildCodeNote(int lines)
{
    QString text = QStringLiteral("# Listing\n\n```cpp\n");
    for (int i = 0; i < lines; ++i)
        text += QStringLiteral("int value%1 = compute(%1, \"text\"); // line %1\n").arg(i);
    return text + QStringLiteral("```\n");
}

/// One note carrying 4 500 distinct tags, 1 500 of them nested under 30
/// parents: a vault that tags by habit. The Tags pane counts every one of
/// them on each index change, so it goes into the generated vault itself.
QString buildTagHeavyNote()
{
    QString text = QStringLiteral("# Tags\n\n");
    for (int i = 0; i < 3000; ++i)
        text += QStringLiteral("#topic%1 ").arg(i);
    text += QStringLiteral("\n\n");
    for (int i = 0; i < 1500; ++i)
        text += QStringLiteral("#area%1/item%2 ").arg(i % 30).arg(i);
    return text + QStringLiteral("\n");
}

/// A heading, a `query` block whose one result is already resolved and
/// cached, and a paragraph to type into - the steady state
/// docs/PERFORMANCE.md holds every rendered block to: unrelated typing must
/// not cost more for a note that happens to have one.
QString buildQueryNote()
{
    return QStringLiteral(
        "# Notes\n\n```query\ntag:#project\n```\n\nA paragraph to type into, well away from it.\n");
}

/// A heading, a `base` block whose one-row table is already resolved and
/// cached, and a paragraph to type into - buildQueryNote's own case, for the
/// other block that runs its own evaluation off the keystroke path.
QString buildBaseNote()
{
    return QStringLiteral(
        "# Notes\n\n```base\nviews:\n  - order: [file.name]\n```\n\n"
        "A paragraph to type into, well away from it.\n");
}

/// A heading, one small HTML block (rendered once and cached by litehtml, or
/// shown as source in a build with none), and a paragraph to type into - the
/// same steady-state case buildQueryNote measures for the query block.
QString buildHtmlNote()
{
    return QStringLiteral(
        "# Notes\n\n<div style=\"padding: 4px\">\n<b>Bold</b> and <span style=\"color:#3388ff\">a note</span> in a box.\n</div>\n\n"
        "A paragraph to type into, well away from it.\n");
}

/// A note holding a `rows`-row HTML table, which the editor lays out and
/// edits itself (HtmlEdit.cpp) rather than handing to litehtml.
QString buildHtmlTableNote(int rows)
{
    QString note = QStringLiteral("# Table\n\n<table>\n  <tr><th>name</th><th>qty</th><th>note</th></tr>\n");
    for (int r = 0; r < rows; ++r)
        note += QStringLiteral("  <tr><td>item %1</td><td>%1</td><td>some <b>bold</b> &amp; text</td></tr>\n").arg(r);
    return note + QStringLiteral("</table>\n\nend\n");
}

bool writeNote(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(text.toUtf8());
    return true;
}

/// VmHWM from /proc/self/status: the high-water mark of resident memory
/// for this process, in kB. Linux-only, which is the only platform this
/// project targets.
qint64 peakRSSKilobytes()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> lines = status.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (!line.startsWith("VmHWM:"))
            continue;
        const QByteArray value = line.mid(6).simplified(); // "12345 kB"
        return value.split(' ').constFirst().toLongLong();
    }
    return -1;
}

// --- statistics ----------------------------------------------------------

double medianOf(std::vector<double> samples)
{
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    return n % 2 ? samples[n / 2] : (samples[n / 2 - 1] + samples[n / 2]) / 2.0;
}

double percentileOf(std::vector<double> samples, double fraction)
{
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t idx = std::min(samples.size() - 1,
                                static_cast<size_t>(fraction * double(samples.size())));
    return samples[idx];
}

/// One measured metric: `runs` independent samples of a timed operation,
/// reduced to the numbers worth reporting. A median rather than one sample,
/// because the plan's own performance section already learned that lesson
/// on the index.
struct Timing {
    QString metric;
    QString detail;
    int runs = 0;
    double meanMs = 0;
    double medianMs = 0;
    double p95Ms = 0;
    double worstMs = 0;
};

Timing reduce(const QString &metric, const QString &detail, std::vector<double> samplesMs)
{
    Timing t;
    t.metric = metric;
    t.detail = detail;
    t.runs = int(samplesMs.size());
    double sum = 0;
    for (double v : samplesMs)
        sum += v;
    t.meanMs = samplesMs.empty() ? 0.0 : sum / double(samplesMs.size());
    t.medianMs = medianOf(samplesMs);
    t.p95Ms = percentileOf(samplesMs, 0.95);
    t.worstMs = samplesMs.empty() ? 0.0 : *std::max_element(samplesMs.begin(), samplesMs.end());
    return t;
}

/// Forces the actual paint, not just layout: QWidget::render into an
/// off-screen image is what tools/shot.cpp already does to capture a frame
/// with no display attached.
QImage paintedFrame(QWidget *widget)
{
    // At the device pixel ratio, or a HiDPI run renders a 1x frame and reports
    // it as 2x: a raster engine's cost is the physical pixels it fills, and an
    // image sized in logical pixels quietly fills a quarter of them.
    const qreal ratio = widget->devicePixelRatioF();
    QImage frame(widget->size() * ratio, QImage::Format_ARGB32_Premultiplied);
    frame.setDevicePixelRatio(ratio);
    widget->render(&frame);
    return frame;
}

// --- the five measurements ------------------------------------------------

/// 1. Cold start: construct MainWindow on a vault, get one real painted
/// frame. A fresh window every run, the way a fresh process would be.
Timing benchColdStart(const QString &vaultPath, int runs)
{
    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        auto window = std::make_unique<MainWindow>();
        window->openVault(vaultPath);
        window->show();
        // Flushes the posted resize/show/layout events a real launch would
        // process before its first frame; bounded, so a slow background
        // QFileSystemModel scan cannot turn this into a wait for the tree.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const QImage frame = paintedFrame(window.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Cold start to first painted frame"), QString(), samples);
}

/// The first window of the process on its own: it alone pays for what a
/// process does once (fonts, the style sheet, plugins), which is what a real
/// launch pays. Its own row, since the median above hides it.
Timing benchFirstWindow(const QString &vaultPath)
{
    QElapsedTimer clock;
    clock.start();
    auto window = std::make_unique<MainWindow>();
    window->openVault(vaultPath);
    window->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    (void)paintedFrame(window.get());
    return reduce(QStringLiteral("Cold start, the process's first window"), QString(),
                  {double(clock.nsecsElapsed()) / 1e6});
}

/// A pop-out window of the process, if one is up.
QWidget *popoutWindowOf()
{
    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (top->objectName() == QLatin1String("popoutWindow") && top->isVisible())
            return top;
    }
    return nullptr;
}

/// Pop-out windows in a window that already has a note open: the command to
/// the first painted frame of the second window (it builds one editor), and
/// moving a tab out, which is one reparent and builds no editor.
QList<Timing> benchPopout(const QString &vaultPath, const QString &note, int runs)
{
    QList<Timing> out;
    auto window = std::make_unique<MainWindow>();
    window->openVault(vaultPath);
    window->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    window->openNote(note);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const auto trigger = [&window](const char *id) {
        window->findChild<QAction *>(QLatin1String(id))->trigger();
    };
    const auto closePopout = [&window, note] {
        if (QWidget *popout = popoutWindowOf())
            popout->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window->openNote(note); // the tab a move leaves behind is empty
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    };
    std::vector<double> open;
    std::vector<double> move;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        trigger("workspace:open-in-new-window");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (QWidget *popout = popoutWindowOf())
            (void)paintedFrame(popout);
        open.push_back(double(clock.nsecsElapsed()) / 1e6);
        closePopout();

        clock.restart();
        trigger("workspace:move-to-new-window");
        move.push_back(double(clock.nsecsElapsed()) / 1e6);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        closePopout();
    }
    out << reduce(QStringLiteral("Pop-out: open to first painted frame"), QString(), open)
        << reduce(QStringLiteral("Move tab to new window"), QString(), move);
    return out;
}

/// A full scan into an empty cache, as on a vault's first open: the time the
/// indexer itself reports. Deletes the vault's index cache before each run, so
/// only ever called on the generated vault.
Timing benchColdIndex(const QString &vaultPath, int runs)
{
    std::vector<double> samples;
    samples.reserve(size_t(runs));
    const QString cache = Index::cachePathFor(vaultPath);
    for (int i = 0; i < runs; ++i) {
        for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")})
            QFile::remove(cache + suffix);
        Index index;
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(Vault(vaultPath));
        if (!done.wait(180000))
            break;
        samples.push_back(double(done.first().at(1).toLongLong()));
    }
    return reduce(QStringLiteral("Cold index"), QStringLiteral("empty cache, whole vault"), samples);
}

/// 2. Note open: MainWindow::openNote(path) to a laid-out, painted frame.
/// `scratchPath` is reopened between samples so every timed call is a real
/// switch - openNote() no-ops when the path is already the open note.
Timing benchNoteOpen(MainWindow &window, const QString &notePath, const QString &scratchPath,
                     const QString &detail, int runs)
{
    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        window.openNote(scratchPath);
        QElapsedTimer clock;
        clock.start();
        window.openNote(notePath);
        const QImage frame = paintedFrame(&window);
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Note open"), detail, samples);
}

/// Median of `runs` calls to `call`, for costs that have no frame of their own.
template <typename Call>
Timing benchCall(const QString &metric, const QString &detail, int runs, Call call)
{
    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        call();
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(metric, detail, samples);
}

/// Returns once the index has stopped growing for half a second: the indexer
/// runs in the background and says nothing the bench can wait on directly.
void waitForIndex(const Index &index)
{
    QElapsedTimer total;
    total.start();
    int last = -1;
    QElapsedTimer steady;
    steady.start();
    while (total.elapsed() < 180000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const int now = index.noteCount();
        if (now != last) {
            last = now;
            steady.restart();
        } else if (now > 0 && steady.elapsed() > 500) {
            return;
        }
    }
}

QString firstNoteIn(const QString &vaultPath)
{
    // Not the tag-heavy note: a row about an ordinary note stays about one.
    QDirIterator it(vaultPath, {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (const QString path = it.next(); !path.endsWith(QLatin1String("/tag-heavy.md")))
            return path;
    }
    return QString();
}

/// 3. Keystroke latency: insert one character mid-document, relay out and
/// paint the affected block. A standalone MarkdownEditor, the same widget
/// tests/test_editor.cpp drives for the 8 ms budget - this adds the paint
/// that assertion does not measure.
/// A keystroke through the whole window, not a bare editor: whatever the
/// shell has connected to the editor, and the application-wide event filter
/// that catches the mouse's back and forward buttons, are all on the path a
/// person's keystroke actually takes. The bare-editor row cannot see them.
Timing benchWindowKeystroke(const QString &vaultPath, const QString &notePath, int runs)
{
    auto window = std::make_unique<MainWindow>();
    window->resize(1180, 760);
    window->openVault(vaultPath);
    window->openNote(notePath);
    window->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    MarkdownEditor *editor = window->editor();
    const qsizetype middle = editor->noteText().size() / 2;
    editor->setCursorOffset(middle);
    editor->enterBlockAt(middle);
    paintedFrame(editor);

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClick(editor, Qt::Key_X);
        const QImage frame = paintedFrame(editor);
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Keystroke through the window"),
                  QStringLiteral("%1 kB note").arg(QFileInfo(notePath).size() / 1024), samples);
}

/// The whole save path of a vault note with one character typed since the last
/// save: the QSaveFile commit (fdatasync), the recovery snapshot, the history
/// record. Timed on the UI thread, as Ctrl+S and an autosave both run it. The
/// vault is a small one of its own, so file recovery and history are on (they
/// only record notes inside a vault) and nothing else is indexed.
Timing benchSave(const QString &scratchDir, int kb, int runs)
{
    QTemporaryDir vault(scratchDir + QStringLiteral("/save-vault-XXXXXX"));
    const QString notePath = vault.path() + QStringLiteral("/Save.md");
    writeNote(notePath, buildNote(kb));
    auto window = std::make_unique<MainWindow>();
    window->openVault(vault.path());
    window->openNote(notePath);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    MarkdownEditor *editor = window->editor();
    editor->setCursorOffset(editor->noteText().size() / 2);

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QTest::keyClick(editor, Qt::Key_X);
        QElapsedTimer clock;
        clock.start();
        QMetaObject::invokeMethod(window.get(), "save");
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Save"), QStringLiteral("%1 kB note, in a vault").arg(kb), samples);
}

Timing benchKeystroke(const QString &noteText, const QString &detail, int runs)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    // Above 128 kB the parse runs on the thread pool; timed before it lands,
    // a note is still plain paragraphs, and a keystroke measures those. The
    // count goes out as -1 at once and for real with the parse.
    QSignalSpy parsed(editor.get(), &MarkdownEditor::wordCountChanged);
    editor->setNoteText(noteText);
    while (parsed.isEmpty() || parsed.last().at(0).toInt() < 0) {
        if (!parsed.wait(30000)) {
            std::fprintf(stderr, "the parse of \"%s\" never came back\n", qPrintable(detail));
            break;
        }
    }
    editor->setCursorOffset(noteText.size() / 2);
    editor->enterBlockAt(noteText.size() / 2);
    // One untimed frame so the first sample is not also paying for the
    // very first paint of the widget.
    paintedFrame(editor.get());

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QImage frame = paintedFrame(editor.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Keystroke to relaid, painted block"), detail, samples);
}

/// The query block's search runs once, off the keystroke path entirely
/// (setQueryRunner's deliver is asynchronous even here); this measures what
/// every *other* keystroke in the note costs once the block has settled into
/// its cached, rendered state, which is the number docs/PERFORMANCE.md's
/// "nothing recomputed that did not change" rule is actually about.
Timing benchQueryKeystroke(int runs)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    const QString note = buildQueryNote();
    editor->setNoteText(note);

    editor->setQueryRunner([](const QString &, const MarkdownEditor::QueryDeliver &deliver) {
        QueryResult result;
        result.path = QStringLiteral("Project.md");
        result.matchCount = 1;
        result.snippet = QStringLiteral("tagged #project");
        deliver({result});
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50); // lets the queued delivery land

    const qsizetype caret = note.indexOf(QStringLiteral("A paragraph"));
    editor->setCursorOffset(caret);
    editor->enterBlockAt(caret);
    paintedFrame(editor.get()); // untimed: not also paying for the first paint

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QImage frame = paintedFrame(editor.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Keystroke to relaid, painted block"),
                  QStringLiteral("note with a rendered query block"), samples);
}

/// benchQueryKeystroke's own measurement, for a ```base``` block: its
/// evaluation (bases::evaluateBaseForEditor, on the thread pool in the real
/// app - see MainWindow's setBaseRunner wiring) runs once, off the keystroke
/// path entirely, so this is what every *other* keystroke costs once the
/// block has settled into its cached, rendered table.
Timing benchBaseKeystroke(int runs)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    const QString note = buildBaseNote();
    editor->setNoteText(note);

    editor->setBaseRunner([](const QString &, const QString &, const MarkdownEditor::BaseDeliver &deliver) {
        BaseTableData table;
        table.viewNames = {QStringLiteral("Table")};
        table.shownView = 0;
        table.headers = {QStringLiteral("Name")};
        table.rows = {{{QStringLiteral("Project")}, QStringLiteral("Project.md")}};
        deliver(table);
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50); // lets the queued delivery land

    const qsizetype caret = note.indexOf(QStringLiteral("A paragraph"));
    editor->setCursorOffset(caret);
    editor->enterBlockAt(caret);
    paintedFrame(editor.get()); // untimed: not also paying for the first paint

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QImage frame = paintedFrame(editor.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Keystroke to relaid, painted block"),
                  QStringLiteral("note with a rendered base block"), samples);
}

/// The HTML block renders once (litehtml lays it out and rasterises it into
/// a cached QImage) on the untimed first frame; this measures what every
/// other keystroke costs once it has settled, the litehtml equivalent of
/// benchQueryKeystroke above.
Timing benchHtmlKeystroke(int runs)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    const QString note = buildHtmlNote();
    editor->setNoteText(note);
    paintedFrame(editor.get()); // untimed: renders and caches the HTML block

    const qsizetype caret = note.indexOf(QStringLiteral("A paragraph"));
    editor->setCursorOffset(caret);
    editor->enterBlockAt(caret);
    paintedFrame(editor.get());

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QImage frame = paintedFrame(editor.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Keystroke to relaid, painted block"),
                  QStringLiteral("note with a rendered HTML block"), samples);
}

/// Typing into a cell of a 50-row HTML table: the table relays out as a
/// grid, as a pipe table does. The second row is the structure pass the
/// parse 20 ms later runs off the UI thread, timed with a fresh block every
/// run so gumbo is never served from its cache.
Timing benchHtmlTableKeystroke(int runs)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    const QString note = buildHtmlTableNote(50);
    editor->setNoteText(note);
    paintedFrame(editor.get());

    const qsizetype caret = note.indexOf(QStringLiteral("item 25<")) + 7;
    editor->setCursorOffset(caret);
    editor->enterBlockAt(caret);
    paintedFrame(editor.get());

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QImage frame = paintedFrame(editor.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Keystroke to relaid, painted block"),
                  QStringLiteral("note with a 50-row HTML table, in a cell"), samples);
}

Timing benchHtmlTableParse(int runs)
{
    Parser parser;
    int run = 0;
    return benchCall(QStringLiteral("Parse + HTML structure pass (cold)"),
                     QStringLiteral("note with a 50-row HTML table"), runs, [&] {
                         QString note = buildHtmlTableNote(50);
                         note.replace(QStringLiteral("item 0<"), QStringLiteral("item 0 %1<").arg(run++));
                         Parser::Result result = parser.parseAll(note);
                         expandEditableHTML(result.decorations, note);
                     });
}

/// A multi-page PDF for the embed benchmarks below - QPdfWriter (already a
/// project dependency; PdfExport.cpp uses it), not pdflatex, so the bench
/// never depends on a TeX install being present on the machine that runs it.
QString buildBenchPdf(const QString &dir, int pages = 30)
{
    const QString path = dir + QStringLiteral("/bench.pdf");
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    QPainter painter(&writer);
    for (int page = 0; page < pages; ++page) {
        if (page > 0)
            writer.newPage();
        painter.drawText(200, 200, QStringLiteral("Bench page %1").arg(page + 1));
    }
    painter.end();
    return path;
}

/// benchQueryKeystroke/benchHtmlKeystroke's own shape, for a 30-page PDF
/// embed in column layout: every page's render happens off the UI thread and
/// is cached by (path, mtime, page, width) - PdfPage's own thread-local
/// Poppler::Document avoids reopening the file once per page - so once the
/// embed has settled this measures every *other* keystroke's cost with a
/// many-page PDF on screen, not the rendering itself.
#ifdef NOTEAPPC_HAVE_EPUB
// --- EPUB: open time and scroll frame time, on a book big enough for the
// chapter cache (kMaxCachedChapters in EpubView.h) to actually evict. ------

/// A zip with `chapterCount` chapters of `paragraphsPerChapter` paragraphs
/// each, in `dir` - the same libarchive write path tests/test_epub.cpp
/// builds its fixtures with. A real EPUB was also tried (the ones under
/// /usr/share/calibre/quick_start/ on a machine that has Calibre installed),
/// but this tool has to run the same way on any machine, so the bench book
/// is synthesised the way gen_bench_vault.py synthesises a vault.
QString buildBenchEpub(const QString &dir, int chapterCount, int paragraphsPerChapter)
{
    const QString path = QDir(dir).filePath(QStringLiteral("bench.epub"));
    archive *archiveHandle = archive_write_new();
    archive_write_set_format_zip(archiveHandle);
    if (archive_write_open_filename(archiveHandle, path.toUtf8().constData()) != ARCHIVE_OK) {
        archive_write_free(archiveHandle);
        return {};
    }

    const auto addEntry = [&](const QString &name, const QByteArray &bytes) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_size(entry, bytes.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_write_header(archiveHandle, entry);
        archive_write_data(archiveHandle, bytes.constData(), size_t(bytes.size()));
        archive_entry_free(entry);
    };

    addEntry(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    addEntry(QStringLiteral("META-INF/container.xml"),
             QByteArrayLiteral("<?xml version='1.0'?><container "
                               "xmlns='urn:oasis:names:tc:opendocument:xmlns:container' "
                               "version='1.0'><rootfiles><rootfile full-path='content.opf' "
                               "media-type='application/oebps-package+xml'/></rootfiles></container>"));

    QString manifest;
    QString spine;
    QString navItems;
    for (int c = 0; c < chapterCount; ++c) {
        QString body;
        for (int p = 0; p < paragraphsPerChapter; ++p) {
            body += QStringLiteral(
                        "<p>Paragraph %1 of chapter %2. It runs long enough to wrap across a "
                        "line or two, with <b>bold</b> and <i>italic</i> words mixed in so the "
                        "text-run map has more than one font to track, the way a real book's "
                        "typography does.</p>\n")
                        .arg(p)
                        .arg(c);
        }
        addEntry(QStringLiteral("ch%1.xhtml").arg(c),
                 QStringLiteral("<html><body><h1>Chapter %1</h1>%2</body></html>").arg(c).arg(body).toUtf8());
        manifest += QStringLiteral("<item id='c%1' href='ch%1.xhtml' media-type='application/xhtml+xml'/>\n")
                        .arg(c);
        spine += QStringLiteral("<itemref idref='c%1'/>\n").arg(c);
        navItems += QStringLiteral("<li><a href='ch%1.xhtml'>Chapter %1</a></li>\n").arg(c);
    }
    addEntry(QStringLiteral("nav.xhtml"),
             QStringLiteral("<html xmlns:epub='http://www.idpf.org/2007/ops'><body>"
                            "<nav epub:type='toc'><ol>%1</ol></nav></body></html>")
                 .arg(navItems)
                 .toUtf8());
    manifest += QStringLiteral("<item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' "
                               "properties='nav'/>\n");
    addEntry(QStringLiteral("content.opf"),
             QStringLiteral("<package xmlns='http://www.idpf.org/2007/opf' version='3.0'>"
                            "<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>"
                            "<dc:title>Bench Book</dc:title></metadata>"
                            "<manifest>%1</manifest><spine>%2</spine></package>")
                 .arg(manifest, spine)
                 .toUtf8());

    archive_write_close(archiveHandle);
    archive_write_free(archiveHandle);
    return path;
}

Timing benchEpubOpen(const QString &path, int runs)
{
    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        auto view = std::make_unique<EpubView>();
        view->resize(1000, 800);
        QElapsedTimer clock;
        clock.start();
        QString error;
        view->load(path, &error);
        view->show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const QImage frame = paintedFrame(view.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("EPUB: open to first painted frame"),
                  QStringLiteral("bench book, %1 kB").arg(QFileInfo(path).size() / 1024), samples);
}

/// benchScroll's own shape (see below): a wheel notch at a time, painted
/// frame timed - past enough chapters that the cache (kMaxCachedChapters)
/// evicts and re-renders, not just blits what is already there.
Timing benchEpubScroll(const QString &path, int steps)
{
    EpubView view;
    view.resize(1000, 800);
    QString error;
    view.load(path, &error);
    view.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    paintedFrame(&view); // untimed warm-up frame

    std::vector<double> samples;
    samples.reserve(size_t(steps));
    for (int i = 0; i < steps; ++i) {
        QWheelEvent wheel(QPointF(500, 400), view.mapToGlobal(QPoint(500, 400)), QPoint(), QPoint(0, -240),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QElapsedTimer clock;
        clock.start();
        QCoreApplication::sendEvent(&view, &wheel);
        const QImage frame = paintedFrame(&view);
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("EPUB: scroll frame"),
                  QStringLiteral("%1 frames through the bench book, off screen").arg(int(samples.size())),
                  samples);
}
#endif // NOTEAPPC_HAVE_EPUB

Timing benchPdfKeystroke(const QString &scratchPath, int runs)
{
    const QString pdfPath = buildBenchPdf(scratchPath);
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setLinkResolver([&](const QString &) { return pdfPath; });
    const QString note = QStringLiteral("A paragraph above the embed.\n\n"
                                        "![[bench.pdf#page=all&layout=column]]\n\n"
                                        "A paragraph below it.\n");
    editor->setNoteText(note);
    // A generous window for the pool to render all 30 pages and queue their
    // deliveries back before anything is timed - the first few keystrokes
    // would otherwise measure the render, not a settled embed.
    for (int i = 0; i < 60; ++i)
        QTest::qWait(50);

    const qsizetype caret = note.indexOf(QStringLiteral("A paragraph above"));
    editor->setCursorOffset(caret);
    editor->enterBlockAt(caret);
    paintedFrame(editor.get()); // untimed: not also paying for the first paint

    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QImage frame = paintedFrame(editor.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Keystroke to relaid, painted block"),
                  QStringLiteral("note with a 30-page PDF embed (column) on screen"), samples);
}

/// benchScroll's own shape, for the same 30-page PDF embed: scrolling past
/// it pages through cached pixmaps (already rendered, by the wait above)
/// rather than QTextLayout blocks, so this is the raster/composite cost
/// alone, not a render's.
Timing benchPdfScroll(const QString &scratchPath, QSize viewport = QSize(1180, 760))
{
    const QString pdfPath = buildBenchPdf(scratchPath);
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(viewport);
    editor->setLinkResolver([&](const QString &) { return pdfPath; });
    editor->setNoteText(QStringLiteral("![[bench.pdf#page=all&layout=column]]\n"));
    for (int i = 0; i < 60; ++i)
        QTest::qWait(50); // let every page's off-thread render land first
    paintedFrame(editor.get()); // untimed warm-up frame

    QScrollBar *bar = editor->verticalScrollBar();
    const int page = qMax(1, bar->pageStep());
    const int maxValue = bar->maximum();

    std::vector<double> samples;
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? page : -page;
        int value = pass == 0 ? 0 : maxValue;
        bar->setValue(value);
        while (true) {
            const int next = qBound(0, value + step, maxValue);
            if (next == value)
                break;
            value = next;
            QElapsedTimer clock;
            clock.start();
            bar->setValue(value);
            const QImage frame = paintedFrame(editor.get());
            (void)frame;
            samples.push_back(double(clock.nsecsElapsed()) / 1e6);
        }
    }
    return reduce(QStringLiteral("Scroll frame"),
                  QStringLiteral("30-page PDF embed (column), %1 frames, off screen")
                      .arg(int(samples.size())),
                  samples);
}

#ifdef NOTEAPPC_HAVE_PDFVIEW
// --- libs/ext/pdfview's own rows: a PDF opened as a tab, not an embed -----

/// A 500-page PDF with real body text on every page - built with pdflatex,
/// not QPdfWriter (buildBenchPdf's own choice for the embed rows above):
/// the measurement this feeds was specified against pdflatex output, and it
/// gives every page a heading and two paragraphs to search, select and
/// highlight against - closer to a document someone would actually
/// annotate than buildBenchPdf's one line. Empty, with a note on stderr,
/// when pdflatex is not on PATH or fails, so the caller can skip these rows
/// rather than fail the whole run over a missing TeX install.
QString buildLargePdfViewBenchPdf(const QString &dir, int pages = 500)
{
    const QString texPath = dir + QStringLiteral("/pdfview-bench.tex");
    const QString pdfPath = dir + QStringLiteral("/pdfview-bench.pdf");
    QFile tex(texPath);
    if (!tex.open(QIODevice::WriteOnly))
        return {};
    QTextStream out(&tex);
    out << "\\documentclass{article}\n\\usepackage[margin=1in]{geometry}\n\\begin{document}\n";
    for (int page = 1; page <= pages; ++page) {
        out << "\\section*{Page " << page << "}\n"
            << "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
               "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim "
               "veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip.\n\n"
            << "A second paragraph of body text, so the page has more than one line to "
               "search, select and highlight against, the way a real document would.\n";
        if (page != pages)
            out << "\\newpage\n";
    }
    out << "\\end{document}\n";
    tex.close();

    QProcess pdflatex;
    pdflatex.setWorkingDirectory(dir);
    pdflatex.setProgram(QStringLiteral("pdflatex"));
    pdflatex.setArguments({QStringLiteral("-interaction=nonstopmode"), texPath});
    pdflatex.start();
    if (!pdflatex.waitForFinished(180000) || !QFileInfo(pdfPath).isFile()) {
        std::fprintf(stderr, "note: pdflatex not available or failed - skipping the PDF viewer rows\n");
        return {};
    }
    return pdfPath;
}

/// PDF view open: PdfPagesArea::load() lays out every page from its point
/// size alone (a metadata read, see PdfDocument::pageSizePoints - the
/// "pages lay out before any render lands" design tests/test_pdfview.cpp
/// guards), so this measures to the first PAINTED frame, the same
/// "usable, not just constructed" bar benchColdStart holds cold start to -
/// mostly the first visible page's off-UI-thread render (20-50ms/page,
/// PdfPage.h's own number) landing and being waited for, not the layout.
Timing benchPdfViewOpen(const QString &pdfPath, int runs)
{
    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        auto area = std::make_unique<PdfPagesArea>();
        area->resize(1180, 900);
        QElapsedTimer clock;
        clock.start();
        area->load(pdfPath);
        area->show();
        for (int wait = 0; wait < 10; ++wait)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const QImage frame = paintedFrame(area.get());
        (void)frame;
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("PDF view open"), QStringLiteral("500-page PDF, to first painted frame"),
                  samples);
}

/// PDF view scroll: benchPdfScroll's own shape (a page at a time, both
/// directions) over the continuous-page canvas instead of an embed - every
/// visible page already rendered and cached by the warm-up loop below, so
/// this is the raster/composite/annotation-overlay cost alone, not a
/// render's.
Timing benchPdfViewScroll(const QString &pdfPath, QSize viewport = QSize(1180, 900))
{
    auto area = std::make_unique<PdfPagesArea>();
    area->resize(viewport);
    area->load(pdfPath);
    area->show();
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    paintedFrame(area.get()); // untimed warm-up frame

    QScrollBar *bar = area->verticalScrollBar();
    const int page = qMax(1, bar->pageStep());
    const int maxValue = bar->maximum();

    std::vector<double> samples;
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? page : -page;
        int value = pass == 0 ? 0 : maxValue;
        bar->setValue(value);
        while (true) {
            const int next = qBound(0, value + step, maxValue);
            if (next == value)
                break;
            value = next;
            QElapsedTimer clock;
            clock.start();
            bar->setValue(value);
            const QImage frame = paintedFrame(area.get());
            (void)frame;
            samples.push_back(double(clock.nsecsElapsed()) / 1e6);
        }
    }
    return reduce(QStringLiteral("Scroll frame"),
                  QStringLiteral("500-page PDF view, %1 frames, off screen").arg(int(samples.size())),
                  samples);
}

/// Annotation save: a realistic session's worth of markup (20 highlights, 5
/// notes) on a fresh copy of the file each sample. A fresh copy, not one
/// copy reused across samples: save() reopens the document in place
/// afterwards (PdfDocument::save's own doc comment), so reusing it would
/// keep adding to an ever-larger annotation set instead of timing the same
/// edit every time.
Timing benchPdfViewSave(const QString &pdfPath, const QString &scratchDir, int runs)
{
    std::vector<double> samples;
    samples.reserve(size_t(runs));
    for (int i = 0; i < runs; ++i) {
        const QString copyPath = scratchDir + QStringLiteral("/save-sample-%1.pdf").arg(i);
        QFile::remove(copyPath);
        if (!QFile::copy(pdfPath, copyPath))
            continue;

        PdfDocument doc;
        if (!doc.load(copyPath))
            continue;
        for (int n = 0; n < 20; ++n) {
            PdfAnnotationData highlight;
            highlight.kind = PdfAnnotationKind::Highlight;
            highlight.page = n % doc.pageCount();
            highlight.quads = {QRectF(0.1, 0.1 + 0.02 * (n % 10), 0.3, 0.02)};
            highlight.boundary = highlight.quads.first();
            highlight.color = QColor(255, 235, 59);
            doc.addAnnotation(highlight);
        }
        for (int n = 0; n < 5; ++n) {
            PdfAnnotationData note;
            note.kind = PdfAnnotationKind::Note;
            note.page = n % doc.pageCount();
            note.boundary = QRectF(0.05, 0.05, 0.02, 0.02);
            note.contents = QStringLiteral("A bench comment.");
            doc.addAnnotation(note);
        }

        QElapsedTimer clock;
        clock.start();
        QString error;
        const bool ok = doc.save(&error);
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
        if (!ok)
            std::fprintf(stderr, "note: bench save failed: %s\n", qPrintable(error));
    }
    return reduce(QStringLiteral("Annotation save"),
                  QStringLiteral("500-page PDF, 25 annotations (20 highlights + 5 notes)"), samples);
}
#endif // NOTEAPPC_HAVE_PDFVIEW

/// 4. Scroll frame time: page down through a long note and back up,
/// timing relayout-of-newly-visible-blocks plus paint for each page - what
/// the reader actually waits on while scrolling. No budget exists yet;
/// this is the number the TODO calls out as the one that matters most.
/// Scroll frame time with the widget actually on screen, which is the only
/// way to compare the raster viewport against the OpenGL one.
///
/// paintedFrame() renders into a QImage, and a QOpenGLWidget asked for that
/// has to read its framebuffer back over the bus - which would measure the
/// readback, not the drawing. So this shows the widget, drives the scrollbar,
/// and waits for a real paint event on the viewport each time. Vsync is off,
/// or every measurement would come back as one refresh interval.
Timing benchScrollOnScreen(int lines, bool gpu, int stepPixels = 0, int maxFrames = 400)
{
    struct PaintCounter : QObject {
        int paints = 0;
        bool eventFilter(QObject *, QEvent *event) override
        {
            if (event->type() == QEvent::Paint)
                ++paints;
            return false;
        }
    } counter;

    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(1180, 760);
    if (gpu)
        editor->setGPUacceleration(true);
    editor->setNoteText(buildLongNote(lines));
    // Meant to run inside a headless compositor (see --gpu and bench-here.sh),
    // which fullscreens every window on its one virtual output. Asking a real
    // desktop for a particular screen does not work: Hyprland put a
    // fullscreen window on the main panel regardless of setScreen().
    editor->show();
    editor->viewport()->installEventFilter(&counter);
    // Let the window map and settle before anything is timed.
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    QScrollBar *bar = editor->verticalScrollBar();
    // A page at a time by default; a wheel moves a few lines instead, which
    // is the case where most of the frame is already on screen.
    const int page = stepPixels > 0 ? stepPixels : qMax(1, bar->pageStep());
    const int maxValue = bar->maximum();

    std::vector<double> samples;
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? page : -page;
        int value = pass == 0 ? 0 : maxValue;
        bar->setValue(value);
        // Wheel steps would take thousands of frames to cross the note; a few
        // hundred each way is plenty of samples.
        for (int frames = 0; stepPixels <= 0 || frames < maxFrames; ++frames) {
            // The range settles as blocks get their real heights; a value past
            // its end would change nothing, paint nothing, and time out.
            const int next = qBound(0, value + step, bar->maximum());
            if (next == bar->value())
                break;
            value = next;
            const int before = counter.paints;
            QElapsedTimer clock;
            clock.start();
            bar->setValue(value); // repaints what it has to, and only that
            // Spin until the viewport has actually repainted, with a ceiling
            // so a dropped frame cannot hang the run.
            while (counter.paints == before && clock.elapsed() < 200)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            samples.push_back(double(clock.nsecsElapsed()) / 1e6);
        }
    }
    return reduce(gpu ? QStringLiteral("Scroll frame, OpenGL viewport")
                      : QStringLiteral("Scroll frame, raster viewport"),
                  QStringLiteral("%1-line note, %2, %3 frames, on screen")
                      .arg(lines)
                      .arg(stepPixels > 0 ? QStringLiteral("%1 px steps").arg(stepPixels)
                                          : QStringLiteral("page steps"))
                      .arg(int(samples.size())),
                  samples);
}

Timing benchScroll(int lines, QSize viewport = QSize(1180, 760))
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(viewport);
    editor->setNoteText(buildLongNote(lines));
    paintedFrame(editor.get()); // untimed warm-up frame, as above

    QScrollBar *bar = editor->verticalScrollBar();
    const int page = qMax(1, bar->pageStep());
    const int maxValue = bar->maximum();

    std::vector<double> samples;
    // Down, then back up: two passes over the same document, so a cache
    // that only helps the second look at a block still shows up.
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? page : -page;
        int value = pass == 0 ? 0 : maxValue;
        bar->setValue(value);
        while (true) {
            const int next = qBound(0, value + step, maxValue);
            if (next == value)
                break;
            value = next;
            QElapsedTimer clock;
            clock.start();
            bar->setValue(value); // triggers updateLayout() for newly visible blocks
            const QImage frame = paintedFrame(editor.get());
            (void)frame;
            samples.push_back(double(clock.nsecsElapsed()) / 1e6);
        }
    }
    // The pixel count is what a raster engine pays for, so it goes in the
    // label: the same note at 4K and at 2x scale is a different measurement.
    const qreal ratio = editor->devicePixelRatioF();
    return reduce(QStringLiteral("Scroll frame time"),
                  QStringLiteral("%1 lines, %2x%3 @%4x, %5 frames")
                      .arg(lines)
                      .arg(viewport.width())
                      .arg(viewport.height())
                      .arg(ratio)
                      .arg(int(samples.size())),
                  samples);
}

// --- relayouts that are not keystrokes --------------------------------------

/// `insert` placed at the section boundary nearest the middle of a
/// buildNote(kilobytes), so the block it adds is on screen with the caret.
QString noteWithBlockInMiddle(int kilobytes, const QString &insert, qsizetype *at)
{
    QString note = buildNote(kilobytes);
    *at = note.indexOf(QStringLiteral("## Section"), note.size() / 2);
    note.insert(*at, insert);
    return note;
}

/// An editor showing `note` with its parse landed, the caret and the
/// viewport at `at`, one frame painted.
std::unique_ptr<MarkdownEditor> settledEditor(const QString &note, qsizetype at, QSize size = QSize(900, 600))
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(size);
    QSignalSpy parsed(editor.get(), &MarkdownEditor::wordCountChanged);
    editor->setNoteText(note);
    while (parsed.isEmpty() || parsed.last().at(0).toInt() < 0) {
        if (!parsed.wait(30000))
            break;
    }
    editor->setCursorOffset(at);
    editor->enterBlockAt(at);
    paintedFrame(editor.get());
    return editor;
}

/// A window resize, two widths alternately. With readable line length on (the
/// default) and both widths wider than it, no line breaks anywhere.
Timing benchResize(int kilobytes, int fromWidth, int toWidth, const QString &what, int runs)
{
    qsizetype at = 0;
    const QString note = noteWithBlockInMiddle(kilobytes, QString(), &at);
    auto editor = settledEditor(note, at, QSize(fromWidth, 600));
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        editor->resize(i % 2 ? fromWidth : toWidth, 600);
        (void)paintedFrame(editor.get());
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Resize to painted"), QStringLiteral("%1 kB note, %2").arg(kilobytes).arg(what),
                  samples);
}

/// A foldable callout on screen, folded and unfolded alternately.
Timing benchFold(int kilobytes, int runs)
{
    qsizetype at = 0;
    const QString note = noteWithBlockInMiddle(
        kilobytes, QStringLiteral("> [!note]- Folded\n> one line\n> and another\n\n"), &at);
    auto editor = settledEditor(note, at);
    // The caret out of the callout, so its source is not what shows.
    editor->setCursorOffset(note.indexOf(QStringLiteral("A paragraph"), at));
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        editor->toggleFoldAt(at + 2);
        (void)paintedFrame(editor.get());
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Fold toggle to painted"), QStringLiteral("%1 kB note").arg(kilobytes), samples);
}

/// A plugin code block's image arriving: what any embed's late answer
/// (a PDF page, a note embed, a diagram) costs once it is back. Timed from
/// the renderer's deliver call, so it includes the queued hop back to the
/// editor's thread - that hop is part of what an arrival costs.
Timing benchMediaArrival(int kilobytes, int runs)
{
    qsizetype at = 0;
    const QString note = noteWithBlockInMiddle(kilobytes, QStringLiteral("```benchimage\nanything\n```\n\n"), &at);
    auto editor = settledEditor(note, at);
    editor->setCursorOffset(note.indexOf(QStringLiteral("A paragraph"), at));
    std::function<void(MarkdownEditor::CodeBlockResult)> pending;
    const auto renderer = [&pending](const QString &, const QString &, int, const QString &,
                                     const std::function<void(MarkdownEditor::CodeBlockResult)> &deliver) {
        pending = deliver;
    };
    QImage image(600, 240, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::darkCyan);
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        pending = nullptr;
        editor->setCodeBlockRenderer({QStringLiteral("benchimage")}, renderer); // a fresh ask, untimed
        (void)paintedFrame(editor.get());
        if (!pending)
            break;
        MarkdownEditor::CodeBlockResult result;
        result.isMarkdown = false;
        result.image = image;
        QElapsedTimer clock;
        clock.start();
        pending(result);
        QCoreApplication::processEvents(); // the answer is posted back to the editor's thread
        (void)paintedFrame(editor.get());
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Media arrival to painted"), QStringLiteral("%1 kB note").arg(kilobytes), samples);
}

/// Source mode, a pipe table in the middle of a long note: a character typed
/// into a cell and Tab straight after it, inside the parse debounce, so the
/// table is realigned before the reparse of the typing has run.
Timing benchSourceTableTab(int kilobytes, int runs)
{
    qsizetype at = 0;
    const QString note = noteWithBlockInMiddle(
        kilobytes, QStringLiteral("| name | qty |\n| --- | --- |\n| a | 1 |\n| b | 2 |\n\n"), &at);
    auto editor = settledEditor(note, at);
    editor->setAlignTables(true);
    editor->setMode(MarkdownEditor::Mode::Source);
    editor->setCursorOffset(at + 2); // in the header's first cell
    (void)paintedFrame(editor.get());
    std::vector<double> samples;
    for (int i = 0; i < runs; ++i) {
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        QElapsedTimer clock;
        clock.start();
        QTest::keyClick(editor.get(), Qt::Key_Tab);
        (void)paintedFrame(editor.get());
        samples.push_back(double(clock.nsecsElapsed()) / 1e6);
    }
    return reduce(QStringLiteral("Source table Tab after typing"), QStringLiteral("%1 kB note").arg(kilobytes),
                  samples);
}

/// A Jupyter notebook of `cells` code cells, each with a stored PNG output of
/// about 200 kB - what opening one costs is reading those into the cache.
QString writeBenchNotebook(const QString &dir, int cells)
{
    QImage image(640, 480, QImage::Format_RGB32);
    quint32 seed = 1;
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<quint32 *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            seed = seed * 1664525u + 1013904223u; // noise, so the PNG does not compress away
            line[x] = 0xff000000u | (seed >> 8);
        }
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    const QString base64 = QString::fromLatin1(png.toBase64());

    QJsonArray cellArray;
    for (int i = 0; i < cells; ++i) {
        QJsonObject output{{QStringLiteral("output_type"), QStringLiteral("display_data")},
                           {QStringLiteral("data"), QJsonObject{{QStringLiteral("image/png"), base64},
                                                                {QStringLiteral("text/plain"), QStringLiteral("<Figure>")}}},
                           {QStringLiteral("metadata"), QJsonObject()}};
        cellArray.append(QJsonObject{{QStringLiteral("cell_type"), QStringLiteral("code")},
                                     {QStringLiteral("execution_count"), i + 1},
                                     {QStringLiteral("metadata"), QJsonObject()},
                                     {QStringLiteral("source"), QStringLiteral("plot(%1)").arg(i)},
                                     {QStringLiteral("outputs"), QJsonArray{output}}});
        cellArray.append(QJsonObject{{QStringLiteral("cell_type"), QStringLiteral("markdown")},
                                     {QStringLiteral("metadata"), QJsonObject()},
                                     {QStringLiteral("source"), QStringLiteral("Figure %1 shows it.").arg(i)}});
    }
    const QJsonObject root{
        {QStringLiteral("cells"), cellArray},
        {QStringLiteral("metadata"),
         QJsonObject{{QStringLiteral("kernelspec"), QJsonObject{{QStringLiteral("language"), QStringLiteral("python")},
                                                                {QStringLiteral("name"), QStringLiteral("python3")}}}}},
        {QStringLiteral("nbformat"), 4},
        {QStringLiteral("nbformat_minor"), 5}};
    const QString path = dir + QStringLiteral("/bench.ipynb");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return path;
}

/// 5. Peak resident memory for a vault open with a large note. Measured
/// first, before any other phase of this tool has allocated and freed a
/// pile of throwaway notes and windows: VmHWM never falls back down within
/// a process, so anything run earlier would inflate a number that is
/// supposed to describe this one scenario.
qint64 benchMemory(const QString &vaultPath, const QString &largeNotePath)
{
    auto window = std::make_unique<MainWindow>();
    window->openVault(vaultPath);
    window->openNote(largeNotePath);
    window->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    paintedFrame(window.get());
    return peakRSSKilobytes();
}

void printTable(const QList<Timing> &timings, qint64 peakKB, const QString &memoryDetail)
{
    QTextStream out(stdout);
    out << QStringLiteral("%1  %2  %3  %4  %5  %6  %7\n")
               .arg(QStringLiteral("metric"), -42)
               .arg(QStringLiteral("detail"), -28)
               .arg(QStringLiteral("runs"), -5)
               .arg(QStringLiteral("mean ms"), 9)
               .arg(QStringLiteral("median ms"), 11)
               .arg(QStringLiteral("p95 ms"), 9)
               .arg(QStringLiteral("worst ms"), 10);
    for (const Timing &t : timings) {
        out << QStringLiteral("%1  %2  %3  %4  %5  %6  %7\n")
                   .arg(t.metric, -42)
                   .arg(t.detail, -28)
                   .arg(t.runs, -5)
                   .arg(t.meanMs, 9, 'f', 2)
                   .arg(t.medianMs, 11, 'f', 2)
                   .arg(t.p95Ms, 9, 'f', 2)
                   .arg(t.worstMs, 10, 'f', 2);
    }
    out << "\n";
    if (peakKB == 0) {
        // --phase left the memory run out
    } else if (peakKB > 0) {
        out << QStringLiteral("Peak resident memory (VmHWM): %1 MB - %2, 1 run "
                              "(a high-water mark does not average)\n")
                   .arg(peakKB / 1024.0, 0, 'f', 1)
                   .arg(memoryDetail);
    } else {
        out << "Peak resident memory: could not read /proc/self/status\n";
    }
}

void printJson(const QList<Timing> &timings, qint64 peakKB, const QString &memoryDetail,
              const QString &vaultPath, int noteCount)
{
    QJsonArray array;
    for (const Timing &t : timings) {
        QJsonObject o;
        o[QStringLiteral("metric")] = t.metric;
        o[QStringLiteral("detail")] = t.detail;
        o[QStringLiteral("runs")] = t.runs;
        o[QStringLiteral("meanMs")] = t.meanMs;
        o[QStringLiteral("medianMs")] = t.medianMs;
        o[QStringLiteral("p95Ms")] = t.p95Ms;
        o[QStringLiteral("worstMs")] = t.worstMs;
        array.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("vault")] = vaultPath;
    root[QStringLiteral("noteCount")] = noteCount;
    root[QStringLiteral("timings")] = array;
    root[QStringLiteral("peakMemoryKB")] = peakKB;
    root[QStringLiteral("peakMemoryDetail")] = memoryDetail;
    QTextStream out(stdout);
    out << QJsonDocument(root).toJson();
}

} // namespace

int main(int argc, char *argv[])
{
    // --gpu needs a real GL context, which the offscreen platform cannot give,
    // so that run keeps whatever platform the session already has.
    const bool wantsGPU = [argc, argv] {
        for (int i = 1; i < argc; ++i) {
            if (qstrcmp(argv[i], "--gpu") == 0)
                return true;
        }
        return false;
    }();
    if (!wantsGPU) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    } else {
        // Without this every OpenGL frame waits for the display's refresh and
        // the measurement is of vsync, not of drawing.
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
    }
    // As in tools/shot.cpp: a benchmark run must not read or write the real
    // ~/.config/noteAppC, and must not silently auto-open whatever vault the
    // real app last had open (MainWindow's constructor does exactly that
    // when it finds one in Settings).
    QTemporaryDir configHome;
    if (configHome.isValid() && !qEnvironmentVariableIsSet("NOTEAPPC_SHOT_KEEP_CONFIG"))
        qputenv("XDG_CONFIG_HOME", configHome.path().toLocal8Bit());
    // File recovery snapshots go to the data home: a save row must not fill the real one.
    QTemporaryDir dataHome;
    if (dataHome.isValid() && !qEnvironmentVariableIsSet("NOTEAPPC_SHOT_KEEP_CONFIG"))
        qputenv("XDG_DATA_HOME", dataHome.path().toLocal8Bit());

    // A command-line tool talks on stderr. Without this Qt 6 sends qWarning
    // to the systemd journal whenever stderr is not a terminal - piped, run
    // from a script, run by an agent - and every error the tool reports simply
    // vanishes.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING"))
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-bench"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-bench"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Headless performance harness: cold start, note open, keystroke "
                       "latency, scroll frame time, peak memory."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("vault"),
                                 QStringLiteral("Vault to measure against. Generated with "
                                               "gen_bench_vault.py when omitted."));
    QCommandLineOption jsonOption(QStringLiteral("json"),
                                  QStringLiteral("Also print the results as JSON."));
    QCommandLineOption notesOption(QStringLiteral("notes"),
                                   QStringLiteral("Notes in the generated vault (ignored with "
                                                 "an explicit vault)."),
                                   QStringLiteral("count"), QStringLiteral("2000"));
    QCommandLineOption viewportOption(
        QStringLiteral("viewport"),
        QStringLiteral("Also measure scrolling at this size, e.g. 3840x2160. Combine with "
                       "QT_SCALE_FACTOR=2 for a HiDPI panel."),
        QStringLiteral("WxH"));
    QCommandLineOption gpuOption(
        QStringLiteral("gpu"),
        QStringLiteral("Only compare the raster and OpenGL viewports, on screen. Run it inside a "
                       "headless compositor so no window reaches a real monitor: "
                       "WLR_BACKENDS=headless cage -- noteAppC-bench --gpu"));
    parser.addOption(jsonOption);
    parser.addOption(notesOption);
    parser.addOption(gpuOption);
    // One viewport at a time, so whatever runs the bench - `perf stat -i` on
    // the compositor, say - can tell the two apart.
    QCommandLineOption onlyOption(QStringLiteral("only"),
                                  QStringLiteral("With --gpu, run one viewport: raster or opengl; "
                                                 "none opens the window and scrolls nothing, as "
                                                 "a baseline for the compositor's own cost."),
                                  QStringLiteral("viewport"));
    parser.addOption(onlyOption);
    parser.addOption(viewportOption);
    // One phase alone, so `perf record` sees only it.
    QCommandLineOption phaseOption(QStringLiteral("phase"),
                                   QStringLiteral("Run one phase: memory, start, open, keystroke, save, "
                                                  "relayout, scroll or pdfview."),
                                   QStringLiteral("name"));
    parser.addOption(phaseOption);
    parser.process(app);
    const QString phase = parser.value(phaseOption);
    const auto runs = [&phase](const char *name) { return phase.isEmpty() || phase == QLatin1String(name); };

    const QStringList args = parser.positionalArguments();
    const int noteCount = parser.value(notesOption).toInt();

    // The vault: generated fresh unless one was given. Kept alive for the
    // whole run by owning the QTemporaryDir here, not inside a branch.
    QTemporaryDir generatedVault;
    QString vaultPath;
    if (!args.isEmpty()) {
        vaultPath = QFileInfo(args.first()).absoluteFilePath();
        if (!QFileInfo(vaultPath).isDir()) {
            qWarning("%s is not a directory", qPrintable(vaultPath));
            return 2;
        }
    } else {
        if (!generatedVault.isValid()) {
            qWarning("could not create a temporary vault directory");
            return 1;
        }
        vaultPath = generatedVault.path() + QStringLiteral("/vault");
        QProcess gen;
        gen.start(QStringLiteral("python3"),
                  {QStringLiteral(NOTEAPPC_GEN_VAULT_SCRIPT), vaultPath,
                   QString::number(noteCount)});
        if (!gen.waitForFinished(180000) || gen.exitCode() != 0) {
            qWarning("gen_bench_vault.py failed:\n%s", gen.readAllStandardError().constData());
            return 1;
        }
        writeNote(vaultPath + QStringLiteral("/tag-heavy.md"), buildTagHeavyNote());
    }

    // Notes for the size-tiered measurements, and the scratch note the
    // note-open benchmark ping-pongs against. A directory of its own, well
    // away from the vault, so this tool never writes into a vault it was
    // only handed to read.
    QTemporaryDir workDir;
    if (!workDir.isValid()) {
        qWarning("could not create a temporary working directory");
        return 1;
    }
    const QString scratchPath = workDir.path() + QStringLiteral("/scratch.md");
    writeNote(scratchPath, QStringLiteral("# scratch\n\nping-pong target.\n"));

    const QList<int> sizesKB = {1, 50, 200, 1024};
    QHash<int, QString> sizedNotes;
    for (int kb : sizesKB) {
        const QString path = workDir.path() + QStringLiteral("/note_%1kb.md").arg(kb);
        writeNote(path, buildNote(kb));
        sizedNotes.insert(kb, path);
    }

    QList<Timing> timings;

    // --gpu measures one thing: raster against OpenGL, on screen. Everything
    // else shows windows too (cold start opens seven), and on a real display
    // those would land on whatever screen someone is working at - so that run
    // is the headless one, and this one does nothing but the comparison.
    if (parser.isSet(gpuOption)) {
        const QString only = parser.value(onlyOption);
        const bool raster = only.isEmpty() || only == QLatin1String("raster");
        const bool opengl = only.isEmpty() || only == QLatin1String("opengl");
        if (only == QLatin1String("none"))
            timings << benchScrollOnScreen(5000, false, 72, 0);
        if (raster) {
            timings << benchScrollOnScreen(5000, false);
            timings << benchScrollOnScreen(5000, false, 72); // three lines, one wheel notch
        }
        if (!opengl) {
            // the OpenGL rows are skipped on request
        } else if (MarkdownEditor::GPUaccelerationAvailable()) {
            timings << benchScrollOnScreen(5000, true);
            timings << benchScrollOnScreen(5000, true, 72);
        } else {
            std::fprintf(stderr, "note: no GL context available, skipping the OpenGL run\n");
        }
        printTable(timings, 0, QStringLiteral("not measured in a --gpu run"));
        return 0;
    }

    // 5 first: see the comment on benchMemory for why.
    const qint64 peakKB = runs("memory") ? benchMemory(vaultPath, sizedNotes.value(1024)) : 0;
    const QString memoryDetail = QStringLiteral("vault (%1 notes) + 1024 kB note").arg(noteCount);

    // 1: cold start. Constructing MainWindow is the most expensive single
    // sample here, so this runs the fewest times.
    if (runs("start")) {
        if (phase == QLatin1String("start")) // in a full run the memory phase opened the first window
            timings << benchFirstWindow(vaultPath);
        timings << benchColdStart(vaultPath, 7);
        if (const QString note = firstNoteIn(vaultPath); !note.isEmpty())
            timings << benchPopout(vaultPath, note, 9);
        if (args.isEmpty()) // a vault given on the command line keeps its cache
            timings << benchColdIndex(vaultPath, 3);
    }

    // 2: note open, one MainWindow reused across sizes - a session opens
    // many notes in one window, it does not restart between them.
    if (runs("open")) {
        auto window = std::make_unique<MainWindow>();
        window->openVault(vaultPath);
        window->show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int kb : sizesKB) {
            timings << benchNoteOpen(*window, sizedNotes.value(kb), scratchPath,
                                     QStringLiteral("%1 kB note").arg(kb), 9);
        }

        // The notes above live outside the vault, so no pane finds anything
        // about them. A vault note is what a real switch costs: backlinks and
        // unlinked mentions search the index for it. Measured once the
        // indexer has settled, or the searches would run over half a vault.
        waitForIndex(*window->index());
        const QString vaultNote = firstNoteIn(vaultPath);
        if (!vaultNote.isEmpty()) {
            timings << benchNoteOpen(*window, vaultNote, scratchPath,
                                     QStringLiteral("vault note, panes filled"), 9);
            const Index &index = *window->index();
            timings << benchCall(QStringLiteral("Unlinked mentions"), QStringLiteral("vault note"),
                                 9, [&] { (void)index.unlinkedMentions(vaultNote); });
            timings << benchCall(QStringLiteral("Search"), QStringLiteral("two words"), 9,
                                 [&] { (void)index.search(QStringLiteral("prose bold")); });
            timings << benchCall(QStringLiteral("Search"), QStringLiteral("rare word"), 9,
                                 [&] { (void)index.search(QStringLiteral("zyxwvu")); });
            timings << benchCall(QStringLiteral("Every note for a base"), QStringLiteral("allNotesForBases"), 5,
                                 [&] { (void)index.allNotesForBases(); });
            if (auto *tags = window->findChild<TagsPane *>()) {
                // A hidden pane only marks itself stale (an index change costs
                // this); shown, it rebuilds, which is what showing it costs.
                tags->hide();
                timings << benchCall(QStringLiteral("Tags pane refresh"), QStringLiteral("hidden, index change"),
                                     5, [&] { tags->refresh(); });
                tags->show();
                timings << benchCall(QStringLiteral("Tags pane refresh"), QStringLiteral("%1 tags, shown").arg(index.allTags().size()),
                                     5, [&] { tags->refresh(); });
            }
        }

        // A notebook's stored outputs go into the output cache when it is read.
        if (const QString notebook = writeBenchNotebook(workDir.path(), 40); !notebook.isEmpty())
            timings << benchNoteOpen(*window, notebook, scratchPath,
                                     QStringLiteral("notebook, 40 PNG outputs"), 9);

        // What every switch on the settings page costs: the page reports a
        // change and the window applies every setting, with a big note open.
        window->openNote(sizedNotes.value(1024));
        paintedFrame(window.get());
        if (auto *page = window->findChild<SettingsPage *>()) {
            timings << benchCall(QStringLiteral("Settings pass"), QStringLiteral("1024 kB note open"), 9, [&] {
                emit page->settingChanged();
                (void)paintedFrame(window.get());
            });
        }
    }

    // 3: keystroke latency, same sizes.
    if (runs("keystroke")) {
        for (int kb : sizesKB)
            timings << benchKeystroke(buildNote(kb), QStringLiteral("%1 kB note").arg(kb), 30);
        timings << benchWindowKeystroke(vaultPath, sizedNotes.value(200), 30);
        for (int lines : {100, 1000, 5000})
            timings << benchKeystroke(buildCodeNote(lines),
                                      QStringLiteral("%1-line code block").arg(lines), 30);
        timings << benchQueryKeystroke(30);
        timings << benchBaseKeystroke(30);
        timings << benchHtmlKeystroke(30);
        timings << benchHtmlTableKeystroke(30);
        timings << benchHtmlTableParse(30);
        timings << benchPdfKeystroke(workDir.path(), 30);
    }

    // The save path, on the two sizes a long note reaches.
    if (runs("save")) {
        for (int kb : {200, 1024})
            timings << benchSave(workDir.path(), kb, 15);
    }

    // Relayouts that are not keystrokes: a resize, a fold, an embed's late
    // answer, and a source-mode table realigned inside the parse debounce.
    if (runs("relayout")) {
        for (int kb : {200, 1024}) {
            timings << benchResize(kb, 1100, 1000, QStringLiteral("text width kept"), 20);
            timings << benchResize(kb, 640, 600, QStringLiteral("text rewrapped"), 20);
            timings << benchFold(kb, 20);
            timings << benchMediaArrival(kb, 20);
        }
        timings << benchSourceTableTab(1024, 15);
    }

    // 4: scroll frame time over a 5 000-line note.
    if (runs("scroll")) {
        timings << benchScroll(5000);
        timings << benchPdfScroll(workDir.path());
    }
#ifdef NOTEAPPC_HAVE_EPUB
    if (runs("scroll")) {
        const QString epubPath = buildBenchEpub(workDir.path(), 150, 6);
        if (!epubPath.isEmpty()) {
            timings << benchEpubOpen(epubPath, 7);
            timings << benchEpubScroll(epubPath, 60);
        }
    }
#endif
    if (parser.isSet(viewportOption) && runs("scroll")) {
        const QStringList parts = parser.value(viewportOption).split(QLatin1Char('x'));
        if (parts.size() == 2)
            timings << benchScroll(5000, QSize(parts.at(0).toInt(), parts.at(1).toInt()));
    }

#ifdef NOTEAPPC_HAVE_PDFVIEW
    // libs/ext/pdfview's own rows: open, scroll and annotation save against
    // a 500-page PDF built once and reused (or a fresh copy of it, for
    // save) across the samples below.
    if (const QString pdfViewBenchPdf = runs("pdfview") ? buildLargePdfViewBenchPdf(workDir.path()) : QString();
        !pdfViewBenchPdf.isEmpty()) {
        timings << benchPdfViewOpen(pdfViewBenchPdf, 5);
        timings << benchPdfViewScroll(pdfViewBenchPdf);
        timings << benchPdfViewSave(pdfViewBenchPdf, workDir.path(), 5);
    }
#endif

    printTable(timings, peakKB, memoryDetail);
    if (parser.isSet(jsonOption))
        printJson(timings, peakKB, memoryDetail, vaultPath, noteCount);

    return 0;
}
