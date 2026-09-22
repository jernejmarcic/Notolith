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
