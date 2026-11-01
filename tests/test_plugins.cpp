#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "Bookmarks.h"
#include "DailyNotes.h"
#include "Metadata.h"
#include "MomentFormat.h"
#include "OutlinePane.h"
#include "Templates.h"
#include "Vault.h"

using namespace noteAppC;

namespace {

QByteArray readAll(const QString &path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

void write(const QString &path, const QByteArray &bytes)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

} // namespace

// --- MomentFormat -----------------------------------------------------------
// 2024-01-01 is a Monday and the start of ISO week 1 of 2024 - a fixed point
// that does not need recomputing by hand for every token.

TEST(MomentFormat, DatePartsWithLeadingZeros)
{
    const QDateTime when(QDate(2024, 1, 1), QTime(0, 0, 0));
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("YYYY-MM-DD")), QStringLiteral("2024-01-01"));
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("YY-M-D")), QStringLiteral("24-1-1"));
}

TEST(MomentFormat, MonthAndDayNamesAreEnglish)
{
    const QDateTime when(QDate(2024, 1, 1), QTime(0, 0, 0));
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("MMMM MMM")), QStringLiteral("January Jan"));
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("dddd ddd")), QStringLiteral("Monday Mon"));
}

TEST(MomentFormat, OrdinalDaySuffix)
{
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 1), {}), QStringLiteral("Do")),
             QStringLiteral("1st"));
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 2), {}), QStringLiteral("Do")),
             QStringLiteral("2nd"));
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 3), {}), QStringLiteral("Do")),
             QStringLiteral("3rd"));
    // The 11th-13th break the usual 1/2/3 -> st/nd/rd pattern.
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 11), {}), QStringLiteral("Do")),
             QStringLiteral("11th"));
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 23), {}), QStringLiteral("Do")),
             QStringLiteral("23rd"));
}

TEST(MomentFormat, IsoWeekTokens)
{
    const QDateTime when(QDate(2024, 1, 1), {});
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("E")), QStringLiteral("1"));
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("W")), QStringLiteral("1"));
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("WW")), QStringLiteral("01"));
    EXPECT_EQ(MomentFormat::format(when, QStringLiteral("gggg")), QStringLiteral("2024"));
}

TEST(MomentFormat, TimeTokens)
{
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 1), QTime(0, 5, 9)),
                                   QStringLiteral("HH:mm:ss A")),
             QStringLiteral("00:05:09 AM"));
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 1), QTime(13, 30, 0)),
                                   QStringLiteral("HH:mm A")),
             QStringLiteral("13:30 PM"));
}

TEST(MomentFormat, ShortAndTwelveHourTimeTokens)
{
    const auto at = [](int h, int m, int sec) { return QDateTime(QDate(2024, 1, 1), QTime(h, m, sec)); };
    EXPECT_EQ(MomentFormat::format(at(13, 5, 7), QStringLiteral("h:mm:s a")), QStringLiteral("1:05:7 pm"));
    EXPECT_EQ(MomentFormat::format(at(0, 5, 0), QStringLiteral("h hh")), QStringLiteral("12 12"));
    EXPECT_EQ(MomentFormat::format(at(12, 0, 0), QStringLiteral("hh A")), QStringLiteral("12 PM"));
    EXPECT_EQ(MomentFormat::format(at(9, 0, 0), QStringLiteral("H:m")), QStringLiteral("9:0"));
    EXPECT_EQ(MomentFormat::format(at(9, 3, 0), QStringLiteral("hh:mm a")), QStringLiteral("09:03 am"));
}

TEST(MomentFormat, LiteralBracketsPassThrough)
{
    EXPECT_EQ(MomentFormat::format(QDateTime(QDate(2024, 1, 1), {}), QStringLiteral("[Week of] YYYY")),
             QStringLiteral("Week of 2024"));
}

// --- Templates ---------------------------------------------------------------

TEST(Templates, RenderTextSubstitutesEveryTag)
{
    const QString content = QStringLiteral(
        "Title: {{title}}\nDate: {{date}}\nCustom: {{date:YYYY}}\nTime: {{time:HH:mm}}\n"
        "Plain {{unknown}} stays.");
    const QDateTime when(QDate(2026, 9, 23), QTime(14, 30, 0));

    const QString rendered = Templates::renderText(content, QStringLiteral("My Note"), when,
                                                    QStringLiteral("YYYY-MM-DD"), QStringLiteral("HH:mm"));

    EXPECT_EQ(rendered, QStringLiteral("Title: My Note\nDate: 2026-09-23\nCustom: 2026\nTime: 14:30\n"
                                       "Plain {{unknown}} stays."));
}

