// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>
#include <QTimer>

#include "Fuzzy.h"
#include "Index.h"
#include "Indexer.h"
#include "Parser.h"
#include "Vault.h"
#include "Perf.h"

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

TEST_F(IndexTest, ALinkFindsANoteThatWasEncrypted)
{
    QFile locked(dir.filePath(QStringLiteral("Secret.mdlock")));
    ASSERT_TRUE(locked.open(QIODevice::WriteOnly));
    locked.write("NAPCLOCK...");
    locked.close();
    Index index;
    build(index);
    // [[Secret]] written when it was Secret.md keeps working.
    EXPECT_EQ(index.resolve(QStringLiteral("Secret")), QStringLiteral("Secret.mdlock"));
    EXPECT_EQ(index.resolve(QStringLiteral("Home")), QStringLiteral("Home.md"));
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

TEST_F(IndexTest, TagCountsAgreeWithNotesWithTag)
{
    // Nested tags, a parent that is never a tag itself, two children of one
    // parent in one note, and a repeated tag: each must count a note once.
    write(QStringLiteral("A.md"), "#area/one #area/two #area/one\n");
    write(QStringLiteral("B.md"), "#area/one/deep #solo\n");
    write(QStringLiteral("C.md"), "---\ntags: [area]\n---\nbody\n");
    Index index;
    build(index);

    const QHash<QString, int> counts = index.tagCounts();
    QStringList paths = index.allTags();
    paths << QStringLiteral("area/one"); // also a parent, of area/one/deep
    for (const QString &path : std::as_const(paths))
        EXPECT_EQ(counts.value(path), index.notesWithTag(path).size()) << path.toStdString();
    EXPECT_EQ(counts.value(QStringLiteral("area")), 3);
    EXPECT_EQ(counts.value(QStringLiteral("area/one")), 2);
    EXPECT_EQ(counts.value(QStringLiteral("thinking")), 1);
}

TEST_F(IndexTest, AScalarTagsAndAliasesValueIsSplit)
{
    write(QStringLiteral("Split.md"), "---\ntags: project, urgent\naliases: Brain, Mind\n---\nbody\n");
    Index index;
    build(index);

    EXPECT_EQ(index.notesWithTag(QStringLiteral("urgent")), QStringList({QStringLiteral("Split.md")}));
    EXPECT_EQ(index.resolve(QStringLiteral("Mind")), QStringLiteral("Split.md"));
}

TEST_F(IndexTest, DotMarkdownIsAnAttachmentNotANote)
{
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Sub")));
    write(QStringLiteral("Sub/Note.markdown"), "# Not a note\n");
    Index index;
    build(index);

    // Only the three .md notes count; the .markdown file and notes.txt are
    // attachments, resolved by their full file name like any other.
    EXPECT_EQ(index.noteCount(), 3);
    EXPECT_EQ(index.attachmentCount(), 2);
    EXPECT_EQ(index.resolve(QStringLiteral("Note.markdown")), QStringLiteral("Sub/Note.markdown"));
    EXPECT_EQ(index.resolve(QStringLiteral("Sub/Note.markdown")), QStringLiteral("Sub/Note.markdown"));
    EXPECT_TRUE(index.resolve(QStringLiteral("Note")).isEmpty());
}

TEST_F(IndexTest, QuickSwitcherFindsByAnyName)
{
    Index index;
    build(index);

    EXPECT_TRUE(index.findByName(QStringLiteral("note")).contains(
        QStringLiteral("Projects/noteAppC.md")));
    EXPECT_TRUE(index.findByName(QStringLiteral("brain")).contains(QStringLiteral("Ideas.md")));
}

TEST_F(IndexTest, LinkCompletionListsAliasesHeadingsAndBlockIDs)
{
    write(QStringLiteral("Sections.md"),
          "---\naliases: Sec one, Sec two\n---\n# Setup\n\ntext ^blk-1\n\n## Usage\n\nmore ^b2\n");
    Index index;
    build(index);

    // An alias is offered as `Note|alias`, so it is not also a plain name.
    const QList<QPair<QString, QString>> aliases = index.aliases();
    EXPECT_TRUE(aliases.contains({QStringLiteral("Brainwaves"), QStringLiteral("Ideas")}));
    EXPECT_TRUE(aliases.contains({QStringLiteral("Sec one"), QStringLiteral("Sections")}));
    EXPECT_TRUE(aliases.contains({QStringLiteral("Sec two"), QStringLiteral("Sections")}));
    const QStringList targets = index.linkTargets();
    EXPECT_TRUE(targets.contains(QStringLiteral("Ideas")));
    EXPECT_FALSE(targets.contains(QStringLiteral("Brainwaves")));

    EXPECT_EQ(index.fragmentsOf(QStringLiteral("Sections.md"), false),
              (QStringList{QStringLiteral("Setup"), QStringLiteral("Usage")}));
    EXPECT_EQ(index.fragmentsOf(QStringLiteral("Sections.md"), true),
              (QStringList{QStringLiteral("blk-1"), QStringLiteral("b2")}));
    EXPECT_TRUE(index.fragmentsOf(QStringLiteral("Nowhere.md"), false).isEmpty());
}

TEST_F(IndexTest, NestedLinkCompletionListsOnlyTheHeadingsUnderTheParent)
{
    write(QStringLiteral("Nested.md"), "# A\n## One\n### Deep\n# B\n## Two\n");
    Index index;
    build(index);
    EXPECT_EQ(index.fragmentsOf(QStringLiteral("Nested.md"), false, {QStringLiteral("A")}),
              (QStringList{QStringLiteral("One"), QStringLiteral("Deep")}));
    EXPECT_EQ(index.fragmentsOf(QStringLiteral("Nested.md"), false, {QStringLiteral("a"), QStringLiteral("one")}),
              QStringList{QStringLiteral("Deep")});
    EXPECT_EQ(index.fragmentsOf(QStringLiteral("Nested.md"), false, {QStringLiteral("B")}),
              QStringList{QStringLiteral("Two")});
    EXPECT_TRUE(index.fragmentsOf(QStringLiteral("Nested.md"), false, {QStringLiteral("Two")}).isEmpty());
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

// The scan thread posts its last batch and ends; until the Index's thread applies
// it, a new scan must queue rather than start, or the apply would wait for the new
// scan and lose the first one's changed().
TEST_F(IndexTest, ARebuildBeforeTheLastBatchIsAppliedQueuesBehindIt)
{
    Index index;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    QSignalSpy changed(&index, &Index::changed); // after open() has announced itself
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // the scan ends; no event is processed
    indexer.rebuild();
    ASSERT_TRUE(QTest::qWaitFor([&] { return done.size() >= 2 && !indexer.isRunning(); }, 20000));
    EXPECT_EQ(done.size(), 2) << "one finished per scan";
    EXPECT_EQ(done.first().at(0).toInt(), 3) << "the first scan's count is its own";
    EXPECT_EQ(changed.size(), 1) << "the first scan's writes are announced";
    EXPECT_EQ(index.noteCount(), 3);
}

// linkTargets() and aliases() are cached between index changes; a write that
// changes either must show in the next call.
TEST_F(IndexTest, TheLinkListsFollowEditsAfterBeingQueried)
{
    Index index;
    build(index);
    EXPECT_FALSE(index.linkTargets().contains(QStringLiteral("Fresh")));
    EXPECT_FALSE(index.aliases().contains({QStringLiteral("Nick"), QStringLiteral("Fresh")}));

    write(QStringLiteral("Fresh.md"), "---\naliases: [Nick]\n---\n");
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    EXPECT_TRUE(index.linkTargets().contains(QStringLiteral("Fresh")));
    EXPECT_FALSE(index.linkTargets().contains(QStringLiteral("Nick")));
    EXPECT_TRUE(index.aliases().contains({QStringLiteral("Nick"), QStringLiteral("Fresh")}));

    write(QStringLiteral("Fresh.md"), "---\naliases: [Other]\n---\n");
    QSignalSpy again(&indexer, &Indexer::finished);
    indexer.rebuild();
    ASSERT_TRUE(again.wait(20000));
    EXPECT_FALSE(index.aliases().contains({QStringLiteral("Nick"), QStringLiteral("Fresh")}));
    EXPECT_TRUE(index.aliases().contains({QStringLiteral("Other"), QStringLiteral("Fresh")}));

    // A note that goes takes its name and aliases out of both.
    QFile::remove(vault.absolute(QStringLiteral("Fresh.md")));
    index.remove(vault.absolute(QStringLiteral("Fresh.md")));
    EXPECT_FALSE(index.linkTargets().contains(QStringLiteral("Fresh")));
    EXPECT_FALSE(index.aliases().contains({QStringLiteral("Other"), QStringLiteral("Fresh")}));
}

TEST_F(IndexTest, AForeignReaderMakesASuffixANoteAndNotebooksStillWork)
{
    write(QStringLiteral("Sketch.ink"), "stands in for an ink file");
    write(QStringLiteral("Projects/Analysis.ipynb"), "{\"cells\": \"stands in for a notebook\"}");
    Index index;
    Indexer indexer(&index);
    {
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }
    EXPECT_FALSE(index.allPaths().contains(QStringLiteral("Sketch.ink"))) << "without a reader the suffix is an attachment";
    EXPECT_EQ(index.resolve(QStringLiteral("Sketch.ink")), QStringLiteral("Sketch.ink")) << "and it resolves as one";

    QSignalSpy inkOn(&indexer, &Indexer::finished);
    indexer.setForeignReader({QStringLiteral("ink")}, [](const QByteArray &bytes) -> std::optional<QString> {
        return bytes.contains("stands in") ? std::optional<QString>(QStringLiteral("See [[Home]] #inked\n")) : std::nullopt;
    });
    ASSERT_TRUE(inkOn.wait(20000)) << "adding a reader rescans by itself";
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("Sketch.ink")));
    EXPECT_EQ(index.notesWithTag(QStringLiteral("inked")), QStringList{QStringLiteral("Sketch.ink")});
    bool fromSketch = false;
    for (const Backlink &link : index.backlinks(vault.absolute(QStringLiteral("Home.md"))))
        fromSketch = fromSketch || link.fromPath == QStringLiteral("Sketch.ink");
    EXPECT_TRUE(fromSketch);
    EXPECT_FALSE(index.allPaths().contains(QStringLiteral("Projects/Analysis.ipynb"))) << "the notebook has no reader of its own here";

    QSignalSpy nbOn(&indexer, &Indexer::finished);
    indexer.setNotebookReader([](const QByteArray &) -> std::optional<QString> { return QStringLiteral("zeppelin\n"); });
    ASSERT_TRUE(nbOn.wait(20000));
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("Projects/Analysis.ipynb")));
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("Sketch.ink"))) << "setNotebookReader leaves the ink reader alone";

    QSignalSpy nbOff(&indexer, &Indexer::finished);
    indexer.setNotebookReader(nullptr);
    ASSERT_TRUE(nbOff.wait(20000));
    EXPECT_FALSE(index.allPaths().contains(QStringLiteral("Projects/Analysis.ipynb")));
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("Sketch.ink"))) << "and clearing the notebook reader leaves it too";

    QSignalSpy inkOff(&indexer, &Indexer::finished);
    indexer.setForeignReader({QStringLiteral("ink")}, nullptr);
    ASSERT_TRUE(inkOff.wait(20000));
    EXPECT_FALSE(index.allPaths().contains(QStringLiteral("Sketch.ink")));
}

