// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// The handwriting-font glyph studio's data layer (libs/ext/hwfont): the glyph
// set, the on-disk names and the Handwriting/<font>/<hex>/<n>.ink store.
#include <gtest/gtest.h>

#ifdef NOTEAPPC_STATIC_HWFONT
#include <QtPlugin>
Q_IMPORT_PLUGIN(HwfontModule) // NOTEAPPC_STATIC_MODULES: the module is linked into this test, as into the app
#endif

#include <algorithm>
#include <memory>
#include <optional>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>

#include "GlyphSet.h"
#include "GlyphStore.h"
#include "GlyphStudio.h"
#include "InkCanvas.h"
#include "InkDocument.h"
#include "InkFormat.h"
#include "InkInput.h"
#include "InkTestSupport.h"
#include "MainWindow.h"
#include "ModuleHost.h"
#include "Settings.h"
#include "Vault.h"
#include "WaitFor.h"

using namespace noteAppC::hwfont;
using noteAppC::ink::InkDocument;
using noteAppC::ink::Stroke;

namespace {

const GlyphTarget *find(const std::vector<GlyphTarget> &set, char32_t codepoint)
{
    const auto it = std::find_if(set.begin(), set.end(), [codepoint](const GlyphTarget &t) { return t.codepoint == codepoint; });
    return it == set.end() ? nullptr : &*it;
}

InkDocument sampleWithOneStroke()
{
    InkDocument sample;
    sample.setSettings(GlyphStore::samplePage());
    auto stroke = std::make_shared<Stroke>();
    stroke->points = {{50, 100, 0.5}, {150, 280, 0.5}};
    sample.addStroke(stroke);
    sample.commit();
    return sample;
}

} // namespace

TEST(GlyphSetTest, DefaultSetHasLettersDigitsPunctuationAndSlovene)
{
    const auto set = defaultGlyphSet(3, 1);
    for (const char32_t c : {U'a', U'Z', U'0', U'!', U'č', U'š', U'ž', U'Č', U'Š', U'Ž'})
        EXPECT_NE(find(set, c), nullptr) << "missing U+" << std::hex << uint32_t(c);
    EXPECT_EQ(find(set, U'a')->wanted, 3);
    EXPECT_EQ(find(set, U'č')->wanted, 3);
    EXPECT_EQ(find(set, U'!')->wanted, 1);
    EXPECT_EQ(find(set, U'0')->wanted, 1);
    EXPECT_TRUE(std::is_sorted(set.begin(), set.end(), [](const GlyphTarget &a, const GlyphTarget &b) { return a.codepoint < b.codepoint; }));
    EXPECT_EQ(std::adjacent_find(set.begin(), set.end(), [](const GlyphTarget &a, const GlyphTarget &b) { return a.codepoint == b.codepoint; }), set.end());
}

TEST(GlyphSetTest, DirectoryNamesRoundTrip)
{
    EXPECT_EQ(glyphDirectoryName(U'a'), QStringLiteral("0061"));
    EXPECT_EQ(glyphDirectoryName(U'č'), QStringLiteral("010d"));
    EXPECT_EQ(glyphDirectoryName(0x1F600), QStringLiteral("01f600"));
    char32_t out = 0;
    ASSERT_TRUE(codepointFromDirectoryName(QStringLiteral("0061"), &out));
    EXPECT_EQ(out, U'a');
    ASSERT_TRUE(codepointFromDirectoryName(QStringLiteral("010d"), &out));
    EXPECT_EQ(out, U'č');
    ASSERT_TRUE(codepointFromDirectoryName(QStringLiteral("01f600"), &out));
    EXPECT_EQ(out, char32_t(0x1F600));
    for (const char *good : {"d7ff", "e000", "10ffff"})
        EXPECT_TRUE(codepointFromDirectoryName(QString::fromLatin1(good), &out)) << good;
    for (const char *bad : {"", "61", "010D", "00061", "zzzz", "0061 ", "..", "110000", "-061", "d800", "dabc", "dfff", "0d800", "ffffff"})
        EXPECT_FALSE(codepointFromDirectoryName(QString::fromLatin1(bad), &out)) << bad;
}