TEST(Templates, ListAndRenderFromAFolder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString vaultRoot = dir.path();
    write(QDir(vaultRoot).filePath(QStringLiteral("Templates/Daily.md")),
         "# {{title}}\n\n{{date:YYYY-MM-DD}}\n");
    write(QDir(vaultRoot).filePath(QStringLiteral(".obsidian/templates.json")),
         R"({"folder":"Templates","dateFormat":"YYYY-MM-DD","timeFormat":"HH:mm"})");

    const Templates::Config config = Templates::readConfig(vaultRoot);
    EXPECT_EQ(config.folder, QStringLiteral("Templates"));

    Templates templates(vaultRoot, config);
    EXPECT_EQ(templates.list(), QStringList{QStringLiteral("Daily")});

    QString rendered;
    QString error;
    ASSERT_TRUE(templates.render(QStringLiteral("Daily"), QStringLiteral("Today"), &rendered, &error))
        << error.toStdString();
    EXPECT_EQ(rendered.left(8), QStringLiteral("# Today\n"));
}

TEST(Templates, MissingFolderDoesNotCreateOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Templates templates(dir.path(), Templates::Config{QStringLiteral("Nope"), {}, {}});
    EXPECT_TRUE(templates.list().isEmpty());
    EXPECT_FALSE(QDir(dir.path()).exists(QStringLiteral("Nope")));
}

// --- DailyNotes ---------------------------------------------------------------

class DailyNotesTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(dir.isValid()); }
    QTemporaryDir dir;
};

TEST_F(DailyNotesTest, DefaultsMatchObsidianWhenNoConfigFile)
{
    const DailyNotes::Config config = DailyNotes::readConfig(dir.path());
    EXPECT_TRUE(config.folder.isEmpty());
    EXPECT_EQ(config.format, QStringLiteral("YYYY-MM-DD"));
    EXPECT_TRUE(config.templatePath.isEmpty());
}

TEST_F(DailyNotesTest, ReadsObsidianConfigWhenPresent)
{
    write(QDir(dir.path()).filePath(QStringLiteral(".obsidian/daily-notes.json")),
         R"({"folder":"Daily","format":"YYYY/MM/DD","template":"Templates/Daily"})");
    const DailyNotes::Config config = DailyNotes::readConfig(dir.path());
    EXPECT_EQ(config.folder, QStringLiteral("Daily"));
    EXPECT_EQ(config.format, QStringLiteral("YYYY/MM/DD"));
    EXPECT_EQ(config.templatePath, QStringLiteral("Templates/Daily"));
}

TEST_F(DailyNotesTest, PathForUsesFolderAndFormat)
{
    DailyNotes notes(dir.path(), DailyNotes::Config{QStringLiteral("Daily"), QStringLiteral("YYYY-MM-DD"), {}});
    const QString expected = QDir(dir.path()).filePath(QStringLiteral("Daily/2026-09-23.md"));
    EXPECT_EQ(notes.pathFor(QDate(2026, 9, 23)), expected);
}

TEST_F(DailyNotesTest, EnsureNoteCreatesOnceAndNeverOverwrites)
{
    DailyNotes notes(dir.path(), DailyNotes::Config{});
    QString error;
    const QString path = notes.ensureNote(QDate(2026, 9, 23), &error);
    ASSERT_FALSE(path.isEmpty()) << error.toStdString();
    ASSERT_TRUE(QFileInfo::exists(path));

    write(path, "the user's own words\n");
    const QString again = notes.ensureNote(QDate(2026, 9, 23), &error);
    EXPECT_EQ(again, path);
    EXPECT_EQ(readAll(path), QByteArray("the user's own words\n"));
}