// An ink note is named by its full file name, as a notebook is: a handwritten Lecture.ink beside Lecture.md
// must not take [[Lecture]] away from the Markdown note.
TEST_F(IndexTest, AnInkNoteIsNamedByItsFileNameAndNeverShadowsAMarkdownNote)
{
    write(QStringLiteral("Lecture.md"), "# Lecture\n");
    write(QStringLiteral("Lecture.ink"), "stands in for an ink file");
    write(QStringLiteral("Link.md"), "See [[Lecture]] and [[Lecture.ink]].\n");
    Index index;
    Indexer indexer(&index);
    indexer.setForeignReader({QStringLiteral("ink")}, [](const QByteArray &) -> std::optional<QString> { return QStringLiteral("text\n"); });
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    EXPECT_EQ(index.resolve(QStringLiteral("Lecture")), QStringLiteral("Lecture.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("Lecture"), QStringLiteral("Link.md")), QStringLiteral("Lecture.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("Lecture.ink")), QStringLiteral("Lecture.ink"));
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("Lecture.ink")));
}

// A reader's maxBytes keeps a huge file from being read whole; 0 (the notebook default) is no limit.
TEST_F(IndexTest, AForeignReaderMaxBytesSkipsBigFilesAndZeroIsUnlimited)
{
    for (const char *name : {"Big.ink", "Small.ink", "Big.ipynb"}) {
        QFile file(vault.absolute(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_TRUE(file.resize(QByteArray(name) == "Small.ink" ? 1000 : (2 << 20))); // sparse
    }
    Index index;
    Indexer indexer(&index);
    const Indexer::ForeignReader reader = [](const QByteArray &) -> std::optional<QString> { return QStringLiteral("text\n"); };
    indexer.setForeignReader({QStringLiteral("ink")}, reader, 1 << 20);
    indexer.setNotebookReader(reader);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    EXPECT_FALSE(index.allPaths().contains(QStringLiteral("Big.ink"))) << "over its reader's cap: never read";
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("Small.ink")));
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("Big.ipynb"))) << "a notebook has no cap";
}

