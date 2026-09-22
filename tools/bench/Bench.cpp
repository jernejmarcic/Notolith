// Headless performance harness for noteAppC. Jernej's requirement is that
// this app measurably beats Obsidian while staying CPU-only, and "nothing
// can be tuned before it is measured" - this tool only measures, on the
// offscreen QPA platform, the same way tools/shot.cpp does.
//
//   noteAppC-bench [--json] [--notes N] [vault]
//
// With no vault argument, one is generated with tools/gen_bench_vault.py
// into a temporary directory that is deleted on exit, so nothing gets
// committed by accident. A supplied vault is opened read-only; the note
// files this tool needs for the size-tiered measurements are written to a
// separate temporary directory, never into the vault itself.

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include <QSurfaceFormat>

#include "MainWindow.h"
#include "MarkdownEditor.h"

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

/// 3. Keystroke latency: insert one character mid-document, relay out and
/// paint the affected block. A standalone MarkdownEditor, the same widget
/// tests/test_editor.cpp drives for the 8 ms budget - this adds the paint
/// that assertion does not measure.
Timing benchKeystroke(const QString &noteText, const QString &detail, int runs)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(noteText);
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
Timing benchScrollOnScreen(int lines, bool gpu)
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
            const int before = counter.paints;
            QElapsedTimer clock;
            clock.start();
            bar->setValue(value);
            editor->viewport()->update();
            // Spin until the viewport has actually repainted, with a ceiling
            // so a dropped frame cannot hang the run.
            while (counter.paints == before && clock.elapsed() < 200)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            samples.push_back(double(clock.nsecsElapsed()) / 1e6);
        }
    }
    return reduce(gpu ? QStringLiteral("Scroll frame, OpenGL viewport")
                      : QStringLiteral("Scroll frame, raster viewport"),
                  QStringLiteral("%1-line note, %2 frames, on screen")
                      .arg(lines)
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
    if (peakKB >= 0) {
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
    parser.addOption(viewportOption);
    parser.process(app);

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
        timings << benchScrollOnScreen(5000, false);
        if (MarkdownEditor::GPUaccelerationAvailable())
            timings << benchScrollOnScreen(5000, true);
        else
            std::fprintf(stderr, "note: no GL context available, skipping the OpenGL run\n");
        printTable(timings, 0, QStringLiteral("not measured in a --gpu run"));
        return 0;
    }

    // 5 first: see the comment on benchMemory for why.
    const qint64 peakKB = benchMemory(vaultPath, sizedNotes.value(1024));
    const QString memoryDetail = QStringLiteral("vault (%1 notes) + 1024 kB note").arg(noteCount);

    // 1: cold start. Constructing MainWindow is the most expensive single
    // sample here, so this runs the fewest times.
    timings << benchColdStart(vaultPath, 7);

    // 2: note open, one MainWindow reused across sizes - a session opens
    // many notes in one window, it does not restart between them.
    {
        auto window = std::make_unique<MainWindow>();
        window->openVault(vaultPath);
        window->show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int kb : sizesKB) {
            timings << benchNoteOpen(*window, sizedNotes.value(kb), scratchPath,
                                     QStringLiteral("%1 kB note").arg(kb), 9);
        }
    }

    // 3: keystroke latency, same sizes.
    for (int kb : sizesKB)
        timings << benchKeystroke(buildNote(kb), QStringLiteral("%1 kB note").arg(kb), 30);

    // 4: scroll frame time over a 5 000-line note.
    timings << benchScroll(5000);
    if (parser.isSet(viewportOption)) {
        const QStringList parts = parser.value(viewportOption).split(QLatin1Char('x'));
        if (parts.size() == 2)
            timings << benchScroll(5000, QSize(parts.at(0).toInt(), parts.at(1).toInt()));
    }

    printTable(timings, peakKB, memoryDetail);
    if (parser.isSet(jsonOption))
        printJson(timings, peakKB, memoryDetail, vaultPath, noteCount);

    return 0;
}
