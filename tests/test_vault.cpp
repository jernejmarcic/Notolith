#include <gtest/gtest.h>
#include "Snapshots.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "Vault.h"

using noteAppC::Note;
using noteAppC::Vault;

namespace {

QByteArray roundTrip(const QByteArray &bytes)
{
    return Vault::encode(Vault::decode(bytes));
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

} // namespace

// Non-negotiable 1: an untouched note must survive open-save byte for byte.
TEST(VaultRoundTrip, PlainLfIsByteIdentical)
{
    const QByteArray bytes = "# Title\n\nBody with [[a wikilink]].\n";
    EXPECT_EQ(roundTrip(bytes), bytes);
}

TEST(VaultRoundTrip, CrlfIsPreserved)
{
    const QByteArray bytes = "# Title\r\n\r\nBody.\r\n";
    EXPECT_EQ(roundTrip(bytes), bytes);
    EXPECT_TRUE(Vault::decode(bytes).crlf);
    EXPECT_EQ(Vault::decode(bytes).text, QStringLiteral("# Title\n\nBody.\n"));
}

TEST(VaultRoundTrip, BomIsPreserved)
{
    const QByteArray bytes = QByteArray("\xEF\xBB\xBF") + "# Title\n";
    EXPECT_EQ(roundTrip(bytes), bytes);
    EXPECT_TRUE(Vault::decode(bytes).bom);
    EXPECT_EQ(Vault::decode(bytes).text, QStringLiteral("# Title\n"));
}

TEST(VaultRoundTrip, BomAndCrlfTogether)
{
    const QByteArray bytes = QByteArray("\xEF\xBB\xBF") + "a\r\nb\r\n";
    EXPECT_EQ(roundTrip(bytes), bytes);
}

TEST(VaultRoundTrip, Utf8AndEmojiSurvive)
{
    const QByteArray bytes = "Zalec, crnina, \xF0\x9F\x93\x9D\n";
    EXPECT_EQ(roundTrip(bytes), bytes);
}

TEST(VaultRoundTrip, NoTrailingNewlineIsNotAdded)
{
    const QByteArray bytes = "no newline at end";
    EXPECT_EQ(roundTrip(bytes), bytes);
}

TEST(VaultRoundTrip, EmptyFile)
{
    EXPECT_EQ(roundTrip(QByteArray()), QByteArray());
}

// Documented ceiling: a mixed-ending file normalises to LF. Pinned so the day
// someone fixes it, the test says so.
TEST(VaultRoundTrip, MixedEndingsNormaliseToLf)
{
    const QByteArray bytes = "a\r\nb\nc\r\n";
    EXPECT_FALSE(Vault::decode(bytes).crlf);
    EXPECT_EQ(roundTrip(bytes), QByteArray("a\nb\nc\n"));
}

TEST(VaultIo, WriteIsAtomicAndReadsBack)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("note.md"));

    Note note;
    note.text = QStringLiteral("# Hello\n");
    QString error;
    ASSERT_TRUE(Vault::write(path, note, &error)) << error.toStdString();
    EXPECT_EQ(readAll(path), QByteArray("# Hello\n"));

    Note back;
    ASSERT_TRUE(Vault::read(path, back, &error)) << error.toStdString();
    EXPECT_EQ(back.text, note.text);

    // No temporary left behind by QSaveFile.
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

TEST(VaultIo, EditKeepsTheFilesLineEndings)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("crlf.md"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("one\r\ntwo\r\n");
    }

    Note note;
    QString error;
    ASSERT_TRUE(Vault::read(path, note, &error));
    note.text.replace(QStringLiteral("two"), QStringLiteral("three"));
    ASSERT_TRUE(Vault::write(path, note, &error));

    EXPECT_EQ(readAll(path), QByteArray("one\r\nthree\r\n"));
}

TEST(VaultIo, ReadReportsAMissingFile)
{
    Note note;
    QString error;
    EXPECT_FALSE(Vault::read(QStringLiteral("/nonexistent/nope.md"), note, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(VaultPaths, NoteExtensions)
{
    EXPECT_TRUE(Vault::isNote(QStringLiteral("/v/a.md")));
    EXPECT_TRUE(Vault::isNote(QStringLiteral("/v/a.MD")));
    EXPECT_TRUE(Vault::isNote(QStringLiteral("/v/a.markdown")));
    EXPECT_FALSE(Vault::isNote(QStringLiteral("/v/a.png")));
    EXPECT_FALSE(Vault::isNote(QStringLiteral("/v/a")));
}

TEST(VaultPaths, RelativeAndContains)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("sub")));

    Vault vault(dir.path());
    ASSERT_TRUE(vault.isValid());
    EXPECT_FALSE(vault.hasObsidianConfig());

    const QString note = dir.filePath(QStringLiteral("sub/note.md"));
    EXPECT_EQ(vault.relative(note), QStringLiteral("sub/note.md"));
    EXPECT_EQ(vault.absolute(QStringLiteral("sub/note.md")), note);
    EXPECT_TRUE(vault.contains(note));
    EXPECT_FALSE(vault.contains(QStringLiteral("/etc/passwd")));

    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    EXPECT_TRUE(vault.hasObsidianConfig());
}