TEST(GlyphStoreTest, SamplePageIsUsableAndFixed)
{
    const auto page = GlyphStore::samplePage();
    EXPECT_TRUE(page.isUsable());
    EXPECT_EQ(page.layout, noteAppC::ink::LayoutKind::Fixed);
    EXPECT_DOUBLE_EQ(page.format.width, 400);
    EXPECT_DOUBLE_EQ(page.format.height, 400);
    EXPECT_EQ(page.background.pattern, noteAppC::ink::Pattern::None);
    EXPECT_EQ(page.background.color, QColor(Qt::white));
    const auto m = GlyphStore::defaultMetrics();
    EXPECT_DOUBLE_EQ(m.baseline, 280);
    EXPECT_DOUBLE_EQ(m.xHeight, 160);
    EXPECT_DOUBLE_EQ(m.ascender, 100);
}

TEST(GlyphStoreTest, SavesUnderHandwritingFontHexN)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    const InkDocument sample = sampleWithOneStroke();
    const quint64 revision = sample.revision();
    const bool modified = sample.isModified();

    QString error;
    ASSERT_TRUE(store.saveSample(U'a', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    ASSERT_TRUE(store.saveSample(U'a', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    const QString dir = vault.path() + QStringLiteral("/Handwriting/Test/0061");
    EXPECT_TRUE(QFileInfo::exists(dir + QStringLiteral("/1.ink")));
    EXPECT_TRUE(QFileInfo::exists(dir + QStringLiteral("/2.ink")));
    EXPECT_EQ(store.sampleCount(U'a'), 2);
    EXPECT_EQ(store.sampleCount(U'b'), 0);
    EXPECT_EQ(store.nextSamplePath(U'a'), dir + QStringLiteral("/3.ink"));
    EXPECT_EQ(store.fontDirectory(), vault.path() + QStringLiteral("/Handwriting/Test"));

    QFile file(dir + QStringLiteral("/1.ink"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    EXPECT_TRUE(bytes.contains("\"glyph\":{")) << bytes.constData();
    EXPECT_TRUE(bytes.contains("\"codepoint\":97")) << bytes.constData();
    InkDocument back;
    ASSERT_TRUE(noteAppC::ink::readInk(bytes, back).ok);
    const QJsonObject glyph = back.settings().extra.value(QStringLiteral("glyph")).toObject();
    EXPECT_EQ(glyph.value(QStringLiteral("codepoint")).toInt(), 97);
    EXPECT_DOUBLE_EQ(glyph.value(QStringLiteral("baseline")).toDouble(), 280);
    EXPECT_DOUBLE_EQ(glyph.value(QStringLiteral("xHeight")).toDouble(), 160);
    EXPECT_DOUBLE_EQ(glyph.value(QStringLiteral("ascender")).toDouble(), 100);
    EXPECT_EQ(back.items().size(), 1u) << "the stroke is saved with its glyph";

    EXPECT_FALSE(sample.settings().extra.contains(QStringLiteral("glyph"))) << "the argument is unchanged";
    EXPECT_EQ(sample.revision(), revision);
    EXPECT_EQ(sample.isModified(), modified);
}

TEST(GlyphStoreTest, SaveCreatesTheMissingDirectories)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    EXPECT_FALSE(QFileInfo::exists(vault.path() + QStringLiteral("/Handwriting")));
    GlyphStore store(vault.path(), QStringLiteral("Hand"));
    QString error;
    ASSERT_TRUE(store.saveSample(U'č', sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    EXPECT_TRUE(QFileInfo(vault.path() + QStringLiteral("/Handwriting/Hand/010d/1.ink")).isFile());
}

TEST(GlyphStoreTest, RefusesFontNamesThatCouldLeaveHandwriting)
{
    for (const QString &name : {QString(), QStringLiteral("a/b"), QStringLiteral("a\\b"), QStringLiteral(".."), QStringLiteral("a..b"),
                                QStringLiteral(".hidden"), QStringLiteral("."), QStringLiteral("../x"), QStringLiteral("bad\nname"),
                                QStringLiteral("tab\there"), QString(QChar(0)) + QStringLiteral("x"), QStringLiteral("/etc"),
                                QStringLiteral("hand "), QStringLiteral("hand."), QStringLiteral("a#b"), QStringLiteral("a^b"),
                                QStringLiteral("a[b"), QStringLiteral("a]b"), QStringLiteral("a|b"), QStringLiteral("a:b"),
                                QStringLiteral("CON"), QStringLiteral("con"), QStringLiteral("Nul"), QStringLiteral("PRN"),
                                QStringLiteral("aux"), QStringLiteral("COM1"), QStringLiteral("com9"), QStringLiteral("LPT1"),
                                QStringLiteral("lpt9"), QStringLiteral("aux.txt"), QStringLiteral(" con "),
                                QString(QChar(0xD800)), QStringLiteral("a") + QChar(0xDC00), QString(QChar(0xD800)) + QStringLiteral("x")}) {
        EXPECT_FALSE(GlyphStore::isValidFontName(name)) << qPrintable(name.toHtmlEscaped());
        QTemporaryDir vault;
        ASSERT_TRUE(vault.isValid());
        GlyphStore store(vault.path(), name);
        QString error;
        EXPECT_FALSE(store.saveSample(U'a', sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error));
        EXPECT_FALSE(error.isEmpty());
        EXPECT_EQ(store.sampleCount(U'a'), 0);
        EXPECT_TRUE(store.nextSamplePath(U'a').isEmpty());
        EXPECT_TRUE(QDir(vault.path()).isEmpty()) << "nothing was written for " << qPrintable(name.toHtmlEscaped());
    }
    for (const QString &name : {QStringLiteral("Test"), QStringLiteral("My hand 2"), QStringLiteral("Piši")})
        EXPECT_TRUE(GlyphStore::isValidFontName(name)) << qPrintable(name);
}

TEST(GlyphStoreTest, DoesNotWriteThroughASymlinkedFolder)
{
    QTemporaryDir vault, outside;
    ASSERT_TRUE(vault.isValid() && outside.isValid());
    ASSERT_TRUE(QFile::link(outside.path(), vault.path() + QStringLiteral("/Handwriting")));
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    QString error;
    EXPECT_FALSE(store.saveSample(U'a', sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error));
    EXPECT_TRUE(QDir(outside.path()).isEmpty()) << "nothing was written outside the vault";
}

TEST(GlyphStoreTest, RefusesCodepointsThatHaveNoDirectoryName)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    for (const char32_t c : {char32_t(0xD800), char32_t(0xDBFF), char32_t(0xDFFF), char32_t(0x110000), char32_t(0x80000000)}) {
        QString error;
        EXPECT_FALSE(store.saveSample(c, sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error)) << uint32_t(c);
        EXPECT_FALSE(error.isEmpty());
        EXPECT_EQ(store.sampleCount(c), 0);
        EXPECT_TRUE(store.nextSamplePath(c).isEmpty());
    }
    EXPECT_TRUE(QDir(vault.path()).isEmpty()) << "no orphan folder was made";
    QString error;
    EXPECT_TRUE(store.saveSample(0x10FFFF, sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
}

TEST(GlyphStoreTest, NumberingFillsGapsAndIgnoresJunk)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    const QString dir = vault.path() + QStringLiteral("/Handwriting/Test/0061");
    ASSERT_TRUE(QDir().mkpath(dir));
    const auto touch = [&](const QString &name) {
        QFile file(dir + QLatin1Char('/') + name);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x");
    };
    for (const char *junk : {"x.ink", "1.ink.bak", "01.ink", "0.ink", "-1.ink", "12345678901234.ink", ".5.ink", "notes.txt"})
        touch(QString::fromLatin1(junk));
    ASSERT_TRUE(QDir().mkpath(dir + QStringLiteral("/3.ink"))); // a folder is not a sample
    ASSERT_TRUE(QDir().mkpath(vault.path() + QStringLiteral("/Handwriting/Test/zzzz")));
    ASSERT_TRUE(QDir().mkpath(vault.path() + QStringLiteral("/Handwriting/Test/d800")));
    EXPECT_EQ(store.sampleCount(U'a'), 0);
    EXPECT_EQ(store.nextSamplePath(U'a'), dir + QStringLiteral("/1.ink"));
    touch(QStringLiteral("1.ink"));
    touch(QStringLiteral("4.ink"));
    EXPECT_EQ(store.sampleCount(U'a'), 2);
    EXPECT_EQ(store.nextSamplePath(U'a'), dir + QStringLiteral("/2.ink")) << "the gap is filled first";
    const auto progress = store.progress({{U'a', 3}, {U'b', 1}});
    ASSERT_EQ(progress.size(), 2u);
    EXPECT_EQ(progress[0].first.codepoint, U'a'); // needs more, by code point
    EXPECT_EQ(progress[0].second, 2);
    EXPECT_EQ(progress[1].second, 0);
}

TEST(GlyphStoreTest, SaveNeverReplacesAnExistingSample)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    const QString dir = vault.path() + QStringLiteral("/Handwriting/Test/0061");
    ASSERT_TRUE(QDir().mkpath(dir + QStringLiteral("/1.ink"))); // taken by something nextSamplePath's listing does not show
    QString error;
    ASSERT_TRUE(store.saveSample(U'a', sampleWithOneStroke(), GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    EXPECT_TRUE(QFileInfo(dir + QStringLiteral("/1.ink")).isDir()) << "the name that was taken is left alone";
    EXPECT_TRUE(QFileInfo(dir + QStringLiteral("/2.ink")).isFile());
    const QStringList left = QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    EXPECT_EQ(left.size(), 2) << qPrintable(left.join(QLatin1Char(',')));
}

TEST(GlyphStoreTest, ProgressPutsNeedsMoreFirst)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    GlyphStore store(vault.path(), QStringLiteral("Test"));
    const InkDocument sample = sampleWithOneStroke();
    QString error;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(store.saveSample(U'a', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    ASSERT_TRUE(store.saveSample(U'b', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);
    ASSERT_TRUE(store.saveSample(U'!', sample, GlyphStore::defaultMetrics(), &error)) << qPrintable(error);

    const std::vector<GlyphTarget> set{{U'!', 1}, {U'b', 3}, {U'a', 3}, {U'c', 3}};
    const auto progress = store.progress(set);
    ASSERT_EQ(progress.size(), 4u);
    // needs more: b (1 of 3), c (0 of 3), by code point; then the finished: '!' (1 of 1), a (3 of 3)
    EXPECT_EQ(progress[0].first.codepoint, U'b');
    EXPECT_EQ(progress[0].second, 1);
    EXPECT_EQ(progress[1].first.codepoint, U'c');
    EXPECT_EQ(progress[1].second, 0);
    EXPECT_EQ(progress[2].first.codepoint, U'!');
    EXPECT_EQ(progress[3].first.codepoint, U'a');
    EXPECT_EQ(progress[3].second, 3);
}

// --- the studio view -----------------------------------------------------------

namespace {

class GlyphStudioTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        vault = std::make_unique<noteAppC::Vault>(dir.path());
        host.vault = vault.get();
    }

    QString sampleFile(const QString &hex, int n = 1, const QString &font = QStringLiteral("Test")) const
    {
        return dir.filePath(QStringLiteral("Handwriting/%1/%2/%3.ink").arg(font, hex).arg(n));
    }

    /// One pen stroke across the cell; `ms` keeps successive strokes in time order.
    static void stroke(noteAppC::hwfont::GlyphStudio &studio, qint64 ms = 1000)
    {
        inktest::penStroke(studio.canvas(), {100, 120}, {220, 270}, ms);
    }

    static QListWidgetItem *rowFor(noteAppC::hwfont::GlyphStudio &studio, char32_t codepoint)
    {
        auto *list = studio.findChild<QListWidget *>(QStringLiteral("glyphList"));
        if (!list)
            return nullptr;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toUInt() == codepoint)
                return list->item(i);
        }
        return nullptr;
    }

    QTemporaryDir dir;
    std::unique_ptr<noteAppC::Vault> vault;
    noteAppC::ModuleHost host;
};

} // namespace

TEST_F(GlyphStudioTest, SaveAdvancesAndWritesTheFile)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    ASSERT_EQ(studio.currentTarget(), U'a') << "a handwriting font starts at the letters";
    QSignalSpy status(&studio, &noteAppC::ModuleView::statusMessage);
    stroke(studio);
    ASSERT_EQ(studio.canvas()->document().items().size(), 1u);

    studio.saveCurrentAndAdvance();
    EXPECT_TRUE(QFile::exists(sampleFile(QStringLiteral("0061"))));
    EXPECT_EQ(studio.currentTarget(), U'b');
    EXPECT_TRUE(studio.canvas()->document().items().empty());
    ASSERT_NE(rowFor(studio, U'a'), nullptr);
    EXPECT_EQ(rowFor(studio, U'a')->text(), QStringLiteral("a 1/3"));
    ASSERT_FALSE(status.isEmpty());
    EXPECT_EQ(status.last().at(0).toString(), QStringLiteral("Saved a 1/3"));
}

TEST_F(GlyphStudioTest, RetargetFromTheListAndDiscard)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    auto *list = studio.findChild<QListWidget *>(QStringLiteral("glyphList"));
    ASSERT_NE(list, nullptr);
    QListWidgetItem *row = rowFor(studio, U'b');
    ASSERT_NE(row, nullptr);
    list->scrollToItem(row);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(row).center());
    EXPECT_EQ(studio.currentTarget(), U'b');

    stroke(studio);
    ASSERT_EQ(studio.canvas()->document().items().size(), 1u);
    QTest::keyClick(studio.canvas(), Qt::Key_Backspace);
    EXPECT_TRUE(studio.canvas()->document().items().empty());
    EXPECT_FALSE(studio.isModified());
    EXPECT_FALSE(QDir(dir.filePath(QStringLiteral("Handwriting"))).exists()) << "a discarded cell writes nothing";
    EXPECT_EQ(studio.currentTarget(), U'b') << "discarding stays on the letter";
}

TEST_F(GlyphStudioTest, EnterSavesBackspaceDiscardsAndCtrlZUndoesAStroke)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    studio.canvas()->setFocus();
    stroke(studio);
    QTest::keyClick(studio.canvas(), Qt::Key_Return);
    EXPECT_TRUE(QFile::exists(sampleFile(QStringLiteral("0061"))));
    EXPECT_FALSE(QFile::exists(sampleFile(QStringLiteral("0061"), 2)));
    EXPECT_EQ(studio.currentTarget(), U'b');

    stroke(studio, 2000);
    stroke(studio, 3000);
    const auto &doc = studio.canvas()->document();
    ASSERT_EQ(doc.items().size(), 2u);
    QTest::keyClick(studio.canvas(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(doc.items().size(), 1u);
    QTest::keyClick(studio.canvas(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(doc.items().size(), 2u);
    QTest::keyClick(studio.canvas(), Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(studio.canvas(), Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(doc.items().size(), 2u);
    QTest::keyClick(studio.canvas(), Qt::Key_Escape);
    EXPECT_EQ(doc.items().size(), 2u) << "Escape does nothing";
    QTest::keyClick(studio.canvas(), Qt::Key_Backspace);
    EXPECT_TRUE(doc.items().empty());
    EXPECT_FALSE(QFile::exists(sampleFile(QStringLiteral("0062"))));
}

// Some pens (e.g. the FydeTab Duo's) are tip-only, so every action is on a button too.
TEST_F(GlyphStudioTest, TheButtonsDoWhatTheKeysDo)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    const auto button = [&studio](const char *name) {
        auto *b = studio.findChild<QPushButton *>(QLatin1String(name));
        EXPECT_NE(b, nullptr) << name;
        return b;
    };
    stroke(studio, 1000);
    stroke(studio, 2000);
    button("glyphUndoButton")->click();
    EXPECT_EQ(studio.canvas()->document().items().size(), 1u);
    button("glyphRedoButton")->click();
    EXPECT_EQ(studio.canvas()->document().items().size(), 2u);
    button("glyphDiscardButton")->click();
    EXPECT_TRUE(studio.canvas()->document().items().empty());
    stroke(studio, 3000);
    button("glyphSaveButton")->click();
    EXPECT_TRUE(QFile::exists(sampleFile(QStringLiteral("0061"))));
    EXPECT_EQ(studio.currentTarget(), U'b');
    button("glyphSkipButton")->click();
    EXPECT_EQ(studio.currentTarget(), U'c') << "moving on without saving";
    EXPECT_FALSE(QFile::exists(sampleFile(QStringLiteral("0062"))));
}

// The teardown crash of InkView (a focus-out or a hide from ~QWidget finishing a stroke or committing a box
// into the half-destroyed owner) applies to the studio, which hosts an InkCanvas the same way.
TEST_F(GlyphStudioTest, DeletingTheStudioMidStrokeIsSafe)
{
    auto *studio = new noteAppC::hwfont::GlyphStudio(host, QStringLiteral("Test"));
    studio->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(studio));
    inktest::tablet(studio->canvas(), QEvent::TabletPress, {100, 100}, 0.5, 1000);
    inktest::tablet(studio->canvas(), QEvent::TabletMove, {140, 120}, 0.5, 1005);
    ASSERT_TRUE(studio->canvas()->isDrawing());
    delete studio;
    QTest::qWait(50);
    SUCCEED();
}

TEST_F(GlyphStudioTest, DeletingTheStudioWithATypedTextBoxOpenIsSafe)
{
    auto *studio = new noteAppC::hwfont::GlyphStudio(host, QStringLiteral("Test"));
    studio->show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(studio));
    QApplication::setActiveWindow(studio);
    studio->canvas()->setTool(noteAppC::ink::InkCanvas::Tool::Text);
    QTest::mouseClick(studio->canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(60, 60));
    auto *edit = studio->canvas()->findChild<QWidget *>(QStringLiteral("inkTextEditor"));
    ASSERT_NE(edit, nullptr);
    edit->setFocus();
    QTest::keyClicks(edit, QStringLiteral("typed"));
    delete studio;
    QTest::qWait(50);
    SUCCEED();
}

TEST_F(GlyphStudioTest, TheBarrelClickSavesAndNeverDraws)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    stroke(studio);
    ASSERT_EQ(studio.canvas()->document().items().size(), 1u);
    const QPointF p(150, 150);
    inktest::tablet(studio.canvas(), QEvent::TabletPress, p, 0.5, 5000, noteAppC::ink::kHeldLassoButton);
    inktest::tablet(studio.canvas(), QEvent::TabletRelease, p, 0.0, 5040, Qt::NoButton);
    EXPECT_TRUE(QFile::exists(sampleFile(QStringLiteral("0061")))) << "the click saved the cell";
    EXPECT_EQ(studio.currentTarget(), U'b');
    EXPECT_TRUE(studio.canvas()->document().items().empty()) << "and drew nothing";

    // Held and dragged more than 4 px: not a click, not a stroke, not a selection.
    stroke(studio, 6000);
    ASSERT_EQ(studio.canvas()->document().items().size(), 1u);
    inktest::tablet(studio.canvas(), QEvent::TabletPress, {100, 100}, 0.5, 7000, noteAppC::ink::kHeldLassoButton);
    for (int i = 1; i <= 8; ++i)
        inktest::tablet(studio.canvas(), QEvent::TabletMove, QPointF(100 + i * 15, 100 + i * 10), 0.5, 7000 + i * 5,
                        noteAppC::ink::kHeldLassoButton);
    inktest::tablet(studio.canvas(), QEvent::TabletRelease, {220, 180}, 0.0, 7050, Qt::NoButton);
    EXPECT_FALSE(QFile::exists(sampleFile(QStringLiteral("0062")))) << "a drag is not a click";
    EXPECT_EQ(studio.canvas()->document().items().size(), 1u) << "no new stroke, nothing erased";
    EXPECT_FALSE(studio.canvas()->isDrawing());
    stroke(studio, 8000);
    EXPECT_EQ(studio.canvas()->document().items().size(), 2u) << "the pen draws again afterwards";
}


// The cell is a drawing surface, so it draws with the mouse whatever ink.drawWithMouse says.
TEST_F(GlyphStudioTest, TheCellDrawsWithTheMouseWhateverTheInkSettingSays)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    ASSERT_FALSE(studio.isModified());
    QTest::mousePress(studio.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(100, 120));
    for (int i = 1; i <= 8; ++i)
        QTest::mouseMove(studio.canvas(), QPoint(100 + i * 15, 120 + i * 18));
    QTest::mouseRelease(studio.canvas(), Qt::LeftButton, Qt::NoModifier, QPoint(220, 264));
    EXPECT_EQ(studio.canvas()->document().items().size(), 1u);
    EXPECT_TRUE(studio.isModified());
}


// A first-time user starts another font from the studio (SettingRow has no free-text kind).
TEST_F(GlyphStudioTest, NewFontSwitchesTheStudioAndRefusesAnUnsafeName)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    auto *button = studio.findChild<QPushButton *>(QStringLiteral("glyphNewFontButton"));
    ASSERT_NE(button, nullptr);
    auto *message = studio.findChild<QLabel *>(QStringLiteral("glyphError"));
    ASSERT_NE(message, nullptr);

    std::optional<QString> answer;
    studio.setFontNameProvider([&answer] { return answer; });
    button->click();
    EXPECT_EQ(studio.fontName(), QStringLiteral("Test")) << "cancelled";

    answer = QStringLiteral("../bad");
    button->click();
    EXPECT_EQ(studio.fontName(), QStringLiteral("Test")) << "refused";
    EXPECT_TRUE(message->text().contains(QStringLiteral("../bad"))) << message->text().toStdString();

    answer = QStringLiteral("Second font");
    button->click();
    EXPECT_EQ(studio.fontName(), QStringLiteral("Second font"));
    EXPECT_TRUE(message->text().isEmpty()) << "the refusal is gone";
    stroke(studio);
    QTest::keyClick(studio.canvas(), Qt::Key_Return);
    EXPECT_TRUE(QFile::exists(sampleFile(QStringLiteral("0061"), 1, QStringLiteral("Second font"))));
    EXPECT_FALSE(QFile::exists(sampleFile(QStringLiteral("0061"))));
    ASSERT_NE(rowFor(studio, U'a'), nullptr);
    EXPECT_EQ(rowFor(studio, U'a')->text(), QStringLiteral("a 1/3"));
    EXPECT_EQ(rowFor(studio, U'z')->text(), QStringLiteral("z 0/3")) << "the list follows the new font";
}

// The studio is a file-less tab; the host saves it through save().
TEST_F(GlyphStudioTest, AnUnsavedCellIsModifiedAndSaveWritesIt)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("Test"));
    studio.show();
    QSignalSpy changed(&studio, &noteAppC::ModuleView::modificationChanged);
    EXPECT_FALSE(studio.isModified());
    QString error;
    EXPECT_TRUE(studio.save(&error)) << error.toStdString();
    EXPECT_FALSE(QDir(dir.filePath(QStringLiteral("Handwriting"))).exists()) << "an empty cell writes nothing";
    EXPECT_EQ(studio.currentTarget(), U'a');

    stroke(studio);
    EXPECT_TRUE(studio.isModified());
    ASSERT_FALSE(changed.isEmpty());
    EXPECT_EQ(changed.last().at(0).toBool(), true);
    EXPECT_TRUE(studio.save(&error)) << error.toStdString();
    EXPECT_TRUE(QFile::exists(sampleFile(QStringLiteral("0061"))));
    EXPECT_FALSE(studio.isModified());
    EXPECT_EQ(changed.last().at(0).toBool(), false);
}

