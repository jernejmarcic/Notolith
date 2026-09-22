#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "Fuzzy.h"
#include "Index.h"
#include "Indexer.h"
#include "Parser.h"
#include "Vault.h"

using namespace noteAppC;

namespace {

class IndexTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Projects")));
        ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));

        write(QStringLiteral("Home.md"),
              "# Home\n\nSee [[Projects/noteAppC]] and [[Ideas|my ideas]].\n\n#home\n");
        write(QStringLiteral("Ideas.md"),
              "---\naliases: [Brainwaves]\nid: 01J9\ntags: [thinking]\n---\n\n"
              "Back to [[Home]]. #thinking/deep\n");
        write(QStringLiteral("Projects/noteAppC.md"),
              "# noteAppC\n\nA link to [[Home]] and an embed ![[Ideas]].\n");
        write(QStringLiteral(".obsidian/app.json"), "{}\n");
        write(QStringLiteral("Projects/notes.txt"), "not a note\n");

        vault = Vault(dir.path());
    }

    void write(const QString &relative, const QByteArray &bytes)
    {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

    /// Runs a full scan and waits for it.
    void build(Index &index)
    {
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }

    QTemporaryDir dir;
    Vault vault;
};

} // namespace

TEST_F(IndexTest, IgnoresDotDirectoriesAndNonNotes)
{
    EXPECT_TRUE(Indexer::isIgnored(QStringLiteral(".obsidian/app.json")));
    EXPECT_TRUE(Indexer::isIgnored(QStringLiteral(".trash/Old.md")));
    EXPECT_FALSE(Indexer::isIgnored(QStringLiteral("Projects/noteAppC.md")));

    Index index;
    build(index);
    EXPECT_EQ(index.noteCount(), 3);
    // The .txt is indexed as an attachment so ![[notes.txt]] resolves, but it
    // is not a note and the dot directory is skipped entirely.
    EXPECT_EQ(index.attachmentCount(), 1);
    EXPECT_EQ(index.resolve(QStringLiteral("notes.txt")),
              QStringLiteral("Projects/notes.txt"));
    EXPECT_FALSE(index.findByName(QStringLiteral("notes.txt"))
                     .contains(QStringLiteral("Projects/notes.txt")));
    EXPECT_EQ(index.allPaths(), QStringList({QStringLiteral("Home.md"),
                                             QStringLiteral("Ideas.md"),
                                             QStringLiteral("Projects/noteAppC.md")}));
}