// A file that grows past its reader's cap leaves the index: its old row would otherwise show text the file no longer has.
TEST_F(IndexTest, AFileThatGrowsPastItsReadersCapLosesItsRow)
{
    write(QStringLiteral("Grow.ink"), "small");
    Index index;
    Indexer indexer(&index);
    indexer.setForeignReader({QStringLiteral("ink")},
                             [](const QByteArray &) -> std::optional<QString> { return QStringLiteral("grown\n"); }, 1 << 20);
    {
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }
    ASSERT_TRUE(index.allPaths().contains(QStringLiteral("Grow.ink")));
    {
        QFile file(vault.absolute(QStringLiteral("Grow.ink")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_TRUE(file.resize(2 << 20));
    }
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.rebuild();
    ASSERT_TRUE(done.wait(20000));
    EXPECT_FALSE(index.allPaths().contains(QStringLiteral("Grow.ink")));
    EXPECT_TRUE(index.search(QStringLiteral("grown")).isEmpty());
}

// A rename rewrites links inside a notebook's JSON; the notebook stays indexed by its reader's text, not by the JSON.
TEST_F(IndexTest, RenamingANoteLinkedFromANotebookKeepsTheNotebookIndexedByItsReader)
{
    write(QStringLiteral("Nb.ipynb"), "{\"cells\": \"see [[Home]] zeppelinjson\"}");
    Index index;
    Indexer indexer(&index);
    indexer.setNotebookReader([](const QByteArray &bytes) -> std::optional<QString> {
        return bytes.contains("[[Home]]") ? std::optional<QString>(QStringLiteral("see [[Home]] zeppelin\n"))
                                          : std::optional<QString>(QStringLiteral("see [[Start]] zeppelin\n"));
    });
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    ASSERT_TRUE(QFile::rename(vault.absolute(QStringLiteral("Home.md")), vault.absolute(QStringLiteral("Start.md"))));
    QString error;
    ASSERT_GE(index.updateLinksAfterRename(QStringLiteral("Home.md"), QStringLiteral("Start.md"), vault, &error), 0);
    QFile file(vault.absolute(QStringLiteral("Nb.ipynb")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray("{\"cells\": \"see [[Start]] zeppelinjson\"}"));
    EXPECT_FALSE(index.search(QStringLiteral("zeppelin")).isEmpty()) << "still its reader's text";
    EXPECT_TRUE(index.search(QStringLiteral("zeppelinjson")).isEmpty()) << "not the JSON";
}

// Suffixes are matched case-insensitively: a SKETCH.INK is read by the "ink" reader.
TEST_F(IndexTest, AForeignReaderMatchesTheSuffixInAnyCase)
{
    write(QStringLiteral("SKETCH.INK"), "stands in for an ink file");
    Index index;
    Indexer indexer(&index);
    indexer.setForeignReader({QStringLiteral("INK")}, [](const QByteArray &) -> std::optional<QString> { return QStringLiteral("loud\n"); });
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    EXPECT_TRUE(index.allPaths().contains(QStringLiteral("SKETCH.INK")));
}

// A notebook is a note only while the indexer has a reader for it (the window
// gives it one with the notebooks feature on): indexed by the reader's text,
// named by its file name with the extension, and a plain file again without.
TEST_F(IndexTest, ANotebookIsIndexedByItsTextWhileAReaderIsSet)
{
    write(QStringLiteral("Projects/Analysis.ipynb"), "{\"cells\": \"stands in for a notebook\"}");
    write(QStringLiteral("Analysis.md"), "# Analysis\n\nthe markdown note\n");
    write(QStringLiteral("Mentions.md"), "See [[Analysis.ipynb]] and [[Analysis]].\n");
    const Indexer::NotebookReader reader = [](const QByteArray &bytes) -> std::optional<QString> {
        return bytes.contains("stands in") ? std::optional<QString>(QStringLiteral("zeppelin cell [[Home]] #nbtag\n"))
                                           : std::nullopt;
    };
    const QString notebook = QStringLiteral("Projects/Analysis.ipynb");
    Index index;
    Indexer indexer(&index);
    const auto scan = [&](bool rebuild) {
        QSignalSpy done(&indexer, &Indexer::finished);
        if (rebuild)
            indexer.rebuild();
        else
            indexer.setVault(vault);
        EXPECT_TRUE(done.wait(20000));
    };

    // Off, as before: a file that is not a note.
    scan(false);
    EXPECT_EQ(index.attachmentCount(), 2) << "notes.txt and the notebook";
    EXPECT_EQ(index.noteCount(), 5);
    EXPECT_TRUE(index.search(QStringLiteral("zeppelin")).isEmpty());
    EXPECT_EQ(index.resolve(QStringLiteral("Analysis.ipynb")), notebook) << "it already resolves, as an attachment";

    // On: a note, found by its text, tags and links, by name with the extension.
    indexer.setNotebookReader(reader);
    QSignalSpy rescanned(&indexer, &Indexer::finished);
    ASSERT_TRUE(rescanned.wait(20000)) << "switching it rescans by itself";
    EXPECT_EQ(index.noteCount(), 6);
    EXPECT_EQ(index.attachmentCount(), 1);
    EXPECT_TRUE(index.allPaths().contains(notebook));
    ASSERT_EQ(index.search(QStringLiteral("zeppelin")).size(), 1);
    EXPECT_EQ(index.search(QStringLiteral("zeppelin")).first().path, notebook);
    EXPECT_EQ(index.notesWithTag(QStringLiteral("nbtag")), QStringList{notebook});
    EXPECT_EQ(index.findByName(QStringLiteral("analysis.ipy")), QStringList{notebook}) << "the quick switcher";
    EXPECT_EQ(index.resolve(QStringLiteral("Analysis.ipynb")), notebook);
    EXPECT_EQ(index.resolve(QStringLiteral("Analysis")), QStringLiteral("Analysis.md")) << "a bare name is still the note";
    const QList<Backlink> toNotebook = index.backlinks(vault.absolute(notebook));
    ASSERT_EQ(toNotebook.size(), 1);
    EXPECT_EQ(toNotebook.first().fromPath, QStringLiteral("Mentions.md"));
    EXPECT_EQ(index.backlinks(vault.absolute(QStringLiteral("Home.md"))).size(), 3) << "the notebook's own [[Home]] counts";

    // A change to the notebook is picked up by a single reindex, a file that is no notebook is not indexed at all.
    write(QStringLiteral("Projects/Analysis.ipynb"), "{\"cells\": \"stands in for it, longer\"}");
    write(QStringLiteral("Broken.ipynb"), "not json");
    QSignalSpy once(&indexer, &Indexer::finished);
    indexer.rebuild();
    ASSERT_TRUE(once.wait(20000));
    EXPECT_FALSE(index.allPaths().contains(QStringLiteral("Broken.ipynb")));

    // Off again: a file that is not a note, and its text is gone from search.
    indexer.setNotebookReader(nullptr);
    QSignalSpy off(&indexer, &Indexer::finished);
    ASSERT_TRUE(off.wait(20000));
    EXPECT_EQ(index.noteCount(), 5);
    EXPECT_FALSE(index.allPaths().contains(notebook));
    EXPECT_TRUE(index.search(QStringLiteral("zeppelin")).isEmpty());
}

namespace {

ParseOptions metaNotesOn(const QString &open = QStringLiteral("["), const QString &close = QStringLiteral("]"))
{
    ParseOptions options;
    options.metaNotes = true;
    options.open = open;
    options.close = close;
    return options;
}

} // namespace

TEST_F(IndexTest, MetaNotesAreIndexedOnlyWhileAskedFor)
{
    const QString draft = QStringLiteral("She left early [check the date] and never came back.\n");
    write(QStringLiteral("Draft.md"), draft.toUtf8());
    Index index;
    build(index);
    EXPECT_TRUE(index.search(QStringLiteral("meta:date")).isEmpty()) << "off by default";
    EXPECT_TRUE(index.search(QStringLiteral("meta:")).isEmpty());

    Indexer indexer(&index);
    indexer.setMetaNotes(metaNotesOn());
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    const QList<SearchResult> found = index.search(QStringLiteral("meta:date"));
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.first().path, QStringLiteral("Draft.md"));
    ASSERT_EQ(index.search(QStringLiteral("meta:")).size(), 1);
    EXPECT_EQ(index.search(QStringLiteral("meta:")).first().matches.first().offset, draft.indexOf(QStringLiteral("[check")));
    EXPECT_TRUE(index.search(QStringLiteral("meta:early")).isEmpty());
}

TEST_F(IndexTest, ChangingTheMetaDelimitersReindexesEveryUnchangedNote)
{
    const QString draft = QStringLiteral("A [square run] and a << angled run >> here.\n");
    write(QStringLiteral("Draft.md"), draft.toUtf8());
    Index index;
    Indexer indexer(&index);
    indexer.setMetaNotes(metaNotesOn());
    QSignalSpy first(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(first.wait(20000));
    ASSERT_EQ(index.search(QStringLiteral("meta:square")).size(), 1);

    QSignalSpy again(&indexer, &Indexer::finished);
    indexer.setMetaNotes(metaNotesOn(QStringLiteral("<<"), QStringLiteral(">>")));
    ASSERT_TRUE(again.wait(20000)) << "changing them rescans by itself";
    EXPECT_TRUE(index.search(QStringLiteral("meta:square")).isEmpty()) << "nothing on disk changed, yet it is re-parsed";
    const QList<SearchResult> found = index.search(QStringLiteral("meta:"));
    ASSERT_EQ(found.size(), 1);
    ASSERT_EQ(found.first().matches.size(), 1);
    EXPECT_EQ(found.first().matches.first().offset, draft.indexOf(QStringLiteral("<<")));

    // The cache remembers them: a warm start by other delimiters re-parses, by the same ones does not.
    Indexer same(&index);
    same.setMetaNotes(metaNotesOn(QStringLiteral("<<"), QStringLiteral(">>")));
    QSignalSpy warm(&same, &Indexer::finished);
    same.setVault(vault);
    ASSERT_TRUE(warm.wait(20000));
    EXPECT_EQ(warm.last().at(0).toInt(), 0) << "nothing to re-parse";
    Indexer other(&index);
    other.setMetaNotes(metaNotesOn());
    QSignalSpy cold(&other, &Indexer::finished);
    other.setVault(vault);
    ASSERT_TRUE(cold.wait(20000));
    EXPECT_EQ(index.search(QStringLiteral("meta:square")).size(), 1);
    EXPECT_TRUE(index.search(QStringLiteral("meta:angled")).isEmpty());
}

TEST_F(IndexTest, TaskStatesAreIndexedOnlyWhenAsked)
{
    write(QStringLiteral("Chores.md"), "- [-] pay rent\n- [ ] call mum\n");
    Index index;
    Indexer indexer(&index);
    QSignalSpy first(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(first.wait(20000));
    EXPECT_TRUE(index.search(QStringLiteral("task:rent")).isEmpty()) << "[-] is only text by default";
    EXPECT_EQ(index.search(QStringLiteral("task:mum")).size(), 1);

    ParseOptions on;
    on.taskStates = true;
    QSignalSpy again(&indexer, &Indexer::finished);
    indexer.setMetaNotes(on);
    ASSERT_TRUE(again.wait(20000)) << "flipping it rescans by itself";
    EXPECT_EQ(index.search(QStringLiteral("task-cancelled:rent")).size(), 1);
    EXPECT_TRUE(index.search(QStringLiteral("task-todo:rent")).isEmpty());
    EXPECT_EQ(index.search(QStringLiteral("task-done:rent")).size(), 1);

    // A warm start by the other setting re-parses; by the same one does not.
    Indexer warmOn(&index);
    warmOn.setMetaNotes(on);
    QSignalSpy same(&warmOn, &Indexer::finished);
    warmOn.setVault(vault);
    ASSERT_TRUE(same.wait(20000));
    EXPECT_EQ(same.last().at(0).toInt(), 0) << "nothing to re-parse";
    Indexer warmOff(&index);
    QSignalSpy other(&warmOff, &Indexer::finished);
    warmOff.setVault(vault);
    ASSERT_TRUE(other.wait(20000));
    EXPECT_GT(other.last().at(0).toInt(), 0) << "the rows were read with states";
    EXPECT_TRUE(index.search(QStringLiteral("task:rent")).isEmpty());
}

TEST_F(IndexTest, SwitchingMetaNotesOffDropsTheRowsWithoutAScan)
{
    write(QStringLiteral("Draft.md"), "She left early [check the date] and never came back.\n");
    Index index;
    Indexer indexer(&index);
    indexer.setMetaNotes(metaNotesOn());
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    ASSERT_EQ(index.search(QStringLiteral("meta:")).size(), 1);

    QSignalSpy scans(&indexer, &Indexer::finished);
    indexer.setMetaNotes(ParseOptions{});
    EXPECT_TRUE(index.search(QStringLiteral("meta:")).isEmpty()) << "at once";
    EXPECT_FALSE(scans.wait(300));
    EXPECT_EQ(scans.count(), 0);

    // And a later warm start with them off has nothing stale to clear.
    Indexer later(&index);
    QSignalSpy warm(&later, &Indexer::finished);
    later.setVault(vault);
    ASSERT_TRUE(warm.wait(20000));
    EXPECT_EQ(warm.last().at(0).toInt(), 0);
    EXPECT_TRUE(index.search(QStringLiteral("meta:")).isEmpty());
}

TEST_F(IndexTest, ARenameKeepsTheMetaNotesOfTheNotesItRewrites)
{
    write(QStringLiteral("Draft.md"), "She left [check the date] for [[Home]].\n");
    Index index;
    Indexer indexer(&index);
    indexer.setMetaNotes(metaNotesOn());
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    ASSERT_EQ(index.search(QStringLiteral("meta:date")).size(), 1);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Home.md")), QStringLiteral("Start"), &newPath, &error))
        << error.toStdString();
    ASSERT_GE(index.updateLinksAfterRename(QStringLiteral("Home.md"), QStringLiteral("Start.md"), vault, &error), 1)
        << error.toStdString();
    const QList<SearchResult> found = index.search(QStringLiteral("meta:date"));
    ASSERT_EQ(found.size(), 1) << "the rewritten linker keeps its run";
    EXPECT_EQ(found.first().path, QStringLiteral("Draft.md"));
}

TEST_F(IndexTest, SwitchingMetaNotesOffDuringAScanLeavesNoRows)
{
    for (int i = 0; i < 1500; ++i)
        write(QStringLiteral("Bulk %1.md").arg(i), "A [run] in every note.\n");
    Index index;
    Indexer indexer(&index);
    indexer.setMetaNotes(metaNotesOn());
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(indexer.isRunning());
    indexer.setMetaNotes(ParseOptions{});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !done.isEmpty() && !indexer.isRunning(); }, 30000));
    EXPECT_TRUE(index.search(QStringLiteral("meta:")).isEmpty());
    EXPECT_TRUE(index.metaNotesKey().isEmpty());
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

// A note that is a link is named and skipped; the notes around it are still rewritten.
// (Before, the first refusal returned -1 with the rest of the batch undone.)
TEST_F(RenameLinksTest, ANoteThatIsALinkIsSkippedAndTheBatchGoesOn)
{
    write(QStringLiteral("A.md"), "# A\n");
    write(QStringLiteral("Aaa.md"), "[[A]]");
    write(QStringLiteral("Zzz.md"), "[[A]]");
    QTemporaryDir outside;
    const QString real = QDir(outside.path()).filePath(QStringLiteral("dot.md"));
    {
        QFile file(real);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("[[A]]");
    }
    ASSERT_TRUE(QFile::link(real, dir.filePath(QStringLiteral("Mid.md"))));

    vault = Vault(dir.path());
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("A.md")), QStringLiteral("B"), &newPath, &error))
        << error.toStdString();
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("A.md"), QStringLiteral("B.md"), vault, &error), 2)
        << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Aaa.md"))), QByteArray("[[B]]"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Zzz.md"))), QByteArray("[[B]]"));
    EXPECT_EQ(read(real), QByteArray("[[A]]")) << "not written through";
    EXPECT_TRUE(QFileInfo(dir.filePath(QStringLiteral("Mid.md"))).isSymLink());
    EXPECT_TRUE(error.contains(QStringLiteral("Mid.md"))) << error.toStdString();
}