TEST_F(DailyNotesTest, EnsureNoteRendersItsTemplate)
{
    write(QDir(dir.path()).filePath(QStringLiteral("Templates/Daily.md")),
         "# {{title}}\n\nCreated {{date:YYYY-MM-DD}}\n");
    DailyNotes notes(dir.path(),
                     DailyNotes::Config{QString(), QStringLiteral("YYYY-MM-DD"), QStringLiteral("Templates/Daily")});

    QString error;
    const QString path = notes.ensureNote(QDate(2026, 9, 23), &error);
    ASSERT_FALSE(path.isEmpty()) << error.toStdString();
    EXPECT_EQ(readAll(path), QByteArray("# 2026-09-23\n\nCreated 2026-09-23\n"));
}

TEST_F(DailyNotesTest, PreviousAndNextWalkCalendarDays)
{
    DailyNotes notes(dir.path(), DailyNotes::Config{});
    const QDate today(2026, 9, 23);
    const QString earlier = notes.pathFor(today.addDays(-3));
    const QString later = notes.pathFor(today.addDays(5));
    write(earlier, "\n");
    write(later, "\n");

    EXPECT_EQ(notes.previousNote(today), earlier);
    EXPECT_EQ(notes.nextNote(today), later);
    EXPECT_TRUE(notes.previousNote(today.addDays(-3)).isEmpty());
}

// The folder, the format and the template come from the vault's own settings, and a vault is what an
// attacker hands over: none of them may reach a file outside it.
TEST_F(DailyNotesTest, FolderOrFormatThatLeavesTheVaultStaysInside)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    const QString vaultRoot = QDir(dir.path()).filePath(QStringLiteral("vault"));
    ASSERT_TRUE(QDir().mkpath(vaultRoot));
    const QDate date(2026, 9, 23);

    for (const QString &folder : {QStringLiteral("../outside"), QStringLiteral("a/../../outside"), outside.path()}) {
        DailyNotes notes(vaultRoot, DailyNotes::Config{folder, QStringLiteral("YYYY-MM-DD"), {}});
        const QString path = QDir::cleanPath(notes.pathFor(date));
        EXPECT_TRUE(path.startsWith(vaultRoot + QLatin1Char('/'))) << folder.toStdString() << " -> " << path.toStdString();
    }
    DailyNotes byFormat(vaultRoot, DailyNotes::Config{{}, QStringLiteral("[../../escape-]YYYY"), {}});
    EXPECT_TRUE(QDir::cleanPath(byFormat.pathFor(date)).startsWith(vaultRoot + QLatin1Char('/')));

    QString error;
    DailyNotes notes(vaultRoot, DailyNotes::Config{QStringLiteral("../outside"), QStringLiteral("YYYY-MM-DD"), {}});
    ASSERT_FALSE(notes.ensureNote(date, &error).isEmpty()) << error.toStdString();
    EXPECT_FALSE(QDir(dir.path()).exists(QStringLiteral("outside")));
}

TEST_F(DailyNotesTest, FolderThroughALinkOutOfTheVaultIsRefused)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    const QString vaultRoot = QDir(dir.path()).filePath(QStringLiteral("vault"));
    ASSERT_TRUE(QDir().mkpath(vaultRoot));
    ASSERT_TRUE(QFile::link(outside.path(), QDir(vaultRoot).filePath(QStringLiteral("out"))));

    DailyNotes notes(vaultRoot, DailyNotes::Config{QStringLiteral("out/sub"), QStringLiteral("YYYY-MM-DD"), {}});
    QString error;
    EXPECT_TRUE(notes.ensureNote(QDate(2026, 9, 23), &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QDir(outside.path()).isEmpty());
}

TEST_F(DailyNotesTest, TemplateOutsideTheVaultIsNotRead)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    write(QDir(outside.path()).filePath(QStringLiteral("secret.md")), "outside words\n");
    const QString vaultRoot = QDir(dir.path()).filePath(QStringLiteral("vault"));
    ASSERT_TRUE(QDir().mkpath(vaultRoot));

    const QString templateName = QDir(dir.path()).relativeFilePath(QDir(outside.path()).filePath(QStringLiteral("secret")));
    for (const QString &templatePath : {QStringLiteral("../") + templateName, QDir(outside.path()).filePath(QStringLiteral("secret"))}) {
        DailyNotes notes(vaultRoot, DailyNotes::Config{{}, QStringLiteral("YYYY-MM-DD"), templatePath});
        QString error;
        const QString path = notes.ensureNote(QDate(2026, 9, 23), &error);
        ASSERT_FALSE(path.isEmpty()) << error.toStdString();
        EXPECT_EQ(readAll(path), QByteArray()) << templatePath.toStdString();
        QFile::remove(path);
    }
}