TEST_F(GlyphStudioTest, AnUnsafeFontNameIsShownAndNothingIsWritten)
{
    noteAppC::hwfont::GlyphStudio studio(host, QStringLiteral("../escape"));
    studio.show();
    auto *message = studio.findChild<QLabel *>(QStringLiteral("glyphError"));
    ASSERT_NE(message, nullptr);
    EXPECT_FALSE(message->text().isEmpty());
    EXPECT_TRUE(message->text().contains(QStringLiteral("../escape")));
    stroke(studio);
    QString error;
    EXPECT_FALSE(studio.save(&error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(studio.isModified()) << "the stroke is kept";
    EXPECT_FALSE(QDir(dir.filePath(QStringLiteral("Handwriting"))).exists());
}

// --- the module, through the real window ----------------------------------------------

namespace {

class HwfontWindowTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(dir.isValid()); }
    static QAction *command(noteAppC::MainWindow &window, const char *id) { return window.command(QLatin1String(id)); }
    static QTabBar *mainTabs(noteAppC::MainWindow &window)
    {
        const auto bars = window.findChildren<QTabBar *>(QStringLiteral("noteTabs"));
        return bars.isEmpty() ? nullptr : bars.first();
    }
    QString firstSample() const { return dir.filePath(QStringLiteral("Handwriting/My handwriting/0061/1.ink")); }
    QTemporaryDir dir;
};

void closeBoxWith(QMessageBox *box, QMessageBox::StandardButton answer)
{
    if (QAbstractButton *button = box->button(answer))
        button->click();
    else
        box->close();
}

} // namespace

