#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <thread>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

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
TEST_F(IndexTest, ACacheFromAnOlderSchemaIsRebuiltNotTrusted)
{
    // What a machine still holds from before links gained `tname`: version 2,
    // the old table, rows the new queries cannot read.
    const QString cache = Index::cachePathFor(dir.path());
    ASSERT_TRUE(QDir().mkpath(QFileInfo(cache).absolutePath()));
    {
        QSqlDatabase old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("old-cache"));
        old.setDatabaseName(cache);
        ASSERT_TRUE(old.open());
        QSqlQuery query(old);
        ASSERT_TRUE(query.exec(QStringLiteral("CREATE TABLE files (id INTEGER PRIMARY KEY, path TEXT UNIQUE, "
                                              "mtime INTEGER, size INTEGER, hash BLOB, words INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral("CREATE TABLE links (file INTEGER, target TEXT, subpath TEXT, "
                                              "alias TEXT, embed INTEGER, wiki INTEGER, external INTEGER, "
                                              "start INTEGER, finish INTEGER, tstart INTEGER, tfinish INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA user_version = 2")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("old-cache"));

    Index index;
    build(index);
    ASSERT_TRUE(index.isOpen());
    EXPECT_GT(index.noteCount(), 0) << "the indexer ran";
    EXPECT_FALSE(index.backlinks(vault.absolute(QStringLiteral("Home.md"))).isEmpty());
}

TEST_F(IndexTest, OpeningAnAlreadyIndexedVaultSaysTheContentsChanged)
{
    {
        Index first;
        build(first);
    }
    // Everything is current, so the indexer writes nothing and emits no
    // change of its own: open() is the only notice a listener gets.
    Index index;
    QSignalSpy changed(&index, &Index::changed);
    build(index);
    EXPECT_GE(changed.count(), 1);
    EXPECT_GT(index.noteCount(), 0);
}

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

// --- Index::search: the whole pipeline (FTS5 narrowing -> confirm in the
// real, indexed text -> snippet), on top of test_search.cpp's grammar-only
// coverage over a bare NoteContext with no database at all. ------------------

TEST_F(IndexTest, SearchFindsPlainWordsAcrossTheVault)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    // "noteAppC" is the heading of Projects/noteAppC.md and also sits inside
    // Home.md's own link text ([[Projects/noteAppC]]), since search reads the
    // raw source, not the rendered note.
    const QList<SearchResult> results = index.search(QStringLiteral("noteAppC"));
    QStringList paths;
    for (const SearchResult &result : results)
        paths << result.path;
    paths.sort();
    EXPECT_EQ(paths, QStringList({QStringLiteral("Home.md"), QStringLiteral("Projects/noteAppC.md")}));

    for (const SearchResult &result : results) {
        EXPECT_GT(result.matchCount, 0);
        EXPECT_FALSE(result.snippet.isEmpty());
    }
}

TEST_F(IndexTest, SearchTagFieldReadsTheRealTagsTable)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    // Ideas.md carries "#thinking/deep" inline and "tags: [thinking]" in its
    // frontmatter; tag:thinking has to find it through either.
    const QList<SearchResult> results = index.search(QStringLiteral("tag:thinking"));
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results.first().path, QStringLiteral("Ideas.md"));
}

TEST_F(IndexTest, SearchOffsetsLandOnTheRealMatch)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const QList<SearchResult> results = index.search(QStringLiteral("Brainwaves"));
    ASSERT_EQ(results.size(), 1);
    ASSERT_FALSE(results.first().matches.isEmpty());

    Note note;
    ASSERT_TRUE(Vault::read(dir.filePath(results.first().path), note));
    const SearchMatch &match = results.first().matches.first();
    EXPECT_EQ(note.text.mid(match.offset, match.length), QStringLiteral("Brainwaves"));
}

TEST_F(IndexTest, SearchRespectsItsLimit)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    // Every note in the fixture links to or mentions another; "the" is a
    // deliberately unrelated needle - what matters here is just the cap.
    const QList<SearchResult> results = index.search(QStringLiteral("noteAppC OR Ideas OR Home"), 1);
    EXPECT_LE(results.size(), 1);
}