TEST_F(RenameLinksTest, FolderRenameRewritesPathLinksIntoIt)
{
    write(QStringLiteral("Folder/A.md"), "# A\n");
    write(QStringLiteral("Folder/B.md"), "[[Folder/A]] [x](A.md)");
    write(QStringLiteral("Other.md"), "[[Folder/A]] [[A]] [y](Folder/A.md)");

    vault = Vault(dir.path());
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Folder")), QStringLiteral("Moved"),
                                 &newPath, &error))
        << error.toStdString();

    // B.md (itself moved with the folder) and Other.md.
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("Folder"), QStringLiteral("Moved"), vault, &error), 2)
        << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Moved/B.md"))), QByteArray("[[Moved/A]] [x](A.md)"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Other.md"))),
              QByteArray("[[Moved/A]] [[A]] [y](Moved/A.md)"));
}

// A note saved since the index last looked may hold a first link to the renamed
// note: named as an extra candidate, it is read from disk and rewritten.
TEST_F(RenameLinksTest, ANoteNamedAsAnExtraCandidateIsReadFromDisk)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    write(QStringLiteral("Home.md"), "Nothing yet.\n");
    vault = Vault(dir.path());
    Index index;
    build(index);
    write(QStringLiteral("Home.md"), "Nothing yet. [[Target]]\n"); // saved, not indexed

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Target.md")), QStringLiteral("Renamed"),
                                 &newPath, &error))
        << error.toStdString();
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("Target.md"), QStringLiteral("Renamed.md"), vault,
                                           &error, {QStringLiteral("Home.md")}),
              1)
        << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Home.md"))), QByteArray("Nothing yet. [[Renamed]]\n"));
}

// What a rename must open besides the index's own list: the notes of the scan
// running and those queued behind it.
TEST_F(RenameLinksTest, TheIndexerNamesTheNotesItHasNotAppliedYet)
{
    write(QStringLiteral("Home.md"), "Home\n");
    vault = Vault(dir.path());
    Index index;
    Indexer indexer(&index);
    EXPECT_TRUE(indexer.pathsInFlight().isEmpty());
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(vault);
    ASSERT_TRUE(done.wait(20000));
    EXPECT_TRUE(indexer.pathsInFlight().isEmpty());

    const QString home = vault.absolute(QStringLiteral("Home.md"));
    indexer.reindex(home);
    EXPECT_EQ(indexer.pathsInFlight(), QStringList{home});
    indexer.reindex(home); // queued behind the running one
    EXPECT_EQ(indexer.pathsInFlight(), (QStringList{home, home}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !indexer.isRunning(); }, 20000));
    EXPECT_TRUE(indexer.pathsInFlight().isEmpty());
}

// A whole-vault scan (a vault just opened) leaves the index without most notes:
// a rename then opens every note, so no link is missed while it runs.
TEST_F(RenameLinksTest, AWholeVaultScanInFlightNamesEveryNote)
{
    write(QStringLiteral("Home.md"), "Home [[Target]]\n");
    write(QStringLiteral("Sub/Other.md"), "Other\n");
    write(QStringLiteral(".hidden/Skip.md"), "Skip\n");
    write(QStringLiteral("picture.png"), "png");
    vault = Vault(dir.path());
    Index index;
    Indexer indexer(&index);
    indexer.setVault(vault); // the whole-vault scan starts here
    QStringList inFlight = indexer.pathsInFlight();
    inFlight.sort();
    EXPECT_EQ(inFlight, (QStringList{vault.absolute(QStringLiteral("Home.md")),
                                     vault.absolute(QStringLiteral("Sub/Other.md"))}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !indexer.isRunning(); }, 20000));
    EXPECT_TRUE(indexer.pathsInFlight().isEmpty());
}

// A note whose links all stay right through a folder rename (wikilinks by name
// to a sibling that moved with it) is not rewritten: no bytes touched, not counted.
TEST_F(RenameLinksTest, AFolderRenameLeavesNotesWithSiblingLinksUntouched)
{
    write(QStringLiteral("Folder/A.md"), "see [[Sibling]]\n");
    write(QStringLiteral("Folder/Sibling.md"), "back to [[A]]\n");
    write(QStringLiteral("Other.md"), "[[Folder/A]]\n");
    vault = Vault(dir.path());
    const QDateTime past = QDateTime::currentDateTime().addDays(-3);
    for (const QString &name : {QStringLiteral("Folder/A.md"), QStringLiteral("Folder/Sibling.md")}) {
        QFile file(vault.absolute(name));
        ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        ASSERT_TRUE(file.setFileTime(past, QFileDevice::FileModificationTime));
    }
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Folder")), QStringLiteral("Moved"), &newPath, &error))
        << error.toStdString();
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("Folder"), QStringLiteral("Moved"), vault, &error), 1)
        << "only Other.md's path link changes: " << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Other.md"))), QByteArray("[[Moved/A]]\n"));
    for (const QString &name : {QStringLiteral("Moved/A.md"), QStringLiteral("Moved/Sibling.md")}) {
        EXPECT_EQ(QFileInfo(vault.absolute(name)).lastModified().toSecsSinceEpoch(), past.toSecsSinceEpoch())
            << name.toStdString() << " was rewritten byte for byte";
    }
}

// A linking note deleted outside the app since the index looked is named, not
// fatal: the notes after it are still rewritten.
TEST_F(RenameLinksTest, ALinkingNoteGoneFromDiskDoesNotStopTheRewrite)
{
    write(QStringLiteral("Target.md"), "# Target\n");
    for (const char *name : {"A", "B", "C"})
        write(QStringLiteral("%1.md").arg(QLatin1String(name)), "[[Target]]\n");
    vault = Vault(dir.path());
    Index index;
    build(index);
    ASSERT_TRUE(QFile::remove(vault.absolute(QStringLiteral("B.md"))));

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Target.md")), QStringLiteral("Renamed"), &newPath,
                                 &error))
        << error.toStdString();
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("Target.md"), QStringLiteral("Renamed.md"), vault, &error), 2);
    EXPECT_TRUE(error.contains(QStringLiteral("B.md"))) << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("A.md"))), QByteArray("[[Renamed]]\n"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("C.md"))), QByteArray("[[Renamed]]\n"));
}

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

