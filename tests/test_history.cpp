// Note history in git: libs/ext/history. Every test gets its own data home,
// so nothing reaches ~/.local/share.

#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "NoteHistory.h"
#include "Perf.h"

using noteAppC::NoteHistory;

namespace {

class HistoryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(data.isValid());
        qputenv("XDG_DATA_HOME", data.path().toUtf8());
    }
    QTemporaryDir data;
    QTemporaryDir vault;
};

} // namespace

TEST_F(HistoryTest, EverySaveIsAVersionNewestFirstAndAnUnchangedSaveIsNot)
{
    NoteHistory history(vault.path());
    ASSERT_TRUE(history.isOpen()) << history.error().toStdString();
    EXPECT_FALSE(NoteHistory::repositoryPathFor(vault.path()).startsWith(vault.path()))
        << "the history never lives in the vault";

    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'800'000'000);
    ASSERT_TRUE(history.record(QStringLiteral("Folder/Note.md"), QStringLiteral("one\n"), t0));
    ASSERT_TRUE(history.record(QStringLiteral("Folder/Note.md"), QStringLiteral("one\ntwo\n"), t0.addSecs(60)));
    ASSERT_TRUE(history.record(QStringLiteral("Folder/Note.md"), QStringLiteral("one\ntwo\n"), t0.addSecs(120)));
    ASSERT_TRUE(history.record(QStringLiteral("Other.md"), QStringLiteral("other\n"), t0));

    const auto versions = history.versions(QStringLiteral("Folder/Note.md"));
    ASSERT_EQ(versions.size(), 2);
    EXPECT_EQ(versions.at(0).time, t0.addSecs(60));
    EXPECT_EQ(history.read(QStringLiteral("Folder/Note.md"), versions.at(0).id), QStringLiteral("one\ntwo\n"));
    EXPECT_EQ(history.read(QStringLiteral("Folder/Note.md"), versions.at(1).id), QStringLiteral("one\n"));
    EXPECT_EQ(history.versions(QStringLiteral("Other.md")).size(), 1);
    EXPECT_TRUE(history.versions(QStringLiteral("Missing.md")).isEmpty());

    // A second instance - the next launch - sees the same history.
    NoteHistory again(vault.path());
    EXPECT_EQ(again.versions(QStringLiteral("Folder/Note.md")).size(), 2);
}

TEST_F(HistoryTest, TheRepositoryIsCreatedByTheFirstSaveNotByOpeningTheVault)
{
    const QString repository = NoteHistory::repositoryPathFor(vault.path());
    {
        NoteHistory history(vault.path());
        EXPECT_TRUE(history.isOpen());
        EXPECT_TRUE(history.versions(QStringLiteral("Note.md")).isEmpty());
        EXPECT_EQ(history.looseObjects(), 0);
    }
    EXPECT_FALSE(QFileInfo::exists(repository)) << "nothing was saved, so nothing is on disk";
    EXPECT_FALSE(QDir(QFileInfo(repository).path()).exists());

    NoteHistory history(vault.path());
    ASSERT_TRUE(history.record(QStringLiteral("Note.md"), QStringLiteral("one\n")));
    EXPECT_TRUE(QFileInfo(repository).isDir());
    EXPECT_EQ(history.versions(QStringLiteral("Note.md")).size(), 1);
    NoteHistory again(vault.path());
    EXPECT_EQ(again.versions(QStringLiteral("Note.md")).size(), 1);
}

TEST_F(HistoryTest, ARenameCarriesTheHistoryAndOldVersionsStillRead)
{
    NoteHistory history(vault.path());
    ASSERT_TRUE(history.record(QStringLiteral("Old.md"), QStringLiteral("before\n")));
    ASSERT_TRUE(history.rename(QStringLiteral("Old.md"), QStringLiteral("Sub/New.md")));
    ASSERT_TRUE(history.record(QStringLiteral("Sub/New.md"), QStringLiteral("after\n")));
    const auto versions = history.versions(QStringLiteral("Sub/New.md"));
    ASSERT_EQ(versions.size(), 2);
    EXPECT_EQ(history.read(QStringLiteral("Sub/New.md"), versions.at(1).id), QStringLiteral("before\n"));
    EXPECT_TRUE(history.versions(QStringLiteral("Old.md")).isEmpty());
}

TEST_F(HistoryTest, PackingKeepsEveryVersionAndStoresThemAsDeltas)
{
    NoteHistory history(vault.path());
    QString text;
    for (int line = 0; line < 400; ++line)
        text += QStringLiteral("Line %1 of a note long enough for deltas to matter.\n").arg(line);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'800'000'000);
    for (int save = 0; save < 50; ++save) {
        text.replace(QStringLiteral("Line %1 ").arg(save), QStringLiteral("Edited line %1 ").arg(save));
        ASSERT_TRUE(history.record(QStringLiteral("Note.md"), text, t0.addSecs(save)));
    }
    EXPECT_GT(history.looseObjects(), 100);
    ASSERT_TRUE(history.pack()) << history.error().toStdString();
    EXPECT_EQ(history.looseObjects(), 0);

    const auto versions = history.versions(QStringLiteral("Note.md"));
    ASSERT_EQ(versions.size(), 50);
    EXPECT_EQ(history.read(QStringLiteral("Note.md"), versions.first().id), text);
    EXPECT_TRUE(history.read(QStringLiteral("Note.md"), versions.last().id).contains(QStringLiteral("Edited line 0 ")));

    // Fifty 21 kB versions, packed: far less than fifty copies.
    qint64 packed = 0;
    for (const QFileInfo &file :
         QDir(NoteHistory::repositoryPathFor(vault.path()) + QStringLiteral("/objects/pack")).entryInfoList(QDir::Files))
        packed += file.size();
    std::printf("[ SIZE     ] 50 versions of a %lld-byte note: %lld bytes packed\n",
                (long long)text.toUtf8().size(), (long long)packed);
    EXPECT_LT(packed, text.toUtf8().size() * 5);
}

TEST_F(HistoryTest, ASaveCostsWellUnderAMillisecond)
{
    NoteHistory history(vault.path());
    QString text;
    for (int line = 0; line < 200; ++line)
        text += QStringLiteral("A line of a 10 kB note, number %1.\n").arg(line);
    SKIP_WALL_CLOCK_GUARD_UNDER_SANITIZER();
    bool allRecorded = true;
    double worst = 0;
    // Best of three medians: load only ever adds time.
    const double median = perf::bestOf([&] {
        std::vector<double> samples;
        for (int save = 0; save < 200; ++save) {
            text += QStringLiteral("x");
            QElapsedTimer clock;
            clock.start();
            allRecorded = history.record(QStringLiteral("Deep/Folder/Note.md"), text) && allRecorded;
            samples.push_back(double(clock.nsecsElapsed()) / 1e6);
        }
        std::sort(samples.begin(), samples.end());
        worst = samples.back();
        return samples[samples.size() / 2];
    }, 3, 1.0);
    ASSERT_TRUE(allRecorded);
    std::printf("[ PERF     ] one save recorded: median %.3f ms, worst %.3f ms\n", median, worst);
    EXPECT_LT(median, 1.0); // and it runs off the UI thread besides
}