// --- file operations -------------------------------------------------------

TEST(VaultFileOps, CreateNoteUniquifiesLikeObsidian)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;

    const QString first = vault.createNote(QString(), QStringLiteral("Untitled"), &error);
    ASSERT_FALSE(first.isEmpty()) << error.toStdString();
    EXPECT_EQ(first, dir.filePath(QStringLiteral("Untitled.md")));

    const QString second = vault.createNote(QString(), QStringLiteral("Untitled"), &error);
    EXPECT_EQ(second, dir.filePath(QStringLiteral("Untitled 1.md")));

    const QString third = vault.createNote(QString(), QStringLiteral("Untitled"), &error);
    EXPECT_EQ(third, dir.filePath(QStringLiteral("Untitled 2.md")));

    Note note;
    ASSERT_TRUE(Vault::read(first, note, &error));
    EXPECT_TRUE(note.text.isEmpty());
}

TEST(VaultFileOps, CreateNoteMakesItsFolder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;

    const QString path = vault.createNote(QStringLiteral("Sub/Folder"), QStringLiteral("Note"), &error);
    ASSERT_FALSE(path.isEmpty()) << error.toStdString();
    EXPECT_EQ(path, dir.filePath(QStringLiteral("Sub/Folder/Note.md")));
    EXPECT_TRUE(QFileInfo::exists(path));
}

TEST(VaultFileOps, CreateFolderUniquifies)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;

    const QString first = vault.createFolder(QString(), QStringLiteral("New Folder"), &error);
    ASSERT_FALSE(first.isEmpty()) << error.toStdString();
    const QString second = vault.createFolder(QString(), QStringLiteral("New Folder"), &error);
    EXPECT_EQ(second, dir.filePath(QStringLiteral("New Folder 1")));
    EXPECT_TRUE(QFileInfo(second).isDir());
}

TEST(VaultFileOps, RenamePathRefusesInvalidCharacters)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QString(), QStringLiteral("Note"), &error);
    ASSERT_FALSE(note.isEmpty());

    QString newPath;
    EXPECT_FALSE(vault.renamePath(note, QStringLiteral("Bad/Name"), &newPath, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(vault.renamePath(note, QStringLiteral("Bad#Name"), &newPath, &error));
    EXPECT_FALSE(vault.renamePath(note, QStringLiteral("Bad[Name]"), &newPath, &error));
    // The original file is still exactly where it was.
    EXPECT_TRUE(QFileInfo::exists(note));
}

TEST(VaultFileOps, RenamePathRefusesAnExistingName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    ASSERT_FALSE(vault.createNote(QString(), QStringLiteral("One"), &error).isEmpty());
    const QString two = vault.createNote(QString(), QStringLiteral("Two"), &error);
    ASSERT_FALSE(two.isEmpty());

    QString newPath;
    EXPECT_FALSE(vault.renamePath(two, QStringLiteral("One"), &newPath, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(VaultFileOps, RenamePathKeepsExtensionAndContent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QString(), QStringLiteral("Old Name"), &error);
    ASSERT_FALSE(note.isEmpty());
    Note contents;
    contents.text = QStringLiteral("# Old Name\n\nBody.\n");
    ASSERT_TRUE(Vault::write(note, contents, &error));

    QString newPath;
    ASSERT_TRUE(vault.renamePath(note, QStringLiteral("New Name"), &newPath, &error)) << error.toStdString();
    EXPECT_EQ(newPath, dir.filePath(QStringLiteral("New Name.md")));
    EXPECT_FALSE(QFileInfo::exists(note));

    Note reread;
    ASSERT_TRUE(Vault::read(newPath, reread, &error));
    EXPECT_EQ(reread.text, contents.text);
}

TEST(VaultFileOps, MoveToTrashKeepsFolderStructureAndSuffixesOnCollision)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QStringLiteral("Sub"), QStringLiteral("Note"), &error);
    ASSERT_FALSE(note.isEmpty());

    ASSERT_TRUE(vault.moveToTrash(note, &error)) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(note));
    const QString trashed = dir.filePath(QStringLiteral(".trash/Sub/Note.md"));
    EXPECT_TRUE(QFileInfo::exists(trashed));

    // Trash a second, unrelated file that happens to land on the same name.
    const QString again = vault.createNote(QStringLiteral("Sub"), QStringLiteral("Note"), &error);
    ASSERT_FALSE(again.isEmpty());
    ASSERT_TRUE(vault.moveToTrash(again, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(QStringLiteral(".trash/Sub/Note 1.md"))));
}