TEST_F(IndexTest, SearchOnAnEmptyOrInvalidQueryReturnsNoResults)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    EXPECT_TRUE(index.search(QStringLiteral("   ")).isEmpty());

    QString error;
    EXPECT_TRUE(index.search(QStringLiteral("\"never closed"), 200, &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

// --- Index::unlinkedMentions -----------------------------------------------

TEST_F(IndexTest, UnlinkedMentionsFindsPlainTextButNotItself)
{
    write(QStringLiteral("Mentions.md"), "Talking about Home today, not linked.\n");

    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const QList<SearchResult> results = index.unlinkedMentions(dir.filePath(QStringLiteral("Home.md")));
    QStringList paths;
    for (const SearchResult &result : results)
        paths << result.path;

    // Home.md's own "# Home" heading and "#home" tag both textually contain
    // "Home" - excluded because a note never mentions itself. Ideas.md and
    // Projects/noteAppC.md both already link to Home ([[Home]]) with no bare
    // mention elsewhere, so they contribute nothing either.
    EXPECT_EQ(paths, QStringList({QStringLiteral("Mentions.md")}));
}

TEST_F(IndexTest, UnlinkedMentionsMaskOutOccurrencesThatAreAlreadyLinks)
{
    // Links Home *and* separately mentions it as plain text - only the
    // second occurrence should survive the mask.
    write(QStringLiteral("Mixed.md"), "[[Home]] is great. I mention Home again here.\n");

    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const QList<SearchResult> results = index.unlinkedMentions(dir.filePath(QStringLiteral("Home.md")));
    const auto mixed = std::find_if(results.cbegin(), results.cend(), [](const SearchResult &r) {
        return r.path == QStringLiteral("Mixed.md");
    });
    ASSERT_NE(mixed, results.cend());
    EXPECT_EQ(mixed->matchCount, 1);

    Note note;
    ASSERT_TRUE(Vault::read(dir.filePath(QStringLiteral("Mixed.md")), note));
    const SearchMatch &match = mixed->matches.first();
    EXPECT_EQ(note.text.mid(match.offset, match.length), QStringLiteral("Home"));
    // The surviving match is the bare mention, not the one inside [[Home]].
    EXPECT_GT(match.offset, note.text.indexOf(QStringLiteral("[[Home]]")) + 8);
}

TEST_F(IndexTest, UnlinkedMentionsIncludeAliases)
{
    // Ideas.md's alias is "Brainwaves" (see the fixture's frontmatter).
    write(QStringLiteral("AliasMention.md"), "Some notes call this Brainwaves instead.\n");

    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    const QList<SearchResult> results = index.unlinkedMentions(dir.filePath(QStringLiteral("Ideas.md")));
    QStringList paths;
    for (const SearchResult &result : results)
        paths << result.path;
    EXPECT_TRUE(paths.contains(QStringLiteral("AliasMention.md")));
}

namespace {

/// Builds a vault of `noteCount` notes, every one containing "needle" (so
/// every one is a genuine candidate FTS5 has to hand to the confirm-and-locate
/// pass, not just a fast rowid lookup that skips most of the vault), indexes
/// it cold, and returns how long index.search("needle") itself took.
double timeSearchOverAGeneratedVault(int noteCount)
{
    QTemporaryDir dir;
    for (int i = 0; i < noteCount; ++i) {
        QFile file(dir.filePath(QStringLiteral("Note%1.md").arg(i)));
        if (!file.open(QIODevice::WriteOnly))
            return -1;
        file.write(QStringLiteral("# Note %1\n\nneedle appears in the body here, padded out with some "
                                  "filler filler filler text so each note has a realistic size.\n")
                       .arg(i)
                       .toUtf8());
    }

    Vault vault(dir.path());
    Index index;
    if (!index.open(dir.path()))
        return -1;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    if (!done.wait(60000))
        return -1;

    QElapsedTimer clock;
    clock.start();
    const QList<SearchResult> results = index.search(QStringLiteral("needle"), noteCount + 10);
    const double ms = clock.nsecsElapsed() / 1e6;
    EXPECT_EQ(results.size(), noteCount);
    return ms;
}

} // namespace

TEST(SearchPerfTest, SearchTimeScalesWithVaultSizeNotQuadratically)
{
    // Asserted as a ratio, the way ParserPerfTest is: five times the notes
    // should cost roughly five times the work. This is the guard against a
    // confirm-and-locate pass that quietly becomes quadratic - e.g. by
    // re-scanning already-seen candidates, or by re-reading tags/headings/
    // tasks per note instead of once per search - the kind of bug this
    // project has hit twice already (see docs/PERFORMANCE.md, fixes 2 and 3).
    const int small = 500;
    const int large = small * 5;

    const double smallMs = timeSearchOverAGeneratedVault(small);
    const double largeMs = timeSearchOverAGeneratedVault(large);
    ASSERT_GE(smallMs, 0.0);
    ASSERT_GE(largeMs, 0.0);

    const double sizeRatio = double(large) / double(small);
    const double timeRatio = largeMs / std::max(0.001, smallMs);
    std::printf("[ PERF     ] search %d notes: %.2f ms, %d notes: %.2f ms (%.1fx notes, %.1fx time)\n",
                small, smallMs, large, largeMs, sizeRatio, timeRatio);

    EXPECT_LT(timeRatio, sizeRatio * 2.0);
}

namespace {

void writeNotes(const QTemporaryDir &dir, const QString &prefix, int count)
{
    for (int i = 0; i < count; ++i) {
        QFile file(dir.filePath(QStringLiteral("%1%2.md").arg(prefix).arg(i)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        // Rows in most of the tables, as a real note has: what a write costs.
        QString text = QStringLiteral("---\ntags: [a, b]\nstatus: draft\n---\n# %1 %2\n\n").arg(prefix).arg(i);
        for (int link = 1; link <= 8; ++link)
            text += QStringLiteral("## Part %1\n\nSee [[%2%3]] #t%1 and text to index.\n- [ ] task %1\n\n")
                        .arg(link)
                        .arg(prefix)
                        .arg(i + link);
        file.write(text.toUtf8());
    }
}

} // namespace

TEST(IndexerThreadTest, AColdIndexLeavesTheEventLoopFree)
{
    // The scan writes on a thread of its own and the Index's thread takes
    // only each chunk's names. Writing every note there held the loop for
    // the whole index, 0.18 ms a note (docs/PERFORMANCE.md, fix 18).
    QTemporaryDir dir;
    writeNotes(dir, QStringLiteral("Note"), 3000);
    Index index;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(Vault(dir.path()));

    QElapsedTimer beat;
    qint64 worst = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { worst = std::max(worst, beat.restart()); });
    beat.start();
    heartbeat.start();
    ASSERT_TRUE(done.wait(60000));
    heartbeat.stop();

    EXPECT_EQ(index.noteCount(), 3000);
    EXPECT_EQ(index.resolve(QStringLiteral("Note2999")), QStringLiteral("Note2999.md"));
    std::printf("[ PERF     ] cold index of 3000 notes: longest wait between 5 ms beats %lld ms\n",
                static_cast<long long>(worst));
    EXPECT_LT(worst, 40);
}

TEST(IndexerThreadTest, AScanStoppedByAVaultSwitchLeavesNothingBehind)
{
    QTemporaryDir old;
    writeNotes(old, QStringLiteral("Old"), 3000);
    QTemporaryDir fresh;
    writeNotes(fresh, QStringLiteral("New"), 1);
    Index index;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(Vault(old.path()));
    // Long enough for the scan to have queued chunks the loop has not run.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    indexer.setVault(Vault(fresh.path()));
    ASSERT_TRUE(done.wait(20000));
    QCoreApplication::processEvents(); // anything the old scan still had queued

    EXPECT_EQ(done.size(), 1) << "the stopped scan never reports";
    int leaked = 0;
    for (int i = 0; i < 3000; ++i)
        leaked += !index.resolve(QStringLiteral("Old%1").arg(i)).isEmpty();
    EXPECT_EQ(leaked, 0) << "names from the old vault's scan";
    EXPECT_EQ(index.resolve(QStringLiteral("New0")), QStringLiteral("New0.md"));
}