TEST_F(IndexTest, ResolvesNamesPathsAliasesAndIds)
{
    Index index;
    build(index);

    EXPECT_EQ(index.resolve(QStringLiteral("Home")), QStringLiteral("Home.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("Home.md")), QStringLiteral("Home.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("home")), QStringLiteral("Home.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("Projects/noteAppC")),
              QStringLiteral("Projects/noteAppC.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("noteAppC")), QStringLiteral("Projects/noteAppC.md"));
    // An alias and a frontmatter id both resolve to the note that declares them.
    EXPECT_EQ(index.resolve(QStringLiteral("Brainwaves")), QStringLiteral("Ideas.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("01J9")), QStringLiteral("Ideas.md"));
    EXPECT_TRUE(index.resolve(QStringLiteral("Nothing here")).isEmpty());
}

TEST_F(IndexTest, BacklinksFollowAliasesAndEmbeds)
{
    Index index;
    build(index);

    QStringList sources;
    for (const Backlink &backlink : index.backlinks(vault.absolute(QStringLiteral("Home.md"))))
        sources << backlink.fromPath;
    sources.sort();
    EXPECT_EQ(sources, QStringList({QStringLiteral("Ideas.md"),
                                    QStringLiteral("Projects/noteAppC.md")}));

    // [[Ideas|my ideas]] and ![[Ideas]] both count.
    const auto ideas = index.backlinks(vault.absolute(QStringLiteral("Ideas.md")));
    EXPECT_EQ(ideas.size(), 2);
    bool sawEmbed = false;
    for (const Backlink &backlink : ideas)
        sawEmbed = sawEmbed || backlink.link.embed;
    EXPECT_TRUE(sawEmbed);
}

TEST_F(IndexTest, OutgoingLinksKeepOrderAndParts)
{
    Index index;
    build(index);

    const auto links = index.outgoingLinks(vault.absolute(QStringLiteral("Home.md")));
    ASSERT_EQ(links.size(), 2);
    EXPECT_EQ(links.at(0).target, QStringLiteral("Projects/noteAppC"));
    EXPECT_EQ(links.at(1).target, QStringLiteral("Ideas"));
    EXPECT_EQ(links.at(1).alias, QStringLiteral("my ideas"));
}

TEST_F(IndexTest, TagsIncludeFrontmatterAndNesting)
{
    Index index;
    build(index);

    EXPECT_EQ(index.allTags(), QStringList({QStringLiteral("home"),
                                            QStringLiteral("thinking"),
                                            QStringLiteral("thinking/deep")}));
    // A parent tag matches its children, as in Obsidian.
    EXPECT_EQ(index.notesWithTag(QStringLiteral("thinking")),
              QStringList({QStringLiteral("Ideas.md")}));
    EXPECT_TRUE(index.notesWithTag(QStringLiteral("nothing")).isEmpty());
}

TEST_F(IndexTest, QuickSwitcherFindsByAnyName)
{
    Index index;
    build(index);

    EXPECT_TRUE(index.findByName(QStringLiteral("note")).contains(
        QStringLiteral("Projects/noteAppC.md")));
    EXPECT_TRUE(index.findByName(QStringLiteral("brain")).contains(QStringLiteral("Ideas.md")));
    EXPECT_TRUE(index.linkTargets().contains(QStringLiteral("Brainwaves")));
}

TEST_F(IndexTest, RescanSkipsUnchangedAndPicksUpEdits)
{
    Index index;
    build(index);
    EXPECT_FALSE(index.isCurrent(vault.absolute(QStringLiteral("Home.md")), 0, 0));
    EXPECT_TRUE(index.isCurrent(
        vault.absolute(QStringLiteral("Home.md")),
        QFileInfo(vault.absolute(QStringLiteral("Home.md"))).lastModified().toMSecsSinceEpoch(),
        QFileInfo(vault.absolute(QStringLiteral("Home.md"))).size()));

    // A second pass has nothing to do.
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    EXPECT_EQ(done.last().at(0).toInt(), 0);

    write(QStringLiteral("Fresh.md"), "# Fresh\n\n[[Home]]\n");
    QSignalSpy again(&indexer, &Indexer::finished);
    indexer.rebuild();
    ASSERT_TRUE(again.wait(20000));
    EXPECT_EQ(index.noteCount(), 4);
    EXPECT_EQ(index.backlinks(vault.absolute(QStringLiteral("Home.md"))).size(), 3);
}

TEST_F(IndexTest, DeletedNotesLeaveTheCache)
{
    Index index;
    build(index);
    ASSERT_TRUE(QFile::remove(vault.absolute(QStringLiteral("Ideas.md"))));

    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));

    EXPECT_EQ(index.noteCount(), 2);
    EXPECT_TRUE(index.resolve(QStringLiteral("Brainwaves")).isEmpty());
}

namespace {

/// A separate vault per test: the rename cases need hand-picked notes rather
/// than IndexTest's fixed fixture.
class RenameLinksTest : public ::testing::Test {
protected:
    void write(const QString &relative, const QByteArray &bytes)
    {
        QFileInfo info(dir.filePath(relative));
        ASSERT_TRUE(QDir().mkpath(info.absolutePath()));
        QFile file(info.absoluteFilePath());
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

    QByteArray read(const QString &absolutePath)
    {
        QFile file(absolutePath);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << absolutePath.toStdString();
        return file.readAll();
    }

    void build(Index &index)
    {
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }

    QTemporaryDir dir;
    Vault vault;
};

} // namespace

TEST_F(RenameLinksTest, PreservesAliasSubpathEmbedAndSkipsCodeFences)
{
    write(QStringLiteral("Old Note.md"), "# Old Note\n\nSome content.\n");
    write(QStringLiteral("Home.md"),
          "# Home\n\n"
          "Alias: [[Old Note|shown]].\n"
          "Subpath: [[Old Note#Heading]].\n"
          "Embed: ![[Old Note]].\n"
          "Two: [[Old Note]] and [[Old Note]] again.\n\n"
          "```\n[[Old Note]]\n```\n");
    write(QStringLiteral("MdLink.md"), "[Link](Old%20Note.md)\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Old Note.md")),
                                 QStringLiteral("New Note"), &newPath, &error))
        << error.toStdString();
    ASSERT_EQ(newPath, vault.absolute(QStringLiteral("New Note.md")));

    const int rewritten = index.updateLinksAfterRename(QStringLiteral("Old Note.md"),
                                                        QStringLiteral("New Note.md"),
                                                        vault, &error);
    EXPECT_EQ(rewritten, 2) << error.toStdString(); // Home.md and MdLink.md

    const QByteArray home = read(vault.absolute(QStringLiteral("Home.md")));
    EXPECT_EQ(home,
              QByteArray("# Home\n\n"
                        "Alias: [[New Note|shown]].\n"
                        "Subpath: [[New Note#Heading]].\n"
                        "Embed: ![[New Note]].\n"
                        "Two: [[New Note]] and [[New Note]] again.\n\n"
                        "```\n[[Old Note]]\n```\n"));

    const QByteArray mdLink = read(vault.absolute(QStringLiteral("MdLink.md")));
    EXPECT_EQ(mdLink, QByteArray("[Link](New%20Note.md)\n"));

    // Re-indexed under the new name.
    EXPECT_EQ(index.resolve(QStringLiteral("New Note")), QStringLiteral("New Note.md"));
    QStringList sources;
    for (const Backlink &backlink : index.backlinks(vault.absolute(QStringLiteral("New Note.md"))))
        sources << backlink.fromPath;
    sources.sort();
    EXPECT_EQ(sources, QStringList({QStringLiteral("Home.md"), QStringLiteral("Home.md"),
                                    QStringLiteral("Home.md"), QStringLiteral("Home.md"),
                                    QStringLiteral("Home.md"), QStringLiteral("MdLink.md")}));
}

TEST_F(RenameLinksTest, FallsBackToAFullPathWhenTheNewBasenameIsAmbiguous)
{
    write(QStringLiteral("Folder/Solo.md"), "# Solo\n");
    write(QStringLiteral("Other/Duplicate.md"), "# Duplicate\n");
    write(QStringLiteral("Ref.md"), "[[Solo]]\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Folder/Solo.md")),
                                 QStringLiteral("Duplicate"), &newPath, &error))
        << error.toStdString();

    const int rewritten = index.updateLinksAfterRename(QStringLiteral("Folder/Solo.md"),
                                                        QStringLiteral("Folder/Duplicate.md"),
                                                        vault, &error);
    EXPECT_EQ(rewritten, 1) << error.toStdString();

    // "Duplicate" alone is now ambiguous with Other/Duplicate.md, so the link
    // becomes a full path instead of staying a bare basename.
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Ref.md"))),
              QByteArray("[[Folder/Duplicate]]\n"));
}