// `[x](<path with spaces.md>)` is CommonMark's spelling for a destination with
// spaces: the brackets are not part of the target, so it is a backlink, and a
// rename of the target rewrites what is inside them and keeps them.
TEST_F(RenameLinksTest, AnAngleBracketMarkdownLinkIsABacklinkAndKeepsItsBracketsOnRename)
{
    write(QStringLiteral("Old Note.md"), "# Old Note\n\n## Part\n");
    write(QStringLiteral("Home.md"), "[a](<Old Note.md>) and [b](<Old Note.md#Part>)\n");
    write(QStringLiteral("Sub/Deep.md"), "[c](<../Old Note.md>)\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    QStringList sources;
    for (const Backlink &backlink : index.backlinks(vault.absolute(QStringLiteral("Old Note.md"))))
        sources << backlink.fromPath;
    sources.sort();
    EXPECT_EQ(sources, QStringList({QStringLiteral("Home.md"), QStringLiteral("Home.md"),
                                    QStringLiteral("Sub/Deep.md")}));

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Old Note.md")), QStringLiteral("New Note"),
                                 &newPath, &error)) << error.toStdString();
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("Old Note.md"), QStringLiteral("New Note.md"),
                                           vault, &error), 2) << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Home.md"))),
              QByteArray("[a](<New Note.md>) and [b](<New Note.md#Part>)\n"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Sub/Deep.md"))), QByteArray("[c](<../New Note.md>)\n"));
}

// The index lags a save: a note edited after it was indexed reaches the rename
// with new text and old offsets. Links are found again in the text read, so
// the prose around them is never overwritten.
TEST_F(RenameLinksTest, ANoteEditedSinceItWasIndexedIsRewrittenFromItsCurrentText)
{
    write(QStringLiteral("Old.md"), "# Old\n");
    write(QStringLiteral("Home.md"), "See [[Old]] for more.\n");
    write(QStringLiteral("More.md"), "[[Old]]\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    // Typed at the top, and a second link, both unseen by the index.
    write(QStringLiteral("Home.md"), "Hello there, a long new line. [[Old#Part]] too.\nSee [[Old]] for more, ![[Old]].\n");
    write(QStringLiteral("More.md"), "A [x](Old.md) first.\n[[Old]]\n");

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Old.md")), QStringLiteral("New"), &newPath, &error))
        << error.toStdString();
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("Old.md"), QStringLiteral("New.md"), vault, &error), 2)
        << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Home.md"))),
              QByteArray("Hello there, a long new line. [[New#Part]] too.\nSee [[New]] for more, ![[New]].\n"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("More.md"))), QByteArray("A [x](New.md) first.\n[[New]]\n"));
}

// Vault::write refuses a lossy note. It is skipped and named, and the rest of
// the batch is still rewritten.
TEST_F(RenameLinksTest, ANonUTF8LinkingNoteIsSkippedAndNamedWhileTheRestAreRewritten)
{
    write(QStringLiteral("Old.md"), "# Old\n");
    const QByteArray lossy("caf\xe9 [[Old]]\n");
    write(QStringLiteral("A-lossy.md"), lossy);
    write(QStringLiteral("B-clean.md"), "[[Old]]\n");
    write(QStringLiteral("C-clean.md"), "[[Old]]\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.renamePath(vault.absolute(QStringLiteral("Old.md")), QStringLiteral("New"), &newPath, &error))
        << error.toStdString();
    error.clear();
    EXPECT_EQ(index.updateLinksAfterRename(QStringLiteral("Old.md"), QStringLiteral("New.md"), vault, &error), 2);
    EXPECT_TRUE(error.contains(QStringLiteral("A-lossy.md"))) << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("A-lossy.md"))), lossy);
    EXPECT_EQ(read(vault.absolute(QStringLiteral("B-clean.md"))), QByteArray("[[New]]\n"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("C-clean.md"))), QByteArray("[[New]]\n"));
}

// A moved note's own Markdown links are paths from its folder; Obsidian
// rewrites them so they still reach the same files. Wikilinks (found by name),
// URLs and same-note anchors are not paths and stay as written.
TEST_F(RenameLinksTest, MovingANoteKeepsItsRelativeMarkdownLinksPointingAtTheSameFiles)
{
    write(QStringLiteral("Top.md"), "# Top\n");
    write(QStringLiteral("A/a.md"), "# a\n");
    write(QStringLiteral("A/img/p.png"), "png");
    write(QStringLiteral("A/B/Keep.md"), "# Keep\n");
    write(QStringLiteral("A/n.md"),
          "# n\n\n[a](a.md) ![](img/p.png) [t](../Top.md) [u](img/p%20x.png#frag)\n"
          "[w](https://example.com/a.md) [h](#n) [[a]] [e](<img/p x.png>)\n");
    write(QStringLiteral("A/x.md"), "[bare](n.md) [rel](./n.md)\n");
    write(QStringLiteral("Other/o.md"), "[to n](../A/n.md) [to n again](../A/n.md#n)\n");
    write(QStringLiteral("A/img/p x.png"), "png");

    vault = Vault(dir.path());
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.movePath(vault.absolute(QStringLiteral("A/n.md")), vault.absolute(QStringLiteral("A/B")),
                               &newPath, &error)) << error.toStdString();
    ASSERT_GE(index.updateLinksAfterRename(QStringLiteral("A/n.md"), QStringLiteral("A/B/n.md"), vault, &error), 0)
        << error.toStdString();

    EXPECT_EQ(read(vault.absolute(QStringLiteral("A/B/n.md"))),
              QByteArray("# n\n\n[a](../a.md) ![](../img/p.png) [t](../../Top.md) [u](../img/p%20x.png#frag)\n"
                         "[w](https://example.com/a.md) [h](#n) [[a]] [e](<../img/p x.png>)\n"));
    // Links to the moved note: a bare name stays a name, a path stays a path.
    EXPECT_EQ(read(vault.absolute(QStringLiteral("A/x.md"))), QByteArray("[bare](n.md) [rel](B/n.md)\n"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Other/o.md"))),
              QByteArray("[to n](../A/B/n.md) [to n again](../A/B/n.md#n)\n"));

    // And up to the vault root again, from the rewritten state.
    ASSERT_TRUE(vault.movePath(vault.absolute(QStringLiteral("A/B/n.md")), dir.path(), &newPath, &error))
        << error.toStdString();
    ASSERT_GE(index.updateLinksAfterRename(QStringLiteral("A/B/n.md"), QStringLiteral("n.md"), vault, &error), 0)
        << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("n.md"))),
              QByteArray("# n\n\n[a](A/a.md) ![](A/img/p.png) [t](Top.md) [u](A/img/p%20x.png#frag)\n"
                         "[w](https://example.com/a.md) [h](#n) [[a]] [e](<A/img/p x.png>)\n"));
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Other/o.md"))),
              QByteArray("[to n](../n.md) [to n again](../n.md#n)\n"));
}