TEST_F(HwfontWindowTest, TheCommandOpensTheStudioTabAndSavesIntoTheVault)
{
    noteAppC::MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    ASSERT_NE(command(window, "handwriting-font:studio"), nullptr);
    ASSERT_TRUE(window.modules()->moduleForCommand(QStringLiteral("handwriting-font:studio")).has_value());
    command(window, "handwriting-font:studio")->trigger();
    WAIT_TRUE(window.findChild<QWidget *>(QStringLiteral("glyphStudio")), 5000);
    auto *canvas = window.findChild<noteAppC::ink::InkCanvas *>(QStringLiteral("glyphCanvas"));
    ASSERT_NE(canvas, nullptr);
    inktest::penStroke(canvas, {100, 120}, {220, 270}, 1000);
    QTest::keyClick(canvas, Qt::Key_Return);
    EXPECT_TRUE(QFile::exists(firstSample()));
    ASSERT_NE(mainTabs(window), nullptr);
    EXPECT_EQ(mainTabs(window)->tabText(mainTabs(window)->currentIndex()), QStringLiteral("Glyph studio"));
}


// The keys reach the cell without a click on it: the tab shown by the host takes the focus,
// and a button click (the buttons never take focus) leaves it there.
TEST_F(HwfontWindowTest, TheShownStudioHasTheKeysAndKeepsThemAfterAButtonClick)
{
    noteAppC::MainWindow window;
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&window));
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "handwriting-font:studio")->trigger();
    WAIT_TRUE(window.findChild<QWidget *>(QStringLiteral("glyphStudio")), 5000);
    auto *canvas = window.findChild<noteAppC::ink::InkCanvas *>(QStringLiteral("glyphCanvas"));
    ASSERT_NE(canvas, nullptr);
    WAIT_TRUE(QApplication::focusWidget() == canvas, 2000);

    inktest::penStroke(canvas, {100, 120}, {220, 270}, 1000);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    EXPECT_TRUE(QFile::exists(firstSample())) << "Enter saved without a click on the cell";

    auto *skip = window.findChild<QPushButton *>(QStringLiteral("glyphSkipButton"));
    ASSERT_NE(skip, nullptr);
    QTest::mouseClick(skip, Qt::LeftButton);
    EXPECT_TRUE(QApplication::focusWidget() == canvas) << "a button click leaves the keys with the cell";
    inktest::penStroke(canvas, {100, 120}, {220, 270}, 2000);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    EXPECT_TRUE(QFile::exists(dir.filePath(QStringLiteral("Handwriting/My handwriting/0063/1.ink")))) << "Skip moved from b to c, and Enter saved c";
}


