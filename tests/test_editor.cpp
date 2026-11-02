// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include <fcntl.h>
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QScroller>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QAbstractItemView>
#include <QCompleter>
#include <QMimeData>
#include <QRandomGenerator>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QImageReader>
#include <QPicture>
#include <QScreen>
#include <QWindow>
#include <QScrollBar>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <memory>

#include "Perf.h"
#include "BubblewrapProbe.h"
#include "WaitFor.h"
#include "DisplayMap.h"

using namespace noteAppC;

namespace {

/// The whole block, which is what most of these tests map over.
Range whole(const QString &text)
{
    return {0, text.size()};
}

} // namespace

TEST(DisplayMapTest, IdentityHidesNothing)
{
    const QString source = QStringLiteral("plain text");
    const DisplayMap map = DisplayMap::identity(source, whole(source));

    EXPECT_EQ(map.text(), source);
    EXPECT_FALSE(map.hidesAnything());
    for (qsizetype i = 0; i <= source.size(); ++i) {
        EXPECT_EQ(map.toDisplay(i), i);
        EXPECT_EQ(map.toSource(i), i);
    }
}

TEST(DisplayMapTest, HidesTheHeadingMarker)
{
    const QString source = QStringLiteral("## Heading");
    const DisplayMap map = DisplayMap::build(source, whole(source), {{0, 3}});

    EXPECT_EQ(map.text(), QStringLiteral("Heading"));
    EXPECT_TRUE(map.hidesAnything());
    EXPECT_EQ(map.toSource(0), 3);
    EXPECT_EQ(map.toDisplay(3), 0);
    // Everything inside the marker collapses onto the first visible position.
    EXPECT_EQ(map.toDisplay(0), 0);
    EXPECT_EQ(map.toDisplay(2), 0);
    EXPECT_TRUE(map.isHidden(1));
    EXPECT_FALSE(map.isHidden(3));
}

TEST(DisplayMapTest, HidesWikilinkBracketsAndTarget)
{
    //                              0         1         2
    //                              0123456789012345678901234
    const QString source = QStringLiteral("see [[Target|shown]] here");
    // Hide "[[Target|" and "]]", which is what live preview does.
    const DisplayMap map = DisplayMap::build(source, whole(source), {{4, 13}, {18, 20}});

    EXPECT_EQ(map.text(), QStringLiteral("see shown here"));
    EXPECT_EQ(map.toSource(map.text().indexOf(QLatin1Char('s'), 4)), 13);
    EXPECT_EQ(map.toDisplay(13), 4);
    EXPECT_EQ(map.toDisplay(20), 9);
}

TEST(DisplayMapTest, OverlappingAndUnsortedRangesMerge)
{
    const QString source = QStringLiteral("abcdefgh");
    const DisplayMap map = DisplayMap::build(source, whole(source), {{4, 6}, {1, 3}, {2, 5}});
    // 1..6 merged away.
    EXPECT_EQ(map.text(), QStringLiteral("agh"));
}

TEST(DisplayMapTest, RangesOutsideTheBlockAreIgnored)
{
    const QString source = QStringLiteral("0123456789");
    const DisplayMap map = DisplayMap::build(source, {3, 7}, {{0, 4}, {6, 9}});

    EXPECT_EQ(map.text(), QStringLiteral("45"));
    EXPECT_EQ(map.toSource(0), 4);
    EXPECT_EQ(map.toSource(2), 6);
}

TEST(DisplayMapTest, EverythingHidden)
{
    const QString source = QStringLiteral("%%a comment%%");
    const DisplayMap map = DisplayMap::build(source, whole(source), {whole(source)});

    EXPECT_TRUE(map.text().isEmpty());
    // Nothing is visible, so the one display position is the block start:
    // the caret sits before the hidden text, and typing there stays outside it.
    EXPECT_EQ(map.toSource(0), 0);
    EXPECT_EQ(map.toDisplay(5), 0);
}

TEST(DisplayMapTest, EmptyBlock)
{
    const QString source;
    const DisplayMap map = DisplayMap::build(source, {0, 0}, {});
    EXPECT_TRUE(map.text().isEmpty());
    EXPECT_EQ(map.toSource(0), 0);
    EXPECT_EQ(map.toDisplay(0), 0);
}

// The gate: a million random maps must never break the
// round trip, because every cursor move in live preview goes through it.
TEST(DisplayMapTest, FuzzRoundTrip)
{
    auto *random = QRandomGenerator::global();
    constexpr int kCases = 4000;

    for (int c = 0; c < kCases; ++c) {
        const qsizetype length = random->bounded(1, 80);
        QString source(length, QLatin1Char('.'));
        for (qsizetype i = 0; i < length; ++i)
            source[i] = QChar(QLatin1Char('a' + random->bounded(26)));

        QList<Range> hidden;
        const int holes = random->bounded(0, 6);
        for (int h = 0; h < holes; ++h) {
            const qsizetype start = random->bounded(int(length));
            const qsizetype end = start + random->bounded(1, int(length - start) + 1);
            hidden.append({start, end});
        }

        const DisplayMap map = DisplayMap::build(source, whole(source), hidden);

        // 1. The display string is the source minus the hidden ranges.
        QString expected;
        for (qsizetype i = 0; i < length; ++i) {
            if (!map.isHidden(i))
                expected += source.at(i);
        }
        ASSERT_EQ(map.text(), expected) << "case " << c;

        // 2. Display -> source -> display is the identity.
        for (qsizetype d = 0; d <= map.text().size(); ++d) {
            const qsizetype s = map.toSource(d);
            ASSERT_GE(s, 0);
            ASSERT_LE(s, length);
            ASSERT_EQ(map.toDisplay(s), d) << "case " << c << " display " << d;
        }

        // 3. Mapping is monotonic in both directions: a cursor never jumps
        //    backwards when it moves forwards.
        qsizetype previous = -1;
        for (qsizetype s = 0; s <= length; ++s) {
            const qsizetype d = map.toDisplay(s);
            ASSERT_GE(d, previous) << "case " << c << " source " << s;
            previous = d;
        }

        // 4. A visible offset survives the trip unchanged.
        for (qsizetype s = 0; s < length; ++s) {
            if (!map.isHidden(s)) {
                ASSERT_EQ(map.toSource(map.toDisplay(s)), s) << "case " << c << " source " << s;
            }
        }
    }
}

// toDisplay and isHidden are binary searches; the linear walk over the runs
// they replaced is the reference, at every offset, holes and substitutions
// both, run boundaries included.
TEST(DisplayMapTest, BinarySearchesAgreeWithALinearWalk)
{
    const auto linearToDisplay = [](const DisplayMap &map, qsizetype sourceOffset) -> qsizetype {
        if (map.runs().isEmpty())
            return 0;
        const qsizetype wanted = qBound(map.block().start, sourceOffset, map.block().end);
        for (const DisplayMap::Run &run : map.runs()) {
            if (wanted < run.sourceStart)
                return run.displayStart;
            if (wanted <= run.sourceStart + run.sourceLength)
                return run.substituted ? run.displayStart + (wanted == run.sourceStart ? 0 : run.length)
                                       : run.displayStart + (wanted - run.sourceStart);
        }
        return map.text().size();
    };
    const auto linearIsHidden = [](const DisplayMap &map, qsizetype sourceOffset) {
        if (sourceOffset < map.block().start || sourceOffset >= map.block().end)
            return false;
        for (const DisplayMap::Run &run : map.runs()) {
            if (sourceOffset < run.sourceStart)
                return true;
            if (sourceOffset < run.sourceStart + run.sourceLength)
                return false;
        }
        return true;
    };
    QRandomGenerator random(7);
    for (int c = 0; c < 3000; ++c) {
        const qsizetype length = random.bounded(1, 60);
        const QString source(length, QLatin1Char('x'));
        QList<Range> hidden;
        for (int h = random.bounded(0, 5); h > 0; --h) {
            const qsizetype start = random.bounded(int(length));
            hidden.append({start, start + random.bounded(1, int(length - start) + 1)});
        }
        QList<Substitution> substitutions; // sorted and disjoint, as the parser gives them
        for (qsizetype at = random.bounded(4); at + 2 <= length; at += 2 + random.bounded(8))
            substitutions.append({{at, at + 1 + random.bounded(2)}, QStringLiteral("&")});
        const Range block{random.bounded(int(length / 3) + 1), length - random.bounded(int(length / 3) + 1)};
        const DisplayMap map = DisplayMap::build(source, block, hidden, substitutions);
        for (qsizetype s = block.start - 1; s <= block.end + 1; ++s) {
            ASSERT_EQ(map.toDisplay(s), linearToDisplay(map, s)) << "case " << c << " source " << s;
            ASSERT_EQ(map.isHidden(s), linearIsHidden(map, s)) << "case " << c << " source " << s;
        }
    }
}

// --- the widget ----------------------------------------------------------

#include <QApplication>
#include <QFontMetricsF>
#include <QDirIterator>
#include <QFile>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <QTest>

#include <utility>

#include "CodeHighlighter.h"
#include "LanguageStyle.h"
#include "Scheme.h"
#include "Theme.h"
#include "Lifeline.h"
#include "MarkdownEditor.h"
#include "Parser.h"
#include "HtmlRenderer.h"
#include "MathRenderer.h"
#include "PdfArgs.h"
#include "PdfPage.h"
#include "TexCompiler.h"

#include <QPainter>
#include <QPdfWriter>

namespace {

/// An editor sized like a real window, on the offscreen platform.
std::unique_ptr<MarkdownEditor> editorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    return editor;
}

/// An editor whose parse has landed. Above kAsyncParseThreshold the parse runs
/// on the thread pool and the note is plain paragraphs until it comes back, so
/// a timing taken before then measures the placeholder: two guards once did.
/// Null if the parse never returns.
std::unique_ptr<MarkdownEditor> parsedEditorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    QSignalSpy parsed(editor.get(), &MarkdownEditor::wordCountChanged);
    editor->setNoteText(text);
    // A small note parses before setNoteText returns. A big one sends -1 at
    // once, "not counted yet", and its real count with the parse.
    while (!parsed.isEmpty() && parsed.last().at(0).toInt() < 0) {
        if (!parsed.wait(10000))
            return nullptr;
    }
    return editor;
}

#ifdef NOTEAPPC_EXPECT_PDF
/// A real, if minimal, multi-page PDF for the page-argument and layout
/// tests - built with QPdfWriter (already a project dependency, PdfExport.cpp
/// uses it) rather than relying on pdflatex being installed, so these tests
/// run wherever poppler itself does. Every call site is itself behind
/// NOTEAPPC_EXPECT_PDF (there is nothing to open a PDF with otherwise), so
/// this is too.
QString makeTestPdf(const QString &dir, int pages, const QString &name = QStringLiteral("doc.pdf"))
{
    const QString path = dir + QLatin1Char('/') + name;
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    QPainter painter(&writer);
    for (int page = 0; page < pages; ++page) {
        if (page > 0)
            writer.newPage();
        painter.drawText(200, 200, QStringLiteral("page %1").arg(page + 1));
    }
    painter.end();
    return path;
}
#endif

} // namespace

TEST(MarkdownEditorTest, RendersTheWholeNoteUntilTheReaderClicksIn)
{
    const QString source = QStringLiteral("# Title\n\nSome **bold** text.\n");
    auto editor = editorWith(source);

    // Untouched: every marker is hidden.
    EXPECT_EQ(editor->displayText(), QStringLiteral("Title\n\nSome bold text.\n"));

    // Click into the heading: that block, and only that block, shows its source.
    editor->enterBlockAt(2);
    EXPECT_EQ(editor->displayText(), QStringLiteral("# Title\n\nSome bold text.\n"));

    // Move to the paragraph: the heading goes back to rendered.
    editor->enterBlockAt(source.indexOf(QStringLiteral("bold")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("Title\n\nSome **bold** text.\n"));
}

TEST(MarkdownEditorTest, AResizeRewrapsOnlyWhenTheTextWidthChanges)
{
    // With readable line length on, a window wider than it keeps every line
    // break, and is not laid out again; narrower, the text rewraps, and back
    // to wide it is exactly as it was.
    QString note;
    for (int i = 0; i < 12; ++i)
        note += QStringLiteral("A paragraph long enough to wrap at any width this test uses, and then some more words "
                               "so a narrow window breaks it over several lines. ").repeated(2)
                + QStringLiteral("\n\n");
    auto editor = editorWith(note);
    editor->resize(1100, 300);
    (void)editor->grab();
    const int wide = editor->verticalScrollBar()->maximum();
    editor->resize(1000, 300);
    (void)editor->grab();
    EXPECT_EQ(editor->verticalScrollBar()->maximum(), wide);
    editor->resize(420, 300);
    (void)editor->grab();
    EXPECT_GT(editor->verticalScrollBar()->maximum(), wide);
    editor->resize(1100, 300);
    (void)editor->grab();
    EXPECT_EQ(editor->verticalScrollBar()->maximum(), wide);
}

TEST(MarkdownEditorTest, SourceModeHidesNothingAndReadingHidesEverything)
{
    const QString source = QStringLiteral("# Title\n\n[[A link|shown]]\n");
    auto editor = editorWith(source);

    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), source);

    editor->setMode(MarkdownEditor::Mode::Reading);
    editor->enterBlockAt(2); // no effect: reading view has no active block
    EXPECT_EQ(editor->displayText(), QStringLiteral("Title\n\nshown\n"));
}

TEST(SourceModeTest, ACalloutHasNoHeaderAndNeverFoldsItsBody)
{
    // Source mode is the text: the callout's panel, title bar and fold are
    // rendering, so `[!x]-` neither takes a header's height nor hides its body.
    const QString callout = QStringLiteral("> [!warning]- T\n> body\n");
    auto editor = editorWith(callout);
    editor->setMode(MarkdownEditor::Mode::Source);
    auto plain = editorWith(QStringLiteral("> T\n> body\n"));
    plain->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), callout);
    EXPECT_NEAR(editor->documentHeight(), plain->documentHeight(), 1);
}

TEST(SourceModeTest, ACodeBlockDrawsNoPanel)
{
    const QString note = QStringLiteral("```cpp\nint x;\n```\n");
    auto editor = editorWith(note);
    editor->setMode(MarkdownEditor::Mode::Source);
    editor->show();
    const QImage shot = editor->viewport()->grab().toImage();
    int y = -1;
    for (const auto &box : editor->blockBoxes()) {
        if (box.kind == BlockSpan::Kind::Code)
            y = int(box.rect.center().y());
    }
    ASSERT_GE(y, 0);
    // 12 px left of the text is inside the panel Live Preview paints (14 px
    // out) and outside any text: it must be the page.
    EXPECT_EQ(shot.pixel(editor->textColumn() - 12, y), shot.pixel(2, y));
}

TEST(MarkdownEditorTest, TypingEditsTheBufferAtTheCaret)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);
    QTest::keyClicks(editor.get(), QStringLiteral(" world"));

    EXPECT_EQ(editor->noteText(), QStringLiteral("hello world\n"));
    EXPECT_TRUE(editor->isModified());
    EXPECT_EQ(editor->cursorOffset(), 11);
    // Counted with the parse, after the typing pause, not per keystroke.
    QTest::qWait(80);
    EXPECT_EQ(editor->wordCount(), 2);
}

TEST(MarkdownEditorTest, BackspaceAndUndo)
{
    auto editor = editorWith(QStringLiteral("abc\n"));
    editor->setCursorOffset(3);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    EXPECT_EQ(editor->noteText(), QStringLiteral("ab\n"));

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("abc\n"));

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("ab\n"));
}

TEST(MarkdownEditorTest, SelectAllThenTypeReplaces)
{
    auto editor = editorWith(QStringLiteral("old text\n"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(editor.get(), QStringLiteral("new"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("new"));
}

TEST(MarkdownEditorTest, TypingTwoBracketsClosesThePair)
{
    auto editor = editorWith(QStringLiteral("see \n"));
    editor->setCursorOffset(4);
    QTest::keyClicks(editor.get(), QStringLiteral("[["));
    EXPECT_EQ(editor->noteText(), QStringLiteral("see [[]]\n"));
    EXPECT_EQ(editor->cursorOffset(), 6);
}

/// The `[[` popup: a top-level window, so not among the editor's children.
QListView *linkPopup()
{
    for (QWidget *widget : QApplication::allWidgets()) {
        if (widget->objectName() == QLatin1String("completerPopup"))
            return qobject_cast<QListView *>(widget);
    }
    return nullptr;
}

/// What the `[[` popup lists right now: empty while it is closed.
QStringList linkPopupRows(MarkdownEditor &)
{
    QListView *popup = linkPopup();
    QStringList rows;
    if (!popup || !popup->isVisible())
        return rows;
    for (int row = 0; row < popup->model()->rowCount(); ++row)
        rows << popup->model()->index(row, 0).data().toString();
    return rows;
}

/// Accepts the popup's current row the way Return does.
void acceptLinkPopup(MarkdownEditor &)
{
    QTest::keyClick(linkPopup(), Qt::Key_Return);
}

TEST(MarkdownEditorTest, LinkCompletionOffersANotesHeadingsAfterAHash)
{
    auto editor = editorWith(QStringLiteral("see \n"));
    editor->show();
    editor->setLinkTargets({QStringLiteral("A")});
    int asked = 0;
    editor->setLinkFragmentProvider([&asked](const QString &note, bool blocks) {
        ++asked;
        return note == QLatin1String("A") && !blocks ? QStringList{QStringLiteral("Setup"), QStringLiteral("Usage")}
                                                      : QStringList();
    });
    editor->setCursorOffset(4);
    QTest::keyClicks(editor.get(), QStringLiteral("[[A"));
    EXPECT_EQ(linkPopupRows(*editor), QStringList{QStringLiteral("A")});
    EXPECT_EQ(asked, 0); // names come from setLinkTargets, the provider is not asked
    QTest::keyClicks(editor.get(), QStringLiteral("#"));
    EXPECT_EQ(linkPopupRows(*editor), (QStringList{QStringLiteral("Setup"), QStringLiteral("Usage")}));
    QTest::keyClicks(editor.get(), QStringLiteral("Se"));
    EXPECT_EQ(linkPopupRows(*editor), QStringList{QStringLiteral("Setup")});
    EXPECT_EQ(asked, 1); // once per popup, not per keystroke
    acceptLinkPopup(*editor);
    EXPECT_EQ(editor->noteText(), QStringLiteral("see [[A#Setup]]\n"));
    EXPECT_EQ(editor->cursorOffset(), 15);
}

TEST(MarkdownEditorTest, LinkCompletionOffersThisNotesHeadingsAndBlockIDs)
{
    const QString note = QStringLiteral("# Alpha\n\n## Beta\n\nA line ^abc1\n\nsee ");
    auto editor = editorWith(note);
    editor->show();
    editor->setCursorOffset(note.size());
    QTest::keyClicks(editor.get(), QStringLiteral("[[#"));
    EXPECT_EQ(linkPopupRows(*editor), (QStringList{QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    QTest::keyClicks(editor.get(), QStringLiteral("Be"));
    acceptLinkPopup(*editor);
    EXPECT_TRUE(editor->noteText().endsWith(QStringLiteral("see [[#Beta]]")));

    editor->setNoteText(note);
    editor->setCursorOffset(note.size());
    QTest::keyClicks(editor.get(), QStringLiteral("[[#^"));
    EXPECT_EQ(linkPopupRows(*editor), QStringList{QStringLiteral("abc1")});
    acceptLinkPopup(*editor);
    EXPECT_TRUE(editor->noteText().endsWith(QStringLiteral("see [[#^abc1]]")));
}

TEST(MarkdownEditorTest, LinkCompletionAfterANestedHashOffersTheSubheadings)
{
    // Another note: the provider is asked for what lies under the parent.
    auto editor = editorWith(QStringLiteral("\n"));
    editor->show();
    editor->setLinkTargets({QStringLiteral("A")});
    QStringList asked;
    editor->setLinkFragmentProvider([&asked](const QString &note, bool blocks) {
        asked << note;
        return blocks ? QStringList()
               : note == QLatin1String("A#Setup") ? QStringList{QStringLiteral("Deep"), QStringLiteral("Deeper")}
                                                  : QStringList{QStringLiteral("Setup"), QStringLiteral("Usage")};
    });
    QTest::keyClicks(editor.get(), QStringLiteral("[[A#Setup#"));
    EXPECT_EQ(linkPopupRows(*editor), (QStringList{QStringLiteral("Deep"), QStringLiteral("Deeper")}));
    QTest::keyClicks(editor.get(), QStringLiteral("Deepe"));
    acceptLinkPopup(*editor);
    EXPECT_EQ(editor->noteText(), QStringLiteral("[[A#Setup#Deeper]]\n"));
    EXPECT_EQ(asked, (QStringList{QStringLiteral("A"), QStringLiteral("A#Setup")}));

    // This note: answered from its own headings, under the parent only.
    const QString note = QStringLiteral("# Alpha\n## Sub\n# Beta\n## Other\n\nsee ");
    editor = editorWith(note);
    editor->show();
    editor->setCursorOffset(note.size());
    QTest::keyClicks(editor.get(), QStringLiteral("[[#Beta#"));
    EXPECT_EQ(linkPopupRows(*editor), QStringList{QStringLiteral("Other")});
}

TEST(MarkdownEditorTest, LinkCompletionOffersANotesBlockIDsAfterHashCaret)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->show();
    editor->setLinkTargets({QStringLiteral("A")});
    editor->setLinkFragmentProvider([](const QString &, bool blocks) {
        return blocks ? QStringList{QStringLiteral("x1"), QStringLiteral("y2")}
                      : QStringList{QStringLiteral("Heading")};
    });
    QTest::keyClicks(editor.get(), QStringLiteral("![[A#^"));
    EXPECT_EQ(linkPopupRows(*editor), (QStringList{QStringLiteral("x1"), QStringLiteral("y2")}));
    QTest::keyClicks(editor.get(), QStringLiteral("y"));
    acceptLinkPopup(*editor);
    EXPECT_EQ(editor->noteText(), QStringLiteral("![[A#^y2]]\n"));
}

// `#`, `|`, `^`, `[`, `]` cannot sit in a [[Note#Heading]]: Obsidian writes
// them as spaces, and the link resolves against the heading read the same way.
TEST(MarkdownEditorTest, LinkCompletionWritesAHeadingTheWayALinkSpellsIt)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->show();
    editor->setLinkTargets({QStringLiteral("A")});
    editor->setLinkFragmentProvider([](const QString &, bool blocks) {
        return blocks ? QStringList() : QStringList{QStringLiteral("Q&A | notes [x] #1 ^b"), QStringLiteral("Plain")};
    });
    QTest::keyClicks(editor.get(), QStringLiteral("[[A#Q"));
    EXPECT_EQ(linkPopupRows(*editor), QStringList{QStringLiteral("Q&A notes x 1 b")});
    acceptLinkPopup(*editor);
    EXPECT_EQ(editor->noteText(), QStringLiteral("[[A#Q&A notes x 1 b]]\n"));
}

TEST(MarkdownEditorTest, LinkCompletionOffersAnAliasAndInsertsItWithTheNote)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->show();
    editor->setLinkTargets({QStringLiteral("Ideas"), QStringLiteral("Other")});
    editor->setLinkAliases({{QStringLiteral("Brainwaves"), QStringLiteral("Ideas")}});
    QTest::keyClicks(editor.get(), QStringLiteral("[[Brain"));
    EXPECT_EQ(linkPopupRows(*editor), QStringList{QStringLiteral("Brainwaves -> Ideas")});
    acceptLinkPopup(*editor);
    EXPECT_EQ(editor->noteText(), QStringLiteral("[[Ideas|Brainwaves]]\n"));

    // A plain name still inserts as itself, and an embed completes the same way.
    editor->setNoteText(QStringLiteral("\n"));
    editor->setCursorOffset(0);
    editor->setLinkAliases({{QStringLiteral("Brainwaves"), QStringLiteral("Folder/Ideas")}});
    QTest::keyClicks(editor.get(), QStringLiteral("[[Brain"));
    EXPECT_EQ(linkPopupRows(*editor), QStringList{QStringLiteral("Brainwaves -> Folder/Ideas")});
    acceptLinkPopup(*editor);
    EXPECT_EQ(editor->noteText(), QStringLiteral("[[Folder/Ideas|Brainwaves]]\n"));

    editor->setNoteText(QStringLiteral("\n"));
    editor->setCursorOffset(0);
    QTest::keyClicks(editor.get(), QStringLiteral("![[Oth"));
    acceptLinkPopup(*editor);
    EXPECT_EQ(editor->noteText(), QStringLiteral("![[Other]]\n"));
}

TEST(MarkdownEditorTest, CursorSkipsOverHiddenSyntax)
{
    // "see [[Target|shown]]" renders as "see shown"; moving right from the
    // space must land on the "s" of "shown", not inside the hidden brackets.
    const QString source = QStringLiteral("see [[Target|shown]] end\n");
    auto editor = editorWith(source);

    editor->setCursorOffset(4);
    EXPECT_EQ(editor->displayText(), QStringLiteral("see shown end\n"));
    EXPECT_EQ(editor->noteText(), source);
}

namespace {

QList<MarkdownEditor::SlashCommand> testSlashCommands()
{
    QList<MarkdownEditor::SlashCommand> commands;
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands())
        commands.append({command.id, command.name,
                         command.keys.isEmpty() ? QString() : command.keys.constFirst().toString()});
    return commands;
}

} // namespace

TEST(SlashCommandTest, SlashAtLineStartOpensThePopupWithCommands)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});

    QTest::keyClicks(editor.get(), QStringLiteral("/"));

    EXPECT_TRUE(editor->slashPopupVisible());
    EXPECT_GT(editor->slashSuggestions().size(), 0);
    EXPECT_EQ(editor->noteText(), QStringLiteral("/\n"));
}

TEST(SlashCommandTest, QueryFuzzyFiltersToTheBestMatch)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));

    const QStringList suggestions = editor->slashSuggestions();
    ASSERT_GT(suggestions.size(), 0);
    EXPECT_EQ(suggestions.constFirst(), QStringLiteral("Toggle bold"));
}

TEST(SlashCommandTest, EnterRunsTheHighlightedCommandAndRemovesTheQuery)
{
    auto editor = editorWith(QStringLiteral("see \n"));
    editor->setCursorOffset(4);
    QString ranID;
    editor->setSlashCommands([] { return testSlashCommands(); },
                             [&ranID](const QString &id) { ranID = id; });

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));
    ASSERT_TRUE(editor->slashPopupVisible());
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(ranID, QStringLiteral("editor:toggle-bold"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("see \n")) << "the \"/bol\" query is gone";
    EXPECT_FALSE(editor->slashPopupVisible());
}

TEST(SlashCommandTest, MidWordSlashDoesNotOpenIt)
{
    auto editor = editorWith(QStringLiteral("a\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});
    editor->setCursorOffset(1);

    QTest::keyClicks(editor.get(), QStringLiteral("/b"));

    EXPECT_EQ(editor->noteText(), QStringLiteral("a/b\n"));
    EXPECT_FALSE(editor->slashPopupVisible()) << "\"a/b\" is text, not a command";
}

TEST(SlashCommandTest, NotInsideACodeBlock)
{
    const QString source = QStringLiteral("```\ncode\n```\n");
    auto editor = editorWith(source);
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});
    editor->setCursorOffset(source.indexOf(QStringLiteral("code")) + 2);

    QTest::keyClicks(editor.get(), QStringLiteral("/"));

    EXPECT_FALSE(editor->slashPopupVisible()) << "a fenced block is not where commands run";
}

TEST(SlashCommandTest, EscapeClosesAndLeavesTheText)
{
    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([] { return testSlashCommands(); }, [](const QString &) {});

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));
    ASSERT_TRUE(editor->slashPopupVisible());
    QTest::keyClick(editor.get(), Qt::Key_Escape);

    EXPECT_FALSE(editor->slashPopupVisible());
    EXPECT_EQ(editor->noteText(), QStringLiteral("/bol\n"));
}

TEST(SlashCommandTest, NullSourceMeansSlashIsPlainText)
{
    auto editor = editorWith(QStringLiteral("\n")); // setSlashCommands never called

    QTest::keyClicks(editor.get(), QStringLiteral("/bol"));

    EXPECT_EQ(editor->noteText(), QStringLiteral("/bol\n"));
    EXPECT_FALSE(editor->slashPopupVisible());
}

// The keystroke budget applies here too: fuzzy-filtering a few hundred commands
// on every keystroke must stay well under it.
TEST(SlashCommandTest, FilteringCostStaysInsideTheKeystrokeBudget)
{
    QList<MarkdownEditor::SlashCommand> commands;
    for (int i = 0; i < 400; ++i)
        commands.append({QStringLiteral("plugin:command-%1").arg(i),
                         QStringLiteral("Some plugin command number %1").arg(i), QString()});

    auto editor = editorWith(QStringLiteral("\n"));
    editor->setSlashCommands([commands] { return commands; }, [](const QString &) {});
    QTest::keyClicks(editor.get(), QStringLiteral("/"));
    ASSERT_TRUE(editor->slashPopupVisible());

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    // Best of three: load only ever adds time. Each run types the word and
    // (untimed) backs out of it again.
    const double perKeystroke = perf::bestOf([&] {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClicks(editor.get(), QStringLiteral("command"));
        const double perKey = double(clock.nsecsElapsed()) / QStringLiteral("command").size() / 1e6;
        for (int i = 0; i < QStringLiteral("command").size(); ++i)
            QTest::keyClick(editor.get(), Qt::Key_Backspace);
        return perKey;
    }, 3, 8.0);

    std::printf("[ PERF     ] slash filter, %d commands: %.3f ms per keystroke\n",
                int(commands.size()), perKeystroke);
    EXPECT_LT(perKeystroke, 8.0);
}

namespace {

/// A note of roughly `kilobytes` kB with realistic structure.
QString generateNote(int kilobytes)
{
    QString text;
    int block = 0;
    while (text.size() < kilobytes * 1024) {
        text += QStringLiteral("## Section %1\n\nA paragraph with **bold**, *italic*, "
                               "a [[Wikilink %1]], a #tag%1 and `code`. It runs long enough "
                               "to wrap across a line or two in the editor.\n\n"
                               "- a list item\n- another one\n\n")
                    .arg(block++);
    }
    return text;
}

} // namespace

// The budget: keystroke to paint under 8 ms on a 200 kB note. This
// asserts the parse-and-relayout half, which is the part that can blow it.
TEST(MarkdownEditorTest, KeystrokeStaysInsideTheBudget)
{
    const QString note = generateNote(200);
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor) << "the parse never came back";
    editor->setCursorOffset(note.size() / 2);
    editor->enterBlockAt(note.size() / 2);

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    constexpr int kKeystrokes = 20;
    const double perKeystroke = perf::bestOf([&] {
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < kKeystrokes; ++i)
            QTest::keyClicks(editor.get(), QStringLiteral("x"));
        return double(clock.nsecsElapsed()) / kKeystrokes / 1e6;
    }, 3, 8.0);

    std::printf("[ PERF     ] %.2f kB note, %.2f ms per keystroke\n",
                note.size() / 1024.0, perKeystroke);
    EXPECT_LT(perKeystroke, 8.0);
}

// Enter is parsed at once (a newline changes what the blocks are) while parses are
// cheap, so it is the one key that can pay for a parse: it must stay inside the
// 10 ms a key may cost on a note the parse is cheap for and on one it is not.
TEST(MarkdownEditorTest, EnterStaysInsideTheBudgetWhateverTheNoteSize)
{
    for (const int kilobytes : {20, 120}) {
    SCOPED_TRACE(kilobytes);
    QString note = generateNote(kilobytes);
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor) << "the parse never came back";
    editor->setCursorOffset(note.size() / 2);
    editor->enterBlockAt(note.size() / 2);

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    constexpr int kEnters = 10;
    const double perEnter = perf::bestOf([&] {
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < kEnters; ++i)
            QTest::keyClick(editor.get(), Qt::Key_Return);
        return double(clock.nsecsElapsed()) / kEnters / 1e6;
    }, 3, 10.0);

    std::printf("[ PERF     ] %.2f kB note, %.2f ms per Enter\n", note.size() / 1024.0, perEnter);
    EXPECT_LT(perEnter, 10.0);
    }
}

TEST(MarkdownEditorTest, OpeningALargeNoteIsFast)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const QString note = generateNote(200);
    long long blocks = 0;
    const double milliseconds = perf::bestOf([&] {
        QElapsedTimer clock;
        clock.start();
        auto editor = editorWith(note);
        const double ms = clock.nsecsElapsed() / 1e6;
        blocks = editor->displayText().count(QLatin1Char('\n'));
        return ms;
    }, 3, 300.0);

    std::printf("[ PERF     ] open %.2f kB note: %.2f ms, %lld blocks\n",
                note.size() / 1024.0, milliseconds, blocks);
    EXPECT_LT(milliseconds, 300.0);
}

TEST(MarkdownEditorTest, OpeningAMegabyteNoteIsImmediate)
{
    // Above kAsyncParseThreshold a note is shown before it is parsed: the text
    // goes up as plain paragraphs and the parse runs on the thread pool. So
    // what this measures is the time to something readable, which is what the
    // reader waits for. The parser's own cost is guarded separately, by
    // ParserPerfTest::ParsingStaysLinearInFileSize in test_markdown.cpp.
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const QString small = generateNote(200);
    const QString large = generateNote(1000);

    // Best of seven for each, the two interleaved: a busy machine (parallel
    // builds) only ever adds time, so the minimum is the cost of the code, and
    // a slow stretch then hits both sizes instead of one (best of three, one
    // size after the other, failed about one run in four under load).
    auto openMs = [](const QString &text) {
        QElapsedTimer clock;
        clock.start();
        auto editor = editorWith(text);
        return clock.nsecsElapsed() / 1e6;
    };
    double smallMs = 1e9;
    double largeMs = 1e9;
    for (int run = 0; run < 7; ++run) {
        smallMs = qMin(smallMs, openMs(small));
        largeMs = qMin(largeMs, openMs(large));
    }

    const double sizeRatio = double(large.size()) / double(small.size());
    const double timeRatio = largeMs / qMax(0.001, smallMs);
    std::printf("[ PERF     ] open %.0f kB: %.1f ms, %.0f kB: %.1f ms (%.1fx text, %.1fx time)\n",
                small.size() / 1024.0, smallMs, large.size() / 1024.0, largeMs,
                sizeRatio, timeRatio);

    // Five times the text may cost up to about five times the wait, never more:
    // what is left is linear (about 7 ms for 1 MB on a desktop machine, a 3.7-4.3x
    // ratio), and the bug this guards against was quadratic. The headroom is
    // for the 2 ms small open, where a single scheduler hiccup moves the ratio.
    EXPECT_LT(timeRatio, sizeRatio * 1.25);
    EXPECT_LT(largeMs, 60.0);
}

// A parse landing was the one place every pause in typing cost work in
// proportion to the whole note: every block rebuilt, its layout dropped (13 ms
// on a megabyte). Blocks nothing has drawn are kept now. Counted in views made
// again, not in milliseconds, so a busy machine cannot move it: the landing
// remakes the few views near the caret (about 90 here), against 7 600 at 200 kB
// and 37 000 at a megabyte when every one was rebuilt.
TEST(EditorPerfTest, ParseLandingCostTracksTheEditNotTheNote)
{
    const auto rebuiltByALanding = [](const QString &note, quint64 *rebuilt) {
        auto editor = parsedEditorWith(note);
        if (!editor)
            return false; // the first parse never came back
        editor->setCursorOffset(note.size() / 2);
        editor->enterBlockAt(note.size() / 2);
        (void)editor->grab();
        QTest::keyClicks(editor.get(), QStringLiteral(" x"));
        (void)editor->grab();
        QSignalSpy counted(editor.get(), &MarkdownEditor::wordCountChanged);
        const quint64 before = editor->rebuiltViewCount();
        if (!counted.wait(10000))
            return false; // the landing never came
        *rebuilt = editor->rebuiltViewCount() - before;
        return true;
    };
    quint64 small = 0;
    quint64 large = 0;
    ASSERT_TRUE(rebuiltByALanding(generateNote(200), &small)) << "the 200 kB parse never came back";
    ASSERT_TRUE(rebuiltByALanding(generateNote(1000), &large)) << "the 1 MB parse never came back";
    std::printf("[ PERF     ] parse landing rebuilt %llu views at 200 kB, %llu at 1 MB\n",
                static_cast<unsigned long long>(small), static_cast<unsigned long long>(large));
    EXPECT_GT(small, 0u);
    EXPECT_LT(small, 400u);
    EXPECT_LT(large, 400u);
}

TEST(MarkdownEditorTest, FinishParseAdoptsABigNotesParseWithoutRunningTheEventLoop)
{
    // A grab inside a paint cannot spin an event loop: a timer or a deferred
    // delete firing there frees the owner mid-paint. finishParse() waits on
    // the pool and adopts the parse directly.
    QString text = QStringLiteral("# Title\n\n");
    while (text.size() < 200 * 1024)
        text += QStringLiteral("A paragraph of words to make the note big enough.\n\n");
    auto editor = editorWith(text);
    ASSERT_LT(editor->wordCount(), 0); // parse still out

    bool fired = false;
    QTimer::singleShot(0, editor.get(), [&fired] { fired = true; });
    editor->finishParse();
    EXPECT_GT(editor->wordCount(), 0);
    EXPECT_EQ(editor->headings().size(), 1);
    EXPECT_FALSE(fired);

    // The finished signal arrives afterwards and finds nothing left to do.
    QSignalSpy counted(editor.get(), &MarkdownEditor::wordCountChanged);
    QTest::qWait(50);
    EXPECT_TRUE(fired);
    EXPECT_TRUE(counted.isEmpty());
}

// While a big note's parse is out its text is shown as chunks of 64 lines,
// not a view per paragraph (1.8 kB each: 75 MB for a megabyte). These
// pin that the chunks look and behave as the paragraphs did: the caret, a
// click, a scroll position and a find land on the same lines, before the
// parse lands and after it.
namespace {

/// Plain paragraphs of three lines, roughly 75 bytes each: 2 400 of them is
/// 180 kB, over kAsyncParseThreshold, and nothing in it changes a line's height.
QString plainBigNote(int paragraphs = 2400)
{
    QString text;
    for (int i = 0; i < paragraphs; ++i) {
        const QString n = QStringLiteral("%1").arg(i, 5, 10, QLatin1Char('0'));
        text += QStringLiteral("Paragraph %1 first line\nParagraph %1 second line\nParagraph %1 third line\n\n").arg(n);
    }
    return text;
}

/// Where the caret is in the document, not the viewport: unchanged by scrolling.
qreal caretDocumentY(const MarkdownEditor &editor)
{
    return editor.caretRectNow().y() + editor.verticalScrollBar()->value();
}

/// Waits for the parse a big note's setNoteText started to land.
bool waitForParse(MarkdownEditor &editor)
{
    QElapsedTimer clock;
    clock.start();
    while (editor.wordCount() < 0 && clock.elapsed() < 10000)
        QTest::qWait(10);
    return editor.wordCount() >= 0;
}

std::unique_ptr<MarkdownEditor> placeholderEditorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    return editor;
}

} // namespace

TEST(PlaceholderTest, ABigNoteIsShownAsCoarseChunksUntilItsParseLands)
{
    const QString note = plainBigNote();
    ASSERT_GT(note.size(), 128 * 1024);
    auto editor = placeholderEditorWith(note);
    ASSERT_LT(editor->wordCount(), 0) << "the parse landed before the placeholder could be inspected";
    const quint64 lines = quint64(note.count(QLatin1Char('\n')));
    // One view per 64 lines, give or take the last: 150 here. One per paragraph and
    // blank line was 4 800.
    EXPECT_GE(editor->rebuiltViewCount(), lines / 64);
    EXPECT_LE(editor->rebuiltViewCount(), lines / 64 + 2);

    const qreal placeholderHeight = editor->documentHeight();
    ASSERT_TRUE(waitForParse(*editor));
    std::printf("[ INFO     ] document height %.0f px as placeholder, %.0f px parsed\n", placeholderHeight,
                editor->documentHeight());
    EXPECT_GT(editor->rebuiltViewCount(), 2 * lines / 64) << "the parse's own blocks replace the chunks";
    EXPECT_EQ(editor->noteText(), note);
}

TEST(PlaceholderTest, ACaretInsideAChunkIsPlacedLineByLineAndHitsWhereItIsClicked)
{
    const QString note = plainBigNote();
    auto editor = placeholderEditorWith(note);
    editor->show();
    ASSERT_LT(editor->wordCount(), 0);
    // Deep inside a chunk (line 4 800 of 9 600), on a word in the middle of its line.
    const qsizetype at = note.indexOf(QStringLiteral("Paragraph 01200 second line")) + 14;
    editor->setCursorOffset(at);
    EXPECT_EQ(editor->cursorOffset(), at);
    const QRectF caret = editor->caretRectNow();
    ASSERT_FALSE(caret.isEmpty());
    EXPECT_GE(caret.top(), 0);
    EXPECT_LE(caret.bottom(), editor->viewport()->height()) << "the jump brings the caret on screen";
    EXPECT_GT(editor->verticalScrollBar()->value(), 0);
    EXPECT_GT(caret.x(), editor->textColumn()) << "past the start of its line";
    // 4 800 lines above it: the document y is that many line heights down.
    EXPECT_GT(caretDocumentY(*editor), 50000.0);

    // The lines of a chunk keep one pitch, the one a paragraph's lines have once parsed:
    // the line before, the line after and forty lines on.
    const auto lineY = [&](qsizetype offset) {
        editor->setCursorOffset(offset);
        return caretDocumentY(*editor);
    };
    const qsizetype lineStart = note.lastIndexOf(QLatin1Char('\n'), at) + 1;
    const qsizetype nextLine = note.indexOf(QLatin1Char('\n'), at) + 1;
    const qreal pitch = lineY(nextLine) - lineY(lineStart);
    ASSERT_GT(pitch, 0.0);
    qsizetype forty = lineStart;
    for (int i = 0; i < 40; ++i)
        forty = note.indexOf(QLatin1Char('\n'), forty) + 1;
    EXPECT_NEAR(lineY(forty) - lineY(lineStart), 40 * pitch, 0.5);

    // A click at the caret lands on the caret.
    editor->setCursorOffset(at);
    const QRectF here = editor->caretRectNow();
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(int(here.x()) + 1, int(here.center().y())));
    EXPECT_NEAR(double(editor->cursorOffset()), double(at), 1.0);
    ASSERT_LT(editor->wordCount(), 0);

    // The parsed note has the same pitch, and keeps the caret where the click put it.
    const qsizetype clicked = editor->cursorOffset();
    ASSERT_TRUE(waitForParse(*editor));
    EXPECT_EQ(editor->cursorOffset(), clicked);
    EXPECT_NEAR(lineY(nextLine) - lineY(lineStart), pitch, 0.5);
}

TEST(PlaceholderTest, TypingInsideAChunkEditsTheText)
{
    const QString note = plainBigNote();
    auto editor = placeholderEditorWith(note);
    editor->show();
    ASSERT_LT(editor->wordCount(), 0);
    const qsizetype at = note.indexOf(QStringLiteral("Paragraph 01500 third line")) + 10;
    editor->setCursorOffset(at);
    const qreal x = editor->caretRectNow().x();
    const qreal y = caretDocumentY(*editor);
    QTest::keyClicks(editor.get(), QStringLiteral("Q"));
    EXPECT_EQ(editor->noteText().mid(at, 1), QStringLiteral("Q"));
    EXPECT_EQ(editor->noteText().size(), note.size() + 1);
    EXPECT_EQ(editor->cursorOffset(), at + 1);
    EXPECT_GT(editor->caretRectNow().x(), x);
    EXPECT_NEAR(caretDocumentY(*editor), y, 1.0);
    // The parse that follows the edit leaves the edit and the caret where they were.
    ASSERT_TRUE(waitForParse(*editor));
    QTest::qWait(100);
    EXPECT_EQ(editor->noteText().mid(at, 1), QStringLiteral("Q"));
    EXPECT_EQ(editor->cursorOffset(), at + 1);
}

TEST(PlaceholderTest, AScrollPositionInsideAChunkShowsWholeLinesOfThePitch)
{
    const QString note = plainBigNote();
    auto editor = placeholderEditorWith(note);
    editor->show();
    ASSERT_LT(editor->wordCount(), 0);
    // A pixel position well inside one chunk; the lines under the top and the bottom of the
    // viewport, found with clicks, are one viewport of the line pitch apart.
    editor->verticalScrollBar()->setValue(60000);
    const int x = editor->textColumn() + 4;
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, 3));
    const qsizetype top = editor->cursorOffset();
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, editor->viewport()->height() - 3));
    const qsizetype bottom = editor->cursorOffset();
    ASSERT_GT(top, 0);
    ASSERT_GT(bottom, top);
    ASSERT_LT(editor->wordCount(), 0);
    const int lines = int(note.mid(top, bottom - top).count(QLatin1Char('\n')));
    // The pitch from two neighbouring lines, as above.
    editor->setCursorOffset(top);
    const qreal y0 = caretDocumentY(*editor);
    editor->setCursorOffset(note.indexOf(QLatin1Char('\n'), top) + 1);
    const qreal pitch = caretDocumentY(*editor) - y0;
    ASSERT_GT(pitch, 0.0);
    EXPECT_NEAR(double(lines), (editor->viewport()->height() - 6) / pitch, 1.5);
}

// The chunks are estimated at their true height, the parse's blocks at more, so the
// same scroll pixel would show other text once the parse lands. The landing puts the
// text that was at the top back at the top.
TEST(PlaceholderTest, TheParseLandingKeepsTheTextAtTheTopOfTheViewport)
{
    const QString note = plainBigNote();
    for (const int scroll : {60000, 140000, 200000}) {
        SCOPED_TRACE(scroll);
        auto editor = placeholderEditorWith(note);
        editor->show();
        ASSERT_LT(editor->wordCount(), 0);
        editor->verticalScrollBar()->setValue(scroll);
        const QPoint topLeft(editor->textColumn() + 4, 3);
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, topLeft);
        const qsizetype before = editor->cursorOffset();
        ASSERT_LT(editor->wordCount(), 0);
        ASSERT_TRUE(waitForParse(*editor));
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, topLeft);
        // Within a few lines of what was under the top (a source line is about 25 characters;
        // without the anchor the text moved by thousands of them).
        EXPECT_NEAR(double(editor->cursorOffset()), double(before), 150.0);
    }
}

TEST(PlaceholderTest, FindInsideAChunkLandsOnTheMatchAndShowsIt)
{
    const QString note = plainBigNote();
    auto editor = placeholderEditorWith(note);
    editor->show();
    ASSERT_LT(editor->wordCount(), 0);
    editor->setFindQuery(QStringLiteral("Paragraph 01800 third"), true, false);
    editor->findStep(1);
    ASSERT_LT(editor->wordCount(), 0);
    const qsizetype match = note.indexOf(QStringLiteral("Paragraph 01800 third"));
    EXPECT_EQ(editor->findMatchCount(), 1);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("Paragraph 01800 third"));
    EXPECT_EQ(qMin(editor->cursorOffset(), editor->anchorOffset()), match);
    const QRectF caret = editor->caretRectNow();
    ASSERT_FALSE(caret.isEmpty());
    EXPECT_GE(caret.top(), 0);
    EXPECT_LE(caret.bottom(), editor->viewport()->height()) << "the match is on screen";
    ASSERT_TRUE(waitForParse(*editor));
    EXPECT_EQ(editor->findMatchCount(), 1);
    EXPECT_EQ(qMin(editor->cursorOffset(), editor->anchorOffset()), match);
}

// The application hands memory back once the parse of a big note has landed (the
// parser's temporaries are free then and stay resident otherwise); the editor only
// says when. Once per note, not once per typing pause.
TEST(PlaceholderTest, TheBigParseLandedHookFiresOncePerBigNoteAndNeverForASmallOne)
{
    int fired = 0;
    MarkdownEditor::setBigParseLandedHook([&fired] { ++fired; });
    const auto reset = qScopeGuard([] { MarkdownEditor::setBigParseLandedHook({}); });
    {
        auto small = placeholderEditorWith(QStringLiteral("# Small\n\nA note.\n"));
        QTest::qWait(100);
        EXPECT_EQ(fired, 0);
    }
    const QString note = plainBigNote();
    auto editor = placeholderEditorWith(note);
    ASSERT_LT(editor->wordCount(), 0);
    EXPECT_EQ(fired, 0) << "not before the parse has landed";
    ASSERT_TRUE(waitForParse(*editor));
    EXPECT_EQ(fired, 1);
    // An edit and the parse that follows it are not a new note.
    editor->setCursorOffset(note.size() / 2);
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    const quint64 rebuilt = editor->rebuiltViewCount();
    for (int waited = 0; editor->rebuiltViewCount() == rebuilt && waited < 10000; waited += 20)
        QTest::qWait(20); // the debounced parse of the edit has landed when the views were rebuilt
    EXPECT_GT(editor->rebuiltViewCount(), rebuilt);
    EXPECT_EQ(fired, 1);
    // A second big note is one more.
    editor->setNoteText(note + QStringLiteral("more\n"));
    ASSERT_TRUE(waitForParse(*editor));
    EXPECT_EQ(fired, 2);
}

// What a remembered place is made of: the text at the top, not a pixel. In the chunks it is
// placed exactly (the chunk, then the line inside it), and the landing keeps it.
TEST(PlaceholderTest, ARememberedTopOffsetIsPlacedExactlyInAChunkAndKeptByTheLanding)
{
    const QString note = plainBigNote();
    const auto lineOf = [&note](qsizetype offset) { return int(note.left(offset).count(QLatin1Char('\n'))); };
    for (const QString &marker : {QStringLiteral("Paragraph 01500 second line"), QStringLiteral("Paragraph 00031 third line"),
                                  QStringLiteral("Paragraph 02300 first line")}) {
        SCOPED_TRACE(marker.toStdString());
        auto editor = placeholderEditorWith(note);
        editor->show();
        ASSERT_LT(editor->wordCount(), 0);
        const qsizetype wanted = note.indexOf(marker);
        editor->restoreScroll(wanted, 0);
        EXPECT_NEAR(lineOf(editor->topOffset()), lineOf(wanted), 1.0) << "placed in the chunk";
        ASSERT_LT(editor->wordCount(), 0);
        ASSERT_TRUE(waitForParse(*editor));
        EXPECT_NEAR(lineOf(editor->topOffset()), lineOf(wanted), 1.0) << "after the landing";
    }
    auto editor = placeholderEditorWith(note);
    editor->restoreScroll(-1, 5000);
    EXPECT_EQ(editor->verticalScrollBar()->value(), 5000) << "an entry from before offsets is a pixel";
    editor->restoreScroll(0, 0);
    EXPECT_EQ(editor->verticalScrollBar()->value(), 0);
}

// A line with only a few pixels left at the top is as good as gone, and the one that shows is the
// next line of the same block, not the next block's start: inside a chunk of short lines and
// inside a long wrapped paragraph, swept over more than a line pitch.
TEST(PlaceholderTest, ATopLineWithFewPixelsLeftNamesTheNextLineOfItsOwnBlock)
{
    QString dense;
    for (int i = 0; i < 6000; ++i)
        dense += QStringLiteral("Line %1 of the dense note\n").arg(i, 5, 10, QLatin1Char('0'));
    QString paragraph;
    while (paragraph.size() < 3000)
        paragraph += QStringLiteral("lorem ipsum dolor sit amet ");
    QString wrapped;
    while (wrapped.size() < 200 * 1024)
        wrapped += paragraph + QStringLiteral("\n\n");
    for (const QString &note : {dense, wrapped}) {
        auto editor = placeholderEditorWith(note);
        editor->show();
        const bool placeholder = note.size() == dense.size();
        if (!placeholder)
            ASSERT_TRUE(waitForParse(*editor));
        else
            ASSERT_LT(editor->wordCount(), 0);
        qsizetype worst = 0;
        for (int px = placeholder ? 40000 : 20000; px < (placeholder ? 41000 : 21000); ++px) {
            editor->verticalScrollBar()->setValue(px);
            const qsizetype top = editor->topOffset();
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(editor->textColumn() + 4, 12));
            worst = qMax(worst, qAbs(top - editor->cursorOffset()));
        }
        // One line is 26 characters in the chunks and about 90 wrapped; two lines at most.
        EXPECT_LT(worst, placeholder ? 2 * 26 : 2 * 100) << (placeholder ? "chunks" : "wrapped paragraphs");
    }
}

/// Waits until `fired` reaches `count`, 10 s at most.
bool waitForFired(const int &fired, int count)
{
    for (int waited = 0; fired < count && waited < 10000; waited += 10)
        QTest::qWait(10);
    return fired >= count;
}

// Opened before the first one's parse has landed, a second big note has its own landing, and
// that one gets the trim. The first's result is adopted onto it first (the editor does that
// today); the text at the top of the viewport stays put through both.
TEST(PlaceholderTest, ASecondBigNoteBeforeTheFirstLandsGetsItsOwnTrimAndAnchor)
{
    int fired = 0;
    MarkdownEditor::setBigParseLandedHook([&fired] { ++fired; });
    const auto reset = qScopeGuard([] { MarkdownEditor::setBigParseLandedHook({}); });
    const QString first = plainBigNote();
    const QString second = plainBigNote(2600);
    auto editor = placeholderEditorWith(first);
    editor->show();
    editor->setNoteText(second);
    ASSERT_LT(editor->wordCount(), 0);
    editor->verticalScrollBar()->setValue(60000);
    const QPoint topLeft(editor->textColumn() + 4, 3);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, topLeft);
    const qsizetype before = editor->cursorOffset();
    ASSERT_TRUE(waitForParse(*editor));
    EXPECT_EQ(fired, 0) << "the first note's landing is not the second's";
    ASSERT_TRUE(waitForFired(fired, 1));
    EXPECT_EQ(fired, 1);
    QTest::qWait(100);
    EXPECT_EQ(fired, 1);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, topLeft);
    EXPECT_NEAR(double(editor->cursorOffset()), double(before), 150.0);
}

// A change of parse options in the placeholder window parses the note on the spot and drops
// the one in flight: the placeholder is gone, so the trim is due now, and it is not due again.
TEST(PlaceholderTest, AParseOptionsChangeDuringThePlaceholderFiresTheHookOnceNow)
{
    int fired = 0;
    MarkdownEditor::setBigParseLandedHook([&fired] { ++fired; });
    const auto reset = qScopeGuard([] { MarkdownEditor::setBigParseLandedHook({}); });
    const QString note = plainBigNote();
    auto editor = placeholderEditorWith(note);
    ASSERT_LT(editor->wordCount(), 0);
    ParseOptions options = editor->parseOptions();
    options.brackets = !options.brackets;
    editor->setParseOptions(options);
    EXPECT_GE(editor->wordCount(), 0);
    EXPECT_EQ(fired, 1);
    editor->setCursorOffset(note.size() / 2);
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    const quint64 rebuilt = editor->rebuiltViewCount();
    for (int waited = 0; editor->rebuiltViewCount() == rebuilt && waited < 10000; waited += 20)
        QTest::qWait(20);
    EXPECT_GT(editor->rebuiltViewCount(), rebuilt);
    EXPECT_EQ(fired, 1) << "the parse after an edit is not a landing";
}

namespace {

/// About 1 MB of lines of `lineBytes` bytes, each followed by a blank line.
QString longLineNote(int lineBytes)
{
    QString line;
    while (line.size() < lineBytes)
        line += QStringLiteral("lorem ipsum dolor ");
    line.truncate(lineBytes);
    QString text;
    while (text.size() < 1024 * 1024)
        text += line + QStringLiteral("\n\n");
    return text;
}

} // namespace

// A chunk of 64 lines of 4 kB is 256 kB in one layout: a keystroke in it relaid all of it
// (7 to 28 ms at 4 and 16 kB lines). A chunk ends at 4 kB of text as well, so the cost is
// the paragraph's, as it was before the chunks.
TEST(PlaceholderTest, AKeystrokeInTheBigPlaceholderStaysInsideTheBudgetOnLongLines)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    for (const int lineBytes : {4000, 16000}) {
        SCOPED_TRACE(lineBytes);
        const QString note = longLineNote(lineBytes);
        double median = 1e9;
        double worst = 1e9;
        for (int attempt = 0; attempt < 3 && (median >= 10.0 || worst >= 10.0); ++attempt) {
            auto editor = placeholderEditorWith(note);
            editor->show();
            ASSERT_LT(editor->wordCount(), 0);
            editor->setCursorOffset(note.size() / 2);
            (void)editor->grab();
            std::vector<double> samples;
            for (int i = 0; i < 30; ++i) {
                QElapsedTimer clock;
                clock.start();
                QTest::keyClicks(editor.get(), QStringLiteral("a"));
                (void)editor->grab();
                samples.push_back(clock.nsecsElapsed() / 1e6);
            }
            ASSERT_LT(editor->wordCount(), 0) << "the parse landed inside the measurement";
            std::sort(samples.begin(), samples.end());
            median = samples[samples.size() / 2];
            worst = samples.back();
        }
        std::printf("[ PERF     ] keystroke in the placeholder, %d-byte lines: median %.2f ms, max %.2f ms\n",
                    lineBytes, median, worst);
        EXPECT_LT(median, 10.0);
        EXPECT_LT(worst, 10.0);
    }
}

TEST(MarkdownEditorTest, SoftBreaksJoinLinesTheWayObsidianDoes)
{
    // One paragraph across two source lines renders as one flowing line;
    // the blank line between paragraphs still breaks.
    auto editor = editorWith(QStringLiteral("one\ntwo\n\nthree\n"));
    EXPECT_EQ(editor->displayText(), QStringLiteral("one\ntwo\n\nthree\n"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("one\ntwo\n\nthree\n"));
    // The buffer is untouched by the rendering decision, which is the point.
    EXPECT_EQ(editor->wordCount(), 3);
}

// Obsidian's "Strict line breaks" is off by default, and off means a newline in
// a paragraph IS a line break (the setting follows CommonMark's joining only
// when on, and only in reading view). The editor had it the other way round:
// the debounced parse after typing marked the new line as the same paragraph
// and drew the newline as a space, so what was typed after Enter ran on at the
// end of the line above.
TEST(SoftBreakTest, ATypedNewLineStaysOnItsOwnLineAfterTheReparse)
{
    for (const QString &start : {QStringLiteral("line one\n"), QStringLiteral("line one"),
                                  QStringLiteral("# Title\n\npara\n")}) {
        SCOPED_TRACE(start.toStdString());
        auto editor = editorWith(start);
        editor->show();
        const qsizetype end = start.endsWith(QLatin1Char('\n')) ? start.size() - 1 : start.size();
        editor->setCursorOffset(end);
        const qreal firstLine = editor->caretRectNow().y();

        QTest::keyClick(editor.get(), Qt::Key_Return);
        QTest::qWait(300); // past the parse timer: the new line is in the paragraph now
        QTest::keyClicks(editor.get(), QStringLiteral("abc"));
        QTest::qWait(300);

        EXPECT_EQ(editor->cursorOffset(), end + 4);
        EXPECT_GT(editor->caretRectNow().y(), firstLine) << "typing ran on at the end of the line above";
    }
}

TEST(SoftBreakTest, StrictLineBreaksJoinTheLinesOfAParagraph)
{
    const QString text = QStringLiteral("one\ntwo\n\nthree\n");
    const auto threeAt = [&](bool strict) {
        auto editor = editorWith(text);
        editor->show();
        editor->setStrictLineBreaks(strict);
        editor->setCursorOffset(text.indexOf(QLatin1String("three")));
        QTest::qWait(100);
        return editor->caretRectNow().y();
    };
    EXPECT_LT(threeAt(true), threeAt(false)) << "strict: the two lines above are one, so the next paragraph sits higher";
}

// Pressing Enter must not make the text jump. What is on screen straight after
// the key is the edit, and nothing the parse, the timers or the scroll do later
// may move a line of it: a line that sits at a different y a frame later,
// though the note did not change, is the jitter.
namespace {

/// The top y of every band of ink in the viewport - one per drawn line of text.
/// The caret's own column is blanked, so only text is compared.
QList<int> inkBands(MarkdownEditor &editor)
{
    QImage frame = editor.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const QRgb background = frame.pixel(frame.width() - 4, 4);
    const QRectF caret = editor.caretRectNow();
    QList<int> bands;
    bool inBand = false;
    for (int y = 0; y < frame.height(); ++y) {
        bool ink = false;
        for (int x = 0; x < frame.width() - 4 && !ink; ++x) {
            if (x >= caret.left() - 2 && x <= caret.right() + 2)
                continue;
            ink = frame.pixel(x, y) != background;
        }
        if (ink && !inBand)
            bands.append(y);
        inBand = ink;
    }
    return bands;
}

/// The viewport as drawn, with the caret's own box blanked: it blinks.
QImage withoutCaret(MarkdownEditor &editor)
{
    QImage frame = editor.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    QPainter painter(&frame);
    painter.fillRect(editor.caretRectNow().adjusted(-3, -2, 3, 2), Qt::black);
    return frame;
}

/// Which rows differ between two frames, "" when they are the same picture.
QString diffRows(const QImage &now, const QImage &before)
{
    if (now.size() != before.size())
        return QStringLiteral("size");
    QStringList rows;
    int first = -1;
    for (int y = 0; y <= now.height(); ++y) {
        const bool differs = y < now.height() && std::memcmp(now.constScanLine(y), before.constScanLine(y), size_t(now.bytesPerLine())) != 0;
        if (differs && first < 0)
            first = y;
        if (!differs && first >= 0) {
            rows << QStringLiteral("%1-%2").arg(first).arg(y - 1);
            first = -1;
        }
    }
    return rows.join(QLatin1Char(' '));
}

QString bandsText(const QList<int> &bands)
{
    QStringList parts;
    for (int y : bands)
        parts << QString::number(y);
    return parts.join(QLatin1Char(' '));
}

} // namespace

TEST(EnterJitterTest, NoLineOfTextMovesAfterEnterOnceTheEditIsDrawn)
{
    struct Case {
        const char *name;
        QString text;
        QString endOf; // the caret goes to the end of the line holding this; "" is the end of the note
        int back = 0;  // ... and this many characters back from there
        int lift = 0;  // the view is scrolled up this far, the caret mid-screen
        bool slow = false; // the last parse was a slow one: the newline waits for the debounce
        bool plain = false; // no letter typed first (it would end a setext underline)
    };
    QString long60;
    for (int i = 0; i < 60; ++i)
        long60 += QStringLiteral("paragraph number %1\n\n").arg(i);
    QList<Case> cases = {
        {"paragraph", QStringLiteral("first paragraph\n\nsecond paragraph here\n\nthird paragraph\n\nfourth\n"),
         QStringLiteral("second paragraph here")},
        {"insideParagraph", QStringLiteral("aa\nbb\ncc\n\ndd\n\nee\n"), QStringLiteral("bb")},
        {"listItem", QStringLiteral("intro\n\n- one\n- two\n- three\n\nafter\n\nmore\n"), QStringLiteral("- two")},
        {"lastListItem", QStringLiteral("intro\n\n- one\n- two\n\nafter\n\nmore\n"), QStringLiteral("- two")},
        {"orderedItem", QStringLiteral("intro\n\n1. one\n2. two\n3. three\n\nafter\n"), QStringLiteral("2. two")},
        {"heading", QStringLiteral("# Title\n\npara\n\nanother\n\nlast\n"), QStringLiteral("# Title")},
        {"subheading", QStringLiteral("intro\n\n## Sub heading\nbody under it\n\nlast\n"), QStringLiteral("## Sub heading")},
        {"blockquote", QStringLiteral("intro\n\n> quote line\n\nafter\n\nmore\n"), QStringLiteral("> quote line")},
        {"endOfNote", QStringLiteral("alpha\n\nbeta\n\ngamma"), QString()},
        {"endOfNoteNewline", QStringLiteral("alpha\n\nbeta\n\ngamma\n"), QString()},
        {"endOfLongNote", long60, QString()},
        {"middleOfLongNote", long60, QStringLiteral("paragraph number 30")},
        {"midScreenOfLongNote", long60, QStringLiteral("paragraph number 40"), 0, 300},
        {"midScreenListOfLongNote", long60 + QStringLiteral("- a\n- b\n- c\n\n") + long60, QStringLiteral("- b"), 0, 300},
        {"midHeading", QStringLiteral("# Title here\n\npara\n\nanother\n\nlast\n"), QStringLiteral("# Title here"), 5},
        {"midListItem", QStringLiteral("intro\n\n- one\n- two words\n- three\n\nafter\n\nmore\n"),
         QStringLiteral("- two words"), 5},
        {"midBlockquote", QStringLiteral("intro\n\n> quote line here\n\nafter\n\nmore\n"),
         QStringLiteral("> quote line here"), 5},
        {"headingEndOfNote", QStringLiteral("para\n\n## Last heading"), QString()},
        {"setext", QStringLiteral("intro\n\nTitle\n=====\n\nafter\n\nmore\n"), QStringLiteral("====="), 0, 0, false, true},
        {"slowHeading", QStringLiteral("# Title\n\npara\n\nanother\n\nlast\n"), QStringLiteral("# Title"), 0, 0, true},
        {"slowSetext", QStringLiteral("intro\n\nTitle\n=====\n\nafter\n\nmore\n"), QStringLiteral("====="), 0, 0, true, true},
        {"slowSetextMid", QStringLiteral("intro\n\nTitle words\n=====\n\nafter\n\nmore\n"), QStringLiteral("Title words"), 5, 0, true, true},
    };
    // Past the size a newline is parsed on the spot at, the stale blocks are drawn
    // until the pool parse lands, and a heading must look like what it becomes.
    const QString filler = generateNote(40);
    for (const char *name : {"paragraph", "heading", "subheading", "midHeading"}) {
        const Case &small = *std::find_if(cases.cbegin(), cases.cend(),
                                          [name](const Case &c) { return std::strcmp(c.name, name) == 0; });
        cases.append({"bigNote", small.text + QStringLiteral("\n") + filler, small.endOf, small.back, small.lift, true});
    }

    for (const Case &c : cases) {
        SCOPED_TRACE(c.name);
        auto editor = editorWith(c.text);
        editor->show();
        qsizetype at = c.text.size();
        if (!c.endOf.isEmpty()) {
            const qsizetype hit = c.text.indexOf(c.endOf);
            ASSERT_GE(hit, 0);
            at = hit + c.endOf.size() - c.back;
        }
        editor->setCursorOffset(at);
        QTest::qWait(300); // the open settles: parse, scroll to the caret
        if (!c.plain)
            QTest::keyClicks(editor.get(), QStringLiteral("x")); // the caret's block is raw, as in a real session
        QTest::qWait(300);
        editor->setCursorOffset(c.plain ? at : at + 1);
        QTest::qWait(100);

        if (c.lift > 0) {
            editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->value() - c.lift);
            QTest::qWait(100);
        }
        const QImage beforeEnter = withoutCaret(*editor);
        const qreal caretBefore = editor->caretRectNow().y();
        if (c.slow)
            editor->setLastParseTimeForTest(50'000'000);
        QTest::keyClick(editor.get(), Qt::Key_Return);
        struct Frame {
            const char *when;
            QList<int> bands;
            int scroll;
            qreal caretY;
            QImage image;
            qreal caretH;
        };
        QList<Frame> frames;
        const auto snap = [&](const char *when) {
            frames.append({when, inkBands(*editor), editor->verticalScrollBar()->value(),
                           editor->caretRectNow().y(), withoutCaret(*editor), editor->caretRectNow().height()});
        };
        snap("immediately");
        QCoreApplication::processEvents();
        snap("afterEvents");
        QTest::qWait(10);
        snap("10ms");
        QTest::qWait(30);
        snap("40ms(parseLanded)");
        QTest::qWait(100);
        snap("140ms");
        QTest::qWait(500);
        snap("640ms");

        // The edit itself: the caret is on screen, and when the view stayed put
        // what sat above the caret's line is the same picture as before.
        EXPECT_GE(frames.last().caretY, 0) << "the caret left the view";
        EXPECT_LT(frames.last().caretY, editor->viewport()->height()) << "the caret left the view";
        if (frames[0].caretY > caretBefore + 1) { // the view stayed put: what is above is what was
            const int rows = qMax(0, int(caretBefore) - 48);
            EXPECT_EQ(diffRows(frames.last().image.copy(0, 0, beforeEnter.width(), rows),
                               beforeEnter.copy(0, 0, beforeEnter.width(), rows))
                          .toStdString(),
                      "")
                << "text above the caret moved";
        }
        for (qsizetype i = 1; i < frames.size(); ++i) {
            EXPECT_EQ(frames[i].scroll, frames[0].scroll) << frames[i].when << " scrolled";
            EXPECT_NEAR(frames[i].caretY, frames[0].caretY, 0.5) << frames[i].when << " caret moved";
            EXPECT_EQ(frames[i].caretH, frames[0].caretH) << frames[i].when << " caret changed size";
            EXPECT_EQ(bandsText(frames[i].bands).toStdString(), bandsText(frames[0].bands).toStdString())
                << frames[i].when << " moved text that had already been drawn";
            EXPECT_EQ(diffRows(frames[i].image, frames[0].image).toStdString(), "")
                << frames[i].when << " drew something other than the edit's own frame";
        }
    }
}

// The note's own last line: a note ends in a newline, and the empty line after it
// belongs to no block, so there was nowhere to draw the caret. Enter at the end
// left it on the line above (or on the top of the last block), and the next
// character then dropped a line.
TEST(EnterJitterTest, EnterAtTheEndOfTheNoteMovesTheCaretToTheNewLineAtOnce)
{
    struct Case {
        const char *name;
        QString text;
        bool press; // Enter pressed at the end; else the note already ends in a newline
        bool list = false;
    };
    QString long60;
    for (int i = 0; i < 60; ++i)
        long60 += QStringLiteral("paragraph number %1\n\n").arg(i);
    const QList<Case> cases = {
        {"paragraph", QStringLiteral("alpha\n\nbeta\n\ngamma"), true},
        {"multiLineParagraph", QStringLiteral("intro\n\none\ntwo\nthree"), true},
        {"newlineEnded", QStringLiteral("alpha\n\nbeta\n\ngamma\n"), false},
        {"heading", QStringLiteral("para\n\n# Title"), true},
        {"subheading", QStringLiteral("para\n\n## Sub heading"), true},
        {"listItem", QStringLiteral("para\n\n- item"), true, true},
        {"quote", QStringLiteral("para\n\n> quoted"), true},
        {"longNote", long60 + QStringLiteral("last words"), true},
        {"longNoteNewlineEnded", long60 + QStringLiteral("last words\n"), false},
    };

    for (const Case &c : cases) {
        SCOPED_TRACE(c.name);
        auto editor = editorWith(c.text);
        editor->show();
        const auto docY = [&] { return editor->caretRectNow().y(); };
        const qsizetype end = c.text.size();
        // The line the text ends on: the end of its last character.
        editor->setCursorOffset(c.press ? end : end - 1);
        QTest::qWait(300);
        const qreal lastLine = docY();
        editor->setCursorOffset(end);
        if (c.press)
            QTest::keyClick(editor.get(), Qt::Key_Return);

        const QRectF afterEnter = editor->caretRectNow();
        const qreal moved = docY() - lastLine;
        // Near the bottom of the view it scrolls to follow the caret, and the
        // distance in the view says nothing.
        if (lastLine < editor->viewport()->height() - 60) {
            EXPECT_GT(moved, 15) << "the caret is not on a line of its own below the text";
            EXPECT_LT(moved, 45) << "the caret is more than a line below the text";
        }
        EXPECT_GE(afterEnter.y(), 0) << "the caret left the view";
        EXPECT_LT(afterEnter.y() + afterEnter.height(), editor->viewport()->height() + 1) << "the caret left the view";
        if (!c.list)
            EXPECT_NEAR(afterEnter.x(), editor->textColumn() - 1, 1.5) << "not at the start of the line";

        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QRectF afterX = editor->caretRectNow();
        EXPECT_NEAR(afterX.y(), afterEnter.y(), 0.5) << "the next character landed on another line";
        EXPECT_GT(afterX.x(), afterEnter.x()) << "the character is not where the caret was";
        QTest::qWait(300); // the parse lands
        const QRectF parsed = editor->caretRectNow();
        EXPECT_NEAR(parsed.y(), afterX.y(), 0.5);
        EXPECT_NEAR(parsed.x(), afterX.x(), 0.5);
    }
}

// What a newline costs: it is parsed on the spot only for the reader's own key,
// only while parses are cheap, and never twice.
TEST(EnterJitterTest, ANewlineIsParsedOnTheSpotOnlyForTheReadersOwnKeyWhileParsesAreCheap)
{
    const QString note = QStringLiteral("# Title\n\nparagraph one\n\n- a\n- b\n\nparagraph two\n");
    const qsizetype at = note.indexOf(QStringLiteral("paragraph one")) + 5;

    // The reader's own Enter: one parse, at once.
    {
        auto editor = editorWith(note);
        editor->setCursorOffset(at);
        const int before = editor->parseCountForTest();
        QTest::keyClick(editor.get(), Qt::Key_Return);
        EXPECT_EQ(editor->parseCountForTest(), before + 1);
        QTest::qWait(100);
        EXPECT_EQ(editor->parseCountForTest(), before + 1) << "parsed again by the debounce";
    }
    // The same note open in a second pane: the mirrored edit is not parsed on
    // the spot, the debounce does it once.
    {
        auto first = editorWith(note);
        auto second = editorWith(note);
        QObject::connect(first.get(), &MarkdownEditor::edited, second.get(), &MarkdownEditor::applyExternalEdit);
        first->setCursorOffset(at);
        const int firstBefore = first->parseCountForTest();
        const int secondBefore = second->parseCountForTest();
        QTest::keyClick(first.get(), Qt::Key_Return);
        EXPECT_EQ(first->parseCountForTest(), firstBefore + 1);
        EXPECT_EQ(second->parseCountForTest(), secondBefore) << "the other pane parsed inside the key";
        QTest::qWait(100);
        EXPECT_EQ(second->parseCountForTest(), secondBefore + 1);
        EXPECT_EQ(second->noteText(), first->noteText());
    }
    // A burst from elsewhere (a peer catching up): 50 edits with a newline each,
    // one parse once it is over.
    {
        auto editor = editorWith(note);
        const int before = editor->parseCountForTest();
        for (int i = 0; i < 50; ++i)
            editor->applyExternalEdit(note.size() / 2, 0, QStringLiteral("edit %1\n").arg(i));
        EXPECT_EQ(editor->parseCountForTest(), before) << "parsed inside the burst";
        QTest::qWait(100);
        EXPECT_EQ(editor->parseCountForTest(), before + 1);
    }
    // A slow parse makes the next newline wait for the debounce, on any machine.
    {
        auto editor = editorWith(note);
        editor->setCursorOffset(at);
        editor->setLastParseTimeForTest(50'000'000);
        const int before = editor->parseCountForTest();
        QTest::keyClick(editor.get(), Qt::Key_Return);
        EXPECT_EQ(editor->parseCountForTest(), before) << "a slow parse ran inside the key";
        QTest::qWait(150);
        EXPECT_EQ(editor->parseCountForTest(), before + 1);
        // ... and a fast one measured since lets the next one be immediate again.
        const int measured = editor->parseCountForTest();
        QTest::keyClick(editor.get(), Qt::Key_Return);
        EXPECT_EQ(editor->parseCountForTest(), measured + 1);
    }
    // A programmatic edit is not the reader's key.
    {
        auto editor = editorWith(note);
        const int before = editor->parseCountForTest();
        editor->insertAtCaret(QStringLiteral("\n"));
        EXPECT_EQ(editor->parseCountForTest(), before);
    }
}

// The view stays put when the parse lands, in a note long enough that blocks
// above the view are estimates: the parse re-estimates them, and without the
// anchor everything on screen moves by the difference.
TEST(EnterJitterTest, ALetterInALongNoteDoesNotMoveTheViewWhenTheParseLands)
{
    QString long60;
    for (int i = 0; i < 60; ++i)
        long60 += QStringLiteral("paragraph number %1\n\n").arg(i);
    struct Case {
        const char *name;
        QString text;
        QString at;
        int lift;
    };
    const QList<Case> cases = {
        {"end", long60, QStringLiteral("paragraph number 59"), 0},
        {"midScreen", long60, QStringLiteral("paragraph number 40"), 300},
        {"bigEnd", generateNote(40), QStringLiteral("another one"), 0},
        {"bigMidScreen", generateNote(40), QStringLiteral("Section 30"), 300},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(c.name);
        auto editor = editorWith(c.text);
        editor->show();
        const qsizetype at = c.text.lastIndexOf(c.at) + c.at.size();
        editor->setCursorOffset(at);
        QTest::qWait(300);
        if (c.lift > 0) {
            editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->value() - c.lift);
            QTest::qWait(100);
        }
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        const QImage typed = withoutCaret(*editor);
        const qreal caret = editor->caretRectNow().y();
        QTest::qWait(300); // the parse lands
        EXPECT_NEAR(editor->caretRectNow().y(), caret, 0.5) << "the caret moved when the parse landed";
        EXPECT_EQ(diffRows(withoutCaret(*editor), typed).toStdString(), "") << "the text moved when the parse landed";
    }
}

// The adaptive budget times the parse and nothing else, on both paths: a
// synchronous Enter once also timed the layout after it, so the next Enter in a
// note of 20-30 kB was debounced, and the one after parsed again at once.
TEST(EnterJitterTest, EveryEnterInANoteOfMiddlingSizeIsParsedOnTheSpot)
{
    for (int kilobytes : {20, 24}) { // Limit: 28 kB parses in 4.2 ms on this loaded box, over the budget on purpose
        SCOPED_TRACE(kilobytes);
        const QString note = generateNote(kilobytes);
        auto editor = editorWith(note);
        editor->show();
        QTest::qWait(300);
        editor->setCursorOffset(note.indexOf(QStringLiteral("Section 20")) + 5);
        for (int i = 0; i < 10; ++i) {
            const int before = editor->parseCountForTest();
            QTest::keyClick(editor.get(), Qt::Key_Return);
            EXPECT_EQ(editor->parseCountForTest(), before + 1) << "Enter number " << i << " waited for the debounce";
        }
    }
}

// The empty line after the note's final newline is a line like any other: it can
// be reached from the line above, by a click, by End and Ctrl+End, and a
// selection to the end takes it.
TEST(EnterJitterTest, TheLineAfterTheFinalNewlineCanBeReached)
{
    const QString note = QStringLiteral("alpha\n\nbeta\n\ngamma\n");
    auto editor = editorWith(note);
    editor->show();
    QTest::qWait(300);
    const qsizetype end = note.size();
    const qsizetype inGamma = note.indexOf(QStringLiteral("gamma")) + 2;

    editor->setCursorOffset(inGamma);
    QTest::keyClick(editor.get(), Qt::Key_Down);
    EXPECT_EQ(editor->cursorOffset(), end) << "Down from the last line";
    const QRectF tail = editor->caretRectNow();
    QTest::keyClick(editor.get(), Qt::Key_Down);
    EXPECT_EQ(editor->cursorOffset(), end) << "Down on the last line stays";
    QTest::keyClick(editor.get(), Qt::Key_Up);
    EXPECT_GE(editor->cursorOffset(), note.indexOf(QStringLiteral("gamma"))) << "Up from the tail";
    EXPECT_LT(editor->cursorOffset(), end);

    editor->setCursorOffset(0);
    const QPoint onTail(editor->textColumn() + 40, int(tail.center().y()));
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, onTail);
    EXPECT_EQ(editor->cursorOffset(), end) << "a click on the tail line";
    editor->setCursorOffset(0);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, onTail + QPoint(0, 40));
    EXPECT_EQ(editor->cursorOffset(), end) << "a click under it";
    editor->setCursorOffset(0);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->textColumn() + 10, int(tail.y()) - 20));
    EXPECT_LT(editor->cursorOffset(), end) << "a click on the last text line is still on it";

    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_End, Qt::ControlModifier);
    EXPECT_EQ(editor->cursorOffset(), end) << "Ctrl+End";
    EXPECT_NEAR(editor->caretRectNow().y(), tail.y(), 0.5);
    QTest::keyClick(editor.get(), Qt::Key_End);
    EXPECT_EQ(editor->cursorOffset(), end) << "End on the tail line stays on it";

    editor->setCursorOffset(inGamma);
    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_EQ(editor->cursorOffset(), end) << "Shift+Down";
    EXPECT_EQ(editor->selectedText(), note.mid(inGamma));
    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_End, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(editor->selectedText(), note) << "Ctrl+Shift+End takes the whole note";

    // Down in a last paragraph of several lines goes line by line first.
    const QString several = QStringLiteral("intro\n\none\ntwo\nthree\n");
    auto other = editorWith(several);
    other->show();
    QTest::qWait(300);
    other->setCursorOffset(several.indexOf(QStringLiteral("one")) + 1);
    QTest::keyClick(other.get(), Qt::Key_Down);
    EXPECT_EQ(other->cursorOffset(), several.indexOf(QStringLiteral("two")) + 1);
    QTest::keyClick(other.get(), Qt::Key_Down);
    EXPECT_EQ(other->cursorOffset(), several.indexOf(QStringLiteral("three")) + 1);
    QTest::keyClick(other.get(), Qt::Key_Down);
    EXPECT_EQ(other->cursorOffset(), several.size());
}

// A table or a callout last in the note: the first letter on the empty line
// after it is drawn where it will be once the parse has landed.
TEST(EnterJitterTest, TheFirstLetterAfterATrailingTableOrCalloutIsDrawnWhereItStays)
{
    const QList<std::pair<const char *, QString>> notes = {
        {"table", QStringLiteral("para\n\n| a | b |\n|---|---|\n| 1 | 2 |\n")},
        {"callout", QStringLiteral("para\n\n> [!note] Title\n> body\n")},
    };
    for (const auto &[name, note] : notes) {
        SCOPED_TRACE(name);
        auto editor = editorWith(note);
        editor->show();
        editor->setCursorOffset(note.size());
        QTest::qWait(300);
        const int parsesBefore = editor->parseCountForTest();
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        EXPECT_EQ(editor->parseCountForTest(), parsesBefore + 1) << "the letter was drawn from the stale blocks";
        const QImage typed = withoutCaret(*editor);
        const qreal caret = editor->caretRectNow().y();
        QTest::qWait(300);
        EXPECT_NEAR(editor->caretRectNow().y(), caret, 0.5) << "the caret moved when the parse landed";
        EXPECT_EQ(diffRows(withoutCaret(*editor), typed).toStdString(), "") << "the text moved when the parse landed";
    }
}

// An insert that is the reader's own action in the focused editor - a slash
// command picked with the mouse, a drop - is parsed on the spot like a key.
TEST(EnterJitterTest, ASlashCommandAndADropAreTheReadersOwnEdit)
{
    const QString note = QStringLiteral("# Title\n\nparagraph one\n\n- a\n- b\n\nparagraph two\n");
    {
        const QString slashNote = QStringLiteral("# Title\n\nparagraph one\n\nsee \n\n- a\n- b\n\nparagraph two\n");
        auto editor = editorWith(slashNote);
        editor->show();
        MarkdownEditor *raw = editor.get();
        raw->setSlashCommands([] { return testSlashCommands(); },
                              [raw](const QString &) { raw->insertAtCaret(QStringLiteral("one\n\n- two\n")); });
        raw->setCursorOffset(slashNote.indexOf(QStringLiteral("see ")) + 4);
        QTest::keyClicks(raw, QStringLiteral("/bol"));
        ASSERT_TRUE(raw->slashPopupVisible());
        QCompleter *slash = nullptr;
        for (QCompleter *completer : raw->findChildren<QCompleter *>()) {
            if (completer->popup()->objectName() == QLatin1String("slashCommandPopup"))
                slash = completer;
        }
        ASSERT_NE(slash, nullptr);
        const int before = raw->parseCountForTest();
        emit slash->activated(raw->slashSuggestions().constFirst());
        EXPECT_GT(raw->noteText().indexOf(QStringLiteral("- two")), 0);
        EXPECT_EQ(raw->parseCountForTest(), before + 1) << "a picked slash command waited for the debounce";
    }
    {
        auto editor = editorWith(note);
        editor->show();
        QTest::qWait(300);
        QMimeData mime;
        mime.setText(QStringLiteral("dropped\n\n- item\n"));
        const qsizetype at = note.indexOf(QStringLiteral("paragraph two"));
        editor->setCursorOffset(at);
        const QPoint point(300, 100);
        const int before = editor->parseCountForTest();
        QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor->viewport(), &enter);
        ASSERT_TRUE(enter.isAccepted());
        QDropEvent drop(QPointF(point), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor->viewport(), &drop);
        ASSERT_TRUE(drop.isAccepted());
        EXPECT_EQ(editor->parseCountForTest(), before + 1) << "a drop waited for the debounce";
    }
}

TEST(CodeHighlighterTest, KnowsKateDefinitionsAndObsidianAliases)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";

    CodeHighlighter highlighter;

    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("cpp")), QStringLiteral("C++"));
    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("py")), QStringLiteral("Python"));
    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("sh")), QStringLiteral("Bash"));
    // An unknown id passes through rather than being mangled.
    EXPECT_EQ(CodeHighlighter::kateNameFor(QStringLiteral("Rust")), QStringLiteral("Rust"));

    EXPECT_TRUE(highlighter.knows(QStringLiteral("cpp")));
    EXPECT_TRUE(highlighter.knows(QStringLiteral("python")));
    EXPECT_FALSE(highlighter.knows(QStringLiteral("not-a-language")));
    EXPECT_FALSE(highlighter.knows(QString()));
}

TEST(CodeHighlighterTest, ColoursTokensAndLeavesUnknownLanguagesAlone)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";

    CodeHighlighter highlighter;
    const QString code = QStringLiteral("int x = 42; // comment\nreturn x;\n");

    const auto formats = highlighter.highlight(code, QStringLiteral("cpp"));
    ASSERT_FALSE(formats.isEmpty());
    for (const QTextLayout::FormatRange &range : formats) {
        EXPECT_GE(range.start, 0);
        EXPECT_GT(range.length, 0);
        EXPECT_LE(range.start + range.length, code.size());
    }

    // The keyword on the second line must be found too, so the per-line
    // offset bookkeeping is right.
    const int secondLine = int(code.indexOf(QStringLiteral("return")));
    bool sawReturn = false;
    for (const QTextLayout::FormatRange &range : formats)
        sawReturn = sawReturn || range.start == secondLine;
    EXPECT_TRUE(sawReturn);

    EXPECT_TRUE(highlighter.highlight(code, QStringLiteral("not-a-language")).isEmpty());
    EXPECT_TRUE(highlighter.highlight(QString(), QStringLiteral("cpp")).isEmpty());
}

// --- auto-pairs ----------------------------------------------------------

TEST(AutoPairTest, InsertsTheClosingHalfAndSitsBetweenThem)
{
    for (const auto &pair : {std::pair{QStringLiteral("("), QStringLiteral("()")},
                             std::pair{QStringLiteral("["), QStringLiteral("[]")},
                             std::pair{QStringLiteral("{"), QStringLiteral("{}")},
                             std::pair{QStringLiteral("\""), QStringLiteral("\"\"")},
                             std::pair{QStringLiteral("'"), QStringLiteral("''")}}) {
        // On an empty line: a quote is its own closer, so it deliberately
        // refuses to open straight after a word (see the test below).
        auto editor = editorWith(QStringLiteral("\n"));
        editor->setCursorOffset(0);
        QTest::keyClicks(editor.get(), pair.first);
        EXPECT_EQ(editor->noteText(), pair.second + QStringLiteral("\n"));
        EXPECT_EQ(editor->cursorOffset(), 1) << "caret belongs between " << qPrintable(pair.second);
    }
}

TEST(AutoPairTest, TypingTheCloserStepsOverIt)
{
    auto editor = editorWith(QStringLiteral("x\n"));
    editor->setCursorOffset(1);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    ASSERT_EQ(editor->noteText(), QStringLiteral("x()\n"));

    QTest::keyClicks(editor.get(), QStringLiteral(")"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("x()\n")); // no second bracket
    EXPECT_EQ(editor->cursorOffset(), 3);
}

TEST(AutoPairTest, BackspaceBetweenAnEmptyPairTakesBoth)
{
    auto editor = editorWith(QStringLiteral("x\n"));
    editor->setCursorOffset(1);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    ASSERT_EQ(editor->noteText(), QStringLiteral("x()\n"));

    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    EXPECT_EQ(editor->noteText(), QStringLiteral("x\n"));
    EXPECT_EQ(editor->cursorOffset(), 1);
}

TEST(AutoPairTest, DoesNotOpenAQuoteInsideAWord)
{
    auto editor = editorWith(QStringLiteral("don\n"));
    editor->setCursorOffset(3);
    QTest::keyClicks(editor.get(), QStringLiteral("'"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("don'\n"));
}

TEST(AutoPairTest, DoesNotCloseInFrontOfAWord)
{
    // CodeMirror's rule, which is the one Obsidian runs: "(" before a word
    // stays a lone bracket.
    auto editor = editorWith(QStringLiteral("word\n"));
    editor->setCursorOffset(0);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    EXPECT_EQ(editor->noteText(), QStringLiteral("(word\n"));
}

TEST(AutoPairTest, WrapsASelectionAndKeepsIt)
{
    auto editor = editorWith(QStringLiteral("word"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);

    QTest::keyClicks(editor.get(), QStringLiteral("*"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("*word*"));

    // The selection survives, so a second press reaches Obsidian's "**".
    QTest::keyClicks(editor.get(), QStringLiteral("*"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("**word**"));
}

TEST(AutoPairTest, WrapsWithTheMarkdownPairsToo)
{
    for (const auto &marker : {QStringLiteral("="), QStringLiteral("~"),
                               QStringLiteral("_"), QStringLiteral("`")}) {
        auto editor = editorWith(QStringLiteral("word"));
        QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(editor.get(), marker);
        QTest::keyClicks(editor.get(), marker);
        EXPECT_EQ(editor->noteText(), marker + marker + QStringLiteral("word") + marker + marker);
    }
}

TEST(AutoPairTest, CanBeTurnedOff)
{
    auto editor = editorWith(QStringLiteral("x\n"));
    editor->setAutoPairs(false);
    editor->setCursorOffset(1);
    QTest::keyClicks(editor.get(), QStringLiteral("("));
    EXPECT_EQ(editor->noteText(), QStringLiteral("x(\n"));
    EXPECT_TRUE(editorWith(QString())->autoPairs()) << "on by default, as Obsidian ships it";
}

// --- list continuation and renumbering ------------------------------------

TEST(ListEditTest, EnterContinuesTheBullet)
{
    auto editor = editorWith(QStringLiteral("- one\n"));
    editor->setCursorOffset(5);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- one\n- \n"));
    EXPECT_EQ(editor->cursorOffset(), 8);
}

TEST(ListEditTest, EnterCarriesAnUntickedCheckbox)
{
    auto editor = editorWith(QStringLiteral("- [x] done\n"));
    editor->setCursorOffset(10);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- [x] done\n- [ ] \n"));
}

TEST(ListEditTest, EnterIncrementsAndRenumbersTheRest)
{
    auto editor = editorWith(QStringLiteral("1. a\n2. b\n3. c\n"));
    editor->setCursorOffset(9); // end of "2. b"
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("1. a\n2. b\n3. \n4. c\n"));
    EXPECT_EQ(editor->cursorOffset(), 13);
}

TEST(ListEditTest, EnterOnAnEmptyItemEndsTheList)
{
    auto editor = editorWith(QStringLiteral("- a\n- \n"));
    editor->setCursorOffset(6);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- a\n\n"));
    EXPECT_EQ(editor->cursorOffset(), 4);
}

TEST(ListEditTest, EnterOnAnEmptyNestedItemOutdentsFirst)
{
    auto editor = editorWith(QStringLiteral("- a\n\t- \n"));
    editor->setCursorOffset(7);
    QTest::keyClick(editor.get(), Qt::Key_Return);

    EXPECT_EQ(editor->noteText(), QStringLiteral("- a\n- \n"));
    EXPECT_EQ(editor->cursorOffset(), 6);
}

TEST(ListEditTest, TabIndentsAndRenumbersBothLevels)
{
    auto editor = editorWith(QStringLiteral("1. a\n2. b\n3. c\n"));
    editor->setCursorOffset(5); // start of "2. b"
    QTest::keyClick(editor.get(), Qt::Key_Tab);

    EXPECT_EQ(editor->noteText(), QStringLiteral("1. a\n\t1. b\n2. c\n"));
    EXPECT_EQ(editor->cursorOffset(), 9);
}

TEST(ListEditTest, ShiftTabOutdentsAndRenumbers)
{
    auto editor = editorWith(QStringLiteral("1. a\n\t1. b\n2. c\n"));
    editor->setCursorOffset(9);
    QTest::keyClick(editor.get(), Qt::Key_Backtab, Qt::ShiftModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("1. a\n2. b\n3. c\n"));
    EXPECT_EQ(editor->cursorOffset(), 8);
}

TEST(ListEditTest, TabOutsideAListStillIndentsTheText)
{
    auto editor = editorWith(QStringLiteral("plain\n"));
    editor->setCursorOffset(5);
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_EQ(editor->noteText(), QStringLiteral("plain    \n"));
}

TEST(ListEditTest, ContinuationIsOneUndoStep)
{
    const QString source = QStringLiteral("1. a\n2. b\n3. c\n");
    auto editor = editorWith(source);
    editor->setCursorOffset(9);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    ASSERT_NE(editor->noteText(), source);

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), source);
}

// --- checkboxes -----------------------------------------------------------

TEST(CheckboxTest, CtrlEnterTogglesWithoutMovingTheCaret)
{
    auto editor = editorWith(QStringLiteral("- [ ] task\n"));
    editor->setCursorOffset(8);

    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [x] task\n"));
    EXPECT_EQ(editor->cursorOffset(), 8);

    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [ ] task\n"));
    EXPECT_EQ(editor->cursorOffset(), 8);
}

TEST(CheckboxTest, CtrlEnterMakesATaskOutOfALineThatIsNotOne)
{
    auto editor = editorWith(QStringLiteral("- item\n"));
    editor->setCursorOffset(6);
    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [ ] item\n"));

    auto plain = editorWith(QStringLiteral("hello\n"));
    plain->setCursorOffset(3);
    QTest::keyClick(plain.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(plain->noteText(), QStringLiteral("- [ ] hello\n"));
    EXPECT_EQ(plain->cursorOffset(), 9); // still on the same character
}

namespace {

/// Where the painter puts the checkbox of a task on the note's first line:
/// the content column is centred, the list margin is 26 px, the box is 0.8 of
/// the line's font height and sits 8 px left of the text, and the first block
/// starts 28 px down. Same numbers as `paintListMarkers` and `checkboxRect`.
QPoint firstCheckboxPoint(const MarkdownEditor &editor)
{
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const qreal fontHeight = QFontMetricsF(Theme::builtin().textFont()).height();
    return QPoint(left + int(26 - 8 - fontHeight * 0.4), int(28 + fontHeight / 2));
}

} // namespace

TEST(CheckboxTest, ClickingTheRenderedBoxToggles)
{
    auto editor = editorWith(QStringLiteral("- [ ] task\n"));
    const QPoint point = firstCheckboxPoint(*editor);

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [x] task\n"))
        << "clicked at " << point.x() << "," << point.y();
}

// --- checkbox states ------------------------------------------------------

namespace {

QAction *menuAction(const QMenu &menu, const QString &name); // below, with the other menu tests

/// An editor that reads any one character in a task's box as a state.
std::unique_ptr<MarkdownEditor> statesEditorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    ParseOptions options = editor->parseOptions();
    options.taskStates = true;
    editor->setParseOptions(options);
    editor->setNoteText(text);
    return editor;
}

} // namespace

TEST(CheckboxTest, AClickWritesTheStatesNext)
{
    const std::pair<const char *, const char *> cases[] = {
        {"- [-] t\n", "- [ ] t\n"}, {"- [/] t\n", "- [x] t\n"}, {"- [ ] t\n", "- [x] t\n"},
        {"- [~] t\n", "- [ ] t\n"}, {"- [q] t\n", "- [x] t\n"}, {"- [x] t\n", "- [ ] t\n"}};
    for (const auto &[before, after] : cases) {
        auto editor = statesEditorWith(QString::fromLatin1(before));
        const qsizetype caret = editor->cursorOffset();
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, firstCheckboxPoint(*editor));
        EXPECT_EQ(editor->noteText(), QString::fromLatin1(after)) << before;
        EXPECT_EQ(editor->cursorOffset(), caret) << before;
    }
}

TEST(CheckboxTest, CtrlEnterCyclesTheSameWayAndIsUnchangedWhenOff)
{
    auto editor = statesEditorWith(QStringLiteral("- [/] b\n"));
    editor->setCursorOffset(6);
    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [x] b\n"));
    EXPECT_EQ(editor->cursorOffset(), 6);

    auto cancelled = statesEditorWith(QStringLiteral("- [-] a\n"));
    cancelled->setCursorOffset(7);
    QTest::keyClick(cancelled.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(cancelled->noteText(), QStringLiteral("- [ ] a\n"));

    // Off, `[/]` is only text, so the line is not a task: today's bytes.
    auto off = editorWith(QStringLiteral("- [/] b\n"));
    off->setCursorOffset(6);
    QTest::keyClick(off.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(off->noteText(), QStringLiteral("- [ ] [/] b\n"));
}

TEST(CheckboxTest, AnAstralCodePointInTheBoxIsNotAStateSoCtrlEnterKeepsTheText)
{
    // Two UTF-16 units: read as a state, the box was 4 units wide and the edit 3 long.
    const QString box = QStringLiteral("[") + QString::fromUcs4(U"\U0001F600") + QStringLiteral("]");
    auto editor = statesEditorWith(QStringLiteral("- ") + box + QStringLiteral(" t\n"));
    editor->setCursorOffset(2 + box.size() + 2);
    QTest::keyClick(editor.get(), Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [ ] ") + box + QStringLiteral(" t\n"));
}

TEST(CheckboxTest, CancelledIsStruckAndMutedInProgressIsNot)
{
    const QString note = QStringLiteral("- [-] aa\n- [~] bb\n- [x] cc\n- [/] dd\n- [?] ee\n");
    auto editor = statesEditorWith(note);
    (void)editor->grab(); // formatAt wants a laid-out block
    const QColor muted = Theme::builtin().color(QStringLiteral("--text-muted"));
    for (const char *text : {"aa", "bb", "cc"}) {
        const QTextCharFormat format = editor->formatAt(note.indexOf(QLatin1String(text)));
        EXPECT_TRUE(format.fontStrikeOut()) << text;
        EXPECT_EQ(format.foreground().color(), muted) << text;
    }
    for (const char *text : {"dd", "ee"}) {
        const QTextCharFormat format = editor->formatAt(note.indexOf(QLatin1String(text)));
        EXPECT_FALSE(format.fontStrikeOut()) << text;
        EXPECT_NE(format.foreground().color(), muted) << text;
    }
}

TEST(CheckboxTest, TheContextMenuOffersEveryState)
{
    auto editor = statesEditorWith(QStringLiteral("- [/] a\n\npara\n"));
    auto menu = editor->contextMenuAt(6);
    QAction *states = menuAction(*menu, QStringLiteral("Checkbox state"));
    ASSERT_TRUE(states);
    const QList<TaskState> &all = TaskStates::current()->all();
    ASSERT_EQ(states->menu()->actions().size(), all.size());
    for (QAction *action : states->menu()->actions())
        EXPECT_EQ(action->isChecked(), action->text().startsWith(QLatin1String("In-progress"))) << action->text().toStdString();
    menuAction(*states->menu(), QStringLiteral("Cancelled"))->trigger();
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [-] a\n\npara\n"));

    // A plain bullet gets a box; a paragraph has no submenu.
    menu.reset();
    editor = statesEditorWith(QStringLiteral("- a\n\npara\n"));
    menu = editor->contextMenuAt(3);
    menuAction(*menuAction(*menu, QStringLiteral("Checkbox state"))->menu(), QStringLiteral("Dropped"))->trigger();
    EXPECT_EQ(editor->noteText(), QStringLiteral("- [~] a\n\npara\n"));
    menu.reset();
    menu = editor->contextMenuAt(editor->noteText().indexOf(QLatin1String("para")) + 1);
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Checkbox state")));

    // Off, the menu is the one it always was.
    menu.reset();
    auto off = editorWith(QStringLiteral("- [ ] a\n"));
    menu = off->contextMenuAt(6);
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Checkbox state")));
    menu.reset(); // before `off`, whose child it is
}

TEST(CheckboxTest, GlyphsPaintInTheirBoxes)
{
    const auto centreOf = [](const char *box, qreal across = 0) {
        auto editor = statesEditorWith(QStringLiteral("- ") + QString::fromLatin1(box) + QStringLiteral(" t\n"));
        const QImage shot = editor->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
        const qreal boxSide = QFontMetricsF(Theme::builtin().textFont()).height() * 0.8;
        const QPoint at = firstCheckboxPoint(*editor) + QPoint(int(across * boxSide), 0);
        return std::pair<QColor, QColor>(shot.pixelColor(at), shot.pixelColor(2, 2));
    };
    const auto [todo, background] = centreOf("[ ]");
    EXPECT_EQ(todo, background);
    const auto [cross, crossBackground] = centreOf("[~]");
    EXPECT_GT(cross.red(), cross.green() + 40);
    EXPECT_GT(cross.red(), cross.blue() + 40);
    const auto [line, lineBackground] = centreOf("[-]");
    EXPECT_NE(line, lineBackground);
    // The half box is filled on its left and open on its right.
    const auto [left, leftBackground] = centreOf("[/]", -0.25);
    const auto [right, rightBackground] = centreOf("[/]", 0.25);
    EXPECT_NE(left, leftBackground);
    EXPECT_EQ(right, rightBackground);
}

// --- moving and duplicating lines ----------------------------------------

TEST(LineMoveTest, AltUpAndAltDownSwapTheLine)
{
    auto editor = editorWith(QStringLiteral("a\nb\nc\n"));
    editor->setCursorOffset(2); // on "b"

    QTest::keyClick(editor.get(), Qt::Key_Up, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("b\na\nc\n"));
    EXPECT_EQ(editor->cursorOffset(), 0);

    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a\nb\nc\n"));
    EXPECT_EQ(editor->cursorOffset(), 2);
}

TEST(LineMoveTest, MovingStopsAtTheEdges)
{
    auto editor = editorWith(QStringLiteral("a\nb"));
    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_Up, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a\nb"));

    editor->setCursorOffset(3);
    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a\nb"));
}

TEST(LineMoveTest, MovingTakesEveryLineTheSelectionTouches)
{
    auto editor = editorWith(QStringLiteral("a\nb\nc\n"));
    editor->setCursorOffset(0);
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier); // "a\nb"

    QTest::keyClick(editor.get(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("c\na\nb\n"));
}

TEST(LineMoveTest, CtrlShiftDDuplicatesAndFollowsTheCopy)
{
    auto editor = editorWith(QStringLiteral("a\nb\n"));
    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("a\na\nb\n"));
    EXPECT_EQ(editor->cursorOffset(), 2);
}

// --- formatting commands --------------------------------------------------

TEST(FormattingTest, CtrlBWrapsThenUnwraps)
{
    auto editor = editorWith(QStringLiteral("hello"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);

    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("**hello**"));

    // The selection stayed on the words, so the same key takes the markers off.
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("hello"));
}

TEST(FormattingTest, ItalicAndHighlightUseTheirOwnMarkers)
{
    auto italic = editorWith(QStringLiteral("hello"));
    QTest::keyClick(italic.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(italic.get(), Qt::Key_I, Qt::ControlModifier);
    EXPECT_EQ(italic->noteText(), QStringLiteral("*hello*"));

    auto highlight = editorWith(QStringLiteral("hello"));
    QTest::keyClick(highlight.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(highlight.get(), Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(highlight->noteText(), QStringLiteral("==hello=="));
}

TEST(FormattingTest, WithNoSelectionThePairOpensRoundTheCaret)
{
    auto editor = editorWith(QStringLiteral("x"));
    editor->setCursorOffset(1);
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("x****"));
    EXPECT_EQ(editor->cursorOffset(), 3);
}

TEST(FormattingTest, CtrlKPutsTheCaretInTheSlotThatIsStillEmpty)
{
    auto url = editorWith(QStringLiteral("https://example.com"));
    QTest::keyClick(url.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(url.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(url->noteText(), QStringLiteral("[](https://example.com)"));
    EXPECT_EQ(url->cursorOffset(), 1) << "a known URL leaves the text slot to fill in";

    // Selected text that is not a URL becomes the target. Which form that
    // takes is links.useWikilinks, which defaults to on as it does in
    // Obsidian, so the wikilink is the default answer here.
    auto wiki = editorWith(QStringLiteral("click me"));
    QTest::keyClick(wiki.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(wiki.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(wiki->noteText(), QStringLiteral("[[click me]]"));
    EXPECT_EQ(wiki->cursorOffset(), 10) << "a wikilink has no second slot to fill";

    auto text = editorWith(QStringLiteral("click me"));
    text->setUseWikilinks(false);
    QTest::keyClick(text.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(text.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(text->noteText(), QStringLiteral("[click me]()"));
    EXPECT_EQ(text->cursorOffset(), 11);

    // An empty selection opens whichever form the vault prefers.
    auto empty = editorWith(QString());
    QTest::keyClick(empty.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(empty->noteText(), QStringLiteral("[[]]"));
    EXPECT_EQ(empty->cursorOffset(), 2);
}

TEST(EditorSettingsTest, TabWidthStrictBreaksAndGuidesAllApply)
{
    // Each of these was shown on the settings page and stored, with nothing
    // reading it. The point of the test is that they now reach the editor.
    auto editor = editorWith(QStringLiteral("one\ntwo\n\nthree\n"));

    // Off, a single newline inside a paragraph joins, which is Obsidian's
    // default. On, CommonMark's rule applies and it breaks.
    EXPECT_FALSE(editor->strictLineBreaks());
    editor->setStrictLineBreaks(true);
    EXPECT_TRUE(editor->strictLineBreaks());
    editor->setStrictLineBreaks(false);

    editor->setTabWidth(2);
    EXPECT_EQ(editor->tabWidth(), 2);
    editor->setCursorOffset(0);
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("  one")))
        << "Tab inserts tabWidth spaces, not a hardcoded four";

    // Out-of-range values are clamped rather than taken literally.
    editor->setTabWidth(99);
    EXPECT_EQ(editor->tabWidth(), 8);
    editor->setTabWidth(0);
    EXPECT_EQ(editor->tabWidth(), 1);

    EXPECT_FALSE(editor->indentGuides());
    editor->setIndentGuides(true);
    EXPECT_TRUE(editor->indentGuides());
}

TEST(FormattingTest, CtrlKOnALinkTakesItApartAgain)
{
    auto editor = editorWith(QStringLiteral("[shown](https://example.com)"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(editor.get(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("shown"));
}

// --- find and replace -----------------------------------------------------

TEST(FindTest, CountsMatchesAndStepsRoundThem)
{
    auto editor = editorWith(QStringLiteral("alpha beta alpha gamma alpha\n"));
    QTest::keyClick(editor.get(), Qt::Key_F, Qt::ControlModifier);
    ASSERT_TRUE(editor->findVisible());

    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    EXPECT_EQ(editor->findMatchCount(), 3);
    EXPECT_EQ(editor->findCurrentIndex(), 0);

    editor->findStep(1);
    EXPECT_EQ(editor->findCurrentIndex(), 1);
    EXPECT_EQ(editor->cursorOffset(), 16); // the match is selected

    editor->findStep(1);
    editor->findStep(1);
    EXPECT_EQ(editor->findCurrentIndex(), 0) << "next wraps round, as Obsidian's bar does";

    editor->findStep(-1);
    EXPECT_EQ(editor->findCurrentIndex(), 2);
}

TEST(FindTest, HonoursCaseAndRegex)
{
    auto editor = editorWith(QStringLiteral("Alpha alpha\n"));

    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    EXPECT_EQ(editor->findMatchCount(), 2);

    editor->setFindQuery(QStringLiteral("alpha"), true, false);
    EXPECT_EQ(editor->findMatchCount(), 1);

    editor->setFindQuery(QStringLiteral("a\\w+a"), false, true);
    EXPECT_EQ(editor->findMatchCount(), 2);
    EXPECT_TRUE(editor->findPatternValid());
}

TEST(FindTest, AnUnparseablePatternReportsItselfRatherThanThrowing)
{
    auto editor = editorWith(QStringLiteral("alpha\n"));
    editor->setFindQuery(QStringLiteral("([unclosed"), false, true);

    EXPECT_FALSE(editor->findPatternValid());
    EXPECT_EQ(editor->findMatchCount(), 0);

    editor->setFindQuery(QStringLiteral("alpha"), false, true);
    EXPECT_TRUE(editor->findPatternValid());
    EXPECT_EQ(editor->findMatchCount(), 1);
}

TEST(FindTest, ReplacesOneAndThenAll)
{
    auto one = editorWith(QStringLiteral("alpha beta alpha\n"));
    one->setFindQuery(QStringLiteral("alpha"), false, false);
    one->replaceCurrent(QStringLiteral("X"));
    EXPECT_EQ(one->noteText(), QStringLiteral("X beta alpha\n"));
    EXPECT_EQ(one->findMatchCount(), 1);

    auto every = editorWith(QStringLiteral("alpha beta alpha\n"));
    every->setFindQuery(QStringLiteral("alpha"), false, false);
    EXPECT_EQ(every->replaceAll(QStringLiteral("Y")), 2);
    EXPECT_EQ(every->noteText(), QStringLiteral("Y beta Y\n"));
    EXPECT_EQ(every->findMatchCount(), 0);
}

TEST(FindTest, ReplaceAllIsOneUndoStep)
{
    const QString source = QStringLiteral("alpha beta alpha\n");
    auto editor = editorWith(source);
    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    editor->replaceAll(QStringLiteral("Y"));
    ASSERT_NE(editor->noteText(), source);

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), source);
}

TEST(FindTest, CtrlHOpensTheReplaceRowAndEscapeCloses)
{
    auto editor = editorWith(QStringLiteral("alpha\n"));
    EXPECT_FALSE(editor->findVisible());

    QTest::keyClick(editor.get(), Qt::Key_H, Qt::ControlModifier);
    EXPECT_TRUE(editor->findVisible());

    QTest::keyClick(editor.get(), Qt::Key_Escape);
    EXPECT_FALSE(editor->findVisible());
    EXPECT_EQ(editor->findMatchCount(), 0);
}

TEST(FindTest, MatchesFollowTheTextAsItIsEdited)
{
    auto editor = editorWith(QStringLiteral("alpha\n"));
    editor->showFind(false);
    editor->setFindQuery(QStringLiteral("alpha"), false, false);
    ASSERT_EQ(editor->findMatchCount(), 1);

    editor->setCursorOffset(5);
    QTest::keyClicks(editor.get(), QStringLiteral(" alpha"));
    EXPECT_EQ(editor->findMatchCount(), 2);
}

// Searching a long note must not cost a full relayout: the current match is
// brought on screen through the same viewport-only path scrolling uses.
TEST(FindTest, SteppingThroughALongNoteStaysInsideTheKeystrokeBudget)
{
    const QString note = generateNote(200);
    auto editor = editorWith(note);
    editor->showFind(false);
    editor->setFindQuery(QStringLiteral("Wikilink"), false, false);
    ASSERT_GT(editor->findMatchCount(), 100);

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    constexpr int kSteps = 20;
    const double perStep = perf::bestOf([&] {
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < kSteps; ++i)
            editor->findStep(1);
        return double(clock.nsecsElapsed()) / kSteps / 1e6;
    }, 3, 8.0);

    std::printf("[ PERF     ] find step on a %.2f kB note: %.2f ms\n",
                note.size() / 1024.0, perStep);
    EXPECT_LT(perStep, 8.0);
}

// --- block spacing --------------------------------------------

TEST(BlockSpacingTest, SourceModeGivesEveryLineTheSameHeight)
{
    // Three lines of one paragraph and three lines with a blank in the middle
    // are both three lines of the file, so they are the same height on screen.
    auto paragraph = editorWith(QStringLiteral("alpha\nbeta\ngamma\n"));
    paragraph->setMode(MarkdownEditor::Mode::Source);
    auto blank = editorWith(QStringLiteral("alpha\n\ngamma\n"));
    blank->setMode(MarkdownEditor::Mode::Source);

    EXPECT_DOUBLE_EQ(paragraph->documentHeight(), blank->documentHeight());
}

TEST(BlockSpacingTest, OnlyLivePreviewPutsAirOverAHeading)
{
    // Obsidian's live preview gives a heading one --p-spacing of air and its
    // source mode none at all.
    const QString note = QStringLiteral("# Heading\n\nbody\n");
    auto live = editorWith(note);
    auto source = editorWith(note);
    source->setMode(MarkdownEditor::Mode::Source);

    const qreal spacing = Theme::builtin().textFont().pixelSize();
    EXPECT_DOUBLE_EQ(live->documentHeight() - source->documentHeight(), spacing);
}

TEST(BlockSpacingTest, ALineIsTheFontSizeTimesItsLineHeight)
{
    // A CSS line-height is a multiple of the font size, not of the font's own
    // metric height: 16 px text at 1.5 is 24 px, whatever the family measures.
    auto editor = editorWith(QStringLiteral("one\n"));
    editor->setMode(MarkdownEditor::Mode::Source);
    const qreal padding = editor->documentHeight()
                          - Theme::builtin().textFont().pixelSize() * 1.5;

    auto three = editorWith(QStringLiteral("one\ntwo\nthree\n"));
    three->setMode(MarkdownEditor::Mode::Source);
    EXPECT_DOUBLE_EQ(three->documentHeight() - padding,
                     Theme::builtin().textFont().pixelSize() * 1.5 * 3);
}

// --- the properties table -------------------------------------

namespace {

/// Where the properties table puts the control of its first row: the key
/// column is 180 px, the first block starts 28 px down, and a row is the text
/// line height plus a fifth. Same numbers as `layoutProperties`.
QPoint firstPropertyControlPoint(const MarkdownEditor &editor)
{
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const qreal rowHeight = Theme::builtin().textFont().pixelSize() * 1.5 * 1.2;
    return QPoint(left + 180 + 6, int(28 + rowHeight / 2));
}

} // namespace

TEST(PropertiesTest, FrontmatterIsATableInLivePreviewAndRawInSourceMode)
{
    const QString note = QStringLiteral("---\ndone: false\ntags:\n  - one\n---\n\nbody\n");
    auto editor = editorWith(note);

    ASSERT_EQ(editor->properties().size(), 2);
    EXPECT_EQ(editor->properties().at(0).key, QStringLiteral("done"));
    EXPECT_EQ(editor->properties().at(0).type, Property::Type::Checkbox);
    EXPECT_EQ(editor->properties().at(1).type, Property::Type::List);
    // The YAML itself is never shown in live preview - the table replaces it.
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("done:")));
    // The table is taller than the nothing that used to be drawn there.
    EXPECT_GT(editor->documentHeight(), editorWith(QStringLiteral("\nbody\n"))->documentHeight());

    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), note);
}

TEST(PropertiesTest, ClickingACheckboxRewritesOnlyThatValue)
{
    auto editor = editorWith(
        QStringLiteral("---\ndone: false\ntitle: Keep me\n---\n\nbody\n"));
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      firstPropertyControlPoint(*editor));

    // Every other byte of the file is exactly as it was: non-negotiable 1.
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("---\ndone: true\ntitle: Keep me\n---\n\nbody\n"));

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      firstPropertyControlPoint(*editor));
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("---\ndone: false\ntitle: Keep me\n---\n\nbody\n"));
}

TEST(PropertiesTest, AQuotedValueIsReadOnly)
{
    // `"false"` is text, not a checkbox: the typed editor cannot round trip it,
    // so clicking where its control would be changes nothing.
    const QString note = QStringLiteral("---\ndone: \"false\"\n---\n\nbody\n");
    auto editor = editorWith(note);
    ASSERT_EQ(editor->properties().at(0).type, Property::Type::Text);

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      firstPropertyControlPoint(*editor));
    EXPECT_EQ(editor->noteText(), note);
}

TEST(PropertiesTest, ABlockScalarIsShownReadOnlyAndKeptAsWritten)
{
    const QString note = QStringLiteral("---\ndone: false\nimplicit-python: |\n  import math\n  import os\n---\n\nbody\n");
    auto editor = editorWith(note);
    ASSERT_EQ(editor->properties().size(), 2);
    EXPECT_EQ(editor->properties().at(1).key, QStringLiteral("implicit-python"));
    EXPECT_EQ(editor->properties().at(1).values.value(0), QStringLiteral("import math\nimport os"));

    // Clicking where the value is opens no editor and changes nothing.
    const QPoint first = firstPropertyControlPoint(*editor);
    const int rowHeight = int(Theme::builtin().textFont().pixelSize() * 1.5 * 1.2);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, first + QPoint(0, rowHeight));
    EXPECT_EQ(editor->noteText(), note);
    EXPECT_TRUE(editor->findChildren<QLineEdit *>().isEmpty());

    // Toggling the sibling checkbox leaves the block scalar byte for byte.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ndone: true\nimplicit-python: |\n  import math\n  import os\n---\n\nbody\n"));
}

TEST(PropertiesTest, TheTableFolds)
{
    auto editor = editorWith(QStringLiteral("---\na: 1\nb: 2\nc: 3\n---\n\nbody\n"));
    const qreal open = editor->documentHeight();

    editor->toggleFoldAt(0);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_LT(editor->documentHeight(), open);

    editor->toggleFoldAt(0);
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);
}

// --- folding callouts, containers and code blocks -------------

TEST(FoldTest, ADetailsContainerStartsFoldedAndShowsItsSummary)
{
    auto editor = editorWith(
        QStringLiteral(":::details summary=\"What it hides\"\nbody\n:::\n"));
    EXPECT_TRUE(editor->isFoldedAt(0));

    editor->toggleFoldAt(0);
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_GT(editor->documentHeight(), 0);
}

TEST(FoldTest, AMinusCalloutStartsFoldedAndAPlainOneNeverFolds)
{
    auto folded = editorWith(QStringLiteral("> [!warning]- Title\n> body\n"));
    EXPECT_TRUE(folded->isFoldedAt(0));
    const qreal collapsed = folded->documentHeight();
    folded->toggleFoldAt(0);
    EXPECT_GT(folded->documentHeight(), collapsed);

    auto plain = editorWith(QStringLiteral("> [!note] Title\n> body\n"));
    EXPECT_FALSE(plain->isFoldedAt(0));
    const qreal open = plain->documentHeight();
    plain->toggleFoldAt(0); // not foldable: the state is kept but changes nothing
    EXPECT_DOUBLE_EQ(plain->documentHeight(), open);
}

TEST(FoldTest, AnotherNoteInTheSameEditorStartsWithItsOwnFolds)
{
    const QString note = QStringLiteral("> [!warning]- Title\n> body\n");
    auto editor = editorWith(note);
    editor->toggleFoldAt(0);
    ASSERT_FALSE(editor->isFoldedAt(0));
    editor->setNoteText(note); // the next note's block at offset 0 is not the one that was opened
    EXPECT_TRUE(editor->isFoldedAt(0));
}

TEST(FoldTest, FoldStateRoundTripsToAnotherNoteAndFollowsItsLineNotItsOffset)
{
    const QString note = QStringLiteral("> [!warning]- Closed\n> body\n\n"
                                        "> [!note]+ Opened\n> body\n\n"
                                        "```python\n#| fold: true\nprint(1)\n```\n");
    auto editor = editorWith(note);
    const qsizetype opened = note.indexOf(QLatin1String("> [!note]+"));
    const qsizetype code = note.indexOf(QLatin1String("print(1)"));
    editor->toggleFoldAt(opened);  // folded by hand
    editor->toggleFoldAt(0);       // opened by hand, though the note asked for closed
    ASSERT_TRUE(editor->isFoldedAt(opened));
    ASSERT_FALSE(editor->isFoldedAt(0));
    const QStringList state = editor->foldState();

    // Another note in between, then the first again with a paragraph above: the
    // offsets all moved, the lines did not.
    editor->setNoteText(QStringLiteral("nothing here\n"));
    EXPECT_TRUE(editor->foldState().isEmpty());
    const QString edited = QStringLiteral("A new first paragraph.\n\n") + note;
    editor->setNoteText(edited);
    editor->setFoldState(state);
    const qsizetype shift = edited.size() - note.size();
    EXPECT_FALSE(editor->isFoldedAt(shift)) << "the callout opened by hand stays open";
    EXPECT_TRUE(editor->isFoldedAt(opened + shift));
    EXPECT_TRUE(editor->isFoldedAt(code + shift)) << "the fence keeps what its note said";
    EXPECT_EQ(editor->foldState(), state);
}

TEST(FoldTest, FoldStateEntriesWithNoMatchingBlockAreDropped)
{
    auto editor = editorWith(QStringLiteral("> [!note]+ Gone\n> body\n\n> [!note]+ Kept\n> body\n"));
    editor->toggleFoldAt(0);
    editor->toggleFoldAt(editor->noteText().indexOf(QLatin1String("> [!note]+ Kept")));
    const QStringList state = editor->foldState();
    ASSERT_EQ(state.size(), 2);

    const QString next = QStringLiteral("> [!note]+ Kept\n> body\n\nA paragraph.\n");
    editor->setNoteText(next);
    editor->setFoldState(state);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_EQ(editor->foldState().size(), 1);
}

TEST(FoldTest, TwoBlocksWithTheSameLineAreToldApartByOrder)
{
    const QString note = QStringLiteral("> [!note]+ Same\n> a\n\n> [!note]+ Same\n> b\n");
    auto editor = editorWith(note);
    const qsizetype second = note.indexOf(QLatin1String("> [!note]+ Same"), 1);
    editor->toggleFoldAt(second);
    const QStringList state = editor->foldState();
    editor->setNoteText(note);
    editor->setFoldState(state);
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->isFoldedAt(second));
}

TEST(FoldTest, FoldStateGivenToABigNoteWaitsForItsParse)
{
    QString note = QStringLiteral("> [!warning]+ Open\n> body\n\n");
    while (note.size() < 200 * 1024)
        note += QStringLiteral("filler paragraph of text to make the note big\n\n");
    auto editor = editorWith(QStringLiteral("> [!warning]+ Open\n> body\n"));
    editor->toggleFoldAt(0);
    const QStringList state = editor->foldState();
    ASSERT_EQ(state.size(), 1);

    editor->setNoteText(note); // parsed on the pool: no callout block yet
    editor->setFoldState(state);
    EXPECT_EQ(editor->foldState(), state) << "asked before the parse lands, it hands back what it was given";
    EXPECT_TRUE(QTest::qWaitFor([&] { return editor->isFoldedAt(0); }, 5000))
        << "folded once its block exists";
    EXPECT_EQ(editor->foldState(), state);
}

TEST(FoldTest, ClickingTheCalloutTitleToggles)
{
    auto editor = editorWith(QStringLiteral("> [!warning]+ Title\n> body\n"));
    ASSERT_FALSE(editor->isFoldedAt(0));

    const int viewport = editor->viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    // Off the title text, which edits it: the empty header row folds.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(left + column - 120, 34));
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_EQ(editor->cursorOffset(), 0) << "folding must not drag the caret with it";
    // So does the fold arrow itself.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(left + column - 9, 34));
    EXPECT_FALSE(editor->isFoldedAt(0));
}

// --- folding headings and list items ---------------------------------------

namespace {

const QString kSections = QStringLiteral("# A\n\nbody a\n\n## A1\n\nbody a1\n\n# B\n\nbody b\n");

/// Where the fold arrow of the line holding offset `at` is, in viewport
/// coordinates: the margin, level with the line. Moves the caret there.
QPoint arrowOf(MarkdownEditor &editor, qsizetype at)
{
    editor.enterBlockAt(at);
    const QRectF caret = editor.caretRectNow();
    return QPoint(int(caret.x() + 1) - 13, int(caret.center().y()));
}

} // namespace

TEST(FoldTest, AHeadingFoldsToTheNextHeadingOfItsLevelOrHigher)
{
    auto editor = editorWith(kSections);
    const qreal open = editor->documentHeight();
    ASSERT_TRUE(editor->displayText().contains(QLatin1String("body a1")));

    editor->toggleFoldAt(0);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_LT(editor->documentHeight(), open);
    const QString shown = editor->displayText();
    EXPECT_FALSE(shown.contains(QLatin1String("body a"))) << "its body and the A1 section go";
    EXPECT_FALSE(shown.contains(QLatin1String("A1")));
    EXPECT_TRUE(shown.contains(QLatin1String("body b"))) << "the next heading of its level stays";

    editor->toggleFoldAt(0);
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("body a1")));
}

TEST(FoldTest, ALowerHeadingFoldsOnlyItsOwnSection)
{
    auto editor = editorWith(kSections);
    editor->toggleFoldAt(kSections.indexOf(QLatin1String("## A1")));
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("body a\n")));
    EXPECT_FALSE(shown.contains(QLatin1String("body a1")));
    EXPECT_TRUE(shown.contains(QLatin1String("body b")));
}

TEST(FoldTest, AnOuterFoldHidesAnInnerFoldAndReopeningShowsItStillFolded)
{
    const QString note = QStringLiteral("# H1\n\n## H2\n\nbody\n\n# Next\n");
    auto editor = editorWith(note);
    const qsizetype inner = note.indexOf(QLatin1String("## H2"));
    editor->toggleFoldAt(inner);
    editor->toggleFoldAt(0);
    ASSERT_FALSE(editor->displayText().contains(QLatin1String("H2")));

    editor->toggleFoldAt(0);
    EXPECT_TRUE(editor->isFoldedAt(inner)) << "the inner fold is still there";
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("H2")));
    EXPECT_FALSE(shown.contains(QLatin1String("body")));
}

TEST(FoldTest, AListParentFoldsItsChildrenAndAChildlessItemCannot)
{
    const QString note = QStringLiteral("- parent\n  - child\n    - grandchild\n- leaf\n- other\n");
    auto editor = editorWith(note);
    const qreal open = editor->documentHeight();

    editor->toggleFoldAt(0);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_LT(editor->documentHeight(), open);
    const QString shown = editor->displayText();
    EXPECT_FALSE(shown.contains(QLatin1String("child")));
    EXPECT_TRUE(shown.contains(QLatin1String("leaf")) && shown.contains(QLatin1String("other")));

    editor->toggleFoldAt(0);
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);

    const qsizetype leaf = note.indexOf(QLatin1String("- leaf"));
    editor->toggleFoldAt(leaf);
    EXPECT_FALSE(editor->isFoldedAt(leaf));
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);
    EXPECT_TRUE(editor->foldState().isEmpty());
    // The grandchild alone folds too, and only what is under it.
    editor->toggleFoldAt(note.indexOf(QLatin1String("  - child")));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("grandchild")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("child")));
}

TEST(FoldTest, AFoldedListItemStaysFoldedWithTheCaretOnItsLineAndInSourceMode)
{
    const QString note = QStringLiteral("- parent\n  - child\n- other\n");
    auto editor = editorWith(note);
    editor->toggleFoldAt(0);
    editor->enterBlockAt(3); // the caret on the parent's own line
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("child")));

    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("child")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("- other")));
}

TEST(FoldTest, TheCaretMovingIntoHiddenTextUnfolds)
{
    auto editor = editorWith(kSections);
    editor->toggleFoldAt(0);
    editor->enterBlockAt(3); // the end of "# A"

    QTest::keyClick(editor.get(), Qt::Key_Down);
    EXPECT_GE(editor->cursorOffset(), kSections.indexOf(QLatin1String("# B")))
        << "Down skips the hidden section";
    EXPECT_TRUE(editor->isFoldedAt(0));

    editor->enterBlockAt(3);
    QTest::keyClick(editor.get(), Qt::Key_Right); // onto the first hidden line
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("body a1")));
}

TEST(FoldTest, AnOuterSectionOpensWhenTheCaretIsInsideAFoldedInnerOne)
{
    auto editor = editorWith(kSections);
    const qsizetype inner = kSections.indexOf(QLatin1String("## A1"));
    editor->toggleFoldAt(inner);
    editor->toggleFoldAt(0);
    editor->enterBlockAt(kSections.indexOf(QLatin1String("body a1")));
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_FALSE(editor->isFoldedAt(inner)) << "the caret is inside both";
}

TEST(FoldTest, EnterAtTheEndOfAFoldedHeadingUnfoldsIt)
{
    auto editor = editorWith(kSections);
    editor->toggleFoldAt(0);
    editor->enterBlockAt(3);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_TRUE(QTest::qWaitFor([&] { return !editor->isFoldedAt(0); }, 2000))
        << "the new line is inside the section, and the caret is on it";
    EXPECT_TRUE(editor->noteText().startsWith(QLatin1String("# A\n\n\nbody a")));
}

TEST(FoldTest, ARestoredNoteKeepsAFoldOverItsRestoredCaret)
{
    auto folded = editorWith(kSections);
    folded->toggleFoldAt(0);
    const QStringList state = folded->foldState();

    auto editor = editorWith(kSections);
    editor->setCursorOffset(kSections.indexOf(QLatin1String("body a1")));
    editor->setFoldState(state); // caret first, then folds: the order a restore must use
    EXPECT_TRUE(editor->isFoldedAt(0)) << "nothing has been touched yet";
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("body a1")));

    QTest::keyClick(editor.get(), Qt::Key_Right);
    EXPECT_FALSE(editor->isFoldedAt(0)) << "the first key opens it";
}

TEST(FoldTest, ClickingTheArrowOrTheEllipsisTogglesAndTheHeadingTextPlacesTheCaret)
{
    auto editor = editorWith(kSections);
    const QPoint arrow = arrowOf(*editor, 0);
    const qreal left = arrow.x() + 13;

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, arrow);
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_EQ(editor->cursorOffset(), 0) << "the arrow does not move the caret";

    // The ellipsis ends the folded line.
    editor->setCursorOffset(3);
    const QRectF end = editor->caretRectNow();
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(int(end.x()) + 6 + 12, arrow.y()));
    EXPECT_FALSE(editor->isFoldedAt(0));

    // The text itself is for the caret, folded or not.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, arrow);
    ASSERT_TRUE(editor->isFoldedAt(0));
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(int(left) + 12, arrow.y()));
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_GT(editor->cursorOffset(), 0);
}

TEST(FoldTest, FoldingASectionTheCaretIsInMovesTheCaretToItsHeading)
{
    auto editor = editorWith(kSections);
    editor->enterBlockAt(kSections.indexOf(QLatin1String("body a1")));
    editor->toggleFoldAt(0);
    EXPECT_TRUE(editor->isFoldedAt(0)) << "the caret did not reopen it";
    EXPECT_EQ(editor->cursorOffset(), 0);
}

TEST(FoldTest, ASelectionAcrossTheEllipsisCopiesTheHiddenText)
{
    auto editor = editorWith(kSections);
    editor->toggleFoldAt(0);
    const qsizetype next = kSections.indexOf(QLatin1String("# B"));
    editor->enterBlockAt(next);
    const int y = int(editor->caretRectNow().center().y());
    editor->enterBlockAt(0);

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::ShiftModifier, QPoint(120, y));
    EXPECT_GE(editor->cursorOffset(), next) << "the click lands on the next heading, not in the hidden blocks";
    EXPECT_TRUE(editor->selectedText().startsWith(kSections.left(next)));
    EXPECT_TRUE(editor->isFoldedAt(0)) << "a selection runs over a fold without opening it";
}

TEST(FoldTest, ClicksNeverLandInHiddenBlocks)
{
    auto editor = editorWith(kSections);
    editor->toggleFoldAt(0);
    editor->enterBlockAt(kSections.indexOf(QLatin1String("# B")));
    const QRectF caret = editor->caretRectNow();
    // Every row from the heading's to the next one's resolves to one of the two.
    for (int y = int(caret.top()) - 30; y < int(caret.bottom()); y += 3) {
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(200, y));
        const qsizetype at = editor->cursorOffset();
        EXPECT_TRUE(at <= 3 || at >= kSections.indexOf(QLatin1String("# B"))) << "y " << y << " -> " << at;
    }
}

TEST(FoldTest, FindRevealsAMatchInsideAFold)
{
    auto editor = editorWith(kSections);
    editor->toggleFoldAt(0);
    editor->showFind(false);
    editor->setFindQuery(QStringLiteral("body a1"), false, false);
    ASSERT_EQ(editor->findMatchCount(), 1);
    editor->findStep(1);
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("body a1")));
}

TEST(FoldTest, UndoAndAWholeTextExternalEditKeepFoldsByLine)
{
    const QString note = QStringLiteral("intro\n\n# A\n\nbody a\n\n# B\n");
    auto editor = editorWith(note);
    const qsizetype heading = note.indexOf(QLatin1String("# A"));
    editor->toggleFoldAt(heading);
    editor->enterBlockAt(0);
    QTest::keyClick(editor.get(), Qt::Key_X); // above the fold: its offset moves
    ASSERT_TRUE(editor->isFoldedAt(heading + 1));

    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    ASSERT_EQ(editor->noteText(), note);
    EXPECT_TRUE(editor->isFoldedAt(heading));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("body a")));

    const QString rewritten = QStringLiteral("a new paragraph\n\n") + note;
    editor->applyExternalEdit(0, note.size(), rewritten);
    EXPECT_TRUE(editor->isFoldedAt(heading + 17));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("body a")));
}

TEST(FoldTest, FoldStateRoundTripsHeadingsAndListItems)
{
    const QString note = QStringLiteral("# A\n\nbody\n\n- p\n  - c\n\n# B\n\nmore\n");
    auto editor = editorWith(note);
    const qsizetype list = note.indexOf(QLatin1String("- p"));
    const qsizetype second = note.indexOf(QLatin1String("# B"));
    editor->toggleFoldAt(0);
    editor->toggleFoldAt(list);
    editor->toggleFoldAt(second);
    editor->toggleFoldAt(second); // opened again: no entry for open
    const QStringList state = editor->foldState();
    ASSERT_EQ(state.size(), 2);

    editor->setNoteText(QStringLiteral("nothing here\n"));
    EXPECT_TRUE(editor->foldState().isEmpty());
    const QString edited = QStringLiteral("A new first paragraph.\n\n") + note;
    editor->setNoteText(edited);
    editor->setFoldState(state);
    const qsizetype shift = edited.size() - note.size();
    EXPECT_TRUE(editor->isFoldedAt(shift));
    EXPECT_TRUE(editor->isFoldedAt(list + shift));
    EXPECT_FALSE(editor->isFoldedAt(second + shift));
    EXPECT_EQ(editor->foldState(), state);
}

TEST(FoldTest, FoldingOffPerSettingDrawsNoArrowAndFoldsNothing)
{
    auto editor = editorWith(QStringLiteral("# A\n\nbody\n\n- p\n  - c\n"));
    editor->grab(); // the geometry settles on the first paint
    const qreal open = editor->documentHeight();
    editor->toggleFoldAt(0);
    ASSERT_LT(editor->documentHeight(), open);
    const QPoint arrow = arrowOf(*editor, 0);
    const auto arrowDrawn = [&] {
        const QImage image = editor->grab().toImage();
        const QRgb background = image.pixel(2, arrow.y());
        for (int y = arrow.y() - 6; y <= arrow.y() + 6; ++y) {
            for (int x = arrow.x() - 6; x <= arrow.x() + 6; ++x) {
                if (image.pixel(x, y) != background)
                    return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(arrowDrawn());

    editor->setFolding(false, true);
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open) << "a heading does not fold with it off";
    EXPECT_FALSE(arrowDrawn());
    editor->toggleFoldAt(0);
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);
    editor->toggleFoldAt(editor->noteText().indexOf(QLatin1String("- p"))); // lists still do
    EXPECT_LT(editor->documentHeight(), open);

    editor->setFolding(false, false);
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);
    editor->setFolding(true, true);
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("- c")))
        << "the remembered list fold is back with its setting";
}

// --- the fold commands ---------------------------------------------------

TEST(FoldTest, FoldCommandsFoldMoreFoldLessAndToggleAtTheCaret)
{
    for (const QString &id : {QStringLiteral("editor:toggle-fold"), QStringLiteral("editor:fold-all"),
                              QStringLiteral("editor:unfold-all"), QStringLiteral("editor:fold-more"),
                              QStringLiteral("editor:fold-less"), QStringLiteral("editor:toggle-fold-properties")}) {
        const auto &all = MarkdownEditor::commands();
        const auto found = std::find_if(all.cbegin(), all.cend(), [&id](const auto &c) { return c.id == id; });
        ASSERT_NE(found, all.cend()) << id.toStdString();
        EXPECT_TRUE(found->keys.isEmpty()) << id.toStdString() << " has no default key, as in Obsidian";
    }

    auto editor = editorWith(kSections);
    const qsizetype inner = kSections.indexOf(QLatin1String("## A1"));
    editor->setCursorOffset(kSections.indexOf(QLatin1String("body a1")));

    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:fold-more")));
    EXPECT_TRUE(editor->isFoldedAt(inner)) << "the innermost section holding the caret";
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_EQ(editor->cursorOffset(), inner) << "the caret goes to the heading it folded";

    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:fold-more")));
    EXPECT_TRUE(editor->isFoldedAt(0)) << "A1 is folded already, so the one above it";
    EXPECT_EQ(editor->cursorOffset(), 0);

    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:fold-less")));
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->isFoldedAt(inner)) << "one step, the section the caret is in";
    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:fold-less")));
    EXPECT_TRUE(editor->isFoldedAt(inner)) << "A1 does not hold the caret";

    // Toggle: the section the caret is in, whichever way it stands.
    editor->setCursorOffset(kSections.indexOf(QLatin1String("body b")));
    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:toggle-fold")));
    const qsizetype second = kSections.indexOf(QLatin1String("# B"));
    EXPECT_TRUE(editor->isFoldedAt(second));
    EXPECT_EQ(editor->cursorOffset(), second);
    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:toggle-fold")));
    EXPECT_FALSE(editor->isFoldedAt(second));

    // A leaf item folds the parent it is under.
    const QString list = QStringLiteral("- p\n  - c\n    - g\n- q\n");
    auto items = editorWith(list);
    items->setCursorOffset(list.indexOf(QLatin1String("- g")));
    ASSERT_TRUE(items->runCommand(QStringLiteral("editor:toggle-fold")));
    EXPECT_TRUE(items->isFoldedAt(list.indexOf(QLatin1String("  - c"))));
    EXPECT_FALSE(items->isFoldedAt(0));
    EXPECT_EQ(items->cursorOffset(), list.indexOf(QLatin1String("  - c")));
}

TEST(FoldTest, FoldAllAndUnfoldAllCoverEveryHeadingAndParentItemAndLeaveNoState)
{
    const QString note = QStringLiteral("# A\n\n- p\n  - child-c\n\n## A1\n\nbody-a1\n\n# B\n\n"
                                        "- q\n  - child-r\n- leaf\n");
    auto editor = editorWith(note);
    const qreal open = editor->documentHeight();
    editor->setCursorOffset(note.indexOf(QLatin1String("child-c")));

    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:fold-all")));
    for (const char *site : {"# A\n", "- p", "## A1", "# B", "- q"})
        EXPECT_TRUE(editor->isFoldedAt(note.indexOf(QLatin1String(site)))) << site;
    EXPECT_FALSE(editor->isFoldedAt(note.indexOf(QLatin1String("- leaf")))) << "a leaf has nothing to fold";
    EXPECT_EQ(editor->cursorOffset(), 0) << "the caret goes to the outermost fold it was in";
    const QString shown = editor->displayText();
    for (const char *hidden : {"child-c", "body-a1", "child-r", "leaf"})
        EXPECT_FALSE(shown.contains(QLatin1String(hidden))) << hidden;
    EXPECT_EQ(editor->foldState().size(), 5);

    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:unfold-all")));
    EXPECT_DOUBLE_EQ(editor->documentHeight(), open);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("child-r")));
    EXPECT_TRUE(editor->foldState().isEmpty()) << "open is no entry, not a remembered 0";

    // Unfold-all on a note nothing was folded in changes nothing.
    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:unfold-all")));
    EXPECT_TRUE(editor->foldState().isEmpty());
}

TEST(FoldTest, FoldAllAndUnfoldAllLeaveACalloutsFoldAlone)
{
    const QString note = QStringLiteral("# A\n\n> [!note]+ Title\n> text\n");
    auto editor = editorWith(note);
    const qsizetype callout = note.indexOf(QLatin1String("> [!note]"));
    editor->toggleFoldAt(callout);
    ASSERT_TRUE(editor->isFoldedAt(callout));
    editor->runCommand(QStringLiteral("editor:fold-all"));
    editor->runCommand(QStringLiteral("editor:unfold-all"));
    EXPECT_TRUE(editor->isFoldedAt(callout));
    EXPECT_FALSE(editor->isFoldedAt(0));
}

TEST(FoldTest, FoldCommandsWorkInReadingView)
{
    auto editor = editorWith(kSections);
    editor->setCursorOffset(kSections.indexOf(QLatin1String("body a\n")));
    editor->setMode(MarkdownEditor::Mode::Reading);

    EXPECT_FALSE(editor->runCommand(QStringLiteral("editor:toggle-bold"))) << "nothing to edit there";
    EXPECT_TRUE(editor->runCommand(QStringLiteral("editor:toggle-fold")));
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->runCommand(QStringLiteral("editor:unfold-all")));
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->runCommand(QStringLiteral("editor:fold-all")));
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->foldState().size() == 3);
}

TEST(FoldTest, TheFoldCommandsFollowTheFoldingSettings)
{
    const QString note = QStringLiteral("# A\n\nbody\n\n- p\n  - c\n");
    auto editor = editorWith(note);
    editor->setFolding(false, true);
    editor->runCommand(QStringLiteral("editor:fold-all"));
    EXPECT_FALSE(editor->isFoldedAt(0));
    EXPECT_TRUE(editor->isFoldedAt(note.indexOf(QLatin1String("- p"))));
    editor->setFolding(true, true);
    EXPECT_FALSE(editor->isFoldedAt(0)) << "heading folding was off: nothing was written for it";
    editor->setFolding(true, false);
    editor->runCommand(QStringLiteral("editor:unfold-all"));
    editor->setFolding(true, true);
    EXPECT_TRUE(editor->isFoldedAt(note.indexOf(QLatin1String("- p")))) << "indent folding was off: untouched";
    editor->runCommand(QStringLiteral("editor:unfold-all"));
    EXPECT_TRUE(editor->foldState().isEmpty());
}

TEST(FoldTest, TheFoldCommandsToggleTheProperties)
{
    auto editor = editorWith(QStringLiteral("---\ntitle: x\n---\n\n# A\n"));
    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:toggle-fold-properties")));
    EXPECT_TRUE(editor->isFoldedAt(0));
    EXPECT_FALSE(editor->isFoldedAt(editor->noteText().indexOf(QLatin1String("# A")))) << "the heading is not touched";
    ASSERT_TRUE(editor->runCommand(QStringLiteral("editor:toggle-fold-properties")));
    EXPECT_FALSE(editor->isFoldedAt(0));
    // Without properties it is a command that has nothing to do, not an unknown one.
    auto bare = editorWith(QStringLiteral("# A\n"));
    EXPECT_TRUE(bare->runCommand(QStringLiteral("editor:toggle-fold-properties")));
    EXPECT_FALSE(bare->isFoldedAt(0));
}

TEST(FoldTest, FoldAllAndUnfoldAllOnAMegabyteNoteStayUnderBudget)
{
    // Every section's list gets a parent with children, so fold-all pays for
    // list folds (a hidden range and an invalidated block each) as well as
    // for headings.
    QString note = generateNote(1024);
    note.replace(QLatin1String("- a list item\n- another one\n"),
                 QLatin1String("- a list item\n  - a child\n  - another child\n- another one\n"));
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor) << "the parse never came back";
    editor->setCursorOffset(note.size() / 2);
    editor->grab(); // the first paint settles the geometry

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const auto timeOf = [&](const char *command, const char *reset) {
        return perf::bestOf([&] {
            editor->runCommand(QLatin1String(reset));
            QElapsedTimer clock;
            clock.start();
            editor->runCommand(QLatin1String(command));
            return double(clock.nsecsElapsed()) / 1e6;
        }, 3, 50.0);
    };
    const double foldAll = timeOf("editor:fold-all", "editor:unfold-all");
    const qsizetype headings = note.count(QLatin1String("## Section "));
    ASSERT_GE(editor->foldState().size(), 2 * headings) << "every heading and list parent is folded";
    QElapsedTimer clock;
    clock.start();
    const qsizetype remembered = editor->foldState().size();
    const double state = double(clock.nsecsElapsed()) / 1e6;
    const double unfoldAll = timeOf("editor:unfold-all", "editor:fold-all");
    EXPECT_TRUE(editor->foldState().isEmpty());

    std::printf("[ PERF     ] 1 MB note: fold-all %.2f ms, unfold-all %.2f ms, foldState() of %lld folds %.2f ms\n",
                foldAll, unfoldAll, static_cast<long long>(remembered), state);
    EXPECT_LT(foldAll, 50.0);
    EXPECT_LT(unfoldAll, 50.0);
    EXPECT_LT(state, 50.0);
}

TEST(FoldTest, KeystrokeStaysInsideTheBudgetWithEverythingFolded)
{
    const QString note = generateNote(200);
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor) << "the parse never came back";
    const qsizetype heading = note.indexOf(QLatin1String("## Section 40\n"));
    ASSERT_GE(heading, 0);
    editor->runCommand(QStringLiteral("editor:fold-all"));
    editor->setCursorOffset(heading + 13); // the end of the heading's own line: visible
    editor->enterBlockAt(heading + 13);
    ASSERT_TRUE(editor->isFoldedAt(heading));

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    constexpr int kKeystrokes = 20;
    const double perKeystroke = perf::bestOf([&] {
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < kKeystrokes; ++i)
            QTest::keyClicks(editor.get(), QStringLiteral("x"));
        return double(clock.nsecsElapsed()) / kKeystrokes / 1e6;
    }, 3, 8.0);

    std::printf("[ PERF     ] %.2f kB note, everything folded: %.2f ms per keystroke\n",
                note.size() / 1024.0, perKeystroke);
    EXPECT_LT(perKeystroke, 8.0);
    EXPECT_TRUE(editor->isFoldedAt(heading)) << "typing on a heading line leaves its fold";
}

// --- a callout's title is edited where it is drawn -------------------------

namespace {

/// An editor on screen with a callout note, and the centre of its title.
struct CalloutTitleFixture {
    std::unique_ptr<MarkdownEditor> editor;
    QPoint title;
    explicit CalloutTitleFixture(const QString &text)
        : editor(editorWith(text))
    {
        editor->show();
        QApplication::processEvents();
        title = editor->calloutTitleRectAt(0).center().toPoint();
    }
    /// The open title editor; one just closed is hidden and only waiting to be deleted.
    QLineEdit *edit() const
    {
        auto *found = editor->findChild<QLineEdit *>(QStringLiteral("calloutTitleEdit"));
        return found && found->isVisible() ? found : nullptr;
    }
    void click() { QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, title); }
};

} // namespace

TEST(CalloutTitleTest, ClickingTheTitleEditsItInPlace)
{
    CalloutTitleFixture fixture(QStringLiteral("> [!note] Title\n> body\n"));
    ASSERT_TRUE(fixture.editor->calloutTitleRectAt(0).isValid());
    fixture.click();
    EXPECT_FALSE(fixture.editor->displayText().contains(QStringLiteral("[!note]")))
        << "the marker line stays hidden: only the title is open";
    QLineEdit *edit = fixture.edit();
    ASSERT_TRUE(edit);
    EXPECT_TRUE(edit->isVisible());
    EXPECT_EQ(edit->text(), QStringLiteral("Title"));
    QTest::keyClicks(edit, QStringLiteral(" 2"));
    QTest::keyClick(edit, Qt::Key_Return);
    EXPECT_EQ(fixture.editor->noteText(), QStringLiteral("> [!note] Title 2\n> body\n"));
    EXPECT_FALSE(fixture.edit()) << "closed once committed";
    QTest::keyClick(fixture.editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(fixture.editor->noteText(), QStringLiteral("> [!note] Title\n> body\n")) << "one undo step";
}

TEST(CalloutTitleTest, EscapeKeepsTheNote)
{
    CalloutTitleFixture fixture(QStringLiteral("> [!note] Title\n> body\n"));
    fixture.click();
    QLineEdit *edit = fixture.edit();
    ASSERT_TRUE(edit);
    QTest::keyClicks(edit, QStringLiteral("zzz"));
    QTest::keyClick(edit, Qt::Key_Escape);
    EXPECT_EQ(fixture.editor->noteText(), QStringLiteral("> [!note] Title\n> body\n"));
    EXPECT_FALSE(fixture.edit());
}

TEST(CalloutTitleTest, AnImplicitTitleGetsASpace)
{
    CalloutTitleFixture fixture(QStringLiteral("> [!note]\n> body\n"));
    fixture.click();
    QLineEdit *edit = fixture.edit();
    ASSERT_TRUE(edit);
    EXPECT_EQ(edit->text(), QString()) << "the drawn \"Note\" is not in the file";
    QTest::keyClicks(edit, QStringLiteral("X"));
    QTest::keyClick(edit, Qt::Key_Return);
    EXPECT_EQ(fixture.editor->noteText(), QStringLiteral("> [!note] X\n> body\n"));
}

TEST(CalloutTitleTest, ClickingTheBodyStillPlacesTheCaret)
{
    CalloutTitleFixture fixture(QStringLiteral("> [!note] Title\n> body\n"));
    const QRectF rect = fixture.editor->calloutTitleRectAt(0);
    QTest::mouseClick(fixture.editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(int(rect.left()) + 10, int(rect.bottom()) + 30));
    EXPECT_FALSE(fixture.edit());
}

TEST(CalloutTitleTest, ReadingModeNeverOpensTheTitle)
{
    // A read-only view must not write to the note, through the overlay or otherwise.
    const QString note = QStringLiteral("> [!note] Title\n> body\n");
    CalloutTitleFixture fixture(note);
    fixture.editor->setMode(MarkdownEditor::Mode::Reading);
    QApplication::processEvents();
    fixture.title = fixture.editor->calloutTitleRectAt(0).center().toPoint();
    fixture.click();
    EXPECT_FALSE(fixture.edit());
    EXPECT_EQ(fixture.editor->noteText(), note);
}

TEST(CalloutTitleTest, ReadingModeNeverFillsAShortRow)
{
    const QString note = QStringLiteral("| a | b | c |\n|---|---|---|\n| 1 |\n\nafter\n");
    for (int y = 4; y < 200; y += 6) {
        for (int x = 4; x < 880; x += 40) {
            auto editor = editorWith(note);
            editor->setMode(MarkdownEditor::Mode::Reading);
            editor->show();
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, QPoint(x, y));
            ASSERT_EQ(editor->noteText(), note) << "click at " << x << "," << y;
        }
    }
}

TEST(CalloutTitleTest, AnExternalEditKeepsTheTypedTitleOnItsCallout)
{
    const QString note = QStringLiteral("> [!note] First\n> a\n\n> [!tip] Second\n> b\n");
    auto editor = editorWith(note);
    editor->show();
    QApplication::processEvents();
    const qsizetype second = note.indexOf(QStringLiteral("> [!tip]"));
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      editor->calloutTitleRectAt(second).center().toPoint());
    auto *edit = editor->findChild<QLineEdit *>(QStringLiteral("calloutTitleEdit"));
    ASSERT_TRUE(edit && edit->isVisible());
    QTest::keyClicks(edit, QStringLiteral(" 2"));
    // Another tab on the same note typed above it, while this title is open:
    // what was typed lands on its own callout, not lost, not on another.
    editor->applyExternalEdit(0, 0, QStringLiteral("intro\n\n"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("intro\n\n> [!note] First\n> a\n\n> [!tip] Second 2\n> b\n"));
    EXPECT_FALSE(editor->findChild<QLineEdit *>(QStringLiteral("calloutTitleEdit"))->isVisible());
}

TEST(CalloutTitleTest, TheCaretLandsWhereTheTitleWasClicked)
{
    CalloutTitleFixture fixture(QStringLiteral("> [!note] A longer title here\n> body\n"));
    const QRectF rect = fixture.editor->calloutTitleRectAt(0);
    fixture.title = QPoint(int(rect.left()) + 4, int(rect.center().y()));
    fixture.click();
    QLineEdit *edit = fixture.edit();
    ASSERT_TRUE(edit);
    EXPECT_LT(edit->cursorPosition(), 3) << "a click at the start is not the end of the text";
}

// --- neighbouring blocks never overlap ------------------------------------

/// Scrolls through the whole note, the way reading it would, and says where two
/// neighbouring blocks' boxes (a callout's panel included) cross. Empty if none.
QString firstBlockOverlap(MarkdownEditor &editor)
{
    for (int pass = 0; pass < 2; ++pass) {
        QScrollBar *bar = editor.verticalScrollBar();
        for (int at = 0; at <= bar->maximum() + editor.viewport()->height(); at += 150) {
            bar->setValue(at);
            QList<MarkdownEditor::BlockBox> boxes = editor.blockBoxes();
            boxes.removeIf([](const MarkdownEditor::BlockBox &box) { return box.rect.isEmpty(); });
            for (qsizetype i = 1; i < boxes.size(); ++i) {
                if (boxes.at(i).rect.top() < boxes.at(i - 1).rect.bottom() - 0.01)
                    return QStringLiteral("block at source %1 (top %2) starts inside the one before (bottom %3)")
                        .arg(boxes.at(i).source.start).arg(boxes.at(i).rect.top()).arg(boxes.at(i - 1).rect.bottom());
            }
        }
        editor.setMode(MarkdownEditor::Mode::Reading); // and again in the other view
    }
    return {};
}

TEST(BlockOverlapTest, ConsecutiveColonFenceCalloutsDoNotOverlap)
{
    auto editor = editorWith(QStringLiteral(
        "## The container form\n\n"
        "::: tip A colon-fence callout\nObsidian does not have this form, but the \"Ideas\" spec this app also\n"
        "reads does; it renders exactly like the `> [!tip]` form above.\n:::\n\n"
        "::: details Foldable, closed by default\nThe details type folds like `-` does on the `>` form.\n:::\n\n"
        "after\n"));
    EXPECT_EQ(firstBlockOverlap(*editor).toStdString(), "");
}

TEST(BlockOverlapTest, ConsecutiveQuoteCalloutsDoNotOverlap)
{
    const QString note = QStringLiteral(
        "> [!tip] one\n> body\n\n> [!note]- two\n> hidden\n\n> [!note]+ three\n> shown\n\nafter\n");
    auto editor = editorWith(note);
    EXPECT_EQ(firstBlockOverlap(*editor).toStdString(), "");
    editor->toggleFoldAt(note.indexOf(QStringLiteral("[!note]-")));
    EXPECT_EQ(firstBlockOverlap(*editor).toStdString(), "");
}

// A callout's panel reaches 8 px above its block, so with no blank line before
// it, the block before - a fence's closing line, say - has to end there.
TEST(BlockOverlapTest, AnythingRightAfterAFenceStartsClearOfIt)
{
    const QStringList followers{
        QStringLiteral("> [!note]\n> body\n"),  QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |\n"),
        QStringLiteral("> a quote\n"),           QStringLiteral("## heading\n"),
        QStringLiteral("- item\n- item\n"),       QStringLiteral("---\n"),
        QStringLiteral("$$\nx^2\n$$\n"),         QStringLiteral("```text\nb\n```\n"),
        QStringLiteral("text\n"),
    };
    for (const QString &follower : followers) {
        auto editor = editorWith(QStringLiteral("```text\na\n```\n") + follower + QStringLiteral("\nafter\n"));
        EXPECT_EQ(firstBlockOverlap(*editor).toStdString(), "") << follower.toStdString();
    }
}

// --- `#|` code block options ----------------------------------

TEST(CodeHeaderTest, ABlockWithALanguageIsTitledByItsName)
{
    // `listing` is what follows the opening fence; the title is read at its
    // last line, the one `#|` options leave in place.
    auto titleOf = [](const QString &fence, const QString &listing) {
        const QString note = fence + QStringLiteral("\n") + listing + QStringLiteral("\n```\n");
        return editorWith(note)->codeTitleAt(note.lastIndexOf(QLatin1Char('\n'), -6) + 1);
    };
    EXPECT_EQ(titleOf(QStringLiteral("```python"), QStringLiteral("x = 1")), QStringLiteral("python"));
    EXPECT_EQ(titleOf(QStringLiteral("```python name=\"Setup script\""), QStringLiteral("x = 1")),
              QStringLiteral("Setup script"));
    EXPECT_EQ(titleOf(QStringLiteral("```python"), QStringLiteral("#| title: main.py\nx = 1")),
              QStringLiteral("main.py"));
    EXPECT_EQ(titleOf(QStringLiteral("```"), QStringLiteral("x = 1")), QString());

    // The strip is a header's height of its own, so a named block is taller
    // than the same listing with no language to name it.
    auto named = editorWith(QStringLiteral("```cpp\nx\n```\n"));
    auto bare = editorWith(QStringLiteral("```\nx\n```\n"));
    EXPECT_GT(named->documentHeight() - bare->documentHeight(), 10);
}

TEST(CodeOptionsTest, ATitleGivesTheBlockAHeaderStrip)
{
    // No language to name it: only the option gives this block a strip.
    auto plain = editorWith(QStringLiteral("```\nx = 1\n```\n"));
    auto titled = editorWith(QStringLiteral("```\n#| title: fib.py\nx = 1\n```\n"));

    // The option line is hidden either way, so the listing is the same size;
    // the difference is the strip the title is drawn in.
    EXPECT_EQ(titled->displayText(), plain->displayText());
    EXPECT_GT(titled->documentHeight(), plain->documentHeight());
}

TEST(CodeOptionsTest, FoldCollapsesTheBlockToItsHeader)
{
    const QString note =
        QStringLiteral("```python\n#| title: fib.py\n#| fold: true\nx = 1\ny = 2\n```\n");
    auto editor = editorWith(note);
    const qsizetype inside = note.indexOf(QStringLiteral("x = 1"));

    EXPECT_TRUE(editor->isFoldedAt(inside));
    const qreal collapsed = editor->documentHeight();

    editor->toggleFoldAt(inside);
    EXPECT_FALSE(editor->isFoldedAt(inside));
    EXPECT_GT(editor->documentHeight(), collapsed);
}

TEST(CodeOptionsTest, LineNumbersIndentTheListingAndLeaveTheBytesAlone)
{
    const QString note =
        QStringLiteral("```python\n#| lines: 10\n#| highlight: 2\nx = 1\ny = 2\n```\n");
    auto editor = editorWith(note);

    EXPECT_EQ(editor->noteText(), note);
    // The options are syntax, so what is laid out is the listing alone.
    EXPECT_EQ(editor->displayText().trimmed(), QStringLiteral("x = 1\ny = 2"));
}

// --- Code Styler: the fence line's parameters, drawn ---------------------------

namespace {

/// An editor with the advanced-code-blocks plugin on (or off), and the
/// highlight names the settings default to.
std::unique_ptr<MarkdownEditor> stylerEditorWith(const QString &text, bool on = true)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    ParseOptions options;
    options.codeStyler = on;
    options.highlightKinds = {QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"),
                              QStringLiteral("custom")};
    editor->setParseOptions(options);
    editor->setNoteText(text);
    return editor;
}

} // namespace

TEST(CodeStylerEditorTest, ATitleOnTheFenceLineNamesTheBlockAndIsHidden)
{
    const QString note = QStringLiteral("```cpp title:\"main file.cpp\" ln:5\nint a;\n```\n");
    auto editor = stylerEditorWith(note);
    const qsizetype inside = note.indexOf(QStringLiteral("int a;"));
    EXPECT_EQ(editor->codeTitleAt(inside), QStringLiteral("main file.cpp"));
    EXPECT_EQ(editor->noteText(), note);
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("title")));
}

TEST(CodeStylerEditorTest, FoldStartsClosedAndNamesItselfWhileFolded)
{
    const QString note = QStringLiteral("```cpp fold:\"Collapsed listing\"\nint a;\nint b;\n```\n");
    auto editor = stylerEditorWith(note);
    const qsizetype inside = note.indexOf(QStringLiteral("int a;"));
    ASSERT_TRUE(editor->isFoldedAt(inside));
    EXPECT_EQ(editor->codeTitleAt(inside), QStringLiteral("Collapsed listing"));
    const qreal folded = editor->documentHeight();
    editor->toggleFoldAt(inside);
    EXPECT_FALSE(editor->isFoldedAt(inside));
    EXPECT_GT(editor->documentHeight(), folded);
    EXPECT_EQ(editor->codeTitleAt(inside), QStringLiteral("cpp")) << "open, it is the language's name again";
}

TEST(CodeStylerEditorTest, IgnoreDropsTheHeaderStrip)
{
    auto plain = stylerEditorWith(QStringLiteral("```cpp\nint a;\n```\n"));
    auto ignored = stylerEditorWith(QStringLiteral("```cpp ignore title:x\nint a;\n```\n"));
    EXPECT_GT(plain->documentHeight() - ignored->documentHeight(), 10);
}

TEST(CodeStylerEditorTest, OffTheFenceLineIsOnlyALanguageAndAttributes)
{
    const QString note = QStringLiteral("```cpp title:x fold\nint a;\n```\n");
    auto editor = stylerEditorWith(note, false);
    const qsizetype inside = note.indexOf(QStringLiteral("int a;"));
    EXPECT_EQ(editor->codeTitleAt(inside), QStringLiteral("cpp"));
    EXPECT_FALSE(editor->isFoldedAt(inside));
    // Turning it on later (the core plugin's switch) re-reads the note.
    ParseOptions on = editor->parseOptions();
    on.codeStyler = true;
    editor->setParseOptions(on);
    EXPECT_EQ(editor->codeTitleAt(inside), QStringLiteral("x"));
    EXPECT_TRUE(editor->isFoldedAt(inside));
}

namespace {

/// The mean colour change between two grabs of the same note, over the pixels
/// that changed, as {red, green, blue}.
std::array<double, 3> meanChange(const QImage &before, const QImage &after, int *changed = nullptr)
{
    std::array<double, 3> sum{0, 0, 0};
    int count = 0;
    for (int y = 0; y < qMin(before.height(), after.height()); ++y) {
        for (int x = 0; x < qMin(before.width(), after.width()); ++x) {
            const QColor a = before.pixelColor(x, y);
            const QColor b = after.pixelColor(x, y);
            if (a == b)
                continue;
            sum[0] += b.red() - a.red();
            sum[1] += b.green() - a.green();
            sum[2] += b.blue() - a.blue();
            ++count;
        }
    }
    if (changed)
        *changed = count;
    for (double &channel : sum)
        channel = count ? channel / count : 0;
    return sum;
}

QImage grabbed(MarkdownEditor &editor)
{
    return editor.grab().toImage().convertToFormat(QImage::Format_RGB32);
}

} // namespace

TEST(CodeStylerEditorTest, NamedHighlightsTintTheirLinesWithTheirOwnColour)
{
    const QString body = QStringLiteral("one\ntwo\nthree\nfour\n```\n");
    const auto paint = [&](const QString &info) {
        auto editor = stylerEditorWith(QStringLiteral("```text ") + info + QStringLiteral("\n") + body);
        editor->setCodeHighlights(QStringLiteral("info=blue, error=red, custom=#00FF00"));
        return grabbed(*editor);
    };
    const QImage none = paint(QString());
    int infoPixels = 0;
    const auto info = meanChange(none, paint(QStringLiteral("info:2")), &infoPixels);
    int errorPixels = 0;
    const auto error = meanChange(none, paint(QStringLiteral("error:2")), &errorPixels);
    const auto custom = meanChange(none, paint(QStringLiteral("custom:2")));
    EXPECT_GT(infoPixels, 500) << "a whole row of the panel changes";
    EXPECT_NEAR(infoPixels, errorPixels, 200) << "the same row, a different colour";
    EXPECT_GT(info[2], info[0]) << "info is blue";
    EXPECT_GT(error[0], error[2]) << "error is red";
    EXPECT_GT(custom[1], custom[0]) << "a hex colour is taken as written";
    EXPECT_GT(custom[1], custom[2]);
}

TEST(CodeStylerEditorTest, AnUnknownNameAndAnOffPluginPaintNothing)
{
    const QString note = QStringLiteral("```text info:2\none\ntwo\n```\n");
    auto named = stylerEditorWith(note);
    named->setCodeHighlights(QStringLiteral("info=blue"));
    auto unknown = stylerEditorWith(note);
    unknown->setCodeHighlights(QStringLiteral("error=red")); // `info` is no name the setting has
    auto without = stylerEditorWith(QStringLiteral("```text info\none\ntwo\n```\n"));
    without->setCodeHighlights(QStringLiteral("error=red"));
    int changed = 0;
    meanChange(grabbed(*without), grabbed(*named), &changed);
    EXPECT_GT(changed, 500) << "control: the named one does paint";
    meanChange(grabbed(*without), grabbed(*unknown), &changed);
    EXPECT_EQ(changed, 0);

    // Off, `info:2` is plain text on the fence line and nothing is tinted.
    auto off = stylerEditorWith(note, false);
    off->setCodeHighlights(QStringLiteral("info=blue"));
    auto offPlain = stylerEditorWith(QStringLiteral("```text\none\ntwo\n```\n"), false);
    meanChange(grabbed(*offPlain), grabbed(*off), &changed);
    EXPECT_EQ(changed, 0);
}

TEST(CodeStylerEditorTest, InlineCodeIsHighlightedInItsLanguageAndTheTagComesBackOnTheCaretLine)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";
    const QString tagged = QStringLiteral("Say `{python} import os` now.\n\nElsewhere.\n");
    const QString plain = QStringLiteral("Say `import os` now.\n\nElsewhere.\n");
    auto styled = stylerEditorWith(tagged);
    auto bare = stylerEditorWith(plain);
    auto off = stylerEditorWith(tagged, false);
    const auto keyword = [](const MarkdownEditor &editor, const QString &note) {
        return editor.formatAt(note.indexOf(QStringLiteral("import"))).foreground().color();
    };
    EXPECT_NE(keyword(*styled, tagged), keyword(*bare, plain)) << "`import` is a keyword in Python";
    EXPECT_EQ(keyword(*off, tagged), keyword(*bare, plain)) << "off: one plain colour for the whole span";

    // Hidden while the caret is elsewhere, written out while it touches the span (as `**` are).
    EXPECT_FALSE(styled->displayText().contains(QStringLiteral("{python}")));
    EXPECT_TRUE(off->displayText().contains(QStringLiteral("{python}")));
    styled->enterBlockAt(tagged.indexOf(QStringLiteral("import")) + 2);
    styled->grab();
    EXPECT_TRUE(styled->displayText().contains(QStringLiteral("{python}")));
    EXPECT_NE(keyword(*styled, tagged), keyword(*bare, plain)) << "still coloured while the tag shows";
}

// setNotebookRunner is a fake here, the same way setQueryRunner and
// setBaseRunner are faked elsewhere in this file - libs/editor must not link
// libs/ext/notebook, so a real kernel is only ever exercised by
// tests/test_notebook.cpp. `deliver` is always called through
// QTimer::singleShot(0, ...), matching every other runner's contract here:
// never synchronously, so a test has to pump the event loop once.
TEST(NotebookBlockTest, RunOptionsShowOutputsBelow)
{
    const QString note = QStringLiteral("```python\n#| title: cell\nprint(1)\n```\n");
    auto editor = editorWith(note);
    const qreal before = editor->documentHeight();

    bool forceRunSeen = false;
    editor->setNotebookRunner([&forceRunSeen](const QString &, const QString &, bool forceRun,
                                              const MarkdownEditor::NotebookRunOptions &,
                                              const MarkdownEditor::NotebookDeliver &deliver) {
        forceRunSeen = forceRunSeen || forceRun;
        MarkdownEditor::NotebookRunResult result;
        result.hasRun = true;
        result.ok = true;
        MarkdownEditor::NotebookOutput output;
        output.kind = MarkdownEditor::NotebookOutput::Kind::Text;
        output.text = QStringLiteral("1\n");
        result.outputs.append(output);
        QTimer::singleShot(0, [deliver, result] { deliver(result); });
    });
    QTest::qWait(50);

    EXPECT_GT(editor->documentHeight(), before);
    const auto info = editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)")));
    EXPECT_TRUE(info.eligible);
    EXPECT_TRUE(info.hasRun);
    EXPECT_TRUE(info.ok);
    // A note opening never runs anything - only a cache-only lookup.
    EXPECT_FALSE(forceRunSeen);
}

TEST(NotebookBlockTest, EvalFalseNeverRuns)
{
    const QString note = QStringLiteral("```python\n#| eval: false\nprint(1)\n```\n\n"
                                        "```python\n#| sandbox: host\nprint(2)\n```\n");
    auto editor = editorWith(note);
    QList<bool> wasmRequests;
    editor->setNotebookRunner([&wasmRequests](const QString &, const QString &, bool forceRun,
                                              const MarkdownEditor::NotebookRunOptions &options,
                                              const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun)
            wasmRequests.append(options.sandboxWasm);
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    QTest::qWait(20);
    EXPECT_FALSE(editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)"))).eligible);
    EXPECT_FALSE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(1)"))));
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(2)"))));
    EXPECT_EQ(wasmRequests, QList<bool>{false}); // "host" is no tier: the block runs as the default one
}

TEST(NotebookBlockTest, SandboxWasmIsPassedOn)
{
    const QString note = QStringLiteral("```python\n#| sandbox: wasm\nprint(3)\n```\n");
    auto editor = editorWith(note);
    QList<bool> requests;
    editor->setNotebookRunner([&requests](const QString &, const QString &, bool forceRun,
                                          const MarkdownEditor::NotebookRunOptions &options,
                                          const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun)
            requests.append(options.sandboxWasm);
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    QTest::qWait(20);
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(3)"))));
    ASSERT_EQ(requests.size(), 1);
    EXPECT_TRUE(requests.first());
}

TEST(NotebookBlockTest, AnEmbeddedScriptRunsFromItsOwnFolder)
{
    QTemporaryDir dir;
    const QString script = dir.filePath(QStringLiteral("hello.py"));
    {
        QFile file(script);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("print('hi')\n");
    }
    auto editor = editorWith(QStringLiteral("![[hello.py]]\n\nafter\n"));
    editor->setLinkResolver([&](const QString &) { return script; });
    QString language, source, folder;
    editor->setNotebookRunner([&](const QString &lang, const QString &src, bool forceRun,
                                  const MarkdownEditor::NotebookRunOptions &options,
                                  const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun) {
            language = lang;
            source = src;
            folder = options.workingDir;
        }
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    editor->setCursorOffset(editor->noteText().size()); // off the embed's line
    QTest::qWait(20);
    ASSERT_TRUE(editor->runNotebookBlockAt(0));
    EXPECT_EQ(language, QStringLiteral("python"));
    EXPECT_EQ(source.trimmed(), QStringLiteral("print('hi')"));
    EXPECT_EQ(folder, dir.path());
}

TEST(NotebookBlockTest, RunControlHitTests)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n");
    auto editor = editorWith(note);

    bool ran = false;
    editor->setNotebookRunner([&ran](const QString &, const QString &, bool forceRun,
                                     const MarkdownEditor::NotebookRunOptions &,
                                     const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun)
            ran = true;
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    QTest::qWait(50);

    const qsizetype offset = note.indexOf(QStringLiteral("print(1)"));
    const auto info = editor->notebookInfoAt(offset);
    ASSERT_TRUE(info.eligible);
    // In the header's corner, the way codeLabelOf's own rect is.
    EXPECT_TRUE(info.runRect.isValid());
    EXPECT_GT(info.runRect.width(), 0);

    EXPECT_TRUE(editor->runNotebookBlockAt(offset));
    QTest::qWait(50);
    EXPECT_TRUE(ran);

    // Outside any code block: nothing to run.
    EXPECT_FALSE(editor->runNotebookBlockAt(0));
}

TEST(NotebookBlockTest, CachedOutputReappearsAfterReopeningWithoutAKernel)
{
    const QString note = QStringLiteral("```python\nprint(2)\n```\n");
    // Stands in for libs/ext/notebook's own sidecar cache (NotebookCache.cpp)
    // - shared across two separate MarkdownEditor instances the way the real
    // cache is shared across two separate times a note is opened.
    QHash<QString, MarkdownEditor::NotebookRunResult> sidecar;
    int forceRunCount = 0;
    const auto runner = [&sidecar, &forceRunCount](const QString &language, const QString &source,
                                                    bool forceRun,
                                                    const MarkdownEditor::NotebookRunOptions &,
                                                    const MarkdownEditor::NotebookDeliver &deliver) {
        const QString key = language.toLower() + QLatin1Char('|') + source;
        if (forceRun) {
            ++forceRunCount;
            MarkdownEditor::NotebookRunResult result;
            result.hasRun = true;
            result.ok = true;
            MarkdownEditor::NotebookOutput output;
            output.kind = MarkdownEditor::NotebookOutput::Kind::Text;
            output.text = QStringLiteral("2\n");
            result.outputs.append(output);
            sidecar.insert(key, result);
            QTimer::singleShot(0, [deliver, result] { deliver(result); });
            return;
        }
        const MarkdownEditor::NotebookRunResult cached = sidecar.value(key);
        QTimer::singleShot(0, [deliver, cached] { deliver(cached); });
    };

    auto first = editorWith(note);
    first->setNotebookRunner(runner);
    const qsizetype offset = note.indexOf(QStringLiteral("print(2)"));
    ASSERT_TRUE(first->runNotebookBlockAt(offset));
    QTest::qWait(50);
    EXPECT_TRUE(first->notebookInfoAt(offset).hasRun);
    EXPECT_EQ(forceRunCount, 1);

    // A fresh editor over the same text - "reopening the note" - sharing
    // only the sidecar, never the first editor's own runtime state.
    auto second = editorWith(note);
    second->setNotebookRunner(runner);
    QTest::qWait(50);
    EXPECT_TRUE(second->notebookInfoAt(offset).hasRun);
    EXPECT_EQ(second->notebookInfoAt(offset).ok, true);
    // Reopening never started anything - still the one run from above.
    EXPECT_EQ(forceRunCount, 1);
}

// --- Jupyter-style notebook features (NotebookBlock.cpp) -------------------

namespace {

/// A runner that answers every cache lookup with `lookup` and every real run
/// with `ran`, recording what it was asked. `deliver` goes through a
/// zero-length timer, as the real one's contract says.
struct FakeNotebook {
    MarkdownEditor::NotebookRunResult lookup;
    MarkdownEditor::NotebookRunResult ran;
    QList<MarkdownEditor::NotebookRunOptions> runs;
    QList<MarkdownEditor::NotebookRunOptions> clears;
    QStringList runSources;
    bool holdRuns = false; ///< a run that never finishes: the block stays "running"

    MarkdownEditor::NotebookRunner runner()
    {
        return [this](const QString &, const QString &source, bool forceRun,
                      const MarkdownEditor::NotebookRunOptions &options,
                      const MarkdownEditor::NotebookDeliver &deliver) {
            if (options.clear) {
                clears.append(options);
                QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult()); });
                return;
            }
            if (forceRun) {
                runs.append(options);
                runSources.append(source);
                if (holdRuns)
                    return;
            }
            const MarkdownEditor::NotebookRunResult answer = forceRun ? ran : lookup;
            QTimer::singleShot(0, [deliver, answer] { deliver(answer); });
        };
    }
};

MarkdownEditor::NotebookRunResult textRun(const QString &text, int count = 0, bool isStderr = false)
{
    MarkdownEditor::NotebookRunResult result;
    result.hasRun = true;
    result.executionCount = count;
    MarkdownEditor::NotebookOutput output;
    output.text = text;
    output.isStderr = isStderr;
    result.outputs.append(output);
    return result;
}

} // namespace

TEST(NotebookBlockTest, OutputFalseHidesTheOutputButStillRuns)
{
    const QString note = QStringLiteral("```python\n#| output: false\nprint(1)\n```\n\n"
                                        "```python\nprint(2)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    fake.lookup = textRun(QStringLiteral("x\n"));
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    const auto hidden = editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)")));
    const auto shown = editor->notebookInfoAt(note.indexOf(QStringLiteral("print(2)")));
    EXPECT_TRUE(hidden.hasRun);
    EXPECT_EQ(hidden.outputCellCount, 0);
    EXPECT_EQ(shown.outputCellCount, 1);
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(1)"))));
    EXPECT_EQ(fake.runs.size(), 1); // output: false does not stop it running
}

TEST(NotebookBlockTest, IncludeFalseHidesTheListingAndTheOutput)
{
    const QString note = QStringLiteral("```python\n#| include: false\nprint(1)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    fake.lookup = textRun(QStringLiteral("x\n"));
    editor->setNotebookRunner(fake.runner());
    editor->setCursorOffset(note.size());
    QTest::qWait(50);
    EXPECT_EQ(editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)"))).outputCellCount, 0);
}

TEST(NotebookBlockTest, CacheTimeoutAndKernelOptionsReachTheRunner)
{
    const QString note = QStringLiteral("```python\n#| cache: false\n#| timeout: 7\n#| kernel: on\nprint(1)\n```\n\n"
                                        "```python\n#| kernel: python3\nprint(2)\n```\n\n"
                                        "```python\nprint(12)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(30);
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(1)"))));
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(2)"))));
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(12)"))));
    ASSERT_EQ(fake.runs.size(), 3);
    EXPECT_FALSE(fake.runs[0].cache);
    EXPECT_EQ(fake.runs[0].timeoutSeconds, 7);
    EXPECT_EQ(fake.runs[0].kernel, QStringLiteral("on"));
    EXPECT_TRUE(fake.runs[1].cache);
    EXPECT_EQ(fake.runs[1].kernel, QStringLiteral("python3"));
    EXPECT_TRUE(fake.runs[2].cache);
    EXPECT_EQ(fake.runs[2].timeoutSeconds, 0);
    EXPECT_TRUE(fake.runs[2].kernel.isEmpty());
}

TEST(NotebookBlockTest, TheExecutionCountIsShown)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    fake.lookup = textRun(QStringLiteral("1\n"), 4);
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    EXPECT_EQ(editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)"))).executionCount, 4);
}

TEST(NotebookBlockTest, EditedCodeKeepsItsOldOutputMarkedOutdatedUntilRunAgain)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    fake.ran = textRun(QStringLiteral("2\n"), 2);
    fake.lookup = textRun(QStringLiteral("1\n"), 1);
    // Only print(1) has anything cached; the edited source has not been run.
    editor->setNotebookRunner([&fake](const QString &, const QString &source, bool forceRun,
                                      const MarkdownEditor::NotebookRunOptions &options,
                                      const MarkdownEditor::NotebookDeliver &deliver) {
        const MarkdownEditor::NotebookRunResult answer =
            forceRun ? fake.ran : (source.contains(QLatin1String("print(1)")) ? fake.lookup : MarkdownEditor::NotebookRunResult());
        Q_UNUSED(options);
        QTimer::singleShot(0, [deliver, answer] { deliver(answer); });
    });
    QTest::qWait(50);
    auto info = editor->notebookInfoAt(12);
    ASSERT_EQ(info.outputCellCount, 1);
    EXPECT_FALSE(info.stale);

    // Edit the code: the result for the new source does not exist.
    editor->setNoteText(QStringLiteral("```python\nprint(2)\n```\n"));
    QTest::qWait(300);
    info = editor->notebookInfoAt(12);
    EXPECT_TRUE(info.stale);
    EXPECT_GE(info.outputCellCount, 2); // the "outdated" line and the old output

    // Running it again replaces the old result.
    ASSERT_TRUE(editor->runNotebookBlockAt(12));
    QTest::qWait(100);
    info = editor->notebookInfoAt(12);
    EXPECT_FALSE(info.stale);
    EXPECT_EQ(info.outputCellCount, 1);
    EXPECT_EQ(info.executionCount, 2);
}

TEST(NotebookBlockTest, StderrAndHtmlOutputsAreLaidOut)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    fake.lookup = textRun(QStringLiteral("warn\n"), 1, true);
    MarkdownEditor::NotebookOutput html;
    html.kind = MarkdownEditor::NotebookOutput::Kind::Html;
    html.text = QStringLiteral("<table><tr><td>cell</td></tr></table>");
    html.alt = QStringLiteral("cell");
    fake.lookup.outputs.append(html);
    MarkdownEditor::NotebookOutput latex;
    latex.kind = MarkdownEditor::NotebookOutput::Kind::Markdown;
    latex.text = QStringLiteral("$$\nx^2\n$$");
    fake.lookup.outputs.append(latex);
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    // The warning, the table (drawn by litehtml, or as its plain twin), the formula.
    EXPECT_EQ(editor->notebookInfoAt(note.indexOf(QStringLiteral("print(1)"))).outputCellCount, 3);
}

TEST(NotebookBlockTest, OutputKeepsItsLineBreaks)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n");
    const qsizetype offset = note.indexOf(QStringLiteral("print(1)"));
    auto height = [&](const QString &text, QStringList *shown) {
        auto editor = editorWith(note);
        editor->setCursorOffset(note.size());
        FakeNotebook fake;
        fake.lookup = textRun(text);
        editor->setNotebookRunner(fake.runner());
        QTest::qWait(50);
        if (shown)
            *shown = editor->notebookOutputTextAt(offset);
        return editor->documentHeight();
    };
    QStringList shown;
    const qreal one = height(QStringLiteral("a\n"), nullptr);
    const qreal three = height(QStringLiteral("a\nb\nc\n"), &shown);
    // QTextLayout ignores \n, so each break has to reach it as U+2028 or the
    // output is one run-on line: every extra line must add one line of height.
    const qreal two = height(QStringLiteral("a\nb\n"), nullptr);
    EXPECT_GT(two - one, 8);
    EXPECT_NEAR(three - one, 2 * (two - one), 1);
    EXPECT_EQ(shown, QStringList{QStringLiteral("a\nb\nc\n")});
}

TEST(NotebookBlockTest, AnErrorShowsItsHeaderThenTheTracebackOnce)
{
    const QString note = QStringLiteral("```rust\nfn main() {}\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size());
    FakeNotebook fake;
    fake.lookup.hasRun = true;
    MarkdownEditor::NotebookOutput error;
    error.kind = MarkdownEditor::NotebookOutput::Kind::Error;
    error.ename = QStringLiteral("Exit code 1");
    error.text = QStringLiteral("boom\n");
    error.traceback = {QStringLiteral("boom")};
    fake.lookup.outputs.append(error);
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    EXPECT_EQ(editor->notebookOutputTextAt(note.indexOf(QStringLiteral("fn main"))),
              QStringList{QStringLiteral("Exit code 1\nboom")});
}

TEST(NotebookBlockTest, ALanguageWithNoRunnerGetsNoRunControl)
{
    // "That that should be run should not have a run object": json has no
    // recipe and no kernel, so it is not a runnable block at all - no control,
    // no output row, no warning triangle after a click, and "Run all" skips it.
    const QString note = QStringLiteral("```json\n{}\n```\n\n```python\nprint(1)\n```\n");
    auto editor = editorWith(note);
    editor->setNotebookRunnable([](const QString &language) { return language == QLatin1String("python"); });
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);

    const qsizetype json = note.indexOf(QLatin1String("{}"));
    const qsizetype python = note.indexOf(QLatin1String("print(1)"));
    EXPECT_FALSE(editor->notebookInfoAt(json).eligible);
    EXPECT_FALSE(editor->notebookInfoAt(json).runRect.isValid());
    EXPECT_FALSE(editor->runNotebookBlockAt(json));
    EXPECT_TRUE(editor->notebookInfoAt(python).eligible);
    EXPECT_EQ(editor->runAllNotebookBlocks(), 1);
}

TEST(NotebookBlockTest, WithoutARunnablePredicateEveryLanguageIsEligible)
{
    const QString note = QStringLiteral("```json\n{}\n```\n\n```python\nprint(1)\n```\n");
    auto editor = editorWith(note);
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    EXPECT_TRUE(editor->notebookInfoAt(note.indexOf(QLatin1String("{}"))).eligible);
    EXPECT_TRUE(editor->notebookInfoAt(note.indexOf(QLatin1String("print(1)"))).eligible);
}

TEST(NotebookBlockTest, ClearingOutputsAsksTheRunnerAndEmptiesTheBlock)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    fake.lookup = textRun(QStringLiteral("1\n"), 1);
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    ASSERT_EQ(editor->notebookInfoAt(12).outputCellCount, 1);

    EXPECT_EQ(editor->clearNotebookOutputs(), 1);
    QTest::qWait(50);
    EXPECT_EQ(fake.clears.size(), 1);
    const auto info = editor->notebookInfoAt(12);
    EXPECT_FALSE(info.hasRun);
    EXPECT_EQ(info.outputCellCount, 0);
}

TEST(NotebookBlockTest, RunToTheCursorStopsThere)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n\n```python\nprint(2)\n```\n\n"
                                        "```python\nprint(12)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(30);
    EXPECT_EQ(editor->runNotebookBlocksUpTo(note.indexOf(QStringLiteral("print(2)"))), 2);
    EXPECT_EQ(fake.runSources, (QStringList{QStringLiteral("print(1)"), QStringLiteral("print(2)")}));
    EXPECT_EQ(editor->runAllNotebookBlocks(), 3);
}

TEST(NotebookBlockTest, RunFromTheCursorStartsAtTheBlockHoldingIt)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n\n```python\nprint(2)\n```\n\n"
                                        "```python\nprint(12)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(30);
    EXPECT_EQ(editor->runNotebookBlocksFrom(note.indexOf(QStringLiteral("print(2)"))), 2);
    EXPECT_EQ(fake.runSources, (QStringList{QStringLiteral("print(2)"), QStringLiteral("print(12)")}));
    fake.runSources.clear();
    EXPECT_EQ(editor->runNotebookBlocksFrom(note.indexOf(QStringLiteral("print(12)"))), 1);
    EXPECT_EQ(editor->runNotebookBlocksFrom(0), 3);
    EXPECT_EQ(editor->runNotebookBlocksFrom(note.size()), 0);
}

TEST(NotebookBlockTest, BlocksThatReadTheSameAskWithTheirOwnOrdinalAndTheCodeUnchanged)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n\n```python\nprint(2)\n```\n\n"
                                        "```python\nprint(1)\n```\n\n```Python\nprint(1)\n```\n\n"
                                        "```bash\nprint(1)\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(30);
    EXPECT_EQ(editor->runAllNotebookBlocks(), 5);
    ASSERT_EQ(fake.runs.size(), 5);
    QList<int> ordinals;
    for (const auto &options : std::as_const(fake.runs))
        ordinals << options.duplicateOrdinal;
    EXPECT_EQ(ordinals, (QList<int>{0, 0, 1, 2, 0})) << "the language is part of what reads the same, its case is not";
    EXPECT_EQ(fake.runSources, (QStringList{QStringLiteral("print(1)"), QStringLiteral("print(2)"), QStringLiteral("print(1)"),
                                            QStringLiteral("print(1)"), QStringLiteral("print(1)")}));
}

namespace {

/// `count` fenced python blocks `x = N`, a blank line between them: far more than a screen.
QString manyBlocks(int count)
{
    QString note;
    for (int i = 0; i < count; ++i)
        note += QStringLiteral("```python\nx = %1\n```\n\n").arg(i);
    return note;
}

} // namespace

// A notebook is longer than its window: what is laid out is the view and a screen either side, and "all"
// means all, not the ones that happened to be laid out.
TEST(NotebookBlockTest, RunningAndClearingActOnEveryBlockOfALongNoteLaidOutOrNot)
{
    const QString note = manyBlocks(200);
    auto editor = parsedEditorWith(note);
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(note.size()); // the end is on screen; the first blocks are far above it
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum());
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    ASSERT_FALSE(editor->notebookInfoAt(note.indexOf(QStringLiteral("x = 0\n"))).eligible) << "not laid out: the premise";

    EXPECT_EQ(editor->runAllNotebookBlocks(), 200);
    EXPECT_EQ(fake.runs.size(), 200);
    QTest::qWait(50); // the runs finish: a block still running is interrupted by a second ask
    fake.runs.clear();
    fake.runSources.clear();
    EXPECT_EQ(editor->runNotebookBlocksFrom(note.indexOf(QStringLiteral("x = 100\n"))), 100);
    ASSERT_EQ(fake.runSources.size(), 100);
    EXPECT_EQ(fake.runSources.first(), QStringLiteral("x = 100"));
    EXPECT_EQ(fake.runSources.last(), QStringLiteral("x = 199"));
    QTest::qWait(50);
    fake.runs.clear();
    EXPECT_EQ(editor->runNotebookBlocksUpTo(note.indexOf(QStringLiteral("x = 49\n"))), 50);
    EXPECT_EQ(fake.runs.size(), 50);

    editor->clearNotebookOutputs();
    QTest::qWait(50);
    EXPECT_EQ(fake.clears.size(), 200);
}

// Laying the far blocks out must not move what is on screen.
TEST(NotebookBlockTest, RunningEveryBlockOfALongNoteLeavesTheViewWhereItWas)
{
    const QString note = manyBlocks(200);
    auto editor = parsedEditorWith(note);
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(note.size());
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum());
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    const qsizetype shown = note.indexOf(QStringLiteral("x = 199\n"));
    ASSERT_TRUE(editor->notebookInfoAt(shown).eligible);
    const qreal before = editor->notebookInfoAt(shown).runRect.top();
    EXPECT_EQ(editor->runAllNotebookBlocks(), 200);
    QTest::qWait(50);
    EXPECT_EQ(editor->notebookInfoAt(shown).runRect.top(), before);
}

// A fenced example inside a markdown cell (here quoted) is not a cell of the notebook, so it is not one of
// the blocks that read the same: the cell after it is the first.
// An edit above can make a block read the same as an earlier one without this block being laid out again: it
// still runs under the key its place in the note gives it now.
TEST(NotebookBlockTest, ABlockRunsWithTheOrdinalItsPlaceGivesItAfterAnEditAbove)
{
    const QString note = QStringLiteral("```python\nprint(1)\n```\n\n```python\nprint(2)\n```\n");
    auto editor = parsedEditorWith(note);
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(note.size());
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(30);
    editor->applyExternalEdit(note.indexOf(QStringLiteral("print(1)")) + 6, 1, QStringLiteral("2")); // print(2) twice now
    const qsizetype second = editor->noteText().lastIndexOf(QStringLiteral("print(2)"));
    ASSERT_TRUE(editor->runNotebookBlockAt(second));
    ASSERT_EQ(fake.runs.size(), 1);
    EXPECT_EQ(fake.runs.constFirst().duplicateOrdinal, 1);
}

TEST(NotebookBlockTest, AFenceNestedInAQuoteIsNotCountedAsABlockThatReadsTheSame)
{
    const QString note = QStringLiteral("> ```python\n> print(1)\n> ```\n\n```python\nprint(1)\n```\n");
    auto editor = parsedEditorWith(note);
    ASSERT_NE(editor, nullptr);
    editor->setCursorOffset(note.size());
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(30);
    const qsizetype cell = note.lastIndexOf(QStringLiteral("print(1)"));
    ASSERT_TRUE(editor->runNotebookBlockAt(cell));
    ASSERT_EQ(fake.runs.size(), 1);
    EXPECT_EQ(fake.runs.constFirst().duplicateOrdinal, 0);
}

TEST(NotebookBlockTest, ClickingARunningBlockAsksForAnInterrupt)
{
    const QString note = QStringLiteral("```python\nwhile True: pass\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size()); // out of the block: it renders, it does not show its source
    FakeNotebook fake;
    fake.holdRuns = true;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(30);
    QStringList asked;
    QObject::connect(editor.get(), &MarkdownEditor::notebookInterruptRequested,
                     [&asked](const QString &language) { asked.append(language); });
    ASSERT_TRUE(editor->runNotebookBlockAt(12));
    EXPECT_TRUE(editor->notebookInfoAt(12).running);
    EXPECT_TRUE(asked.isEmpty());
    ASSERT_TRUE(editor->runNotebookBlockAt(12)); // the stop control
    EXPECT_EQ(asked, QStringList{QStringLiteral("python")});
    EXPECT_EQ(fake.runs.size(), 1);
}

// input() from a running block: the kernel asks through options.onInput, the
// block grows a prompt row with a line edit, Return answers, Esc cancels.
namespace {
struct InputRun {
    MarkdownEditor::NotebookRunOptions options;
    MarkdownEditor::NotebookDeliver deliver;
};

/// A runner that holds every real run open, keeping what it was given to ask with.
MarkdownEditor::NotebookRunner holdingRunner(std::shared_ptr<InputRun> run)
{
    return [run](const QString &, const QString &, bool forceRun, const MarkdownEditor::NotebookRunOptions &options,
                 const MarkdownEditor::NotebookDeliver &deliver) {
        if (forceRun) {
            run->options = options;
            run->deliver = deliver;
            return;
        }
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult()); });
    };
}
} // namespace

TEST(NotebookBlockTest, InputShowsAPromptRowAnsweredByReturnAndCancelledByEsc)
{
    const QString note = QStringLiteral("```python\nname = input('Name: ')\n```\n\nafter\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size());
    auto run = std::make_shared<InputRun>();
    editor->setNotebookRunner(holdingRunner(run));
    QTest::qWait(30);
    ASSERT_TRUE(editor->runNotebookBlockAt(12));
    QTest::qWait(30);
    const qreal running = editor->documentHeight(); // "Running..." is under the block
    ASSERT_TRUE(bool(run->options.onInput)) << "a real run carries the means to ask";
    EXPECT_FALSE(editor->notebookInputInfo().active);

    std::optional<std::pair<bool, QString>> answer;
    run->options.onInput({QStringLiteral("Name: "), false},
                         [&answer](bool ok, const QString &line) { answer = std::pair{ok, line}; });
    QTest::qWait(30); // the editor takes it on its own thread
    auto info = editor->notebookInputInfo();
    ASSERT_TRUE(info.active);
    EXPECT_EQ(info.prompt, QStringLiteral("Name: "));
    EXPECT_TRUE(info.visible);
    EXPECT_GT(editor->documentHeight(), running);
    QLineEdit *edit = editor->notebookInputEdit();
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->echoMode(), QLineEdit::Normal);
    EXPECT_TRUE(editor->viewport()->rect().contains(edit->geometry())) << "it sits in the block's row, on screen";
    EXPECT_FALSE(answer.has_value());

    QTest::keyClicks(edit, QStringLiteral("Ada"));
    QTest::keyClick(edit, Qt::Key_Return);
    ASSERT_TRUE(answer.has_value());
    EXPECT_TRUE(answer->first);
    EXPECT_EQ(answer->second, QStringLiteral("Ada"));
    EXPECT_FALSE(editor->notebookInputInfo().active);
    EXPECT_EQ(editor->notebookInputEdit(), nullptr);
    EXPECT_DOUBLE_EQ(editor->documentHeight(), running) << "the prompt row is gone, 'Running...' is left";
    EXPECT_EQ(editor->noteText(), note) << "Return answered the prompt, it was not typed into the note";

    // Esc: the run is interrupted, not answered; getpass masks.
    answer.reset();
    run->options.onInput({QStringLiteral("Secret: "), true},
                         [&answer](bool ok, const QString &line) { answer = std::pair{ok, line}; });
    QTest::qWait(30);
    edit = editor->notebookInputEdit();
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->echoMode(), QLineEdit::Password);
    QTest::keyClicks(edit, QStringLiteral("hunter2"));
    QTest::keyClick(edit, Qt::Key_Escape);
    ASSERT_TRUE(answer.has_value());
    EXPECT_FALSE(answer->first);
    EXPECT_FALSE(editor->notebookInputInfo().active);
}

TEST(NotebookBlockTest, APromptEndsWhenItsRunDoesAndFollowsItsBlockWhenScrolled)
{
    QString note;
    for (int i = 0; i < 60; ++i)
        note += QStringLiteral("filler line %1\n\n").arg(i);
    note += QStringLiteral("```python\nx = input('go? ')\n```\n");
    for (int i = 0; i < 60; ++i)
        note += QStringLiteral("\ntrailing line %1\n").arg(i);
    auto editor = editorWith(note);
    editor->setCursorOffset(0);
    auto run = std::make_shared<InputRun>();
    editor->setNotebookRunner(holdingRunner(run));
    QTest::qWait(30);
    const qsizetype at = note.indexOf(QStringLiteral("x = input"));
    editor->setCursorOffset(0); // away from the block, which would show its source
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum() / 2);
    QTest::qWait(30);
    ASSERT_TRUE(editor->notebookInfoAt(at).eligible) << "the block is laid out where the scroll bar put us";
    ASSERT_TRUE(editor->runNotebookBlockAt(at));
    editor->verticalScrollBar()->setValue(0); // and off screen again
    int replies = 0;
    run->options.onInput({QStringLiteral("go? "), false}, [&replies](bool, const QString &) { ++replies; });
    QTest::qWait(30);
    ASSERT_TRUE(editor->notebookInputInfo().active);
    EXPECT_FALSE(editor->notebookInputInfo().visible) << "its block is below the screen";

    int bestValue = -1;
    for (int value = 0; value <= editor->verticalScrollBar()->maximum(); value += 20) {
        editor->verticalScrollBar()->setValue(value);
        if (editor->notebookInputInfo().visible) {
            bestValue = value;
            break;
        }
    }
    ASSERT_GE(bestValue, 0) << "scrolling to the block shows the prompt";
    const int top = editor->notebookInputInfo().editRect.top();
    editor->verticalScrollBar()->setValue(bestValue + 10);
    EXPECT_EQ(editor->notebookInputInfo().editRect.top(), top - 10) << "the edit moves with the block";

    // The run ends (finished, interrupted, kernel gone): the prompt goes, unanswered.
    run->deliver(MarkdownEditor::NotebookRunResult());
    QTest::qWait(30);
    EXPECT_FALSE(editor->notebookInputInfo().active);
    EXPECT_EQ(editor->notebookInputEdit(), nullptr);
    EXPECT_EQ(replies, 0);
}

TEST(NotebookBlockTest, APromptForARunThatIsNoLongerCurrentIsCancelled)
{
    const QString note = QStringLiteral("```python\ninput()\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size());
    auto run = std::make_shared<InputRun>();
    editor->setNotebookRunner(holdingRunner(run));
    QTest::qWait(30);
    ASSERT_TRUE(editor->runNotebookBlockAt(12));
    const auto stale = run->options.onInput;
    run->deliver(MarkdownEditor::NotebookRunResult()); // the run finished before the question got here
    QTest::qWait(30);
    std::optional<bool> ok;
    stale({QStringLiteral("late? "), false}, [&ok](bool answered, const QString &) { ok = answered; });
    QTest::qWait(30);
    ASSERT_TRUE(ok.has_value());
    EXPECT_FALSE(*ok);
    EXPECT_FALSE(editor->notebookInputInfo().active);
}

TEST(NotebookBlockTest, MoreScriptTypesAreRunnableEmbeds)
{
    QTemporaryDir dir;
    for (const char *name : {"a.c", "b.cpp", "c.hs", "d.java", "e.lua", "f.pl"}) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x\n");
    }
    for (const char *name : {"a.c", "b.cpp", "c.hs", "d.java", "e.lua", "f.pl"}) {
        auto editor = editorWith(QStringLiteral("![[%1]]\n\nafter\n").arg(QString::fromLatin1(name)));
        const QString path = dir.filePath(QString::fromLatin1(name));
        editor->setLinkResolver([path](const QString &) { return path; });
        FakeNotebook fake;
        editor->setNotebookRunner(fake.runner());
        editor->setCursorOffset(editor->noteText().size());
        QTest::qWait(20);
        EXPECT_TRUE(editor->notebookInfoAt(0).eligible) << name;
    }
}

TEST(PdfPageTest, AvailableAndRefusesWhatIsNotAPdf)
{
    // Available exactly when the build found poppler-qt6; a build without it -
    // a permissive one, or one for a phone - reports itself unavailable rather
    // than pretending. The rest holds either way.
#ifdef NOTEAPPC_EXPECT_PDF
    EXPECT_TRUE(PdfPage::isAvailable());
#else
    EXPECT_FALSE(PdfPage::isAvailable());
#endif
    EXPECT_EQ(PdfPage::pageCount(QStringLiteral("/nonexistent/none.pdf")), 0);
    EXPECT_TRUE(PdfPage::render(QStringLiteral("/nonexistent/none.pdf"), 0, 400).isNull());
    // A real file that is not a PDF must not crash the renderer either.
    EXPECT_EQ(PdfPage::pageCount(QStringLiteral("/etc/hostname")), 0);
}

#ifdef NOTEAPPC_EXPECT_PDF
TEST(PdfPageTest, PageSizeReadsDimensionsWithoutRendering)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = makeTestPdf(dir.path(), 2);
    const QSizeF size = PdfPage::pageSize(path, 0);
    EXPECT_GT(size.width(), 0);
    EXPECT_GT(size.height(), 0);
    EXPECT_TRUE(PdfPage::pageSize(path, 99).isEmpty()) << "out of range";
    EXPECT_EQ(PdfPage::pageCount(path), 2);
}
#endif

// --- PdfArgs: the fragment grammar, one form per test ----------------------

TEST(PdfArgsTest, NoArgumentsIsTheWholeDocumentAsSlides)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QString());
    EXPECT_TRUE(args.ok());
    EXPECT_FALSE(args.pagesGiven);
    EXPECT_FALSE(args.allPages);
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Slides);

    const QList<int> pages = resolvePdfPages(args, 5);
    EXPECT_EQ(pages, (QList<int>{0, 1, 2, 3, 4}));
}

TEST(PdfArgsTest, SinglePage)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=3"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{2}));
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Column) << "pages listed default to column";
}

TEST(PdfArgsTest, SeveralPages)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=2,5,9"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{1, 4, 8}));
}

TEST(PdfArgsTest, ARange)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=3-7"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{2, 3, 4, 5, 6}));
}

TEST(PdfArgsTest, MixedListAndRange)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1,3-5,9"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.pages, (QList<int>{0, 2, 3, 4, 8}));
}

TEST(PdfArgsTest, AllPages)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=all"));
    ASSERT_TRUE(args.ok());
    EXPECT_TRUE(args.allPages);
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Column);
    EXPECT_EQ(resolvePdfPages(args, 4), (QList<int>{0, 1, 2, 3}));
}

TEST(PdfArgsTest, HeightAloneKeepsTheSlidesDefault)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#height=400"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.height, 400);
    EXPECT_FALSE(args.pagesGiven);
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Slides);
}

TEST(PdfArgsTest, ExplicitLayoutOverridesTheDefault)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1,2&layout=slides"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Slides);
}

TEST(PdfArgsTest, GridWithColumns)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#layout=grid&columns=3"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(args.layout, PdfEmbedArgs::Layout::Grid);
    EXPECT_EQ(args.columns, 3);
}

TEST(PdfArgsTest, BadPageNumberErrors)
{
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#page=abc")).ok());
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#page=0")).ok()) << "pages start at 1";
}

TEST(PdfArgsTest, ReversedRangeErrors)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=7-3"));
    EXPECT_FALSE(args.ok());
    EXPECT_FALSE(args.error.isEmpty());
}

TEST(PdfArgsTest, BadHeightErrors)
{
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#height=big")).ok());
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#height=0")).ok());
}

TEST(PdfArgsTest, UnknownLayoutErrors)
{
    EXPECT_FALSE(parsePdfEmbedArgs(QStringLiteral("#layout=carousel")).ok());
}

TEST(PdfArgsTest, UnknownKeyIsIgnoredNotAnError)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1&zoom=150"));
    EXPECT_TRUE(args.ok());
}

TEST(PdfArgsTest, ResolvePdfPagesDropsOutOfRangeRatherThanClamping)
{
    const PdfEmbedArgs args = parsePdfEmbedArgs(QStringLiteral("#page=1,5"));
    ASSERT_TRUE(args.ok());
    EXPECT_EQ(resolvePdfPages(args, 3), (QList<int>{0})) << "page 5 of a 3-page document is dropped";
}

// --- PDF embeds in the editor: layout, async render, slides nav ------------

#ifdef NOTEAPPC_EXPECT_PDF

TEST(PdfEmbedTest, ColumnLayoutShowsEveryRequestedPage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 6);

    auto editor = editorWith(QStringLiteral("![[report.pdf#page=1,3-5]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    const MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_TRUE(info.error.isEmpty()) << info.error.toStdString();
    EXPECT_EQ(info.pageCount, 6);
    EXPECT_EQ(info.layout, PdfEmbedArgs::Layout::Column);
    EXPECT_EQ(info.cellCount, 4) << "1, 3, 4, 5";
}

TEST(PdfEmbedTest, SlidesLayoutShowsOnePageAndStepsWithPdfSlideStep)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 4);

    auto editor = editorWith(QStringLiteral("![[deck.pdf]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_EQ(info.layout, PdfEmbedArgs::Layout::Slides);
    EXPECT_EQ(info.cellCount, 1);
    EXPECT_EQ(info.slideIndex, 0);
    EXPECT_EQ(info.slideCount, 4);

    editor->pdfSlideStep(0, 1);
    info = editor->pdfEmbedInfoAt(0);
    EXPECT_EQ(info.slideIndex, 1);

    editor->pdfSlideStep(0, -1);
    editor->pdfSlideStep(0, -1); // clamps at 0, does not wrap
    info = editor->pdfEmbedInfoAt(0);
    EXPECT_EQ(info.slideIndex, 0);
}

namespace {

/// A slides embed of a 4-page PDF in a fresh editor, with a clock the test
/// moves by hand. `clickAt` is a real press and release on the viewport.
struct PdfNavFixture {
    QTemporaryDir dir;
    std::unique_ptr<MarkdownEditor> editor;
    qint64 nowMs = 100000;

    explicit PdfNavFixture(int pages = 4)
    {
        const QString pdf = makeTestPdf(dir.path(), pages);
        editor = editorWith(QStringLiteral("![[deck.pdf]]\n"));
        editor->setLinkResolver([pdf](const QString &) { return pdf; });
        editor->setPdfNavClock([this] { return nowMs; });
    }
    MarkdownEditor::PdfEmbedInfo info() const { return editor->pdfEmbedInfoAt(0); }
    /// A click `dx` / `dy` from the slide's bottom-right corner (negative: inside).
    void clickAt(qreal dx, qreal dy)
    {
        const QRectF cell = info().cellRect;
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPointF(cell.right() + dx, cell.bottom() + dy).toPoint());
    }
    /// Whether the click that just happened reached the caret: the embed turned into its source.
    bool openedSource() const { return !info().isPdfEmbed; }
};

} // namespace

TEST(PdfEmbedTest, ClickJustAboveNextStepsInsteadOfOpeningTheSource)
{
    PdfNavFixture f;
    ASSERT_EQ(f.info().cellCount, 1);
    // The nav row starts at the slide's bottom edge, 6 px above the button's own rect.
    EXPECT_NEAR(f.info().nextRect.top() - f.info().cellRect.bottom(), 6, 0.5);
    f.clickAt(-20, 2);
    EXPECT_EQ(f.info().slideIndex, 1);
    EXPECT_FALSE(f.openedSource());
}

TEST(PdfEmbedTest, PreviousIsTheLeftHalfOfTheRow)
{
    PdfNavFixture f;
    f.editor->pdfSlideStep(0, 1);
    f.editor->pdfSlideStep(0, 1);
    ASSERT_EQ(f.info().slideIndex, 2);
    f.clickAt(-f.info().cellRect.width() + 200, 20); // well right of the "< Previous" label
    EXPECT_EQ(f.info().slideIndex, 1);
}

TEST(PdfEmbedTest, ClickOnAHiddenButtonsHalfDoesNothing)
{
    PdfNavFixture f(2);
    f.clickAt(-20, 10); // Next: slide 2 of 2
    ASSERT_EQ(f.info().slideIndex, 1);
    f.clickAt(-20, 10); // Next again, button hidden on the last slide
    EXPECT_EQ(f.info().slideIndex, 1);
    EXPECT_FALSE(f.openedSource());
    f.clickAt(-f.info().cellRect.width() + 20, 10); // Previous
    ASSERT_EQ(f.info().slideIndex, 0);
    f.clickAt(-f.info().cellRect.width() + 20, 10); // hidden Previous on the first slide
    EXPECT_EQ(f.info().slideIndex, 0);
    EXPECT_FALSE(f.openedSource());
}

TEST(PdfEmbedTest, DeadBandAlongTheSlidesBottomEdgeOpensNothing)
{
    PdfNavFixture f;
    f.clickAt(-20, -5);
    EXPECT_EQ(f.info().slideIndex, 0);
    EXPECT_FALSE(f.openedSource());
    // Past the band the slide is an ordinary click again.
    f.clickAt(-20, -40);
    EXPECT_TRUE(f.openedSource());
}

TEST(PdfEmbedTest, JustUsedButtonKeepsTheSlidesBottomQuarterForOneSecond)
{
    PdfNavFixture f;
    f.clickAt(-20, 10); // Next
    ASSERT_EQ(f.info().slideIndex, 1);
    const qreal quarterUp = -f.info().cellRect.height() * 0.2; // inside the bottom 25%
    f.nowMs += 900;
    f.clickAt(-20, quarterUp);
    EXPECT_EQ(f.info().slideIndex, 2) << "the pointer drifted up, the button still owns it";
    EXPECT_FALSE(f.openedSource());
    // The other side of the slide was never the button's.
    f.nowMs += 500;
    f.clickAt(-f.info().cellRect.width() + 20, quarterUp);
    EXPECT_TRUE(f.openedSource());
}

TEST(PdfEmbedTest, TheBottomQuarterIsOrdinaryOnceTheWindowHasPassed)
{
    PdfNavFixture f;
    f.clickAt(-20, 10);
    ASSERT_EQ(f.info().slideIndex, 1);
    f.nowMs += 1500;
    f.clickAt(-20, -f.info().cellRect.height() * 0.2);
    EXPECT_TRUE(f.openedSource()) << "an ordinary click on the slide, not a third step";
}

TEST(PdfEmbedTest, TheEnlargedButtonAreasShowAPointingHand)
{
    PdfNavFixture f(2);
    const auto shapeAt = [&f](qreal dx, qreal dy) {
        const QRectF cell = f.info().cellRect;
        const QPointF at(cell.right() + dx, cell.bottom() + dy); // QTest::mouseMove needs an exposed window
        QMouseEvent move(QEvent::MouseMove, at, f.editor->viewport()->mapToGlobal(at), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(f.editor->viewport(), &move);
        return f.editor->viewport()->cursor().shape();
    };
    EXPECT_EQ(shapeAt(-20, 2), Qt::PointingHandCursor) << "beside the Next label, still the button";
    EXPECT_EQ(shapeAt(-20, -5), Qt::ArrowCursor) << "the dead band swallows the click";
    EXPECT_EQ(shapeAt(-f.info().cellRect.width() + 20, 10), Qt::ArrowCursor)
        << "Previous is hidden on the first slide";
}

TEST(PdfEmbedTest, GridLayoutRespectsColumns)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 5);

    auto editor = editorWith(QStringLiteral("![[deck.pdf#page=all&layout=grid&columns=2]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    const MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_EQ(info.layout, PdfEmbedArgs::Layout::Grid);
    EXPECT_EQ(info.cellCount, 5);
}

TEST(PdfEmbedTest, BadArgumentShowsAnErrorInsteadOfCrashing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 2);

    auto editor = editorWith(QStringLiteral("![[deck.pdf#page=9-3]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });

    const MarkdownEditor::PdfEmbedInfo info = editor->pdfEmbedInfoAt(0);
    ASSERT_TRUE(info.isPdfEmbed);
    EXPECT_FALSE(info.error.isEmpty());
    EXPECT_EQ(info.cellCount, 0);
}


// --- zooming a PDF embed -----------------------------------------------------

namespace {

/// One PDF embed (`#height=300` unless the note says otherwise) in an editor
/// with the pointer events a user would send: Ctrl+wheel, shift+wheel, drags.
struct PdfZoomFixture {
    QTemporaryDir dir;
    std::unique_ptr<MarkdownEditor> editor;
    qsizetype offset = 0; ///< where the first embed starts in the note

    explicit PdfZoomFixture(const QString &note = QStringLiteral("intro\n\n![[deck.pdf#height=300]]\n"),
                            int pages = 4)
    {
        const QString pdf = makeTestPdf(dir.path(), pages);
        editor = editorWith(note);
        // Shown, so the viewport is the 900x600 asked for: hidden, it keeps the
        // 640x480 default, which a note this tall only just fits - a scrollbar
        // appearing (the empty line after the final newline counts) changes the
        // column width under a raster that was drawn for the other one.
        editor->show();
        editor->setLinkResolver([pdf](const QString &) { return pdf; });
        editor->setPdfNavClock([] { return qint64(100000); });
        offset = note.indexOf(QLatin1String("![["));
    }
    MarkdownEditor::PdfEmbedInfo info(qsizetype at = -1) const
    {
        return editor->pdfEmbedInfoAt(at < 0 ? offset : at);
    }
    /// A point `dx`, `dy` inside the first embed's cell, viewport coordinates.
    QPoint inCell(qreal dx, qreal dy, qsizetype at = -1) const
    {
        return (info(at).cellRect.topLeft() + QPointF(dx, dy)).toPoint();
    }
    bool wheel(const QPoint &at, int dx, int dy, Qt::KeyboardModifiers modifiers)
    {
        QWheelEvent event(QPointF(at), QPointF(at), QPoint(0, 0), QPoint(dx, dy), Qt::NoButton, modifiers,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(editor->viewport(), &event);
        return event.isAccepted();
    }
    void zoom(const QPoint &at, int notches)
    {
        for (int i = 0; i < qAbs(notches); ++i)
            wheel(at, 0, notches > 0 ? 120 : -120, Qt::ControlModifier);
    }
    /// What a real double click sends: the first click (a widget's QTest::mouseDClick sends
    /// only the double-click event itself), then that event, then the last release.
    void doubleClick(const QPoint &at)
    {
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseDClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseRelease(editor->viewport(), Qt::LeftButton, Qt::NoModifier, at);
    }
    void move(const QPoint &to)
    {
        QMouseEvent event(QEvent::MouseMove, QPointF(to), editor->viewport()->mapToGlobal(QPointF(to)),
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(editor->viewport(), &event);
    }
};

/// The page content under `at`, in the unzoomed cell's pixels: what a zoom
/// anchored at `at` must leave alone.
QPointF contentUnder(const MarkdownEditor::PdfEmbedInfo &info, const QPoint &at)
{
    return info.pan + (QPointF(at) - info.cellRect.topLeft()) / info.zoom;
}

QString paddedNote(const QString &embed)
{
    return embed + QStringLiteral("\n\n") + QStringLiteral("filler line\n\n").repeated(200);
}

} // namespace

TEST(PdfEmbedTest, CtrlWheelZoomsAnchoredAtPointer)
{
    PdfZoomFixture f;
    const QPoint at = f.inCell(300, 150);
    const MarkdownEditor::PdfEmbedInfo before = f.info();
    ASSERT_EQ(before.zoom, 1);
    EXPECT_TRUE(f.wheel(at, 0, 120, Qt::ControlModifier));
    const MarkdownEditor::PdfEmbedInfo after = f.info();
    EXPECT_NEAR(after.zoom, 1.1, 1e-6);
    EXPECT_NEAR(contentUnder(after, at).x(), contentUnder(before, at).x(), 0.5);
    EXPECT_NEAR(contentUnder(after, at).y(), contentUnder(before, at).y(), 0.5);
    EXPECT_EQ(after.cellRect, before.cellRect) << "the cell is a fixed window, so the note never reflows";
    EXPECT_EQ(after.nextRect, before.nextRect);
}

TEST(PdfEmbedTest, PlainWheelScrollsTheNoteOverAZoomedEmbed)
{
    PdfZoomFixture f(paddedNote(QStringLiteral("![[deck.pdf#height=300]]")));
    const QPoint at = f.inCell(300, 150);
    f.zoom(at, 7);
    const MarkdownEditor::PdfEmbedInfo zoomed = f.info();
    ASSERT_GT(zoomed.zoom, 1.5);
    ASSERT_EQ(f.editor->verticalScrollBar()->value(), 0);
    f.wheel(at, 0, -120, Qt::NoModifier);
    EXPECT_GT(f.editor->verticalScrollBar()->value(), 0) << "an embed must never leave the page stuck";
    EXPECT_EQ(f.info(f.offset).pan, zoomed.pan);
}

TEST(PdfEmbedTest, ShiftWheelPansInsideAndClampsAtTheEdge)
{
    PdfZoomFixture f(paddedNote(QStringLiteral("![[deck.pdf#height=300]]")));
    const QPoint at = f.inCell(300, 150);
    f.zoom(at, 7);
    const qreal zoom = f.info().zoom;
    ASSERT_GT(zoom, 1.5);
    const qreal before = f.info().pan.y();
    EXPECT_TRUE(f.wheel(at, 0, -120, Qt::ShiftModifier));
    EXPECT_GT(f.info().pan.y(), before);
    EXPECT_EQ(f.editor->verticalScrollBar()->value(), 0);
    for (int i = 0; i < 50; ++i)
        EXPECT_TRUE(f.wheel(at, 0, -120, Qt::ShiftModifier)) << "an edge still keeps the wheel";
    EXPECT_NEAR(f.info().pan.y(), 300 - 300 / zoom, 1e-6);
    EXPECT_EQ(f.editor->verticalScrollBar()->value(), 0) << "the edge does not chain to the note";
}

TEST(PdfEmbedTest, HorizontalWheelPansSideways)
{
    PdfZoomFixture f;
    const QPoint at = f.inCell(300, 150);
    f.zoom(at, 7);
    const qreal before = f.info().pan.x();
    EXPECT_TRUE(f.wheel(at, 120, 0, Qt::NoModifier));
    EXPECT_LT(f.info().pan.x(), before);
}

TEST(PdfEmbedTest, DragPansAndNeitherMovesTheCaretNorRevealsSource)
{
    PdfZoomFixture f;
    const QPoint at = f.inCell(300, 150);
    f.zoom(at, 7);
    const MarkdownEditor::PdfEmbedInfo zoomed = f.info();
    const qsizetype caret = f.editor->cursorOffset();
    QTest::mousePress(f.editor->viewport(), Qt::LeftButton, Qt::NoModifier, at);
    f.move(at + QPoint(30, 30));
    QTest::mouseRelease(f.editor->viewport(), Qt::LeftButton, Qt::NoModifier, at + QPoint(30, 30));
    const MarkdownEditor::PdfEmbedInfo panned = f.info();
    EXPECT_NEAR(panned.pan.x(), zoomed.pan.x() - 30 / zoomed.zoom, 1e-6);
    EXPECT_NEAR(panned.pan.y(), zoomed.pan.y() - 30 / zoomed.zoom, 1e-6);
    EXPECT_EQ(f.editor->cursorOffset(), caret);
    EXPECT_TRUE(panned.isPdfEmbed) << "a revealed block is laid out as source";
    EXPECT_EQ(panned.cellCount, 1);

    // A click that does not move is no pan, and no way to the source either.
    QTest::mouseClick(f.editor->viewport(), Qt::LeftButton, Qt::NoModifier, at);
    EXPECT_TRUE(f.info().isPdfEmbed);
    EXPECT_EQ(f.editor->cursorOffset(), caret);
    EXPECT_EQ(f.info().pan, panned.pan);
}

TEST(PdfEmbedTest, ClickOnAnUnzoomedPageIsTheCaretAsBefore)
{
    PdfZoomFixture f;
    QTest::mouseClick(f.editor->viewport(), Qt::LeftButton, Qt::NoModifier, f.inCell(300, 150));
    EXPECT_FALSE(f.info().isPdfEmbed) << "the click revealed the source";
    EXPECT_GE(f.editor->cursorOffset(), f.offset);
}

TEST(PdfEmbedTest, DoubleClickTogglesTwoXAndBack)
{
    PdfZoomFixture f;
    const QPoint at = f.inCell(300, 150);
    const MarkdownEditor::PdfEmbedInfo before = f.info();
    const qsizetype caret = f.editor->cursorOffset();
    f.doubleClick(at);
    MarkdownEditor::PdfEmbedInfo info = f.info();
    ASSERT_TRUE(info.isPdfEmbed) << "the click that began the double click revealed the source; it is undone";
    EXPECT_NEAR(info.zoom, 2, 1e-6);
    EXPECT_NEAR(contentUnder(info, at).x(), contentUnder(before, at).x(), 0.5);
    EXPECT_NEAR(contentUnder(info, at).y(), contentUnder(before, at).y(), 0.5);
    EXPECT_EQ(f.editor->cursorOffset(), caret) << "zooming a page leaves the caret where it was";

    f.doubleClick(at);
    info = f.info();
    EXPECT_EQ(info.zoom, 1);
    EXPECT_EQ(info.pan, QPointF());
    EXPECT_TRUE(info.isPdfEmbed);
}

TEST(PdfEmbedTest, CtrlZeroResetsEveryZoomedEmbed)
{
    const QString note = QStringLiteral("![[a.pdf#height=300]]\n\n![[b.pdf#height=300]]\n");
    PdfZoomFixture f(note);
    f.editor->resize(900, 900);
    const qsizetype second = note.indexOf(QLatin1String("![[b"));
    f.zoom(f.inCell(300, 100), 5);
    f.zoom(f.inCell(300, 100, second), 5);
    ASSERT_GT(f.info(0).zoom, 1);
    ASSERT_GT(f.info(second).zoom, 1);
    QTest::keyClick(f.editor.get(), Qt::Key_0, Qt::ControlModifier);
    EXPECT_EQ(f.info(0).zoom, 1);
    EXPECT_EQ(f.info(second).zoom, 1);
}

TEST(PdfEmbedTest, ZoomShowsTheScaledRasterAtOnceAndRerendersAfterItSettles)
{
    PdfZoomFixture f;
    const int width = int(f.info().cellRect.width());
    WAIT_TRUE(f.info().renderedWidth == width, 3000);
    f.zoom(f.inCell(300, 150), 7);
    const qreal zoom = f.info().zoom;
    ASSERT_GT(zoom, 1.5);
    EXPECT_EQ(f.info().renderedWidth, width) << "the stretched base raster covers the wait";
    const int wanted = qMin(int(width * (qRound(zoom * 20) / 20.0)), 2600);
    WAIT_TRUE(f.info().renderedWidth == wanted, 3000);
}

TEST(PdfEmbedTest, ZoomSurvivesAnEditAbove)
{
    PdfZoomFixture f;
    f.zoom(f.inCell(300, 150), 7);
    const qreal zoom = f.info().zoom;
    ASSERT_GT(zoom, 1.5);
    f.editor->setCursorOffset(0);
    QTest::keyClicks(f.editor.get(), QStringLiteral("xy"));
    EXPECT_EQ(f.info(f.offset + 2).zoom, zoom) << "the embed moved down two characters";
}

TEST(PdfEmbedTest, SlideStepAndEditsKeepZoomAndSlide)
{
    PdfZoomFixture f(QStringLiteral("intro\n\n![[deck.pdf#height=300]]\n"), 4);
    f.zoom(f.inCell(300, 150), 7);
    const qreal zoom = f.info().zoom;
    f.editor->pdfSlideStep(f.offset, 1);
    EXPECT_EQ(f.info().slideIndex, 1);
    EXPECT_EQ(f.info().zoom, zoom) << "a deck keeps its zoom from slide to slide";

    f.editor->setCursorOffset(0);
    QTest::keyClicks(f.editor.get(), QStringLiteral("xy"));
    // A block below an edit keeps its layout until something lays it out again: a finished render does.
    const qsizetype movedAt = f.offset + 2;
    f.zoom(f.inCell(300, 150, movedAt), 1);
    WAIT_TRUE(f.info(movedAt).renderedWidth > int(f.info(movedAt).cellRect.width()), 3000);
    const MarkdownEditor::PdfEmbedInfo moved = f.info(movedAt);
    EXPECT_EQ(moved.slideIndex, 1) << "the slide an edit above would reset";
    EXPECT_GT(moved.zoom, zoom);
}

TEST(PdfEmbedTest, ZoomClampsToItsRange)
{
    PdfZoomFixture f;
    const QPoint at = f.inCell(300, 150);
    f.zoom(at, 40);
    EXPECT_EQ(f.info().zoom, 4);
    EXPECT_LE(f.info().pan.x(), f.info().cellRect.width() * 0.75 + 1e-6);
    EXPECT_LE(f.info().pan.y(), 300 * 0.75 + 1e-6);
    f.zoom(at, -40);
    EXPECT_EQ(f.info().zoom, 1);
    EXPECT_EQ(f.info().pan, QPointF());
}

TEST(PdfEmbedTest, ZoomedRastersStayUnderTheirBudget)
{
    // A short window on each page, but a raster of the whole page: they are big.
    PdfZoomFixture f(QStringLiteral("![[deck.pdf#height=300]]\n"), 5);
    f.editor->viewport()->grab(); // the first paint settles the width the pages are laid out at
    const qint64 raster = qint64(2600) * qint64(2600 * 1.4143) * 4; // the largest a page gets
    f.editor->setPdfZoomCacheBudget(raster * 3 / 2);
    f.zoom(f.inCell(300, 150), 40);
    for (int page = 0; page < 5; ++page) {
        SCOPED_TRACE(page);
        // A paint finds a short raster and asks for the zoomed one; the app paints on every delivery.
        const int wanted = qMin(int(f.info().cellRect.width()) * 4, 2600);
        WAIT_TRUE((f.editor->viewport()->grab(), f.info().renderedWidth == wanted), 5000);
        EXPECT_LE(f.editor->pdfZoomCacheBytes(), raster * 3 / 2);
        f.editor->pdfSlideStep(0, 1);
    }
    EXPECT_GT(f.editor->pdfZoomCacheBytes(), 0);
}

TEST(PdfEmbedTest, ClearingTheRasterCacheClearsTheZoomBudgetToo)
{
    PdfZoomFixture f(QStringLiteral("![[deck.pdf#height=300]]\n"), 2);
    f.editor->viewport()->grab();
    f.zoom(f.inCell(300, 150), 40);
    WAIT_TRUE((f.editor->viewport()->grab(), f.editor->pdfZoomCacheBytes() > 0), 5000);
    auto features = f.editor->features();
    features.maths = !features.maths; // any change empties the page cache
    f.editor->setFeatures(features);
    EXPECT_EQ(f.editor->pdfZoomCacheBytes(), 0) << "the bytes of rasters that are gone count against nothing";
}

TEST(PdfEmbedTest, AnotherNoteInTheSameEditorStartsUnzoomedOnItsFirstSlide)
{
    const QString note = QStringLiteral("![[deck.pdf#height=300]]\n");
    PdfZoomFixture f(note);
    f.zoom(f.inCell(300, 150), 7);
    f.editor->pdfSlideStep(0, 1);
    ASSERT_GT(f.info(0).zoom, 1);
    ASSERT_EQ(f.info(0).slideIndex, 1);
    f.editor->setNoteText(note);
    EXPECT_EQ(f.info(0).zoom, 1);
    EXPECT_EQ(f.info(0).slideIndex, 0);
}

TEST(PdfEmbedTest, NativeZoomGestureZoomsAnchored)
{
    PdfZoomFixture f;
    const QPoint at = f.inCell(300, 150);
    const MarkdownEditor::PdfEmbedInfo before = f.info();
    QNativeGestureEvent gesture(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, QPointF(at),
                                QPointF(at), f.editor->viewport()->mapToGlobal(QPointF(at)), 0.5, QPointF());
    QCoreApplication::sendEvent(f.editor->viewport(), &gesture);
    const MarkdownEditor::PdfEmbedInfo after = f.info();
    EXPECT_NEAR(after.zoom, 1.5, 1e-6);
    EXPECT_NEAR(contentUnder(after, at).x(), contentUnder(before, at).x(), 0.5);
    EXPECT_NEAR(contentUnder(after, at).y(), contentUnder(before, at).y(), 0.5);
}

TEST(PdfEmbedTest, SlideControlsWinOverPanWhileZoomed)
{
    PdfNavFixture f;
    const QRectF cell = f.info().cellRect;
    QWheelEvent zoom(cell.center(), cell.center(), QPoint(0, 0), QPoint(0, 120 * 7), Qt::NoButton,
                     Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(f.editor->viewport(), &zoom);
    const MarkdownEditor::PdfEmbedInfo zoomed = f.info();
    ASSERT_GT(zoomed.zoom, 1);
    f.clickAt(-20, 10); // Next, below the slide
    ASSERT_EQ(f.info().slideIndex, 1);
    f.nowMs += 500;
    // Inside the bottom quarter, where the button just used keeps the slide for a moment.
    f.clickAt(-20, -f.info().cellRect.height() * 0.2);
    EXPECT_EQ(f.info().slideIndex, 2) << "the controls come before a pan";
    EXPECT_EQ(f.info().pan, zoomed.pan);
    EXPECT_TRUE(f.info().isPdfEmbed);
}

#endif // NOTEAPPC_EXPECT_PDF

// --- .tex embeds -------------------------------------------------------------

TEST(TexCompilerTest, IsFullDocumentTellsMathsAloneApart)
{
    EXPECT_FALSE(TexCompiler::isFullDocument(QStringLiteral("E = mc^2")));
    EXPECT_TRUE(TexCompiler::isFullDocument(
        QStringLiteral("\\documentclass{article}\\begin{document}hi\\end{document}")));
}

TEST(TexCompilerTest, CompilesATinyDocumentAndCachesIt)
{
    if (!TexCompiler::isAvailable())
        GTEST_SKIP() << "no pdflatex on PATH";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}hello\\end{document}\n");
    }

    const TexCompiler::Result first = TexCompiler::compile(path);
    ASSERT_TRUE(first.ok) << first.log.toStdString() << first.error.toStdString();
    EXPECT_TRUE(QFile::exists(first.pdfPath));

    // Unchanged bytes hash to the same cache entry, so a second call answers
    // without running pdflatex again - observable here as the same path.
    const TexCompiler::Result second = TexCompiler::compile(path);
    EXPECT_EQ(second.pdfPath, first.pdfPath);
}

TEST(TexCompilerTest, ReportsAFailedCompile)
{
    if (!TexCompiler::isAvailable())
        GTEST_SKIP() << "no pdflatex on PATH";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("broken.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}\\undefinedcommand{x}\\end{document}\n");
    }

    const TexCompiler::Result result = TexCompiler::compile(path);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.log.isEmpty());
}

TEST(TexCompilerTest, NeverRunsWrite18)
{
    if (!TexCompiler::isAvailable())
        GTEST_SKIP() << "no pdflatex on PATH";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("pwned"));
    const QString path = dir.filePath(QStringLiteral("evil.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(("\\documentclass{article}\\begin{document}"
                    "\\immediate\\write18{touch " + marker.toUtf8() + "}"
                    "\\end{document}\n"));
    }

    (void)TexCompiler::compile(path);
    EXPECT_FALSE(QFile::exists(marker)) << "-no-shell-escape must have refused \\write18";
}

TEST(TexCompilerTest, ADocumentCannotReadAFileOutsideItsOwnDirectory)
{
    if (!TexCompiler::isAvailable() || !bubblewrapUsable())
        GTEST_SKIP() << "needs pdflatex and a usable bubblewrap (installed, user namespaces allowed)";

    // A file the user can read, somewhere a document has no business in.
    QTemporaryDir elsewhere;
    const QString secret = elsewhere.filePath(QStringLiteral("secret.tex"));
    {
        QFile file(secret);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("private\n");
    }
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nosy.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}\\input{" + secret.toUtf8()
                   + "}\\end{document}\n");
    }
    const TexCompiler::Result result = TexCompiler::compile(path);
    EXPECT_FALSE(result.ok) << "the sandbox must hide files outside the compile directory";
    EXPECT_TRUE(result.log.contains(QLatin1String("not found"))) << result.log.toStdString();
}

// The compile directory is the sandbox's to write, and the PDF is copied out of it by the app, as the user: a
// link standing in for it (made here by a stand-in engine; the real one cannot) must not be followed.
TEST(TexCompilerTest, AnOutputThatIsALinkToAUsersFileIsNotCached)
{
    if (!bubblewrapUsable())
        GTEST_SKIP() << "needs a usable bubblewrap (installed, user namespaces allowed)";
    QTemporaryDir bin;
    QTemporaryDir secretDir;
    QTemporaryDir dir;
    ASSERT_TRUE(bin.isValid() && secretDir.isValid() && dir.isValid());
    const QString secret = secretDir.filePath(QStringLiteral("secret.txt"));
    {
        QFile file(secret);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("TOP SECRET\n");
    }
    {
        QFile engine(bin.filePath(QStringLiteral("pdflatex")));
        ASSERT_TRUE(engine.open(QIODevice::WriteOnly));
        engine.write("#!/bin/sh\nln -s '" + secret.toUtf8() + "' doc.pdf\nexit 0\n");
        engine.close();
        engine.setPermissions(engine.permissions() | QFileDevice::ExeOwner);
    }
    const QByteArray path = qgetenv("PATH");
    qputenv("PATH", bin.path().toUtf8() + ":" + path);
    const QString tex = dir.filePath(QStringLiteral("linked.tex"));
    {
        QFile file(tex);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}a link, not a document\\end{document}\n");
    }
    const TexCompiler::Result result = TexCompiler::compile(tex);
    qputenv("PATH", path);
    EXPECT_FALSE(result.ok) << "the link was taken for the PDF";
    if (!result.pdfPath.isEmpty() && QFileInfo::exists(result.pdfPath)) {
        QFile cached(result.pdfPath);
        ASSERT_TRUE(cached.open(QIODevice::ReadOnly));
        EXPECT_FALSE(cached.readAll().contains("TOP SECRET")) << "the user's file is in the cache";
    }
}

TEST(TexCompilerTest, WithoutBubblewrapNothingIsCompiled)
{
    if (!TexCompiler::isAvailable())
        GTEST_SKIP() << "no pdflatex on PATH";
    // A PATH that has the engine and no bwrap: TeX is not run unsandboxed instead.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.filePath(QStringLiteral("bin"))));
    ASSERT_TRUE(QFile::link(TexCompiler::enginePath(), dir.filePath(QStringLiteral("bin/pdflatex"))));
    const QString path = dir.filePath(QStringLiteral("doc.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}no sandbox, no run\\end{document}\n");
    }
    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", dir.filePath(QStringLiteral("bin")).toUtf8());
    const TexCompiler::Result result = TexCompiler::compile(path);
    qputenv("PATH", oldPath);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QLatin1String("bubblewrap"))) << result.error.toStdString();
}

TEST(MarkdownEditorTest, TexMathsAloneRendersThroughMicroTeX)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("formula.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("E = mc^2");
    }

    auto editor = editorWith(QStringLiteral("![[formula.tex]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("formula.tex")));
    EXPECT_GT(editor->documentHeight(), 0);
}

// A document embed starts pdflatex: in a vault that is not trusted that waits for a click and a yes.
TEST(TexGateTest, ADocumentEmbedCompilesNothingUntilTheGateAnswersYes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}Alpha\\end{document}\n");
    }
    int looked = 0;
    int asked = 0;
    bool allowed = false;
    auto editor = editorWith(QStringLiteral("![[doc.tex]]\n\ntail\n"));
    editor->setProcessGate([&](const QString &language, const QString &source, bool ask) {
        EXPECT_EQ(language, QStringLiteral("tex"));
        EXPECT_TRUE(source.contains(QStringLiteral("Alpha"))) << "the question names what would run";
        EXPECT_TRUE(source.contains(path));
        (ask ? asked : looked)++;
        return allowed;
    });
    editor->setLinkResolver([&](const QString &) { return path; });
    editor->setCursorOffset(editor->noteText().size());
    WAIT_TRUE(!editor->codeBlockImageRectAt(0).isEmpty(), 3000);
    QTest::qWait(200);
    EXPECT_GT(looked, 0);
    EXPECT_EQ(asked, 0) << "laying the note out asks nothing";
    EXPECT_EQ(editor->texCompilesForTest(), 0) << "and starts nothing";

    const QRect placeholder = editor->codeBlockImageRectAt(0);
    const QPoint button = placeholder.bottomLeft() + QPoint(30, -22);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, button);
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(editor->texCompilesForTest(), 0) << "a no starts nothing";
    EXPECT_EQ(editor->codeBlockImageRectAt(0), placeholder);

    allowed = true;
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, button);
    EXPECT_EQ(asked, 2);
    WAIT_TRUE(editor->codeBlockImageRectAt(0).isEmpty(), 3000); // the source panel is gone
    if (TexCompiler::isAvailable())
        EXPECT_EQ(editor->texCompilesForTest(), 1) << "the yes compiled it";
}

TEST(TexGateTest, AnEditorWithNoGateCompilesNothing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("\\documentclass{article}\\begin{document}Alpha\\end{document}\n");
    }
    // A hover preview is such an editor: nobody hands it a gate.
    auto editor = editorWith(QStringLiteral("![[doc.tex]]\n\ntail\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    editor->setCursorOffset(editor->noteText().size());
    WAIT_TRUE(!editor->codeBlockImageRectAt(0).isEmpty(), 3000);
    QTest::qWait(200);
    EXPECT_EQ(editor->texCompilesForTest(), 0);
}

TEST(TexGateTest, MathsAloneNeedsNoGate)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("formula.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("E = mc^2");
    }
    auto editor = editorWith(QStringLiteral("![[formula.tex]]\n"));
    editor->setProcessGate([](const QString &, const QString &, bool) {
        ADD_FAILURE() << "MicroTeX runs in this process: nothing to ask";
        return false;
    });
    editor->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(editor->codeBlockImageRectAt(0).isEmpty());
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("formula.tex"))) << "drawn, not left as the link";
    EXPECT_GT(editor->documentHeight(), 0);
}

// A vault is untrusted: an embed of a file of any size reads at most what opening it would.
TEST(EmbedSizeTest, AHugeFileIsNotReadByAnEmbed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const char *name : {"huge.md", "huge.base", "huge.tex"}) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_TRUE(file.resize(qint64(1) << 30)) << "a sparse gibibyte";
    }
    for (const char *name : {"huge", "huge.base", "huge.tex"}) {
        const QString embed = QStringLiteral("![[%1]]\n").arg(QLatin1String(name));
        const QString path = dir.filePath(QString::fromLatin1(name).contains(QLatin1Char('.')) ? QString::fromLatin1(name)
                                                                                                : QStringLiteral("huge.md"));
        auto editor = editorWith(embed);
        editor->setLinkResolver([&](const QString &) { return path; });
        QElapsedTimer timer;
        timer.start();
        editor->setCursorOffset(editor->noteText().size());
        EXPECT_TRUE(editor->panelTextAt(0).contains(QStringLiteral("too large"))) << name << ": " << qPrintable(editor->panelTextAt(0));
        EXPECT_LT(timer.elapsed(), 3000) << name << " was read";
    }
}

// --- file cards and several embeds in one block -----------------------------

TEST(FileEmbedTest, BinaryFileBecomesACardNotAWallOfBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("blob.bin"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        QByteArray bytes(256, char(0));
        for (int i = 0; i < bytes.size(); ++i)
            bytes[i] = char(i % 256);
        file.write(bytes);
    }

    auto editor = editorWith(QStringLiteral("![[blob.bin]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_TRUE(editor->isFileCardAt(0));
    EXPECT_FALSE(editor->displayText().contains(QChar(0)));
}

TEST(FileEmbedTest, TextFileStaysAReadOnlyListing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notes.txt"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("plain text, nothing binary here\n");
    }

    auto editor = editorWith(QStringLiteral("![[notes.txt]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_FALSE(editor->isFileCardAt(0));
}

TEST(MultiEmbedTest, AParagraphOfSeveralImageEmbedsRendersEveryOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    const QString a = dir.filePath(QStringLiteral("a.png"));
    const QString b = dir.filePath(QStringLiteral("b.png"));
    ASSERT_TRUE(image.save(a));
    ASSERT_TRUE(image.save(b));

    auto editor = editorWith(QStringLiteral("![[a.png]]\n![[b.png]]\n"));
    editor->setLinkResolver([&](const QString &target) {
        return target == QLatin1String("a.png") ? a : target == QLatin1String("b.png") ? b : QString();
    });

    // Two embeds, each on its own line, nothing else in the block: each is
    // its own row of one cell.
    EXPECT_EQ(editor->embedCellCountAt(0), 2);
}

namespace {

QByteArray redPNG()
{
    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

} // namespace

TEST(InlineImageTest, AnAttachmentSourceDrawsThePictureTheTableHolds)
{
    auto editor = editorWith(QStringLiteral("![x](attachment:pic.png)\n"));
    editor->setLinkResolver([](const QString &) { return QString(); });
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("x"))) << "no table: the alt text, as a broken link shows";

    editor->setInlineImages({{QStringLiteral("pic.png"), redPNG()}});
    EXPECT_TRUE(editor->displayText().trimmed().isEmpty()) << "its source is hidden: the picture is the block";
    editor->setInlineImages({});
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("x"))) << "and gone with the table";
}

TEST(InlineImageTest, ASameNamedPictureIsLookedUpUnderTheScopeOfItsOffset)
{
    const QString note = QStringLiteral("![x](attachment:pic.png)\n\n![y](attachment:pic.png)\n");
    auto editor = editorWith(note);
    editor->setLinkResolver([](const QString &) { return QString(); });
    const qsizetype second = note.lastIndexOf(QStringLiteral("!["));
    // Only the second embed's scope finds a picture: the first stays text.
    editor->setInlineImages({{QStringLiteral("1/pic.png"), redPNG()}},
                            [second](qsizetype offset) { return offset >= second ? QStringLiteral("1/") : QString(); });
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("x"))) << "no picture under the first one's scope";
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("y"))) << "the second shows its picture";
}

TEST(InlineImageTest, AnAttachmentNeitherInTheTableNorAFileStaysText)
{
    auto editor = editorWith(QStringLiteral("![x](attachment:other.png)\n"));
    editor->setLinkResolver([](const QString &) { return QString(); });
    editor->setInlineImages({{QStringLiteral("pic.png"), redPNG()}});
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("x")));
}

TEST(InlineImageTest, AnAttachmentInRunningTextIsAnInlineCell)
{
    auto editor = editorWith(QStringLiteral("see ![x](attachment:pic.png) here\n"));
    editor->setLinkResolver([](const QString &) { return QString(); });
    editor->setInlineImages({{QStringLiteral("pic.png"), redPNG()}});
    EXPECT_EQ(editor->embedCellCountAt(0), 1);
    EXPECT_EQ(editor->embedCellRectAt(0, 0).size(), QSizeF(20, 20));
}

TEST(MultiEmbedTest, AnEmbedInsideRunningTextStillRenders)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString path = dir.filePath(QStringLiteral("inline.png"));
    ASSERT_TRUE(image.save(path));

    auto editor = editorWith(QStringLiteral("see ![[inline.png]] here\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    EXPECT_EQ(editor->embedCellCountAt(0), 1);
    // The paragraph's own text is still there - only the embed's own source
    // range was hidden, not the words around it.
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("see")));
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("here")));
    // The "!" carries the image, so both spaces around it survive.
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("see ! here")));
    // Inline: after "see " on the first line, not a row under the text.
    const QRectF cell = editor->embedCellRectAt(0, 0);
    EXPECT_GT(cell.left(), 10);
    EXPECT_LT(cell.top(), 20);
    EXPECT_EQ(cell.size(), QSizeF(20, 20));

    // The caret inside the embed shows its source instead.
    editor->enterBlockAt(8);
    EXPECT_EQ(editor->embedCellCountAt(0), 0);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("![[inline.png]]")));
}

TEST(MultiEmbedTest, ATallInlineEmbedGrowsItsLineAndTheTextAfterItFollowsOnIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage image(40, 120, QImage::Format_RGB32);
    image.fill(Qt::green);
    const QString path = dir.filePath(QStringLiteral("tall.png"));
    ASSERT_TRUE(image.save(path));

    auto editor = editorWith(QStringLiteral("one ![[tall.png]] two ![[tall.png]] three\n"));
    editor->setLinkResolver([&](const QString &) { return path; });

    ASSERT_EQ(editor->embedCellCountAt(0), 2);
    const QRectF first = editor->embedCellRectAt(0, 0);
    const QRectF second = editor->embedCellRectAt(0, 1);
    // Side by side on one line, both from its top, the second after "two".
    EXPECT_EQ(first.top(), second.top());
    EXPECT_GT(second.left(), first.right() + 10);
    EXPECT_GE(first.top(), 0);
}

// --- inline <img>: the Markdown image's own path, spelled in HTML ------------

namespace {

/// A 100x50 PNG in `dir`, and a resolver that finds it as "p.png" (anything
/// else is unresolved, as a remote or unknown target is).
struct HtmlImgFixture {
    QTemporaryDir dir;
    QString path;
    HtmlImgFixture()
    {
        QImage image(100, 50, QImage::Format_RGB32);
        image.fill(Qt::red);
        path = dir.filePath(QStringLiteral("p.png"));
        image.save(path);
    }
    std::function<QString(const QString &)> resolver()
    {
        return [this](const QString &target) {
            return target == QLatin1String("p.png") ? path : QString();
        };
    }
};

const QString kImgTable = QStringLiteral("| a | b |\n|---|---|\n| x | %1 |\n");

} // namespace

TEST(HtmlImgTest, AnImgInATableCellBecomesAnImageOfTheRequestedWidth)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = editorWith(kImgTable.arg(QStringLiteral("<img src=\"p.png\" width=\"40\">")));
    editor->setLinkResolver(fixture.resolver());

    ASSERT_EQ(editor->embedCellCountAt(0), 1);
    // 100x50 asked to be 40 wide: the height follows at 20.
    EXPECT_EQ(editor->embedCellRectAt(0, 0).size(), QSizeF(40, 20));
    EXPECT_TRUE(editor->embedCellLabelAt(0, 0).isEmpty());
}

TEST(HtmlImgTest, AnImgInATableWithAnEmptyHeaderStillRenders)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = editorWith(QStringLiteral("| | |\n|---|---|\n"
                                            "| Flashes red when hit | <img src=\"p.png\" width=\"40\"> |\n"
                                            "| White sweep | <img src=\"p.png\" width=\"60\"> |\n"));
    editor->setLinkResolver(fixture.resolver());

    ASSERT_EQ(editor->embedCellCountAt(0), 2);
    EXPECT_EQ(editor->embedCellRectAt(0, 0).size(), QSizeF(40, 20));
    EXPECT_EQ(editor->embedCellRectAt(0, 1).size(), QSizeF(60, 30));
}

TEST(HtmlImgTest, AMarkdownImageInATableCellRendersToo)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = editorWith(kImgTable.arg(QStringLiteral("![40](p.png)")));
    editor->setLinkResolver(fixture.resolver());

    // The shared path: whatever draws the one draws the other.
    ASSERT_EQ(editor->embedCellCountAt(0), 1);
    EXPECT_EQ(editor->embedCellRectAt(0, 0).size(), QSizeF(40, 20));
}

TEST(HtmlImgTest, AnImgInsideRunningTextIsAnInlineImage)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = editorWith(QStringLiteral("see <img src=\"p.png\" width=\"60\"> here\n"));
    editor->setLinkResolver(fixture.resolver());

    ASSERT_EQ(editor->embedCellCountAt(0), 1);
    EXPECT_EQ(editor->embedCellRectAt(0, 0).size(), QSizeF(60, 30));
    EXPECT_GT(editor->embedCellRectAt(0, 0).left(), 10); // after "see ", not a row under it
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("see")));
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("here")));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("src=")));
}

TEST(HtmlImgTest, AnImgInAListItemAndAQuoteIsDrawnToo)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = editorWith(QStringLiteral("- item <img src=\"p.png\" width=\"20\">\n\n"
                                            "> quoted <img src=\"p.png\" width=\"20\">\n"));
    editor->setLinkResolver(fixture.resolver());

    EXPECT_EQ(editor->embedCellCountAt(0), 1);
    const qsizetype quote = editor->noteText().indexOf(QLatin1Char('>'));
    EXPECT_EQ(editor->embedCellCountAt(quote), 1);
}

TEST(HtmlImgTest, OneAttributeKeepsTheAspectAndBothUseBoth)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto heightOnly = editorWith(QStringLiteral("a <img src=\"p.png\" height=\"25\"> b\n"));
    heightOnly->setLinkResolver(fixture.resolver());
    ASSERT_EQ(heightOnly->embedCellCountAt(0), 1);
    EXPECT_EQ(heightOnly->embedCellRectAt(0, 0).size(), QSizeF(50, 25));

    auto both = editorWith(QStringLiteral("a <img src=\"p.png\" width=\"30px\" height=\"30\"> b\n"));
    both->setLinkResolver(fixture.resolver());
    ASSERT_EQ(both->embedCellCountAt(0), 1);
    EXPECT_EQ(both->embedCellRectAt(0, 0).size(), QSizeF(30, 30));

    auto neither = editorWith(QStringLiteral("a <img src='p.png'> b\n"));
    neither->setLinkResolver(fixture.resolver());
    ASSERT_EQ(neither->embedCellCountAt(0), 1);
    EXPECT_EQ(neither->embedCellRectAt(0, 0).size(), QSizeF(100, 50));
}

TEST(HtmlImgTest, TheSourceShowsWhileTheCaretIsInsideIt)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto paragraph = editorWith(QStringLiteral("see <img src=\"p.png\" width=\"60\"> here\n"));
    paragraph->setLinkResolver(fixture.resolver());
    ASSERT_EQ(paragraph->embedCellCountAt(0), 1);
    paragraph->enterBlockAt(8);
    EXPECT_EQ(paragraph->embedCellCountAt(0), 0);
    EXPECT_TRUE(paragraph->displayText().contains(QStringLiteral("<img src=\"p.png\" width=\"60\">")));

    const QString text = kImgTable.arg(QStringLiteral("<img src=\"p.png\" width=\"40\">"));
    auto table = editorWith(text);
    table->setLinkResolver(fixture.resolver());
    ASSERT_EQ(table->embedCellCountAt(0), 1);
    table->enterBlockAt(text.indexOf(QStringLiteral("src=")));
    EXPECT_EQ(table->embedCellCountAt(0), 0);
    table->enterBlockAt(2); // the header's own cell: the other cell is an image again
    EXPECT_EQ(table->embedCellCountAt(0), 1);
}

TEST(HtmlImgTest, AMissingFileShowsTheAltInsteadOfAnImage)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto unresolved = editorWith(QStringLiteral("x <img src=\"gone.png\" alt=\"a red flash\"> y\n"));
    unresolved->setLinkResolver(fixture.resolver());
    ASSERT_EQ(unresolved->embedCellCountAt(0), 1);
    EXPECT_EQ(unresolved->embedCellLabelAt(0, 0), QStringLiteral("a red flash"));

    // A path that resolves to nothing on disk is the same placeholder.
    auto absent = editorWith(kImgTable.arg(QStringLiteral("<img src=\"gone.png\" alt=\"a red flash\">")));
    absent->setLinkResolver([&](const QString &) { return fixture.dir.filePath(QStringLiteral("gone.png")); });
    ASSERT_EQ(absent->embedCellCountAt(0), 1);
    EXPECT_EQ(absent->embedCellLabelAt(0, 0), QStringLiteral("a red flash"));
}

TEST(HtmlImgTest, AMalformedTagStaysAsText)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    for (const QString &tag : {QStringLiteral("<img width=\"40\">"),            // no src
                               QStringLiteral("<img src=\"p.png width=\"40\">"), // unclosed quote
                               QStringLiteral("<img src=\"\">")}) {             // empty src
        auto editor = editorWith(QStringLiteral("x %1 y\n").arg(tag));
        editor->setLinkResolver(fixture.resolver());
        EXPECT_EQ(editor->embedCellCountAt(0), 0) << qPrintable(tag);
        EXPECT_TRUE(editor->displayText().contains(tag)) << qPrintable(tag);
    }
}

TEST(HtmlImgTest, AnOddSchemeRendersNothing)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    for (const QString &src : {QStringLiteral("javascript:alert(1)"), QStringLiteral("data:image/png;base64,AAAA"),
                               QStringLiteral("file:///etc/passwd"), QStringLiteral("//evil.example/x.png")}) {
        auto editor = editorWith(QStringLiteral("x <img src=\"%1\" width=\"40\"> y\n").arg(src));
        // Even a resolver that would hand back a real file for anything.
        editor->setLinkResolver([&](const QString &) { return fixture.path; });
        EXPECT_EQ(editor->embedCellCountAt(0), 0) << qPrintable(src);
    }
}

TEST(HtmlImgTest, ATallImageGrowsItsRowAndSitsInsideItsCell)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto plain = editorWith(kImgTable.arg(QStringLiteral("x")));
    plain->setLinkResolver(fixture.resolver());
    auto withImage = editorWith(kImgTable.arg(QStringLiteral("<img src=\"p.png\" width=\"300\">")));
    withImage->setLinkResolver(fixture.resolver());

    // 100x50 at 300 wide is 150 tall: the row grows by about that much.
    EXPECT_GT(withImage->documentHeight(), plain->documentHeight() + 100);
    const QRectF cell = withImage->tableCellRect(0, 1, 1);
    ASSERT_FALSE(cell.isEmpty());
    EXPECT_GT(cell.height(), 150);
    EXPECT_GE(cell.width(), 300); // the column is as wide as the image wants

    // Block-local to viewport: cell (0, 0) starts at the block's origin.
    const QRectF origin = withImage->tableCellRect(0, 0, 0);
    const QRectF image = withImage->embedCellRectAt(0, 0).translated(origin.topLeft());
    EXPECT_EQ(image.size(), QSizeF(300, 150));
    EXPECT_TRUE(cell.contains(image)) << "image and cell rects disagree";
}

TEST(HtmlImgTest, AClickOnTheImageInACellPutsTheCaretInThatCell)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    const QString text = kImgTable.arg(QStringLiteral("<img src=\"p.png\" width=\"40\">"));
    auto editor = editorWith(text);
    editor->setLinkResolver(fixture.resolver());
    ASSERT_EQ(editor->embedCellCountAt(0), 1);

    const QRectF origin = editor->tableCellRect(0, 0, 0);
    const QPoint inside = (editor->embedCellRectAt(0, 0).translated(origin.topLeft())).center().toPoint();
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, inside);

    const qsizetype tag = text.indexOf(QStringLiteral("<img"));
    EXPECT_GE(editor->cursorOffset(), tag);
    EXPECT_LE(editor->cursorOffset(), text.indexOf(QLatin1Char('>'), tag) + 1);
    EXPECT_EQ(editor->embedCellCountAt(0), 0) << "the cell shows its source now";
}

TEST(HtmlImgTest, TwoImagesInOneCellBothDrawAndStayInIt)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = editorWith(
        kImgTable.arg(QStringLiteral("<img src=\"p.png\" width=\"40\"> <img src=\"p.png\" width=\"60\">")));
    editor->setLinkResolver(fixture.resolver());

    ASSERT_EQ(editor->embedCellCountAt(0), 2);
    const QRectF origin = editor->tableCellRect(0, 0, 0);
    const QRectF cell = editor->tableCellRect(0, 1, 1);
    const QRectF first = editor->embedCellRectAt(0, 0).translated(origin.topLeft());
    const QRectF second = editor->embedCellRectAt(0, 1).translated(origin.topLeft());
    EXPECT_EQ(first.size(), QSizeF(40, 20));
    EXPECT_EQ(second.size(), QSizeF(60, 30));
    EXPECT_TRUE(cell.contains(first));
    EXPECT_TRUE(cell.contains(second));
    EXPECT_GE(second.left(), first.right()); // side by side, not on top of each other
}

TEST(HtmlImgTest, BothSizesAreCappedKeepingTheGivenRatio)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    for (const QString &source : {QStringLiteral("a <img src=\"p.png\" width=\"1\" height=\"99999\"> b\n"),
                                  QStringLiteral("a ![[p.png|1x99999]] b\n")}) {
        auto editor = editorWith(source);
        editor->setLinkResolver(fixture.resolver());
        ASSERT_EQ(editor->embedCellCountAt(0), 1) << qPrintable(source);
        const QSizeF size = editor->embedCellRectAt(0, 0).size();
        EXPECT_LE(size.height(), 4096) << qPrintable(source);
        EXPECT_NEAR(size.width() / size.height(), 1.0 / 99999, 1e-6) << qPrintable(source);
    }
}

TEST(HtmlImgTest, ATagOnItsOwnAcrossTwoLinesIsStillAnImgNotATranscludedFile)
{
    HtmlImgFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());
    // Not an HTML block (the tag is incomplete on its first line), so the
    // paragraph is the one tag: the sole-embed path's case.
    auto sized = editorWith(QStringLiteral("<img src=\"p.png\"\n     height=\"25\">\n"));
    sized->setLinkResolver(fixture.resolver());
    ASSERT_EQ(sized->embedCellCountAt(0), 1);
    EXPECT_EQ(sized->embedCellRectAt(0, 0).size(), QSizeF(50, 25)); // aspect kept, not 100x25

    const QString pdf = fixture.dir.filePath(QStringLiteral("doc.pdf"));
    {
        QFile file(pdf);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("%PDF-1.4\n");
    }
    auto document = editorWith(QStringLiteral("<img src=\"doc.pdf\"\n     alt=\"a doc\">\n"));
    document->setLinkResolver([&](const QString &) { return pdf; });
    ASSERT_EQ(document->embedCellCountAt(0), 1);
    EXPECT_EQ(document->embedCellLabelAt(0, 0), QStringLiteral("a doc"));
    EXPECT_TRUE(document->embedCellRectAt(0, 0).height() <= 44) << "a small card, not a PDF page";
}

TEST(HtmlImgTest, AVoidImgDoesNotBreakAnEnclosingPair)
{
    auto editor = editorWith(QStringLiteral("<b>one <img src=\"p.png\"> two</b>\n"));
    // Paired and drawn: the <b> tags are hidden, not left as source.
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("<b>")));
}

// --- animated GIFs ------------------------------------------------------------

namespace {

// 8x8, three frames (red, green, blue), 20 ms each, looping; and the same
// picture as a one-frame GIF. Qt has no GIF writer, so they are bytes.
const unsigned char kAnimatedGIF[] = {0x47,0x49,0x46,0x38,0x39,0x61,0x08,0x00,0x08,0x00,0x81,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x21,0xff,0x0b,0x4e,0x45,0x54,0x53,0x43,0x41,0x50,0x45,0x32,0x2e,0x30,0x03,0x01,0x00,0x00,0x00,0x21,0xf9,0x04,0x04,0x02,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x00,0x08,0x0f,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x2a,0x4c,0x18,0x10,0x00,0x21,0xf9,0x04,0x05,0x02,0x00,0x01,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x81,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x0f,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x2a,0x4c,0x18,0x10,0x00,0x21,0xf9,0x04,0x05,0x02,0x00,0x01,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x81,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x0f,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x2a,0x4c,0x18,0x10,0x00,0x3b};
const unsigned char kOnceGIF[] = {0x47,0x49,0x46,0x38,0x39,0x61,0x08,0x00,0x08,0x00,0x81,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x21,0xf9,0x04,0x04,0x02,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x00,0x08,0x0f,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x2a,0x4c,0x18,0x10,0x00,0x21,0xf9,0x04,0x05,0x02,0x00,0x01,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x81,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x0f,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x2a,0x4c,0x18,0x10,0x00,0x21,0xf9,0x04,0x05,0x02,0x00,0x01,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x81,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x0f,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x2a,0x4c,0x18,0x10,0x00,0x3b};
const unsigned char kStillGIF[] = {0x47,0x49,0x46,0x38,0x37,0x61,0x08,0x00,0x08,0x00,0x81,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x00,0x08,0x0f,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x2a,0x4c,0x18,0x10,0x00,0x3b};

/// A directory of GIF files: g0.gif ... g(count-1).gif, animated or still.
struct GIFfixture {
    QTemporaryDir dir;
    QString path(int index) const { return dir.filePath(QStringLiteral("g%1.gif").arg(index)); }
    /// `once` is the animated picture without a loop extension: it plays out and stops.
    GIFfixture(int count, bool animated, bool once = false)
    {
        for (int i = 0; i < count; ++i) {
            QFile file(path(i));
            if (!file.open(QIODevice::WriteOnly))
                continue;
            if (once)
                file.write(QByteArray(reinterpret_cast<const char *>(kOnceGIF), sizeof kOnceGIF));
            else
                file.write(animated ? QByteArray(reinterpret_cast<const char *>(kAnimatedGIF), sizeof kAnimatedGIF)
                                    : QByteArray(reinterpret_cast<const char *>(kStillGIF), sizeof kStillGIF));
        }
    }
    std::function<QString(const QString &)> resolver()
    {
        return [this](const QString &target) {
            return target.endsWith(QLatin1String(".gif")) ? dir.filePath(target) : QString();
        };
    }
};

/// A shown editor, painted once so what is on screen has started.
std::unique_ptr<MarkdownEditor> shownEditorWith(const QString &text, GIFfixture &fixture)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(600, 300);
    editor->setLinkResolver(fixture.resolver());
    editor->setNoteText(text);
    editor->show();
    QTest::qWaitForWindowExposed(editor.get());
    editor->viewport()->repaint();
    return editor;
}

} // namespace

TEST(GIFtest, AnAnimatedGIFPlaysWhileItIsOnScreen)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    ASSERT_EQ(QImageReader(fixture.path(0)).imageCount(), 3) << "the fixture is a three-frame GIF";
    for (const QString &text : {QStringLiteral("![[g0.gif]]\n"),                       // alone in a block
                                QStringLiteral("see ![](g0.gif) here\n"),              // inline in text
                                QStringLiteral("a <img src=\"g0.gif\" width=\"40\"> b\n"),
                                QStringLiteral("| a | b |\n|---|---|\n| x | <img src=\"g0.gif\"> |\n")}) {
        auto editor = shownEditorWith(text, fixture);
        EXPECT_EQ(editor->GIFmovieCount(), 1) << qPrintable(text);
        // Frames move on by themselves, and each one repaints (the viewport is
        // asked to update its rect, which the event loop turns into a paint).
        const bool advanced = QTest::qWaitFor([&] { return editor->GIFframeOf(fixture.path(0)) >= 1; }, 3000);
        EXPECT_TRUE(advanced) << qPrintable(text);
    }
}

TEST(GIFtest, AStillGIFAndAPngNeverStartAMovie)
{
    GIFfixture still(1, false);
    ASSERT_TRUE(still.dir.isValid());
    ASSERT_EQ(QImageReader(still.path(0)).imageCount(), 1);
    auto editor = shownEditorWith(QStringLiteral("![[g0.gif]] and ![[g0.gif]]\n"), still);
    EXPECT_EQ(editor->GIFmovieCount(), 0);

    HtmlImgFixture png;
    ASSERT_TRUE(png.dir.isValid());
    auto plain = std::make_unique<MarkdownEditor>();
    plain->resize(600, 300);
    plain->setLinkResolver(png.resolver());
    plain->setNoteText(QStringLiteral("![[p.png]] <img src=\"p.png\">\n"));
    plain->show();
    QTest::qWaitForWindowExposed(plain.get());
    plain->viewport()->repaint();
    EXPECT_EQ(plain->GIFmovieCount(), 0);
}

TEST(GIFtest, AFileNamedGIFThatIsNotOneNeverStartsAMovie)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    QImage red(8, 8, QImage::Format_RGB32);
    red.fill(Qt::red);
    ASSERT_TRUE(red.save(fixture.path(0), "PNG")); // PNG bytes under a .gif name
    auto editor = shownEditorWith(QStringLiteral("![[g0.gif]]\n"), fixture);
    EXPECT_EQ(editor->GIFmovieCount(), 0);
}

TEST(GIFtest, TheSameFileTwiceIsOneMovie)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = shownEditorWith(QStringLiteral("a ![[g0.gif]] b ![[g0.gif]] c\n"), fixture);
    EXPECT_EQ(editor->GIFmovieCount(), 1);
}

TEST(GIFtest, AMovieEndsWhenItScrollsOutHidesOrTheNoteCloses)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    QString text = QStringLiteral("![[g0.gif]]\n\n");
    for (int i = 0; i < 80; ++i)
        text += QStringLiteral("filler paragraph %1\n\n").arg(i);
    auto editor = shownEditorWith(text, fixture);
    ASSERT_EQ(editor->GIFmovieCount(), 1);

    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum());
    EXPECT_EQ(editor->GIFmovieCount(), 0) << "scrolled out of the viewport";
    QTest::qWait(100);
    editor->viewport()->repaint();
    EXPECT_EQ(editor->GIFmovieCount(), 0) << "and a later paint does not bring it back";

    editor->verticalScrollBar()->setValue(0);
    editor->viewport()->repaint();
    EXPECT_EQ(editor->GIFmovieCount(), 1) << "scrolled back in";
    editor->hide();
    EXPECT_EQ(editor->GIFmovieCount(), 0) << "hidden";

    editor->show();
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            editor->viewport()->update();
            return editor->GIFmovieCount() == 1;
        },
        3000))
        << "shown again: the next paint starts it";
    editor->setNoteText(QStringLiteral("another note\n"));
    EXPECT_EQ(editor->GIFmovieCount(), 0) << "the note closed";
}

TEST(GIFtest, NoMoreThanEightMoviesPlayAtOnce)
{
    GIFfixture fixture(12, true);
    ASSERT_TRUE(fixture.dir.isValid());
    QString text;
    for (int i = 0; i < 12; ++i)
        text += QStringLiteral("![[g%1.gif]] ").arg(i);
    auto editor = shownEditorWith(text + QStringLiteral("\n"), fixture);
    EXPECT_EQ(editor->GIFmovieCount(), 8);
}

TEST(GIFtest, AGIFthatIsNotPaintedToTheViewportDoesNotPlay)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = std::make_unique<MarkdownEditor>(); // never shown
    editor->resize(600, 300);
    editor->setLinkResolver(fixture.resolver());
    editor->setNoteText(QStringLiteral("![[g0.gif]]\n"));
    editor->grab(); // a render to a pixmap, as an export or a screenshot does
    EXPECT_EQ(editor->GIFmovieCount(), 0);
}

namespace {

/// The pixels the shown editor really has on screen.
QImage screenOf(MarkdownEditor &editor)
{
    return editor.windowHandle()->screen()->grabWindow(editor.winId()).toImage().convertToFormat(QImage::Format_RGB32);
}

int pixelsOf(const QImage &image, const QColor &colour)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            count += image.pixelColor(x, y) == colour;
    }
    return count;
}

} // namespace

TEST(GIFtest, TheDrawnPixelsAreTheMoviesFrame)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = shownEditorWith(QStringLiteral("a <img src=\"g0.gif\" width=\"40\"> b\n"), fixture);
    ASSERT_EQ(editor->GIFmovieCount(), 1);
    // The fixture's frames are solid red, green, blue; at 40 px wide a frame is 1600 px of it.
    ASSERT_TRUE(QTest::qWaitFor([&] { return pixelsOf(screenOf(*editor), Qt::green) > 1000; }, 3000))
        << "the viewport shows the movie's second frame, not the still's first";
    // And it keeps moving: the third frame, then back round to the first.
    EXPECT_TRUE(QTest::qWaitFor([&] { return pixelsOf(screenOf(*editor), Qt::blue) > 1000; }, 3000));
    EXPECT_TRUE(QTest::qWaitFor([&] { return pixelsOf(screenOf(*editor), Qt::red) > 1000; }, 3000));
}

TEST(GIFtest, AnExportRenderDrawsTheFirstFrameWhileTheScreenPlays)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    auto editor = shownEditorWith(QStringLiteral("a <img src=\"g0.gif\" width=\"40\"> b\n"), fixture);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->GIFframeOf(fixture.path(0)) >= 1; }, 3000));

    // A QPicture is a paint engine of its own (not the raster one the screen
    // uses), as a PDF is: the movie is on frame 1 or 2 and this must show frame 0.
    QPicture picture;
    {
        QPainter painter(&picture);
        editor->render(&painter);
    }
    QImage replay(600, 300, QImage::Format_RGB32);
    replay.fill(Qt::white);
    {
        QPainter painter(&replay);
        painter.drawPicture(0, 0, picture);
    }
    EXPECT_GT(pixelsOf(replay, Qt::red), 1000);
    EXPECT_EQ(pixelsOf(replay, Qt::green), 0);
    EXPECT_EQ(pixelsOf(replay, Qt::blue), 0);
}

TEST(GIFtest, AFrameInALongTableRepaintsOnlyTheRowsBeingRepainted)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    QString text = QStringLiteral("| a | b |\n|---|---|\n| row | <img src=\"g0.gif\" width=\"40\"> |\n");
    for (int row = 0; row < 500; ++row)
        text += QStringLiteral("| r%1 | x |\n").arg(row);
    auto editor = shownEditorWith(text, fixture);
    ASSERT_EQ(editor->GIFmovieCount(), 1);

    // The whole table is 1000 cells; a full repaint draws the screenful.
    editor->viewport()->repaint();
    EXPECT_GT(editor->tableCellsPainted(), 0);
    EXPECT_LT(editor->tableCellsPainted(), 60);

    // A frame repaints the image's own rect: one row of the table.
    const QRectF origin = editor->tableCellRect(0, 0, 0);
    const QRect image = editor->embedCellRectAt(0, 0).translated(origin.topLeft()).toAlignedRect();
    editor->viewport()->repaint(image);
    EXPECT_GT(editor->tableCellsPainted(), 0);
    EXPECT_LE(editor->tableCellsPainted(), 4);
}

TEST(GIFtest, AMovieEndsOnResizeResolverChangeEditAndDestruction)
{
    GIFfixture fixture(1, true);
    ASSERT_TRUE(fixture.dir.isValid());
    const auto start = [&](const QString &text, int height) {
        auto editor = std::make_unique<MarkdownEditor>();
        editor->resize(600, height);
        editor->setLinkResolver(fixture.resolver());
        editor->setNoteText(text);
        editor->show();
        QTest::qWaitFor([&] {
            editor->viewport()->update();
            return editor->GIFmovieCount() == 1;
        }, 3000);
        return editor;
    };
    QString low;
    for (int i = 0; i < 25; ++i)
        low += QStringLiteral("filler paragraph %1\n\n").arg(i);
    low += QStringLiteral("![[g0.gif]]\n");

    { // resize: the GIF is below the new, shorter viewport
        auto editor = start(low, 1500);
        ASSERT_EQ(editor->GIFmovieCount(), 1);
        editor->resize(600, 150);
        EXPECT_TRUE(QTest::qWaitFor([&] { return editor->GIFmovieCount() == 0; }, 3000));
    }
    { // a new resolver: everything starts over
        auto editor = start(QStringLiteral("a ![[g0.gif]] b\n"), 300);
        ASSERT_EQ(editor->GIFmovieCount(), 1);
        editor->setLinkResolver(fixture.resolver());
        EXPECT_EQ(editor->GIFmovieCount(), 0);
    }
    { // an edit deletes the embed while it plays: released from its own frameChanged
        const QString tag = QStringLiteral("<img src=\"g0.gif\" width=\"40\">");
        auto editor = start(QStringLiteral("see ") + tag + QStringLiteral(" end\n"), 300);
        ASSERT_EQ(editor->GIFmovieCount(), 1);
        editor->setCursorOffset(4 + tag.size());
        for (int i = 0; i < tag.size(); ++i)
            QTest::keyClick(editor.get(), Qt::Key_Backspace);
        ASSERT_EQ(editor->noteText(), QStringLiteral("see  end\n"));
        EXPECT_TRUE(QTest::qWaitFor([&] { return editor->GIFmovieCount() == 0; }, 3000));
        QTest::qWait(100); // the deferred release runs, nothing touches the freed movie
    }
    { // the editor goes away mid-animation
        auto editor = start(QStringLiteral("a ![[g0.gif]] b\n"), 300);
        ASSERT_EQ(editor->GIFmovieCount(), 1);
        editor.reset();
        QTest::qWait(150);
    }
}

TEST(GIFtest, AGIFThatPlayedOutKeepsItsLastFrameAndFreesItsSlot)
{
    GIFfixture once(1, true, true);
    ASSERT_TRUE(once.dir.isValid());
    auto editor = shownEditorWith(QStringLiteral("a <img src=\"g0.gif\" width=\"40\"> b\n"), once);
    ASSERT_EQ(editor->GIFmovieCount(), 1);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->GIFmovieCount() == 0; }, 5000)) << "finished: the slot is free";
    EXPECT_TRUE(QTest::qWaitFor([&] { return pixelsOf(screenOf(*editor), Qt::blue) > 1000; }, 3000))
        << "and the picture stays on the last frame";
    editor->viewport()->repaint();
    QTest::qWait(100);
    EXPECT_EQ(editor->GIFmovieCount(), 0) << "a paint does not start it again";
}

// --- note embeds: whole, #Heading, #^block ----------------------------------

TEST(NoteEmbedTest, WholeHeadingAndBlockEachExtractTheRightSlice)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Target.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("Intro line.\n\n"
                   "# First\n\nFirst section body.\n\n"
                   "# Second\n\nSecond section body. ^myblock\n");
    }

    // The panel's own text - not displayText(), which only ever reflects
    // view.map, and a note embed (like a source-file listing) substitutes a
    // synthetic QTextLayout instead of mapping the note's own bytes.
    auto whole = editorWith(QStringLiteral("![[Target]]\n"));
    whole->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(whole->panelTextAt(0).contains(QStringLiteral("Intro line")));
    EXPECT_TRUE(whole->panelTextAt(0).contains(QStringLiteral("Second section body")));

    auto heading = editorWith(QStringLiteral("![[Target#First]]\n"));
    heading->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(heading->panelTextAt(0).contains(QStringLiteral("First section body")));
    EXPECT_FALSE(heading->panelTextAt(0).contains(QStringLiteral("Second section body")));

    auto block = editorWith(QStringLiteral("![[Target#^myblock]]\n"));
    block->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(block->panelTextAt(0).contains(QStringLiteral("Second section body")));
    EXPECT_FALSE(block->panelTextAt(0).contains(QStringLiteral("First section body")));
}

TEST(ListLabelTest, CountedNumbersAndWordStyleOutlines)
{
    auto editor = editorWith(QString());
    ListItemSpan item;
    item.ordered = true;
    item.number = 1;      // typed "1." ...
    item.shownNumber = 3; // ... as the third item
    item.bullet = QLatin1Char(')');
    item.outline = QStringLiteral("2.3");
    EXPECT_EQ(editor->listLabel(item), QStringLiteral("3.")) << "the drawn ending is the setting's, not the note's";
    editor->setListNumberSuffix(QString());
    EXPECT_EQ(editor->listLabel(item), QStringLiteral("3)")) << "empty: as written";
    editor->setListNumberSuffix(QStringLiteral("."));
    editor->setOutlineNumbering(true);
    EXPECT_EQ(editor->listLabel(item), QStringLiteral("2.3."));
    item.outline = QStringLiteral("4"); // the top level keeps its own look
    EXPECT_EQ(editor->listLabel(item), QStringLiteral("3."));
    ListItemSpan bullet;
    EXPECT_TRUE(editor->listLabel(bullet).isEmpty());
}

namespace {
const QString kFootnoteNote = QStringLiteral(
    "Text.[^a] Inline.^[Right here.] Again.[^a]\n\n[^a]: The body.\n\nAfter.\n");
}

TEST(FootnoteTest, ReferencesAndDefinitionsAreDrawnAsNumbers)
{
    auto editor = editorWith(kFootnoteNote);
    editor->resize(900, 600);
    editor->show();
    editor->setCursorOffset(kFootnoteNote.size()); // on "After.", off every footnote
    QApplication::processEvents();
    EXPECT_EQ(editor->footnoteMarkTexts(),
              (QStringList{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("1"), QStringLiteral("1. ")}));
    const auto entries = editor->footnotes();
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries.at(0).label, QStringLiteral("a"));
    EXPECT_EQ(entries.at(0).preview, QStringLiteral("The body."));
    EXPECT_EQ(entries.at(1).preview, QStringLiteral("Right here."));

    // The caret inside a footnote shows that one as written, as Obsidian does.
    editor->setCursorOffset(kFootnoteNote.indexOf(QStringLiteral("Right")));
    QTest::keyClick(editor.get(), Qt::Key_Right); // a person's caret, not a program's
    QApplication::processEvents();
    EXPECT_FALSE(editor->footnoteMarkTexts().contains(QStringLiteral("2")));
}

TEST(FootnoteTest, ReadingViewListsTheDefinitionsAtTheEnd)
{
    auto editor = editorWith(kFootnoteNote);
    editor->resize(900, 600);
    editor->show();
    editor->setMode(MarkdownEditor::Mode::Reading);
    QApplication::processEvents();
    EXPECT_EQ(editor->footnotesListRowCount(), 1); // one definition; the inline one lives in its sentence
    EXPECT_FALSE(editor->footnoteMarkTexts().contains(QStringLiteral("1. "))) << "the definition itself is not shown";
    editor->setMode(MarkdownEditor::Mode::LivePreview);
    QApplication::processEvents();
    EXPECT_EQ(editor->footnotesListRowCount(), 0);
}

TEST(FootnoteTest, CtrlClickOnAReferenceGoesToItsDefinition)
{
    auto editor = editorWith(kFootnoteNote);
    editor->resize(900, 600);
    editor->show();
    editor->setCursorOffset(kFootnoteNote.size());
    QApplication::processEvents();
    const qsizetype body = kFootnoteNote.indexOf(QStringLiteral("The body."));
    bool jumped = false;
    for (int y = 2; y < 60 && !jumped; y += 2) {
        for (int x = 2; x < 400 && !jumped; x += 2) {
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::ControlModifier, QPoint(x, y));
            jumped = editor->cursorOffset() == body;
            if (!jumped)
                editor->setCursorOffset(kFootnoteNote.size());
        }
    }
    EXPECT_TRUE(jumped);
}

TEST(RuleTest, AThematicBreakIsDrawnAsALine)
{
    auto editor = editorWith(QStringLiteral("above\n\n---\n\nbelow\n"));
    editor->setCursorOffset(editor->noteText().size()); // off the rule's line
    editor->show();
    QApplication::processEvents();
    const QImage shot = editor->viewport()->grab().toImage();
    const QColor background = shot.pixelColor(2, shot.height() - 2);
    // A line: some row where most of the text column is one colour that is
    // not the background.
    bool found = false;
    for (int y = 0; y < shot.height() && !found; ++y) {
        QHash<QRgb, int> counts;
        for (int x = 0; x < shot.width(); ++x)
            ++counts[shot.pixel(x, y)];
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            if (QColor(it.key()) != background && it.value() > shot.width() / 2)
                found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(CodeBlockHookTest, APluginLanguageIsDrawnAsItsRendererAnswers)
{
    auto editor = editorWith(QStringLiteral("```calc\n2+2\n```\n\n```text\nplain\n```\n\nafter\n"));
    editor->setCursorOffset(editor->noteText().size()); // off both fences
    const qsizetype calc = editor->noteText().indexOf(QLatin1String("2+2"));
    QStringList asked;
    editor->setCodeBlockRenderer({QStringLiteral("CALC")}, [&](const QString &language, const QString &source, int width,
                                                               const QString &theme, const auto &deliver) {
        asked << language + QLatin1Char('|') + source.trimmed() + QLatin1Char('|') + theme;
        QImage image(width, 30, QImage::Format_ARGB32);
        image.fill(Qt::red);
        // From another thread, as a plugin host's answer arrives.
        (void)QtConcurrent::run([deliver, image] { deliver({false, QString(), image}); });
    });
    WAIT_TRUE(!editor->embedImageSizeAt(calc).isEmpty(), 2000);
    EXPECT_EQ(editor->embedImageSizeAt(calc).height(), 30);
    EXPECT_EQ(asked, QStringList{QStringLiteral("calc|2+2|dark")});
    // Another language stays a listing.
    EXPECT_TRUE(editor->embedImageSizeAt(editor->noteText().indexOf(QLatin1String("plain"))).isEmpty());

    // Markdown text is drawn formatted; unregistering brings the listing back.
    editor->setCodeBlockRenderer({QStringLiteral("calc")}, [](const QString &, const QString &, int, const QString &,
                                                              const auto &deliver) {
        deliver({true, QStringLiteral("**4**"), QImage()});
    });
    WAIT_TRUE(!editor->embedImageSizeAt(calc).isEmpty(), 2000);
    editor->setCodeBlockRenderer({QStringLiteral("calc")}, nullptr);
    EXPECT_TRUE(editor->embedImageSizeAt(calc).isEmpty());
}

TEST(NoteEmbedTest, ANoteEmbedIsDrawnFormattedAMomentLater)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("Target.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("# Heading\n\nSome **bold** text.\n");
    }
    auto editor = editorWith(QStringLiteral("![[Target]]\n\nafter\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    editor->setCursorOffset(editor->noteText().size()); // off the embed's line
    // Text at once, so layout never waits on the formatted drawing...
    EXPECT_TRUE(editor->panelTextAt(0).contains(QStringLiteral("bold")));
    EXPECT_TRUE(editor->embedImageSizeAt(0).isEmpty());
    // ...which replaces it a turn of the event loop later.
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 2000);
    EXPECT_TRUE(editor->panelTextAt(0).isEmpty());
    EXPECT_GT(editor->embedImageSizeAt(0).height(), 20);
}

// --- a hooked embed's own input: wheel, drag, click (setFileEmbedInputHandler) --

namespace {
/// A registered renderer that answers with one fixed-colour image, delivered
/// through the same queued path the real one (deliverFileEmbed) uses, so a
/// test still needs a WAIT_TRUE turn for the cold render exactly like every
/// other hooked/async embed test does.
void registerFixedImageEmbed(MarkdownEditor *editor, const QString &suffix)
{
    editor->setFileEmbedRenderer({suffix}, [](const QString &, const QString &, int width,
                                              const std::function<void(QImage)> &deliver) {
        QImage image(width, 200, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        deliver(image);
    });
}
} // namespace

TEST(HookedEmbedInputTest, CtrlWheelIsForwardedAndAcceptedWhilePlainWheelIsDeclined)
{
    // Tall enough to scroll: the plain-wheel half of this test needs
    // somewhere for the note itself to go.
    QString text = QStringLiteral("![[board.plane]]\n\n");
    for (int i = 0; i < 60; ++i)
        text += QStringLiteral("Paragraph %1.\n\n").arg(i);
    auto editor = editorWith(text);
    editor->setLinkResolver([](const QString &) { return QStringLiteral("board.plane"); });
    registerFixedImageEmbed(editor.get(), QStringLiteral("plane"));
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 2000);

    struct Call {
        MarkdownEditor::FileEmbedInputKind kind;
        QPoint delta;
        Qt::KeyboardModifiers modifiers;
    };
    QList<Call> calls;
    editor->setFileEmbedInputHandler(
        {QStringLiteral("plane")},
        [&calls](const QString &, const QString &, const MarkdownEditor::FileEmbedInputEvent &event) {
            calls.append({event.kind, event.delta, event.modifiers});
            MarkdownEditor::FileEmbedInputResult result;
            if (event.kind == MarkdownEditor::FileEmbedInputKind::Wheel
                && !event.modifiers.testFlag(Qt::ControlModifier))
                return result; // declined: a plain wheel is not this embed's to keep
            result.image = QImage(4, 4, QImage::Format_ARGB32_Premultiplied);
            result.image.fill(Qt::blue);
            return result;
        });

    const QRect rect = editor->hookedEmbedImageRectAt(0);
    ASSERT_FALSE(rect.isEmpty());
    const QPoint centre = rect.center();

    QWheelEvent ctrlWheel(QPointF(centre), QPointF(centre), QPoint(0, 0), QPoint(0, 240), Qt::NoButton,
                         Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(editor->viewport(), &ctrlWheel);
    ASSERT_EQ(calls.size(), 1);
    EXPECT_EQ(calls.last().kind, MarkdownEditor::FileEmbedInputKind::Wheel);
    EXPECT_EQ(calls.last().modifiers, Qt::ControlModifier);
    EXPECT_TRUE(ctrlWheel.isAccepted()) << "a Ctrl+wheel the embed used must not also scroll the note";

    const int scrollBefore = editor->verticalScrollBar()->value();
    QWheelEvent plainWheel(QPointF(centre), QPointF(centre), QPoint(0, 0), QPoint(0, -240), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(editor->viewport(), &plainWheel);
    ASSERT_EQ(calls.size(), 2);
    EXPECT_EQ(calls.last().kind, MarkdownEditor::FileEmbedInputKind::Wheel);
    EXPECT_EQ(calls.last().modifiers, Qt::KeyboardModifiers());
    // Declined by the handler above, so the editor's own wheelEvent falls
    // through to its normal handling instead of leaving the page stuck.
    EXPECT_NE(editor->verticalScrollBar()->value(), scrollBefore);
}

TEST(HookedEmbedInputTest, ADragPansInsteadOfSelectingAndAPlainClickOpensACard)
{
    auto editor = editorWith(QStringLiteral("![[board.plane]]\n"));
    editor->setLinkResolver([](const QString &) { return QStringLiteral("board.plane"); });
    registerFixedImageEmbed(editor.get(), QStringLiteral("plane"));
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 2000);

    QList<QPoint> drags;
    QString clickedPath;
    editor->setFileEmbedInputHandler(
        {QStringLiteral("plane")},
        [&](const QString &, const QString &, const MarkdownEditor::FileEmbedInputEvent &event) {
            MarkdownEditor::FileEmbedInputResult result;
            if (event.kind == MarkdownEditor::FileEmbedInputKind::Drag) {
                drags.append(event.delta);
                result.image = QImage(4, 4, QImage::Format_ARGB32_Premultiplied);
            } else if (event.kind == MarkdownEditor::FileEmbedInputKind::Click) {
                result.openPath = QStringLiteral("Card.md");
            }
            return result;
        });

    QString activated;
    QObject::connect(editor.get(), &MarkdownEditor::fileEmbedActivated,
                     [&](const QString &path) { activated = path; });

    const QRect rect = editor->hookedEmbedImageRectAt(0);
    const QPoint start = rect.center();
    const QPoint moved = start + QPoint(30, 12);

    QTest::mousePress(editor->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, moved, editor->viewport()->mapToGlobal(moved), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor->viewport(), &move);
    QTest::mouseRelease(editor->viewport(), Qt::LeftButton, Qt::NoModifier, moved);

    ASSERT_FALSE(drags.isEmpty()) << "a real drag pans the embed";
    EXPECT_EQ(drags.first(), QPoint(30, 12));
    EXPECT_TRUE(editor->selectedText().isEmpty()) << "a drag over the embed must not select the note's text";
    EXPECT_TRUE(activated.isEmpty()) << "a drag is not a click";

    // A plain click (no movement) on the same embed opens its card instead.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    EXPECT_EQ(activated, QStringLiteral("Card.md"));
}

TEST(HookedEmbedInputTest, DoubleClickingEmptyEmbedSpaceOpensTheFileItself)
{
    auto editor = editorWith(QStringLiteral("![[board.plane]]\n"));
    editor->setLinkResolver([](const QString &) { return QStringLiteral("board.plane"); });
    registerFixedImageEmbed(editor.get(), QStringLiteral("plane"));
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 2000);

    editor->setFileEmbedInputHandler(
        {QStringLiteral("plane")},
        [](const QString &, const QString &, const MarkdownEditor::FileEmbedInputEvent &event) {
            MarkdownEditor::FileEmbedInputResult result;
            if (event.kind == MarkdownEditor::FileEmbedInputKind::DoubleClick)
                result.openPath = QStringLiteral("board.plane");
            return result;
        });

    QString activated;
    QObject::connect(editor.get(), &MarkdownEditor::fileEmbedActivated,
                     [&](const QString &path) { activated = path; });

    const QPoint centre = editor->hookedEmbedImageRectAt(0).center();
    QTest::mouseDClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, centre);
    EXPECT_EQ(activated, QStringLiteral("board.plane"));
}

// --- feature switches: one on/off pair per feature --------------------------

TEST(FeaturesTest, QueryBlocksToggle)
{
    const QString note = QStringLiteral("```query\nhello\n```\n");
    auto editor = editorWith(note);
    // No runner needed: even "No search source connected" is the query
    // machinery's own message, drawn through a substituted QTextLayout - so
    // displayText() (which only ever reflects view.map) shows nothing for
    // this block, not the fence's raw "hello". panelTextAt is query.cpp's
    // own message text; displayText is enough to prove the fence's content
    // is not leaking through as plain text.
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("hello")))
        << "on: rendered through the query machinery, not as a plain fence";

    MarkdownEditor::Features off;
    off.queryBlocks = false;
    editor->setFeatures(off);
    // Fence lines are syntax, hidden in live preview like any other marker;
    // the fence's own content showing through plainly (mapped, not
    // substituted) is what "off" actually looks like.
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("hello")));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("Searching")));
}

TEST(FeaturesTest, BaseBlocksToggle)
{
    const QString note = QStringLiteral("```base\nviews:\n  - type: table\n```\n");
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("views:")))
        << "on: rendered through the base machinery (\"No base source connected\" with no "
           "runner), not as a plain fence";

    MarkdownEditor::Features off;
    off.baseBlocks = false;
    editor->setFeatures(off);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("views:")))
        << "off: the fence's own content shows as a plain listing";
}

#ifdef NOTEAPPC_EXPECT_HTML
TEST(FeaturesTest, HTMLblocksToggle)
{
    // style= keeps it litehtml's: a plain <div> the editor lays out itself.
    const QString note = QStringLiteral("<div style=\"color: red\">hello</div>\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(editor->htmlImageSizeAt(0).isValid()) << "on by default";

    MarkdownEditor::Features off;
    off.HTMLblocks = false;
    editor->setFeatures(off);
    EXPECT_FALSE(editor->htmlImageSizeAt(0).isValid());
}
#endif

#ifdef NOTEAPPC_EXPECT_PDF
TEST(FeaturesTest, PDFembedsToggle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString pdf = makeTestPdf(dir.path(), 1);
    auto editor = editorWith(QStringLiteral("![[deck.pdf]]\n"));
    editor->setLinkResolver([&](const QString &) { return pdf; });
    EXPECT_TRUE(editor->pdfEmbedInfoAt(0).isPdfEmbed);

    MarkdownEditor::Features off;
    off.PDFembeds = false;
    editor->setFeatures(off);
    EXPECT_FALSE(editor->pdfEmbedInfoAt(0).isPdfEmbed);
}
#endif

TEST(FeaturesTest, TexEmbedsToggle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("formula.tex"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("E = mc^2");
    }
    auto editor = editorWith(QStringLiteral("![[formula.tex]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("formula.tex")))
        << "on: rendered - maths alone, through MicroTeX";
    const qreal on = editor->documentHeight();

    MarkdownEditor::Features off;
    off.texEmbeds = false;
    editor->setFeatures(off);
    // Still not the raw link - fileEmbeds (still on) renders it as a source
    // listing instead; panelTextAt is that listing's own text, which
    // displayText() cannot see (see the comment on QueryBlocksToggle).
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("formula.tex")))
        << "off: falls back to a source listing, not to the raw link";
    EXPECT_TRUE(editor->panelTextAt(0).contains(QStringLiteral("E = mc^2")))
        << "off: MicroTeX never ran - this is the file's own bytes, plainly listed";
    EXPECT_NE(editor->documentHeight(), on)
        << "off: a source listing is laid out differently from a rendered formula";
}

TEST(FeaturesTest, MediaEmbedsToggle)
{
    auto editor = editorWith(QStringLiteral("![[song.mp3]]\n"));
    editor->setLinkResolver([](const QString &target) { return QStringLiteral("/nowhere/") + target; });
    const qreal on = editor->documentHeight();

    MarkdownEditor::Features off;
    off.mediaEmbeds = false;
    editor->setFeatures(off);
    const qreal disabled = editor->documentHeight();
    EXPECT_LT(disabled, on) << "off: no player card, just the link";
}

TEST(FeaturesTest, MathsToggle)
{
    auto editor = editorWith(QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n"));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("\\pi")));

    MarkdownEditor::Features off;
    off.maths = false;
    editor->setFeatures(off);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("\\pi")))
        << "off: the raw LaTeX shows, MicroTeX never runs";
}

TEST(FeaturesTest, FileEmbedsToggle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notes.txt"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("plain text\n");
    }
    auto editor = editorWith(QStringLiteral("![[notes.txt]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("notes.txt")))
        << "on: rendered as a listing, the wikilink text is gone";

    MarkdownEditor::Features off;
    off.fileEmbeds = false;
    editor->setFeatures(off);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("notes.txt")))
        << "off: falls back to the plain link";
}

TEST(MarkdownEditorTest, EmbedsASourceFileAsAReadOnlyCodeBlock)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("snippet.cpp"));
    {
        QFile file(source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("int one() { return 1; }\n"
                   "int two() { return 2; }\n"
                   "int three() { return 3; }\n");
    }

    auto editor = editorWith(QStringLiteral("![[snippet.cpp#L2-L3]]\n"));
    editor->setLinkResolver([&](const QString &target) {
        return target == QLatin1String("snippet.cpp") ? source : QString();
    });

    // The embed replaced the text: the wikilink itself is no longer shown.
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("snippet.cpp")));
    // And the buffer is untouched, which is what makes the embed read-only.
    EXPECT_EQ(editor->noteText(), QStringLiteral("![[snippet.cpp#L2-L3]]\n"));
}

TEST(MathRendererTest, RendersLatexWithInk)
{
    EXPECT_TRUE(MathRenderer::isAvailable());

    MathRenderer renderer;
    const QImage image = renderer.render(
        QStringLiteral("\\int_0^\\infty e^{-x^2}\\,dx = \\frac{\\sqrt{\\pi}}{2}"), 20,
        QColor(0xda, 0xda, 0xda), 700, 1.0);

    ASSERT_FALSE(image.isNull()) << renderer.lastError().toStdString();
    EXPECT_GT(image.width(), 40);
    EXPECT_GT(image.height(), 20);

    // Glyphs, not just the fraction rules: count how many rows carry ink.
    int rowsWithInk = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 8) {
                ++rowsWithInk;
                break;
            }
        }
    }
    std::printf("[ MATH     ] %dx%d, %d rows with ink\n", image.width(), image.height(),
                rowsWithInk);
    EXPECT_GT(rowsWithInk, image.height() / 4);

    // MicroTeX is forgiving with a truncated formula - it lays out what it
    // can rather than failing, which is the behaviour we want in a note.
}

TEST(MarkdownEditorTest, DisplayMathBecomesARenderedBlock)
{
    auto editor = editorWith(QStringLiteral("before\n\n$$\nE = mc^2\n$$\n\nafter\n"));

    const QString shown = editor->displayText();
    std::printf("[ MATH     ] display = %s\n", shown.toUtf8().constData());
    // The formula is drawn, so its source is not part of the text on screen.
    EXPECT_FALSE(shown.contains(QStringLiteral("E = mc^2")));
    EXPECT_TRUE(shown.contains(QStringLiteral("before")));
    EXPECT_TRUE(shown.contains(QStringLiteral("after")));
    // And the buffer still holds every character.
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("$$")));
}

TEST(MarkdownEditorTest, InlineMathRendersAsOneCharacterWhenNotActive)
{
    ASSERT_TRUE(MathRenderer::isAvailable());
    const QString source = QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n");
    auto editor = editorWith(source);

    // Everything the formula's image now stands for - the LaTeX and the
    // closing "$" - is gone from the display string; only the opening "$"
    // that anchors the image remains.
    EXPECT_EQ(editor->displayText(), QStringLiteral("Inline $ formula.\n"));
}

TEST(MarkdownEditorTest, InlineMathShowsSourceWhenTheCaretIsInIt)
{
    const QString source = QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n");
    auto editor = editorWith(source);

    // Only the element the caret is in shows its source.
    editor->enterBlockAt(source.indexOf(QLatin1String("pi")));
    EXPECT_EQ(editor->displayText(), source);

    // Elsewhere in the same paragraph the formula stays drawn.
    editor->enterBlockAt(source.indexOf(QLatin1String("formula")));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("\\pi")));
}

TEST(MarkdownEditorTest, InlineMathShowsSourceInSourceMode)
{
    const QString source = QStringLiteral("Inline $e^{i\\pi}+1=0$ formula.\n");
    auto editor = editorWith(source);

    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), source);
}

TEST(MarkdownEditorTest, UnrenderableInlineMathKeepsItsSourceVisible)
{
    // MicroTeX forgives nearly everything - an unknown command, an unclosed
    // brace, a dangling ^ all still render. A bare `&` outside an array is one
    // of the few things it refuses, and a real mistake to make in prose maths.
    // It must fall back to the raw source rather than to an empty gap, the
    // same rule display maths follows.
    const QString source = QStringLiteral("Inline $a & b$ formula.\n");
    auto editor = editorWith(source);

    EXPECT_EQ(editor->displayText(), source);
}

TEST(MathRendererTest, ItsFontsAreReachableAsResources)
{
    // MicroTeX names its fonts by a path relative to the working directory and
    // falls back to ":/<path>", which is why they are compiled in.
    QDirIterator it(QStringLiteral(":/fonts"), QStringList{QStringLiteral("*.ttf")},
                    QDir::Files, QDirIterator::Subdirectories);
    QStringList found;
    while (it.hasNext())
        found << it.next();
    std::printf("[ MATH     ] %lld font resources\n", (long long)found.size());
    ASSERT_FALSE(found.isEmpty());

    // The fonts the large operators come from must be among them, and Qt must
    // actually accept them.
    for (const QString &wanted : {QStringLiteral(":/fonts/base/cmex10.ttf"),
                                  QStringLiteral(":/fonts/maths/cmsy10.ttf"),
                                  QStringLiteral(":/fonts/base/cmmi10.ttf")}) {
        EXPECT_TRUE(found.contains(wanted)) << wanted.toStdString();
        const int id = QFontDatabase::addApplicationFont(wanted);
        EXPECT_NE(id, -1) << wanted.toStdString();
        if (id != -1) {
            std::printf("[ MATH     ] %s -> %s\n", wanted.toUtf8().constData(),
                        QFontDatabase::applicationFontFamilies(id).join(QLatin1Char(','))
                            .toUtf8().constData());
        }
    }
}

TEST(TouchModeTest, WithoutATouchscreenADragStillSelects)
{
    // tests/test_touch.cpp has the other half; this binary has no touchscreen.
    MarkdownEditor editor;
    // hasScroller(), not grabbedGesture(): that one answers the same for a
    // widget nobody grabbed anything on.
    EXPECT_FALSE(QScroller::hasScroller(editor.viewport()));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// A link to a note that does not exist is dimmed, which is the only way a
// typo in a wikilink is visible at all.
TEST(UnresolvedLinkTest, OnlyALinkWithNoNoteBehindItIsDimmed)
{
    MarkdownEditor editor;
    const Theme theme = Theme::builtin(Theme::Mode::Dark);
    editor.setTheme(theme);
    // Only "Real" exists; the external link must stay a normal link.
    editor.setLinkResolver([](const QString &target) {
        return target == QStringLiteral("Real") ? QStringLiteral("/vault/Real.md") : QString();
    });
    editor.setNoteText(QStringLiteral(
        "See [[Real]], [[Missing]], [[Real#Heading]] and [x](https://example.com).\n"));
    editor.resize(900, 600);
    editor.show();
    QTest::qWait(60);

    const QColor accent = theme.color(QStringLiteral("--text-accent"));
    const QColor dim = theme.color(QStringLiteral("--link-unresolved-color"));
    ASSERT_NE(accent, dim);

    const QString text = editor.noteText();
    const auto colourOfLinkAt = [&](const QString &needle) {
        const qsizetype at = text.indexOf(needle);
        EXPECT_GE(at, 0);
        return editor.linkColourAt(at + 1);
    };

    EXPECT_EQ(colourOfLinkAt(QStringLiteral("[[Real]]")), accent);
    EXPECT_EQ(colourOfLinkAt(QStringLiteral("[[Missing]]")), dim);
    // A subpath does not make a resolved note unresolved.
    EXPECT_EQ(colourOfLinkAt(QStringLiteral("[[Real#Heading]]")), accent);
    // An external link resolves to nothing and is not meant to.
    EXPECT_EQ(colourOfLinkAt(QStringLiteral("https://example.com")), accent);
}

// --- what a selection reveals ----------------------------------------------

namespace {

/// A point on the first line of the first block, `dx` pixels into the text.
QPoint firstLinePoint(const MarkdownEditor &editor, int dx)
{
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const qreal fontHeight = QFontMetricsF(Theme::builtin().textFont()).height();
    return QPoint(left + dx, int(28 + fontHeight / 2));
}

/// Mouse events sent the way Qt Widgets delivers them, so a test sees the same
/// sequence a real double-click produces: press, release, double-click,
/// release - the second press arrives as the double-click, not as a press.
void send(QWidget *target, QEvent::Type type, QPoint at, Qt::MouseButtons held)
{
    const Qt::MouseButton button =
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, QPointF(at), target->mapToGlobal(QPointF(at)), button, held,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

const QString kRevealNote = QStringLiteral("Some **bold** words to select across.\n\nSecond.\n");

} // namespace

TEST(RevealTest, DraggingASelectionNeverRevealsSyntax)
{
    // Obsidian keeps rendering while a selection is dragged. Revealing the
    // markers mid-drag moved the text out from under the pointer.
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();
    ASSERT_FALSE(rendered.contains(QLatin1String("**")));

    QWidget *viewport = editor->viewport();
    send(viewport, QEvent::MouseButtonPress, firstLinePoint(*editor, 4), Qt::LeftButton);
    EXPECT_EQ(editor->displayText(), rendered) << "nothing may move while the button is down";
    send(viewport, QEvent::MouseMove, firstLinePoint(*editor, 180), Qt::LeftButton);
    EXPECT_EQ(editor->displayText(), rendered);
    send(viewport, QEvent::MouseButtonRelease, firstLinePoint(*editor, 180), Qt::NoButton);

    EXPECT_FALSE(editor->selectedText().isEmpty());
    EXPECT_EQ(editor->displayText(), rendered) << "a finished selection stays rendered too";
}

TEST(RevealTest, APlainClickRevealsOnRelease)
{
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();

    // Aim inside "bold": a caret shows the markers of the element it is in.
    const qreal advance = QFontMetricsF(Theme::builtin().textFont())
                              .horizontalAdvance(rendered.left(rendered.indexOf(QLatin1String("bold")) + 2));
    const QPoint onBold = firstLinePoint(*editor, int(advance));
    QWidget *viewport = editor->viewport();
    send(viewport, QEvent::MouseButtonPress, onBold, Qt::LeftButton);
    EXPECT_EQ(editor->displayText(), rendered);
    send(viewport, QEvent::MouseButtonRelease, onBold, Qt::NoButton);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("**")))
        << "a bare caret in an element shows that element's markers";
}

TEST(RevealTest, AKeyboardSelectionRendersAgain)
{
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();
    editor->enterBlockAt(kRevealNote.indexOf(QLatin1String("bold")) + 1);
    ASSERT_TRUE(editor->displayText().contains(QLatin1String("**")));

    QTest::keyClick(editor.get(), Qt::Key_End, Qt::ShiftModifier);
    EXPECT_FALSE(editor->selectedText().isEmpty());
    EXPECT_EQ(editor->displayText(), rendered);
}

TEST(RevealTest, DoubleClickSelectsTheWordThatWasAimedAt)
{
    // The first click's release reveals the block, which shifts everything
    // after the markers four characters to the right. The word selected has
    // to be the one under the pointer before that happened.
    auto editor = editorWith(kRevealNote);
    const QString rendered = editor->displayText();
    const qsizetype wordsAt = rendered.indexOf(QLatin1String("words"));
    ASSERT_GT(wordsAt, 0);
    const qreal advance = QFontMetricsF(Theme::builtin().textFont())
                              .horizontalAdvance(rendered.left(wordsAt + 2));
    const QPoint onWords = firstLinePoint(*editor, int(advance));

    QWidget *viewport = editor->viewport();
    send(viewport, QEvent::MouseButtonPress, onWords, Qt::LeftButton);
    send(viewport, QEvent::MouseButtonRelease, onWords, Qt::NoButton);
    send(viewport, QEvent::MouseButtonDblClick, onWords, Qt::LeftButton);
    send(viewport, QEvent::MouseButtonRelease, onWords, Qt::NoButton);

    EXPECT_EQ(editor->selectedText(), QStringLiteral("words"));
}

// --- style files reload live ----------------------------------------------

namespace {

/// Pixels of exactly this colour in the editor as it would paint now. The
/// callout rail is the only thing drawn in a pure primary, so counting its
/// pixels is how a test sees which style the editor resolved.
int pixelsOf(MarkdownEditor &editor, QRgb colour)
{
    const QImage frame = editor.grab().toImage().convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x)
            count += (row[x] & 0xFFFFFF) == (colour & 0xFFFFFF);
    }
    return count;
}

void writeAtomically(const QString &path, const QByteArray &bytes)
{
    // Write a new file and rename it over the old one, the way most editors
    // save. inotify drops a watch on the renamed-over file, which is exactly
    // the case the reload has to survive.
    const QString temporary = path + QStringLiteral(".new");
    QFile file(temporary);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
    file.close();
    QFile::remove(path);
    ASSERT_TRUE(QFile::rename(temporary, path));
}

} // namespace

TEST(StyleFilesTest, EditingCalloutsIniRestylesWithoutARestart)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("notolith")));
    const QString ini = QDir(home.path()).filePath(QStringLiteral("notolith/callouts.ini"));
    writeAtomically(ini, "[note]\nbar = #FF0000ff\n");

    auto editor = editorWith(QStringLiteral("> [!note] Title\n> Body.\n"));
    EXPECT_GT(pixelsOf(*editor, qRgb(255, 0, 0)), 0) << "the file's colour at start";

    writeAtomically(ini, "[note]\nbar = #0000FFff\n");
    QElapsedTimer waited;
    waited.start();
    while (pixelsOf(*editor, qRgb(0, 0, 255)) == 0 && waited.elapsed() < 3000)
        QTest::qWait(20);
    EXPECT_GT(pixelsOf(*editor, qRgb(0, 0, 255)), 0) << "reloaded after a rename-over save";
    EXPECT_EQ(pixelsOf(*editor, qRgb(255, 0, 0)), 0) << "and the old colour is gone";

    // A second save still reaches it: the rename took the file's watch away,
    // and the reload has to have put it back.
    writeAtomically(ini, "[note]\nbar = #00FF00ff\n");
    waited.restart();
    while (pixelsOf(*editor, qRgb(0, 255, 0)) == 0 && waited.elapsed() < 3000)
        QTest::qWait(20);
    EXPECT_GT(pixelsOf(*editor, qRgb(0, 255, 0)), 0);

    qputenv("XDG_CONFIG_HOME", previous);
}

// The config directory is also where the settings are saved, on every note switch: only the
// style files' own change may re-lay the note out (a relayout in the middle of restoring a
// scroll position put the text two lines off).
TEST(StyleFilesTest, SavingOtherFilesInTheConfigDirectoryDoesNotLayTheNoteOutAgain)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("notolith")));
    const QString ini = QDir(home.path()).filePath(QStringLiteral("notolith/callouts.ini"));
    writeAtomically(ini, "[note]\nbar = #FF0000ff\n");

    auto editor = editorWith(QStringLiteral("> [!note] Title\n> Body.\n"));
    QTest::qWait(100);
    const quint64 before = editor->layoutCount();

    for (const char *other : {"notolith/settings.ini", "notolith/hotkeys.json", "notolith/plugins.ini"})
        writeAtomically(QDir(home.path()).filePath(QString::fromLatin1(other)), "[state]\nrecent-vaults = /x\n");
    QTest::qWait(400);
    EXPECT_EQ(editor->layoutCount(), before) << "a settings save laid the note out again";

    writeAtomically(ini, "[note]\nbar = #0000FFff\n");
    EXPECT_TRUE(QTest::qWaitFor([&] { return editor->layoutCount() > before; }, 3000)) << "the style file's own change still does";
    qputenv("XDG_CONFIG_HOME", previous);
}

// --- the code block label -------------------------------------------------

namespace {

/// Clicks across the top-right of the first code block until something lands
/// on the clipboard. Sweeping, rather than computing the label's rectangle,
/// keeps the test from restating the editor's spacing - it only asserts that
/// the label is up there and does the right thing when hit.
bool clickLabelUntilCopied(MarkdownEditor &editor, int bottom = 110)
{
    QGuiApplication::clipboard()->clear();
    const int viewport = editor.viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    const int right = left + column + 4;
    for (int y = 12; y < bottom; y += 4) {
        for (int x = right - 6; x > right - 140; x -= 6) {
            // "Copy" only shows while the pointer is over the block. (Sent
            // by hand: QTest::mouseMove needs an exposed window.)
            QMouseEvent move(QEvent::MouseMove, QPointF(x, y), editor.viewport()->mapToGlobal(QPointF(x, y)),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(editor.viewport(), &move);
            QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
            if (!QGuiApplication::clipboard()->text().isEmpty())
                return true;
            // A miss lands in the listing, which then opens as source and
            // has no header: the caret goes back below the block.
            editor.setCursorOffset(editor.noteText().size());
            editor.verticalScrollBar()->setValue(0);
        }
    }
    return false;
}

} // namespace

TEST(CodeLabelTest, ClickingTheLanguageCopiesTheListing)
{
    const QString note = QStringLiteral("```python\nprint(1)\nprint(2)\n```\n\nafter\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(clickLabelUntilCopied(*editor)) << "no label in the block's top-right corner";

    // The listing as the reader sees it: no fences, no language tag, and no
    // trailing newline dragged along from before the closing fence.
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("print(1)\nprint(2)"));
    EXPECT_EQ(editor->noteText(), note) << "copying must not touch the note";
}

TEST(CodeLabelTest, OptionLinesAreNotCopied)
{
    auto editor = editorWith(
        QStringLiteral("```python\n#| title: Setup\nprint(1)\n```\n\nafter\n"));
    ASSERT_TRUE(clickLabelUntilCopied(*editor)) << "no label in the header strip";
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("print(1)"));
}

TEST(NotebookBlockTest, ImplicitCodeTravelsWithTheRunNotWithCopy)
{
    const QString frontmatter = QStringLiteral("---\nimplicit-python: |\n  import math\nimplicit-python-main: |\n  def main():\n  {{code}}\n---\n\n");
    const QString note = frontmatter + QStringLiteral("```python\nprint(math.pi)\n```\n\n"
                                                      "```python\n#| implicit: false\nprint(2)\n```\n\n"
                                                      "```rust\nlet x = 1;\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size());
    FakeNotebook fake;
    fake.ran = textRun(QStringLiteral("ok\n"));
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(math"))));
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("print(2)"))));
    ASSERT_TRUE(editor->runNotebookBlockAt(note.indexOf(QStringLiteral("let x"))));
    ASSERT_EQ(fake.runs.size(), 3);
    EXPECT_EQ(fake.runs[0].implicit, QStringLiteral("import math"));
    EXPECT_EQ(fake.runs[0].implicitMain, QStringLiteral("def main():\n{{code}}"));
    EXPECT_TRUE(fake.runs[1].implicit.isEmpty()); // `#| implicit: false`
    EXPECT_TRUE(fake.runs[1].implicitMain.isEmpty());
    EXPECT_TRUE(fake.runs[2].implicit.isEmpty()); // another language's block gets nothing
    EXPECT_EQ(fake.runSources[0], QStringLiteral("print(math.pi)")); // the listing is the block alone

    // Copy takes the listing, not the note's implicit code.
    auto copying = editorWith(frontmatter + QStringLiteral("```python\nprint(math.pi)\n```\n\nafter\n"));
    ASSERT_TRUE(clickLabelUntilCopied(*copying, 220)) << "no label in the block's header";
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("print(math.pi)"));
}

TEST(NotebookBlockTest, EditingTheImplicitCodeOutdatesTheOutputOfItsBlocks)
{
    const auto noteWith = [](const QString &prelude) {
        return QStringLiteral("---\nimplicit-python: ") + prelude + QStringLiteral("\n---\n\n```python\nprint(1)\n```\n");
    };
    const QString note = noteWith(QStringLiteral("import math"));
    auto editor = editorWith(note);
    editor->setCursorOffset(note.size());
    const qsizetype at = note.indexOf(QStringLiteral("print(1)"));
    // Only a block that ran with `import math` has anything cached.
    editor->setNotebookRunner([](const QString &, const QString &, bool forceRun,
                                 const MarkdownEditor::NotebookRunOptions &options,
                                 const MarkdownEditor::NotebookDeliver &deliver) {
        const MarkdownEditor::NotebookRunResult answer =
            !forceRun && options.implicit == QLatin1String("import math") ? textRun(QStringLiteral("1\n"), 1)
                                                                          : MarkdownEditor::NotebookRunResult();
        QTimer::singleShot(0, [deliver, answer] { deliver(answer); });
    });
    QTest::qWait(300);
    auto info = editor->notebookInfoAt(at);
    ASSERT_TRUE(info.hasRun);
    EXPECT_FALSE(info.stale);

    const QString edited = noteWith(QStringLiteral("import os"));
    editor->setNoteText(edited);
    QTest::qWait(300);
    info = editor->notebookInfoAt(edited.indexOf(QStringLiteral("print(1)")));
    EXPECT_TRUE(info.stale);
}

// --- folding a callout inside a callout -------------------------------------

namespace {

/// Where the parser says the nested callout starts - the key its fold state
/// lives under - rather than an offset the test would have to guess.
qsizetype nestedCalloutStart(const QString &text)
{
    const Decorations decorations = Parser().parseAll(text).decorations;
    for (const CalloutSpan &callout : decorations.callouts) {
        if (callout.depth > 0)
            return callout.range.start;
    }
    return -1;
}

} // namespace

TEST(NestedCalloutTest, AMinusStartsFoldedAndOpensAgain)
{
    // The empty `>` line is what steps back out to the outer callout. Without
    // it, `> Back out.` would be a lazy continuation of the inner paragraph -
    // CommonMark's rule, which md4qt and Obsidian both follow - and folding
    // would rightly hide it with the rest of the inner callout.
    const QString note = QStringLiteral(
        "> [!tip] Outer\n> Outer body.\n> > [!info]- Inner\n> > Hidden body.\n>\n> Back out.\n");
    auto editor = editorWith(note);
    const qsizetype inner = nestedCalloutStart(note);
    ASSERT_GE(inner, 0);

    EXPECT_TRUE(editor->isFoldedAt(inner));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("Hidden body")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Inner")))
        << "the title stays, to be clicked open";
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Back out")))
        << "folding the inner callout must not swallow the outer one's text";

    editor->toggleFoldAt(inner);
    EXPECT_FALSE(editor->isFoldedAt(inner));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Hidden body")));
    EXPECT_EQ(editor->noteText(), note) << "folding is a view, never an edit";
}

TEST(NestedCalloutTest, AFoldIsRememberedAcrossNotes)
{
    const QString note = QStringLiteral(
        "> [!tip] Outer\n> Outer body.\n> > [!info]- Inner\n> > Hidden body.\n>\n> Back out.\n");
    auto editor = editorWith(note);
    const qsizetype inner = nestedCalloutStart(note);
    ASSERT_GE(inner, 0);
    editor->toggleFoldAt(inner);
    ASSERT_FALSE(editor->isFoldedAt(inner));
    const QStringList state = editor->foldState();
    editor->setNoteText(note);
    ASSERT_TRUE(editor->isFoldedAt(inner));
    editor->setFoldState(state);
    EXPECT_FALSE(editor->isFoldedAt(inner));
}

TEST(NestedCalloutTest, APlusStartsOpenAndFolds)
{
    const QString note = QStringLiteral(
        "> [!tip] Outer\n> Outer body.\n> > [!info]+ Inner\n> > Shown body.\n");
    auto editor = editorWith(note);
    const qsizetype inner = nestedCalloutStart(note);
    ASSERT_GE(inner, 0);

    EXPECT_FALSE(editor->isFoldedAt(inner));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Shown body")));
    editor->toggleFoldAt(inner);
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("Shown body")));
}

namespace {

/// Pixels in a code block's title - the top left of the first block - where
/// one channel (0 red, 1 green, 2 blue) clearly outweighs the other two.
/// Not an exact colour match: the title is antialiased text, and only a few of
/// its pixels ever land on the exact colour - how many depends on the state of
/// Qt's glyph cache, which is why an exact count passed alone and failed after
/// other tests had drawn text first.
int labelPixelsDominatedBy(MarkdownEditor &editor, int channel)
{
    const QImage frame = editor.grab().toImage().convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int y = 0; y < qMin(frame.height(), 110); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < frame.width() * 2 / 5; ++x) {
            const int c[3] = {qRed(row[x]), qGreen(row[x]), qBlue(row[x])};
            if (c[channel] - qMax(c[(channel + 1) % 3], c[(channel + 2) % 3]) > 70)
                ++count;
        }
    }
    return count;
}

} // namespace

TEST(CodeLabelTest, TheFenceItselfOverridesCodeblocksIni)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("notolith")));
    writeAtomically(QDir(home.path()).filePath(QStringLiteral("notolith/codeblocks.ini")),
                    "[python]\nname = Python\ncolor = #0000FFff\n");

    // The file's colour, then the fence's own, the same three layers a
    // callout has: the language, codeblocks.ini, the block itself.
    auto fromFile = editorWith(QStringLiteral("```python\nx = 1\n```\n"));
    EXPECT_GT(labelPixelsDominatedBy(*fromFile, 2), 3);

    auto overridden = editorWith(QStringLiteral("```python color=#FF0000ff name=Setup\nx = 1\n```\n"));
    EXPECT_GT(labelPixelsDominatedBy(*overridden, 0), 3);
    EXPECT_EQ(labelPixelsDominatedBy(*overridden, 2), 0);
    EXPECT_FALSE(overridden->displayText().contains(QLatin1String("color=")))
        << "the attributes are hidden with the fence";

    qputenv("XDG_CONFIG_HOME", previous);
}

TEST(ShiftTest, ACalloutBelowTheCaretKeepsItsPanelWhileTyping)
{
    // A guard, not a regression that happened. Between a keystroke and the
    // debounced reparse only syntax, styles, blocks and properties are
    // shifted; callout ranges are not. It holds anyway because a block below
    // the edit keeps its cached layout and its pointer to the callout, so the
    // stale range is never consulted - unless that block is laid out again in
    // those 20 ms. If a change ever relays out every block per keystroke,
    // this is what will notice the callouts vanishing.
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("notolith")));
    writeAtomically(QDir(home.path()).filePath(QStringLiteral("notolith/callouts.ini")),
                    "[note]\nbar = #FF0000ff\n");

    auto editor = editorWith(QStringLiteral("Typing here.\n\n> [!note] Title\n> Body.\n"));
    ASSERT_GT(pixelsOf(*editor, qRgb(255, 0, 0)), 0);

    editor->enterBlockAt(0);
    QTest::keyClicks(editor.get(), QStringLiteral("abc"));
    // Straight away, before the parse has had its 20 ms.
    EXPECT_GT(pixelsOf(*editor, qRgb(255, 0, 0)), 0) << "the callout's rail vanished mid-typing";

    qputenv("XDG_CONFIG_HOME", previous);
}

TEST(ShiftTest, TypingDoesNotRunIntoTheNextBlock)
{
    // The edited block's end was shifted twice, once by shiftRanges and once
    // more by applyEdit, so until the reparse it showed the start of the next
    // block as well - a flicker of duplicated text on every keystroke.
    const QString note = QStringLiteral("abc\n\ndef **x**\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(1);
    editor->grab();
    for (int i = 0; i < 5; ++i) {
        QTest::keyClick(editor.get(), Qt::Key_Y);
        editor->grab();
    }
    // Before the parse has had its 20 ms.
    EXPECT_EQ(editor->displayText(), QStringLiteral("ayyyyybc\n\ndef x\n"));
}

TEST(ShiftTest, TheNextItemKeepsItsBulletWhileTyping)
{
    // List items are found by a binary search on their starts, so they have
    // to move with the text: a stale start would leave every item after the
    // edit out of its block, and its bullet undrawn, until the reparse.
    auto editor = editorWith(QStringLiteral("- one\n- two\n- three\n"));
    editor->enterBlockAt(3);
    const QImage before = editor->grab().toImage();
    // Longer than an item, or a stale slice can pick up the item after the
    // one it lost and draw its bullet in the same place.
    QTest::keyClicks(editor.get(), QStringLiteral("abcdefghijkl"));
    const QImage after = editor->grab().toImage(); // before the 20 ms reparse
    // Only the edited line may change; the rows of the other two items, their
    // bullets included, must come out exactly as they were.
    ASSERT_EQ(before.size(), after.size());
    int first = -1;
    int last = -1;
    for (int y = 0; y < before.height(); ++y) {
        if (std::memcmp(before.constScanLine(y), after.constScanLine(y),
                        size_t(before.bytesPerLine()))
            != 0) {
            if (first < 0)
                first = y;
            last = y;
        }
    }
    ASSERT_GE(first, 0) << "typing drew nothing";
    EXPECT_LT(last - first, 30) << "rows below the edited line changed";
}

// --- code block tabs ---------------------------------------------------------

namespace {

const QString kTabbedNote = QStringLiteral(
    "Before.\n\n"
    "```python tab=\"Python\"\nprint('hi')\n```\n\n"
    "```cpp tab\nputs(\"hi\");\n```\n\n"
    "After.\n");

} // namespace

TEST(CodeTabTest, OnlyTheSelectedTabIsShown)
{
    auto editor = editorWith(kTabbedNote);
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("print('hi')")));
    EXPECT_FALSE(shown.contains(QLatin1String("puts(\"hi\")")));
    EXPECT_TRUE(shown.contains(QLatin1String("Before.")));
    EXPECT_TRUE(shown.contains(QLatin1String("After.")))
        << "the group hides its own fences, nothing around it";

    editor->selectCodeTab(kTabbedNote.indexOf(QLatin1String("```python")), 1);
    const QString second = editor->displayText();
    EXPECT_FALSE(second.contains(QLatin1String("print('hi')")));
    EXPECT_TRUE(second.contains(QLatin1String("puts(\"hi\")")));
    EXPECT_EQ(editor->noteText(), kTabbedNote) << "switching tabs is a view, never an edit";
}

TEST(CodeTabTest, TheCaretInsideTheGroupShowsEveryFence)
{
    // So the caret can never be inside a tab nobody can see.
    auto editor = editorWith(kTabbedNote);
    editor->enterBlockAt(kTabbedNote.indexOf(QLatin1String("print")));
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("print('hi')")));
    EXPECT_TRUE(shown.contains(QLatin1String("puts(\"hi\")")));
}

TEST(CodeTabTest, TheCaretInsideTheGroupShowsPlainSourceWithoutPanels)
{
    auto editor = editorWith(kTabbedNote);
    FakeNotebook fake;
    editor->setNotebookRunner(fake.runner());
    QTest::qWait(50);
    ASSERT_TRUE(editor->notebookInfoAt(kTabbedNote.indexOf(QLatin1String("print"))).eligible);
    const qsizetype puts = kTabbedNote.indexOf(QLatin1String("puts"));
    editor->enterBlockAt(kTabbedNote.indexOf(QLatin1String("print")));

    // Fences and tab attributes as text; nothing to run until the caret leaves.
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("```python tab=\"Python\"")));
    EXPECT_TRUE(shown.contains(QLatin1String("```cpp tab")));
    EXPECT_FALSE(editor->notebookInfoAt(puts).eligible);
    // The tab that was showing its run control a moment ago loses it too.
    EXPECT_FALSE(editor->notebookInfoAt(kTabbedNote.indexOf(QLatin1String("print"))).eligible);

    editor->show();
    const QImage shot = editor->viewport()->grab().toImage();
    int checked = 0;
    for (const auto &box : editor->blockBoxes()) {
        if (box.kind != BlockSpan::Kind::Code)
            continue;
        const int y = int(box.rect.center().y());
        EXPECT_EQ(shot.pixel(editor->textColumn() - 12, y), shot.pixel(2, y)) << "panel behind a member at y=" << y;
        ++checked;
    }
    EXPECT_EQ(checked, 2);
}

// A single listing with a cached output, and the lookups to read its pieces
// back while the caret goes in and out of it.
struct EditedListing {
    const QString note = QStringLiteral("Before.\n\n```python\nprint(1)\nprint(2)\n```\n\nAfter.\n");
    std::unique_ptr<MarkdownEditor> editor = editorWith(note);
    FakeNotebook fake;
    qsizetype inside = note.indexOf(QLatin1String("print(2)"));

    EditedListing()
    {
        fake.lookup = textRun(QStringLiteral("1\n"), 3);
        editor->setNotebookRunner(fake.runner());
        editor->show();
        editor->grab();
        const qsizetype offset = note.indexOf(QLatin1String("print(1)"));
        EXPECT_TRUE(QTest::qWaitFor([&] { return editor->notebookInfoAt(offset).outputCellCount > 0; }, 2000));
    }
    QRectF codeBox() const
    {
        for (const auto &box : editor->blockBoxes()) {
            if (box.kind == BlockSpan::Kind::Code)
                return box.rect;
        }
        return {};
    }
    QImage shot() const { return editor->viewport()->grab().toImage(); }
    QRgb pixelAt(const QImage &image, int x, qreal y) const { return image.pixel(x, int(y)); }
};

TEST(CodeTabTest, ACaretInASingleListingKeepsItsPanelAndShowsItsFencesOnIt)
{
    // The panel and its header strip were drawn over
    // the opened fence lines, half hiding "```python" above the listing. The
    // panel now reaches out over both fence lines instead, with no strip.
    EditedListing listing;
    const QRectF rest = listing.codeBox();
    const QImage before = listing.shot();
    const int margin = listing.editor->textColumn() - 12;
    ASSERT_NE(listing.pixelAt(before, margin, rest.center().y()), listing.pixelAt(before, 2, rest.center().y()))
        << "a listing at rest sits on its panel";

    listing.editor->enterBlockAt(listing.inside);
    EXPECT_TRUE(listing.editor->displayText().contains(QLatin1String("```python")));
    const QRectF open = listing.codeBox();
    const QImage shot = listing.shot();
    const QRgb panel = listing.pixelAt(shot, margin, open.center().y());
    EXPECT_NE(panel, listing.pixelAt(shot, 2, open.center().y())) << "the listing keeps its panel";
    // The two fence lines sit inside it: the line above the listing and the
    // one below, at the left margin where no text is drawn.
    EXPECT_EQ(listing.pixelAt(shot, margin, open.top() - 6), panel) << "opening fence line";
    EXPECT_EQ(listing.pixelAt(shot, margin, open.bottom() + 6), panel) << "closing fence line";
    // ... and it does not reach past them.
    EXPECT_NE(listing.pixelAt(shot, margin, open.top() - 40), panel);
    EXPECT_NE(listing.pixelAt(shot, margin, open.bottom() + 40), panel);
    // The strip went; what that costs the block is that strip and no more.
    EXPECT_GT(rest.height() - open.height(), 0);
    EXPECT_LE(rest.height() - open.height(), 40);
}

TEST(CodeTabTest, TheLastOutputSurvivesTheCaretEnteringASingleListing)
{
    EditedListing listing;
    const qsizetype print1 = listing.note.indexOf(QLatin1String("print(1)"));
    const auto atRest = listing.editor->notebookInfoAt(print1);
    ASSERT_TRUE(atRest.eligible);
    ASSERT_GT(atRest.outputCellCount, 0);

    listing.editor->enterBlockAt(listing.inside);
    const auto edited = listing.editor->notebookInfoAt(print1);
    EXPECT_TRUE(edited.eligible);
    EXPECT_EQ(edited.outputCellCount, atRest.outputCellCount);
    EXPECT_EQ(listing.editor->notebookOutputTextAt(print1), QStringList{QStringLiteral("1\n")});
    EXPECT_EQ(edited.executionCount, 3);

    // Back out: unchanged.
    listing.editor->setCursorOffset(listing.note.size() - 2);
    EXPECT_EQ(listing.editor->notebookInfoAt(print1).outputCellCount, atRest.outputCellCount);
}

TEST(CodeTabTest, TheRunControlOfAnEditedListingStaysOffItsFenceText)
{
    EditedListing listing;
    listing.editor->enterBlockAt(listing.inside);
    const auto info = listing.editor->notebookInfoAt(listing.inside);
    ASSERT_TRUE(info.eligible);
    ASSERT_TRUE(info.runRect.isValid());
    const QRectF open = listing.codeBox();
    // On the opening fence line, whose text is only as wide as it is written.
    const qreal lineBottom = open.top();
    EXPECT_GT(info.runRect.center().y(), lineBottom - 24);
    EXPECT_LT(info.runRect.center().y(), lineBottom);
    const QFontMetricsF metrics(listing.editor->font());
    const QRectF fenceText(listing.editor->textColumn(), lineBottom - 24,
                           metrics.horizontalAdvance(QStringLiteral("```python")) * 2, 24);
    EXPECT_FALSE(fenceText.intersects(info.runRect));
    // Nor does it touch the closing fence or the listing.
    EXPECT_LT(info.runRect.bottom(), lineBottom + 1);
    // It still works from there.
    listing.fake.ran = textRun(QStringLiteral("2\n"), 4);
    const QPoint click = info.runRect.center().toPoint();
    QTest::mouseClick(listing.editor->viewport(), Qt::LeftButton, Qt::NoModifier, click);
    EXPECT_TRUE(QTest::qWaitFor([&] { return !listing.fake.runs.isEmpty(); }, 2000));
}

TEST(CodeTabTest, TheSelectedTabIsAttachedToItsPanel)
{
    auto editor = editorWith(kTabbedNote);
    editor->show();
    const QRectF tab = editor->selectedTabRectAt(kTabbedNote.indexOf(QLatin1String("print")));
    ASSERT_TRUE(tab.isValid());
    const QImage shot = editor->viewport()->grab().toImage();

    // Down a column right of the strip's tabs: header, the hairline under it,
    // then the panel. The hairline is the row that is neither of the other two.
    const int outside = int(tab.right()) + 120;
    const int bottom = int(tab.bottom());
    const QRgb header = shot.pixel(outside, bottom - 8);
    const QRgb panel = shot.pixel(outside, bottom + 8);
    int line = -1;
    for (int y = bottom - 3; y <= bottom + 3; ++y) {
        if (shot.pixel(outside, y) != header && shot.pixel(outside, y) != panel)
            line = y;
    }
    ASSERT_GE(line, 0) << "no hairline under the strip";
    // The tab is part of the listing's panel: the hairline stops under it.
    const int inside = int(tab.center().x());
    EXPECT_EQ(shot.pixel(inside, line), panel);
    EXPECT_EQ(shot.pixel(inside, bottom - 6), panel) << "the tab is filled like the panel";
}

TEST(CodeTabTest, SourceModeShowsEveryFence)
{
    auto editor = editorWith(kTabbedNote);
    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_EQ(editor->displayText(), kTabbedNote);
}

TEST(CodeTabTest, TheSelectionSurvivesTypingAboveTheGroup)
{
    // The chosen tab is keyed by the group's position, which moves with the
    // text - and survives the reparse that follows.
    auto editor = editorWith(kTabbedNote);
    editor->selectCodeTab(kTabbedNote.indexOf(QLatin1String("```python")), 1);
    editor->enterBlockAt(0);
    QTest::keyClicks(editor.get(), QStringLiteral("More "));
    QTest::qWait(80); // past the parse debounce
    editor->enterBlockAt(editor->noteText().indexOf(QLatin1String("After.")));
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("puts(\"hi\")")));
    EXPECT_FALSE(shown.contains(QLatin1String("print('hi')")));
}

// --- revealing per element, as Obsidian does ---------------------------------

TEST(PerElementRevealTest, OnlyTheElementUnderTheCaretShowsItsMarkers)
{
    const QString note = QStringLiteral("One **first** and **second** and `code`.\n");
    auto editor = editorWith(note);

    editor->enterBlockAt(note.indexOf(QLatin1String("first")) + 1);
    EXPECT_EQ(editor->displayText(), QStringLiteral("One **first** and second and code.\n"));

    editor->enterBlockAt(note.indexOf(QLatin1String("code")) + 1);
    EXPECT_EQ(editor->displayText(), QStringLiteral("One first and second and `code`.\n"));

    // Plain text between elements reveals nothing at all.
    editor->enterBlockAt(note.indexOf(QLatin1String(" and ")) + 2);
    EXPECT_EQ(editor->displayText(), QStringLiteral("One first and second and code.\n"));
}

TEST(PerElementRevealTest, TouchingAnElementsEdgeCounts)
{
    // A caret just after the closing marker is still at the element, which is
    // where it sits after typing the last character of it.
    const QString note = QStringLiteral("A **bold** word.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("** word")) + 2);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("**bold**")));
}

TEST(PerElementRevealTest, NestedMarkersJoinTheirElement)
{
    const QString note = QStringLiteral("Very ***strong*** text.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("strong")) + 2);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("***strong***")));
}

TEST(PerElementRevealTest, AHeadingShowsItsHashOnItsOwnLine)
{
    const QString note = QStringLiteral("# Title\n\nBody.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(3);
    EXPECT_TRUE(editor->displayText().startsWith(QLatin1String("# Title")));
}

TEST(PerElementRevealTest, AListKeepsItsBulletsWhileTheCaretIsInTheText)
{
    const QString note = QStringLiteral("- one **x**\n- two\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("one")) + 1);
    EXPECT_EQ(editor->displayText(), QStringLiteral("one x\ntwo\n"))
        << "markers stay hidden and drawn as bullets";

    editor->enterBlockAt(note.indexOf(QLatin1String("x")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("one **x**\ntwo\n"));
}

TEST(PerElementRevealTest, ALinkShowsItsTargetOnlyWithTheCaretInIt)
{
    const QString note = QStringLiteral("See [the docs](https://example.org) here.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("docs")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("[the docs](https://example.org)")));
    editor->enterBlockAt(note.indexOf(QLatin1String("here")));
    EXPECT_EQ(editor->displayText(), QStringLiteral("See the docs here.\n"));
}

TEST(PerElementRevealTest, ACalloutStillShowsItsWholeSource)
{
    // Callouts, tables and code are widgets in Obsidian: clicking into one
    // shows all of its source, not one element of it.
    const QString note = QStringLiteral("> [!note] Title\n> Some **bold** text.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("text")));
    EXPECT_EQ(editor->displayText(), note);
}

TEST(ListPerfTest, TypingInALongListDoesNotDependOnItsLength)
{
    // A list used to be one block, laid out whole on every keystroke and on
    // every caret move into an element: 11.6 ms per keystroke at 1 000 items,
    // over the budget, and 154 ms at 5 000. Now each top-level item is its own
    // block. Measured with a paint after each step - without one, the relayout
    // is deferred and a timing measures nothing, which is exactly how the
    // first version of this guard passed while the cost was 150 ms.
    const auto measure = [](int items, double *typing, double *crossing) {
        QString note;
        for (int i = 0; i < items; ++i)
            note += QStringLiteral("- item %1 **b** x\n").arg(i);
        auto editor = editorWith(note);
        const qsizetype middle = note.indexOf(QStringLiteral("- item %1 ").arg(items / 2));
        const qsizetype bold = note.indexOf(QLatin1String("**b**"), middle);
        editor->enterBlockAt(bold - 3);
        editor->grab();

        QElapsedTimer clock;
        clock.start();
        QTest::keyClick(editor.get(), Qt::Key_Y);
        editor->grab();
        *typing = clock.nsecsElapsed() / 1e6;

        clock.restart();
        editor->enterBlockAt(bold + 3); // into the bold: what is revealed changes
        editor->grab();
        *crossing = clock.nsecsElapsed() / 1e6;
    };

    // Best of three, each figure on its own: load only ever adds time.
    const auto best = [&](int items, double *typing, double *crossing) {
        *typing = *crossing = 1e9;
        for (int run = 0; run < 3; ++run) {
            double t = 0, c = 0;
            measure(items, &t, &c);
            *typing = qMin(*typing, t);
            *crossing = qMin(*crossing, c);
            if (*typing < 8.0 && *crossing < 8.0)
                break; // a run inside the budget needs no repeat
        }
    };

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    double shortTyping = 0, shortCrossing = 0, longTyping = 0, longCrossing = 0;
    best(100, &shortTyping, &shortCrossing);
    best(5000, &longTyping, &longCrossing);
    std::printf("[ PERF     ] list of 100: type %.2f ms, cross %.2f ms; of 5000: type %.2f ms, "
                "cross %.2f ms\n",
                shortTyping, shortCrossing, longTyping, longCrossing);

    EXPECT_LT(longTyping, 8.0);
    EXPECT_LT(longCrossing, 8.0);
    // Fifty times the list must not cost anything like fifty times the step.
    EXPECT_LT(longTyping, qMax(1.0, shortTyping) * 4);
}

TEST(CodeBlockPerfTest, TypingInALongCodeBlockStaysInBudget)
{
    // A fence was one block however long it was, relaid and re-highlighted
    // whole on every keystroke: 26 ms at 1 000 lines, and the async parse
    // hid how much worse 5 000 was. Median of several keystrokes, each with a
    // paint after it.
    const auto measure = [](int lines) {
        QString note = QStringLiteral("# Listing\n\n```cpp\n");
        for (int i = 0; i < lines; ++i)
            note += QStringLiteral("int value%1 = compute(%1, \"text\"); // line %1\n").arg(i);
        note += QStringLiteral("```\n");
        auto editor = parsedEditorWith(note);
        if (!editor)
            return -1.0; // never parsed: fails the check below
        const qsizetype middle = note.indexOf(QStringLiteral("value%1 ").arg(lines / 2));
        editor->setCursorOffset(middle);
        editor->enterBlockAt(middle);
        editor->grab();

        // Best of three medians: load only ever adds time.
        return perf::bestOf([&] {
            std::vector<double> samples;
            for (int i = 0; i < 9; ++i) {
                QElapsedTimer clock;
                clock.start();
                QTest::keyClick(editor.get(), Qt::Key_Y);
                editor->grab();
                samples.push_back(clock.nsecsElapsed() / 1e6);
            }
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        }, 3, 8.0);
    };

    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const double shortTyping = measure(100);
    const double longTyping = measure(5000);
    ASSERT_GE(longTyping, 0.0) << "the parse never came back";
    std::printf("[ PERF     ] code block of 100 lines: type %.2f ms; of 5000: %.2f ms\n",
                shortTyping, longTyping);

    EXPECT_LT(longTyping, 8.0);
    // Relaying out the whole listing was about 60 times the short one. What
    // is left grows with the note through the per-keystroke O(document)
    // passes, which weigh more in an unoptimised build: 1.1x in Release,
    // 3.7x at -O0 without highlighting. Eight catches the one that matters.
    EXPECT_LT(longTyping, qMax(1.0, shortTyping) * 8);
}

TEST(CodeBlockPerfTest, TypingInALongListingInsideACalloutStaysInBudget)
{
    // A fence inside a callout was part of the callout's one layout, so a
    // keystroke in it relaid out the whole listing.
    const auto measure = [](int lines) {
        QString note = QStringLiteral("# Listing\n\n> [!note] Code\n> ```cpp\n");
        for (int i = 0; i < lines; ++i)
            note += QStringLiteral("> int value%1 = compute(%1, \"text\"); // line %1\n").arg(i);
        note += QStringLiteral("> ```\n");
        auto editor = parsedEditorWith(note);
        if (!editor)
            return -1.0;
        const qsizetype middle = note.indexOf(QStringLiteral("value%1 ").arg(lines / 2));
        editor->setCursorOffset(middle);
        editor->enterBlockAt(middle);
        editor->grab();
        // Best of three medians: load only ever adds time.
        return perf::bestOf([&] {
            std::vector<double> samples;
            for (int i = 0; i < 9; ++i) {
                QElapsedTimer clock;
                clock.start();
                QTest::keyClick(editor.get(), Qt::Key_Y);
                editor->grab();
                samples.push_back(clock.nsecsElapsed() / 1e6);
            }
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        }, 3, 8.0);
    };
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    const double shortTyping = measure(100);
    const double longTyping = measure(2000);
    ASSERT_GE(longTyping, 0.0) << "the parse never came back";
    std::printf("[ PERF     ] callout listing of 100 lines: type %.2f ms; of 2000: %.2f ms\n",
                shortTyping, longTyping);
    EXPECT_LT(longTyping, 8.0);
}

// --- complex code blocks: the colour knobs -----------------------------------

namespace {

/// Points XDG_CONFIG_HOME at a throwaway directory holding `codeblocks.ini`
/// for as long as it lives; editors built meanwhile read it.
struct CodeblocksHome {
    QTemporaryDir dir;
    QByteArray previous = qgetenv("XDG_CONFIG_HOME");

    explicit CodeblocksHome(const QByteArray &ini = {})
    {
        qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
        QDir(dir.path()).mkpath(QStringLiteral("notolith"));
        if (!ini.isEmpty())
            writeAtomically(QDir(dir.path()).filePath(QStringLiteral("notolith/codeblocks.ini")), ini);
    }
    ~CodeblocksHome() { qputenv("XDG_CONFIG_HOME", previous); }
};

/// Pixels of the editor as painted now that one channel clearly outweighs the
/// other two (0 red, 1 green, 2 blue). Text is antialiased, so an exact colour
/// match undercounts it.
int dominatedBy(const QImage &image, int channel, int margin = 120)
{
    const QImage frame = image.convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const int c[3] = {qRed(row[x]), qGreen(row[x]), qBlue(row[x])};
            if (c[channel] - qMax(c[(channel + 1) % 3], c[(channel + 2) % 3]) > margin)
                ++count;
        }
    }
    return count;
}

int dominatedBy(MarkdownEditor &editor, int channel, int margin = 120)
{
    return dominatedBy(editor.grab().toImage(), channel, margin);
}

TokenStyle tokenColoured(const QColor &colour)
{
    TokenStyle style;
    style.color = colour;
    return style;
}

/// The colour of the first token that starts at `start`, invalid if none does.
QColor tokenColourAt(const QList<QTextLayout::FormatRange> &formats, int start)
{
    for (const QTextLayout::FormatRange &range : formats) {
        if (range.start == start)
            return range.format.foreground().color();
    }
    return {};
}

} // namespace

TEST(ComplexCodeTest, OffIsByteForByte)
{
    const QString note = QStringLiteral("```python\nfoo = print(1) # c\n```\n");
    QImage plain;
    {
        CodeblocksHome none;
        plain = editorWith(note)->grab().toImage();
    }

    // Everything configured - section, theme layer - and the setting off.
    CodeblocksHome home("[python]\nbg = #FF0000ff\nfg = #0000FFff\nborder = #00FF00ff\n"
                        "syntax.comment = #FFFF00ff italic\nsyntax.function = #00FFFFff\n");
    Theme themed = Theme::builtin(Theme::Mode::Dark);
    themed.setToken(QStringLiteral("--syntax-keyword"), QStringLiteral("#FF00FF bold"));
    themed.setToken(QStringLiteral("--code-string"), QStringLiteral("#FF00FF"));
    auto off = editorWith(note);
    off->setTheme(themed);
    off->setComplexCodeBlocks(false);
    EXPECT_EQ(off->grab().toImage(), plain) << "off must draw as if nothing were configured";

    off->setComplexCodeBlocks(true);
    EXPECT_NE(off->grab().toImage(), plain) << "and on must actually show the configuration";

    // Nothing configured, setting on: the same bytes again.
    CodeblocksHome none;
    auto unconfigured = editorWith(note);
    EXPECT_EQ(unconfigured->grab().toImage(), plain);

    // The highlighter itself: no map, an empty map and a map with no entries
    // are all the format list it always produced.
    CodeHighlighter highlighter;
    const QString code = QStringLiteral("foo = print(1) # c\nreturn 'x'\n");
    const auto reference = highlighter.highlight(code, QStringLiteral("python"));
    const SyntaxMap empty;
    const SyntaxMap noEntries({SyntaxStyles()});
    for (const SyntaxMap *map : {&empty, &noEntries}) {
        const auto formats = highlighter.highlight(code, QStringLiteral("python"), map);
        ASSERT_EQ(formats.size(), reference.size());
        for (qsizetype i = 0; i < formats.size(); ++i) {
            EXPECT_EQ(formats.at(i).start, reference.at(i).start);
            EXPECT_EQ(formats.at(i).length, reference.at(i).length);
            EXPECT_EQ(formats.at(i).format, reference.at(i).format);
        }
    }
}

TEST(ComplexCodeTest, IniSectionColoursThePanel)
{
    CodeblocksHome home("[python]\nbg = #FF0000ff\nfg = #0000FFff\nborder = #00FF00ff\n");
    auto editor = editorWith(QStringLiteral("```python\nfoo\n```\n"));
    const QImage frame = editor->grab().toImage().convertToFormat(QImage::Format_RGB32);

    // The panel's extent: where the fill is.
    QRect fill;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const QRgb c = frame.pixel(x, y);
            if (qRed(c) > 240 && qGreen(c) < 20 && qBlue(c) < 20)
                fill = fill.united(QRect(x, y, 1, 1));
        }
    }
    ASSERT_GT(fill.width(), 300) << "the bg colour filled no panel";
    const QRgb inside = frame.pixel(fill.left() + 12, fill.top() + 12);
    EXPECT_EQ(inside & 0xFFFFFF, 0xFF0000u) << "the panel corner is the section's bg";
    EXPECT_GT(dominatedBy(frame, 1), fill.width()) << "the 1 px border runs along the panel";
    EXPECT_GT(dominatedBy(frame, 2), 3) << "plain code text takes the section's fg";
}

TEST(ComplexCodeTest, FenceOverridesIni)
{
    CodeblocksHome home("[python]\nbg = #FF0000ff\nfg = #0000FFff\n");
    auto editor = editorWith(QStringLiteral("```python bg=#00FF00ff fg=#fff\nfoo\n```\n"));
    const QImage frame = editor->grab().toImage();
    EXPECT_GT(dominatedBy(frame, 1), 5000) << "the fence's bg wins";
    EXPECT_LT(dominatedBy(frame, 0), 20) << "the ini's red is gone, not mixed in";
    EXPECT_EQ(dominatedBy(frame, 2), 0) << "fg=#fff beats the ini's blue";
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("bg=")))
        << "the attributes are hidden with the fence";
}

TEST(ComplexCodeTest, FenceSyntaxKeysSurviveTheParser)
{
    // `syntax.keyword` is one name with a dot in it; the bare-word flags
    // around it, and a callout's `.class`, are unchanged.
    const Decorations decorations = Parser()
                                        .parseAll(QStringLiteral("```python tab syntax.keyword=\"#C678DD bold\" bg=#123\nx\n```\n"))
                                        .decorations;
    ASSERT_EQ(decorations.codeFences.size(), 1);
    const QMap<QString, QString> &attributes = decorations.codeFences.first().attributes;
    EXPECT_EQ(attributes.value(QStringLiteral("syntax.keyword")), QStringLiteral("#C678DD bold"));
    EXPECT_EQ(attributes.value(QStringLiteral("bg")), QStringLiteral("#123"));
    EXPECT_TRUE(attributes.contains(QStringLiteral("tab")));
    EXPECT_EQ(attributes.size(), 3);
}

TEST(ComplexCodeTest, LongestDottedPrefixWins)
{
    const QColor red = Qt::red;
    const QColor blue = Qt::blue;
    const SyntaxMap map({SyntaxStyles{{QStringLiteral("function"), tokenColoured(red)},
                                      {QStringLiteral("function.builtin"), tokenColoured(blue)}}});
    EXPECT_EQ(map.resolve(QStringLiteral("function.builtin"))->color, blue);
    EXPECT_EQ(map.resolve(QStringLiteral("function.builtin.x"))->color, blue);
    EXPECT_EQ(map.resolve(QStringLiteral("function.special"))->color, red);
    EXPECT_EQ(map.resolve(QStringLiteral("function"))->color, red);
    EXPECT_EQ(map.resolve(QStringLiteral("functionx")), nullptr) << "a cut is always on a dot";
    EXPECT_EQ(map.resolve(QStringLiteral("keyword")), nullptr);

    // Layer by layer, nearest first: a fence's `keyword` also covers a
    // `keyword.control` the theme spells out.
    const SyntaxMap layered({SyntaxStyles{{QStringLiteral("keyword"), tokenColoured(red)}},
                             SyntaxStyles{{QStringLiteral("keyword.control"), tokenColoured(blue)}}});
    EXPECT_EQ(layered.resolve(QStringLiteral("keyword.control"))->color, red);

    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";

    // And through the highlighter, on whatever capture `print` carries.
    CodeHighlighter highlighter;
    const QString code = QStringLiteral("print(1)\n");
    const SyntaxMap tagging = SyntaxMap::tagging();
    const auto tagged = highlighter.highlight(code, QStringLiteral("python"), &tagging);
    QString capture;
    for (const QTextLayout::FormatRange &range : tagged) {
        if (range.start == 0)
            capture = SyntaxMap::captureOf(range.format);
    }
    ASSERT_FALSE(capture.isEmpty()) << "print is a token with a capture name";

    const SyntaxMap exact({SyntaxStyles{{capture, tokenColoured(blue)}}});
    EXPECT_EQ(tokenColourAt(highlighter.highlight(code, QStringLiteral("python"), &exact), 0), blue);
    const SyntaxMap sibling({SyntaxStyles{{capture + QLatin1Char('x'), tokenColoured(blue)}}});
    EXPECT_NE(tokenColourAt(highlighter.highlight(code, QStringLiteral("python"), &sibling), 0), blue);
    if (const qsizetype dot = capture.indexOf(QLatin1Char('.')); dot > 0) {
        const SyntaxMap parent({SyntaxStyles{{capture.left(dot), tokenColoured(red)}}});
        EXPECT_EQ(tokenColourAt(highlighter.highlight(code, QStringLiteral("python"), &parent), 0), red);
    }
}

TEST(ComplexCodeTest, ObsidianCodeVarsFeedTheDefaultMap)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";

    const QHash<QString, QString> tokens = Scheme::parseObsidianCSS(
        QStringLiteral(".theme-dark { --code-keyword: #FF0000; --code-comment: #00FF00; }"),
        Theme::Mode::Dark);
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    theme.setTokens(tokens);
    const SyntaxStyles styles = theme.syntaxStyles();
    ASSERT_TRUE(styles.contains(QStringLiteral("keyword")));
    EXPECT_EQ(styles.value(QStringLiteral("keyword")).color, QColor(255, 0, 0));
    EXPECT_EQ(styles.value(QStringLiteral("comment")).color, QColor(0, 255, 0));

    CodeblocksHome none;
    const QString note = QStringLiteral("```cpp\nreturn x;\n```\n");
    auto plain = editorWith(note);
    auto themed = editorWith(note);
    themed->setTheme(theme);
    EXPECT_GT(dominatedBy(*themed, 0), dominatedBy(*plain, 0) + 5)
        << "`return` is a keyword.control and takes the theme's --code-keyword";
}

TEST(ComplexCodeTest, TabClickRelaysOutOnlyTheGroup)
{
    // A tab click used to lay every block of the note out again. Now it is the
    // group's own blocks, which are the middle of the note's seven.
    auto editor = editorWith(kTabbedNote);
    editor->grab();
    const quint64 wholeNote = editor->layoutCount();
    const qsizetype group = kTabbedNote.indexOf(QLatin1String("```python"));

    editor->selectCodeTab(group, 1);
    editor->grab();
    const quint64 relaid = editor->layoutCount() - wholeNote;
    EXPECT_GT(relaid, 0u);
    EXPECT_LT(relaid, wholeNote) << "the paragraphs outside the group were laid out again";
    EXPECT_LE(relaid, 4u);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("puts(\"hi\")")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("Before.")));
}

TEST(ComplexCodeTest, TypingInAStyledBlockStaysInBudget)
{
    // CodeBlockPerfTest's long listing again, with every colour knob set: the
    // per-keystroke cost of a map is a signature and a few hash lookups.
    CodeblocksHome home("[cpp]\nbg = #101820ff\nfg = #D8DEE9ff\nborder = #3776AB80\n"
                        "syntax.keyword = #C678DD bold\nsyntax.string = #98C379 italic\n"
                        "syntax.comment = #5C6370 italic\nsyntax.function.builtin = #61AFEF\n");
    const int lines = 5000;
    QString note = QStringLiteral("# Listing\n\n```cpp\n");
    for (int i = 0; i < lines; ++i)
        note += QStringLiteral("int value%1 = compute(%1, \"text\"); // line %1\n").arg(i);
    note += QStringLiteral("```\n");
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor) << "the parse never came back";
    const qsizetype middle = note.indexOf(QStringLiteral("value%1 ").arg(lines / 2));
    editor->setCursorOffset(middle);
    editor->enterBlockAt(middle);
    editor->grab();

    std::vector<double> samples;
    for (int i = 0; i < 9; ++i) {
        QElapsedTimer clock;
        clock.start();
        QTest::keyClick(editor.get(), Qt::Key_Y);
        editor->grab();
        samples.push_back(clock.nsecsElapsed() / 1e6);
    }
    std::sort(samples.begin(), samples.end());
    std::printf("[ PERF     ] styled code block of %d lines: type %.2f ms\n", lines, samples[4]);
    EXPECT_LT(samples[4], 8.0);
}

// --- long fences, in pieces --------------------------------------------------

namespace {

/// A C++ fence of `lines` numbered lines, with a block comment running from
/// line `open` to line `close` - long enough that the comment crosses from
/// one piece into the next ones.
QString longFence(int lines, int open, int close)
{
    QString note = QStringLiteral("Before.\n\n```cpp\n");
    for (int i = 0; i < lines; ++i) {
        if (i == open)
            note += QStringLiteral("/* comment opens\n");
        else if (i == close)
            note += QStringLiteral("comment closes */\n");
        else
            note += QStringLiteral("int value%1 = %1;\n").arg(i);
    }
    return note + QStringLiteral("```\n\nAfter.\n");
}

QColor colourAt(const MarkdownEditor &editor, qsizetype offset)
{
    return editor.formatAt(offset).foreground().color();
}

} // namespace

TEST(CodePieceTest, AnOpenCommentColoursTheLaterPiecesToo)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";
    const QString note = longFence(100, 5, 80);
    auto editor = editorWith(note);
    const qsizetype opener = note.indexOf(QLatin1String("/* comment"));
    const qsizetype inside = note.indexOf(QLatin1String("int value50"));
    const qsizetype after = note.indexOf(QLatin1String("int value90"));
    editor->enterBlockAt(opener + 3);
    editor->grab();
    const QColor comment = colourAt(*editor, opener + 3);
    const QColor keyword = colourAt(*editor, note.indexOf(QLatin1String("int value0")) + 1);
    ASSERT_NE(comment, keyword) << "the highlighter tells a comment from a type";

    editor->enterBlockAt(after); // lays the last lines out, wherever they are
    editor->grab();
    EXPECT_EQ(colourAt(*editor, inside + 1), comment) << "line 50 is inside the comment";
    EXPECT_EQ(colourAt(*editor, after + 1), keyword) << "line 90 is after it";
}

TEST(CodePieceTest, RemovingTheOpenerRecoloursTheLaterPiecesAtOnce)
{
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";
    const QString note = longFence(100, 5, 80);
    auto editor = editorWith(note);
    const qsizetype opener = note.indexOf(QLatin1String("/* comment"));
    const qsizetype inside = note.indexOf(QLatin1String("int value50"));
    editor->enterBlockAt(opener + 2);
    editor->grab();
    const QColor keyword = colourAt(*editor, note.indexOf(QLatin1String("int value0")) + 1);
    ASSERT_NE(colourAt(*editor, inside + 1), keyword) << "line 50 starts in the comment";

    // Backspace twice: "/*" is gone, before the reparse has had its 20 ms.
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    editor->grab();
    EXPECT_EQ(colourAt(*editor, inside - 1), keyword)
        << "a piece below the edit kept the colours of a comment that is gone";
}

TEST(CodePieceTest, APieceLaidOutEarlierFarBelowIsRecolouredToo)
{
    // Only the piece after the edit is relaid at once; one further down that
    // was laid out on an earlier visit still holds the old colours, and has
    // to lose them rather than show them when the reader scrolls back.
    if (!CodeHighlighter::isAvailable())
        GTEST_SKIP() << "built without KF6 SyntaxHighlighting";
    const QString note = longFence(300, 5, 290);
    auto editor = editorWith(note);
    const qsizetype opener = note.indexOf(QLatin1String("/* comment"));
    const qsizetype far = note.indexOf(QLatin1String("int value250"));
    editor->enterBlockAt(opener + 2);
    editor->grab();
    const QColor comment = colourAt(*editor, opener + 3);
    const QColor keyword = colourAt(*editor, note.indexOf(QLatin1String("int value0")) + 1);

    editor->enterBlockAt(far);
    editor->grab();
    ASSERT_EQ(colourAt(*editor, far + 1), comment) << "line 250 starts in the comment";

    editor->enterBlockAt(opener + 2);
    editor->grab();
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    editor->grab();

    editor->enterBlockAt(far - 2);
    editor->grab();
    EXPECT_EQ(colourAt(*editor, far - 1), keyword) << "line 250 kept the comment's colour";
}

TEST(CodePieceTest, TheCaretAnywhereRevealsEveryPiece)
{
    // With the caret in the fence its options show as written - they are on
    // the first piece, and the caret is two pieces further down. C++ takes
    // Quarto's `//|` for them.
    const QString note = QString(longFence(100, 5, 80))
                             .replace(QStringLiteral("```cpp\n"),
                                      QStringLiteral("```cpp\n//| title: t\n"));
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("//| title")));
    editor->enterBlockAt(note.indexOf(QLatin1String("int value90")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("//| title: t")));
}

TEST(CodePieceTest, CopyingTakesTheWholeListing)
{
    const QString note = longFence(100, 5, 80);
    auto editor = editorWith(note);
    // A title puts the label in a header strip the sweep can find.
    const QString titled = QString(note).replace(QStringLiteral("```cpp\n"),
                                                 QStringLiteral("```cpp\n//| title: t\n"));
    editor->setNoteText(titled);
    ASSERT_TRUE(clickLabelUntilCopied(*editor));
    const qsizetype from = titled.indexOf(QLatin1String("int value0"));
    const qsizetype to = titled.lastIndexOf(QLatin1String("\n```"));
    EXPECT_EQ(QGuiApplication::clipboard()->text(), titled.mid(from, to - from));
}

TEST(CodePieceTest, FoldingFoldsEveryPiece)
{
    const QString note = QString(longFence(100, 5, 80))
                             .replace(QStringLiteral("```cpp\n"),
                                      QStringLiteral("```cpp\n//| title: t\n//| fold: true\n"));
    auto editor = editorWith(note);
    const qsizetype late = note.indexOf(QLatin1String("int value90"));
    EXPECT_TRUE(editor->isFoldedAt(late));
    const qreal folded = editor->documentHeight();
    editor->toggleFoldAt(late);
    EXPECT_FALSE(editor->isFoldedAt(late));
    EXPECT_GT(editor->documentHeight(), folded + 100 * 10) << "a hundred lines came back";
}

// --- editing property values -------------------------------------------------

namespace {

/// Types `text` into the open property editor and presses `key` there.
bool typeIntoPropertyEditor(MarkdownEditor &editor, const QString &text, Qt::Key key)
{
    auto *edit = editor.findChild<QLineEdit *>();
    if (!edit || edit->isHidden())
        return false;
    edit->setText(text);
    QTest::keyClick(edit, key);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return true;
}

const QString kPropertiesNote = QStringLiteral(
    "---\ntitle: Old\ncount: 3\nempty:\ntags:\n  - a\n  - b\ndone: false\n"
    "noted: kept # a comment\n---\n\nBody\n");

} // namespace

TEST(PropertyEditTest, AValueIsRewrittenInPlaceAndQuotedOnlyWhenItHasTo)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("title"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("New: title"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), QString(kPropertiesNote).replace(
                                      QStringLiteral("title: Old"),
                                      QStringLiteral("title: \"New: title\"")));
    EXPECT_FALSE(editor->findChild<QLineEdit *>()) << "the editor closes";

    // Undo takes the edit back as one step.
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), kPropertiesNote);
}

TEST(PropertyEditTest, EscapeLeavesTheFileAlone)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("title"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("Something else"), Qt::Key_Escape));
    EXPECT_EQ(editor->noteText(), kPropertiesNote);
}

TEST(PropertyEditTest, ANumberStaysANumber)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("count"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("many"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), kPropertiesNote) << "not a number: refused";

    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("count"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("4.5"), Qt::Key_Return));
    EXPECT_TRUE(editor->noteText().contains(QLatin1String("\ncount: 4.5\n")));
}

TEST(PropertyEditTest, AnEmptyValueIsFilledIn)
{
    auto editor = editorWith(kPropertiesNote);
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("empty"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("now"), Qt::Key_Return));
    EXPECT_TRUE(editor->noteText().contains(QLatin1String("\nempty: now\ntags:")));
}

TEST(PropertyEditTest, WhatCannotBeRewrittenSafelyIsNotOpened)
{
    auto editor = editorWith(kPropertiesNote);
    EXPECT_FALSE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("tags"))))
        << "a list";
    EXPECT_FALSE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("done"))))
        << "a checkbox toggles";
    EXPECT_FALSE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("noted"))))
        << "a comment on the line would be lost";
    EXPECT_FALSE(editor->findChild<QLineEdit *>());
}

TEST(PropertyEditTest, ClickingAValueOpensItsEditor)
{
    auto editor = editorWith(kPropertiesNote);
    // Swept, like the code label, rather than restating the table's spacing:
    // somewhere right of the key column on the first rows is the title.
    const int viewport = editor->viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    QLineEdit *edit = nullptr;
    for (int y = 4; y < 80 && !edit; y += 3) {
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(left + 260, y));
        edit = editor->findChild<QLineEdit *>();
    }
    ASSERT_TRUE(edit) << "no value editor near the top of the table";
    EXPECT_EQ(edit->text(), QStringLiteral("Old"));
}

TEST(PropertyEditTest, AListLosesAndGainsItems)
{
    const QString note = QStringLiteral("---\ntags:\n  - a\n  - 'b'\nnext: 1\n---\n\nBody\n");
    auto editor = editorWith(note);
    const qsizetype tags = note.indexOf(QLatin1String("tags"));

    ASSERT_TRUE(editor->removeListItemAt(tags, 0));
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags:\n  - 'b'\nnext: 1\n---\n\nBody\n"))
        << "the other item kept exactly as written";

    ASSERT_TRUE(editor->addListItemAt(tags));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("c: d"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("---\ntags:\n  - 'b'\n  - \"c: d\"\nnext: 1\n---\n\nBody\n"));

    // Each change is one undo step.
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), note);
}

TEST(PropertyEditTest, AListItemIsChangedInPlace)
{
    const QString note = QStringLiteral("---\ntags:\n  - one\n  - two\n---\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(editor->editListItemAt(note.indexOf(QLatin1String("tags")), 1));
    EXPECT_EQ(editor->findChild<QLineEdit *>()->text(), QStringLiteral("two"));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("#2"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags:\n  - one\n  - \"#2\"\n---\n"));
}

TEST(PropertyEditTest, AFlowListStaysAFlowList)
{
    const QString note = QStringLiteral("---\ntags: [a, b]\n---\n");
    auto editor = editorWith(note);
    ASSERT_TRUE(editor->addListItemAt(note.indexOf(QLatin1String("tags"))));
    ASSERT_TRUE(typeIntoPropertyEditor(*editor, QStringLiteral("c"), Qt::Key_Return));
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags: [a, b, c]\n---\n"));
}

TEST(PropertyEditTest, AListWithACommentIsLeftAlone)
{
    const QString note = QStringLiteral("---\ntags:\n  - a # keep\n  - b\n---\n");
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->addListItemAt(note.indexOf(QLatin1String("tags"))));
    EXPECT_FALSE(editor->removeListItemAt(note.indexOf(QLatin1String("tags")), 1));
    EXPECT_EQ(editor->noteText(), note);
}

TEST(PropertyEditTest, ClickingAChipsCrossRemovesIt)
{
    const QString note = QStringLiteral("---\ntags:\n  - alpha\n  - beta\n---\n\nBody\n");
    auto editor = editorWith(note);
    const int viewport = editor->viewport()->width();
    const int column = qMax(120, qMin(viewport - 48, 700));
    const int left = qMax(24, (viewport - column) / 2);
    // Across the first chip line, right of the key column, until something
    // changes: only a cross changes the note.
    for (int y = 4; y < 60 && editor->noteText() == note; y += 2) {
        for (int x = left + 180; x < left + 420 && editor->noteText() == note; x += 2)
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
    }
    EXPECT_EQ(editor->noteText(), QStringLiteral("---\ntags:\n  - beta\n---\n\nBody\n"));
}

TEST(PropertyEditTest, ClosingTheEditorWithAValueOpenIsSafe)
{
    // The open line edit outlived the editor's members and called back into
    // them as it lost focus: a heap corruption on closing a note mid-edit.
    auto editor = editorWith(kPropertiesNote);
    editor->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(editor.get()));
    ASSERT_TRUE(editor->editPropertyAt(kPropertiesNote.indexOf(QLatin1String("title"))));
    editor.reset(); // must not crash, under ASan or without it
}

// --- spelling ------------------------------------------------------------------

namespace {

/// A dictionary of a few words, given the way the window gives the real one.
struct FakeSpelling {
    QStringList known = {QStringLiteral("good"), QStringLiteral("words"),
                         QStringLiteral("here"), QStringLiteral("and"), QStringLiteral("don't")};
    QStringList added;
    MarkdownEditor::Spelling spelling()
    {
        return {[this](const QString &word) { return known.contains(word) || added.contains(word); },
                [](const QString &) { return QStringList{QStringLiteral("words")}; },
                [this](const QString &word) { added << word; },
                [this](const QString &word) { return added.contains(word); },
                [this](const QString &word) { added.removeAll(word); }};
    }
};

bool underlined(const MarkdownEditor &editor, qsizetype offset)
{
    return editor.formatAt(offset).underlineStyle() == QTextCharFormat::SpellCheckUnderline;
}

} // namespace

TEST(SpellingTest, UnderlinesProseAndLeavesCodeLinksAndTagsAlone)
{
    FakeSpelling dictionary;
    const QString note = QStringLiteral(
        "good wrds here and `codee` and [[Linkk]] and #tagg and don't x2y\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());

    EXPECT_TRUE(underlined(*editor, note.indexOf(QLatin1String("wrds")) + 1));
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("good")) + 1));
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("codee")) + 1)) << "inline code";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("Linkk")) + 1)) << "a link";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("tagg")) + 1)) << "a tag";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("don't")) + 1)) << "an apostrophe";
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("x2y")))) << "not a word";

    editor->setSpelling({});
    EXPECT_FALSE(underlined(*editor, note.indexOf(QLatin1String("wrds")) + 1)) << "switched off";
}

TEST(SpellingTest, TheWordBeingTypedIsNotUnderlinedYet)
{
    FakeSpelling dictionary;
    const QString note = QStringLiteral("good wrds here\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());
    const qsizetype inside = note.indexOf(QLatin1String("wrds")) + 2;
    editor->enterBlockAt(inside);
    EXPECT_FALSE(underlined(*editor, inside));
    editor->enterBlockAt(note.indexOf(QLatin1String("here")) + 2);
    EXPECT_TRUE(underlined(*editor, inside)) << "finished once the caret leaves it";
}

TEST(SpellingTest, TheMenuReplacesTheWordOrLearnsIt)
{
    FakeSpelling dictionary;
    const QString note = QStringLiteral("good wrds here\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());
    const qsizetype at = note.indexOf(QLatin1String("wrds")) + 1;
    EXPECT_FALSE(editor->spellingMenuAt(note.indexOf(QLatin1String("good")))) << "a correct word";

    std::unique_ptr<QMenu> menu = editor->spellingMenuAt(at);
    ASSERT_TRUE(menu);
    ASSERT_FALSE(menu->actions().isEmpty());
    EXPECT_EQ(menu->actions().first()->text(), QStringLiteral("words"));
    menu->actions().first()->trigger();
    EXPECT_EQ(editor->noteText(), QStringLiteral("good words here\n"));

    editor->setNoteText(note);
    menu = editor->spellingMenuAt(at);
    ASSERT_TRUE(menu);
    menu->actions().last()->trigger(); // "Add to dictionary"
    EXPECT_EQ(dictionary.added, QStringList{QStringLiteral("wrds")});
    EXPECT_FALSE(underlined(*editor, at));
}

TEST(SpellingTest, LearningAWordKeepsTheViewWhereItWas)
{
    // One long list is one block: the caret deep inside it, then the word
    // learnt and the caret placed again (what the click that follows does).
    FakeSpelling dictionary;
    QString note;
    for (int i = 0; i < 60; ++i)
        note += QStringLiteral("Paragraph %1 above the list.\n\n").arg(i);
    for (int i = 0; i < 300; ++i)
        note += QStringLiteral("- item %1 with wrds in it\n").arg(i);
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());
    editor->show();
    editor->grab();
    const qsizetype caret = note.indexOf(QStringLiteral("item 200 ")) + 6; // in the number
    const qsizetype at = note.indexOf(QLatin1String("wrds"), caret) + 1;
    editor->enterBlockAt(caret);
    for (int settle = 0; settle < 3; ++settle) { // estimated heights above become real
        editor->setCursorOffset(caret);
        editor->grab();
    }
    QScrollBar *bar = editor->verticalScrollBar();
    const int before = bar->value();
    ASSERT_GT(before, 0) << "scrolled to the caret, deep in the list";
    const QRect caretBefore = static_cast<QWidget *>(editor.get())->inputMethodQuery(Qt::ImCursorRectangle).toRect();
    std::unique_ptr<QMenu> menu = editor->spellingMenuAt(at);
    ASSERT_TRUE(menu);
    menu->actions().last()->trigger(); // "Add to dictionary"
    editor->setCursorOffset(caret);
    editor->grab();
    EXPECT_EQ(bar->value(), before) << "the view stays put, not at the top of the list";
    EXPECT_EQ(static_cast<QWidget *>(editor.get())->inputMethodQuery(Qt::ImCursorRectangle).toRect(), caretBefore)
        << "and the caret's line with it";
    EXPECT_FALSE(underlined(*editor, at));
}

// --- context menu --------------------------------------------------------------

namespace {

/// The action whose text starts with `name` (the key text follows a tab).
QAction *menuAction(const QMenu &menu, const QString &name)
{
    for (QAction *action : menu.actions()) {
        if (action->text().startsWith(name))
            return action;
    }
    return nullptr;
}

/// "good words here", the caret in front of "words" and `count` letters selected.
std::unique_ptr<MarkdownEditor> editorSelecting(const QString &note, qsizetype from, int count)
{
    auto editor = editorWith(note);
    editor->setCursorOffset(from);
    for (int i = 0; i < count; ++i)
        QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier);
    return editor;
}

} // namespace

TEST(ContextMenuTest, EveryWordGetsAMenu)
{
    const QString note = QStringLiteral("good words here\n");
    auto editor = editorWith(note);
    const auto menu = editor->contextMenuAt(note.indexOf(QLatin1String("words")) + 1);
    ASSERT_TRUE(menu);
    for (const char *name : {"Cut", "Copy", "Paste", "Format", "Add link"})
        EXPECT_TRUE(menuAction(*menu, QLatin1String(name))) << name;
    QAction *format = menuAction(*menu, QStringLiteral("Format"));
    ASSERT_TRUE(format && format->menu());
    QStringList names;
    for (QAction *action : format->menu()->actions())
        names << action->text().section(QLatin1Char('\t'), 0, 0);
    EXPECT_EQ(names, (QStringList{"Bold", "Italic", "Strikethrough", "Highlight", "Code", "Math", "Comment"}));
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Look up"))) << "off until the window turns it on";
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Search")));
}

TEST(ContextMenuTest, FormatWrapsTheSelectionOrTheWordUnderThePointer)
{
    const QString note = QStringLiteral("good words here\n");
    const qsizetype words = note.indexOf(QLatin1String("words"));
    auto editor = editorSelecting(note, words, 5);
    auto menu = editor->contextMenuAt(words + 1);
    QAction *format = menuAction(*menu, QStringLiteral("Format"));
    ASSERT_TRUE(format);
    menuAction(*format->menu(), QStringLiteral("Strikethrough"))->trigger();
    EXPECT_EQ(editor->noteText(), QStringLiteral("good ~~words~~ here\n"));

    // Nothing selected: the word the pointer was on.
    menu.reset(); // a menu is its editor's child: gone with it
    editor = editorWith(note);
    menu = editor->contextMenuAt(words + 2);
    menuAction(*menuAction(*menu, QStringLiteral("Format"))->menu(), QStringLiteral("Bold"))->trigger();
    EXPECT_EQ(editor->noteText(), QStringLiteral("good **words** here\n"));
}

TEST(ContextMenuTest, CutCopyAndPasteWork)
{
    const QString note = QStringLiteral("good words here\n");
    const qsizetype words = note.indexOf(QLatin1String("words"));
    auto editor = editorSelecting(note, words, 5);
    auto menu = editor->contextMenuAt(words + 1);
    menuAction(*menu, QStringLiteral("Copy"))->trigger();
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("words"));
    menuAction(*menu, QStringLiteral("Cut"))->trigger();
    EXPECT_EQ(editor->noteText(), QStringLiteral("good  here\n"));
    menuAction(*menu, QStringLiteral("Paste"))->trigger();
    EXPECT_EQ(editor->noteText(), note);

    // Nothing selected: nothing to cut or copy.
    menu.reset();
    editor = editorWith(note);
    menu = editor->contextMenuAt(words);
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Cut"))->isEnabled());
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Copy"))->isEnabled());
}

/// Runs `open` (which blocks in the context menu's exec) and, once the menu is
/// up, triggers its "Look up" item, which closes it. False if no menu came.
bool openMenuAndLookUp(const std::function<void()> &open)
{
    bool found = false;
    QTimer::singleShot(100, [&found] {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top->objectName() != QLatin1String("editorContextMenu") || !top->isVisible())
                continue;
            if (QAction *look = menuAction(*static_cast<QMenu *>(top), QStringLiteral("Look up"))) {
                found = true;
                look->trigger();
            }
            top->close();
        }
    });
    open();
    return found;
}

TEST(ContextMenuTest, LookUpFromTheKeyboardAnchorsAtTheMenuNotTheMouse)
{
    auto editor = editorWith(QStringLiteral("good words here\n"));
    editor->move(300, 200);
    editor->setContextActions({false, true, {}});
    editor->setCursorOffset(7); // inside "words"
    QSignalSpy lookup(editor.get(), &MarkdownEditor::lookupRequested);

    // The menu key: Qt sends a Keyboard event at the caret; the popover goes
    // where the event says, whatever the mouse is doing.
    const QPoint where(521, 223);
    ASSERT_TRUE(openMenuAndLookUp([&] {
        QContextMenuEvent event(QContextMenuEvent::Keyboard, QPoint(40, 20), where);
        QCoreApplication::sendEvent(editor.get(), &event);
    }));
    ASSERT_EQ(lookup.size(), 1);
    EXPECT_EQ(lookup.first().at(1).toPoint(), where);

    // Shift+F10 does the same from the caret, on platforms that do not.
    lookup.clear();
    ASSERT_TRUE(openMenuAndLookUp([&] { QTest::keyClick(editor.get(), Qt::Key_F10, Qt::ShiftModifier); }));
    ASSERT_EQ(lookup.size(), 1);
    EXPECT_EQ(lookup.first().at(0).toString(), QStringLiteral("words")) << "the word at the caret";
    const QPoint anchor = lookup.first().at(1).toPoint();
    EXPECT_TRUE(QRect(editor->mapToGlobal(QPoint(0, 0)), editor->size()).contains(anchor))
        << anchor.x() << "," << anchor.y();
}

TEST(ContextMenuTest, LookUpAndSearchFollowTheContextActions)
{
    const QString note = QStringLiteral("good words here\n");
    const qsizetype at = note.indexOf(QLatin1String("words")) + 1;
    auto editor = editorWith(note);
    QSignalSpy lookup(editor.get(), &MarkdownEditor::lookupRequested);
    QSignalSpy search(editor.get(), &MarkdownEditor::searchRequested);

    editor->setContextActions({true, true, QStringLiteral("https://x.test/?q=%1")});
    auto menu = editor->contextMenuAt(at);
    QAction *look = menuAction(*menu, QStringLiteral("Look up"));
    QAction *find = menuAction(*menu, QStringLiteral("Search for"));
    QAction *web = menuAction(*menu, QStringLiteral("Search the web for"));
    ASSERT_TRUE(look && find && web);
    EXPECT_TRUE(look->text().contains(QLatin1String("words")));
    EXPECT_TRUE(web->text().contains(QLatin1String("words")));
    look->trigger();
    find->trigger(); // the web item opens a browser, which a test must not
    ASSERT_EQ(lookup.size(), 1);
    ASSERT_EQ(search.size(), 1);
    EXPECT_EQ(lookup.first().at(0).toString(), QStringLiteral("words"));
    EXPECT_EQ(search.first().at(0).toString(), QStringLiteral("words"));

    editor->setContextActions({true, false, {}});
    menu = editor->contextMenuAt(at);
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Look up")));
    EXPECT_TRUE(menuAction(*menu, QStringLiteral("Search for")));
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Search the web")));

    // Between two spaces there is no word to look up.
    editor->setContextActions({true, true, QStringLiteral("https://x.test/?q=%1")});
    editor->setNoteText(QStringLiteral("a  b\n"));
    menu = editor->contextMenuAt(2);
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Look up")));
}

TEST(ContextMenuTest, ASelectionLongerThanALineIsNotLookedUp)
{
    const QString note = QStringLiteral("one\ntwo\nthree\n");
    auto editor = editorSelecting(note, 0, 9);
    editor->setContextActions({true, true, QStringLiteral("https://x.test/?q=%1")});
    const auto menu = editor->contextMenuAt(1);
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Look up")));
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Search")));
    EXPECT_TRUE(menuAction(*menu, QStringLiteral("Format")));

    // One line of a few words is a phrase worth looking up.
    editor->setNoteText(QStringLiteral("a kind of magic\n"));
    editor->setCursorOffset(0);
    for (int i = 0; i < 6; ++i)
        QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_TRUE(menuAction(*editor->contextMenuAt(1), QStringLiteral("Look up \"a kind\"")));
}

TEST(ContextMenuTest, ReadingViewOffersCopyAndLookUpOnly)
{
    const QString note = QStringLiteral("good words here\n");
    auto editor = editorWith(note);
    editor->setContextActions({true, true, {}});
    editor->setMode(MarkdownEditor::Mode::Reading);
    const auto menu = editor->contextMenuAt(note.indexOf(QLatin1String("words")) + 1);
    EXPECT_TRUE(menuAction(*menu, QStringLiteral("Copy")));
    EXPECT_TRUE(menuAction(*menu, QStringLiteral("Look up")));
    for (const char *name : {"Cut", "Paste", "Format", "Add link"})
        EXPECT_FALSE(menuAction(*menu, QLatin1String(name))) << name;
}

TEST(ContextMenuTest, ARemovedWordIsUnderlinedAgain)
{
    FakeSpelling dictionary;
    const QString note = QStringLiteral("good wrds here\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());
    const qsizetype at = note.indexOf(QLatin1String("wrds")) + 1;
    EXPECT_FALSE(menuAction(*editor->contextMenuAt(at), QStringLiteral("Remove")));

    menuAction(*editor->contextMenuAt(at), QStringLiteral("Add \"wrds\""))->trigger();
    EXPECT_FALSE(underlined(*editor, at));
    const auto menu = editor->contextMenuAt(at);
    QAction *remove = menuAction(*menu, QStringLiteral("Remove \"wrds\""));
    ASSERT_TRUE(remove);
    EXPECT_FALSE(menuAction(*menu, QStringLiteral("Add \"wrds\"")));
    remove->trigger();
    EXPECT_TRUE(underlined(*editor, at));
    EXPECT_TRUE(dictionary.added.isEmpty());
}

// --- commands and their keys ---------------------------------------------------

TEST(CommandTest, AKeyCanBeMovedToAnotherCommandOrTakenAway)
{
    const QString note = QStringLiteral("word\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(0);
    for (int i = 0; i < 4; ++i)
        QTest::keyClick(editor.get(), Qt::Key_Right, Qt::ShiftModifier); // "word"

    // Bold moves to Ctrl+Shift+B, and Ctrl+B is left doing nothing.
    editor->setKeyBindings({{QStringLiteral("editor:toggle-bold"),
                             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)}}});
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), note);
    QTest::keyClick(editor.get(), Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("**word**\n"));

    // An empty list unbinds; the command still runs by its id.
    editor->setKeyBindings({{QStringLiteral("editor:toggle-italics"), {}}});
    const QString bolded = editor->noteText();
    QTest::keyClick(editor.get(), Qt::Key_I, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), bolded);
    EXPECT_TRUE(editor->runCommand(QStringLiteral("editor:toggle-italics")));
    EXPECT_NE(editor->noteText(), bolded);
    EXPECT_FALSE(editor->runCommand(QStringLiteral("no:such-command")));
}

TEST(CommandTest, EveryCommandHasAnIDANameAndRuns)
{
    for (const MarkdownEditor::Command &command : MarkdownEditor::commands()) {
        EXPECT_TRUE(command.id.contains(QLatin1Char(':'))) << qPrintable(command.id);
        EXPECT_FALSE(command.name.isEmpty()) << qPrintable(command.id);
        auto editor = editorWith(QStringLiteral("one\ntwo\n"));
        editor->enterBlockAt(1);
        EXPECT_TRUE(editor->runCommand(command.id)) << qPrintable(command.id);
    }
}

// --- query blocks ------------------------------------------------------------------

namespace {

/// A fake `setQueryRunner`: records every search asked for and hands the
/// test the `deliver` callback instead of calling it, so a test can decide
/// exactly when - and in what order - results come back, without depending
/// on a real index or on timing.
struct FakeQueryRunner {
    struct Call {
        QString query;
        MarkdownEditor::QueryDeliver deliver;
    };
    QList<Call> calls;

    void operator()(const QString &query, const MarkdownEditor::QueryDeliver &deliver)
    {
        calls.append({query, deliver});
    }
};

QueryResult makeQueryResult(const QString &path, int matchCount, const QString &snippet,
                            qsizetype firstOffset)
{
    QueryResult result;
    result.path = path;
    result.matchCount = matchCount;
    result.snippet = snippet;
    result.firstOffset = firstOffset;
    if (!snippet.isEmpty())
        result.snippetHighlights.append({0, qMin<qsizetype>(3, snippet.size())});
    return result;
}

/// Runs the queued delivery FakeQueryRunner's callback posts (setQueryRunner
/// marshals it through QMetaObject::invokeMethod) and lets layout catch up.
void pumpQueuedDelivery()
{
    QCoreApplication::processEvents();
    QTest::qWait(10);
    QCoreApplication::processEvents();
}

} // namespace

TEST(LifelineTest, NothingIsPostedOnceTheReceiverHasEnded)
{
    noteAppC::Lifeline lifeline;
    QObject receiver;
    int calls = 0;
    EXPECT_TRUE(lifeline.post(&receiver, [&calls] { ++calls; }));
    QCoreApplication::processEvents();
    EXPECT_EQ(calls, 1);
    lifeline.end();
    EXPECT_FALSE(lifeline.post(&receiver, [&calls] { ++calls; }));
    QCoreApplication::processEvents();
    EXPECT_EQ(calls, 1);
}

TEST(QueryBlockTest, AResultArrivingAfterTheEditorIsGoneIsDropped)
{
    FakeQueryRunner fake;
    {
        auto editor = editorWith(QStringLiteral("```query\nfoo\n```\n"));
        editor->setQueryRunner(std::ref(fake));
        ASSERT_EQ(fake.calls.size(), 1);
    }
    // From another thread, as a real search delivers: nothing may reach the
    // freed editor, neither the post nor the queued call.
    std::thread([&fake] { fake.calls.constFirst().deliver({}); }).join();
    pumpQueuedDelivery();
}

TEST(QueryBlockTest, RendersResultsFromAnAsyncRunner)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));

    const qsizetype queryOffset = note.indexOf(QStringLiteral("foo"));
    // Rendered but still waiting: the runner was asked, but nothing answered
    // yet, so there is nothing to click - render must never block on it.
    ASSERT_EQ(fake.calls.size(), 1);
    EXPECT_EQ(fake.calls.first().query, QStringLiteral("foo"));
    EXPECT_TRUE(editor->queryRowsAt(queryOffset).isEmpty());

    fake.calls.first().deliver({makeQueryResult(QStringLiteral("One.md"), 3, QStringLiteral("a foo b"), 10),
                                makeQueryResult(QStringLiteral("Two.md"), 1, QStringLiteral("foo alone"), 4)});
    pumpQueuedDelivery();

    const QList<MarkdownEditor::QueryResultRow> rows = editor->queryRowsAt(queryOffset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("One.md"));
    EXPECT_EQ(rows.at(0).offset, 10);
    EXPECT_EQ(rows.at(1).path, QStringLiteral("Two.md"));
    EXPECT_EQ(rows.at(1).offset, 4);

    // Re-asking for the same text without an intervening edit or
    // refreshQueries is a cache hit, not a second search.
    editor->enterBlockAt(1);   // touches the "Intro" block, not the query one
    editor->enterBlockAt(queryOffset);
    editor->enterBlockAt(1);
    EXPECT_EQ(fake.calls.size(), 1);
}

TEST(QueryBlockTest, CaretInsideRevealsSourceInsteadOfResults)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver({makeQueryResult(QStringLiteral("One.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();
    ASSERT_FALSE(editor->queryRowsAt(note.indexOf(QStringLiteral("foo"))).isEmpty());

    editor->enterBlockAt(note.indexOf(QStringLiteral("foo")));
    // Fences are chrome, never part of displayText() even in source mode
    // (CodeOptionsTest.LineNumbersIndentTheListingAndLeaveTheBytesAlone makes
    // the same point) - what matters here is that the raw query text is what
    // shows, not a rendered result.
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("foo")));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("One.md")));
    EXPECT_TRUE(editor->queryRowsAt(note.indexOf(QStringLiteral("foo"))).isEmpty());
}

TEST(QueryBlockTest, ClickingARowActivatesItsNoteAndOffset)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    // The default caret (offset 0) starts inside the query block itself,
    // same as it would in any other block; it has to leave before the block
    // renders results rather than raw source.
    editor->enterBlockAt(note.indexOf(QStringLiteral("After")));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        {makeQueryResult(QStringLiteral("Target.md"), 2, QStringLiteral("has foo in it"), 42)});
    pumpQueuedDelivery();

    const qsizetype queryOffset = note.indexOf(QStringLiteral("foo"));
    const QList<MarkdownEditor::QueryResultRow> rows = editor->queryRowsAt(queryOffset);
    ASSERT_EQ(rows.size(), 1);

    QSignalSpy activated(editor.get(), &MarkdownEditor::queryResultActivated);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      rows.first().rect.center().toPoint());
    ASSERT_EQ(activated.size(), 1);
    EXPECT_EQ(activated.first().at(0).toString(), QStringLiteral("Target.md"));
    EXPECT_EQ(activated.first().at(1).value<qsizetype>(), 42);
}

TEST(QueryBlockTest, EditingElsewhereAsksNothingNew)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver({makeQueryResult(QStringLiteral("One.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();

    editor->setCursorOffset(5); // end of "Intro", nowhere near the query block
    QTest::keyClicks(editor.get(), QStringLiteral(" more"));
    EXPECT_EQ(fake.calls.size(), 1) << "a keystroke outside the block must not re-run its search";
}

TEST(QueryBlockTest, AStaleDeliveryAfterRefreshIsDropped)
{
    const QString note = QStringLiteral("Intro\n\n```query\nfoo\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    const MarkdownEditor::QueryDeliver firstDeliver = fake.calls.first().deliver;

    // The index changed while the first search was still in flight:
    // refreshQueries drops the cache and asks again for the same text.
    editor->refreshQueries();
    QTest::qWait(250); // past the 200 ms debounce
    QCoreApplication::processEvents();
    ASSERT_EQ(fake.calls.size(), 2);
    EXPECT_EQ(fake.calls.at(1).query, QStringLiteral("foo"));
    const MarkdownEditor::QueryDeliver secondDeliver = fake.calls.at(1).deliver;

    // The new request answers first (the common case: refreshQueries fires
    // because something changed, and the fresh search is what a person is
    // waiting on) and the old, slower one lands after it - a broken
    // generation check would let this second, stale delivery win simply by
    // arriving last.
    secondDeliver({makeQueryResult(QStringLiteral("Fresh.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();
    firstDeliver({makeQueryResult(QStringLiteral("Stale.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();

    const QList<MarkdownEditor::QueryResultRow> rows =
        editor->queryRowsAt(note.indexOf(QStringLiteral("foo")));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().path, QStringLiteral("Fresh.md"))
        << "the superseded request's answer must not overwrite the current one";
}

// --- the embedded search header (result count, collapse, sort) -------------------

TEST(QueryHeaderTest, ShowsResultCountSingularAndPlural)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\n```query\nbar\n```\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 2);

    const qsizetype fooOffset = note.indexOf(QStringLiteral("foo"));
    const qsizetype barOffset = note.indexOf(QStringLiteral("bar"));
    for (const FakeQueryRunner::Call &call : fake.calls) {
        if (call.query == QStringLiteral("foo"))
            call.deliver({makeQueryResult(QStringLiteral("One.md"), 1, QString(), 0)});
        else
            call.deliver({makeQueryResult(QStringLiteral("One.md"), 1, QString(), 0),
                          makeQueryResult(QStringLiteral("Two.md"), 1, QString(), 0)});
    }
    pumpQueuedDelivery();

    EXPECT_EQ(editor->queryHeaderAt(fooOffset).countText, QStringLiteral("1 result"));
    EXPECT_EQ(editor->queryHeaderAt(barOffset).countText, QStringLiteral("2 results"));
}

TEST(QueryHeaderTest, ClickingCollapseHidesRowsAndPersistsAcrossAnEdit)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    editor->enterBlockAt(note.indexOf(QStringLiteral("After"))); // leave the block so it renders
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        {makeQueryResult(QStringLiteral("One.md"), 1, QStringLiteral("a foo b"), 0)});
    pumpQueuedDelivery();

    const qsizetype offset = note.indexOf(QStringLiteral("foo"));
    ASSERT_FALSE(editor->queryRowsAt(offset).isEmpty());
    EXPECT_FALSE(editor->queryCollapsedAt(offset));

    MarkdownEditor::QueryHeader header = editor->queryHeaderAt(offset);
    ASSERT_TRUE(header.collapseRect.isValid());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      header.collapseRect.center().toPoint());

    EXPECT_TRUE(editor->queryCollapsedAt(offset));
    EXPECT_TRUE(editor->queryRowsAt(offset).isEmpty())
        << "collapsed hides the result rows, not just some header flag";
    EXPECT_FALSE(editor->queryHeaderAt(offset).countText.isEmpty())
        << "the header itself - the count to expand back from - stays";

    // Remembered for the block for the life of the editor even though
    // nothing was written to the note: an edit elsewhere leaves it collapsed.
    editor->setCursorOffset(note.indexOf(QStringLiteral("After")));
    QTest::keyClicks(editor.get(), QStringLiteral("!"));
    EXPECT_TRUE(editor->queryCollapsedAt(offset));

    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      editor->queryHeaderAt(offset).collapseRect.center().toPoint());
    EXPECT_FALSE(editor->queryCollapsedAt(offset));
    EXPECT_FALSE(editor->queryRowsAt(offset).isEmpty());
}

TEST(QueryHeaderTest, ClickingSortTogglesToFileNameOrder)
{
    const QString note = QStringLiteral("```query\nfoo\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    editor->enterBlockAt(note.indexOf(QStringLiteral("After")));
    ASSERT_EQ(fake.calls.size(), 1);
    // Delivered out of file-name order, so sorting is observable.
    fake.calls.first().deliver({makeQueryResult(QStringLiteral("Zebra.md"), 1, QString(), 0),
                                makeQueryResult(QStringLiteral("Apple.md"), 1, QString(), 0)});
    pumpQueuedDelivery();

    const qsizetype offset = note.indexOf(QStringLiteral("foo"));
    QList<MarkdownEditor::QueryResultRow> rows = editor->queryRowsAt(offset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("Zebra.md")) << "best-match: the delivered order";
    EXPECT_FALSE(editor->queryHeaderAt(offset).sortByName);

    const MarkdownEditor::QueryHeader header = editor->queryHeaderAt(offset);
    ASSERT_TRUE(header.sortRect.isValid());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      header.sortRect.center().toPoint());

    EXPECT_TRUE(editor->queryHeaderAt(offset).sortByName);
    rows = editor->queryRowsAt(offset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("Apple.md"));
    EXPECT_EQ(rows.at(1).path, QStringLiteral("Zebra.md"));

    // Toggling back restores delivery order, and neither toggle asks the
    // runner for anything - sorting is client-side over the cached results.
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      editor->queryHeaderAt(offset).sortRect.center().toPoint());
    EXPECT_FALSE(editor->queryHeaderAt(offset).sortByName);
    EXPECT_EQ(fake.calls.size(), 1) << "sorting is client-side, never a second search";
}

// --- base blocks -------------------------------------------------------------------

namespace {

/// A fake `setBaseRunner`, the same shape FakeQueryRunner already is for
/// setQueryRunner: records every YAML text asked for and hands the test the
/// `deliver` callback instead of calling it.
struct FakeBaseRunner {
    struct Call {
        QString yamlText;
        QString viewName;
        MarkdownEditor::BaseDeliver deliver;
    };
    QList<Call> calls;

    void operator()(const QString &yamlText, const QString &viewName,
                    const MarkdownEditor::BaseDeliver &deliver)
    {
        calls.append({yamlText, viewName, deliver});
    }
};

BaseTableData makeBaseTable(const QStringList &headers, QList<BaseTableRow> rows)
{
    BaseTableData table;
    table.viewNames = {QStringLiteral("Table")};
    table.shownView = 0;
    table.headers = headers;
    table.rows = std::move(rows);
    return table;
}

} // namespace

TEST(BaseBlockTest, RendersTableFromAnAsyncRunner)
{
    const QString note = QStringLiteral("Intro\n\n```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));

    const qsizetype baseOffset = note.indexOf(QStringLiteral("views:"));
    // Rendered but still waiting: the runner was asked, but nothing answered
    // yet, so there is nothing to click - render must never block on it.
    ASSERT_EQ(fake.calls.size(), 1);
    EXPECT_TRUE(editor->baseRowsAt(baseOffset).isEmpty());

    fake.calls.first().deliver(
        makeBaseTable({QStringLiteral("Name")}, {{{QStringLiteral("One")}, QStringLiteral("One.md")},
                                                 {{QStringLiteral("Two")}, QStringLiteral("Two.md")}}));
    pumpQueuedDelivery();

    const QList<MarkdownEditor::BaseRowRect> rows = editor->baseRowsAt(baseOffset);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).path, QStringLiteral("One.md"));
    EXPECT_EQ(rows.at(1).path, QStringLiteral("Two.md"));

    // Re-asking for the same YAML without an intervening edit or
    // refreshBases is a cache hit, not a second evaluation.
    editor->enterBlockAt(1);   // touches the "Intro" block, not the base one
    editor->enterBlockAt(baseOffset);
    editor->enterBlockAt(1);
    EXPECT_EQ(fake.calls.size(), 1);
}

TEST(BaseBlockTest, CaretInsideRevealsSourceInsteadOfTheTable)
{
    const QString note = QStringLiteral("```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        makeBaseTable({QStringLiteral("Name")}, {{{QStringLiteral("One")}, QStringLiteral("One.md")}}));
    pumpQueuedDelivery();

    const qsizetype baseOffset = note.indexOf(QStringLiteral("views:"));
    ASSERT_FALSE(editor->baseRowsAt(baseOffset).isEmpty());
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("order:")))
        << "rendered: the table shows cell text, not the YAML that produced it";

    editor->enterBlockAt(baseOffset);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("order:")));
    EXPECT_TRUE(editor->baseRowsAt(baseOffset).isEmpty());
}

TEST(BaseBlockTest, ClickingARowActivatesItsNote)
{
    const QString note =
        QStringLiteral("```base\nviews:\n  - order: [file.name]\n```\n\nAfter\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    // The default caret starts inside the base block itself; it has to
    // leave before the block renders a table rather than raw YAML.
    editor->enterBlockAt(note.indexOf(QStringLiteral("After")));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(makeBaseTable(
        {QStringLiteral("Name")}, {{{QStringLiteral("Target")}, QStringLiteral("Target.md")}}));
    pumpQueuedDelivery();

    const qsizetype baseOffset = note.indexOf(QStringLiteral("views:"));
    const QList<MarkdownEditor::BaseRowRect> rows = editor->baseRowsAt(baseOffset);
    ASSERT_EQ(rows.size(), 1);

    QSignalSpy activated(editor.get(), &MarkdownEditor::baseRowActivated);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      rows.first().rect.center().toPoint());
    ASSERT_EQ(activated.size(), 1);
    EXPECT_EQ(activated.first().at(0).toString(), QStringLiteral("Target.md"));
}

TEST(BaseBlockTest, EditingElsewhereAsksNothingNew)
{
    const QString note = QStringLiteral("Intro\n\n```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver(
        makeBaseTable({QStringLiteral("Name")}, {{{QStringLiteral("One")}, QStringLiteral("One.md")}}));
    pumpQueuedDelivery();

    editor->setCursorOffset(5); // end of "Intro", nowhere near the base block
    QTest::keyClicks(editor.get(), QStringLiteral(" more"));
    EXPECT_EQ(fake.calls.size(), 1) << "a keystroke outside the block must not re-evaluate it";
}

TEST(BaseBlockTest, AStaleDeliveryAfterRefreshIsDropped)
{
    const QString note = QStringLiteral("Intro\n\n```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    const MarkdownEditor::BaseDeliver firstDeliver = fake.calls.first().deliver;

    editor->refreshBases();
    QTest::qWait(250); // past the 200 ms debounce
    QCoreApplication::processEvents();
    ASSERT_EQ(fake.calls.size(), 2);
    const MarkdownEditor::BaseDeliver secondDeliver = fake.calls.at(1).deliver;

    secondDeliver(makeBaseTable({QStringLiteral("Name")},
                                {{{QStringLiteral("Fresh")}, QStringLiteral("Fresh.md")}}));
    pumpQueuedDelivery();
    firstDeliver(makeBaseTable({QStringLiteral("Name")},
                               {{{QStringLiteral("Stale")}, QStringLiteral("Stale.md")}}));
    pumpQueuedDelivery();

    const QList<MarkdownEditor::BaseRowRect> rows =
        editor->baseRowsAt(note.indexOf(QStringLiteral("views:")));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().path, QStringLiteral("Fresh.md"))
        << "the superseded request's answer must not overwrite the current one";
}

TEST(BaseBlockTest, FileEmbedFragmentNamesTheRequestedView)
{
    // ![[Books.base#Reading]] (obsidian.md/help/bases, "Embed a base"): the
    // fragment after "#" is a view name, not a heading/block reference the
    // way a note embed's is - MarkdownEditor only has to carry it through to
    // the runner unchanged; which view it selects is evaluateBaseForEditor's
    // job (BaseEditorTableTest.ViewNameSelectsThatViewCaseInsensitively).
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Books.base"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("views:\n  - name: Reading\n    order: [file.name]\n");
    }

    auto editor = editorWith(QStringLiteral("![[Books.base#Reading]]\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));

    ASSERT_EQ(fake.calls.size(), 1);
    EXPECT_EQ(fake.calls.first().viewName, QStringLiteral("Reading"));
}

TEST(BaseBlockTest, FencedBlockAsksForNoParticularView)
{
    // The other half of the same change: a ```base fence has no fragment to
    // name a view with, so it must keep asking for the default (empty
    // viewName, first view) exactly as before this feature existed.
    const QString note = QStringLiteral("```base\nviews:\n  - order: [file.name]\n```\n");
    auto editor = editorWith(note);
    FakeBaseRunner fake;
    editor->setBaseRunner(std::ref(fake));

    ASSERT_EQ(fake.calls.size(), 1);
    EXPECT_TRUE(fake.calls.first().viewName.isEmpty());
}

// --- CSV / TSV embeds ---------------------------------------------------------------

namespace {

QString writeTextFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    return path;
}

/// An editor showing `![[name]]`, resolved to `path`, and waited until the
/// table the off-thread read and parse produces has landed.
std::unique_ptr<MarkdownEditor> CSVeditorWith(const QString &path, bool waitForTable = true)
{
    auto editor = editorWith(QStringLiteral("![[%1]]\n").arg(QFileInfo(path).fileName()));
    editor->setLinkResolver([path](const QString &) { return path; });
    if (waitForTable)
        EXPECT_TRUE(QTest::qWaitFor([&] { return !editor->tableCellTextsAt(0).isEmpty(); }, 5000));
    return editor;
}

QList<QStringList> grid(std::initializer_list<std::initializer_list<const char *>> rows)
{
    QList<QStringList> out;
    for (const auto &row : rows) {
        QStringList cells;
        for (const char *cell : row)
            cells << QString::fromUtf8(cell);
        out << cells;
    }
    return out;
}

} // namespace

TEST(CSVembedTest, LaysOutAsATableWithItsHeaderAndCellTexts)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("data.csv"),
                                       "name,qty\nApple,3\n\"Smith, J\",\"say \"\"hi\"\"\"\n");
    auto editor = CSVeditorWith(path, false);
    EXPECT_TRUE(editor->tableCellTextsAt(0).isEmpty()) << "nothing is read on the layout path: loading first";
    EXPECT_NE(editor->panelTextAt(0).indexOf(QStringLiteral("Loading")), -1);

    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->tableCellTextsAt(0).isEmpty(); }, 5000));
    EXPECT_EQ(editor->tableCellTextsAt(0),
              grid({{"name", "qty"}, {"Apple", "3"}, {"Smith, J", "say \"hi\""}}));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("data.csv")));
    EXPECT_GT(editor->documentHeight(), 3 * QFontMetricsF(editor->font()).height());
}

TEST(CSVembedTest, ATableCellKeepsALineBreakAsALineSeparator)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("data.csv"), "a,b\r\n\"one\r\ntwo\",x\r\n");
    auto editor = CSVeditorWith(path);
    ASSERT_EQ(editor->tableCellTextsAt(0).size(), 2);
    EXPECT_EQ(editor->tableCellTextsAt(0).at(1).at(0), QStringLiteral("one") + QChar(QChar::LineSeparator) + QStringLiteral("two"));
}

TEST(CSVembedTest, SemicolonCSVAndTSVAndBOM)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("eu.csv"), "\xEF\xBB\xBFname;price\nTea;1,50\n"));
        EXPECT_EQ(editor->tableCellTextsAt(0), grid({{"name", "price"}, {"Tea", "1,50"}}));
    }
    {
        // .tsv is always tab, even where commas would win the sniff
        auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("t.tsv"), "a,b\tc\n1\t2,3\n"));
        EXPECT_EQ(editor->tableCellTextsAt(0), grid({{"a,b", "c"}, {"1", "2,3"}}));
    }
}

TEST(CSVembedTest, RaggedRowsArePaddedToTheWidestRow)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("r.csv"), "a,b,c\n1\n1,2,3,4\n"));
    EXPECT_EQ(editor->tableCellTextsAt(0), grid({{"a", "b", "c", ""}, {"1", "", "", ""}, {"1", "2", "3", "4"}}));
}

TEST(CSVembedTest, ALongFileShowsTheFirstRowsAndAFooter)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray bytes = "id,v\n";
    for (int i = 1; i <= 700; ++i)
        bytes += QByteArray::number(i) + ",x\n";
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("big.csv"), bytes));
    const QList<QStringList> rows = editor->tableCellTextsAt(0);
    ASSERT_EQ(rows.size(), 1 + 500 + 1) << "header, 500 rows, a footer";
    EXPECT_EQ(rows.at(500).at(0), QStringLiteral("500"));
    EXPECT_NE(rows.last().at(0).indexOf(QStringLiteral("200")), -1) << rows.last().at(0).toStdString();
}

TEST(CSVembedTest, AWideFileShowsFewerRowsSoLayoutStaysCheap)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray bytes;
    for (int row = 0; row <= 600; ++row)
        for (int column = 0; column < 40; ++column)
            bytes += QByteArray::number(row) + (column < 39 ? "," : "\n");
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("wide.csv"), bytes));
    const QList<QStringList> rows = editor->tableCellTextsAt(0);
    ASSERT_EQ(rows.size(), 1 + 37 + 1) << "header, 1500 cells' worth of rows, a footer";
    EXPECT_EQ(rows.at(37).at(0), QStringLiteral("37"));
    EXPECT_EQ(rows.last().at(0), QStringLiteral("and 563 more rows"));
}

TEST(CSVembedTest, AHeaderOnlyFileIsATableOfOneRow)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("h.csv"), "a,b\n"));
    EXPECT_EQ(editor->tableCellTextsAt(0), grid({{"a", "b"}}));
}

TEST(CSVembedTest, AnEmptyFileSaysSo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("e.csv"), ""), false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->panelTextAt(0).contains(QStringLiteral("empty"), Qt::CaseInsensitive); }, 5000))
        << editor->panelTextAt(0).toStdString();
    EXPECT_TRUE(editor->tableCellTextsAt(0).isEmpty());
}

TEST(CSVembedTest, AMissingFileFallsBackToTheLinkLikeAnyOtherBrokenEmbed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto editor = CSVeditorWith(dir.filePath(QStringLiteral("gone.csv")), false);
    QTest::qWait(50);
    EXPECT_TRUE(editor->tableCellTextsAt(0).isEmpty());
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("gone.csv")));
}

TEST(CSVembedTest, AnUnreadableFileShowsAnError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("locked.csv"), "a,b\n1,2\n");
    QFile::setPermissions(path, QFileDevice::Permissions());
    if (QFile(path).open(QIODevice::ReadOnly))
        GTEST_SKIP() << "this user can read a mode 000 file";
    auto editor = CSVeditorWith(path, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->panelTextAt(0).contains(QStringLiteral("locked.csv")); }, 5000))
        << editor->panelTextAt(0).toStdString();
    EXPECT_TRUE(editor->tableCellTextsAt(0).isEmpty());
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

TEST(CSVembedTest, AChangedFileIsReadAgain)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("live.csv"), "a,b\n1,2\n");
    auto editor = CSVeditorWith(path);
    ASSERT_EQ(editor->tableCellTextsAt(0), grid({{"a", "b"}, {"1", "2"}}));

    writeTextFile(dir, QStringLiteral("live.csv"), "a,b\n1,2\n3,4\n");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
    file.close();

    // the same note again, so every block is laid out afresh against the file as it is now
    editor->setNoteText(QStringLiteral("![[live.csv]]\n\nafter\n"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellTextsAt(0).size() == 3; }, 5000));
    EXPECT_EQ(editor->tableCellTextsAt(0).at(2), (QStringList{QStringLiteral("3"), QStringLiteral("4")}));
}

TEST(CSVembedTest, ABaseRefreshDoesNotRelayOutACSVTable)
{
    // refreshBases() fires on every index change - every note save in the
    // vault - and relays out each isBase block; a CSV table is not a base's.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("r.csv"), "a,b\n1,2\n"));
    ASSERT_FALSE(editor->tableCellTextsAt(0).isEmpty());
    const quint64 before = editor->layoutCount();
    editor->refreshBases();
    QTest::qWait(300); // past the 200 ms debounce
    EXPECT_EQ(editor->layoutCount(), before);
    editor->setBaseRunner([](const QString &, const QString &, const MarkdownEditor::BaseDeliver &) {});
    EXPECT_EQ(editor->layoutCount(), before);
    EXPECT_FALSE(editor->tableCellTextsAt(0).isEmpty());
}

TEST(CSVembedTest, AFileRewrittenWhileTheNoteIsOpenIsReadAgainWithNothingElseHappening)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("live.csv"), "a,b\n1,2\n");
    auto editor = CSVeditorWith(path);
    ASSERT_EQ(editor->tableCellTextsAt(0), grid({{"a", "b"}, {"1", "2"}}));

    // another program saves it: a new file renamed over the old, as most editors do
    const QString replacement = writeTextFile(dir, QStringLiteral("live.csv.tmp"), "a,b\n1,2\n3,4\n");
    QFile temporary(replacement);
    ASSERT_TRUE(temporary.open(QIODevice::ReadWrite));
    ASSERT_TRUE(temporary.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
    temporary.close();
    ASSERT_TRUE(QFile::remove(path));
    ASSERT_TRUE(QFile::rename(replacement, path));

    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellTextsAt(0).size() == 3; }, 5000));
    EXPECT_EQ(editor->tableCellTextsAt(0).at(2), (QStringList{QStringLiteral("3"), QStringLiteral("4")}));

    // and a second time, in place: the watch survived the rename
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("a,b\n1,2\n3,4\n5,6\n");
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellTextsAt(0).size() == 4; }, 5000));
}

TEST(CSVembedTest, TheOldTableStaysUpUntilTheNewOneLands)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("flicker.csv"), "a,b\n1,2\n");
    const QString note = QStringLiteral("![[flicker.csv]]\n");
    auto editor = CSVeditorWith(path);
    ASSERT_FALSE(editor->tableCellTextsAt(0).isEmpty());

    writeTextFile(dir, QStringLiteral("flicker.csv"), "a,b\n1,2\n3,4\n");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
    file.close();
    editor->setNoteText(note + QStringLiteral("\nafter\n")); // a layout, with the read still to come
    EXPECT_EQ(editor->tableCellTextsAt(0), grid({{"a", "b"}, {"1", "2"}})) << "not \"Loading...\" meanwhile";
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellTextsAt(0).size() == 3; }, 5000));
}

TEST(CSVembedTest, ThousandsOfColumnsAreCutAndSaidSo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray bytes(100000, ',');
    bytes += "\n";
    for (int i = 0; i < 500; ++i)
        bytes += QByteArray::number(i) + ",x\n";
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("wide.csv"), bytes));
    const QList<QStringList> rows = editor->tableCellTextsAt(0);
    ASSERT_GE(rows.size(), 3);
    EXPECT_EQ(rows.at(0).size(), 65) << "64 columns and a \"+N columns\" cell";
    EXPECT_EQ(rows.at(0).last(), QStringLiteral("+%1 columns").arg(100001 - 64));
    EXPECT_EQ(rows.at(1).size(), 65);
    EXPECT_NE(rows.last().at(0).indexOf(QStringLiteral("more rows")), -1) << rows.last().at(0).toStdString();
    EXPECT_NE(rows.last().at(0).indexOf(QStringLiteral("more columns")), -1);
}

TEST(CSVembedTest, AHugeCellIsCutAtAThousandCharacters)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray bytes = "h,i\n\"" + QByteArray(2 << 20, 'x') + "\",y\n";
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("cell.csv"), bytes));
    const QList<QStringList> rows = editor->tableCellTextsAt(0);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(1).at(0).size(), 1001);
    EXPECT_TRUE(rows.at(1).at(0).endsWith(QChar(0x2026)));
    EXPECT_EQ(rows.at(1).at(1), QStringLiteral("y"));
}

TEST(CSVembedTest, AFileOverTheReadLimitSaysSoInsteadOfBeingEmpty)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // one 33 MB line: nothing to cut it at, so what was read is shown (cut) and the footer says why
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("long.csv"), QByteArray(33 << 20, 'x')));
    const QList<QStringList> rows = editor->tableCellTextsAt(0);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_NE(rows.last().at(0).indexOf(QStringLiteral("32 MB")), -1) << rows.last().at(0).toStdString();
}

TEST(CSVembedTest, OtherTextEncodingsAreReadAsText)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("utf16.tsv"),
                                                  QByteArray("\xFF\xFE" "a\0\t\0" "\x0D\x01\n\0", 10)));
        EXPECT_EQ(editor->tableCellTextsAt(0).first(), (QStringList{QStringLiteral("a"), QString(QChar(0x010D))}));
    }
    {
        // Excel's "CSV" on Windows: a code page, not UTF-8
        auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("legacy.csv"), "name;city\nJo\xE9;Z\xFCrich\n"));
        EXPECT_EQ(editor->tableCellTextsAt(0).at(1),
                  (QStringList{QString::fromUtf8("Jo\xC3\xA9"), QString::fromUtf8("Z\xC3\xBC" "rich")}));
    }
}

TEST(CSVembedTest, ARelayoutOfTheWorstCappedTableTakesUnderTenMilliseconds)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // the cell budget is what bounds it, whatever the shape: a narrow file's 500 rows, a wide one's 64 columns
    for (const int columns : {2, 8, 64, 200}) {
        QByteArray bytes;
        for (int row = 0; row <= 600; ++row)
            for (int column = 0; column < columns; ++column)
                bytes += "cell " + QByteArray::number(row * column) + (column + 1 < columns ? "," : "\n");
        const QString name = QStringLiteral("budget%1.csv").arg(columns);
        auto editor = CSVeditorWith(writeTextFile(dir, name, bytes));
        ASSERT_FALSE(editor->tableCellTextsAt(0).isEmpty());
        const double best = perf::bestOf(
            [&] {
                QElapsedTimer timer;
                timer.start();
                editor->setNoteText(QStringLiteral("![[%1]]\n").arg(name)); // every block laid out afresh, the read cached
                return timer.nsecsElapsed() / 1e6;
            },
            7, 10.0);
        fprintf(stderr, "CSV relayout %d columns: %.2f ms\n", columns, best);
        EXPECT_LT(best, 10.0) << columns << " columns";
    }
}

TEST(CSVembedTest, ADeletedFileIsForgottenAndItsBlockLaidOutAgain)
{
    // the app's resolver answers {} for a file that is gone (so the block becomes the plain link,
    // and a restored file is found again by Index::changed - see the window tests); what the
    // watcher owes is dropping the read, which would otherwise count as "changed" at every event
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("gone.csv"), "a,b\n1,2\n");
    auto editor = CSVeditorWith(path);
    ASSERT_EQ(editor->CSVcacheSize(), 1);
    ASSERT_TRUE(QFile::remove(path));
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->CSVcacheSize() == 0; }, 5000));
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("gone.csv")));
}

TEST(CSVembedTest, AReadErrorIsRetriedWhenTheFileBecomesReadable)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("locked.csv"), "a,b\n1,2\n");
    QFile::setPermissions(path, QFileDevice::Permissions());
    if (QFile(path).open(QIODevice::ReadOnly))
        GTEST_SKIP() << "this user can read a mode 000 file";
    auto editor = CSVeditorWith(path, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->panelTextAt(0).contains(QStringLiteral("locked.csv")); }, 5000));

    // a chmod changes neither mtime nor size, only ctime (kept to the millisecond, so not within the one
    // the first chmod fell in): the watcher's attribute event is all there is
    QTest::qWait(20);
    const QDateTime mtime = QFileInfo(path).lastModified();
    ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    ASSERT_EQ(QFileInfo(path).lastModified(), mtime);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellTextsAt(0).size() == 2; }, 5000));
}

TEST(CSVembedTest, AReadErrorIsRetriedAtTheNextLayout)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("locked2.csv"), "a,b\n1,2\n");
    QFile::setPermissions(path, QFileDevice::Permissions());
    if (QFile(path).open(QIODevice::ReadOnly))
        GTEST_SKIP() << "this user can read a mode 000 file";
    auto editor = CSVeditorWith(path, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->panelTextAt(0).contains(QStringLiteral("locked2.csv")); }, 5000));
    QTest::qWait(20); // the ctime the cache is keyed on has millisecond resolution
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // the next layout of the block - here the note set again - asks again, whatever the watcher did
    editor->setNoteText(QStringLiteral("![[locked2.csv]]\n\nafter\n"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellTextsAt(0).size() == 2; }, 5000));
}

namespace {
std::atomic<int> gCSVreads{0};
}

TEST(CSVembedTest, AChmodIsOneReReadNotOnePerLayout)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("locked3.csv"), "a,b\n1,2\n");
    QFile::setPermissions(path, QFileDevice::Permissions());
    if (QFile(path).open(QIODevice::ReadOnly))
        GTEST_SKIP() << "this user can read a mode 000 file";
    gCSVreads = 0;
    MarkdownEditor::setCSVreadHookForTests([](const QString &) { ++gCSVreads; });
    auto editor = CSVeditorWith(path, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->panelTextAt(0).contains(QStringLiteral("locked3.csv")); }, 5000));
    EXPECT_EQ(gCSVreads.load(), 1);
    // layouts while it stays unreadable ask nothing: the ctime has not moved
    for (int i = 0; i < 3; ++i) {
        editor->setNoteText(QStringLiteral("![[locked3.csv]]\n\nlayout %1\n").arg(i));
        QTest::qWait(30);
    }
    EXPECT_EQ(gCSVreads.load(), 1);

    ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->tableCellTextsAt(0).size() == 2; }, 5000));
    for (int i = 0; i < 3; ++i) {
        editor->setNoteText(QStringLiteral("![[locked3.csv]]\n\nagain %1\n").arg(i));
        QTest::qWait(30);
    }
    MarkdownEditor::setCSVreadHookForTests(nullptr);
    EXPECT_EQ(gCSVreads.load(), 2) << "one read for the chmod, none for the layouts after it";
}

TEST(CSVembedTest, AFileAppendedToContinuouslyStillRefreshes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("log.csv"), "t,v\n0,0\n");
    auto editor = CSVeditorWith(path);
    ASSERT_EQ(editor->tableCellTextsAt(0).size(), 2);

    // a write every 100 ms, never a 200 ms pause: a plain debounce would wait for the end
    bool refreshedWhileWriting = false;
    for (int i = 1; i <= 30 && !refreshedWhileWriting; ++i) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::Append));
        file.write(QByteArray::number(i) + "," + QByteArray::number(i) + "\n");
        file.close();
        QTest::qWait(100);
        refreshedWhileWriting = editor->tableCellTextsAt(0).size() > 2;
    }
    EXPECT_TRUE(refreshedWhileWriting) << "within 3 s of constant writing";
}

TEST(CSVembedTest, AWorkerThatThrowsLeavesANoticeNotLoadingForever)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("boom.csv"), "a,b\n1,2\n");
    MarkdownEditor::setCSVreadHookForTests([](const QString &) { throw std::bad_alloc(); });
    auto editor = CSVeditorWith(path, false);
    const bool noticed = QTest::qWaitFor(
        [&] { return editor->panelTextAt(0).contains(QStringLiteral("Could not read boom.csv")); }, 5000);
    MarkdownEditor::setCSVreadHookForTests(nullptr);
    EXPECT_TRUE(noticed) << editor->panelTextAt(0).toStdString();
}

TEST(CSVembedTest, TurningTheFeatureOffForgetsTheFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto editor = CSVeditorWith(writeTextFile(dir, QStringLiteral("t.csv"), "a,b\n1,2\n"));
    EXPECT_EQ(editor->CSVcacheSize(), 1);
    MarkdownEditor::Features off;
    off.CSVembeds = false;
    editor->setFeatures(off);
    EXPECT_EQ(editor->CSVcacheSize(), 0);
}

TEST(CSVembedTest, TwoEmbedsOfOneFileBothLand)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("twice.csv"), "a,b\n1,2\n");
    const QString note = QStringLiteral("![[twice.csv]]\n\n![[twice.csv]]\n");
    auto editor = editorWith(note);
    editor->setLinkResolver([path](const QString &) { return path; });
    const qsizetype second = note.lastIndexOf(QStringLiteral("!["));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->tableCellTextsAt(0).isEmpty() && !editor->tableCellTextsAt(second).isEmpty(); }, 5000));
}

TEST(FeaturesTest, CSVembedsToggle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("t.csv"), "name,qty\nApple,3\n");
    auto editor = CSVeditorWith(path);
    ASSERT_FALSE(editor->tableCellTextsAt(0).isEmpty());

    MarkdownEditor::Features off;
    off.CSVembeds = false;
    editor->setFeatures(off);
    EXPECT_TRUE(editor->tableCellTextsAt(0).isEmpty()) << "off: no table";
    EXPECT_TRUE(editor->panelTextAt(0).contains(QStringLiteral("name,qty")))
        << "off: the file is a plain source listing again, as before CSV embeds existed";
}

// --- HTML blocks and inline HTML ----------------------------------------------------

TEST(HtmlRendererTest, AvailableExactlyWhenLitehtmlWasFound)
{
    // Available exactly when the build found litehtml; a build without it -
    // NOTEAPPC_WITH_HTML off, or the package missing - reports itself
    // unavailable rather than pretending, the same contract PdfPage keeps.
#ifdef NOTEAPPC_EXPECT_HTML
    EXPECT_TRUE(HtmlRenderer::isAvailable());
#else
    EXPECT_FALSE(HtmlRenderer::isAvailable());
#endif
}

namespace {

HtmlRenderer::Theme testHtmlTheme()
{
    HtmlRenderer::Theme theme;
    theme.text = QColor(20, 20, 20);
    theme.background = Qt::white;
    theme.link = Qt::blue;
    theme.border = Qt::gray;
    theme.codeBackground = QColor(230, 230, 230);
    theme.textFont = QFont(QStringLiteral("sans-serif"), 12);
    theme.codeFont = QFont(QStringLiteral("monospace"), 12);
    return theme;
}

} // namespace

TEST(HtmlRendererTest, ScriptNeverRendersItsText)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml (NOTEAPPC_WITH_HTML off, or the package was not found)";

    const HtmlRenderer::Theme theme = testHtmlTheme();
    const QImage plain = HtmlRenderer::render(QStringLiteral("<div>Hello</div>"), theme, 300, QString());
    const QImage withScript = HtmlRenderer::render(
        QStringLiteral("<div>Hello<script>document.title='pwned';alert(1)</script></div>"), theme, 300,
        QString());
    ASSERT_FALSE(plain.isNull());
    ASSERT_FALSE(withScript.isNull());
    // Not just "no visible <script> text": pixel-identical, so the tag
    // contributed nothing at all - not even the layout space a hidden but
    // present element would still occupy.
    EXPECT_EQ(plain, withScript) << "a <script> body must never add a pixel to the rendered block";
}

// A note picks the height: a million pixels at 800 wide would be a 3 GB image.
TEST(HtmlRendererTest, ABlockTooBigToDrawGetsANoticeNotAnImage)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    const HtmlRenderer::Theme theme = testHtmlTheme();

    const QImage tall = HtmlRenderer::render(QStringLiteral("<div style=\"height:1000000px\"></div>"), theme, 800,
                                             QString());
    ASSERT_FALSE(tall.isNull());
    EXPECT_LE(tall.height(), 64) << "a notice, not the block";
    // Too many pixels while under the height cap: 5000 x 16000 is 80 Mpx.
    const QImage wide = HtmlRenderer::render(QStringLiteral("<div style=\"height:16000px\"></div>"), theme, 5000,
                                             QString());
    ASSERT_FALSE(wide.isNull());
    EXPECT_LE(wide.height(), 64);
    // The device ratio counts: 800 x 10000 at 3x is 72 Mpx.
    const QImage dense = HtmlRenderer::render(QStringLiteral("<div style=\"height:10000px\"></div>"), theme, 800,
                                              QString(), 3.0);
    ASSERT_FALSE(dense.isNull());
    EXPECT_LE(dense.height(), 64 * 3);

    // A block under both caps is drawn whole, and a caller can lift the height cap (an EPUB chapter).
    const QImage fine = HtmlRenderer::render(QStringLiteral("<div style=\"height:2000px\"></div>"), theme, 800,
                                             QString());
    EXPECT_GT(fine.height(), 2000);
    const QImage chapter = HtmlRenderer::render(QStringLiteral("<div style=\"height:20000px\"></div>"), theme, 800,
                                                QString(), 1.0, QString(), nullptr, {}, 1 << 20);
    EXPECT_GT(chapter.height(), 20000);
}

TEST(HtmlRendererTest, GradientsRoundedCornersAndPerSideBorders)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    const HtmlRenderer::Theme theme = testHtmlTheme();
    const auto near = [](QColor a, QColor b) {
        return qAbs(a.red() - b.red()) < 40 && qAbs(a.green() - b.green()) < 40 && qAbs(a.blue() - b.blue()) < 40;
    };

    // litehtml's default body margin puts every box 8 px in.
    // A left-to-right gradient: red at one end, blue at the other, not one flat stop.
    const QImage gradient = HtmlRenderer::render(
        QStringLiteral("<div style=\"width:200px;height:40px;background:linear-gradient(to right, #ff0000, #0000ff)\"></div>"),
        theme, 300, QString());
    ASSERT_FALSE(gradient.isNull());
    EXPECT_TRUE(near(gradient.pixelColor(12, 28), Qt::red)) << gradient.pixelColor(12, 28).name().toStdString();
    EXPECT_TRUE(near(gradient.pixelColor(203, 28), Qt::blue)) << gradient.pixelColor(203, 28).name().toStdString();

    // A rounded box leaves its corner unpainted.
    const QImage rounded = HtmlRenderer::render(
        QStringLiteral("<div style=\"width:100px;height:60px;background:#00ff00;border-radius:20px\"></div>"),
        theme, 300, QString());
    EXPECT_TRUE(near(rounded.pixelColor(58, 38), Qt::green));
    EXPECT_FALSE(near(rounded.pixelColor(9, 9), Qt::green)) << "the corner is cut";

    // Each side its own colour.
    const QImage sides = HtmlRenderer::render(
        QStringLiteral("<div style=\"width:100px;height:60px;border-top:6px solid #ff0000;"
                       "border-bottom:6px solid #0000ff\"></div>"),
        theme, 300, QString());
    EXPECT_TRUE(near(sides.pixelColor(58, 10), Qt::red));
    EXPECT_TRUE(near(sides.pixelColor(58, 77), Qt::blue)) << sides.pixelColor(58, 77).name().toStdString();
}

namespace {

QImage renderBox(const QString &style)
{
    return HtmlRenderer::render(QStringLiteral("<div style=\"width:100px;height:60px;%1\"></div>").arg(style),
                                testHtmlTheme(), 300, QString());
}

/// Nothing drawn here: the block's image is transparent where no box paints.
bool isWhite(QColor c)
{
    return c.alpha() < 20 || (c.red() > 235 && c.green() > 235 && c.blue() > 235);
}

} // namespace

TEST(HtmlRendererTest, DoubleBorderLeavesAGap)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    // 9 px border at (8,8): outer third y 8-11, gap 11-14, inner third 14-17.
    const QImage image = renderBox(QStringLiteral("border:9px double #ff0000"));
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(58, 9), QColor(Qt::red));
    EXPECT_TRUE(isWhite(image.pixelColor(58, 12))) << image.pixelColor(58, 12).name().toStdString();
    EXPECT_EQ(image.pixelColor(58, 15), QColor(Qt::red));
    // The inside of the box is still empty.
    EXPECT_TRUE(isWhite(image.pixelColor(58, 40)));
}

TEST(HtmlRendererTest, GrooveRidgeInsetOutsetShadeTheirSides)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    const auto lum = [](QColor c) { return c.red() + c.green() + c.blue(); };
    const QColor base(0, 160, 0);
    // Box is 118 x 78 from (8,8): top band y 8-17, bottom band y 77-86.
    const QImage groove = renderBox(QStringLiteral("border:10px groove #00a000"));
    ASSERT_FALSE(groove.isNull());
    EXPECT_LT(lum(groove.pixelColor(58, 10)), lum(groove.pixelColor(58, 15))) << "groove top: outer half dark";
    EXPECT_GT(lum(groove.pixelColor(58, 84)), lum(groove.pixelColor(58, 79))) << "groove bottom: outer half light";
    const QImage ridge = renderBox(QStringLiteral("border:10px ridge #00a000"));
    EXPECT_GT(lum(ridge.pixelColor(58, 10)), lum(ridge.pixelColor(58, 15))) << "ridge top: outer half light";
    EXPECT_LT(lum(ridge.pixelColor(58, 84)), lum(ridge.pixelColor(58, 79))) << "ridge bottom: outer half dark";
    const QImage inset = renderBox(QStringLiteral("border:10px inset #00a000"));
    EXPECT_LT(lum(inset.pixelColor(58, 12)), lum(inset.pixelColor(58, 82))) << "inset: top darker than bottom";
    EXPECT_EQ(inset.pixelColor(58, 82), base);
    EXPECT_LT(lum(inset.pixelColor(10, 40)), lum(inset.pixelColor(124, 40))) << "inset: left darker than right";
    const QImage outset = renderBox(QStringLiteral("border:10px outset #00a000"));
    EXPECT_GT(lum(outset.pixelColor(58, 12)), lum(outset.pixelColor(58, 82))) << "outset: top lighter than bottom";
    EXPECT_EQ(outset.pixelColor(58, 12), base);
}

TEST(HtmlRendererTest, RadiiWithMismatchedSides)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    // Red top, blue bottom, no side borders, 20 px radius, one corner square.
    const QImage image = renderBox(QStringLiteral(
        "border-top:6px solid #ff0000;border-bottom:6px solid #0000ff;border-radius:20px 20px 20px 0"));
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(58, 10), QColor(Qt::red));
    EXPECT_EQ(image.pixelColor(58, 75), QColor(Qt::blue));
    EXPECT_TRUE(isWhite(image.pixelColor(9, 9))) << "the rounded top-left corner is cut, not mitred";
    EXPECT_TRUE(isWhite(image.pixelColor(124, 9))) << "so is the top-right one";
    EXPECT_EQ(image.pixelColor(10, 74), QColor(Qt::blue)) << "the square bottom-left corner is filled";
    // The red follows the curve: past the radius the stroke is on the top edge.
    EXPECT_EQ(image.pixelColor(30, 9), QColor(Qt::red));

    // Dashed with a differing side: the dashes leave gaps along the top.
    const QImage dashed = renderBox(QStringLiteral("border-top:4px dashed #ff0000;border-bottom:4px solid #0000ff"));
    int red = 0, gaps = 0;
    for (int x = 12; x < 120; ++x) {
        const QColor c = dashed.pixelColor(x, 10);
        red += c == QColor(Qt::red);
        gaps += isWhite(c);
    }
    EXPECT_GT(red, 20);
    EXPECT_GT(gaps, 10);
}

TEST(HtmlRendererTest, ListMarkersUseTheirNumeralSystem)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    // Markers are painted, not text runs, so compare against a list drawn
    // with the same items and the decimal style: the pictures must differ.
    const QImage roman = HtmlRenderer::render(
        QStringLiteral("<ol style=\"list-style-type:upper-roman\"><li>a</li><li>b</li><li>c</li><li>d</li></ol>"),
        testHtmlTheme(), 300, QString());
    const QImage decimal = HtmlRenderer::render(
        QStringLiteral("<ol><li>a</li><li>b</li><li>c</li><li>d</li></ol>"), testHtmlTheme(), 300, QString());
    ASSERT_FALSE(roman.isNull());
    EXPECT_NE(roman, decimal);
}

TEST(HTMLBlockTest, RendersAsANonEmptyCachedImage)
{
    // style= is what keeps these blocks litehtml's (HtmlEdit.cpp lays a plain
    // <div> out itself; tests/test_html_edit.cpp covers that side).
    const QString note = QStringLiteral("Intro\n\n<div style=\"padding: 4px\">\n<b>Hello</b> world\n</div>\n\nAfter\n");
    auto editor = editorWith(note);
    const QSize size = editor->htmlImageSizeAt(note.indexOf(QStringLiteral("Hello")));
    if (HtmlRenderer::isAvailable()) {
        EXPECT_GT(size.width(), 0);
        EXPECT_GT(size.height(), 0);
    } else {
        EXPECT_TRUE(size.isEmpty()) << "without litehtml the block falls back to showing its source";
    }
}

namespace {
/// Red pixels in what the editor paints: a solid red picture that was drawn leaves plenty.
qsizetype redPixels(MarkdownEditor &editor)
{
    const QImage image = editor.grab().toImage();
    qsizetype count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            count += c.red() > 200 && c.green() < 60 && c.blue() < 60;
        }
    }
    return count;
}
} // namespace

// Fails on the old code: the written path was kept when the resolver found nothing, and a CSS url() or an
// unquoted src was never looked at, so the renderer loaded whatever file the path named.
TEST(HTMLBlockTest, AnImageThatLeavesTheVaultIsNotDrawn)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    QTemporaryDir outer;
    ASSERT_TRUE(outer.isValid());
    const QString vault = outer.filePath(QStringLiteral("vault"));
    ASSERT_TRUE(QDir().mkpath(vault));
    QImage red(80, 80, QImage::Format_RGB32);
    red.fill(Qt::red);
    const QString outside = outer.filePath(QStringLiteral("outside.png"));
    ASSERT_TRUE(red.save(outside));
    ASSERT_TRUE(red.save(vault + QStringLiteral("/in.png")));
    // What the window's resolver does: a file of the vault, or nothing.
    const auto resolver = [vault](const QString &target) -> QString {
        const QString path = QDir::cleanPath(QDir(vault).absoluteFilePath(target));
        return path.startsWith(vault + QLatin1Char('/')) && QFileInfo::exists(path) ? path : QString();
    };
    const QString climbing = QDir::current().relativeFilePath(outside); // "../../..", resolved from the cwd
    const auto drawn = [&](const QString &html) {
        auto editor = editorWith(QStringLiteral("Intro\n\n") + html + QStringLiteral("\n\nAfter\n"));
        editor->setLinkResolver(resolver);
        return redPixels(*editor);
    };
    EXPECT_GT(drawn(QStringLiteral("<div style=\"padding:4px\"><img src=\"in.png\" width=\"80\" height=\"80\"></div>")), 500)
        << "control: a picture of the vault is drawn";
    for (const QString &bad : {outside, climbing}) {
        EXPECT_EQ(drawn(QStringLiteral("<div style=\"padding:4px\"><img src=\"%1\" width=\"80\" height=\"80\"></div>").arg(bad)), 0)
            << "quoted src: " << bad.toStdString();
        EXPECT_EQ(drawn(QStringLiteral("<div style=\"padding:4px\"><img src=%1 width=80 height=80></div>").arg(bad)), 0)
            << "unquoted src: " << bad.toStdString();
        EXPECT_EQ(drawn(QStringLiteral("<div style=\"width:80px;height:80px;background:url(%1)\"></div>").arg(bad)), 0)
            << "css url(): " << bad.toStdString();
    }
    EXPECT_GT(drawn(QStringLiteral("<div style=\"width:80px;height:80px;background:url(in.png)\"></div>")), 500)
        << "control: a css url() inside the vault is drawn";
}

// Fails on the old code: Qt's SVG plugin loads an <image href> from any file, so an SVG of the vault, which the
// resolver is happy with, drew a picture that was never in the vault - absolute, or "../" from its own folder.
TEST(HTMLBlockTest, AnSVGThatNamesAFileOutsideTheVaultDrawsNothingOfIt)
{
    QTemporaryDir outer;
    ASSERT_TRUE(outer.isValid());
    const QString vault = outer.filePath(QStringLiteral("vault"));
    ASSERT_TRUE(QDir().mkpath(vault + QStringLiteral("/sub")));
    QImage red(80, 80, QImage::Format_RGB32);
    red.fill(Qt::red);
    const QString outside = outer.filePath(QStringLiteral("outside.png"));
    ASSERT_TRUE(red.save(outside));
    const auto svgNaming = [](const QString &href) {
        return QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
                              "width='80' height='80'><image xlink:href='%1' width='80' height='80'/></svg>").arg(href).toUtf8();
    };
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(vault + QLatin1Char('/') + name);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    };
    write(QStringLiteral("evil.svg"), svgNaming(outside));
    write(QStringLiteral("sub/evil-relative.svg"), svgNaming(QStringLiteral("../../outside.png")));
    write(QStringLiteral("ok.svg"), "<svg xmlns='http://www.w3.org/2000/svg' width='80' height='80'><rect width='80' height='80' fill='red'/></svg>");
    const auto resolver = [vault](const QString &target) -> QString {
        const QString path = QDir::cleanPath(QDir(vault).absoluteFilePath(target));
        return path.startsWith(vault + QLatin1Char('/')) && QFileInfo::exists(path) ? path : QString();
    };
    const auto drawn = [&](const QString &note) {
        auto editor = editorWith(note);
        editor->setLinkResolver(resolver);
        return redPixels(*editor);
    };
    EXPECT_GT(drawn(QStringLiteral("Intro\n\n![[ok.svg]]\n\nAfter\n")), 500) << "control: an SVG of the vault is drawn";
    EXPECT_EQ(drawn(QStringLiteral("Intro\n\n![[evil.svg]]\n\nAfter\n")), 0) << "absolute href";
    EXPECT_EQ(drawn(QStringLiteral("Intro\n\n![[sub/evil-relative.svg]]\n\nAfter\n")), 0) << "relative href";
    if (HtmlRenderer::isAvailable()) {
        EXPECT_GT(drawn(QStringLiteral("Intro\n\n<div style=\"padding:4px\"><img src=\"ok.svg\" width=\"80\" height=\"80\"></div>\n\nAfter\n")), 500)
            << "control: an <img> of an SVG of the vault";
        EXPECT_EQ(drawn(QStringLiteral("Intro\n\n<div style=\"padding:4px\"><img src=\"evil.svg\" width=\"80\" height=\"80\"></div>\n\nAfter\n")), 0)
            << "<img> of an SVG that names a file";
    }
}

// Fails on the old code: an installed Qt image plugin (kimg_eps runs ghostscript) read any vault file
// it recognised, whatever it was called.
TEST(HTMLBlockTest, AFileOnlyAnImagePluginReadsIsNotDrawn)
{
    QTemporaryDir vaultDir;
    ASSERT_TRUE(vaultDir.isValid());
    const QString vault = vaultDir.path();
    QImage red(80, 80, QImage::Format_RGB32);
    red.fill(Qt::red);
    ASSERT_TRUE(red.save(vault + QStringLiteral("/ok.png")));
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(vault + QLatin1Char('/') + name);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    };
    const QByteArray eps = "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 80 80\n1 0 0 setrgbcolor 0 0 80 80 rectfill\nshowpage\n";
    write(QStringLiteral("evil.eps"), eps);
    write(QStringLiteral("pic.png"), eps);
    QByteArray tiff;
    QBuffer buffer(&tiff);
    buffer.open(QIODevice::WriteOnly);
    const bool haveTIFF = red.save(&buffer, "tiff");
    write(QStringLiteral("scan.tiff"), tiff);
    write(QStringLiteral("scan-as.png"), tiff);
    const auto resolver = [vault](const QString &target) -> QString {
        const QString path = QDir::cleanPath(QDir(vault).absoluteFilePath(target));
        return QFileInfo::exists(path) ? path : QString();
    };
    const auto drawn = [&](const QString &note) {
        auto editor = editorWith(note);
        editor->setLinkResolver(resolver);
        return redPixels(*editor);
    };
    EXPECT_GT(drawn(QStringLiteral("Intro\n\n![[ok.png]]\n\nAfter\n")), 500) << "control: a PNG is drawn";
    EXPECT_EQ(drawn(QStringLiteral("Intro\n\n![[evil.eps]]\n\nAfter\n")), 0) << ".eps";
    EXPECT_EQ(drawn(QStringLiteral("Intro\n\n![[pic.png]]\n\nAfter\n")), 0) << "an EPS called .png";
    if (haveTIFF) {
        EXPECT_EQ(drawn(QStringLiteral("Intro\n\n![[scan.tiff]]\n\nAfter\n")), 0) << ".tiff";
        EXPECT_EQ(drawn(QStringLiteral("Intro\n\n![[scan-as.png]]\n\nAfter\n")), 0) << "a TIFF called .png";
    }
    if (HtmlRenderer::isAvailable()) {
        EXPECT_EQ(drawn(QStringLiteral("Intro\n\n<div style=\"padding:4px\"><img src=\"evil.eps\" width=\"80\" height=\"80\"></div>\n\nAfter\n")), 0)
            << "<img> of an .eps";
        EXPECT_EQ(drawn(QStringLiteral("Intro\n\n<div style=\"padding:4px\"><img src=\"pic.png\" width=\"80\" height=\"80\"></div>\n\nAfter\n")), 0)
            << "<img> of an EPS called .png";
    }
}

TEST(HTMLBlockTest, CaretInsideRevealsSourceInsteadOfTheImage)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";

    const QString note = QStringLiteral("Intro\n\n<div style=\"padding: 4px\">\n<b>Hello</b> world\n</div>\n\nAfter\n");
    auto editor = editorWith(note);
    const qsizetype at = note.indexOf(QStringLiteral("Hello"));
    ASSERT_FALSE(editor->htmlImageSizeAt(at).isEmpty());

    editor->enterBlockAt(at);
    EXPECT_TRUE(editor->htmlImageSizeAt(at).isEmpty());
    EXPECT_EQ(editor->displayText(), note)
        << "the caret is inside it: raw HTML source, like any other block";
}

TEST(HTMLBlockTest, OpeningANoteWithNoHTMLConstructsNothingFromLitehtml)
{
    const qint64 before = HtmlRenderer::renderCount();
    auto editor = editorWith(QStringLiteral("# Title\n\nJust **prose**, a [[link]] and `code`.\n"));
    (void)editor;
    EXPECT_EQ(HtmlRenderer::renderCount(), before)
        << "no HTML block in the note: litehtml must never be asked to lay anything out";
}

TEST(HTMLBlockTest, UnrelatedKeystrokeIsACacheHitNotARebuild)
{
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";

    const QString note = QStringLiteral("Intro\n\n<div style=\"padding: 4px\">\n<b>Hello</b> world\n</div>\n\nAfter\n");
    auto editor = editorWith(note);
    ASSERT_FALSE(editor->htmlImageSizeAt(note.indexOf(QStringLiteral("Hello"))).isEmpty());
    const qint64 afterFirstRender = HtmlRenderer::renderCount();

    editor->setCursorOffset(5); // end of "Intro", nowhere near the HTML block
    QTest::keyClicks(editor.get(), QStringLiteral(" more"));
    EXPECT_EQ(HtmlRenderer::renderCount(), afterFirstRender)
        << "an edit outside the block must not rebuild its cached image";
}

TEST(InlineHtmlTest, RecognisedTagsFormatAndHideTheirMarkup)
{
    const QString note = QStringLiteral("Press <kbd>Ctrl</kbd> then <b>Go</b>.\n");
    auto editor = editorWith(note);
    EXPECT_EQ(editor->displayText(), QStringLiteral("Press Ctrl then Go.\n"))
        << "the tags hide, like any other syntax marker";

    const qsizetype ctrl = note.indexOf(QStringLiteral("Ctrl"));
    EXPECT_EQ(editor->formatAt(ctrl).font().family(), Theme::builtin().monospaceFont().family());
    const qsizetype go = note.indexOf(QStringLiteral(">Go<")) + 1;
    EXPECT_GE(editor->formatAt(go).fontWeight(), int(QFont::Bold));
}

TEST(InlineHtmlTest, TheCaretInsideAnElementRevealsItsTags)
{
    const QString note = QStringLiteral("Water is H<sub>2</sub>O and <b>Go</b>.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("2<")) + 1); // after the "2"
    EXPECT_EQ(editor->displayText(), QStringLiteral("Water is H<sub>2</sub>O and Go.\n"))
        << "that element's pair shows as written, the other stays hidden";
    EXPECT_EQ(editor->formatAt(note.indexOf(QStringLiteral("Go<"))).fontWeight() >= int(QFont::Bold), true);

    editor->enterBlockAt(note.indexOf(QStringLiteral("Go<")) + 1);
    EXPECT_EQ(editor->displayText(), QStringLiteral("Water is H2O and <b>Go</b>.\n"));
    EXPECT_GE(editor->formatAt(note.indexOf(QStringLiteral("Go<"))).fontWeight(), int(QFont::Bold))
        << "revealed tags still format their text";

    editor->enterBlockAt(note.indexOf(QStringLiteral("is"))); // in the block, outside both
    EXPECT_EQ(editor->displayText(), QStringLiteral("Water is H2O and Go.\n"));
}

TEST(InlineHtmlTest, AScriptShowsAsRedBoldSourceAndNeverRuns)
{
    const QString note = QStringLiteral("Before <script>alert(1)</script> after.\n\n"
                                        "<script>document.title = \"RAN\";</script>\n\nend\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QStringLiteral("end")));
    // Inline: the tags stay, the whole element is the error colour, bold.
    EXPECT_TRUE(editor->displayText().startsWith(QStringLiteral("Before <script>alert(1)</script> after.")))
        << editor->displayText().toStdString();
    const QTextCharFormat inside = editor->formatAt(note.indexOf(QStringLiteral("alert")));
    EXPECT_GE(inside.fontWeight(), int(QFont::Bold));
    EXPECT_EQ(inside.foreground().color(), Theme::builtin().color(QStringLiteral("--text-error")));
    EXPECT_GE(editor->formatAt(note.indexOf(QStringLiteral("<script>"))).fontWeight(), int(QFont::Bold));
    // A block: drawn (its source as text) rather than left as raw purple
    // source, and nothing it says has happened.
    if (HtmlRenderer::isAvailable())
        EXPECT_FALSE(editor->htmlImageSizeAt(note.indexOf(QStringLiteral("document"))).isEmpty());
    EXPECT_NE(editor->window()->windowTitle(), QStringLiteral("RAN"));
}

TEST(InlineHtmlTest, SpanStyleColourIsRead)
{
    const QString note = QStringLiteral("A <span style=\"color: #ff0000\">warning</span> word.\n");
    auto editor = editorWith(note);
    const qsizetype at = note.indexOf(QStringLiteral("warning"));
    EXPECT_EQ(editor->formatAt(at).foreground().color(), QColor(0xff, 0, 0));
}

TEST(InlineHtmlTest, AnUnrecognisedTagStaysAsRawSource)
{
    // No case in htmlTagFormat matches <marquee>, so layoutInlineHtml never
    // hides it - the same "leave it alone rather than guess" rule an
    // unrenderable formula follows.
    const QString note = QStringLiteral("A <marquee>party</marquee> word.\n");
    auto editor = editorWith(note);
    EXPECT_EQ(editor->displayText(), note);
}

TEST(InlineHtmlTest, ABlockLevelElementIsLeftToLayoutHtml)
{
    // layoutInlineHtml steps aside entirely for a whole-block HTML span -
    // pairing tags inside it would fight with litehtml's own rendering of
    // the same markup.
    if (!HtmlRenderer::isAvailable())
        GTEST_SKIP() << "built without litehtml";
    const QString note = QStringLiteral("<div style=\"margin: 0\">\n<b>Hello</b>\n</div>\n");
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->htmlImageSizeAt(note.indexOf(QStringLiteral("Hello"))).isEmpty());
}

// --- page preview ----------------------------------------------------------------

namespace {

/// Moves the pointer across the first line until the editor reports a link,
/// holding `modifiers`; the position of the link is the editor's business.
QString hoverFirstLine(MarkdownEditor &editor, Qt::KeyboardModifiers modifiers)
{
    QSignalSpy hovered(&editor, &MarkdownEditor::linkHovered);
    for (int x = 20; x < editor.viewport()->width() && hovered.isEmpty(); x += 3) {
        const QPointF at(x, 40);
        QMouseEvent move(QEvent::MouseMove, at, editor.viewport()->mapToGlobal(at), Qt::NoButton,
                         Qt::NoButton, modifiers);
        QCoreApplication::sendEvent(editor.viewport(), &move);
    }
    return hovered.isEmpty() ? QString() : hovered.first().at(0).toString();
}

} // namespace

TEST(PagePreviewTest, ReadingViewPreviewsOnHoverAndEditingNeedsCtrl)
{
    // Obsidian: plain hover in reading view, Ctrl held while editing.
    const QString note = QStringLiteral("[[Other note#Part two]] [[Other note#Part two]] and more\n");
    auto editing = editorWith(note);
    EXPECT_TRUE(hoverFirstLine(*editing, Qt::NoModifier).isEmpty());
    EXPECT_EQ(hoverFirstLine(*editing, Qt::ControlModifier), QStringLiteral("Other note#Part two"))
        << "the heading comes with the note, so the preview can open there";

    auto reading = editorWith(note);
    reading->setMode(MarkdownEditor::Mode::Reading);
    EXPECT_EQ(hoverFirstLine(*reading, Qt::NoModifier), QStringLiteral("Other note#Part two"));
}

TEST(PagePreviewTest, AnExternalLinkHasNothingToPreview)
{
    auto reading = editorWith(QStringLiteral("[a site](https://example.org) and [a site](https://example.org)\n"));
    reading->setMode(MarkdownEditor::Mode::Reading);
    EXPECT_TRUE(hoverFirstLine(*reading, Qt::NoModifier).isEmpty());
}

// --- export to PDF ---------------------------------------------------------------

TEST(PdfExportTest, APageBreaksBetweenBlocksAndOnlyATallOneIsCut)
{
    // Blocks as (top, height), pages 100 high.
    EXPECT_EQ(MarkdownEditor::pageStarts({{0, 40}, {40, 50}}, 100), (QList<qreal>{0}));
    // The third block would cross the edge, so it starts page two.
    EXPECT_EQ(MarkdownEditor::pageStarts({{0, 40}, {40, 50}, {90, 30}}, 100),
              (QList<qreal>{0, 90}));
    // Taller than a page: it fills the rest of this page and runs on, cut at
    // the page height as many times as it takes - as a browser prints it.
    EXPECT_EQ(MarkdownEditor::pageStarts({{0, 20}, {20, 250}}, 100),
              (QList<qreal>{0, 100, 200}));
}

TEST(PdfExportTest, ALongNoteBecomesSeveralPages)
{
    QString note = QStringLiteral("---\ntags: [a]\n---\n");
    for (int i = 0; i < 120; ++i)
        note += QStringLiteral("Paragraph %1 with **bold** text that runs on for a while.\n\n").arg(i);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("out.pdf"));
    QString error;
    ASSERT_TRUE(MarkdownEditor::exportPdf(path, note, QStringLiteral("A title"), {},
                                          MarkdownEditor::PdfOptions{}, &error))
        << qPrintable(error);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray pdf = file.readAll();
    EXPECT_TRUE(pdf.startsWith("%PDF"));
    static const QRegularExpression pageObject(QStringLiteral("/Type\\s*/Page[^s]"));
    EXPECT_GE(QString::fromLatin1(pdf).count(pageObject), 3) << "120 paragraphs on A4";
}

TEST(PdfExportTest, AFailedExportLeavesTheEarlierFileAlone)
{
    // Writes past a 1 KiB file size limit fail (EFBIG), as a full disk does; the PDF of a long note is far larger.
    QString note;
    for (int i = 0; i < 60; ++i)
        note += QStringLiteral("Paragraph %1 with **bold** text that runs on for a while.\n\n").arg(i);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("out.pdf"));
    {
        QFile old(path);
        ASSERT_TRUE(old.open(QIODevice::WriteOnly));
        old.write("EARLIER");
    }
    struct rlimit saved {};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &saved), 0);
    ::signal(SIGXFSZ, SIG_IGN);
    struct rlimit tight = saved;
    tight.rlim_cur = 1024;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &tight), 0);
    QString error;
    const bool ok = MarkdownEditor::exportPdf(path, note, QStringLiteral("T"), {}, MarkdownEditor::PdfOptions{}, &error);
    ::setrlimit(RLIMIT_FSIZE, &saved);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.isEmpty());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray("EARLIER"));
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1) << "no temporary file left behind";
}

// --- audio and video ------------------------------------------------------------

TEST(MediaEmbedTest, AudioAndVideoBecomePlayerCards)
{
    // The card's file only has to resolve for the layout; playing it is the
    // media backend's business.
    const auto heightOf = [](const QString &note) {
        auto editor = std::make_unique<MarkdownEditor>();
        editor->resize(900, 600);
        editor->setLinkResolver([](const QString &target) { return QStringLiteral("/nowhere/") + target; });
        editor->setNoteText(note);
        return editor->documentHeight();
    };
    const qreal audio = heightOf(QStringLiteral("![[song.mp3]]\n"));
    const qreal video = heightOf(QStringLiteral("![[clip.mp4]]\n"));
    const qreal plain = heightOf(QStringLiteral("song.mp3\n"));
    EXPECT_GT(audio, plain) << "an audio embed is a card with a header and controls";
    EXPECT_GT(video, audio + 200) << "a video card has a picture above its controls";
}

// --- links to headings -------------------------------------------------------

TEST(HeadingLinkTest, ANoteAndHeadingReadAsNoteThenHeading)
{
    // Obsidian shows [[Note#Heading]] as "Note > Heading". The "#" stays in
    // the display text, in invisible ink and as wide as " > ", which the
    // paint step draws over it.
    const QString note = QStringLiteral("See [[Note#Heading]] here.\n");
    auto editor = editorWith(note);
    EXPECT_EQ(editor->displayText(), QStringLiteral("See Note#Heading here.\n"));
    const QTextCharFormat hash = editor->formatAt(note.indexOf(QLatin1Char('#')));
    EXPECT_EQ(hash.foreground().color().alpha(), 0) << "the \"#\" is drawn over, not shown";
    EXPECT_GT(hash.fontLetterSpacing(), 0.0) << "and leaves room for \" > \"";
}

TEST(HeadingLinkTest, AHeadingInTheSameNoteReadsAsTheHeading)
{
    auto editor = editorWith(QStringLiteral("See [[#Heading]] here.\n"));
    EXPECT_EQ(editor->displayText(), QStringLiteral("See Heading here.\n"));
}

TEST(HeadingLinkTest, TheCaretInsideShowsTheLinkAsWritten)
{
    const QString note = QStringLiteral("See [[Note#Heading]] here.\n");
    auto editor = editorWith(note);
    editor->enterBlockAt(note.indexOf(QLatin1String("Head")));
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("[[Note#Heading]]")));
    EXPECT_NE(editor->formatAt(note.indexOf(QLatin1Char('#'))).foreground().color().alpha(), 0);
}

TEST(HeadingLinkTest, BlockReferencesAndAliasesAreLeftAlone)
{
    const QString block = QStringLiteral("See [[Note#^id]] here.\n");
    auto blockEditor = editorWith(block);
    EXPECT_EQ(blockEditor->displayText(), QStringLiteral("See Note#^id here.\n"));
    EXPECT_NE(blockEditor->formatAt(block.indexOf(QLatin1Char('#'))).foreground().color().alpha(), 0);

    auto aliased = editorWith(QStringLiteral("See [[Note#Heading|the part]] here.\n"));
    EXPECT_EQ(aliased->displayText(), QStringLiteral("See the part here.\n"));
}

TEST(HeadingLinkTest, ALinkRecoloursWhenItsNoteAppears)
{
    // A link drawn before its note was indexed stayed "missing" until the
    // next edit: nothing told the editor the index had changed.
    bool exists = false;
    const QString note = QStringLiteral("See [[Other]] here.\n");
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setLinkResolver([&exists](const QString &target) {
        return exists && target == QLatin1String("Other") ? QStringLiteral("/v/Other.md")
                                                           : QString();
    });
    editor->setNoteText(note);
    const qsizetype at = note.indexOf(QLatin1String("Other")) + 1;
    const QColor missing = editor->formatAt(at).foreground().color();

    exists = true;
    editor->refreshLinkStatus();
    EXPECT_NE(editor->formatAt(at).foreground().color(), missing);
}

// --- scrolling -------------------------------------------------------------

TEST(ScrollTest, AScrolledFrameMatchesAFreshOne)
{
    // A small scroll moves the pixels already on screen and paints only the
    // strip that came in. Whatever that leaves on screen has to be exactly
    // what painting the whole frame would have drawn.
    QString note;
    for (int i = 0; i < 400; ++i) {
        if (i % 12 == 0)
            note += QStringLiteral("## Section %1\n\n").arg(i / 12);
        note += QStringLiteral("Line %1 with **bold**, `code` and a [[link %1]].\n").arg(i);
        if (i % 30 == 29)
            note += QStringLiteral("\n```cpp\nint x = %1;\n```\n\n").arg(i);
    }
    auto editor = parsedEditorWith(note);
    ASSERT_TRUE(editor);
    editor->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(editor.get()));
    // Where the pointer rests is not what is compared: the listing under it
    // says "Copy", and which one that is depends on how tall the text above is.
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(editor.get(), &leave);
    editor->viewport()->setAttribute(Qt::WA_UnderMouse, false);

    QScrollBar *bar = editor->verticalScrollBar();
    for (int step = 0; step < 60; ++step) {
        bar->setValue(bar->value() + (step < 40 ? 37 : -53)); // odd sizes, both ways
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();

    QScreen *screen = editor->screen();
    const QImage shown = screen->grabWindow(editor->winId()).toImage().convertToFormat(QImage::Format_RGB32);
    const QImage fresh = editor->grab().toImage().convertToFormat(QImage::Format_RGB32);
    ASSERT_EQ(shown.size(), fresh.size());
    EXPECT_TRUE(shown == fresh) << "what the scroll left on screen differs from a full repaint";
}



// --- a table edited where it is drawn (TableCells.cpp) ---------------------

namespace {

const QString kTableNote = QStringLiteral("above\n\n| name | qty |\n|---|---:|\n| apple | 3 |\n| pear |  |\n\nbelow\n");

/// Clicks across and down the note until the caret lands in `range`; the
/// point it did, or a null point.
QPoint clickInto(MarkdownEditor &editor, Range range)
{
    editor.setTableHandles(false); // a probe over the whole note would press the grips and bars too
    for (int y = 4; y < 400; y += 4) {
        for (int x = 4; x < editor.viewport()->width(); x += 12) {
            QTest::mouseClick(editor.viewport(), Qt::LeftButton, {}, QPoint(x, y));
            if (editor.cursorOffset() > range.start && editor.cursorOffset() < range.end)
                return QPoint(x, y);
        }
    }
    return {};
}

/// A drop the way Qt delivers one: to the viewport, and only after a drag
/// entering it was accepted. True when the editor took the drop.
bool dropOnto(MarkdownEditor &editor, QPoint point, const QMimeData &mime)
{
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &enter);
    if (!enter.isAccepted())
        return false;
    QDropEvent drop(QPointF(point), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &drop);
    return drop.isAccepted();
}

} // namespace

TEST(TableCellTest, AClickEditsTheCellAndTheTableStaysATable)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    ASSERT_TRUE(editor->tableRenderedAt(apple));
    ASSERT_FALSE(clickInto(*editor, {apple, apple + 5}).isNull());
    EXPECT_TRUE(editor->tableRenderedAt(apple)); // not dropped to "| apple | 3 |"

    editor->setCursorOffset(apple + 5);
    QTest::keyClicks(editor.get(), QStringLiteral("s|x"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| apples\\|x | 3 |")));
    EXPECT_TRUE(editor->tableRenderedAt(apple));
}

// Tab from the last cell adds a row, and the row is parsed once: the newline in
// it is parsed on the spot and the command must not parse again.
TEST(TableCellTest, ANewRowIsParsedOnce)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype lastCell = kTableNote.indexOf(QStringLiteral("|  |\n\nbelow")) + 2;
    editor->setCursorOffset(lastCell);
    const int before = editor->parseCountForTest();
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| pear |  |\n|")));
    EXPECT_EQ(editor->parseCountForTest(), before + 1);
}

TEST(TableCellTest, KeysMoveBetweenCellsAndNeverEatAPipe)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    const qsizetype three = kTableNote.indexOf(QStringLiteral("3 |"));
    editor->setCursorOffset(apple);

    QTest::keyClick(editor.get(), Qt::Key_Backspace); // at the cell's start: nothing
    EXPECT_EQ(editor->noteText(), kTableNote);
    editor->setCursorOffset(apple + 5);
    QTest::keyClick(editor.get(), Qt::Key_Right); // past the end: into the next cell
    EXPECT_EQ(editor->cursorOffset(), three);
    QTest::keyClick(editor.get(), Qt::Key_Left);
    EXPECT_EQ(editor->cursorOffset(), apple + 5);

    QTest::keyClick(editor.get(), Qt::Key_Tab);
    EXPECT_EQ(editor->cursorOffset(), three + 1);
    QTest::keyClick(editor.get(), Qt::Key_Return); // down a row, same column: the empty cell
    QTest::keyClicks(editor.get(), QStringLiteral("7"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| pear | 7 |")));

    // Tab from the last cell adds a row, as Advanced Tables does.
    QTest::keyClick(editor.get(), Qt::Key_Tab);
    QTest::keyClicks(editor.get(), QStringLiteral("fig"));
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| pear | 7 |\n| fig |  |\n")))
        << editor->noteText().toStdString();
    EXPECT_TRUE(editor->tableRenderedAt(apple));
}

TEST(TableCellTest, SourceModeStillShowsThePipes)
{
    auto editor = editorWith(kTableNote);
    editor->setMode(MarkdownEditor::Mode::Source);
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    editor->setCursorOffset(apple - 2);
    EXPECT_EQ(editor->cursorOffset(), apple - 2); // no snapping into cells there
    EXPECT_FALSE(editor->tableRenderedAt(apple));
}

TEST(ClickToEditTest, MathsAndHTMLOpenWhereTheyAreClicked)
{
    // Drawn as images, they once let a click fall through to the paragraph
    // below; only the sliver above them opened them. A fresh editor per
    // click, since an opened block shows its source and moves everything.
    for (const QString &element : {QStringLiteral("$$\nx^2 + y^2 = z^2\n$$"), QStringLiteral("<div>hello html</div>")}) {
        const QString text = QStringLiteral("above paragraph\n\n") + element + QStringLiteral("\n\nbelow paragraph\n");
        const qsizetype start = text.indexOf(element);
        int landed = 0;
        for (int y = 2; y < 300; y += 2) {
            auto editor = editorWith(text);
            editor->show();
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, QPoint(editor->viewport()->width() / 2, y));
            if (editor->cursorOffset() >= start && editor->cursorOffset() <= start + element.size())
                ++landed;
        }
        EXPECT_GE(landed, 10) << element.toStdString() << ": only " << landed * 2 << " px of it opens on a click";
    }
}

TEST(TableFormatEditorTest, LeavingAnEditedTableAlignsItAndNothingElse)
{
    const QString note = QStringLiteral("para  with  spaces\n\n| a | b |\n|---|--:|\n| 1 | 22 |\n\n|x|y|\n|-|-|\n|1|2|\n\nend\n");
    for (const MarkdownEditor::Mode mode : {MarkdownEditor::Mode::LivePreview, MarkdownEditor::Mode::Source}) {
        auto editor = editorWith(note);
        editor->setMode(mode);
        editor->show();
        editor->setCursorOffset(note.indexOf(QStringLiteral("22")) + 2);
        QTest::keyClicks(editor.get(), QStringLiteral("333"));
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| 1 | 22333 |"))); // not yet: still in it
        editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("end")) + 1);
        const QString after = editor->noteText();
        EXPECT_TRUE(after.contains(QStringLiteral("| a   |     b |\n| --- | ----: |\n| 1   | 22333 |\n")))
            << after.toStdString();
        EXPECT_TRUE(after.startsWith(QStringLiteral("para  with  spaces\n\n"))); // the rest byte for byte
        EXPECT_TRUE(after.contains(QStringLiteral("|x|y|\n|-|-|\n|1|2|\n")));   // an unedited table too
        EXPECT_EQ(editor->cursorOffset(), after.indexOf(QStringLiteral("end")) + 1); // the caret moved with its text

        QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier); // the alignment is its own step
        EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| 1 | 22333 |")));
    }
}

TEST(TableFormatEditorTest, FormatAllTablesKeepsTheCaretInItsCell)
{
    const QString note = QStringLiteral("|x|long header|\n|-|-|\n|1|2|\n\n|p|q|\n|:-:|-|\n|r|s|\n");
    auto editor = editorWith(note);
    editor->show();
    const qsizetype two = note.indexOf(QStringLiteral("2|"));
    editor->setCursorOffset(two + 1); // after the "2"
    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:format-all-tables")));
    EXPECT_EQ(editor->noteText(),
              QStringLiteral("| x   | long header |\n| --- | ----------- |\n| 1   | 2           |\n\n"
                             "|  p  | q   |\n| :-: | --- |\n|  r  | s   |\n"));
    EXPECT_EQ(editor->noteText().at(editor->cursorOffset() - 1), QLatin1Char('2'));
}

TEST(TableCommandTest, RowsAndColumnsAsAdvancedTablesDoes)
{
    const QString note = QStringLiteral("| a | b |\n| --- | --: |\n| 1 | 2 |\n| 3 | 4 |\n");
    const auto run = [&](qsizetype caret, const char *id) {
        auto editor = editorWith(note);
        editor->show();
        editor->setCursorOffset(caret);
        EXPECT_TRUE(editor->runCommand(QString::fromLatin1(id))); // a known command runs, even as a no-op
        // What it changed; nothing, for a row with nowhere to go or a caret not in a table.
        const QString after = editor->noteText();
        return std::make_pair(after != note ? after : QString(), after.at(editor->cursorOffset()));
    };
    const qsizetype one = note.indexOf(QLatin1Char('1'));
    const qsizetype four = note.indexOf(QLatin1Char('4'));

    EXPECT_EQ(run(one, "noteAppC:table-insert-row-after").first,
              QStringLiteral("| a   |   b |\n| --- | --: |\n| 1   |   2 |\n|     |     |\n| 3   |   4 |\n"));
    EXPECT_EQ(run(one, "noteAppC:table-delete-row").first,
              QStringLiteral("| a   |   b |\n| --- | --: |\n| 3   |   4 |\n"));
    auto moved = run(one, "noteAppC:table-move-row-down");
    EXPECT_EQ(moved.first, QStringLiteral("| a   |   b |\n| --- | --: |\n| 3   |   4 |\n| 1   |   2 |\n"));
    EXPECT_EQ(moved.second, QLatin1Char('1')); // the caret went with its row
    EXPECT_EQ(run(one, "noteAppC:table-move-row-up").first, QString()); // already the first body row
    EXPECT_EQ(run(four, "noteAppC:table-insert-column-before").first,
              QStringLiteral("| a   |     |   b |\n| --- | --- | --: |\n| 1   |     |   2 |\n| 3   |     |   4 |\n"));
    EXPECT_EQ(run(four, "noteAppC:table-delete-column").first,
              QStringLiteral("| a   |\n| --- |\n| 1   |\n| 3   |\n"));
    moved = run(one, "noteAppC:table-move-column-right");
    EXPECT_EQ(moved.first, QStringLiteral("|   b | a   |\n| --: | --- |\n|   2 | 1   |\n|   4 | 3   |\n"));
    EXPECT_EQ(moved.second, QLatin1Char('1'));
    EXPECT_EQ(run(0, "noteAppC:table-delete-row").first, QString()); // nowhere near a cell
}

// --- paste and drop (PasteDrop.cpp) -----------------------------------------

TEST(PasteDropTest, ImagePasteCallsTheSaverAndInsertsTheEmbed)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);

    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    QGuiApplication::clipboard()->setImage(image);

    QString gotName;
    QByteArray gotBytes;
    editor->setAttachmentSaver([&](const QString &name, const QByteArray &bytes) {
        gotName = name;
        gotBytes = bytes;
        return QStringLiteral("![[") + name + QStringLiteral("]]");
    });

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_TRUE(gotName.startsWith(QStringLiteral("Pasted image "))) << gotName.toStdString();
    EXPECT_TRUE(gotName.endsWith(QStringLiteral(".png")));
    ASSERT_FALSE(gotBytes.isEmpty());
    EXPECT_TRUE(QImage::fromData(gotBytes, "PNG").size() == image.size());
    EXPECT_EQ(editor->noteText(), QStringLiteral("hello![[") + gotName + QStringLiteral("]]\n"));
}

TEST(PasteDropTest, ShiftPasteIsTextOnlyEvenWithAnImageOnTheClipboard)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);

    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QGuiApplication::clipboard()->setImage(image);

    bool saverCalled = false;
    editor->setAttachmentSaver([&](const QString &, const QByteArray &) {
        saverCalled = true;
        return QString();
    });

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);

    EXPECT_FALSE(saverCalled);
    EXPECT_EQ(editor->noteText(), QStringLiteral("hello\n")); // no text on the clipboard either
}

TEST(PasteDropTest, NoSaverSetDoesNothingForAnImagePaste)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::green);
    QGuiApplication::clipboard()->setImage(image);

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("hello\n"));
}

// Obsidian: a drop makes [title](url), a paste of the same URL is the URL as typed.
TEST(PasteDropTest, PasteOfABrowserLinkIsTheBareUrl)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->setCursorOffset(5);
    auto *mime = new QMimeData;
    mime->setUrls({QUrl(QStringLiteral("https://example.com/a"))});
    mime->setText(QStringLiteral("A title"));
    QGuiApplication::clipboard()->setMimeData(mime);

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("hellohttps://example.com/a\n"));
}

TEST(PasteDropTest, PasteOfABrowserLinkOverASelectionStaysAsItWas)
{
    auto editor = editorWith(QStringLiteral("hello\n"));
    editor->selectRange({0, 5});
    auto *mime = new QMimeData;
    mime->setUrls({QUrl(QStringLiteral("https://example.com/a"))});
    QGuiApplication::clipboard()->setMimeData(mime);

    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_EQ(editor->noteText(), QStringLiteral("hello\n")); // main inserted nothing for a remote URL list
}

TEST(PasteDropTest, DropOfAnOutsideFileImportsAndInsertsTheEmbedAtTheDropPoint)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7}); // the start of "below"
    ASSERT_FALSE(point.isNull());

    QString gotPath;
    editor->setFileImporter([&](const QString &path) {
        gotPath = path;
        return QStringLiteral("![[photo.png]]");
    });

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/outside/photo.png"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(gotPath, QStringLiteral("/outside/photo.png"));
    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n![[photo.png]]below\n"));
}

TEST(PasteDropTest, DropOfAnInVaultNoteInsertsWhateverTheImporterReturns)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    ASSERT_FALSE(point.isNull());

    editor->setFileImporter([](const QString &) { return QStringLiteral("[[Note]]"); });

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/vault/Note.md"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n[[Note]]below\n"));
}

TEST(PasteDropTest, DropWithNoImporterInsertsThePathAsText)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    ASSERT_FALSE(point.isNull());

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/outside/photo.png"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n/outside/photo.pngbelow\n"));
}

TEST(PasteDropTest, DropOfABrowserLinkInsertsAMarkdownLinkTitledByTheMozUrl)
{
    auto editor = editorWith(QStringLiteral("above\nbelow\n"));
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    ASSERT_FALSE(point.isNull());

    bool imported = false;
    editor->setFileImporter([&](const QString &) {
        imported = true;
        return QString();
    });
    // Firefox: text/x-moz-url is "URL\ntitle" in UTF-16.
    const QString pair = QStringLiteral("https://example.com/page\nExample [page]");
    QByteArray moz(reinterpret_cast<const char *>(pair.utf16()), pair.size() * 2);
    QMimeData mime;
    mime.setUrls({QUrl(QStringLiteral("https://example.com/page"))});
    mime.setData(QStringLiteral("text/x-moz-url"), moz);
    mime.setText(QStringLiteral("https://example.com/page"));
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_FALSE(imported);
    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n[Example \\[page\\]](https://example.com/page)below\n"));
}

TEST(PasteDropTest, DropOfABrowserLinkFallsBackToPlainTextThenTheHost)
{
    {
        auto editor = editorWith(QStringLiteral("above\nbelow\n"));
        editor->show();
        const QPoint point = clickInto(*editor, {5, 7});
        QMimeData mime; // Chrome: the page title as text, beside the URL
        mime.setUrls({QUrl(QStringLiteral("https://example.com/a"))});
        mime.setText(QStringLiteral("A title"));
        ASSERT_TRUE(dropOnto(*editor, point, mime));
        EXPECT_EQ(editor->noteText(), QStringLiteral("above\n[A title](https://example.com/a)below\n"));
    }
    {
        auto editor = editorWith(QStringLiteral("above\nbelow\n"));
        editor->show();
        const QPoint point = clickInto(*editor, {5, 7});
        QMimeData mime; // the text is the URL itself: nothing better than the host
        mime.setUrls({QUrl(QStringLiteral("https://example.com/a"))});
        mime.setText(QStringLiteral("https://example.com/a"));
        ASSERT_TRUE(dropOnto(*editor, point, mime));
        EXPECT_EQ(editor->noteText(), QStringLiteral("above\n[example.com](https://example.com/a)below\n"));
    }
}

TEST(PasteDropTest, DropOfAScriptOrDataLinkIsPlainTextNeverALink)
{
    for (const QString &raw : {QStringLiteral("javascript:alert(1)"), QStringLiteral("data:text/html;base64,PGI+")}) {
        auto editor = editorWith(QStringLiteral("above\nbelow\n"));
        editor->show();
        const QPoint point = clickInto(*editor, {5, 7});
        bool imported = false;
        editor->setFileImporter([&](const QString &) {
            imported = true;
            return QString();
        });
        QMimeData mime;
        mime.setUrls({QUrl(raw)});
        ASSERT_TRUE(dropOnto(*editor, point, mime));
        EXPECT_FALSE(imported);
        EXPECT_EQ(editor->noteText(), QStringLiteral("above\n") + raw + QStringLiteral("below\n"));
    }
}

TEST(PasteDropTest, ADroppedNonLinkUrlOverTwoThousandCharactersIsTextAndSaysSo)
{
    auto editor = editorWith(QStringLiteral("above\nbelow\n"));
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    QSignalSpy notice(editor.get(), &MarkdownEditor::noticeRaised);
    const QString raw = QStringLiteral("data:text/plain,") + QString(3000, QLatin1Char('a'));
    QMimeData mime;
    mime.setUrls({QUrl(raw)});
    ASSERT_TRUE(dropOnto(*editor, point, mime));
    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n") + raw + QStringLiteral("below\n"));
    EXPECT_EQ(notice.count(), 1);
}

TEST(PasteDropTest, DropOfAMailLinkTitlesItByTheAddressNotTheWholeUrl)
{
    auto editor = editorWith(QStringLiteral("above\nbelow\n"));
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    QMimeData mime;
    mime.setUrls({QUrl(QStringLiteral("mailto:a@example.com"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));
    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n[a@example.com](mailto:a@example.com)below\n"));
}

TEST(PasteDropTest, DropOfALocalFileAndABrowserLinkKeepsOrderAndImportsOnlyTheFile)
{
    auto editor = editorWith(QStringLiteral("above\nbelow\n"));
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    QStringList imported;
    editor->setFileImporter([&](const QString &path) {
        imported << path;
        return QStringLiteral("![[photo.png]]");
    });
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/outside/photo.png")), QUrl(QStringLiteral("https://example.com/a"))});
    ASSERT_TRUE(dropOnto(*editor, point, mime));
    EXPECT_EQ(imported, QStringList{QStringLiteral("/outside/photo.png")});
    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n![[photo.png]]\n[example.com](https://example.com/a)below\n"));
}

TEST(PasteDropTest, ABrowserTitleWithBackslashAndAMozUrlWithBomAndNulStillMakeAWorkingLink)
{
    auto editor = editorWith(QStringLiteral("above\nbelow\n"));
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    const QString pair = QStringLiteral("\uFEFFhttps://example.com/p\nC:\\dir\\");
    QByteArray moz(reinterpret_cast<const char *>(pair.utf16()), pair.size() * 2);
    moz.append(QByteArray(2, '\0')); // NUL-terminated
    QMimeData mime;
    mime.setUrls({QUrl(QStringLiteral("https://example.com/p"))});
    mime.setData(QStringLiteral("text/x-moz-url"), moz);
    ASSERT_TRUE(dropOnto(*editor, point, mime));
    EXPECT_EQ(editor->noteText(), QStringLiteral("above\n[C:\\\\dir\\\\](https://example.com/p)below\n"));
}

TEST(PasteDropTest, DroppedTextIsInsertedAtThePoint)
{
    const QString note = QStringLiteral("above\nbelow\n");
    auto editor = editorWith(note);
    editor->show();
    const QPoint point = clickInto(*editor, {5, 7});
    ASSERT_FALSE(point.isNull());

    QMimeData mime;
    mime.setText(QStringLiteral("dropped"));
    ASSERT_TRUE(dropOnto(*editor, point, mime));

    EXPECT_EQ(editor->noteText(), QStringLiteral("above\ndroppedbelow\n"));
}

TEST(PasteDropTest, PasteIntoATableCellStaysSafe)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    ASSERT_FALSE(clickInto(*editor, {apple, apple + 5}).isNull());
    editor->setCursorOffset(apple + 5);

    QGuiApplication::clipboard()->setText(QStringLiteral("a|b\nc"));
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);

    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| applea\\|b c | 3 |")))
        << editor->noteText().toStdString();
}

// --- pasted rich text becomes Markdown (PasteHTML.cpp) -----------------------

namespace {

/// Puts `html` (when not null) and `plain` on the clipboard, pastes with
/// Ctrl+V (Ctrl+Shift+V when `plain`) into `editor`, and returns the note.
void putOnClipboard(const QString &html, const QString &plain)
{
    auto *mime = new QMimeData;
    if (!html.isNull())
        mime->setHtml(html);
    mime->setText(plain);
    QGuiApplication::clipboard()->setMimeData(mime);
}

QString pasteRich(const QString &html, const QString &plain, bool shift = false)
{
    auto editor = editorWith(QString());
    putOnClipboard(html, plain);
    QTest::keyClick(editor.get(), Qt::Key_V,
                    shift ? Qt::ControlModifier | Qt::ShiftModifier : Qt::ControlModifier);
    return editor->noteText();
}

/// What Chrome puts on the clipboard around a selection.
QString chromeHTML(const QString &fragment)
{
    return QStringLiteral("<html><body><!--StartFragment-->") + fragment
         + QStringLiteral("<!--EndFragment--></body></html>");
}

} // namespace

TEST(PasteHTMLTest, InlineFormattingBecomesMarkdown)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral(
        "<p style=\"color:rgb(0,0,0);\">Some <strong>bold</strong>, <em>italic</em>, <del>gone</del> and "
        "<code>inline code</code> with a <a href=\"https://example.com/a_b\">link_text</a>.</p>")),
        QStringLiteral("Some bold, italic, gone and inline code with a link_text."));
    EXPECT_EQ(md, QStringLiteral("Some **bold**, *italic*, ~~gone~~ and `inline code` with a "
                                 "[link_text](https://example.com/a_b)."))
        << md.toStdString();
}

TEST(PasteHTMLTest, HeadingsKeepTheirLevel)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral("<h2>Title here</h2><h4>Deeper <em>one</em></h4>")),
                                 QStringLiteral("Title here\nDeeper one"));
    EXPECT_EQ(md, QStringLiteral("## Title here\n\n#### Deeper one"));
}

TEST(PasteHTMLTest, NestedBulletAndOrderedLists)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral(
        "<ul><li>one</li><li>two<ul><li>nested a</li><li>nested b</li></ul></li></ul>")),
        QStringLiteral("one\ntwo\nnested a\nnested b"));
    EXPECT_EQ(md, QStringLiteral("- one\n- two\n  - nested a\n  - nested b"));

    const QString ordered = pasteRich(chromeHTML(QStringLiteral(
        "<ol><li>first</li><li>second<ol><li>inner</li></ol></li></ol>")),
        QStringLiteral("first\nsecond\ninner"));
    EXPECT_EQ(ordered, QStringLiteral("1. first\n2. second\n    1. inner")) << ordered.toStdString();
}

TEST(PasteHTMLTest, BlockquoteBecomesAQuote)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral("<blockquote>quoted text</blockquote>")),
                                 QStringLiteral("quoted text"));
    EXPECT_EQ(md, QStringLiteral("> quoted text"));
}

TEST(PasteHTMLTest, PreBecomesAFencedBlockKeepingEveryLine)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral(
        "<pre>line1\n  line2\n    line3</pre><p>after <b>it</b></p>")),
        QStringLiteral("line1\n  line2\n    line3\nafter it"));
    EXPECT_EQ(md, QStringLiteral("```\nline1\n  line2\n    line3\n```\n\nafter **it**"));

    const QString lang = pasteRich(chromeHTML(QStringLiteral(
        "<pre><code class=\"language-js\">const a = 1;\nconst b = \"x\" &amp;&amp; y;</code></pre>")),
        QStringLiteral("const a = 1;\nconst b = \"x\" && y;"));
    EXPECT_EQ(lang, QStringLiteral("```js\nconst a = 1;\nconst b = \"x\" && y;\n```"));
}

TEST(PasteHTMLTest, TableBecomesAPipeTable)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral(
        "<table><thead><tr><th>Name</th><th>Qty</th></tr></thead><tbody>"
        "<tr><td>apple</td><td>3</td></tr><tr><td>pear</td><td>4</td></tr></tbody></table>")),
        QStringLiteral("Name\tQty\napple\t3\npear\t4"));
    EXPECT_EQ(md, QStringLiteral("|Name |Qty|\n|-----|---|\n|apple|3  |\n|pear |4  |")) << md.toStdString();
}

TEST(PasteHTMLTest, RemoteImagesAreLinksAndDataImagesAreDropped)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral(
        "<p>see <img src=\"https://example.com/i.png\"> and <img src=\"data:image/png;base64,AAAA\"> "
        "<img src=\"file:///tmp/clip.png\"><img src=\"cid:image001@x\"> there <b>now</b></p>")),
        QStringLiteral("see  and  there now"));
    EXPECT_TRUE(md.contains(QStringLiteral("![](https://example.com/i.png)"))) << md.toStdString();
    EXPECT_FALSE(md.contains(QStringLiteral("base64"))) << md.toStdString();
    EXPECT_FALSE(md.contains(QStringLiteral("file:"))) << md.toStdString();
    EXPECT_FALSE(md.contains(QStringLiteral("cid:"))) << md.toStdString();
}

TEST(PasteHTMLTest, GoogleDocsBoldSpansAndItsWrapperBold)
{
    // Docs wraps everything in <b style="font-weight:normal"> and bolds with a 700 span.
    const QString html = QStringLiteral(
        "<meta charset='utf-8'><b style=\"font-weight:normal;\" id=\"docs-internal-guid-1\">"
        "<p dir=\"ltr\" style=\"line-height:1.38;margin-top:0pt;\"><span style=\"font-size:11pt;font-weight:700;"
        "white-space:pre-wrap;\">Bold words</span><span style=\"font-weight:400;\"> and </span>"
        "<span style=\"font-style:italic;\">italic words</span></p></b>");
    EXPECT_EQ(pasteRich(html, QStringLiteral("Bold words and italic words")),
              QStringLiteral("**Bold words** and *italic words*"));
}

TEST(PasteHTMLTest, WordNonBreakingSpacesBecomeSpacesAndBulletParagraphsBecomeAList)
{
    const QString html = QStringLiteral(
        "<html xmlns:o=\"urn:schemas-microsoft-com:office:office\"><head><meta name=Generator content=\"Microsoft Word 15\">"
        "<style><!-- p.MsoNormal{margin:0cm;} --></style></head><body lang=EN-GB><!--StartFragment-->"
        "<h1><span style='mso-ascii-font-family:Calibri'>Heading</span></h1>"
        "<p class=MsoNormal>Plain&nbsp;para with <b>bold</b>&nbsp;and <i>it</i>.<o:p></o:p></p>"
        "<p class=MsoListParagraph style='text-indent:-18.0pt;mso-list:l0 level1 lfo1'>"
        "<![if !supportLists]><span>&middot;&nbsp;&nbsp;</span><![endif]>bullet one</p>"
        "<p class=MsoListParagraph style='text-indent:-18.0pt;mso-list:l0 level2 lfo1'>"
        "<![if !supportLists]><span>o&nbsp;&nbsp;</span><![endif]>sub bullet</p>"
        "<p class=MsoListParagraph style='text-indent:-18.0pt;mso-list:l0 level1 lfo1'>"
        "<![if !supportLists]><span>&middot;&nbsp;&nbsp;</span><![endif]>bullet two</p>"
        "<!--EndFragment--></body></html>");
    const QString md = pasteRich(html, QStringLiteral("Heading\nPlain para with bold and it.\nbullet one\nsub bullet\nbullet two"));
    EXPECT_EQ(md, QStringLiteral("# Heading\n\nPlain para with **bold** and *it*.\n\n"
                                 "- bullet one\n  - sub bullet\n- bullet two"));
    EXPECT_FALSE(md.contains(QChar(0x00A0)));
}

TEST(PasteHTMLTest, TextJustWrappedInASpanIsExactlyThePlainText)
{
    // A terminal or plain editor: a styled span around text full of Markdown
    // punctuation. Nothing may be escaped, reflowed or code-spanned.
    const QString plain = QStringLiteral("use *this* and_that [x] # not heading 1. also <tag>\n  indented line\n\nlast - one");
    const QString html = chromeHTML(QStringLiteral(
        "<span style=\"color: rgb(204, 204, 204); font-family: Menlo, monospace; white-space: pre;\">"
        "use *this* and_that [x] # not heading 1. also &lt;tag&gt;\n  indented line\n\nlast - one</span>"));
    EXPECT_EQ(pasteRich(html, plain), plain);

    const QString divs = QStringLiteral("<div style=\"font-family: monospace;\">$ ls -la<br>total 8<br>foo_bar *</div>");
    EXPECT_EQ(pasteRich(divs, QStringLiteral("$ ls -la\ntotal 8\nfoo_bar *")), QStringLiteral("$ ls -la\ntotal 8\nfoo_bar *"));
}

TEST(PasteHTMLTest, StrayEscapesAndBlankLinesAreTidied)
{
    const QString md = pasteRich(chromeHTML(QStringLiteral(
        "<p><b>Ref</b> [1] and 5 * 6</p><div><br></div><div><br></div><div><br></div><p>end&nbsp;</p>")),
        QStringLiteral("Ref [1] and 5 * 6\nend"));
    EXPECT_EQ(md, QStringLiteral("**Ref** [1] and 5 * 6\n\nend"));
}

TEST(PasteHTMLTest, WhitespaceInsideEmphasisMovesOut)
{
    EXPECT_EQ(pasteRich(chromeHTML(QStringLiteral("<p>a <b>bold&nbsp;</b>tail</p>")), QStringLiteral("a bold tail")),
              QStringLiteral("a **bold** tail"));
}

TEST(PasteHTMLTest, PasteInsideAFencedBlockIsPlain)
{
    const QString note = QStringLiteral("```\ncode\n```\n");
    auto editor = editorWith(note);
    editor->setCursorOffset(8); // after "code"
    putOnClipboard(chromeHTML(QStringLiteral("<p>some <b>bold</b> text</p>")), QStringLiteral("some bold text"));
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("```\ncodesome bold text\n```\n"));
}

TEST(PasteHTMLTest, CtrlShiftVPastesPlainText)
{
    const QString html = chromeHTML(QStringLiteral("<p>some <b>bold</b> text</p>"));
    EXPECT_EQ(pasteRich(html, QStringLiteral("some bold text"), true), QStringLiteral("some bold text"));
    EXPECT_EQ(pasteRich(html, QStringLiteral("some bold text")), QStringLiteral("some **bold** text"));
}

TEST(PasteHTMLTest, HTMLOverTheSizeCapFallsBackToPlainText)
{
    const QString filler(2 * 1024 * 1024 + 16, QLatin1Char('x'));
    const QString html = chromeHTML(QStringLiteral("<p>a <b>bold</b> ") + filler + QStringLiteral("</p>"));
    QElapsedTimer timer;
    timer.start();
    EXPECT_EQ(pasteRich(html, QStringLiteral("a bold fallback")), QStringLiteral("a bold fallback"));
    EXPECT_LT(timer.elapsed(), 10000);
}

TEST(PasteHTMLTest, DeeplyNestedHostileHTMLDoesNotHang)
{
    QString html = chromeHTML(QString());
    QString nest;
    for (int i = 0; i < 50000; ++i)
        nest += QStringLiteral("<b><i>");
    nest += QStringLiteral("deep");
    html = chromeHTML(nest);
    ASSERT_LT(html.size(), 2 * 1024 * 1024);
    QElapsedTimer timer;
    timer.start();
    const QString md = pasteRich(html, QStringLiteral("deep"));
    EXPECT_LT(timer.elapsed(), 10000);
    EXPECT_EQ(md, QStringLiteral("deep"));
}

TEST(PasteHTMLTest, PagesOfUnclosedOrTornTagsFallBackToPlainTextFast)
{
    // Each of these made a lazy regex or QTextDocument scan to the end once per tag.
    for (const QString &unit : {QStringLiteral("<pre>"), QStringLiteral("<style>"), QStringLiteral("<pre"),
                                QStringLiteral("<p class=\"mso-list:l0 level1\">"), QStringLiteral("<table><tr><td>")}) {
        QString html;
        while (html.size() < 600 * 1024)
            html += unit;
        QElapsedTimer timer;
        timer.start();
        EXPECT_EQ(pasteRich(chromeHTML(html), QStringLiteral("fallback")), QStringLiteral("fallback"))
            << unit.toStdString();
        EXPECT_LT(timer.elapsed(), 10000) << unit.toStdString();
    }
}

TEST(PasteHTMLTest, StarsInsideInlineCodeAreLeftAlone)
{
    EXPECT_EQ(pasteRich(chromeHTML(QStringLiteral("<p>call <code>f(**a, **b)</code> now <b>bold </b>x</p>")),
                        QStringLiteral("call f(**a, **b) now bold x")),
              QStringLiteral("call `f(**a, **b)` now **bold** x"));
    EXPECT_EQ(pasteRich(chromeHTML(QStringLiteral("<p>is <code>2 ** 3 ** 4</code> and <code>a ~~ b ~~ c</code> <i>it </i>z</p>")),
                        QStringLiteral("is 2 ** 3 ** 4 and a ~~ b ~~ c it z")),
              QStringLiteral("is `2 ** 3 ** 4` and `a ~~ b ~~ c` *it* z"));
}

TEST(PasteHTMLTest, ResourcesAreNeverLoadedWhileConverting)
{
    // A named pipe is "read" the moment anything opens it: a poller thread
    // that can open the write end without blocking (ENXIO until a reader
    // exists) sees every attempt to load it as a stylesheet or an image.
    const QString fifoPath = QDir::temp().filePath(QStringLiteral("notolith-paste-fifo-%1").arg(QCoreApplication::applicationPid()));
    QFile::remove(fifoPath);
    ASSERT_EQ(::mkfifo(fifoPath.toLocal8Bit().constData(), 0600), 0);
    std::atomic<bool> stop{false}, read{false};
    std::thread poller([&] {
        while (!stop.load()) {
            const int fd = ::open(fifoPath.toLocal8Bit().constData(), O_WRONLY | O_NONBLOCK);
            if (fd >= 0) {
                read = true;
                ::close(fd);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const QString url = QUrl::fromLocalFile(fifoPath).toString();
    const QString closedStyle = QStringLiteral("<style>@import url(%1);</style>").arg(url);
    const QString openStyle = QStringLiteral("<style>@import url(%1);").arg(url); // never closed
    const QString image = QStringLiteral("<img src=\"%1\">").arg(url);
    const QString link = QStringLiteral("<link rel=\"stylesheet\" href=\"%1\">").arg(url);
    for (const QString &page : {closedStyle + QStringLiteral("<p>a <b>b</b></p>") + image + link,
                                openStyle + QStringLiteral("<p>a <b>b</b></p>") + image + link,
                                QStringLiteral("<p>a <b>b</b></p>") + openStyle}) {
        const QString md = pasteRich(chromeHTML(page), QStringLiteral("a b"));
        EXPECT_TRUE(md == QStringLiteral("a b") || md.startsWith(QStringLiteral("a **b**"))) << md.toStdString();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    poller.join();
    QFile::remove(fifoPath);
    EXPECT_FALSE(read.load()) << "converting opened a local file the page named";
}

TEST(PasteHTMLTest, PasteIntoATableCellStaysCellSafe)
{
    auto editor = editorWith(kTableNote);
    editor->show();
    const qsizetype apple = kTableNote.indexOf(QStringLiteral("apple"));
    ASSERT_FALSE(clickInto(*editor, {apple, apple + 5}).isNull());
    editor->setCursorOffset(apple + 5);
    putOnClipboard(chromeHTML(QStringLiteral("<p><b>a|b</b><br>c</p>")), QStringLiteral("a|b\nc"));
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("| apple**a\\|b**")))
        << editor->noteText().toStdString();
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("\n| pear |"))) << editor->noteText().toStdString();
}

TEST(PasteHTMLTest, MarkdownCopiedFromTheEditorPastesAsItIs)
{
    auto editor = editorWith(QStringLiteral("**b** and *i* and 1. x_y_z [x]"));
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(editor.get(), Qt::Key_C, Qt::ControlModifier);
    editor->setCursorOffset(editor->noteText().size());
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("**b** and *i* and 1. x_y_z [x]**b** and *i* and 1. x_y_z [x]"));

    // A copied table-cell rectangle carries text/markdown beside rich text: the Markdown wins.
    auto *mime = new QMimeData;
    mime->setHtml(chromeHTML(QStringLiteral("<p><b>html</b></p>")));
    mime->setText(QStringLiteral("html"));
    mime->setData(QStringLiteral("text/markdown"), "| md |");
    QGuiApplication::clipboard()->setMimeData(mime);
    auto other = editorWith(QString());
    QTest::keyClick(other.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(other->noteText(), QStringLiteral("| md |"));
}

TEST(PasteHTMLTest, AnImageAloneStillGoesToTheAttachmentSaver)
{
    // Chrome's "copy image": an <img> in HTML beside the bitmap, no text.
    auto editor = editorWith(QString());
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::green);
    auto *mime = new QMimeData;
    mime->setHtml(QStringLiteral("<img src=\"https://example.com/i.png\">"));
    mime->setImageData(image);
    QGuiApplication::clipboard()->setMimeData(mime);
    bool saved = false;
    editor->setAttachmentSaver([&](const QString &name, const QByteArray &) {
        saved = true;
        return QStringLiteral("![[") + name + QStringLiteral("]]");
    });
    QTest::keyClick(editor.get(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_TRUE(saved);
    EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("![[Pasted image "))) << editor->noteText().toStdString();
}

TEST(TableCellTest, AShortRowGetsItsMissingCellsWhenReached)
{
    const QString note = QStringLiteral("| a | b | c |\n|---|---|---|\n| 1 |\n\nafter\n");
    {
        auto editor = editorWith(note);
        editor->show();
        editor->setCursorOffset(note.indexOf(QLatin1Char('1')) + 1);
        QTest::keyClick(editor.get(), Qt::Key_Tab); // into a cell the row lacks
        QTest::keyClicks(editor.get(), QStringLiteral("x"));
        EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("| a | b | c |\n|---|---|---|\n| 1 | x |  |\n")))
            << editor->noteText().toStdString();
        QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
        EXPECT_EQ(editor->noteText(), note); // typing, then the filling, each undone
    }
    // A click on a missing cell: the row is filled and the caret lands in it.
    bool filled = false;
    for (int y = 4; y < 200 && !filled; y += 6) {
        for (int x = 4; x < 880 && !filled; x += 40) {
            auto editor = editorWith(note);
            editor->setTableHandles(false); // the probe would press the add bars too
            editor->show();
            QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, QPoint(x, y));
            if (editor->noteText() == note)
                continue;
            filled = true;
            EXPECT_TRUE(editor->noteText().startsWith(QStringLiteral("| a | b | c |\n|---|---|---|\n| 1 |  |  |\n")))
                << editor->noteText().toStdString();
            const qsizetype caret = editor->cursorOffset();
            EXPECT_EQ(editor->noteText().at(caret - 1), QLatin1Char(' '));
            EXPECT_EQ(editor->noteText().at(caret), QLatin1Char(' ')); // inside a new "|  |"
        }
    }
    EXPECT_TRUE(filled) << "no click reached a missing cell";
}

TEST(TableCellTest, ASelectionAcrossCellsIsDrawnInThem)
{
    const QString note = QStringLiteral("| alpha | beta | gamma |\n|---|---|---|\n| one | two | three |\n");
    auto editor = editorWith(note);
    editor->show();
    const QImage before = editor->viewport()->grab().toImage();
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
    ASSERT_TRUE(editor->tableRenderedAt(note.indexOf(QStringLiteral("two"))));
    const QImage after = editor->viewport()->grab().toImage();
    EXPECT_NE(before, after) << "the selection left the drawn table unchanged";
}

TEST(FileEmbedTest, AnEncryptedNoteEmbedsAsACardNeverItsBytes)
{
    QTemporaryDir dir;
    const QString locked = dir.filePath(QStringLiteral("Secret.mdlock"));
    {
        QFile file(locked);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("NAPCLOCK\x01\x01 text-looking ciphertext with no zero byte");
    }
    auto editor = editorWith(QStringLiteral("![[Secret]]\n\nafter\n"));
    editor->setLinkResolver([&](const QString &) { return locked; });
    editor->setCursorOffset(editor->noteText().size()); // off the embed's line
    editor->show();
    QTest::qWait(50);
    EXPECT_FALSE(editor->panelTextAt(0).contains(QStringLiteral("NAPCLOCK")));
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("ciphertext")));
}


#ifdef NOTEAPPC_EXPECT_PDF
TEST(PdfPageTest, EditingAnEmbedsLineShowsItsSourceNotThePdf)
{
    QTemporaryDir dir;
    const QString pdf = makeTestPdf(dir.path(), 3);
    const QString note = QStringLiteral("# Lecture\n\n![[doc.pdf]]\n\nSumming the integers\n");
    auto editor = editorWith(note);
    editor->setLinkResolver([&](const QString &) { return pdf; });
    editor->setCursorOffset(note.size());
    editor->show();
    const qsizetype embed = note.indexOf(QStringLiteral("![["));
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->pdfEmbedInfoAt(embed).isPdfEmbed; }, 3000));
    const qreal rendered = editor->documentHeight();

    // The caret on the embed's line: its source, and nothing of the PDF.
    editor->enterBlockAt(embed + 3);
    QTest::qWait(50);
    EXPECT_FALSE(editor->pdfEmbedInfoAt(embed).isPdfEmbed);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("![[doc.pdf]]")));
    EXPECT_LT(editor->documentHeight(), rendered) << "the page's height went with it";

    // Off the line again: the PDF is back.
    editor->setCursorOffset(note.size());
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->pdfEmbedInfoAt(embed).isPdfEmbed; }, 3000));
}
#endif

#ifdef NOTEAPPC_EXPECT_PDF
TEST(PdfPageTest, AClickOnAnEmbeddedPdfOpensItsSource)
{
    QTemporaryDir dir;
    const QString pdf = makeTestPdf(dir.path(), 3);
    const QString note = QStringLiteral("# Lecture\n\n![[doc.pdf]]\n\nSumming the integers\n");
    const qsizetype embed = note.indexOf(QStringLiteral("![["));
    int opened = 0;
    int tried = 0;
    for (int y = 60; y < 700; y += 20) {
        auto editor = editorWith(note);
        editor->setLinkResolver([&](const QString &) { return pdf; });
        editor->setCursorOffset(note.size());
        editor->show();
        if (!QTest::qWaitFor([&] { return editor->pdfEmbedInfoAt(embed).isPdfEmbed; }, 3000))
            continue;
        const QRect page = editor->viewport()->rect();
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, QPoint(page.width() / 2, y));
        QTest::qWait(20);
        if (editor->cursorOffset() >= embed && editor->cursorOffset() <= embed + 11) {
            ++opened;
            EXPECT_TRUE(editor->displayText().contains(QStringLiteral("![[doc.pdf]]")));
        }
        ++tried;
    }
    EXPECT_GE(opened, 5) << "a click on the page itself opens the embed for editing";
}
#endif

TEST(NotebookBlockTest, TheRunControlSitsBesideTheLanguageNotOnIt)
{
    const QString note = QStringLiteral("```haskell\nsum [1..10]\n```\n\nafter\n");
    auto editor = editorWith(note);
    editor->setNotebookRunner([](const QString &, const QString &, bool, const MarkdownEditor::NotebookRunOptions &,
                                 const MarkdownEditor::NotebookDeliver &deliver) {
        QTimer::singleShot(0, [deliver] { deliver(MarkdownEditor::NotebookRunResult{}); });
    });
    editor->setCursorOffset(note.size());
    editor->show();
    QTest::qWait(50);
    const auto info = editor->notebookInfoAt(note.indexOf(QStringLiteral("sum")));
    ASSERT_TRUE(info.eligible);
    ASSERT_TRUE(info.labelRect.isValid());
    EXPECT_FALSE(info.runRect.intersects(info.labelRect)) << "the icon covers the language name";
}

// Ctrl+Space and Shift+Tab in a code block, with a fake kernel behind the
// introspector: the real one is exercised by test_notebook and test_window.
namespace {

struct FakeKernel {
    bool running = true;
    QStringList matches{QStringLiteral("value_alpha"), QStringLiteral("value_beta")};
    QString docs = QStringLiteral("alpha docs");
    int asked = 0;
    QString lastLanguage;
    QString lastCode;
    qsizetype lastCursor = -1;
};

void attach(MarkdownEditor &editor, FakeKernel &kernel)
{
    editor.setNotebookIntrospector([&kernel](const QString &language, const QString &code, qsizetype cursor,
                                             bool inspect, const MarkdownEditor::NotebookRunOptions &,
                                             const MarkdownEditor::NotebookIntrospectionDeliver &deliver) {
        if (!kernel.running)
            return false;
        ++kernel.asked;
        kernel.lastLanguage = language;
        kernel.lastCode = code;
        kernel.lastCursor = cursor;
        MarkdownEditor::NotebookIntrospection answer;
        if (inspect) {
            answer.text = kernel.docs;
        } else {
            answer.matches = kernel.matches;
            // "value_" is what is replaced: the token left of the caret.
            answer.from = cursor - 6;
            answer.to = cursor;
        }
        QTimer::singleShot(0, [deliver, answer] { deliver(answer); });
        return true;
    });
}

} // namespace

TEST(NotebookCompleteTest, CtrlSpaceListsTheKernelsMatchesAndEnterTakesTheChosenOne)
{
    const QString note = QStringLiteral("```python\nvalue_alpha = 1\nvalue_\n```\n");
    auto editor = editorWith(note);
    FakeKernel kernel;
    attach(*editor, kernel);
    const qsizetype caret = note.indexOf(QStringLiteral("value_\n")) + 6;
    editor->setCursorOffset(caret);
    QTest::keyClick(editor.get(), Qt::Key_Space, Qt::ControlModifier);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->notebookCompletionRows().isEmpty(); }, 2000));
    EXPECT_EQ(editor->notebookCompletionRows(), kernel.matches);
    EXPECT_EQ(kernel.lastLanguage, QStringLiteral("python"));
    EXPECT_EQ(kernel.lastCode, QStringLiteral("value_alpha = 1\nvalue_")); // the listing, without its fences
    EXPECT_EQ(kernel.lastCursor, qsizetype(kernel.lastCode.size()));

    QTest::keyClick(editor.get(), Qt::Key_Down);
    QTest::keyClick(editor.get(), Qt::Key_Return);
    EXPECT_TRUE(editor->notebookCompletionRows().isEmpty());
    EXPECT_EQ(editor->noteText(), QStringLiteral("```python\nvalue_alpha = 1\nvalue_beta\n```\n"));
    EXPECT_EQ(editor->cursorOffset(), qsizetype(note.indexOf(QStringLiteral("value_\n")) + 10));
}

TEST(NotebookCompleteTest, EscapeAndTypingCloseTheListAndOneMatchIsInsertedAtOnce)
{
    const QString note = QStringLiteral("```python\nvalue_\n```\n");
    auto editor = editorWith(note);
    FakeKernel kernel;
    attach(*editor, kernel);
    editor->setCursorOffset(note.indexOf(QStringLiteral("value_")) + 6);

    QTest::keyClick(editor.get(), Qt::Key_Space, Qt::ControlModifier);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->notebookCompletionRows().isEmpty(); }, 2000));
    QTest::keyClick(editor.get(), Qt::Key_Escape);
    EXPECT_TRUE(editor->notebookCompletionRows().isEmpty());
    EXPECT_EQ(editor->noteText(), note);

    QTest::keyClick(editor.get(), Qt::Key_Space, Qt::ControlModifier);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->notebookCompletionRows().isEmpty(); }, 2000));
    QTest::keyClicks(editor.get(), QStringLiteral("x")); // typing carries on in the note
    EXPECT_TRUE(editor->notebookCompletionRows().isEmpty());
    EXPECT_TRUE(editor->noteText().contains(QStringLiteral("value_x")));

    kernel.matches = {QStringLiteral("value_only")};
    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("value_x")) + 6);
    QTest::keyClick(editor.get(), Qt::Key_Space, Qt::ControlModifier);
    ASSERT_TRUE(QTest::qWaitFor([&] { return editor->noteText().contains(QStringLiteral("value_onlyx")); }, 2000));
    EXPECT_TRUE(editor->notebookCompletionRows().isEmpty()) << "one way to finish is not a choice";
}

TEST(NotebookCompleteTest, AReplyForAnEarlierCaretIsDropped)
{
    const QString note = QStringLiteral("```python\nvalue_\n```\n");
    auto editor = editorWith(note);
    FakeKernel kernel;
    attach(*editor, kernel);
    editor->setCursorOffset(note.indexOf(QStringLiteral("value_")) + 6);
    QTest::keyClick(editor.get(), Qt::Key_Space, Qt::ControlModifier);
    QTest::keyClicks(editor.get(), QStringLiteral("a")); // before the reply is handled
    QTest::qWait(100);
    EXPECT_TRUE(editor->notebookCompletionRows().isEmpty());
    EXPECT_EQ(editor->noteText(), QStringLiteral("```python\nvalue_a\n```\n"));
}

TEST(NotebookCompleteTest, ShiftTabShowsTheKernelsDocumentationUntilEscape)
{
    const QString note = QStringLiteral("```python\nlen\n```\n");
    auto editor = editorWith(note);
    FakeKernel kernel;
    attach(*editor, kernel);
    editor->setCursorOffset(note.indexOf(QStringLiteral("len")) + 3);
    QTest::keyClick(editor.get(), Qt::Key_Backtab, Qt::ShiftModifier);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->notebookInspectText().isEmpty(); }, 2000));
    EXPECT_EQ(editor->notebookInspectText(), QStringLiteral("alpha docs"));
    QTest::keyClick(editor.get(), Qt::Key_Escape);
    EXPECT_TRUE(editor->notebookInspectText().isEmpty());
    EXPECT_EQ(editor->noteText(), note) << "nothing was typed";
}

TEST(NotebookCompleteTest, WithoutAKernelTheKeysAreOthersAndNothingIsAsked)
{
    // Shift+Tab outdents a list item as it always did; Ctrl+Space types nothing.
    const QString note = QStringLiteral("- a\n  - b\n\n```python\nvalue_\n```\n");
    auto editor = editorWith(note);
    FakeKernel kernel;
    kernel.running = false;
    attach(*editor, kernel);
    editor->setCursorOffset(note.indexOf(QStringLiteral("b")) + 1);
    QTest::keyClick(editor.get(), Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("- a\n- b\n\n```python\nvalue_\n```\n"));

    editor->setCursorOffset(editor->noteText().indexOf(QStringLiteral("value_")) + 6);
    QTest::keyClick(editor.get(), Qt::Key_Space, Qt::ControlModifier);
    QTest::keyClick(editor.get(), Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(50);
    EXPECT_TRUE(editor->notebookCompletionRows().isEmpty());
    EXPECT_TRUE(editor->notebookInspectText().isEmpty());
    EXPECT_EQ(kernel.asked, 0);

    // Outside a code block the introspector is not asked at all.
    kernel.running = true;
    editor->setCursorOffset(2);
    QTest::keyClick(editor.get(), Qt::Key_Space, Qt::ControlModifier);
    EXPECT_EQ(kernel.asked, 0);
}

// --- the `</>` button over rendered blocks (HoverEdit.cpp) ---------------------

namespace {

const QString kHoverNote = QStringLiteral(
    "intro\n\n> [!note] Title\n> body text\n\nmiddle\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n"
    "```cpp\nint x;\n```\n\n```query\nfoo\n```\n\nend\n");

QPoint centreOf(const QRectF &rect)
{
    return rect.center().toPoint();
}

/// A pointer move over the viewport. QTest::mouseMove needs an exposed
/// window; the editor's own handler is what is under test.
void hoverAt(MarkdownEditor &editor, const QPoint &point)
{
    QMouseEvent move(QEvent::MouseMove, QPointF(point), editor.viewport()->mapToGlobal(QPointF(point)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor.viewport(), &move);
}

/// Records the region of every paint the viewport gets.
class PaintSpy : public QObject {
public:
    QList<QRect> regions;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            regions.append(static_cast<QPaintEvent *>(event)->region().boundingRect());
        return false;
    }
};

} // namespace

TEST(HoverEditTest, ButtonAppearsOnlyWhileHovered)
{
    auto editor = editorWith(kHoverNote);
    const qsizetype callout = kHoverNote.indexOf(QStringLiteral("body"));
    EXPECT_FALSE(editor->editButtonRect().isValid());

    hoverAt(*editor, centreOf(editor->blockPanelAt(callout)));
    ASSERT_TRUE(editor->editButtonRect().isValid());
    EXPECT_TRUE(editor->blockPanelAt(callout).contains(editor->editButtonRect()));

    hoverAt(*editor, centreOf(editor->blockPanelAt(2)));
    EXPECT_FALSE(editor->editButtonRect().isValid()) << "a paragraph has none";

    // A table gets one too; a plain code block does not.
    hoverAt(*editor, centreOf(editor->blockPanelAt(kHoverNote.indexOf(QStringLiteral("| 1")))));
    EXPECT_TRUE(editor->editButtonRect().isValid());
    hoverAt(*editor, centreOf(editor->blockPanelAt(kHoverNote.indexOf(QStringLiteral("int x")))));
    EXPECT_FALSE(editor->editButtonRect().isValid());

    // Leaving the viewport takes it away.
    hoverAt(*editor, centreOf(editor->blockPanelAt(callout)));
    ASSERT_TRUE(editor->editButtonRect().isValid());
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(editor.get(), &leave);
    EXPECT_FALSE(editor->editButtonRect().isValid());
}

TEST(HoverEditTest, ClickSelectsTheWholeSource)
{
    auto editor = editorWith(kHoverNote);
    const QString source = QStringLiteral("> [!note] Title\n> body text");
    const qsizetype start = kHoverNote.indexOf(source);
    ASSERT_GE(start, 0);
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("[!note]"))) << "rendered";

    hoverAt(*editor, centreOf(editor->blockPanelAt(start)));
    ASSERT_TRUE(editor->editButtonRect().isValid());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, centreOf(editor->editButtonRect()));

    EXPECT_EQ(editor->selectedText(), source);
    EXPECT_EQ(editor->cursorOffset(), start) << "caret at the start";
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("[!note]"))) << "source shows";
    EXPECT_EQ(editor->noteText(), kHoverNote);

    // Typing replaces nothing by surprise: moving the caret hides the source again.
    QTest::keyClick(editor.get(), Qt::Key_Right);
    EXPECT_TRUE(editor->selectedText().isEmpty());
}

TEST(HoverEditTest, ABodyClickStillPutsTheCaretThere)
{
    auto editor = editorWith(kHoverNote);
    const QRectF panel = editor->blockPanelAt(kHoverNote.indexOf(QStringLiteral("body")));
    hoverAt(*editor, centreOf(panel));
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, centreOf(panel));
    EXPECT_TRUE(editor->selectedText().isEmpty());
}

TEST(HoverEditTest, QueryBlockButtonReachesSource)
{
    auto editor = editorWith(kHoverNote);
    FakeQueryRunner fake;
    editor->setQueryRunner(std::ref(fake));
    ASSERT_EQ(fake.calls.size(), 1);
    fake.calls.first().deliver({makeQueryResult(QStringLiteral("One.md"), 1, QStringLiteral("foo"), 0)});
    pumpQueuedDelivery();
    const qsizetype query = kHoverNote.indexOf(QStringLiteral("foo"));
    ASSERT_FALSE(editor->queryRowsAt(query).isEmpty());

    // Today a click in the body of the block is swallowed.
    hoverAt(*editor, centreOf(editor->blockPanelAt(query)));
    ASSERT_TRUE(editor->editButtonRect().isValid());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, centreOf(editor->editButtonRect()));
    EXPECT_EQ(editor->selectedText(), QStringLiteral("```query\nfoo\n```"));
    EXPECT_EQ(editor->cursorOffset(), kHoverNote.indexOf(QStringLiteral("```query")));
}

TEST(HoverEditTest, HoverRepaintsOnlyTheBlock)
{
    auto editor = editorWith(kHoverNote);
    editor->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(editor.get()));
    QCoreApplication::processEvents();

    const QRect callout = editor->blockPanelAt(kHoverNote.indexOf(QStringLiteral("body"))).toAlignedRect();
    const QRect table = editor->blockPanelAt(kHoverNote.indexOf(QStringLiteral("| 1"))).toAlignedRect();
    PaintSpy spy;
    editor->viewport()->installEventFilter(&spy);

    hoverAt(*editor, centreOf(callout));
    QCoreApplication::processEvents();
    hoverAt(*editor, centreOf(table));
    QCoreApplication::processEvents();
    ASSERT_FALSE(spy.regions.isEmpty()) << "something repainted";
    const QRect allowed = callout.united(table);
    for (const QRect &region : spy.regions)
        EXPECT_TRUE(allowed.contains(region)) << "repainted outside the two panels";
    EXPECT_LT(allowed.height(), editor->viewport()->height());
}

TEST(HoverEditTest, OffMeansNoTracking)
{
    auto editor = editorWith(kHoverNote);
    editor->setHoverEditButton(false);
    hoverAt(*editor, centreOf(editor->blockPanelAt(kHoverNote.indexOf(QStringLiteral("body")))));
    EXPECT_EQ(editor->hoveredBlock(), -1);
    EXPECT_FALSE(editor->editButtonRect().isValid());

    editor->setHoverEditButton(true);
    hoverAt(*editor, centreOf(editor->blockPanelAt(kHoverNote.indexOf(QStringLiteral("body")))));
    EXPECT_GE(editor->hoveredBlock(), 0);
}

TEST(HoverEditTest, CommandEditsCaretBlock)
{
    auto editor = editorWith(kHoverNote);
    const qsizetype body = kHoverNote.indexOf(QStringLiteral("body"));
    editor->setCursorOffset(body);
    ASSERT_TRUE(editor->runCommand(QStringLiteral("noteAppC:edit-block-source")));
    EXPECT_EQ(editor->selectedText(), QStringLiteral("> [!note] Title\n> body text"));
    EXPECT_EQ(editor->cursorOffset(), kHoverNote.indexOf(QStringLiteral("> [!note]")));
}

TEST(HoverEditTest, EveryRenderedKindSelectsItsWholeSource)
{
    const QStringList sources = {
        QStringLiteral("$$\nx^2 + y^2\n$$"),
        QStringLiteral("<div>\n<b>hello</b>\n</div>"),
        QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |"),
        QStringLiteral("> [!tip] Outer\n> text\n> > [!bug] Inner\n> > more"),
    };
    int clicked = 0;
    for (const QString &source : sources) {
        const QString note = QStringLiteral("before\n\n") + source + QStringLiteral("\n\nafter\n");
        auto editor = editorWith(note);
        const qsizetype start = note.indexOf(source);
        hoverAt(*editor, centreOf(editor->blockPanelAt(start + source.size() / 2)));
        if (!editor->editButtonRect().isValid()) {
            // A formula or HTML the build cannot draw stays source: nothing to edit.
            EXPECT_TRUE(source.startsWith(QLatin1Char('$')) || source.startsWith(QLatin1Char('<')))
                << qPrintable(source);
            continue;
        }
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, centreOf(editor->editButtonRect()));
        EXPECT_EQ(editor->selectedText(), source);
        EXPECT_EQ(editor->cursorOffset(), start);
        ++clicked;
    }
    std::printf("[ INFO     ] %d of %d kinds had a button\n", clicked, int(sources.size()));
    EXPECT_GE(clicked, 2); // the table and the callout, whatever the build draws
}

// --- fences, callout menu, nested embeds, block refs -------------

TEST(CodeFenceRevealTest, TheCaretAnywhereInTheListingShowsBothFences)
{
    // Obsidian: the caret anywhere from the opening fence to the closing one
    // shows both fence lines (and the language tag); away from it they hide.
    const QString note = QStringLiteral("before\n\n```python\nprint(1)\nprint(2)\n```\n\nafter\n");
    auto editor = editorWith(note);
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("```")));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("python")));
    for (const QString &where : {QStringLiteral("print(1)"), QStringLiteral("print(2)")}) {
        editor->enterBlockAt(note.indexOf(where) + 2);
        EXPECT_TRUE(editor->displayText().contains(QLatin1String("```python\nprint(1)\nprint(2)\n```")))
            << qPrintable(where) << ": " << qPrintable(editor->displayText());
    }
    editor->enterBlockAt(note.indexOf(QLatin1String("after")));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("```")));
    editor->enterBlockAt(note.indexOf(QLatin1String("before")));
    EXPECT_FALSE(editor->displayText().contains(QLatin1String("```")));
    // On the fence line itself, as before.
    editor->enterBlockAt(note.indexOf(QLatin1String("```python")) + 3);
    EXPECT_TRUE(editor->displayText().contains(QLatin1String("```python")));
}

namespace {
std::unique_ptr<MarkdownEditor> editorWithCallout(const QString &callout)
{
    return editorWith(QStringLiteral("before\n\n") + callout + QStringLiteral("\n\nafter\n"));
}
} // namespace

TEST(CalloutMenuTest, ChangeTypeRewritesTheMarkerInOneUndoStep)
{
    const QString callout = QStringLiteral("> [!note|bar=#FF0000ff]+ Title\n> body\n> > [!tip] inner");
    auto editor = editorWithCallout(callout);
    const qsizetype at = editor->noteText().indexOf(QLatin1String("body"));
    EXPECT_EQ(editor->calloutSourceAt(at), callout);
    ASSERT_TRUE(editor->changeCalloutType(at, QStringLiteral("warning")));
    EXPECT_EQ(editor->calloutSourceAt(at),
              QStringLiteral("> [!warning|bar=#FF0000ff]+ Title\n> body\n> > [!tip] inner"));
    EXPECT_FALSE(editor->changeCalloutType(at, QStringLiteral("warning"))) << "already that type";
    EXPECT_FALSE(editor->changeCalloutType(at, QStringLiteral("two words")));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("before\n\n") + callout + QStringLiteral("\n\nafter\n"));
}

TEST(CalloutMenuTest, ChangeTypeWorksOnAColonFence)
{
    for (const QString &fence : {QStringLiteral("::: note Title\nbody\n:::"),
                                 QStringLiteral("::: {#id .note title=\"T\"}\nbody\n:::")}) {
        auto editor = editorWithCallout(fence);
        const qsizetype at = editor->noteText().indexOf(QLatin1String("body"));
        ASSERT_TRUE(editor->changeCalloutType(at, QStringLiteral("bug"))) << qPrintable(fence);
        QString expected = fence;
        expected.replace(QLatin1String("note"), QLatin1String("bug"));
        EXPECT_EQ(editor->calloutSourceAt(at), expected);
    }
}

TEST(CalloutMenuTest, RemoveKeepsTheContentAndAnExplicitTitle)
{
    auto quote = editorWithCallout(QStringLiteral("> [!note]+ My title\n> line one\n>\n> - item\n> > nested"));
    const qsizetype at = quote->noteText().indexOf(QLatin1String("line"));
    ASSERT_TRUE(quote->removeCallout(at));
    EXPECT_EQ(quote->noteText(), QStringLiteral("before\n\nMy title\n\nline one\n\n- item\n> nested\n\nafter\n"))
        << qPrintable(quote->noteText());
    QTest::keyClick(quote.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_TRUE(quote->noteText().contains(QLatin1String("> [!note]+ My title")));

    auto untitled = editorWithCallout(QStringLiteral("> [!tip]\n> only body"));
    ASSERT_TRUE(untitled->removeCallout(untitled->noteText().indexOf(QLatin1String("only"))));
    EXPECT_EQ(untitled->noteText(), QStringLiteral("before\n\nonly body\n\nafter\n")) << qPrintable(untitled->noteText());

    auto colons = editorWithCallout(QStringLiteral("::: warning\ntext\n:::"));
    ASSERT_TRUE(colons->removeCallout(colons->noteText().indexOf(QLatin1String("text"))));
    EXPECT_EQ(colons->noteText(), QStringLiteral("before\n\ntext\n\nafter\n")) << qPrintable(colons->noteText());

    auto none = editorWith(QStringLiteral("plain\n"));
    EXPECT_FALSE(none->removeCallout(1));
    EXPECT_FALSE(none->changeCalloutType(1, QStringLiteral("note")));
    EXPECT_TRUE(none->calloutSourceAt(1).isEmpty());
}

TEST(CalloutMenuTest, KnownTypesStartWithObsidiansAndAddTheIni)
{
    const QStringList types = CalloutStyles::load(QStringLiteral("/nonexistent/callouts.ini")).knownTypes();
    ASSERT_EQ(types.size(), 13);
    EXPECT_EQ(types.first(), QStringLiteral("note"));
    EXPECT_TRUE(types.contains(QStringLiteral("quote")));
}

TEST(NoteEmbedTest, ANoteEmbeddedInRunningTextHoldsTheSameContentAsOnOwnLine)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("Target.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("# Overview\n\nIntro.\n\n## Details\n\nA paragraph worth linking to. ^anchor\n\n"
                   "A second paragraph after the anchored one.\n");
    }
    const auto resolver = [&](const QString &) { return path; };
    const auto drawn = [&](const QString &note, const QString &embed, QSizeF &size) {
        auto editor = editorWith(note);
        editor->setLinkResolver(resolver);
        editor->setCursorOffset(editor->noteText().size()); // off the embed
        if (embed.isEmpty())
            QTest::qWaitFor([&] { return !editor->embedImageSizeAt(0).isEmpty(); }, 3000);
        else
            QTest::qWaitFor([&] { return editor->embedCellRectAt(0, 0).height() > 44; }, 3000);
        size = embed.isEmpty() ? QSizeF(editor->embedImageSizeAt(0)) : editor->embedCellRectAt(0, 0).size();
        return editor;
    };
    for (const QString &target : {QStringLiteral("Target#Details"), QStringLiteral("Target#^anchor"),
                                  QStringLiteral("Target")}) {
        QSizeF own;
        QSizeF inlineSize;
        drawn(QStringLiteral("![[%1]]\n\nafter\n").arg(target), QString(), own);
        auto editor = drawn(QStringLiteral("here is the target ![[%1]] embedded between clauses\n").arg(target),
                            target, inlineSize);
        EXPECT_GT(own.height(), 20) << qPrintable(target);
        EXPECT_EQ(inlineSize, own) << qPrintable(target) << ": the same drawing as the embed on its own line";
        // The paragraph's words stay in place around the embed's block.
        EXPECT_TRUE(editor->displayText().contains(QStringLiteral("here is the target")));
        EXPECT_TRUE(editor->displayText().contains(QStringLiteral("embedded between clauses")));
        EXPECT_GE(editor->embedCellRectAt(0, 0).top(), 0);
    }

    // Reading view lays the paragraph out the same way.
    auto reading = editorWith(QStringLiteral("here is the target ![[Target#Details]] embedded\n"));
    reading->setMode(MarkdownEditor::Mode::Reading);
    reading->setLinkResolver(resolver);
    EXPECT_TRUE(QTest::qWaitFor([&] { return reading->embedCellRectAt(0, 0).height() > 44; }, 3000));
}

TEST(NoteEmbedTest, ANoteEmbeddedInAnEmbeddedNoteIsDrawnFormattedToo)
{
    QTemporaryDir dir;
    const auto write = [&](const QString &name, const QByteArray &body) {
        QFile file(dir.filePath(name));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(body);
        return dir.filePath(name);
    };
    const QString inner = write(QStringLiteral("Inner.md"), "==marked words==\n");
    const QString middle = write(QStringLiteral("Middle.md"), "![[Inner]]\n");
    const QString outer = write(QStringLiteral("Outer.md"), "![[Middle]]\n");
    // The highlight's fill is the one pixel colour only a formatted draw has.
    const auto marked = [](const QImage &image) {
        const QColor highlight = Theme::builtin().color(QStringLiteral("--color-yellow"));
        int found = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (qAbs(c.hue() - highlight.hue()) < 12 && c.saturation() > 80 && c.value() > 90)
                    ++found;
            }
        return found;
    };
    const auto draw = [&](const QString &note, const QString &target) {
        auto editor = editorWith(QStringLiteral("![[%1]]\n\nafter\n").arg(note));
        editor->setLinkResolver([&](const QString &name) { return name == QLatin1String("Inner")  ? inner
                                                                  : name == QLatin1String("Middle") ? middle
                                                                                                    : target; });
        editor->setCursorOffset(editor->noteText().size());
        QTest::qWaitFor([&] { return !editor->embedImageSizeAt(0).isEmpty(); }, 3000);
        return editor->embedImageAt(0);
    };
    const int direct = marked(draw(QStringLiteral("Inner"), inner));
    ASSERT_GT(direct, 20) << "the highlight is drawn when the note is embedded directly";
    EXPECT_GT(marked(draw(QStringLiteral("Middle"), middle)), 20) << "one level down";
    // Nothing in the chain loops: Outer is only ever the document here.
    EXPECT_FALSE(outer.isEmpty());
}

TEST(NoteEmbedTest, ANoteThatEmbedsItselfStopsAtTheFirstLevel)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("Loop.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("# Loop\n\n![[Loop]]\n");
    }
    auto editor = editorWith(QStringLiteral("![[Loop]]\n\nafter\n"));
    editor->setLinkResolver([&](const QString &) { return path; });
    editor->setCursorOffset(editor->noteText().size());
    WAIT_TRUE(!editor->embedImageSizeAt(0).isEmpty(), 3000);
    EXPECT_LT(editor->embedImageSizeAt(0).height(), 600) << "a loop would have run to the 1200 px cap";
}

TEST(NoteEmbedTest, ABlockEmbedHasNoTitleAndAnHTMLHeadingIsASection)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("Target.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("intro\n\n<h2>Two &amp; Three</h2>\n\nhtml section body\n\n## Next\n\nmarkdown body\n\npara ^blk\n");
    }
    auto html = editorWith(QStringLiteral("![[Target#Two & Three]]\n"));
    html->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(html->panelTextAt(0).contains(QLatin1String("html section body"))) << qPrintable(html->panelTextAt(0));
    EXPECT_FALSE(html->panelTextAt(0).contains(QLatin1String("markdown body")));
    EXPECT_FALSE(html->panelTextAt(0).contains(QLatin1String("intro")));

    auto md = editorWith(QStringLiteral("![[Target#Next]]\n"));
    md->setLinkResolver([&](const QString &) { return path; });
    EXPECT_TRUE(md->panelTextAt(0).contains(QLatin1String("markdown body")));

    auto block = editorWith(QStringLiteral("![[Target#^blk]]\n"));
    block->setLinkResolver([&](const QString &) { return path; });
    EXPECT_EQ(block->panelTextAt(0), QStringLiteral("para"));
    EXPECT_EQ(block->panelLabelAt(0), QString()) << "Obsidian titles an embed only when it has no subpath";
}

TEST(BlockReferenceTest, ASameNoteReferenceHidesItsHashAndAnotherNotesStaysWritten)
{
    auto editor = editorWith(QStringLiteral("a [[#^abc]] b [[Other#^abc]] c [[#Head]] d\n"));
    editor->setCursorOffset(0);
    // Obsidian's live preview rewrites only a leading "#": "^abc", "Other#^abc".
    editor->enterBlockAt(editor->noteText().size()); // caret out of the paragraph's way
    const QString shown = editor->displayText();
    EXPECT_TRUE(shown.contains(QLatin1String("a ^abc b Other#^abc c Head d"))) << qPrintable(shown);
}

TEST(CodeInCalloutTest, ALongListingInsideACalloutWrapsInLivePreview)
{
    // Obsidian wraps it too (its callout is a reading-view pre-wrap), so a long
    // line makes the callout taller, not wider than the column.
    const auto heightOf = [](const QString &line) {
        auto editor = editorWith(QStringLiteral("> [!note] t\n> ```\n> ") + line + QStringLiteral("\n> ```\n\nafter\n"));
        editor->setCursorOffset(editor->noteText().size());
        return editor->documentHeight();
    };
    EXPECT_GT(heightOf(QString(400, QLatin1Char('x')) + QStringLiteral(" y")), heightOf(QStringLiteral("x y")) + 20);
}

// --- a fence scrolls sideways instead of wrapping -------------------------------

namespace {

/// A line with spaces, so a wrapping layout has somewhere to break: 300
/// characters, far wider than the 900 px window at any monospace size.
QString longLine()
{
    return QStringLiteral("word ").repeated(60);
}

/// A fence of `body`, then enough paragraphs for the note to scroll.
QString fenceNote(const QString &body, const QString &options = QString())
{
    return QStringLiteral("```text\n") + options + body + QStringLiteral("\n```\n\n")
           + QStringLiteral("after\n\n").repeated(80);
}

/// The start of the fence's first line, and its end.
constexpr qsizetype kFenceBody = 8; // after "```text\n"

void sendWheel(MarkdownEditor &editor, const QPoint &at, const QPoint &pixels, const QPoint &degrees,
               Qt::KeyboardModifiers modifiers, bool *accepted = nullptr)
{
    QWheelEvent event(QPointF(at), QPointF(at), pixels, degrees, Qt::NoButton, modifiers,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(editor.viewport(), &event);
    if (accepted)
        *accepted = event.isAccepted();
}

/// The text column's right edge: the panel is 14 px wider each side.
qreal columnRight(const MarkdownEditor &editor, qsizetype offset)
{
    return editor.textColumn() + editor.blockPanelAt(offset).width() - 28;
}

} // namespace

TEST(CodeScrollTest, ALongLineIsOneLineNotFive)
{
    const auto heightOf = [](const QString &body) {
        auto editor = editorWith(fenceNote(body));
        editor->setCursorOffset(editor->noteText().size());
        return editor->documentHeight();
    };
    EXPECT_DOUBLE_EQ(heightOf(longLine()), heightOf(QStringLiteral("x")));
}

TEST(CodeScrollTest, WrapTrueOptsBackIntoWrapping)
{
    const auto heightOf = [](const QString &body, const QString &options) {
        auto editor = editorWith(fenceNote(body, options));
        editor->setCursorOffset(editor->noteText().size());
        return editor->documentHeight();
    };
    const qreal scrolling = heightOf(longLine(), QString());
    const qreal wrapping = heightOf(longLine(), QStringLiteral("#| wrap: true\n"));
    EXPECT_GT(wrapping, scrolling + 20);
    // `wrap: false` is the default spelt out: it scrolls, so the header the option line adds is not the difference.
    EXPECT_GT(wrapping, heightOf(longLine(), QStringLiteral("#| wrap: false\n")) + 20);
}

TEST(CodeScrollTest, ShiftWheelPansTheListingNotThePage)
{
    auto editor = editorWith(fenceNote(longLine()));
    editor->setCursorOffset(kFenceBody);
    const QRectF before = editor->caretRectNow();
    const int page = editor->verticalScrollBar()->value();
    ASSERT_DOUBLE_EQ(editor->sideScrollAt(kFenceBody), 0);

    bool accepted = false;
    sendWheel(*editor, centreOf(before), QPoint(), QPoint(0, -240), Qt::ShiftModifier, &accepted);

    EXPECT_TRUE(accepted);
    EXPECT_GT(editor->sideScrollAt(kFenceBody), 0);
    EXPECT_LT(editor->caretRectNow().x(), before.x()) << "the caret is drawn where its character is";
    EXPECT_EQ(editor->verticalScrollBar()->value(), page);
}

TEST(CodeScrollTest, HorizontalWheelAndTrackpadPixelsPanToo)
{
    auto editor = editorWith(fenceNote(longLine()));
    editor->setCursorOffset(kFenceBody);
    const QPoint at = centreOf(editor->caretRectNow());

    sendWheel(*editor, at, QPoint(), QPoint(-240, 0), Qt::NoModifier);
    const qreal wheel = editor->sideScrollAt(kFenceBody);
    EXPECT_GT(wheel, 0);

    sendWheel(*editor, at, QPoint(-50, 0), QPoint(), Qt::NoModifier);
    EXPECT_GT(editor->sideScrollAt(kFenceBody), wheel);
}

TEST(CSVembedTest, ASidewaysScrollSurvivesTheReparse)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QByteArray bytes;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 30; ++column)
            bytes += "a fairly long cell " + QByteArray::number(row * column) + (column < 29 ? "," : "\n");
    const QString path = writeTextFile(dir, QStringLiteral("wide.csv"), bytes);
    const QString note = QStringLiteral("![[wide.csv]]\n\nend");
    auto editor = editorWith(note);
    editor->setLinkResolver([path](const QString &) { return path; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return !editor->tableCellTextsAt(0).isEmpty(); }, 5000));
    editor->setCursorOffset(note.size());

    sendWheel(*editor, centreOf(editor->blockPanelAt(0)), QPoint(), QPoint(0, -240), Qt::ShiftModifier);
    const qreal scrolled = editor->sideScrollAt(0);
    ASSERT_GT(scrolled, 0);

    QTest::keyClicks(editor.get(), QStringLiteral("zz")); // a reparse rebuilds every block a moment later
    QTest::qWait(400);
    ASSERT_TRUE(editor->noteText().endsWith(QStringLiteral("endzz")));
    EXPECT_DOUBLE_EQ(editor->sideScrollAt(0), scrolled);
}

TEST(CodeScrollTest, AShortListingIsNotScrollable)
{
    auto editor = editorWith(fenceNote(QStringLiteral("x = 1")));
    editor->setCursorOffset(kFenceBody);
    const QPoint at = centreOf(editor->caretRectNow());
    const int page = editor->verticalScrollBar()->value();

    sendWheel(*editor, at, QPoint(), QPoint(0, -240), Qt::ShiftModifier);

    EXPECT_DOUBLE_EQ(editor->sideScrollAt(kFenceBody), 0);
    EXPECT_GT(editor->verticalScrollBar()->value(), page) << "the wheel falls through to the page";
}

TEST(CodeScrollTest, EndKeepsTheCaretInsideThePanel)
{
    auto editor = editorWith(fenceNote(longLine()));
    editor->setCursorOffset(kFenceBody);

    QTest::keyClick(editor.get(), Qt::Key_End);
    EXPECT_EQ(editor->cursorOffset(), kFenceBody + longLine().size());
    const QRectF caret = editor->caretRectNow();
    EXPECT_LE(caret.right(), columnRight(*editor, kFenceBody));
    EXPECT_GE(caret.left(), editor->textColumn());
    EXPECT_GT(editor->sideScrollAt(kFenceBody), 0);

    QTest::keyClick(editor.get(), Qt::Key_Home);
    EXPECT_DOUBLE_EQ(editor->sideScrollAt(kFenceBody), 0);
}

TEST(CodeScrollTest, TypingPastTheEdgeSurvivesTheReparse)
{
    auto editor = editorWith(fenceNote(longLine()));
    editor->setCursorOffset(kFenceBody + longLine().size());
    QTest::keyClicks(editor.get(), QStringLiteral("zz"));
    const qreal typed = editor->sideScrollAt(kFenceBody);
    ASSERT_GT(typed, 0);

    // The reparse runs a few milliseconds after the keystroke and rebuilds
    // every block; the offset must come through it.
    QTest::qWait(150);
    editor->grab();
    EXPECT_GT(editor->sideScrollAt(kFenceBody), 0);
    EXPECT_LE(editor->caretRectNow().right(), columnRight(*editor, kFenceBody));
    EXPECT_GE(editor->caretRectNow().left(), editor->textColumn());
}

TEST(CodeScrollTest, ClickInTheScrolledPartLandsOnTheRightCharacter)
{
    auto editor = editorWith(fenceNote(longLine()));
    const qsizetype end = kFenceBody + longLine().size();
    editor->setCursorOffset(kFenceBody);
    QTest::keyClick(editor.get(), Qt::Key_End);
    ASSERT_GT(editor->sideScrollAt(kFenceBody), 0);

    // The caret is at the end of the line, drawn far right of where the
    // unscrolled text would put it: a click on it lands on that same offset.
    const QPoint where = centreOf(editor->caretRectNow());
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, where);
    EXPECT_EQ(editor->cursorOffset(), end);

    // A drag from there to the panel's left edge selects the tail of the line.
    const int leftEdge = editor->textColumn() + 4;
    QTest::mousePress(editor->viewport(), Qt::LeftButton, Qt::NoModifier, where);
    QTest::mouseMove(editor->viewport(), QPoint(leftEdge, where.y()));
    QTest::mouseRelease(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(leftEdge, where.y()));
    const QString picked = editor->selectedText();
    EXPECT_FALSE(picked.isEmpty());
    EXPECT_TRUE(longLine().endsWith(picked));
}

TEST(CodeScrollTest, PiecesShareOneOffset)
{
    QStringList lines;
    for (int i = 0; i < 40; ++i)
        lines << (i == 34 ? longLine() : QStringLiteral("line %1").arg(i));
    auto editor = editorWith(fenceNote(lines.join(QLatin1Char('\n'))));
    const qsizetype first = kFenceBody;
    const qsizetype long35 = kFenceBody + lines.mid(0, 34).join(QLatin1Char('\n')).size() + 1;
    editor->setCursorOffset(long35); // lays the second piece out and scrolls it into view

    sendWheel(*editor, centreOf(editor->caretRectNow()), QPoint(), QPoint(0, -240), Qt::ShiftModifier);

    EXPECT_GT(editor->sideScrollAt(long35), 0);
    EXPECT_DOUBLE_EQ(editor->sideScrollAt(first), editor->sideScrollAt(long35));
}

// A parse landing looks the old view of each fresh one up (the sideways offset of a
// listing lives there) with a forward cursor over the old blocks. The views a landing
// remakes are the ones that were drawn - here four listings, one of them edited around -
// and each must find its own old self, not a neighbour's, while the 80 paragraphs
// nothing drew are not remade at all.
TEST(CodeScrollTest, ALandingKeepsEveryListingsOffsetAndLeavesUntouchedViewsAlone)
{
    const QString fence = QStringLiteral("```text\n") + longLine() + QStringLiteral("\n```\n\n");
    const QString note = fence + fence + QStringLiteral("middle\n\n") + fence + fence
                         + QStringLiteral("after\n\n").repeated(80);
    auto editor = editorWith(note);
    editor->setCursorOffset(note.indexOf(QLatin1String("middle")));
    (void)editor->grab();
    const qsizetype first = kFenceBody;
    const qsizetype second = first + fence.size();
    sendWheel(*editor, centreOf(editor->blockPanelAt(first)), QPoint(), QPoint(0, -120), Qt::ShiftModifier);
    sendWheel(*editor, centreOf(editor->blockPanelAt(second)), QPoint(), QPoint(0, -480), Qt::ShiftModifier);
    const qreal one = editor->sideScrollAt(first);
    const qreal two = editor->sideScrollAt(second);
    ASSERT_GT(one, 0);
    ASSERT_GT(two, one);

    const quint64 rebuilt = editor->rebuiltViewCount();
    QTest::keyClicks(editor.get(), QStringLiteral("x"));
    for (int waited = 0; editor->rebuiltViewCount() == rebuilt && waited < 5000; waited += 20)
        QTest::qWait(20);
    ASSERT_GT(editor->rebuiltViewCount(), rebuilt) << "the landing never came";
    EXPECT_DOUBLE_EQ(editor->sideScrollAt(first), one);
    EXPECT_DOUBLE_EQ(editor->sideScrollAt(second), two);
    EXPECT_LT(editor->rebuiltViewCount() - rebuilt, 60u) << "the paragraphs nothing drew stay as they are";
}

TEST(CodeScrollTest, ALineNumberGutterStaysPut)
{
    auto editor = editorWith(fenceNote(longLine(), QStringLiteral("#| lines: true\n")));
    editor->setCursorOffset(editor->noteText().size()); // out of the fence, which renders with its gutter
    editor->verticalScrollBar()->setValue(0);
    editor->grab(); // the first paint settles the viewport the geometry below is read from
    const QRectF panel = editor->blockPanelAt(kFenceBody);
    // Above the scroll bar, which starts at the panel's left edge and moves.
    const QRect strip(editor->textColumn(), int(panel.top()), 24, int(panel.height()) - 14);
    const QImage before = editor->grab().toImage().copy(strip);

    sendWheel(*editor, centreOf(panel), QPoint(), QPoint(0, -490), Qt::ShiftModifier);
    ASSERT_GT(editor->sideScrollAt(kFenceBody), 0);
    EXPECT_TRUE(editor->grab().toImage().copy(strip) == before)
        << "the numbers belong to the panel, not to the text that slides under them";
}

TEST(CodeScrollTest, AListingInACalloutStillWraps)
{
    const auto heightOf = [](const QString &line) {
        auto editor = editorWith(QStringLiteral("> [!note] t\n> ```\n> ") + line + QStringLiteral("\n> ```\n\nafter\n"));
        editor->setCursorOffset(editor->noteText().size());
        return editor->documentHeight();
    };
    EXPECT_GT(heightOf(longLine()), heightOf(QStringLiteral("x y")) + 20);
}

// --- bare brackets, meta notes and what the counts leave out -------------------

namespace {

constexpr QLatin1StringView kBracketNote{"She left early [check the date] and never came back. Also [[wiki]] and [a](u).\n"};

/// A shown editor whose theme gives bare brackets and meta notes colours of
/// their own, so each can be told from the other and from plain text.
std::unique_ptr<MarkdownEditor> bracketEditor(const ParseOptions &options, MarkdownEditor::Mode mode)
{
    auto editor = std::make_unique<MarkdownEditor>();
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    theme.setToken(QStringLiteral("--bracket-color"), QStringLiteral("#ff0000"));
    theme.setToken(QStringLiteral("--meta-note-color"), QStringLiteral("#00ff00"));
    editor->setTheme(theme);
    editor->setParseOptions(options);
    editor->resize(900, 600);
    editor->setNoteText(QString(kBracketNote));
    editor->show();
    QTest::qWait(60);
    editor->setMode(mode); // after the note is up, as a user switches
    QTest::qWait(60);
    return editor;
}

QColor colourOf(const MarkdownEditor &editor, const QString &needle, int into = 1)
{
    const qsizetype at = editor.noteText().indexOf(needle);
    EXPECT_GE(at, 0);
    return editor.formatAt(at + into).foreground().color();
}

} // namespace

TEST(BracketStyleTest, ABareBracketTakesItsTokenInEveryMode)
{
    for (const auto mode : {MarkdownEditor::Mode::LivePreview, MarkdownEditor::Mode::Source,
                            MarkdownEditor::Mode::Reading}) {
        auto editor = bracketEditor(ParseOptions{.brackets = true}, mode);
        const QColor red(0xff, 0, 0);
        const QString needle = QStringLiteral("[check the date]");
        EXPECT_EQ(colourOf(*editor, needle, 0), red) << "the opening bracket is in the run";
        EXPECT_EQ(colourOf(*editor, needle, 6), red);
        EXPECT_EQ(colourOf(*editor, needle, int(needle.size()) - 1), red) << "and the closing one";
        EXPECT_NE(colourOf(*editor, QStringLiteral("She left")), red);
        EXPECT_NE(colourOf(*editor, QStringLiteral(" and never"), 1), red);
        EXPECT_NE(colourOf(*editor, QStringLiteral("[a](u)"), 3), red) << "a link keeps the link colour";
    }
}

TEST(BracketStyleTest, OffInTheParseOptionsIsPlainText)
{
    auto editor = bracketEditor(ParseOptions{}, MarkdownEditor::Mode::LivePreview);
    EXPECT_NE(colourOf(*editor, QStringLiteral("[check the date]"), 5), QColor(0xff, 0, 0));
}

TEST(MetaNoteStyleTest, ANoteTakesItsOwnTokenInEveryModeAndWinsOverTheBracket)
{
    for (const auto mode : {MarkdownEditor::Mode::LivePreview, MarkdownEditor::Mode::Source,
                            MarkdownEditor::Mode::Reading}) {
        auto editor = bracketEditor(ParseOptions{.brackets = true, .metaNotes = true}, mode);
        const QColor green(0, 0xff, 0);
        const QString needle = QStringLiteral("[check the date]");
        EXPECT_EQ(colourOf(*editor, needle, 0), green);
        EXPECT_EQ(colourOf(*editor, needle, 6), green);
        EXPECT_EQ(colourOf(*editor, needle, int(needle.size()) - 1), green);
        EXPECT_NE(colourOf(*editor, QStringLiteral("She left")), green);
        EXPECT_NE(colourOf(*editor, QStringLiteral(" and never"), 1), green);
        EXPECT_EQ(editor->metaNoteRanges().size(), 1);
    }
}

TEST(MetaNoteStyleTest, ACustomPairIsTheNoteAndTheBracketsStayBrackets)
{
    auto editor = std::make_unique<MarkdownEditor>();
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    theme.setToken(QStringLiteral("--bracket-color"), QStringLiteral("#ff0000"));
    theme.setToken(QStringLiteral("--meta-note-color"), QStringLiteral("#00ff00"));
    editor->setTheme(theme);
    editor->setParseOptions(ParseOptions{.brackets = true, .metaNotes = true, .open = QStringLiteral("<<"),
                                         .close = QStringLiteral(">>")});
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("a << fix this >> b [bare] c\n"));
    editor->show();
    QTest::qWait(60);
    EXPECT_EQ(colourOf(*editor, QStringLiteral("<< fix"), 0), QColor(0, 0xff, 0));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("fix this"), 2), QColor(0, 0xff, 0));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("[bare]"), 2), QColor(0xff, 0, 0));
}

TEST(MetaNoteStyleTest, ARunInATableCellIsColouredToo)
{
    auto editor = std::make_unique<MarkdownEditor>();
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    theme.setToken(QStringLiteral("--meta-note-color"), QStringLiteral("#00ff00"));
    editor->setTheme(theme);
    editor->setParseOptions(ParseOptions{.metaNotes = true});
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("| A | B |\n|---|---|\n| x [check] y | z |\n"));
    editor->show();
    QTest::qWait(60);
    ASSERT_EQ(editor->metaNoteRanges().size(), 1);
    // The cell is laid out by the grid; the editor's own cell format is what is read.
    editor->setMode(MarkdownEditor::Mode::Source);
    QTest::qWait(30);
    EXPECT_EQ(colourOf(*editor, QStringLiteral("[check]"), 2), QColor(0, 0xff, 0));
}

namespace {

std::unique_ptr<MarkdownEditor> metaEditor(const QString &text,
                                           MarkdownEditor::Mode mode = MarkdownEditor::Mode::LivePreview)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->setParseOptions(ParseOptions{.brackets = true, .metaNotes = true});
    editor->resize(900, 600);
    editor->setNoteText(text);
    editor->show();
    QTest::qWait(60);
    editor->setMode(mode);
    QTest::qWait(30);
    return editor;
}

} // namespace

TEST(MetaNoteJumpTest, NextAndPreviousSelectWholeRunsAndWrapAround)
{
    for (const auto mode : {MarkdownEditor::Mode::LivePreview, MarkdownEditor::Mode::Source,
                            MarkdownEditor::Mode::Reading}) {
        auto editor = metaEditor(QStringLiteral("one [a] two [bb] three\n\n[ccc] four\n"), mode);
        ASSERT_EQ(editor->metaNoteRanges().size(), 3);
        editor->setCursorOffset(0);
        int index = 0;
        int count = 0;
        const auto jump = [&](int direction) { return editor->selectMetaNote(direction, &index, &count); };

        EXPECT_EQ(jump(+1), MarkdownEditor::MetaJump::Moved);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("[a]")) << "delimiters included";
        EXPECT_EQ(index, 1);
        EXPECT_EQ(count, 3);
        EXPECT_EQ(jump(+1), MarkdownEditor::MetaJump::Moved);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("[bb]"));
        EXPECT_EQ(jump(+1), MarkdownEditor::MetaJump::Moved);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("[ccc]"));
        EXPECT_EQ(index, 3);
        EXPECT_EQ(jump(+1), MarkdownEditor::MetaJump::Wrapped);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("[a]"));
        EXPECT_EQ(index, 1);
        EXPECT_EQ(jump(-1), MarkdownEditor::MetaJump::Wrapped);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("[ccc]"));
        EXPECT_EQ(jump(-1), MarkdownEditor::MetaJump::Moved);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("[bb]"));
        EXPECT_EQ(index, 2);
    }
}

TEST(MetaNoteJumpTest, WithoutRunsNothingMoves)
{
    auto editor = metaEditor(QStringLiteral("no notes here, just [[a link]] and text\n"));
    editor->setCursorOffset(7);
    int count = -1;
    EXPECT_EQ(editor->selectMetaNote(+1, nullptr, &count), MarkdownEditor::MetaJump::None);
    EXPECT_EQ(editor->selectMetaNote(-1), MarkdownEditor::MetaJump::None);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(editor->cursorOffset(), 7);
    EXPECT_TRUE(editor->selectedText().isEmpty());
}

TEST(MetaNoteJumpTest, ACaretOnTheOpeningDelimiterSelectsThatRun)
{
    auto editor = metaEditor(QStringLiteral("a [x] b [y] c\n"));
    editor->setCursorOffset(2); // on the first "["
    EXPECT_EQ(editor->selectMetaNote(+1), MarkdownEditor::MetaJump::Moved);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("[x]")) << "not the one after it";
    editor->setCursorOffset(editor->noteText().indexOf(QLatin1Char('y')) + 1); // inside the second run
    EXPECT_EQ(editor->selectMetaNote(-1), MarkdownEditor::MetaJump::Moved);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("[x]"));
}

TEST(MetaNoteJumpTest, BackspaceDeletesTheSelectedRunInOneStroke)
{
    auto editor = metaEditor(QStringLiteral("a [x] b\n"));
    editor->setCursorOffset(0);
    editor->selectMetaNote(+1);
    QTest::keyClick(editor.get(), Qt::Key_Backspace);
    EXPECT_EQ(editor->noteText(), QStringLiteral("a  b\n"));
}

TEST(MetaNoteContextMenuTest, DeleteMetaNoteLeavesNoStraySpace)
{
    const auto deleteAt = [](MarkdownEditor &editor, const QString &needle) {
        const auto menu = editor.contextMenuAt(editor.noteText().indexOf(needle) + 1);
        QAction *found = nullptr;
        for (QAction *action : menu->actions()) {
            if (action->text() == QLatin1String("Delete meta note"))
                found = action;
        }
        return found ? (found->trigger(), true) : false;
    };
    const QString original = QStringLiteral("She left early [x] and went.\n\n[y] She came back [z].\n");
    auto editor = metaEditor(original);
    ASSERT_EQ(editor->metaNoteRanges().size(), 3);

    EXPECT_TRUE(deleteAt(*editor, QStringLiteral("[x]")));
    EXPECT_EQ(editor->noteText(), QStringLiteral("She left early and went.\n\n[y] She came back [z].\n"));
    QTest::qWait(60); // the reparse after the edit
    EXPECT_TRUE(deleteAt(*editor, QStringLiteral("[y]")));
    EXPECT_EQ(editor->noteText(), QStringLiteral("She left early and went.\n\nShe came back [z].\n"));
    QTest::qWait(60);
    EXPECT_TRUE(deleteAt(*editor, QStringLiteral("[z]")));
    EXPECT_EQ(editor->noteText(), QStringLiteral("She left early and went.\n\nShe came back.\n"));

    EXPECT_FALSE(deleteAt(*editor, QStringLiteral("came"))) << "outside any run there is no such action";
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), original) << "each delete is one undo step";
}

TEST(MetaNoteStyleTest, ABareBracketIsTheAccentAndAMetaNoteIsOrange)
{
    for (const Theme::Mode mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        const Theme theme = Theme::builtin(mode);
        EXPECT_EQ(theme.color(QStringLiteral("--bracket-color")), theme.color(QStringLiteral("--text-accent")));
        EXPECT_EQ(theme.color(QStringLiteral("--meta-note-color")), theme.color(QStringLiteral("--color-orange")));
        EXPECT_NE(theme.color(QStringLiteral("--meta-note-color")), theme.color(QStringLiteral("--bracket-color")));
    }
}

TEST(CountTest, TheCharacterCountLeavesOutWhatTheWordCountDoes)
{
    ParseOptions options;
    auto editor = editorWith(QStringLiteral("one [two three] four\n"));
    const qsizetype all = editor->characterCount();
    EXPECT_EQ(editor->wordCount(), 4);
    options.excludeBetween = true;
    options.brackets = true;
    editor->setParseOptions(options);
    EXPECT_EQ(editor->wordCount(), 2);
    EXPECT_EQ(editor->characterCount(), all - QStringLiteral("[two three]").size());
    // The selection of the whole note is counted the same way.
    QTest::keyClick(editor.get(), Qt::Key_A, Qt::ControlModifier);
    EXPECT_EQ(editor->selectionCount().words, 2);
    options.excludeBetween = false;
    editor->setParseOptions(options);
    EXPECT_EQ(editor->wordCount(), 4);
    EXPECT_EQ(editor->characterCount(), all);
}

TEST(BracketStyleTest, SwitchingModeInANoteWithFootnotesKeepsItsColours)
{
    // Reading view adds a footnotes block, so the mode switch rebuilds the
    // blocks; they must still know which styles are theirs.
    auto editor = std::make_unique<MarkdownEditor>();
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    theme.setToken(QStringLiteral("--bracket-color"), QStringLiteral("#ff0000"));
    editor->setTheme(theme);
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("a [bare] b[^1]\n\n[^1]: the note\n"));
    editor->show();
    QTest::qWait(60);
    for (const auto mode : {MarkdownEditor::Mode::Source, MarkdownEditor::Mode::Reading,
                            MarkdownEditor::Mode::LivePreview}) {
        editor->setMode(mode);
        QTest::qWait(30);
        EXPECT_EQ(colourOf(*editor, QStringLiteral("[bare]"), 2), QColor(0xff, 0, 0)) << int(mode);
    }
}

// --- formatting colours -------------------------------------------------------

namespace {

/// An editor on a theme with `tokens` pinned, its first block (where the
/// caret starts, so its markers show) a throwaway: tests read the blocks after.
std::unique_ptr<MarkdownEditor> editorOn(const QString &text, const QHash<QString, QString> &tokens)
{
    auto editor = std::make_unique<MarkdownEditor>();
    Theme theme = Theme::builtin();
    theme.setTokens(tokens);
    editor->setTheme(theme);
    editor->resize(900, 600);
    editor->setNoteText(QStringLiteral("start\n\n") + text);
    editor->grab(); // lays the visible blocks out
    return editor;
}

QTextCharFormat formatOf(const MarkdownEditor &editor, const QString &needle, int into = 1)
{
    const qsizetype at = editor.noteText().indexOf(needle);
    EXPECT_GE(at, 0) << needle.toStdString();
    return editor.formatAt(at + into);
}

} // namespace

TEST(FormatColoursTest, UnsetBoldLeavesNoForeground)
{
    // A foreground on a bold range would override the block's own colour:
    // bold in a callout or a heading would snap to the text colour.
    auto editor = editorOn(QStringLiteral("Some **bold** and *italic* and ~~gone~~.\n"), {});
    for (const char *word : {"bold", "italic", "gone"})
        EXPECT_FALSE(formatOf(*editor, QString::fromLatin1(word)).hasProperty(QTextFormat::ForegroundBrush)) << word;
    EXPECT_GE(formatOf(*editor, QStringLiteral("bold")).fontWeight(), int(QFont::Bold));
}

TEST(FormatColoursTest, BoldItalicHeadingQuoteTokensTint)
{
    auto editor = editorOn(QStringLiteral("Some **bold** and *ital* and ***both***\n\n## Head two\n\n> quoted\n\n# Head one\n"),
                           {{QStringLiteral("--bold-color"), QStringLiteral("#ff0000")},
                            {QStringLiteral("--italic-color"), QStringLiteral("#00ff00")},
                            {QStringLiteral("--h2-color"), QStringLiteral("#0000ff")},
                            {QStringLiteral("--blockquote-color"), QStringLiteral("#123456")}});
    EXPECT_EQ(colourOf(*editor, QStringLiteral("bold")), QColor(0xff, 0, 0));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("ital")), QColor(0, 0xff, 0));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("both")), QColor(0xff, 0, 0)) << "bold italic takes the bold colour";
    EXPECT_EQ(colourOf(*editor, QStringLiteral("Head two")), QColor(0, 0, 0xff));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("quoted")), QColor(0x12, 0x34, 0x56));
    EXPECT_FALSE(formatOf(*editor, QStringLiteral("Head one")).hasProperty(QTextFormat::ForegroundBrush))
        << "h1 has no colour set";
}

TEST(FormatColoursTest, AWordInsideAColouredHeadingKeepsItsOwnColour)
{
    auto editor = editorOn(QStringLiteral("## Head **loud** [[Missing]]\n"),
                           {{QStringLiteral("--h2-color"), QStringLiteral("#0000ff")},
                            {QStringLiteral("--bold-color"), QStringLiteral("#ff0000")}});
    EXPECT_EQ(colourOf(*editor, QStringLiteral("Head")), QColor(0, 0, 0xff));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("loud")), QColor(0xff, 0, 0));
}

TEST(FormatColoursTest, InternalExternalAndUnresolvedLinksDiffer)
{
    const QHash<QString, QString> tokens{{QStringLiteral("--link-color"), QStringLiteral("#010101")},
                                         {QStringLiteral("--link-external-color"), QStringLiteral("#020202")},
                                         {QStringLiteral("--link-unresolved-color"), QStringLiteral("#030303")}};
    auto editor = std::make_unique<MarkdownEditor>();
    Theme theme = Theme::builtin();
    theme.setTokens(tokens);
    editor->setTheme(theme);
    editor->resize(900, 600);
    editor->setLinkResolver([](const QString &target) {
        return target == QStringLiteral("Real") ? QStringLiteral("/vault/Real.md") : QString();
    });
    editor->setNoteText(QStringLiteral("start\n\nSee [[Real]], [x](https://a.example), [[Missing]].\n"));
    editor->grab();
    EXPECT_EQ(colourOf(*editor, QStringLiteral("[[Real]]"), 3), QColor(1, 1, 1));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("[x](https"), 1), QColor(2, 2, 2));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("[[Missing]]"), 3), QColor(3, 3, 3));
}

TEST(FormatColoursTest, UnderlineLineColourIsNative)
{
    auto editor = editorOn(QStringLiteral("a <u>under</u> b\n"),
                           {{QStringLiteral("--underline-decoration-color"), QStringLiteral("#ff0000")},
                            {QStringLiteral("--underline-color"), QStringLiteral("#00ff00")}});
    const QTextCharFormat format = formatOf(*editor, QStringLiteral("under"));
    EXPECT_EQ(format.underlineStyle(), QTextCharFormat::SingleUnderline);
    EXPECT_EQ(format.underlineColor(), QColor(0xff, 0, 0));
    EXPECT_EQ(format.foreground().color(), QColor(0, 0xff, 0));
}

namespace {

/// The most pixels of `colour` in one row of the editor's picture.
int widestRun(const QImage &shot, const QColor &colour)
{
    int best = 0;
    for (int y = 0; y < shot.height(); ++y) {
        int count = 0;
        for (int x = 0; x < shot.width(); ++x)
            count += QColor(shot.pixel(x, y)) == colour;
        best = qMax(best, count);
    }
    return best;
}

} // namespace

TEST(FormatColoursTest, StrikeLineColourIsPaintedNotQts)
{
    const QString note = QStringLiteral("Some ~~struck words here~~ and <del>deleted too</del>\n\n- [x] done task\n");
    auto plain = editorOn(note, {});
    EXPECT_TRUE(formatOf(*plain, QStringLiteral("struck")).fontStrikeOut());
    EXPECT_EQ(widestRun(plain->viewport()->grab().toImage(), QColor(0xff, 0, 0)), 0);

    auto editor = editorOn(note, {{QStringLiteral("--strikethrough-decoration-color"), QStringLiteral("#ff0000")}});
    for (const char *word : {"struck", "deleted", "done task"}) {
        const QTextCharFormat format = formatOf(*editor, QString::fromLatin1(word));
        EXPECT_FALSE(format.fontStrikeOut()) << word << ": Qt would draw it in the text colour";
        EXPECT_TRUE(format.hasProperty(QTextFormat::UserProperty + 1)) << word << ": the line colour, for paintStrikeLines";
    }
    // "struck words here" is some 100 px of text, struck through in one line.
    EXPECT_GE(widestRun(editor->viewport()->grab().toImage(), QColor(0xff, 0, 0)), 60);
}

namespace {

/// Pixels within `tolerance` of `colour` on every channel.
int pixelsNear(const QImage &shot, const QColor &colour, int tolerance = 40)
{
    int count = 0;
    for (int y = 0; y < shot.height(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            const QColor pixel(shot.pixel(x, y));
            count += qAbs(pixel.red() - colour.red()) <= tolerance && qAbs(pixel.green() - colour.green()) <= tolerance
                     && qAbs(pixel.blue() - colour.blue()) <= tolerance;
        }
    }
    return count;
}

} // namespace

TEST(FormatColoursTest, AStruckTableCellIsPaintedToo)
{
    const QString table = QStringLiteral("| a | b |\n|---|---|\n| ~~struck cell here~~ | x |\n");
    auto editor = editorOn(table, {{QStringLiteral("--strikethrough-decoration-color"), QStringLiteral("#ff0000")}});
    EXPECT_GE(widestRun(editor->viewport()->grab().toImage(), QColor(0xff, 0, 0)), 40);
}

TEST(FormatColoursTest, ListMarkersAndCheckboxesTakeTheirColours)
{
    const QString list = QStringLiteral("- bullet\n- [x] ticked\n- [ ] open\n");
    auto plain = editorOn(list, {});
    EXPECT_EQ(pixelsNear(plain->viewport()->grab().toImage(), QColor(0xff, 0, 0)), 0);
    auto editor = editorOn(list, {{QStringLiteral("--list-marker-color"), QStringLiteral("#ff0000")},
                                  {QStringLiteral("--checkbox-color"), QStringLiteral("#00ff00")},
                                  {QStringLiteral("--checkbox-border-color"), QStringLiteral("#0000ff")}});
    const QImage shot = editor->viewport()->grab().toImage();
    EXPECT_GT(pixelsNear(shot, QColor(0xff, 0, 0)), 0) << "the bullet";
    EXPECT_GT(pixelsNear(shot, QColor(0, 0xff, 0)), 20) << "the ticked box's fill";
    EXPECT_GT(pixelsNear(shot, QColor(0, 0, 0xff)), 10) << "the open box's border";
}

TEST(FormatColoursTest, StrikeTextColourAppliesWithoutALineColour)
{
    auto editor = editorOn(QStringLiteral("~~gone~~\n"), {{QStringLiteral("--strikethrough-color"), QStringLiteral("#00ff00")}});
    const QTextCharFormat format = formatOf(*editor, QStringLiteral("gone"));
    EXPECT_EQ(format.foreground().color(), QColor(0, 0xff, 0));
    EXPECT_TRUE(format.fontStrikeOut());
}

TEST(FormatColoursTest, TheOtherKindsReadTheirOwnTokens)
{
    auto editor = editorOn(QStringLiteral("a ==marked== `code` #tag [^1] <mark>m</mark>\n\n- [x] done\n\n[^1]: n\n"),
                           {{QStringLiteral("--text-highlight-bg"), QStringLiteral("#111111")},
                            {QStringLiteral("--text-highlight-fg"), QStringLiteral("#222222")},
                            {QStringLiteral("--code-normal"), QStringLiteral("#333333")},
                            {QStringLiteral("--code-background"), QStringLiteral("#444444")},
                            {QStringLiteral("--tag-color"), QStringLiteral("#555555")},
                            {QStringLiteral("--checklist-done-color"), QStringLiteral("#666666")}});
    const QTextCharFormat mark = formatOf(*editor, QStringLiteral("marked"));
    EXPECT_EQ(mark.background().color(), QColor(0x11, 0x11, 0x11));
    EXPECT_EQ(mark.foreground().color(), QColor(0x22, 0x22, 0x22));
    const QTextCharFormat code = formatOf(*editor, QStringLiteral("code"));
    EXPECT_EQ(code.foreground().color(), QColor(0x33, 0x33, 0x33));
    EXPECT_EQ(code.background().color(), QColor(0x44, 0x44, 0x44));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("#tag")), QColor(0x55, 0x55, 0x55));
    EXPECT_EQ(formatOf(*editor, QStringLiteral("<mark>m"), 6).background().color(), QColor(0x11, 0x11, 0x11));
    EXPECT_EQ(colourOf(*editor, QStringLiteral("done"), 1), QColor(0x66, 0x66, 0x66));
}

TEST(FormatColoursTest, RecolourKeepsHeightsScrollAndLaysOutOnlyTheScreen)
{
    QString note;
    // Uneven blocks, so that estimating them all again would not land on the
    // heights they were measured at.
    for (int i = 0; i < 1500; ++i)
        note += QStringLiteral("Paragraph %1 with **bold** words that run on for a good while, long enough to wrap.%2\n\n")
                    .arg(i)
                    .arg(QStringLiteral(" And a little more to read.").repeated(i % 7 * 2));
    ASSERT_GT(note.size(), 120000);
    auto editor = parsedEditorWith(note);
    ASSERT_NE(editor, nullptr);
    editor->setTheme(Theme::builtin()); // a real theme to start from, not the default-constructed one
    editor->grab();
    auto *bar = editor->verticalScrollBar();
    bar->setValue(bar->maximum() / 2);
    editor->grab();
    // Estimates settle into measured heights as blocks around the view get laid
    // out; a recolour from a settled state must not move anything. (Nudging the
    // scroll, not the theme: a theme change is what is being tested.)
    qreal height = 0;
    for (int round = 0; round < 20 && editor->documentHeight() != height; ++round) {
        height = editor->documentHeight();
        bar->setValue(bar->value() + 400);
        editor->grab();
        bar->setValue(bar->value() - 400);
        editor->grab();
    }
    const int scroll = bar->value();
    const quint64 before = editor->layoutCount();
    ASSERT_GT(scroll, 1000);

    Theme theme = Theme::builtin();
    theme.setToken(QStringLiteral("--bold-color"), QStringLiteral("#ff0000"));
    editor->setTheme(theme);
    editor->grab();
    EXPECT_EQ(bar->value(), scroll);
    EXPECT_EQ(editor->documentHeight(), height);
    EXPECT_GT(editor->layoutCount(), before) << "the blocks on screen are drawn again, in the new colour";
    EXPECT_LT(editor->layoutCount() - before, 100u) << "not the whole note";
    int red = 0;
    for (int i = 600; i < 900; ++i)
        red += colourOf(*editor, QStringLiteral("Paragraph %1 with **bold**").arg(i), 22) == QColor(0xff, 0, 0);
    EXPECT_GE(red, 3) << "the paragraphs on screen are in the new colour";

    // A size is not a colour: that still lays everything out again, so the
    // whole note grows, not just the blocks on screen.
    theme.setToken(QStringLiteral("--font-text-size"), QStringLiteral("24"));
    editor->setTheme(theme);
    editor->grab();
    EXPECT_GT(editor->documentHeight(), height * 1.2);
}

TEST(FormatColoursTest, NothingIsWrittenToTheNote)
{
    const QString note = QStringLiteral("start\n\nSome **bold** and ~~gone~~ and <u>u</u>.\n");
    auto editor = editorWith(note);
    Theme theme = Theme::builtin();
    theme.setTokens({{QStringLiteral("--bold-color"), QStringLiteral("#ff0000")},
                     {QStringLiteral("--strikethrough-decoration-color"), QStringLiteral("#00ff00")},
                     {QStringLiteral("--underline-decoration-color"), QStringLiteral("#0000ff")}});
    editor->setTheme(theme);
    editor->grab();
    EXPECT_EQ(editor->noteText(), note);
    QTest::keyClick(editor.get(), Qt::Key_X);
    editor->setTheme(Theme::builtin());
    editor->grab();
    EXPECT_EQ(editor->noteText(), QStringLiteral("x") + note);
}

TEST(FormatColoursTest, PdfExportTakesTheCallersTheme)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MarkdownEditor::PdfOptions options;
    options.theme = Theme::builtin(Theme::Mode::Light);
    options.theme->setToken(QStringLiteral("--h1-color"), QStringLiteral("#ff0000"));
    QString error;
    // Colours inside a PDF cannot be read back without a rasteriser; the
    // plumbing is the editor's own formats, tested above.
    EXPECT_TRUE(MarkdownEditor::exportPdf(dir.filePath(QStringLiteral("a.pdf")), QStringLiteral("# Red\n\n**b**\n"),
                                          QStringLiteral("t"), {}, options, &error))
        << qPrintable(error);
}

// An edit from elsewhere (another tab, another person) moves the undo steps
// with it instead of emptying them.
TEST(ExternalEditTest, UndoCarriesAnEditMadeAfterTheLocalOnes)
{
    auto editor = editorWith(QStringLiteral("alpha\n\nbeta\n"));
    editor->setCursorOffset(5);
    editor->insertAtCaret(QStringLiteral("123"));
    ASSERT_EQ(editor->noteText(), QStringLiteral("alpha123\n\nbeta\n"));
    editor->applyExternalEdit(editor->noteText().size(), 0, QStringLiteral("Z"));
    ASSERT_EQ(editor->noteText(), QStringLiteral("alpha123\n\nbeta\nZ"));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("alpha\n\nbeta\nZ")) << "only the local typing is undone";
    QTest::keyClick(editor.get(), Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("alpha123\n\nbeta\nZ"));
}

TEST(ExternalEditTest, UndoCarriesAnEditMadeBeforeTheLocalOnes)
{
    auto editor = editorWith(QStringLiteral("alpha\n\nbeta\n"));
    editor->setCursorOffset(editor->noteText().size());
    editor->insertAtCaret(QStringLiteral("end"));
    editor->applyExternalEdit(0, 0, QStringLiteral("top\n"));
    ASSERT_EQ(editor->noteText(), QStringLiteral("top\nalpha\n\nbeta\nend"));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("top\nalpha\n\nbeta\n"));
}

TEST(ExternalEditTest, AnEditInsideWhatWasTypedDropsThatStepAndTheOnesBefore)
{
    auto editor = editorWith(QStringLiteral("one\n"));
    editor->setCursorOffset(0);
    editor->insertAtCaret(QStringLiteral("first "));
    QTest::qWait(600); // a second undo step
    editor->setCursorOffset(editor->noteText().size());
    editor->insertAtCaret(QStringLiteral("second"));
    ASSERT_EQ(editor->noteText(), QStringLiteral("first one\nsecond"));
    // Inside the newest step's region: it and the older step go; nothing is garbled.
    editor->applyExternalEdit(editor->noteText().size() - 3, 0, QStringLiteral("#"));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("first one\nsec#ond")) << "no step left to undo";
}

TEST(ExternalEditTest, TheWholeTextReplacedEmptiesTheHistory)
{
    auto editor = editorWith(QStringLiteral("abc"));
    editor->setCursorOffset(3);
    editor->insertAtCaret(QStringLiteral("d"));
    editor->applyExternalEdit(0, 4, QStringLiteral("new"));
    QTest::keyClick(editor.get(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(editor->noteText(), QStringLiteral("new"));
}

TEST(HunkTest, TheRegionThatDiffersNeverSplitsASurrogatePair)
{
    // Two emoji that share their first half: U+1F600 and U+1F601.
    const QString one = QString::fromUcs4(U"a\U0001F600b", 3);
    const QString other = QString::fromUcs4(U"a\U0001F601b", 3);
    ASSERT_EQ(one.size(), 4);
    const MarkdownEditor::Hunk hunk = MarkdownEditor::hunkBetween(one, other);
    EXPECT_EQ(hunk.at, 1);
    EXPECT_EQ(hunk.removed, 2);
    EXPECT_EQ(hunk.inserted, other.mid(1, 2));
    EXPECT_EQ(MarkdownEditor::hunkBetween(one, one).removed, 0);
    EXPECT_TRUE(MarkdownEditor::hunkBetween(one, one).inserted.isEmpty());
    // the same for the ends: a shared second half
    const QString low = QString::fromUcs4(U"\U0001F600", 1);
    const QString lowOther = QString::fromUcs4(U"\U0001F200", 1);
    const MarkdownEditor::Hunk tail = MarkdownEditor::hunkBetween(low, lowOther);
    EXPECT_EQ(tail.at, 0);
    EXPECT_EQ(tail.removed, 2);
}

// --- Code Styler: ```reference blocks ----------------------------------------------

#ifdef NOTEAPPC_HAVE_NETWORK
#include <QTcpServer>
#include <QTcpSocket>
#endif

namespace {

/// An offset inside the block's listing, just past the opening fence.
constexpr qsizetype kInsideReference = 13;

/// An editor on `fenceLines` (the lines inside a ```reference fence), with the
/// plugin on and every target resolved to `path` unless it is a URL.
std::unique_ptr<MarkdownEditor> referenceEditorWith(const QString &fenceLines, const QString &path,
                                                    bool on = true)
{
    // the caret goes below the block: with it inside, the source shows
    const QString note = QStringLiteral("```reference\n%1```\n\nafter\n").arg(fenceLines);
    auto editor = stylerEditorWith(note, on);
    editor->setLinkResolver([path](const QString &) { return path; });
    // the file's folder is the vault: confinement has its own tests below
    const QString root = QFileInfo(path).absolutePath();
    editor->setVaultRootProvider([root] { return root; });
    editor->setCursorOffset(note.size() - 2);
    return editor;
}

/// A vault in a temp folder next to a file outside it, and the resolver the
/// window has (the note's folder, then the vault root: whatever exists).
struct ReferenceVault {
    QTemporaryDir outer;
    QString vault;
    QString secret;

    ReferenceVault()
    {
        vault = outer.filePath(QStringLiteral("vault"));
        EXPECT_TRUE(QDir().mkpath(vault));
        secret = outer.filePath(QStringLiteral("secret.txt"));
        QFile(secret).open(QIODevice::WriteOnly);
        write(secret, "TOPSECRET\n");
        write(vault + QStringLiteral("/inside.txt"), "visible\n");
        EXPECT_TRUE(QFile::link(QStringLiteral("../secret.txt"), vault + QStringLiteral("/escape.txt")));
        EXPECT_TRUE(QFile::link(QStringLiteral("inside.txt"), vault + QStringLiteral("/alias.txt")));
    }
    static void write(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }
    std::unique_ptr<MarkdownEditor> editorOn(const QString &fenceLines) const
    {
        const QString note = QStringLiteral("```reference\n%1```\n\nafter\n").arg(fenceLines);
        auto editor = stylerEditorWith(note, true);
        const QString root = vault;
        editor->setLinkResolver([root](const QString &target) -> QString {
            const QString beside = root + QLatin1Char('/') + target;
            if (QFileInfo::exists(beside))
                return beside;
            const QString fromRoot = QDir(root).absoluteFilePath(target);
            return QFileInfo::exists(fromRoot) ? fromRoot : QString();
        });
        editor->setVaultRootProvider([root] { return root; });
        editor->setCursorOffset(note.size() - 2);
        return editor;
    }
};

/// The panel's text with its line separators as the newlines they stand for.
QString listingOf(MarkdownEditor &editor)
{
    (void)editor.grab(); // blocks are laid out as they are painted
    QString text = editor.panelTextAt(kInsideReference);
    text.replace(QChar(QChar::LineSeparator), QLatin1Char('\n'));
    return text;
}

bool waitForListing(MarkdownEditor &editor, const QString &needle)
{
    return QTest::qWaitFor([&] { return listingOf(editor).contains(needle); }, 5000);
}

QString numberedFile(const QTemporaryDir &dir, int lines)
{
    QByteArray bytes;
    for (int i = 1; i <= lines; ++i)
        bytes += "line" + QByteArray::number(i) + (i == 4 ? "  marker\n" : "\n");
    return writeTextFile(dir, QStringLiteral("many.txt"), bytes);
}

std::atomic<bool> gReferenceReadOnUIthread{false};
std::thread::id gReferenceUIthread;

} // namespace

TEST(ReferenceBlockTest, ShowsTheFileHighlightedUnderItsName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeTextFile(dir, QStringLiteral("hello.py"), "import os\nprint(os.name)\n");
    auto editor = referenceEditorWith(QStringLiteral("file: hello.py\n"), path);

    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("Loading"))) << "nothing is read on the layout path: loading first";
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("import os")));
    EXPECT_EQ(listingOf(*editor), QStringLiteral("import os\nprint(os.name)"));
    EXPECT_EQ(editor->panelLabelAt(kInsideReference), QStringLiteral("hello.py"));
#ifdef NOTEAPPC_EXPECT_SYNTAX_HIGHLIGHTING
    EXPECT_GT(editor->panelFormatCountAt(kInsideReference), 0) << "highlighted by the file's suffix";
#endif
    EXPECT_EQ(editor->referenceCacheSize(), 1);
}

// A printed page waits for the excerpt, not only for the file read: the cut is a second worker, started
// by the layout that follows the read (what exportPdf does between its first pass and the page).
TEST(ReferenceBlockTest, AwaitingReferencesWaitsForTheExcerptToo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // long enough that cutting the excerpt (a scan for the start word) outlasts the event that delivers the read
    QByteArray bytes;
    for (int i = 0; i < 60000; ++i)
        bytes += "filler line " + QByteArray::number(i) + "\n";
    bytes += "needle here\n";
    const QString path = writeTextFile(dir, QStringLiteral("big.txt"), bytes);
    auto editor = referenceEditorWith(QStringLiteral("file: big.txt\nstart: needle\n"), path);
    (void)editor->grab(); // the first layout starts the read
    editor->awaitReferences(5000);
    EXPECT_FALSE(editor->referencesPending());
    EXPECT_EQ(listingOf(*editor), QStringLiteral("needle here")) << "no Loading... on the printed page";
}

#ifdef NOTEAPPC_EXPECT_SYNTAX_HIGHLIGHTING
TEST(ReferenceBlockTest, TheLanguageKeyOverridesTheSuffix)
{
    QTemporaryDir dir;
    const QString path = writeTextFile(dir, QStringLiteral("script.txt"), "import os\n");
    auto plain = referenceEditorWith(QStringLiteral("file: script.txt\n"), path);
    ASSERT_TRUE(waitForListing(*plain, QStringLiteral("import")));
    auto python = referenceEditorWith(QStringLiteral("file: script.txt\nlang: python\n"), path);
    ASSERT_TRUE(waitForListing(*python, QStringLiteral("import")));
    EXPECT_GT(python->panelFormatCountAt(kInsideReference), plain->panelFormatCountAt(kInsideReference));
}
#endif

TEST(ReferenceBlockTest, StartAndEndPickTheLines)
{
    QTemporaryDir dir;
    const QString path = numberedFile(dir, 20);

    auto numbers = referenceEditorWith(QStringLiteral("file: many.txt\nstart: 3\nend: 5\n"), path);
    ASSERT_TRUE(waitForListing(*numbers, QStringLiteral("line3")));
    EXPECT_EQ(listingOf(*numbers), QStringLiteral("line3\nline4  marker\nline5"));
    EXPECT_EQ(numbers->panelLabelAt(kInsideReference), QStringLiteral("many.txt:3-5"));

    // `+N` counts on from start, inclusive: 10 through 12
    auto relative = referenceEditorWith(QStringLiteral("file: many.txt\nstart: 10\nend: +2\n"), path);
    ASSERT_TRUE(waitForListing(*relative, QStringLiteral("line10")));
    EXPECT_EQ(listingOf(*relative), QStringLiteral("line10\nline11\nline12"));

    // a word is the first line containing it, a /regex/ the first line it matches
    auto word = referenceEditorWith(QStringLiteral("file: many.txt\nstart: marker\nend: /^line7$/\n"), path);
    ASSERT_TRUE(waitForListing(*word, QStringLiteral("marker")));
    EXPECT_EQ(listingOf(*word), QStringLiteral("line4  marker\nline5\nline6\nline7"));

    // no end: to the end of the file
    auto open = referenceEditorWith(QStringLiteral("file: many.txt\nstart: 19\n"), path);
    ASSERT_TRUE(waitForListing(*open, QStringLiteral("line19")));
    EXPECT_EQ(listingOf(*open), QStringLiteral("line19\nline20"));
}

TEST(ReferenceBlockTest, ALineThatIsNotThereIsSaidSo)
{
    QTemporaryDir dir;
    const QString path = numberedFile(dir, 5);
    auto editor = referenceEditorWith(QStringLiteral("file: many.txt\nstart: nonexistent\n"), path);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("nonexistent")));
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("No line matches")));
}

TEST(ReferenceBlockTest, AFileThatIsNotThereShowsWhy)
{
    auto editor = referenceEditorWith(QStringLiteral("file: gone.py\n"), QString());
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("Could not find gone.py")));
}

TEST(ReferenceBlockTest, PlainSourceWhenThePluginIsOff)
{
    QTemporaryDir dir;
    const QString path = writeTextFile(dir, QStringLiteral("hello.py"), "print(1)\n");
    auto editor = referenceEditorWith(QStringLiteral("file: hello.py\n"), path, false);
    QTest::qWait(50);
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("file: hello.py"))) << "the plain listing";
    EXPECT_TRUE(editor->panelLabelAt(kInsideReference).isEmpty());
    EXPECT_EQ(editor->referenceCacheSize(), 0) << "nothing is read when the feature is off";
}

TEST(ReferenceBlockTest, TheCaretInsideShowsTheSource)
{
    QTemporaryDir dir;
    const QString path = writeTextFile(dir, QStringLiteral("hello.py"), "print(1)\n");
    auto editor = referenceEditorWith(QStringLiteral("file: hello.py\n"), path);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("print")));
    editor->enterBlockAt(editor->noteText().indexOf(QStringLiteral("hello")) + 2);
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("file: hello.py")));
    EXPECT_TRUE(editor->panelLabelAt(kInsideReference).isEmpty());
}

TEST(ReferenceBlockTest, AChangeOnDiskIsReadAgain)
{
    QTemporaryDir dir;
    const QString path = writeTextFile(dir, QStringLiteral("live.txt"), "first\n");
    auto editor = referenceEditorWith(QStringLiteral("file: live.txt\n"), path);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("first")));

    writeTextFile(dir, QStringLiteral("live.txt"), "second version, longer\n");
    EXPECT_TRUE(waitForListing(*editor, QStringLiteral("second version"))) << "the watcher relays the block out";
    EXPECT_EQ(editor->referenceCacheSize(), 1);
}

TEST(ReferenceBlockTest, TheFileIsNeverReadOnTheUIthread)
{
    QTemporaryDir dir;
    const QString path = writeTextFile(dir, QStringLiteral("hello.py"), "print(1)\n");
    gReferenceUIthread = std::this_thread::get_id();
    gReferenceReadOnUIthread = false;
    MarkdownEditor::setReferenceReadHookForTests([](const QString &) {
        gReferenceReadOnUIthread = std::this_thread::get_id() == gReferenceUIthread;
    });
    auto editor = referenceEditorWith(QStringLiteral("file: hello.py\n"), path);
    const bool shown = waitForListing(*editor, QStringLiteral("print"));
    MarkdownEditor::setReferenceReadHookForTests(nullptr);
    ASSERT_TRUE(shown);
    EXPECT_FALSE(gReferenceReadOnUIthread);
}

TEST(ReferenceBlockTest, ClickingTheHeaderAsksForTheFileToBeOpened)
{
    QTemporaryDir dir;
    const QString path = writeTextFile(dir, QStringLiteral("hello.py"), "print(1)\n");
    auto editor = referenceEditorWith(QStringLiteral("file: hello.py\n"), path);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("print")));
    (void)editor->grab();

    QSignalSpy spy(editor.get(), &MarkdownEditor::referenceActivated);
    const QRectF panel = editor->blockPanelAt(kInsideReference);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->textColumn() + 10, int(panel.top()) + 20)); // on the title strip
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toString(), path);

    // a click on the listing is no link: it puts the caret in, and the source shows
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(editor->textColumn() + 10, int(panel.top()) + 50));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("file: hello.py")));
}

#ifndef NOTEAPPC_HAVE_NETWORK
TEST(ReferenceBlockTest, ARemoteFileIsNotSupportedInThisBuild)
{
    auto editor = referenceEditorWith(QStringLiteral("link: https://example.com/dir/file.txt\n"), QString());
    QTest::qWait(100);
    EXPECT_EQ(editor->referenceCacheSize(), 0) << "nothing is read or fetched";
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("not supported")));
}
#endif

#ifdef NOTEAPPC_HAVE_NETWORK
TEST(ReferenceBlockTest, AHostingSitesPageLinkIsFetchedAsTheRawFile)
{
    EXPECT_EQ(MarkdownEditor::rawReferenceURL(QStringLiteral("https://github.com/u/r/blob/main/src/a.py")),
              QStringLiteral("https://raw.githubusercontent.com/u/r/main/src/a.py"));
    EXPECT_EQ(MarkdownEditor::rawReferenceURL(QStringLiteral("https://gitlab.com/g/p/-/blob/v1/dir/a.py")),
              QStringLiteral("https://gitlab.com/g/p/-/raw/v1/dir/a.py"));
    const QString other = QStringLiteral("https://example.com/blob/a/b/c/d.txt");
    EXPECT_EQ(MarkdownEditor::rawReferenceURL(other), other);
}

namespace {

/// The remote-host check refuses loopback, which is where the tests' server is:
/// this lets 127.0.0.1 through for one test, and only that address.
struct AllowLoopback {
    AllowLoopback() { MarkdownEditor::allowReferenceAddressForTests(QStringLiteral("127.0.0.1")); }
    ~AllowLoopback() { MarkdownEditor::allowReferenceAddressForTests(QString()); }
};

/// A web server on 127.0.0.1 answering every GET with `body`, counting them.
struct FakeServer {
    QTcpServer server;
    int requests = 0;
    QByteArray body = "remote line 1\nremote line 2\n";
    int status = 200;
    QByteArray redirectTo; ///< answered as a 302 to this URL

    explicit FakeServer(const QHostAddress &address = QHostAddress::LocalHost)
    {
        EXPECT_TRUE(server.listen(address));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (!socket->peek(8192).contains("\r\n\r\n"))
                        return;
                    socket->readAll();
                    ++requests;
                    if (!redirectTo.isEmpty())
                        socket->write("HTTP/1.1 302 Found\r\nLocation: " + redirectTo
                                      + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    else
                        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Length: "
                                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString url(const QString &path = QStringLiteral("/dir/file.txt")) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path);
    }
};

} // namespace

TEST(ReferenceBlockTest, ARemoteFileIsNotFetchedWhileTheSettingIsOff)
{
    FakeServer server;
    auto editor = referenceEditorWith(QStringLiteral("link: %1\n").arg(server.url()), QString());
    QTest::qWait(200);
    EXPECT_EQ(server.requests, 0);
    EXPECT_EQ(editor->referenceCacheSize(), 0);
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("off")));
    EXPECT_EQ(editor->panelLabelAt(kInsideReference), QStringLiteral("file.txt (127.0.0.1)")) << "the host is in the title";
}

TEST(ReferenceBlockTest, ARemoteFileIsFetchedOnceAndCached)
{
    AllowLoopback allow;
    FakeServer server;
    const QString url = server.url(QStringLiteral("/dir/cached.txt"));
    auto editor = referenceEditorWith(QStringLiteral("link: %1\n").arg(url), QString());
    editor->setRemoteReferences(true);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("remote line 2")));
    EXPECT_EQ(server.requests, 1);

    // another editor, the same URL: the copy of the first is what it shows
    auto second = referenceEditorWith(QStringLiteral("link: %1\n").arg(url), QString());
    second->setRemoteReferences(true);
    EXPECT_TRUE(waitForListing(*second, QStringLiteral("remote line 1"))) << "from the cache (the cut is on a worker)";
    // a relayout of the first (a width change) asks for nothing either
    editor->resize(700, 500);
    (void)editor->grab();
    QTest::qWait(100);
    EXPECT_EQ(server.requests, 1);
}

TEST(ReferenceBlockTest, ARemoteFailureIsShownAndNotRetriedPerLayout)
{
    AllowLoopback allow;
    FakeServer server;
    server.status = 404;
    auto editor = referenceEditorWith(QStringLiteral("link: %1\n").arg(server.url(QStringLiteral("/missing.txt"))),
                                      QString());
    editor->setRemoteReferences(true);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("404")));
    editor->resize(700, 500);
    (void)editor->grab();
    QTest::qWait(100);
    EXPECT_EQ(server.requests, 1);
}

#endif // NOTEAPPC_HAVE_NETWORK

// --- reference blocks stay inside the vault ------------------------------------------

TEST(ReferenceBlockTest, AFileInsideTheVaultIsShownWhicheverWayItIsWritten)
{
    ReferenceVault vault;
    for (const char *written : {"inside.txt", "./inside.txt", "@/inside.txt", "alias.txt"}) {
        SCOPED_TRACE(written);
        auto editor = vault.editorOn(QStringLiteral("file: %1\n").arg(QLatin1String(written)));
        EXPECT_TRUE(waitForListing(*editor, QStringLiteral("visible")));
    }
}

TEST(ReferenceBlockTest, AFileOutsideTheVaultIsRefusedWithANotice)
{
    ReferenceVault vault;
    const QStringList written = {vault.secret,                    // absolute
                                 QStringLiteral("../secret.txt"), // out by ..
                                 QStringLiteral("@/../secret.txt"),
                                 QStringLiteral("./../secret.txt"),
                                 QStringLiteral("escape.txt"),   // a symlink in the vault pointing out
                                 QStringLiteral("[[../secret.txt]]")};
    for (const QString &target : written) {
        SCOPED_TRACE(target.toStdString());
        auto editor = vault.editorOn(QStringLiteral("file: %1\n").arg(target));
        EXPECT_TRUE(waitForListing(*editor, QStringLiteral("Outside the vault")));
        EXPECT_FALSE(listingOf(*editor).contains(QStringLiteral("TOPSECRET")));
        EXPECT_EQ(editor->referenceCacheSize(), 0) << "never read";

        // and the title opens nothing
        (void)editor->grab();
        QSignalSpy spy(editor.get(), &MarkdownEditor::referenceActivated);
        const QRectF panel = editor->blockPanelAt(kInsideReference);
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(editor->textColumn() + 10, int(panel.top()) + 20));
        EXPECT_EQ(spy.count(), 0);
    }
}

TEST(ReferenceBlockTest, WithoutAVaultNoLocalFileIsShown)
{
    ReferenceVault vault;
    auto editor = vault.editorOn(QStringLiteral("file: inside.txt\n"));
    editor->setVaultRootProvider(nullptr);
    EXPECT_TRUE(waitForListing(*editor, QStringLiteral("Outside the vault")));
}

TEST(ReferenceBlockTest, ARunawayStartPatternIsStoppedQuickly)
{
    QTemporaryDir dir;
    QByteArray bytes;
    for (int i = 0; i < 300; ++i)
        bytes += QByteArray(30, 'a') + "!\n";
    const QString path = writeTextFile(dir, QStringLiteral("bad.txt"), bytes);
    auto editor = referenceEditorWith(QStringLiteral("file: bad.txt\nstart: /(a+)+$/\n"), path);
    QElapsedTimer clock;
    clock.start();
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("No line matches start")));
    EXPECT_LT(clock.elapsed(), 1500) << "ms: without the match budget this took seconds";
}

TEST(ReferenceBlockTest, AScanOverABigFileNeverStallsTheUIthread)
{
    QTemporaryDir dir;
    QByteArray bytes;
    for (int i = 0; i < 80000; ++i)
        bytes += "lorem ipsum dolor sit am!\n"; // 2 MB of prose no `$` can follow a word on
    const QString path = writeTextFile(dir, QStringLiteral("big.txt"), bytes);
    auto editor = referenceEditorWith(QStringLiteral("file: big.txt\nstart: /(\\w+\\s?)+$/\n"), path);
    (void)editor->grab();
    // a ticking timer measures the longest the event loop was kept from running
    QElapsedTimer since;
    since.start();
    qint64 last = 0;
    qint64 longest = 0;
    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        longest = qMax(longest, since.elapsed() - last);
        last = since.elapsed();
    });
    ticker.start(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return listingOf(*editor).contains(QStringLiteral("No line matches start")); }, 15000));
    ticker.stop();
    EXPECT_LT(longest, 300) << "ms the UI thread was held: the scan belongs on the worker";
}

#ifdef NOTEAPPC_HAVE_NETWORK
// --- remote references: hosts that are not the internet ---------------------------------

TEST(ReferenceBlockTest, LiteralLocalAndPrivateHostsAreRefused)
{
    const QStringList urls = {
        QStringLiteral("http://10.1.2.3/a.txt"),         QStringLiteral("http://192.168.0.7/a.txt"),
        QStringLiteral("http://172.16.0.1/a.txt"),       QStringLiteral("http://169.254.169.254/latest/meta-data"),
        QStringLiteral("http://127.0.0.1:9/a.txt"),      QStringLiteral("http://[::1]/a.txt"),
        QStringLiteral("http://[fd00::1]/a.txt"),        QStringLiteral("http://[fe80::1]/a.txt"),
        QStringLiteral("http://0.0.0.0/a.txt"),          QStringLiteral("http://[::ffff:10.0.0.1]/a.txt"),
        QStringLiteral("http://[::127.0.0.1]/a.txt"),    QStringLiteral("http://[::a00:1]/a.txt"),
        QStringLiteral("http://[64:ff9b::7f00:1]/a.txt"), QStringLiteral("http://[64:ff9b::a9fe:a9fe]/a.txt"),
        QStringLiteral("http://[2002:7f00:1::1]/a.txt")};
    for (const QString &url : urls) {
        SCOPED_TRACE(url.toStdString());
        auto editor = referenceEditorWith(QStringLiteral("link: %1\n").arg(url), QString());
        editor->setRemoteReferences(true);
        EXPECT_TRUE(waitForListing(*editor, QStringLiteral("local address")));
        EXPECT_FALSE(editor->referencesPending());
    }
}

TEST(ReferenceBlockTest, ANameThatResolvesToLoopbackIsRefusedBeforeAnyRequest)
{
    FakeServer server;
    auto editor = referenceEditorWith(
        QStringLiteral("link: http://localhost:%1/a.txt\n").arg(server.server.serverPort()), QString());
    editor->setRemoteReferences(true);
    EXPECT_TRUE(waitForListing(*editor, QStringLiteral("local address")));
    QTest::qWait(200);
    EXPECT_EQ(server.requests, 0);

    // by address, with no allowance for it, the same
    auto direct = referenceEditorWith(QStringLiteral("link: %1\n").arg(server.url()), QString());
    direct->setRemoteReferences(true);
    EXPECT_TRUE(waitForListing(*direct, QStringLiteral("local address")));
    QTest::qWait(200);
    EXPECT_EQ(server.requests, 0);
}

TEST(ReferenceBlockTest, ALookupThatEndsAfterTheSettingIsOffSendsNothing)
{
    // "localhost" stands for a public name: both of its addresses are allowed
    MarkdownEditor::allowReferenceAddressForTests(QStringLiteral("127.0.0.1,::1"));
    const auto restore = qScopeGuard([] { MarkdownEditor::allowReferenceAddressForTests(QString()); });
    FakeServer server;
    const QString note = QStringLiteral("link: http://localhost:%1/a.txt\n").arg(server.server.serverPort());

    // control: left on, the request does arrive
    {
        auto editor = referenceEditorWith(note, QString());
        editor->setRemoteReferences(true);
        EXPECT_TRUE(QTest::qWaitFor([&] { (void)editor->grab(); return server.requests > 0; }, 5000));
    }
    server.requests = 0;

    // the lookup is asked for in the layout and answers on the event loop: the setting goes off in between
    auto off = referenceEditorWith(note, QString());
    off->setRemoteReferences(true);
    (void)off->grab();
    off->setRemoteReferences(false);
    QTest::qWait(500);
    EXPECT_EQ(server.requests, 0);

    // and so does the plugin
    auto plugin = referenceEditorWith(note, QString());
    plugin->setRemoteReferences(true);
    (void)plugin->grab();
    auto none = plugin->parseOptions();
    none.codeStyler = false;
    plugin->setParseOptions(none);
    QTest::qWait(500);
    EXPECT_EQ(server.requests, 0);
}

TEST(ReferenceBlockTest, ARedirectToALocalHostIsRefusedToo)
{
    AllowLoopback allow; // 127.0.0.1 stands for "a public host" here; 127.0.0.2 is where it must not go
    FakeServer inner(QHostAddress(QStringLiteral("127.0.0.2")));
    FakeServer outer;
    outer.redirectTo = QByteArray("http://127.0.0.2:") + QByteArray::number(inner.server.serverPort()) + "/secret.txt";
    auto editor = referenceEditorWith(QStringLiteral("link: %1\n").arg(outer.url(QStringLiteral("/a.txt"))), QString());
    editor->setRemoteReferences(true);
    EXPECT_TRUE(waitForListing(*editor, QStringLiteral("local address")));
    EXPECT_EQ(outer.requests, 1);
    EXPECT_EQ(inner.requests, 0);
}

TEST(ReferenceBlockTest, ARedirectToAPublicHostIsFollowed)
{
    AllowLoopback allow;
    FakeServer target;
    target.body = "landed\n";
    FakeServer outer;
    outer.redirectTo = target.url(QStringLiteral("/b.txt")).toUtf8();
    auto editor = referenceEditorWith(QStringLiteral("link: %1\n").arg(outer.url(QStringLiteral("/a.txt"))), QString());
    editor->setRemoteReferences(true);
    EXPECT_TRUE(waitForListing(*editor, QStringLiteral("landed")));
    EXPECT_EQ(target.requests, 1);
}

// --- what is kept, and when it is dropped ----------------------------------------------------

TEST(ReferenceBlockTest, TurningThePluginOffDropsTheCacheTheWatchesAndTheFetchedText)
{
    AllowLoopback allow;
    FakeServer server;
    QTemporaryDir dir;
    const QString path = writeTextFile(dir, QStringLiteral("hello.py"), "print(1)\n");
    const QString note = QStringLiteral("```reference\nfile: hello.py\n```\n\n```reference\nlink: %1\n```\n\nafter\n")
                             .arg(server.url(QStringLiteral("/kept.txt")));
    auto editor = stylerEditorWith(note, true);
    editor->setLinkResolver([path](const QString &) { return path; });
    editor->setVaultRootProvider([root = dir.path()] { return root; });
    editor->setRemoteReferences(true);
    editor->setCursorOffset(note.size() - 2);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            (void)editor->grab();
            return editor->referenceCacheSize() == 2 && !editor->referencesPending();
        },
        5000));
    EXPECT_GT(editor->referenceWatchedForTests(), 0);
    EXPECT_GT(MarkdownEditor::remoteReferenceCopiesForTests(), 0);

    ParseOptions off = editor->parseOptions();
    off.codeStyler = false;
    editor->setParseOptions(off);
    EXPECT_EQ(editor->referenceCacheSize(), 0);
    EXPECT_EQ(editor->referenceWatchedForTests(), 0);
    EXPECT_EQ(MarkdownEditor::remoteReferenceCopiesForTests(), 0);
}

TEST(ReferenceBlockTest, TurningRemoteFetchingOffForgetsWhatWasFetched)
{
    AllowLoopback allow;
    FakeServer server;
    auto editor = referenceEditorWith(QStringLiteral("link: %1\n").arg(server.url(QStringLiteral("/gone.txt"))), QString());
    editor->setRemoteReferences(true);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("remote line 1")));
    ASSERT_EQ(editor->referenceCacheSize(), 1);

    editor->setRemoteReferences(false);
    EXPECT_EQ(editor->referenceCacheSize(), 0);
    EXPECT_EQ(MarkdownEditor::remoteReferenceCopiesForTests(), 0);
    EXPECT_TRUE(listingOf(*editor).contains(QStringLiteral("off")));
}

#endif // NOTEAPPC_HAVE_NETWORK

TEST(ReferenceBlockTest, ARelayoutDoesNotCutTheFileAgain)
{
    QTemporaryDir dir;
    QByteArray bytes;
    for (int i = 1; i <= 200; ++i)
        bytes += "line " + QByteArray::number(i) + "\n";
    const QString path = writeTextFile(dir, QStringLiteral("many.txt"), bytes);
    auto editor = referenceEditorWith(QStringLiteral("file: many.txt\nstart: 20\nend: 40\n"), path);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("line 40")));
    const int scans = MarkdownEditor::referenceScansForTests();
    EXPECT_GT(scans, 0);
    for (int width : {700, 640, 800, 900}) {
        editor->resize(width, 500);
        (void)editor->grab();
    }
    EXPECT_EQ(MarkdownEditor::referenceScansForTests(), scans) << "the excerpt is kept per file, range and theme";
    EXPECT_GT(editor->panelFormatCountAt(kInsideReference) + 1, 0);
}

TEST(ReferenceBlockTest, AVeryLongLineIsCutAndABinaryFileIsReadOnce)
{
    QTemporaryDir dir;
    const QString minified = writeTextFile(dir, QStringLiteral("min.js"), QByteArray(100000, 'x') + "\n");
    auto editor = referenceEditorWith(QStringLiteral("file: min.js\n"), minified);
    ASSERT_TRUE(waitForListing(*editor, QStringLiteral("xxx")));
    EXPECT_LT(listingOf(*editor).size(), 3000);

    static std::atomic<int> reads{0};
    reads = 0;
    const QString binary = writeTextFile(dir, QStringLiteral("blob.bin"), QByteArray("ab\0cd", 5) + QByteArray(100, 'x'));
    MarkdownEditor::setReferenceReadHookForTests([](const QString &) { ++reads; });
    auto second = referenceEditorWith(QStringLiteral("file: blob.bin\n"), binary);
    const bool said = waitForListing(*second, QStringLiteral("not a text file"));
    for (int width : {700, 640, 800})
        second->resize(width, 500), (void)second->grab();
    QTest::qWait(100);
    MarkdownEditor::setReferenceReadHookForTests(nullptr);
    ASSERT_TRUE(said);
    EXPECT_EQ(reads.load(), 1) << "a file that is not text is not asked for again at every layout";
}

TEST(CodeStylerEditorTest, IgnoreWinsOverAnOptionLine)
{
    auto plain = stylerEditorWith(QStringLiteral("```cpp\nint a;\n```\n"));
    auto ignored = stylerEditorWith(QStringLiteral("```cpp ignore\n#| title: X\nint a;\n```\n"));
    EXPECT_GT(plain->documentHeight() - ignored->documentHeight(), 10) << "no header strip";
}

TEST(CodeStylerEditorTest, InlineLanguagePrefixIsTextInSourceModeAndHiddenInReading)
{
    const QString note = QStringLiteral("Say `{python} import os` now.\n\nend\n");
    auto editor = stylerEditorWith(note);
    editor->setCursorOffset(note.size() - 2);
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("{python}")));
    editor->setMode(MarkdownEditor::Mode::Source);
    EXPECT_TRUE(editor->displayText().contains(QStringLiteral("{python}")));
    editor->setMode(MarkdownEditor::Mode::Reading);
    EXPECT_FALSE(editor->displayText().contains(QStringLiteral("{python}")));
}

// --- spelling on right click ----------------------------------------------

TEST(SpellingTest, TheMenuStillOffersTheWordTheCaretIsIn)
{
    // A right click puts the caret in the word before the menu is built, and
    // the word being typed is exempt from underlining: the menu is not.
    FakeSpelling dictionary;
    const QString note = QStringLiteral("good wrds here\n");
    auto editor = editorWith(note);
    editor->setSpelling(dictionary.spelling());
    const qsizetype at = note.indexOf(QLatin1String("wrds")) + 2;
    editor->enterBlockAt(at);
    ASSERT_FALSE(underlined(*editor, at)) << "the exemption is still there for typing";

    std::unique_ptr<QMenu> menu = editor->spellingMenuAt(at);
    ASSERT_TRUE(menu);
    EXPECT_EQ(menu->actions().first()->text(), QStringLiteral("words"));
    EXPECT_EQ(menu->actions().last()->text(), QStringLiteral("Add \"wrds\" to dictionary"));
    // And the menu the right click really opens leads with it.
    const std::unique_ptr<QMenu> full = editor->contextMenuAt(at);
    ASSERT_TRUE(full);
    EXPECT_EQ(full->actions().first()->text(), QStringLiteral("words"));
}