// A folder moves whole: links between its own files keep their bytes, links
// out of it are rewritten for the new depth.
TEST_F(RenameLinksTest, MovingAFolderRewritesOnlyTheLinksThatLeaveIt)
{
    write(QStringLiteral("Top.md"), "# Top\n");
    write(QStringLiteral("A/a.md"), "# a\n");
    write(QStringLiteral("A/img/p.png"), "png");
    write(QStringLiteral("A/n.md"), "[in](a.md) ![](img/p.png) [out](../Top.md)\n");
    write(QStringLiteral("Z/z.md"), "# z\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    QString error;
    QString newPath;
    ASSERT_TRUE(vault.movePath(vault.absolute(QStringLiteral("A")), vault.absolute(QStringLiteral("Z")), &newPath, &error))
        << error.toStdString();
    ASSERT_GE(index.updateLinksAfterRename(QStringLiteral("A"), QStringLiteral("Z/A"), vault, &error), 0)
        << error.toStdString();
    EXPECT_EQ(read(vault.absolute(QStringLiteral("Z/A/n.md"))),
              QByteArray("[in](a.md) ![](img/p.png) [out](../../Top.md)\n"));
}

// A note that is not valid UTF-8 (a Latin-1 export) is searched by what it
// says: each bad byte reads as the Latin-1 letter it most likely is, not as
// U+FFFD, and the file itself is never rewritten.
TEST_F(IndexTest, ANoteThatIsNotValidUTF8IsSearchedByItsLatin1Text)
{
    const QByteArray latin("caf\xE9 au lait, then the word zebra\n[[Ideas]]\n");
    write(QStringLiteral("Latin.md"), latin);
    write(QStringLiteral("Cut.md"), "a cut \xE2\x82 sequence then quokka\n"); // not one byte per bad character

    Index index;
    build(index);
    const auto found = [&](const QString &query) {
        QStringList paths;
        for (const SearchResult &result : index.search(query))
            paths << result.path;
        return paths;
    };
    EXPECT_EQ(found(QStringLiteral("zebra")), QStringList{QStringLiteral("Latin.md")});
    EXPECT_EQ(found(QStringLiteral("caf\u00E9")), QStringList{QStringLiteral("Latin.md")});
    EXPECT_EQ(found(QStringLiteral("quokka")), QStringList{QStringLiteral("Cut.md")});
    // Its links are still its links.
    bool linksIdeas = false;
    for (const Backlink &backlink : index.backlinks(vault.absolute(QStringLiteral("Ideas.md"))))
        linksIdeas = linksIdeas || backlink.fromPath == QLatin1String("Latin.md");
    EXPECT_TRUE(linksIdeas);

    QFile file(vault.absolute(QStringLiteral("Latin.md")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), latin);
}

// An alias is offered as [[Note|alias]]; with two notes of one name that Note
// has to be the path, or the link goes to whichever resolves first.
TEST_F(RenameLinksTest, AnAliasOfOneOfTwoSameNameNotesNamesItByPath)
{
    write(QStringLiteral("A/Dup.md"), "---\naliases: [First]\n---\n");
    write(QStringLiteral("B/Dup.md"), "---\naliases: [Second, Dup]\n---\n");
    write(QStringLiteral("Solo.md"), "---\naliases: [Lone]\n---\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    const QList<QPair<QString, QString>> aliases = index.aliases();
    EXPECT_TRUE(aliases.contains({QStringLiteral("First"), QStringLiteral("A/Dup")}));
    EXPECT_TRUE(aliases.contains({QStringLiteral("Second"), QStringLiteral("B/Dup")}));
    EXPECT_TRUE(aliases.contains({QStringLiteral("Lone"), QStringLiteral("Solo")}));
    // Each name the suggester would insert reaches the note it was offered for.
    EXPECT_EQ(index.resolve(QStringLiteral("A/Dup")), QStringLiteral("A/Dup.md"));
    EXPECT_EQ(index.resolve(QStringLiteral("B/Dup")), QStringLiteral("B/Dup.md"));
    // An alias equal to its note's own name does not take that name out of the plain list.
    EXPECT_EQ(index.linkTargets().count(QStringLiteral("Dup")), 1);
}

// Obsidian lists both: the note named Foo and Bar's alias Foo.
TEST_F(IndexTest, AnAliasThatIsAnotherNotesNameLeavesThatNoteCompletable)
{
    write(QStringLiteral("Foo.md"), "# Foo\n");
    write(QStringLiteral("Bar.md"), "---\naliases: [Foo, Baz]\n---\n");

    vault = Vault(dir.path());
    Index index;
    build(index);

    const QStringList targets = index.linkTargets();
    EXPECT_TRUE(targets.contains(QStringLiteral("Foo")));
    EXPECT_FALSE(targets.contains(QStringLiteral("Baz"))); // a plain alias is only offered as Bar|Baz
    EXPECT_TRUE(index.aliases().contains({QStringLiteral("Foo"), QStringLiteral("Bar")}));
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
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// The full-text table is keyed by the file's id. These guard it: a stale or
// duplicated row would be invisible until search used it and then look like a
// parser bug.
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

TEST_F(IndexTest, SearchFindsNotesByTheirProperties)
{
    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);
    // Ideas.md's frontmatter: aliases [Brainwaves], id 01J9, tags [thinking].
    QList<SearchResult> results = index.search(QStringLiteral("[aliases:brainwaves]"));
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results.first().path, QStringLiteral("Ideas.md"));
    EXPECT_EQ(index.search(QStringLiteral("[id]")).size(), 1);
    EXPECT_TRUE(index.search(QStringLiteral("[id:nothing]")).isEmpty());
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

TEST_F(IndexTest, UnlinkedMentionsReadAFrontMatterLongerThanTheFirstRead)
{
    // Only the start of the file is read for its front matter; one that runs
    // past it, with the alias at its end, is read whole.
    QByteArray head = "---\nnotes: \"" + QByteArray(40 * 1024, 'x') + "\"\naliases: [Longform]\n---\nbody\n";
    write(QStringLiteral("Long.md"), head);
    write(QStringLiteral("LongMention.md"), "Somebody wrote Longform here.\n");

    Index index;
    ASSERT_TRUE(index.open(dir.path()));
    build(index);

    QStringList paths;
    for (const SearchResult &result : index.unlinkedMentions(dir.filePath(QStringLiteral("Long.md"))))
        paths << result.path;
    EXPECT_TRUE(paths.contains(QStringLiteral("LongMention.md")));
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

    // Best of three searches of the one index: load only ever adds time.
    return perf::bestOf([&] {
        QElapsedTimer clock;
        clock.start();
        const QList<SearchResult> results = index.search(QStringLiteral("needle"), noteCount + 10);
        const double ms = clock.nsecsElapsed() / 1e6;
        EXPECT_EQ(results.size(), noteCount);
        return ms;
    });
}

} // namespace

TEST(SearchPerfTest, SearchTimeScalesWithVaultSizeNotQuadratically)
{
    // Asserted as a ratio, the way ParserPerfTest is: five times the notes
    // should cost roughly five times the work. This is the guard against a
    // confirm-and-locate pass that quietly becomes quadratic - e.g. by
    // re-scanning already-seen candidates, or by re-reading tags/headings/
    // tasks per note instead of once per search - the kind of bug this
    // project has hit twice already (see docs/PERFORMANCE.md, "Indexing and queries").
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
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
    // the whole index, 0.18 ms a note (docs/PERFORMANCE.md, "Indexing and queries").
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    QTemporaryDir dir;
    writeNotes(dir, QStringLiteral("Note"), 3000);
    // Best of three cold indexes (a fresh Index each): a loaded machine starves
    // the loop for a moment at random, which says nothing about the code, and
    // only ever adds to the longest wait.
    qint64 worst = std::numeric_limits<qint64>::max();
    for (int run = 0; run < 3 && worst >= 40; ++run) {
        Index index;
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(Vault(dir.path()));
        QElapsedTimer beat;
        qint64 runWorst = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { runWorst = std::max(runWorst, beat.restart()); });
        beat.start();
        heartbeat.start();
        ASSERT_TRUE(done.wait(60000));
        heartbeat.stop();
        worst = std::min(worst, runWorst);
        EXPECT_EQ(index.noteCount(), 3000);
        EXPECT_EQ(index.resolve(QStringLiteral("Note2999")), QStringLiteral("Note2999.md"));
    }
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

// The watches come from the scan's own walk: a note written into an existing
// subfolder after the scan, and one in a folder made after it, are both seen.
// A folder inotify cannot watch (here: no permission; in the wild, past
// fs.inotify.max_user_watches) used to be dropped without a word, and its
// outside changes were never seen. Now the indexer says so once and polls.
TEST(IndexerThreadTest, AFolderThatCannotBeWatchedTurnsOnPolling)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root ignores file permissions";
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("open")));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("closed")));
    {
        Index index;
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        QSignalSpy failed(&indexer, &Indexer::watchesFailed);
        indexer.setVault(Vault(dir.path()));
        ASSERT_TRUE(done.wait(20000));
        EXPECT_FALSE(indexer.pollsForChanges()) << "every folder is watched";
        EXPECT_TRUE(failed.isEmpty());
    }
    const QString closed = dir.filePath(QStringLiteral("closed"));
    ASSERT_TRUE(QFile::setPermissions(closed, QFileDevice::Permissions()));
    Index index;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    QSignalSpy failed(&indexer, &Indexer::watchesFailed);
    indexer.setVault(Vault(dir.path()));
    const bool finished = done.wait(20000);
    const bool polls = indexer.pollsForChanges();
    QFile::setPermissions(closed, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    ASSERT_TRUE(finished);
    EXPECT_TRUE(polls);
    ASSERT_EQ(failed.size(), 1);
    EXPECT_EQ(failed.first().first().toInt(), 1);
}

TEST(IndexerThreadTest, TheScansWalkWatchesEveryFolderIncludingNewOnes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("a/b")));
    const auto write = [&](const QString &relative, const QByteArray &text) {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(text);
    };
    write(QStringLiteral("a/b/First.md"), "# First\n");
    Index index;
    Indexer indexer(&index);
    QSignalSpy done(&indexer, &Indexer::finished);
    indexer.setVault(Vault(dir.path()));
    ASSERT_TRUE(done.wait(20000));

    write(QStringLiteral("a/b/Second.md"), "# Second\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return !index.resolve(QStringLiteral("Second")).isEmpty(); }, 5000))
        << "a note in a folder the scan walked";

    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("a/c")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return done.size() >= 3; }, 5000)) << "the new folder's rescan";
    write(QStringLiteral("a/c/Third.md"), "# Third\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return !index.resolve(QStringLiteral("Third")).isEmpty(); }, 5000))
        << "a note in a folder made after the first scan";
}