TEST_F(HwfontWindowTest, NewFontWritesTheSettingAndSavesIntoTheNewFolder)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    auto restore = qScopeGuard([&previous] { qputenv("XDG_CONFIG_HOME", previous); });
    noteAppC::MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "handwriting-font:studio")->trigger();
    WAIT_TRUE(window.findChild<QWidget *>(QStringLiteral("glyphStudio")), 5000);
    auto *studio = window.findChild<noteAppC::hwfont::GlyphStudio *>(QStringLiteral("glyphStudio"));
    ASSERT_NE(studio, nullptr);
    studio->setFontNameProvider([] { return std::optional<QString>(QStringLiteral("Second font")); });
    window.findChild<QPushButton *>(QStringLiteral("glyphNewFontButton"))->click();
    EXPECT_EQ(window.settings().text(QStringLiteral("hwfont.fontName")), QStringLiteral("Second font"));
    EXPECT_EQ(noteAppC::Settings::load().text(QStringLiteral("hwfont.fontName")), QStringLiteral("Second font")) << "saved to disk";
    inktest::penStroke(studio->canvas(), {100, 120}, {220, 270}, 1000);
    studio->saveCurrentAndAdvance();
    EXPECT_TRUE(QFile::exists(dir.filePath(QStringLiteral("Handwriting/Second font/0061/1.ink"))));
}