TEST(TemplatesFolder, FolderThatLeavesTheVaultListsTheVaultInstead)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    write(QDir(outside.path()).filePath(QStringLiteral("elsewhere.md")), "x\n");
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    write(QDir(vault.path()).filePath(QStringLiteral("mine.md")), "x\n");

    Templates templates(vault.path(), Templates::Config{QDir(vault.path()).relativeFilePath(outside.path()), {}, {}});
    EXPECT_EQ(templates.list(), QStringList{QStringLiteral("mine")});
}

// --- Bookmarks -----------------------------------------------------------

class BookmarksTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(dir.isValid()); }

    QString obsidianBookmarksPath() const
    {
        return QDir(dir.path()).filePath(QStringLiteral(".obsidian/bookmarks.json"));
    }

    QTemporaryDir dir;
};

TEST_F(BookmarksTest, ImportsObsidianShapeWithoutWritingTheVault)
{
    const QByteArray original = R"({
        "items": [
            {"type": "file", "path": "Note.md"},
            {"type": "file", "path": "Note.md", "subpath": "#Heading"},
            {"type": "search", "query": "tag:#project", "title": "Projects"},
            {"type": "group", "title": "Group A", "items": [
                {"type": "file", "path": "Other.md"}
            ]}
        ]
    })";
    write(obsidianBookmarksPath(), original);

    Bookmarks bookmarks(dir.path());
    bookmarks.load();
    const QList<BookmarkItem> &items = bookmarks.items();
    ASSERT_EQ(items.size(), 4);
    EXPECT_EQ(items.at(0).type, BookmarkItem::Type::File);
    EXPECT_EQ(items.at(0).path, QStringLiteral("Note.md"));
    EXPECT_FALSE(items.at(0).isHeading());
    EXPECT_TRUE(items.at(1).isHeading());
    EXPECT_EQ(items.at(1).subpath, QStringLiteral("#Heading"));
    EXPECT_EQ(items.at(2).type, BookmarkItem::Type::Search);
    EXPECT_EQ(items.at(2).query, QStringLiteral("tag:#project"));
    ASSERT_EQ(items.at(3).type, BookmarkItem::Type::Group);
    ASSERT_EQ(items.at(3).items.size(), 1);
    EXPECT_EQ(items.at(3).items.at(0).path, QStringLiteral("Other.md"));

    QString error;
    ASSERT_TRUE(bookmarks.save(&error)) << error.toStdString();

    // The vault's own copy is never touched - byte for byte, not just "still valid JSON".
    EXPECT_EQ(readAll(obsidianBookmarksPath()), original);
    // noteAppC kept its own copy instead, outside the vault.
    const QString ownPath = Bookmarks::storagePathFor(dir.path());
    EXPECT_FALSE(ownPath.startsWith(dir.path()));
    EXPECT_TRUE(QFileInfo::exists(ownPath));
}

TEST_F(BookmarksTest, OwnCopyIsAuthoritativeOnceSaved)
{
    write(obsidianBookmarksPath(), R"({"items":[{"type":"file","path":"First.md"}]})");

    Bookmarks first(dir.path());
    first.load();
    ASSERT_EQ(first.items().size(), 1);
    ASSERT_TRUE(first.save());

    // The vault's file changes afterwards - noteAppC must not go back to it.
    write(obsidianBookmarksPath(), R"({"items":[{"type":"file","path":"Second.md"},
                                              {"type":"file","path":"Third.md"}]})");

    Bookmarks second(dir.path());
    second.load();
    ASSERT_EQ(second.items().size(), 1);
    EXPECT_EQ(second.items().at(0).path, QStringLiteral("First.md"));
}