// --- ignore filters ----------------------------------------------------

TEST(VaultIgnoreFilters, UnconfiguredVaultSkipsBuildDirectoriesByDefault)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());

    EXPECT_TRUE(vault.isIgnored(QStringLiteral("build/output.md")));
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("projects/app/build-release/out.o")));
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("Projects/notes.md")));
}

TEST(VaultIgnoreFilters, ConfiguredFiltersMatchPrefixAndRegexAndDropTheBuildDefault)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));

    QJsonArray filters;
    filters.append(QStringLiteral("Templates/"));
    // A JavaScript regex literal, as Obsidian stores it.
    filters.append(QStringLiteral("/(^|\\/)node_modules\\//"));
    QJsonObject appJson;
    appJson[QStringLiteral("userIgnoreFilters")] = filters;

    QFile file(dir.filePath(QStringLiteral(".obsidian/app.json")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(appJson).toJson());
    file.close();

    Vault vault(dir.path()); // filters are read at construction time

    EXPECT_TRUE(vault.isIgnored(QStringLiteral("Templates/daily.md")));
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("src/node_modules/pkg/index.js")));
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("node_modules/pkg/index.js")));
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("Projects/notes.md")));
    // Filters are configured, so the build*/ fallback no longer applies.
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("build/output.md")));
}

// --- file recovery -------------------------------------------------------------

namespace {

/// A data home of its own, so snapshots from one test are not another's.
class SnapshotsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_home.isValid());
        m_previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", m_home.path().toUtf8());
    }
    void TearDown() override { qputenv("XDG_DATA_HOME", m_previous); }
    QTemporaryDir m_home;
    QByteArray m_previous;
};

} // namespace

TEST_F(SnapshotsTest, OnePerIntervalAndNeverTheSameTextTwice)
{
    const QString note = QStringLiteral("/vault/Note.md");
    const qint64 minute = 60'000;
    EXPECT_TRUE(noteAppC::Snapshots::record(note, QStringLiteral("one"), 5 * minute, 1'000'000));
    EXPECT_FALSE(noteAppC::Snapshots::record(note, QStringLiteral("two"), 5 * minute, 1'000'000 + minute))
        << "inside the interval";
    EXPECT_FALSE(noteAppC::Snapshots::record(note, QStringLiteral("one"), 5 * minute,
                                             1'000'000 + 10 * minute))
        << "nothing changed";
    EXPECT_TRUE(noteAppC::Snapshots::record(note, QStringLiteral("three"), 5 * minute,
                                            1'000'000 + 10 * minute));

    const QList<noteAppC::Snapshots::Version> versions = noteAppC::Snapshots::versions(note);
    ASSERT_EQ(versions.size(), 2);
    EXPECT_EQ(noteAppC::Snapshots::read(versions.at(0)), QStringLiteral("three")) << "newest first";
    EXPECT_EQ(noteAppC::Snapshots::read(versions.at(1)), QStringLiteral("one"));
    EXPECT_EQ(noteAppC::Snapshots::notes(), QStringList{note});
    EXPECT_TRUE(noteAppC::Snapshots::directory().startsWith(m_home.path())) << "outside the vault";
}

TEST_F(SnapshotsTest, OldVersionsArePrunedAndTheirNoteForgotten)
{
    const qint64 day = 24LL * 3600 * 1000;
    noteAppC::Snapshots::record(QStringLiteral("/v/Old.md"), QStringLiteral("old"), 0, 10 * day);
    noteAppC::Snapshots::record(QStringLiteral("/v/New.md"), QStringLiteral("new"), 0, 19 * day);
    EXPECT_EQ(noteAppC::Snapshots::prune(7 * day, 20 * day), 1);
    EXPECT_EQ(noteAppC::Snapshots::notes(), QStringList{QStringLiteral("/v/New.md")});
}