// The tab and the window title both show the mark; closing saves the cell.
TEST_F(HwfontWindowTest, TheStudioTabShowsTheModifiedMarkAndCloseSavesTheCell)
{
    QTemporaryDir config;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    auto restore = qScopeGuard([&previous] { qputenv("XDG_CONFIG_HOME", previous); });
    noteAppC::Settings off = noteAppC::Settings::load();
    off.setValue(QStringLiteral("files.autosave"), false);
    ASSERT_TRUE(off.save());

    noteAppC::MainWindow window;
    window.show();
    window.openVault(dir.path());
    window.scanModulesNow();
    command(window, "handwriting-font:studio")->trigger();
    WAIT_TRUE(window.findChild<QWidget *>(QStringLiteral("glyphStudio")), 5000);
    auto *canvas = window.findChild<noteAppC::ink::InkCanvas *>(QStringLiteral("glyphCanvas"));
    ASSERT_NE(canvas, nullptr);
    QTabBar *tabs = mainTabs(window);
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(tabs->tabText(tabs->currentIndex()), QStringLiteral("Glyph studio"));
    EXPECT_FALSE(window.windowTitle().startsWith(QLatin1Char('*')));

    inktest::penStroke(canvas, {100, 120}, {220, 270}, 1000);
    EXPECT_EQ(tabs->tabText(tabs->currentIndex()), QStringLiteral("Glyph studio *"));
    EXPECT_TRUE(window.windowTitle().startsWith(QLatin1Char('*'))) << window.windowTitle().toStdString();

    QString question;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, [&question] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            question = box->text();
            closeBoxWith(box, QMessageBox::Save);
        }
    });
    watch.start(20);
    command(window, "workspace:close")->trigger();
    EXPECT_TRUE(question.contains(QStringLiteral("Glyph studio"))) << question.toStdString();
    EXPECT_TRUE(QFile::exists(firstSample())) << "Save reached the studio's save()";
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