TEST_F(BookmarksTest, AddRemoveAndRenameGroup)
{
    Bookmarks bookmarks(dir.path());
    bookmarks.load(); // nothing to import; starts empty

    const QString groupId = bookmarks.addGroup(QStringLiteral("Reading"));
    const QString noteId = bookmarks.addNote(QStringLiteral("Note.md"), {}, QStringLiteral("Reading"));
    ASSERT_EQ(bookmarks.items().size(), 1);
    ASSERT_EQ(bookmarks.items().at(0).items.size(), 1);
    EXPECT_EQ(bookmarks.items().at(0).items.at(0).id, noteId);

    ASSERT_TRUE(bookmarks.renameGroup(groupId, QStringLiteral("To read")));
    EXPECT_EQ(bookmarks.find(groupId)->title, QStringLiteral("To read"));

    ASSERT_TRUE(bookmarks.remove(noteId));
    EXPECT_TRUE(bookmarks.items().at(0).items.isEmpty());
    EXPECT_EQ(bookmarks.find(noteId), nullptr);
}

// --- OutlinePane ---------------------------------------------------------

namespace {

Heading makeHeading(int level, const QString &text, qsizetype start, qsizetype end)
{
    Heading heading;
    heading.level = level;
    heading.text = text;
    heading.range = Range{start, end};
    return heading;
}

} // namespace

TEST(OutlinePaneTest, BuildsNestedTreeByLevel)
{
    OutlinePane pane;
    const QList<Heading> headings = {
        makeHeading(1, QStringLiteral("Intro"), 0, 8),
        makeHeading(2, QStringLiteral("Background"), 20, 33),
        makeHeading(1, QStringLiteral("Conclusion"), 60, 73),
    };
    pane.setHeadings(headings);

    auto *tree = pane.findChild<QTreeWidget *>(QStringLiteral("outlineTree"));
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("Intro"));
    ASSERT_EQ(tree->topLevelItem(0)->childCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->child(0)->text(0), QStringLiteral("Background"));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), QStringLiteral("Conclusion"));
}

TEST(OutlinePaneTest, SameHeadingsSkipRebuildSoCollapseSurvives)
{
    OutlinePane pane;
    QList<Heading> headings = {
        makeHeading(1, QStringLiteral("Intro"), 0, 8),
        makeHeading(2, QStringLiteral("Background"), 20, 33),
    };
    pane.setHeadings(headings);

    auto *tree = pane.findChild<QTreeWidget *>(QStringLiteral("outlineTree"));
    ASSERT_NE(tree, nullptr);
    tree->topLevelItem(0)->setExpanded(false);

    // The exact same headings again - typing prose reparses without adding
    // or removing a single one - must not touch the tree.
    pane.setHeadings(headings);
    EXPECT_FALSE(tree->topLevelItem(0)->isExpanded());

    // An actual change (a heading's text moved) does rebuild, which is
    // allowed to reset collapse state.
    headings[0].text = QStringLiteral("Changed");
    pane.setHeadings(headings);
    EXPECT_TRUE(tree->topLevelItem(0)->isExpanded());
}

TEST(OutlinePaneTest, HighlightsTheHeadingTheCaretIsIn)
{
    OutlinePane pane;
    const QList<Heading> headings = {
        makeHeading(1, QStringLiteral("Intro"), 0, 8),
        makeHeading(1, QStringLiteral("Conclusion"), 60, 73),
    };
    pane.setHeadings(headings);
    auto *tree = pane.findChild<QTreeWidget *>(QStringLiteral("outlineTree"));
    ASSERT_NE(tree, nullptr);

    pane.setCursorOffset(30); // inside "Intro", before "Conclusion" starts
    EXPECT_TRUE(tree->topLevelItem(0)->font(0).bold());
    EXPECT_FALSE(tree->topLevelItem(1)->font(0).bold());

    pane.setCursorOffset(65); // inside "Conclusion"
    EXPECT_FALSE(tree->topLevelItem(0)->font(0).bold());
    EXPECT_TRUE(tree->topLevelItem(1)->font(0).bold());
}

TEST(OutlinePaneTest, ClickingAHeadingEmitsItsSourceOffset)
{
    OutlinePane pane;
    pane.resize(300, 200);
    pane.show();
    const QList<Heading> headings = {makeHeading(1, QStringLiteral("Intro"), 42, 50)};
    pane.setHeadings(headings);

    auto *tree = pane.findChild<QTreeWidget *>(QStringLiteral("outlineTree"));
    ASSERT_NE(tree, nullptr);
    QTreeWidgetItem *item = tree->topLevelItem(0);
    ASSERT_NE(item, nullptr);

    QSignalSpy spy(&pane, &OutlinePane::jumpRequested);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(item).center());
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toLongLong(), 42);
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