namespace {

/// A temp vault with a few notes in a subfolder, indexed and watched.
class WatchedVaultTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("a/b")));
        write(QStringLiteral("Root.md"), "# Root\n");
        write(QStringLiteral("a/b/First.md"), "# First\n");
        write(QStringLiteral("a/b/Second.md"), "# Second\n");
        indexer = std::make_unique<Indexer>(&index);
        QSignalSpy done(indexer.get(), &Indexer::finished);
        indexer->setVault(Vault(dir.path()));
        ASSERT_TRUE(done.wait(20000));
        // The watch-race rescan, if the walk saw a folder mid-change.
        QTest::qWait(500);
        walks = indexer->wholeScans();
    }

    void write(const QString &relative, const QByteArray &text)
    {
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).absolutePath()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(text);
    }

    bool found(const QString &name) const { return !index.resolve(name).isEmpty(); }

    QTemporaryDir dir;
    Index index;
    std::unique_ptr<Indexer> indexer;
    int walks = 0;
};

} // namespace

TEST_F(WatchedVaultTest, ANoteChangedOutsideTheAppIsReindexedWithoutAWholeWalk)
{
    {
        QSaveFile save(dir.filePath(QStringLiteral("a/b/First.md"))); // an editor's save: a temp file renamed over it
        ASSERT_TRUE(save.open(QIODevice::WriteOnly));
        save.write("# First\n\nzeppelin\n");
        ASSERT_TRUE(save.commit());
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return !index.search(QStringLiteral("zeppelin")).isEmpty(); }, 5000));
    EXPECT_EQ(indexer->wholeScans(), walks);
}

// The window refreshes its open notes' links on this, and the index says nothing
// for a file it does not keep as a note: an image, a CSV, a PDF.
TEST_F(WatchedVaultTest, AFileThatIsNoNoteComingAndGoingIsSignalled)
{
    QSignalSpy spy(indexer.get(), &Indexer::vaultFilesChanged);
    write(QStringLiteral("a/b/pic.png"), "not really a picture");
    ASSERT_TRUE(QTest::qWaitFor([&] { return spy.count() >= 1; }, 5000));
    spy.clear();
    ASSERT_TRUE(QFile::remove(dir.filePath(QStringLiteral("a/b/pic.png"))));
    ASSERT_TRUE(QTest::qWaitFor([&] { return spy.count() >= 1; }, 5000));
    spy.clear();
    ASSERT_TRUE(QDir(dir.filePath(QStringLiteral("a"))).removeRecursively()); // a whole folder, as a trash empties
    ASSERT_TRUE(QTest::qWaitFor([&] { return spy.count() >= 1; }, 5000));
}

TEST_F(WatchedVaultTest, ADeletedAttachmentStopsResolving)
{
    write(QStringLiteral("a/b/pic.png"), "not really a picture");
    ASSERT_TRUE(QTest::qWaitFor([&] { return found(QStringLiteral("pic.png")); }, 5000));
    ASSERT_TRUE(QFile::remove(dir.filePath(QStringLiteral("a/b/pic.png"))));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !found(QStringLiteral("pic.png")); }, 5000));
}

// Another program that opens a note, writes and closes it (no temp file, no
// rename) raises no folder entry event: inotify's IN_CLOSE_WRITE does.
TEST_F(WatchedVaultTest, ANoteWrittenInPlaceAndAppendedToIsReindexedWithoutAWholeWalk)
{
    {
        QFile file(dir.filePath(QStringLiteral("a/b/First.md")));
        ASSERT_TRUE(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
        file.write("# First\n\nzeppelin\n");
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return !index.search(QStringLiteral("zeppelin")).isEmpty(); }, 5000));
    {
        QFile file(dir.filePath(QStringLiteral("a/b/First.md")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Append));
        file.write("\nblimp\n");
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return !index.search(QStringLiteral("blimp")).isEmpty(); }, 5000));
    EXPECT_FALSE(index.search(QStringLiteral("zeppelin")).isEmpty()) << "the earlier text is still there";
    EXPECT_EQ(indexer->wholeScans(), walks);
}

// A moved folder's watches follow the inodes, not the names: the folder and the
// one inside it must both be watched again under the new names.
TEST_F(WatchedVaultTest, ANoteWrittenInPlaceInsideARenamedFolderIsSeen)
{
    write(QStringLiteral("a/b/sub/Inner.md"), "# Inner\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return found(QStringLiteral("Inner")); }, 5000));
    ASSERT_TRUE(QDir(dir.path()).rename(QStringLiteral("a/b"), QStringLiteral("a/c")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return index.resolve(QStringLiteral("Inner")) == QStringLiteral("a/c/sub/Inner.md"); },
                                5000));
    {
        QFile file(dir.filePath(QStringLiteral("a/c/sub/Inner.md")));
        ASSERT_TRUE(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
        file.write("# Inner\n\nzeppelin\n");
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return !index.search(QStringLiteral("zeppelin")).isEmpty(); }, 5000));
    EXPECT_EQ(indexer->wholeScans(), walks);
}

TEST_F(WatchedVaultTest, ANewNoteAndANewFolderWithNotesAreFoundWithoutAWholeWalk)
{
    write(QStringLiteral("a/b/Third.md"), "# Third\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return found(QStringLiteral("Third")); }, 5000));
    // Folder and note made together, two levels deep: no watch exists for it yet.
    write(QStringLiteral("x/y/Deep.md"), "# Deep\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return found(QStringLiteral("Deep")); }, 5000));
    // And the new folders are watched from then on.
    write(QStringLiteral("x/y/Later.md"), "# Later\n");
    ASSERT_TRUE(QTest::qWaitFor([&] { return found(QStringLiteral("Later")); }, 5000));
    EXPECT_EQ(indexer->wholeScans(), walks);
}

TEST_F(WatchedVaultTest, ADeletedNoteAndADeletedFolderLeaveTheIndexWithoutAWholeWalk)
{
    ASSERT_TRUE(QFile::remove(dir.filePath(QStringLiteral("a/b/First.md"))));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !found(QStringLiteral("First")); }, 5000));
    EXPECT_TRUE(found(QStringLiteral("Second")));
    ASSERT_TRUE(QDir(dir.filePath(QStringLiteral("a"))).removeRecursively());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !found(QStringLiteral("Second")); }, 5000))
        << "a note in a folder that went with its folder";
    EXPECT_TRUE(found(QStringLiteral("Root")));
    EXPECT_EQ(indexer->wholeScans(), walks);
}

TEST_F(WatchedVaultTest, ARenamedFolderMovesItsNotesWithoutAWholeWalk)
{
    ASSERT_TRUE(QDir(dir.path()).rename(QStringLiteral("a/b"), QStringLiteral("a/c")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return index.resolve(QStringLiteral("Second")) == QStringLiteral("a/c/Second.md"); },
                                5000));
    EXPECT_EQ(index.noteCount(), 3) << "nothing left under the old name";
    EXPECT_EQ(indexer->wholeScans(), walks);
}

// saveLeaf() reindexes the note itself; the folder change its save also causes
// is read for that folder alone, and finds nothing left to do.
TEST_F(WatchedVaultTest, AnInAppSaveCausesNoWholeWalkAndNoSecondChange)
{
    const QString path = dir.filePath(QStringLiteral("a/b/First.md"));
    QSignalSpy done(indexer.get(), &Indexer::finished);
    QSignalSpy changed(&index, &Index::changed);
    {
        QSaveFile save(path); // as Vault::write: a temp file renamed over the note
        ASSERT_TRUE(save.open(QIODevice::WriteOnly));
        save.write("# First\n\nsaved in the app\n");
        ASSERT_TRUE(save.commit());
    }
    indexer->reindex(path);
    ASSERT_TRUE(QTest::qWaitFor([&] { return done.size() >= 2; }, 5000)) << "the note's scan, then the folder's";
    QTest::qWait(500);
    EXPECT_EQ(indexer->wholeScans(), walks);
    EXPECT_EQ(done.size(), 2);
    EXPECT_EQ(changed.size(), 1) << "the folder's read found the note current";
    EXPECT_FALSE(index.search(QStringLiteral("saved in the app")).isEmpty());
}

namespace {
/// The eight per-file child tables of the schema: each is deleted by `file` when a note is re-put.
const QStringList kChildTables = {QStringLiteral("headings"),   QStringLiteral("links"),
                                  QStringLiteral("tags"),       QStringLiteral("blocks"),
                                  QStringLiteral("tasks"),      QStringLiteral("props"),
                                  QStringLiteral("meta_notes"), QStringLiteral("names")};

/// Names of the indexes the cache file holds, read on a connection of its own.
QStringList indexNamesIn(const QString &cache)
{
    QStringList names;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("index-names"));
        db.setDatabaseName(cache);
        if (db.open()) {
            QSqlQuery query(db);
            query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'index'"));
            while (query.next())
                names << query.value(0).toString();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("index-names"));
    return names;
}

bool hasAllChildIndexes(const QString &cache)
{
    const QStringList names = indexNamesIn(cache);
    return std::all_of(kChildTables.cbegin(), kChildTables.cend(),
                       [&](const QString &table) { return names.contains(table + QStringLiteral("_file")); });
}