TEST(IndexerIgnoreFilters, SkipsPathsMatchingAConfiguredFilter)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("vendor")));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));

    const auto write = [&](const QString &relative, const QByteArray &bytes) {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    };
    write(QStringLiteral("Home.md"), "# Home\n");
    write(QStringLiteral("vendor/Thing.md"), "# Thing\n");
    write(QStringLiteral(".obsidian/app.json"), R"({"userIgnoreFilters": ["vendor/"]})");

    Vault vault(dir.path()); // reads the filter at construction
    Index index;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));

    EXPECT_EQ(index.noteCount(), 1);
    EXPECT_EQ(index.allPaths(), QStringList({QStringLiteral("Home.md")}));
}

TEST(FuzzyTest, SubsequenceMatchingAndRanking)
{
    EXPECT_TRUE(Fuzzy::matches(QStringLiteral("nac"), QStringLiteral("noteAppC")));
    EXPECT_TRUE(Fuzzy::matches(QStringLiteral(""), QStringLiteral("anything")));
    EXPECT_FALSE(Fuzzy::matches(QStringLiteral("zz"), QStringLiteral("noteAppC")));
    // Case insensitive both ways.
    EXPECT_TRUE(Fuzzy::matches(QStringLiteral("HOME"), QStringLiteral("home.md")));

    const auto exact = Fuzzy::score(QStringLiteral("home"), QStringLiteral("Home"));
    const auto longer = Fuzzy::score(QStringLiteral("home"), QStringLiteral("Homework notes"));
    const auto scattered = Fuzzy::score(QStringLiteral("home"), QStringLiteral("h o m e"));
    EXPECT_GT(exact.score, longer.score);
    EXPECT_GT(longer.score, scattered.score);

    // Word starts beat mid-word hits: "pn" should prefer "Projects/noteAppC".
    const auto boundary = Fuzzy::score(QStringLiteral("pn"), QStringLiteral("Projects/noteAppC"));
    const auto midWord = Fuzzy::score(QStringLiteral("pn"), QStringLiteral("apron"));
    EXPECT_GT(boundary.score, midWord.score);

    EXPECT_EQ(Fuzzy::score(QStringLiteral("ac"), QStringLiteral("abc")).positions,
              QList<qsizetype>({0, 2}));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Keep the cache out of the real ~/.cache.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// The full-text table is keyed by the file's id. Nothing reads it until the
// search grammar lands, so these guard it: a stale or duplicated row would be
// invisible until Phase 5 and then look like a parser bug.
TEST_F(IndexTest, ReindexingLeavesOneContentRowPerFile)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));

    build(index);
    const int files = index.noteCount() + index.attachmentCount();
    ASSERT_GT(files, 0);
    EXPECT_EQ(index.contentRowCount(), index.noteCount());

    // A second and third pass must not accumulate rows.
    build(index);
    build(index);
    EXPECT_EQ(index.noteCount() + index.attachmentCount(), files);
    EXPECT_EQ(index.contentRowCount(), index.noteCount());
}

TEST_F(IndexTest, RemovingANoteTakesItsContentWithIt)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const int notes = index.noteCount();
    ASSERT_EQ(index.contentRowCount(), notes);

    index.remove(dir.filePath(QStringLiteral("Home.md")));
    EXPECT_EQ(index.noteCount(), notes - 1);
    EXPECT_EQ(index.contentRowCount(), notes - 1);
}