QStringList capturedWarnings;
void captureWarnings(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg)
        capturedWarnings << message;
}
} // namespace

// Deleting a note's row cascades into eight child tables. Without an index on
// each `file` column that is a full scan of each table per deleted note, so a
// save, and a re-index of a vault whose notes all changed, were quadratic.
TEST_F(IndexTest, ACacheWrittenBeforeTheChildIndexesGainsThemAndKeepsItsRows)
{
    const QString cache = Index::cachePathFor(dir.path());
    {
        Index first;
        build(first);
    }
    ASSERT_TRUE(hasAllChildIndexes(cache)) << "a from-scratch build ends with them";
    // The cache as an older build left it: same schema version, no child indexes.
    {
        QSqlDatabase old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("old-cache"));
        old.setDatabaseName(cache);
        ASSERT_TRUE(old.open());
        QSqlQuery query(old);
        for (const QString &table : kChildTables)
            ASSERT_TRUE(query.exec(QStringLiteral("DROP INDEX IF EXISTS %1_file").arg(table)));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("old-cache"));
    for (const QString &table : kChildTables)
        ASSERT_FALSE(indexNamesIn(cache).contains(table + QStringLiteral("_file")));

    // Every note changed since: the scan re-puts all of them, each a cascade delete.
    write(QStringLiteral("Home.md"), "# Home\n\nSee [[Projects/noteAppC]] and [[Ideas|my ideas]]. Edited.\n\n#home\n");
    write(QStringLiteral("Ideas.md"), "---\naliases: [Brainwaves]\nid: 01J9\n---\n\nBack to [[Home]]. Edited.\n");

    Index index;
    build(index);
    const QStringList names = indexNamesIn(cache);
    for (const QString &table : kChildTables)
        EXPECT_TRUE(names.contains(table + QStringLiteral("_file"))) << qPrintable(table);
    EXPECT_EQ(index.noteCount(), 3);
    EXPECT_EQ(index.resolve(QStringLiteral("Brainwaves")), QStringLiteral("Ideas.md"));
    EXPECT_FALSE(index.allTags().contains(QStringLiteral("thinking"))) << "the old row's tags went with it";
    EXPECT_EQ(index.backlinks(vault.absolute(QStringLiteral("Home.md"))).size(), 2);
    EXPECT_EQ(index.contentRowCount(), index.noteCount());
}

TEST_F(IndexTest, ReindexingEveryChangedNoteCostsAboutWhatAColdIndexDoes)
{
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    constexpr int kNotes = 1500;
    QDir(dir.path()).mkpath(QStringLiteral("bulk"));
    const auto writeAll = [&](const char *tail) {
        for (int i = 0; i < kNotes; ++i) {
            QByteArray body = "# Note " + QByteArray::number(i) + "\n\n#tag" + QByteArray::number(i % 40) + "\n\n";
            for (int link = 1; link <= 6; ++link)
                body += "- See [[Note " + QByteArray::number((i * 7 + link * 13) % kNotes) + "]]\n";
            write(QStringLiteral("bulk/Note %1.md").arg(i), body + tail);
        }
    };
    writeAll("");
    Index index;
    QElapsedTimer clock;
    clock.start();
    build(index);
    const double cold = double(clock.elapsed());
    ASSERT_GE(index.noteCount(), kNotes);

    writeAll("changed\n"); // a different size, so every row is stale
    clock.start();
    build(index);
    const double stale = double(clock.elapsed());
    EXPECT_EQ(index.contentRowCount(), index.noteCount());
    // Quadratic it was 40x and worse; the bound is generous so a busy machine does not trip it.
    EXPECT_LT(stale, cold * 4 + 100) << "cold " << cold << " ms, every note stale " << stale << " ms";
}

// A whole walk checks every file against the cache; that is one read of the
// table, and it must still tell an unchanged file from a changed one, and a
// note from a plain file the cache holds under the same path.
TEST_F(IndexTest, AWarmWalkReindexesExactlyTheFilesThatChanged)
{
    const auto scan = [&](Index &index) {
        Indexer indexer(&index);
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        EXPECT_TRUE(done.wait(20000));
        return done.isEmpty() ? -1 : done.first().first().toInt();
    };
    Index index;
    EXPECT_EQ(scan(index), 3);
    EXPECT_EQ(scan(index), 0) << "nothing changed";

    write(QStringLiteral("Home.md"), "# Home\n\nEdited, and longer than before.\n");
    write(QStringLiteral("Added.md"), "# Added\n");
    write(QStringLiteral("Projects/notes.txt"), "still not a note, but different\n");
    EXPECT_EQ(scan(index), 2) << "the edited note and the new one; the changed .txt is an attachment";
    EXPECT_EQ(scan(index), 0);
    EXPECT_EQ(index.noteCount(), 4);
    EXPECT_EQ(index.attachmentCount(), 1);
    EXPECT_EQ(index.contentRowCount(), index.noteCount());
}

// graphData() resolves each link from the linking note's vault-relative path;
// that must stay what resolve() says for the same note, folder preference
// between two notes of one name included.
TEST_F(IndexTest, GraphLinksResolveAsResolveDoesFromTheLinkingNote)
{
    write(QStringLiteral("Projects/Home.md"), "# Other home\n\nSee [[Ideas]] and [[Projects/noteAppC]].\n");
    write(QStringLiteral("Projects/Linker.md"), "[[Home]] [[../Ideas]] [[Missing]]\n");
    Index index;
    build(index);
    const Index::GraphData data = index.graphData();
    ASSERT_GE(data.links.size(), 7);
    bool sawFolderPreference = false;
    for (const Index::GraphLinkInfo &link : data.links) {
        EXPECT_EQ(link.toPath, index.resolve(link.targetName, vault.absolute(link.fromPath)))
            << qPrintable(link.fromPath + QStringLiteral(" -> ") + link.targetName);
        if (link.fromPath == QStringLiteral("Projects/Linker.md") && link.targetName == QStringLiteral("Home")) {
            EXPECT_EQ(link.toPath, QStringLiteral("Projects/Home.md")) << "the linking note's own folder wins";
            sawFolderPreference = true;
        }
    }
    EXPECT_TRUE(sawFolderPreference);
}

// The scan thread makes a from-scratch build's indexes before it reports the scan done: afterwards the UI thread
// writes to the cache, and DDL still in flight on the scan thread could fail that write. Read at the moment
// finished() lands, on a connection of its own, so the Writer destructor's fallback cannot hide a late finish().
TEST_F(IndexTest, TheChildIndexesExistWhenFinishedLandsAndMakingThemWarnsOfNothing)
{
    const QString cache = Index::cachePathFor(dir.path());
    // Enough rows that making the indexes takes longer than the UI thread needs to notice the scan is done.
    QDir(dir.path()).mkpath(QStringLiteral("bulk"));
    for (int i = 0; i < 3000; ++i) {
        write(QStringLiteral("bulk/N%1.md").arg(i),
              QStringLiteral("# N%1\n\n#t%2 [[N%3]] [[N%4]] [[Home]]\n\n- [ ] a task\n").arg(i).arg(i % 50).arg((i + 1) % 3000).arg((i * 7) % 3000).toUtf8());
    }
    capturedWarnings.clear();
    const QtMessageHandler previous = qInstallMessageHandler(captureWarnings);
    bool atFinished = false;
    {
        Index index;
        Indexer indexer(&index);
        QObject::connect(&indexer, &Indexer::finished, &indexer, [&] { atFinished = hasAllChildIndexes(cache); });
        QSignalSpy done(&indexer, &Indexer::finished);
        indexer.setVault(vault);
        ASSERT_TRUE(done.wait(20000));
    }
    qInstallMessageHandler(previous);
    EXPECT_TRUE(atFinished);
    EXPECT_TRUE(capturedWarnings.isEmpty()) << qPrintable(capturedWarnings.join(QLatin1Char('\n')));
}

// A UI batch that reads and then writes must not lose the write because another connection committed in
// between (SQLite answers such an upgrade "database is locked" at once, without waiting): the batch takes the
// write lock when it begins. The other connection does not wait here, so it is the one that is refused.
TEST_F(IndexTest, ABatchKeepsItsWriteWhenAnotherConnectionWantsToWriteFirst)
{
    Index index;
    build(index);
    const int before = index.noteCount();
    ASSERT_GT(before, 0);

    index.beginBatch();
    (void)index.noteCount(); // the batch has read: it holds a snapshot
    {
        QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("other-writer"));
        other.setDatabaseName(Index::cachePathFor(dir.path()));
        other.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=0"));
        ASSERT_TRUE(other.open());
        QSqlQuery query(other);
        query.exec(QStringLiteral("INSERT OR REPLACE INTO state(key, value) VALUES('other', '1')")); // refused or not
    }
    QSqlDatabase::removeDatabase(QStringLiteral("other-writer"));
    index.remove(vault.absolute(QStringLiteral("Home.md")));
    index.endBatch();
    EXPECT_EQ(index.noteCount(), before - 1) << "the batch's own write went through";
}
