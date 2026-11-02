// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>
#include "Snapshots.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <csignal>
#include <sys/resource.h>

#include "FileIcons.h"
#include "ConfigFile.h"
#include "Settings.h"
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

// Invariant: an untouched note must survive open-save byte for byte.
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

TEST(VaultRoundTrip, MixedEndingsSurviveAndAnEditKeepsTheLinesAroundIt)
{
    const QByteArray bytes = "a\r\nb\nc\r\nd\r\n";
    EXPECT_FALSE(Vault::decode(bytes).crlf);
    EXPECT_EQ(roundTrip(bytes), bytes);
    const auto edited = [&](const QString &from, const QString &to) {
        Note note = Vault::decode(bytes);
        note.text.replace(from, to);
        return Vault::encode(note);
    };
    EXPECT_EQ(edited(QStringLiteral("b"), QStringLiteral("B")), QByteArray("a\r\nB\nc\r\nd\r\n"));
    EXPECT_EQ(edited(QStringLiteral("c"), QStringLiteral("c\nnew")), QByteArray("a\r\nb\nc\r\nnew\nd\r\n"));
    EXPECT_EQ(edited(QStringLiteral("a\n"), QString()), QByteArray("b\nc\r\nd\r\n"));
    EXPECT_EQ(edited(QStringLiteral("d\n"), QString()), QByteArray("a\r\nb\nc\r\n"));
    // Text typed before the CR of a CRLF ends up before it.
    EXPECT_EQ(edited(QStringLiteral("a"), QStringLiteral("ax")), QByteArray("ax\r\nb\nc\r\nd\r\n"));
}

TEST(VaultRoundTrip, InvalidUtf8IsFlaggedLossyAndRefusedByWrite)
{
    EXPECT_FALSE(Vault::decode("a\xC3\xA8" "b\n").lossy); // valid: e with grave
    EXPECT_FALSE(Vault::decode("\xEF\xBB\xBF" "ok\n").lossy);
    for (const QByteArray &bad : {QByteArray("a\xE8" "b\n"), QByteArray("cut\xE2\x82"), QByteArray("\xFF")}) {
        const Note note = Vault::decode(bad);
        EXPECT_TRUE(note.lossy) << bad.toHex().toStdString();
        EXPECT_TRUE(note.text.contains(QChar(0xFFFD)));
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bad.md"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("a\xE8" "b\n");
    file.close();
    Note note;
    ASSERT_TRUE(Vault::read(path, note));
    ASSERT_TRUE(note.lossy);
    QString error;
    EXPECT_FALSE(Vault::write(path, note, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(readAll(path), QByteArray("a\xE8" "b\n")); // untouched
    note.lossy = false; // the explicit choice
    EXPECT_TRUE(Vault::write(path, note, &error));
    EXPECT_EQ(readAll(path), QByteArray("a\xEF\xBF\xBD" "b\n"));
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

// A save replaces the file atomically and keeps its mode.
TEST(VaultIo, ASaveKeepsTheFilesMode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("plain.md"));
    {
        QFile file(target);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("old\n");
    }
    ASSERT_TRUE(QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner));

    Note note;
    note.text = QStringLiteral("new\n");
    QString error;
    ASSERT_TRUE(Vault::write(target, note, &error)) << error.toStdString();
    EXPECT_EQ(readAll(target), QByteArray("new\n"));
    const QFileDevice::Permissions mode = QFile::permissions(target) & 0xF0FF; // owner, group, other
    EXPECT_EQ(mode, QFile::ReadOwner | QFile::WriteOwner);
}

// A vault is untrusted: a link at the path must not carry a write to wherever it points.
TEST(VaultIo, AWriteThroughALeafLinkIsRefused)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(outside.isValid());
    Note note;
    note.text = QStringLiteral("new\n");

    const QString dangling = dir.filePath(QStringLiteral("dangling.md"));
    const QString missing = outside.filePath(QStringLiteral("missing.md"));
    ASSERT_TRUE(QFile::link(missing, dangling));
    QString error;
    EXPECT_FALSE(Vault::write(dangling, note, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(missing));
    EXPECT_FALSE(Vault::writeBytes(dangling, QByteArray("x"), &error));
    EXPECT_FALSE(QFileInfo::exists(missing));

    const QString existing = outside.filePath(QStringLiteral("existing.md"));
    {
        QFile file(existing);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("old\n");
    }
    const QString link = dir.filePath(QStringLiteral("linked.md"));
    ASSERT_TRUE(QFile::link(existing, link));
    EXPECT_FALSE(Vault::write(link, note, &error));
    EXPECT_FALSE(Vault::writeBytes(link, QByteArray("x"), &error));
    EXPECT_EQ(readAll(existing), QByteArray("old\n"));
    EXPECT_TRUE(QFileInfo(link).isSymLink());
}

TEST(VaultIo, CopyReplacingKeepsTheOldFileWhenTheCopyFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString from = dir.filePath(QStringLiteral("from.bin"));
    const QString to = dir.filePath(QStringLiteral("to.bin"));
    const auto write = [](const QString &path, const QByteArray &bytes) {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };
    write(from, QByteArray(64 * 1024, 'n'));
    write(to, "EARLIER");

    struct rlimit saved {};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &saved), 0);
    ::signal(SIGXFSZ, SIG_IGN);
    struct rlimit tight = saved;
    tight.rlim_cur = 1024;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &tight), 0);
    QString error;
    const bool failed = Vault::copyReplacing(from, to, &error);
    ::setrlimit(RLIMIT_FSIZE, &saved);
    EXPECT_FALSE(failed);
    EXPECT_FALSE(error.isEmpty());
    QFile kept(to);
    ASSERT_TRUE(kept.open(QIODevice::ReadOnly));
    EXPECT_EQ(kept.readAll(), QByteArray("EARLIER"));
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 2) << "no temporary file left behind";

    ASSERT_TRUE(Vault::copyReplacing(from, to, &error)) << error.toStdString();
    EXPECT_EQ(QFileInfo(to).size(), 64 * 1024);
}

TEST(VaultIo, RemoveNoFollowNeverLeavesTheFolder)
{
    QTemporaryDir dir;
    QTemporaryDir elsewhere;
    ASSERT_TRUE(dir.isValid() && elsewhere.isValid());
    const auto touch = [](const QString &path) {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x");
    };
    touch(dir.filePath(QStringLiteral("inside.txt")));
    touch(elsewhere.filePath(QStringLiteral("outside.txt")));
    ASSERT_TRUE(QFile::link(elsewhere.path(), dir.filePath(QStringLiteral("out"))));
    ASSERT_TRUE(QFile::link(elsewhere.filePath(QStringLiteral("outside.txt")), dir.filePath(QStringLiteral("leaf"))));

    QString error;
    EXPECT_FALSE(Vault::removeNoFollowUnder(dir.path(), QStringLiteral("out/outside.txt"), &error));
    EXPECT_FALSE(error.isEmpty()) << "a link on the way is refused";
    EXPECT_FALSE(Vault::removeNoFollowUnder(dir.path(), QStringLiteral("../x"), &error));
    EXPECT_FALSE(Vault::removeNoFollowUnder(dir.path(), QStringLiteral("a//b"), &error));
    EXPECT_FALSE(Vault::removeNoFollowUnder(dir.path(), QStringLiteral("a\\b"), &error));
    EXPECT_TRUE(QFileInfo::exists(elsewhere.filePath(QStringLiteral("outside.txt"))));

    EXPECT_TRUE(Vault::removeNoFollowUnder(dir.path(), QStringLiteral("leaf"), &error)); // the link, not its target
    EXPECT_FALSE(QFileInfo(dir.filePath(QStringLiteral("leaf"))).isSymLink());
    EXPECT_TRUE(QFileInfo::exists(elsewhere.filePath(QStringLiteral("outside.txt"))));
    EXPECT_TRUE(Vault::removeNoFollowUnder(dir.path(), QStringLiteral("inside.txt"), &error));
    EXPECT_FALSE(Vault::removeNoFollowUnder(dir.path(), QStringLiteral("inside.txt"), &error));
    EXPECT_TRUE(error.isEmpty()) << "a file that was not there is not a refusal";
}

TEST(VaultIo, ReadReportsAMissingFile)
{
    Note note;
    QString error;
    EXPECT_FALSE(Vault::read(QStringLiteral("/nonexistent/nope.md"), note, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(VaultIo, ReadRefusesAFileOverItsLimitWithoutReadingIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString big = dir.filePath(QStringLiteral("huge.md"));
    {
        QFile file(big);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_TRUE(file.resize(qint64(1) << 30)); // sparse
    }
    Note note;
    QString error;
    EXPECT_FALSE(Vault::read(big, note, &error)) << "the default is the note cap";
    EXPECT_TRUE(error.contains(QStringLiteral("too large to open"))) << error.toStdString();
    EXPECT_TRUE(note.text.isEmpty());

    const QString small = dir.filePath(QStringLiteral("small.md"));
    {
        QFile file(small);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("0123456789");
    }
    EXPECT_TRUE(Vault::read(small, note, &error, 10)) << "exactly at the limit";
    EXPECT_FALSE(Vault::read(small, note, &error, 9)) << "one over";
    EXPECT_TRUE(Vault::read(small, note, &error, -1)) << "no limit";
    EXPECT_FALSE(Vault::read(dir.path(), note, &error)) << "not a file";
}

TEST(VaultPaths, NoteExtensions)
{
    EXPECT_TRUE(Vault::isNote(QStringLiteral("/v/a.md")));
    EXPECT_TRUE(Vault::isNote(QStringLiteral("/v/a.MD")));
    // Obsidian treats only .md as a note; .markdown is an attachment.
    EXPECT_FALSE(Vault::isNote(QStringLiteral("/v/a.markdown")));
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

TEST(VaultFileOps, RenamingAnyFileKeepsItsExtension)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    for (const char *name : {"cat.png", "board.canvas", "archive.tar.gz"}) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QString newPath, error;
    // The tree edits the name without its extension, so "kitty" is what comes in.
    ASSERT_TRUE(vault.renamePath(dir.filePath(QStringLiteral("cat.png")), QStringLiteral("kitty"), &newPath, &error))
        << error.toStdString();
    EXPECT_EQ(newPath, dir.filePath(QStringLiteral("kitty.png")));
    ASSERT_TRUE(vault.renamePath(dir.filePath(QStringLiteral("board.canvas")), QStringLiteral("plan.canvas"), &newPath,
                                 &error));
    EXPECT_EQ(newPath, dir.filePath(QStringLiteral("plan.canvas"))) << "typed with its extension: not doubled";
    ASSERT_TRUE(vault.renamePath(dir.filePath(QStringLiteral("archive.tar.gz")), QStringLiteral("backup.tar"), &newPath,
                                 &error));
    EXPECT_EQ(newPath, dir.filePath(QStringLiteral("backup.tar.gz")));
}

TEST(VaultFileOps, ANameStartingWithADotIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error, newPath;
    const QString note = vault.createNote(QString(), QStringLiteral("Ideas"), &error);
    ASSERT_FALSE(note.isEmpty());
    EXPECT_FALSE(vault.renamePath(note, QStringLiteral(".Ideas"), &newPath, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(note));
    error.clear();
    EXPECT_TRUE(vault.createNote(QString(), QStringLiteral(".x"), &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(vault.createNote(QStringLiteral(".archive"), QStringLiteral("x"), &error).isEmpty());
    EXPECT_TRUE(vault.createFolder(QString(), QStringLiteral(".archive"), &error).isEmpty());
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral(".archive"))));
}

TEST(VaultFileOps, NothingIsCreatedOutsideTheVault)
{
    QTemporaryDir dir, outside;
    ASSERT_TRUE(dir.isValid() && outside.isValid());
    Vault vault(dir.path());
    QString error;
    EXPECT_TRUE(vault.createNote(outside.path(), QStringLiteral("x"), &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(vault.createNote(QString(), QStringLiteral("../x"), &error).isEmpty());
    EXPECT_TRUE(vault.createFolder(outside.path(), QStringLiteral("x"), &error).isEmpty());
    EXPECT_TRUE(QDir(outside.path()).isEmpty());
    // A dangling symlink where the note goes (exists() says no) is not written through.
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Sub")));
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("target.md")), dir.filePath(QStringLiteral("Sub/y.md"))));
    EXPECT_FALSE(vault.createNote(QStringLiteral("Sub"), QStringLiteral("y"), &error).isEmpty()) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(outside.filePath(QStringLiteral("target.md"))));
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

// Needs a case-insensitive filesystem, which a test cannot mount; point
// NOTOLITH_CASE_INSENSITIVE_DIR at a writable directory on one (a FAT image
// mounted with udisksctl, say). Skipped otherwise.
TEST(VaultFileOps, ACaseOnlyRenameWorksOnACaseInsensitiveFilesystem)
{
    const QString root = qEnvironmentVariable("NOTOLITH_CASE_INSENSITIVE_DIR");
    if (root.isEmpty())
        GTEST_SKIP() << "NOTOLITH_CASE_INSENSITIVE_DIR is not set";
    QTemporaryDir dir(root + QStringLiteral("/vault-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QFile(dir.filePath(QStringLiteral("probe"))).open(QIODevice::WriteOnly));
    ASSERT_TRUE(QFileInfo::exists(dir.filePath(QStringLiteral("PROBE")))) << "not case-insensitive";

    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QString(), QStringLiteral("note"), &error);
    ASSERT_FALSE(note.isEmpty()) << error.toStdString();
    Note contents;
    contents.text = QStringLiteral("# note\n\nBody.\n");
    ASSERT_TRUE(Vault::write(note, contents, &error));

    QString newPath;
    ASSERT_TRUE(vault.renamePath(note, QStringLiteral("Note"), &newPath, &error)) << error.toStdString();
    EXPECT_EQ(newPath, dir.filePath(QStringLiteral("Note.md")));
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).contains(QStringLiteral("Note.md")))
        << "the name on disk kept its old case";
    Note reread;
    ASSERT_TRUE(Vault::read(newPath, reread, &error));
    EXPECT_EQ(reread.text, contents.text);

    // A real clash is still one: another file differing only in case.
    const QString other = vault.createNote(QString(), QStringLiteral("other"), &error);
    ASSERT_FALSE(other.isEmpty());
    EXPECT_FALSE(vault.renamePath(other, QStringLiteral("NOTE"), &newPath, &error));

    // A folder too.
    const QString folder = vault.createFolder(QString(), QStringLiteral("folder"), &error);
    ASSERT_FALSE(folder.isEmpty()) << error.toStdString();
    ASSERT_TRUE(vault.renamePath(folder, QStringLiteral("Folder"), &newPath, &error)) << error.toStdString();
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).contains(QStringLiteral("Folder")));
    const QStringList left = QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    EXPECT_TRUE(left.filter(QStringLiteral("renaming")).isEmpty()) << left.join(u' ').toStdString();
}

TEST(VaultFileOps, RenamePathStaysInsideTheVault)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("vault")));
    Vault vault(dir.filePath(QStringLiteral("vault")));
    QFile outside(dir.filePath(QStringLiteral("outside.md")));
    ASSERT_TRUE(outside.open(QIODevice::WriteOnly));
    outside.close();
    QString newPath, error;
    EXPECT_FALSE(vault.renamePath(outside.fileName(), QStringLiteral("moved"), &newPath, &error));
    EXPECT_TRUE(outside.exists());
    EXPECT_FALSE(vault.renamePath(dir.filePath(QStringLiteral("vault")), QStringLiteral("other"), &newPath, &error));
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(QStringLiteral("vault"))));
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

TEST(VaultConfinedNotePath, OnlyNotesInsideTheVault)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("sub")));
    QFile(outside.filePath(QStringLiteral("secret.md"))).open(QIODevice::WriteOnly);
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("secret.md")), dir.filePath(QStringLiteral("link.md"))));
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral("away"))));
    const Vault vault(dir.path());

    EXPECT_EQ(vault.confinedNotePath(QStringLiteral("sub/New.md")), dir.filePath(QStringLiteral("sub/New.md")));
    EXPECT_EQ(vault.confinedNotePath(QStringLiteral("sub/../Top.md")), dir.filePath(QStringLiteral("Top.md")));
    EXPECT_TRUE(vault.confinedNotePath(QStringLiteral("../x.md")).isEmpty());
    EXPECT_TRUE(vault.confinedNotePath(QStringLiteral("sub/../../x.md")).isEmpty());
    EXPECT_TRUE(vault.confinedNotePath(outside.filePath(QStringLiteral("secret.md"))).isEmpty());
    EXPECT_TRUE(vault.confinedNotePath(QStringLiteral("link.md")).isEmpty());
    EXPECT_TRUE(vault.confinedNotePath(QStringLiteral("away/secret.md")).isEmpty());
    EXPECT_TRUE(vault.confinedNotePath(QStringLiteral("missing/New.md")).isEmpty());
    EXPECT_TRUE(vault.confinedNotePath(QStringLiteral(".obsidian/app.json")).isEmpty());
}

TEST(VaultIsConfinedPath, AnythingThatLeavesTheVaultOrHidesInItIsRefused)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("sub")));
    QFile(outside.filePath(QStringLiteral("secret.md"))).open(QIODevice::WriteOnly);
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("secret.md")), dir.filePath(QStringLiteral("link.md"))));
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral("away"))));
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("missing")), dir.filePath(QStringLiteral("dangling"))));
    const Vault vault(dir.path());

    EXPECT_TRUE(vault.isConfinedPath(QStringLiteral("sub/New.md")));
    EXPECT_TRUE(vault.isConfinedPath(QStringLiteral("not/there/yet/New.png"))); // a folder a pull may create
    for (const QString &bad : {QString(), QStringLiteral("../x.md"), QStringLiteral("sub/../../x.md"),
                               QStringLiteral("sub/../x.md"), QStringLiteral("./x.md"), QStringLiteral("sub//x.md"),
                               QStringLiteral("sub/"), outside.filePath(QStringLiteral("x.md")),
                               QStringLiteral("a\\b.md"), QString::fromUtf8(QByteArray("a\0b.md", 6)),
                               QStringLiteral(".obsidian/plugins/p/main.js"), QStringLiteral("sub/.git/hooks/x"),
                               QStringLiteral("away/x.md"), QStringLiteral("away/new/x.md"),
                               QStringLiteral("dangling/x.md")}) {
        EXPECT_FALSE(vault.isConfinedPath(bad)) << bad.toStdString();
        EXPECT_FALSE(vault.isConfinedPath(bad, true)) << bad.toStdString();
    }
    // The file itself being a link: only where the caller says it may be.
    EXPECT_FALSE(vault.isConfinedPath(QStringLiteral("link.md")));
    EXPECT_TRUE(vault.isConfinedPath(QStringLiteral("link.md"), true));
    EXPECT_FALSE(vault.isConfinedPath(QStringLiteral("dangling")));
    EXPECT_TRUE(vault.isConfinedPath(QStringLiteral("dangling"), true));
}

TEST(VaultAttachmentFolder, ObsidiansFourForms)
{
    QTemporaryDir dir;
    const Vault vault(dir.path());
    const QString note = dir.filePath(QStringLiteral("notes/Day.md"));
    EXPECT_EQ(vault.attachmentFolderFor(QString(), note), vault.root());
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("/"), note), vault.root());
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("./"), note), dir.filePath(QStringLiteral("notes")));
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("./assets"), note), dir.filePath(QStringLiteral("notes/assets")));
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("Attachments"), note), dir.filePath(QStringLiteral("Attachments")));
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("../../outside"), note), vault.root()); // never out
}

TEST(VaultAttachmentFolder, UniqueAttachmentPathAddsACountBeforeTheExtension)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // Nothing there yet: the plain name.
    EXPECT_EQ(Vault::uniqueAttachmentPath(dir.path(), QStringLiteral("Pasted image.png")),
             dir.filePath(QStringLiteral("Pasted image.png")));

    // Taken: " 1", then " 2", same as createNote()'s "Untitled 1".
    QFile(dir.filePath(QStringLiteral("Pasted image.png"))).open(QIODevice::WriteOnly);
    EXPECT_EQ(Vault::uniqueAttachmentPath(dir.path(), QStringLiteral("Pasted image.png")),
             dir.filePath(QStringLiteral("Pasted image 1.png")));
    QFile(dir.filePath(QStringLiteral("Pasted image 1.png"))).open(QIODevice::WriteOnly);
    EXPECT_EQ(Vault::uniqueAttachmentPath(dir.path(), QStringLiteral("Pasted image.png")),
             dir.filePath(QStringLiteral("Pasted image 2.png")));

    // No extension: the count still lands before nothing.
    EXPECT_EQ(Vault::uniqueAttachmentPath(dir.path(), QStringLiteral("README")),
             dir.filePath(QStringLiteral("README")));
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
    vault.setTrusted(true);  // a regex of the vault's counts only once it is trusted

    EXPECT_TRUE(vault.isIgnored(QStringLiteral("Templates/daily.md")));
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("src/node_modules/pkg/index.js")));
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("node_modules/pkg/index.js")));
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("Projects/notes.md")));
    // Filters are configured, so the build*/ fallback no longer applies.
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("build/output.md")));
}

// A regex in a stranger's app.json is a program for the matcher: not compiled, not run, until the vault is trusted.
TEST(VaultIgnoreFilters, ARegexOfAnUntrustedVaultIsNeitherCompiledNorRun)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    QJsonArray filters;
    filters.append(QStringLiteral("Templates/"));
    filters.append(QStringLiteral("/(a+)+$/"));
    filters.append(QStringLiteral("/([/")); // not valid: taken as a path once trusted
    QJsonObject appJson;
    appJson[QStringLiteral("userIgnoreFilters")] = filters;
    QFile file(dir.filePath(QStringLiteral(".obsidian/app.json")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(appJson).toJson());
    file.close();

    Vault vault(dir.path());
    ASSERT_FALSE(vault.isTrusted()) << "a Vault starts untrusted";
    const QString catastrophic = QString(60, QLatin1Char('a')) + QLatin1Char('b');
    QElapsedTimer timer;
    timer.start();
    EXPECT_FALSE(vault.isIgnored(catastrophic)) << "its regex is not run";
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("aaa")));
    EXPECT_LT(timer.elapsed(), 500);
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("Templates/daily.md"))) << "a plain filter still applies";
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("build/output.md"))) << "the vault has filters of its own";

    vault.setTrusted(true);
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("aaa"))) << "trusted: its regex applies";
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("/([/x.md"))) << "an invalid regex is a path";
    EXPECT_TRUE(vault.isIgnored(QStringLiteral("Templates/daily.md")));
    vault.setTrusted(false);
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("aaa")));
}

namespace {
/// `bytes` of nothing written, as a sparse file.
void writeSparse(const QString &path, qint64 bytes, const QByteArray &head = {})
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(head), head.size());
    ASSERT_TRUE(file.resize(bytes));
}
} // namespace

// A vault's settings are read to 1 MiB: past it the file is ignored, whole, and a gibibyte is never read.
TEST(VaultIgnoreFilters, AnAppJsonOverTheLimitIsIgnoredWithoutBeingRead)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".obsidian")));
    const QByteArray json = R"({"userIgnoreFilters":["secret/"]})";
    const QString app = dir.filePath(QStringLiteral(".obsidian/app.json"));
    {
        QFile file(app);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(json + QByteArray((1 << 20) + 1 - json.size(), ' ')); // valid JSON, one byte over
    }
    EXPECT_FALSE(Vault(dir.path()).isIgnored(QStringLiteral("secret/x.md"))) << "over the limit: ignored whole";

    {
        QFile file(app);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(json + QByteArray((1 << 20) - json.size(), ' ')); // exactly the limit
    }
    EXPECT_TRUE(Vault(dir.path()).isIgnored(QStringLiteral("secret/x.md"))) << "at the limit it still counts";

    writeSparse(app, qint64(1) << 30, json);
    QElapsedTimer timer;
    timer.start();
    const Vault vault(dir.path());
    EXPECT_FALSE(vault.isIgnored(QStringLiteral("secret/x.md")));
    EXPECT_LT(timer.elapsed(), 1000) << "a gibibyte was read";
}

// Publish reads a note with the same cap the editor has.
TEST(VaultTest, ReadInsideStopsAtTheNoteLimit)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Vault vault(dir.path());
    const QString huge = dir.filePath(QStringLiteral("huge.md"));
    writeSparse(huge, qint64(1) << 30);
    Note note;
    QString error;
    EXPECT_FALSE(vault.readInside(huge, note, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("too large"))) << error.toStdString();

    const QString small = dir.filePath(QStringLiteral("small.md"));
    {
        QFile file(small);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("hello\n");
    }
    EXPECT_TRUE(vault.readInside(small, note, &error)) << error.toStdString();
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


namespace {

/// Every path under `root`, hidden ones too.
QStringList listing(const QString &root)
{
    QStringList paths;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        paths << it.next();
    paths.sort();
    return paths;
}

} // namespace

TEST(FileIconsTest, RoundTripAndStoragePathIsOutsideTheVault)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    QFile note(vault.filePath(QStringLiteral("Note.md")));
    ASSERT_TRUE(note.open(QIODevice::WriteOnly));
    note.close();
    ASSERT_TRUE(QDir(vault.path()).mkpath(QStringLiteral("Projects")));
    ASSERT_TRUE(QDir(vault.path()).mkpath(QStringLiteral("Daily")));
    const QStringList before = listing(vault.path());

    noteAppC::FileIcons icons(vault.path());
    icons.load();
    EXPECT_TRUE(icons.isEmpty()) << "no file yet is no icons";
    icons.set(QStringLiteral("Note.md"), QStringLiteral("zap"));
    icons.set(QStringLiteral("Projects"), QStringLiteral("folder-open"));
    icons.set(QStringLiteral("Daily"), QStringLiteral("D"));
    icons.set(QStringLiteral("Gone.md"), QStringLiteral("zap")); // moved away outside the app
    ASSERT_TRUE(icons.save());

    noteAppC::FileIcons again(vault.path());
    again.load();
    EXPECT_EQ(again.iconFor(QStringLiteral("Note.md")), QStringLiteral("zap"));
    EXPECT_EQ(again.iconFor(QStringLiteral("Projects")), QStringLiteral("folder-open"));
    EXPECT_EQ(again.iconFor(QStringLiteral("Daily")), QStringLiteral("D"));
    EXPECT_TRUE(again.iconFor(QStringLiteral("Gone.md")).isEmpty()) << "a path no longer there is dropped";
    again.set(QStringLiteral("Daily"), QString());
    EXPECT_TRUE(again.iconFor(QStringLiteral("Daily")).isEmpty()) << "an empty icon removes it";

    const QString stored = noteAppC::FileIcons::storagePathFor(vault.path());
    EXPECT_TRUE(stored.startsWith(noteAppC::appDir(noteAppC::XDGroot::Data)));
    EXPECT_FALSE(stored.startsWith(vault.path()));
    EXPECT_EQ(listing(vault.path()), before) << "nothing written into the vault";
}

TEST(FileIconsTest, RenamePrefixMovesAFolderAndItsChildren)
{
    noteAppC::FileIcons icons;
    icons.set(QStringLiteral("A"), QStringLiteral("folder"));
    icons.set(QStringLiteral("A/x.md"), QStringLiteral("zap"));
    icons.set(QStringLiteral("AB/y.md"), QStringLiteral("tag"));
    EXPECT_TRUE(icons.renamePrefix(QStringLiteral("A"), QStringLiteral("B")));
    EXPECT_FALSE(icons.renamePrefix(QStringLiteral("C"), QStringLiteral("D"))) << "nothing moved: nothing to save";
    EXPECT_EQ(icons.iconFor(QStringLiteral("B")), QStringLiteral("folder"));
    EXPECT_EQ(icons.iconFor(QStringLiteral("B/x.md")), QStringLiteral("zap"));
    EXPECT_TRUE(icons.iconFor(QStringLiteral("A/x.md")).isEmpty());
    EXPECT_EQ(icons.iconFor(QStringLiteral("AB/y.md")), QStringLiteral("tag")) << "a sibling sharing the prefix stays";
    EXPECT_TRUE(icons.removeUnder(QStringLiteral("B")));
    EXPECT_FALSE(icons.removeUnder(QStringLiteral("B")));
    EXPECT_TRUE(icons.iconFor(QStringLiteral("B")).isEmpty());
    EXPECT_TRUE(icons.iconFor(QStringLiteral("B/x.md")).isEmpty());
    EXPECT_EQ(icons.iconFor(QStringLiteral("AB/y.md")), QStringLiteral("tag"));
}

TEST(FileIconsTest, AGlyphIsOneCharacterAsAReaderSeesIt)
{
    using noteAppC::FileIcons;
    EXPECT_TRUE(FileIcons::isGlyph(QStringLiteral("A")));
    EXPECT_TRUE(FileIcons::isGlyph(QString::fromUcs4(U"\U00002764\U0000FE0F"))) << "with a variation selector";
    EXPECT_TRUE(FileIcons::isGlyph(QString::fromUcs4(U"\U0001F1F8\U0001F1EE"))) << "a flag: two regional indicators";
    EXPECT_TRUE(FileIcons::isGlyph(QString::fromUcs4(U"\U0001F44D\U0001F3FD"))) << "with a skin tone";
    EXPECT_FALSE(FileIcons::isGlyph(QStringLiteral("ab")));
    EXPECT_FALSE(FileIcons::isGlyph(QString()));
}

TEST(VaultFileOps, MovePathMovesANoteAndAFolderIntoAFolder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QString(), QStringLiteral("Note"), &error);
    const QString target = vault.createFolder(QString(), QStringLiteral("Target"), &error);
    const QString folder = vault.createFolder(QString(), QStringLiteral("Folder"), &error);
    ASSERT_FALSE(note.isEmpty() || target.isEmpty() || folder.isEmpty());
    ASSERT_FALSE(vault.createNote(QStringLiteral("Folder"), QStringLiteral("Inner"), &error).isEmpty());

    QString moved;
    ASSERT_TRUE(vault.movePath(note, target, &moved, &error)) << error.toStdString();
    EXPECT_EQ(moved, dir.filePath(QStringLiteral("Target/Note.md")));
    EXPECT_TRUE(QFileInfo::exists(moved));
    EXPECT_FALSE(QFileInfo::exists(note));

    ASSERT_TRUE(vault.movePath(folder, target, &moved, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(QStringLiteral("Target/Folder/Inner.md"))));
    // And back to the root.
    ASSERT_TRUE(vault.movePath(moved, dir.path(), &moved, &error)) << error.toStdString();
    EXPECT_EQ(moved, dir.filePath(QStringLiteral("Folder")));
}

TEST(VaultFileOps, MovePathRefusesWhatWouldLoseOrHideAFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    const QString folder = vault.createFolder(QString(), QStringLiteral("Folder"), &error);
    const QString child = vault.createFolder(QStringLiteral("Folder"), QStringLiteral("Child"), &error);
    const QString other = vault.createFolder(QString(), QStringLiteral("Other"), &error);
    ASSERT_FALSE(folder.isEmpty() || child.isEmpty() || other.isEmpty());
    const QString clash = vault.createNote(QStringLiteral("Other"), QStringLiteral("Note"), &error);
    const QString note = vault.createNote(QStringLiteral("Folder"), QStringLiteral("Note"), &error);
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".trash")));
    QTemporaryDir outside;
    QString moved;

    EXPECT_FALSE(vault.movePath(folder, child, &moved, &error)) << "into its own subfolder";
    EXPECT_TRUE(error.contains(QStringLiteral("itself"))) << "refused by us, not by the OS: " << error.toStdString();
    error.clear();
    EXPECT_FALSE(vault.movePath(folder, folder, &moved, &error)) << "into itself";
    EXPECT_TRUE(error.contains(QStringLiteral("itself")));
    EXPECT_FALSE(vault.movePath(note, other, &moved, &error)) << "name clash";
    EXPECT_TRUE(error.contains(QStringLiteral("already exists")));
    EXPECT_FALSE(vault.movePath(note, dir.filePath(QStringLiteral(".trash")), &moved, &error));
    EXPECT_FALSE(vault.movePath(note, outside.path(), &moved, &error)) << "outside the vault";
    EXPECT_FALSE(vault.movePath(note, clash, &moved, &error)) << "target is a file";
    EXPECT_TRUE(QFileInfo::exists(note));
    EXPECT_TRUE(QFileInfo::exists(clash));
}

// A hostile vault ships "Launchers -> somewhere outside": a drop onto it must not leave the vault.
TEST(VaultFileOps, MovePathRefusesAFolderThatIsALinkOutOfTheVault)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(outside.isValid());
    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QString(), QStringLiteral("evil"), &error);
    const QString plain = vault.createFolder(QString(), QStringLiteral("Plain"), &error);
    ASSERT_FALSE(note.isEmpty() || plain.isEmpty());
    const QString link = dir.filePath(QStringLiteral("Launchers"));
    ASSERT_TRUE(QFile::link(outside.path(), link));
    // A real folder behind the link, and a link to a folder inside the vault.
    ASSERT_TRUE(QDir(outside.path()).mkpath(QStringLiteral("deep")));
    const QString inner = dir.filePath(QStringLiteral("Launchers/deep"));
    const QString inVaultLink = dir.filePath(QStringLiteral("ToPlain"));
    ASSERT_TRUE(QFile::link(plain, inVaultLink));

    QString moved;
    EXPECT_FALSE(vault.movePath(note, link, &moved, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("Launchers"))) << error.toStdString();
    EXPECT_FALSE(vault.movePath(note, inner, &moved, &error)) << "behind a link";
    EXPECT_FALSE(vault.movePath(note, inVaultLink, &moved, &error)) << "a link is no destination";
    EXPECT_TRUE(QFileInfo::exists(note));
    EXPECT_EQ(QDir(outside.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries),
              QStringList{QStringLiteral("deep")});
    EXPECT_TRUE(QDir(plain).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());

    ASSERT_TRUE(vault.movePath(note, plain, &moved, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(moved));
}

TEST(VaultFileOps, MovePathIntoItsOwnFolderChangesNothing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QStringLiteral("Folder"), QStringLiteral("Note"), &error);
    ASSERT_FALSE(note.isEmpty());
    QString moved;
    ASSERT_TRUE(vault.movePath(note, QFileInfo(note).absolutePath(), &moved, &error));
    EXPECT_EQ(moved, note);
    EXPECT_TRUE(QFileInfo::exists(note));
}

TEST(VaultFileOps, ACaseOnlyChangeIsNotAClash)
{
    EXPECT_TRUE(Vault::isCaseOnlyChange(QStringLiteral("/v/note.md"), QStringLiteral("/v/Note.md")));
    EXPECT_FALSE(Vault::isCaseOnlyChange(QStringLiteral("/v/note.md"), QStringLiteral("/v/note.md")));
    EXPECT_FALSE(Vault::isCaseOnlyChange(QStringLiteral("/v/note.md"), QStringLiteral("/v/other.md")));

    // On a case-sensitive filesystem the rename is an ordinary one; a
    // different file already holding the new case is still refused.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    QString error;
    const QString note = vault.createNote(QString(), QStringLiteral("note"), &error);
    ASSERT_FALSE(note.isEmpty());
    QString renamed;
    ASSERT_TRUE(vault.renamePath(note, QStringLiteral("Note"), &renamed, &error)) << error.toStdString();
    EXPECT_EQ(renamed, dir.filePath(QStringLiteral("Note.md")));
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).contains(QStringLiteral("Note.md")));
    EXPECT_FALSE(QDir(dir.path()).entryList(QDir::Files).contains(QStringLiteral("note.md")));

    const QString lower = vault.createNote(QString(), QStringLiteral("note"), &error);
    if (QDir(dir.path()).entryList(QDir::Files).contains(QStringLiteral("note.md"))) { // case-sensitive here
        EXPECT_FALSE(vault.renamePath(lower, QStringLiteral("Note"), &renamed, &error)) << "a second, different file";
        EXPECT_TRUE(QFileInfo::exists(lower));
    }
}

namespace {
QStringList dirNames(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
}
} // namespace

TEST(VaultWriteNoFollow, WritesNestedFilesAndMakesTheFolders)
{
    QTemporaryDir dir;
    const Vault vault(dir.path());
    QString error;
    ASSERT_TRUE(vault.writeBytesNoFollow(QStringLiteral("a/b/c/New.png"), QByteArray("\x89PNG\0\1", 6), &error)) << error.toStdString();
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("a/b/c/New.png"))), QByteArray("\x89PNG\0\1", 6));
    ASSERT_TRUE(vault.writeBytesNoFollow(QStringLiteral("a/b/c/New.png"), QByteArray("again"), &error)) << error.toStdString();
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("a/b/c/New.png"))), QByteArray("again"));
    EXPECT_EQ(dirNames(dir.filePath(QStringLiteral("a/b/c"))), QStringList{QStringLiteral("New.png")}); // no temp left
}

TEST(VaultWriteNoFollow, AnExistingNoteKeepsItsPermissions)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("private.md"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("old");
    }
    ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const Vault vault(dir.path());
    QString error;
    ASSERT_TRUE(vault.writeBytesNoFollow(QStringLiteral("private.md"), QByteArray("new"), &error)) << error.toStdString();
    EXPECT_EQ(readAll(path), QByteArray("new"));
    EXPECT_FALSE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)); // still 0600
}

TEST(VaultWriteNoFollow, ALeafSymlinkIsReplacedNotFollowed)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    const QString target = outside.filePath(QStringLiteral("secret.md"));
    {
        QFile f(target);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("secret");
    }
    ASSERT_TRUE(QFile::link(target, dir.filePath(QStringLiteral("link.md"))));
    const Vault vault(dir.path());
    QString error;
    ASSERT_TRUE(vault.writeBytesNoFollow(QStringLiteral("link.md"), QByteArray("mine"), &error)) << error.toStdString();
    EXPECT_FALSE(QFileInfo(dir.filePath(QStringLiteral("link.md"))).isSymLink());
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("link.md"))), QByteArray("mine"));
    EXPECT_EQ(readAll(target), QByteArray("secret")); // untouched
}

TEST(VaultWriteNoFollow, ALinkAsAFolderOfThePathRefusesAndWritesNothing)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral("away"))));
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("missing")), dir.filePath(QStringLiteral("dangling"))));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("ok")));
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral("ok/inner"))));
    const Vault vault(dir.path());
    for (const QString &bad : {QStringLiteral("away/x.md"), QStringLiteral("away/new/x.md"),
                               QStringLiteral("dangling/x.md"), QStringLiteral("ok/inner/deep/x.md")}) {
        QString error;
        EXPECT_FALSE(vault.writeBytesNoFollow(bad, QByteArray("x"), &error)) << bad.toStdString();
        EXPECT_FALSE(error.isEmpty()) << bad.toStdString();
    }
    EXPECT_TRUE(dirNames(outside.path()).isEmpty());
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("dangling")) + QLatin1String("/x.md")));
}

TEST(VaultWriteNoFollow, AFileWhereAFolderIsNeededAndOddPathsFail)
{
    QTemporaryDir dir;
    {
        QFile f(dir.filePath(QStringLiteral("file.md")));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("keep");
    }
    const Vault vault(dir.path());
    for (const QString &bad : {QStringLiteral("file.md/x.md"), QStringLiteral("../x.md"), QStringLiteral("a/../x.md"),
                               QStringLiteral("a//x.md"), QString()}) {
        QString error;
        EXPECT_FALSE(vault.writeBytesNoFollow(bad, QByteArray("x"), &error)) << bad.toStdString();
        EXPECT_FALSE(error.isEmpty()) << bad.toStdString();
    }
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("file.md"))), QByteArray("keep"));
    EXPECT_EQ(dirNames(dir.path()), QStringList{QStringLiteral("file.md")});
}

TEST(VaultWriteNoFollow, AFailedWriteLeavesNoTempFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("sub/Taken.md"))); // the leaf is a folder: the rename fails
    const Vault vault(dir.path());
    QString error;
    EXPECT_FALSE(vault.writeBytesNoFollow(QStringLiteral("sub/Taken.md"), QByteArray("x"), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(dirNames(dir.filePath(QStringLiteral("sub"))), QStringList{QStringLiteral("Taken.md")});
    EXPECT_TRUE(QFileInfo(dir.filePath(QStringLiteral("sub/Taken.md"))).isDir());
}

TEST(VaultWriteNoFollow, ANoteKeepsItsLineEndingsAndBomAndALossyOneIsRefused)
{
    QTemporaryDir dir;
    const Vault vault(dir.path());
    const QByteArray original("\xEF\xBB\xBF" "one\r\ntwo\r\n");
    Note note = Vault::decode(original);
    QString error;
    ASSERT_TRUE(vault.writeNoteNoFollow(QStringLiteral("n/Note.md"), note, &error)) << error.toStdString();
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("n/Note.md"))), original);

    Note lossy = Vault::decode(QByteArray("a\xFF" "b"));
    ASSERT_TRUE(lossy.lossy);
    EXPECT_FALSE(vault.writeNoteNoFollow(QStringLiteral("n/Lossy.md"), lossy, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("n/Lossy.md"))));
}

// Two versions recorded in the same millisecond are two versions, the later
// one the newer: an autosave's recovery copies come in a burst.
TEST_F(SnapshotsTest, TwoCopiesInTheSameMillisecondDoNotReplaceEachOther)
{
    const QString note = QStringLiteral("/vault/Burst.md");
    EXPECT_TRUE(noteAppC::Snapshots::record(note, QStringLiteral("first"), 0, 5000));
    EXPECT_TRUE(noteAppC::Snapshots::record(note, QStringLiteral("second"), 0, 5000));
    const auto versions = noteAppC::Snapshots::versions(note);
    ASSERT_EQ(versions.size(), 2);
    EXPECT_EQ(noteAppC::Snapshots::read(versions.at(0)), QStringLiteral("second"));
    EXPECT_EQ(noteAppC::Snapshots::read(versions.at(1)), QStringLiteral("first"));
    EXPECT_GT(versions.at(0).time, versions.at(1).time);
}

// An image's annotations live in "<image file name> annotations.md" beside it (ImageAnnotations::sidecarPath): the
// file operations take it along, so a renamed or moved image does not lose its marks.
namespace {
void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
    ASSERT_EQ(file.write(bytes), bytes.size());
}
} // namespace

TEST(VaultFileOps, RenamingAnImageTakesItsAnnotationsSidecarAlong)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    writeFile(dir.filePath(QStringLiteral("pic.png")), "png");
    writeFile(dir.filePath(QStringLiteral("pic.png annotations.md")), "marks");
    QString renamed, error;
    ASSERT_TRUE(vault.renamePath(dir.filePath(QStringLiteral("pic.png")), QStringLiteral("photo"), &renamed, &error))
        << error.toStdString();
    EXPECT_EQ(renamed, dir.filePath(QStringLiteral("photo.png")));
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("photo.png annotations.md"))), QByteArray("marks"));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("pic.png annotations.md"))));
}

TEST(VaultFileOps, RenamingAnImageNeverOverwritesASidecarAlreadyAtTheNewName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Vault vault(dir.path());
    writeFile(dir.filePath(QStringLiteral("pic.png")), "png");
    writeFile(dir.filePath(QStringLiteral("pic.png annotations.md")), "marks of pic");
    writeFile(dir.filePath(QStringLiteral("photo.png annotations.md")), "someone else's");
    QString renamed, error;
    ASSERT_TRUE(vault.renamePath(dir.filePath(QStringLiteral("pic.png")), QStringLiteral("photo"), &renamed, &error));
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("photo.png annotations.md"))), QByteArray("someone else's"));
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("pic.png annotations.md"))), QByteArray("marks of pic"));
}

TEST(VaultFileOps, MovingAnImageTakesItsAnnotationsSidecarAlong)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Dest")));
    Vault vault(dir.path());
    writeFile(dir.filePath(QStringLiteral("pic.png")), "png");
    writeFile(dir.filePath(QStringLiteral("pic.png annotations.md")), "marks");
    QString moved, error;
    ASSERT_TRUE(vault.movePath(dir.filePath(QStringLiteral("pic.png")), dir.filePath(QStringLiteral("Dest")), &moved, &error))
        << error.toStdString();
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("Dest/pic.png annotations.md"))), QByteArray("marks"));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("pic.png annotations.md"))));
}

TEST(VaultFileOps, RenamingAFolderKeepsEachSidecarBesideItsImage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Notes")));
    Vault vault(dir.path());
    writeFile(dir.filePath(QStringLiteral("Notes/pic.png")), "png");
    writeFile(dir.filePath(QStringLiteral("Notes/pic.png annotations.md")), "marks");
    QString renamed, error;
    ASSERT_TRUE(vault.renamePath(dir.filePath(QStringLiteral("Notes")), QStringLiteral("Archive"), &renamed, &error));
    EXPECT_EQ(readAll(dir.filePath(QStringLiteral("Archive/pic.png annotations.md"))), QByteArray("marks"));
}

// --- links out of the vault (a vault is untrusted input) ---------------------

TEST(VaultResolvesInside, FollowsLinksAndNeedsNothingToExist)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("real")));
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral("away"))));
    ASSERT_TRUE(QFile::link(dir.filePath(QStringLiteral("real")), dir.filePath(QStringLiteral("home"))));
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("missing")), dir.filePath(QStringLiteral("dangling"))));
    const Vault vault(dir.path());
    EXPECT_TRUE(vault.resolvesInside(dir.path()));
    EXPECT_TRUE(vault.resolvesInside(dir.filePath(QStringLiteral("real/new/deep.md"))));
    EXPECT_TRUE(vault.resolvesInside(dir.filePath(QStringLiteral("home/x.md")))); // a link that stays in
    EXPECT_FALSE(vault.resolvesInside(dir.filePath(QStringLiteral("away"))));
    EXPECT_FALSE(vault.resolvesInside(dir.filePath(QStringLiteral("away/x/y.md"))));
    EXPECT_FALSE(vault.resolvesInside(dir.filePath(QStringLiteral("dangling"))));
    EXPECT_FALSE(vault.resolvesInside(dir.filePath(QStringLiteral("../elsewhere.md"))));
    EXPECT_FALSE(vault.resolvesInside(outside.path()));
    EXPECT_FALSE(vault.resolvesInside(QString()));
    EXPECT_FALSE(Vault::resolvesInside(QString(), dir.path()));
    EXPECT_FALSE(Vault::resolvesInside(dir.filePath(QStringLiteral("nowhere")), dir.filePath(QStringLiteral("nowhere/x"))));
}

// Fails on the old code: contains() is lexical, so "att" passed and the paste went through the link.
TEST(VaultAttachmentFolder, AFolderThatIsALinkOutOfTheVaultIsTheRootInstead)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral("att"))));
    const Vault vault(dir.path());
    const QString note = dir.filePath(QStringLiteral("Day.md"));
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("att"), note), vault.root());
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("att/sub"), note), vault.root());
    EXPECT_EQ(vault.attachmentFolderFor(QStringLiteral("./att"), note), vault.root());
}

TEST(VaultAttachmentFolder, ALinkAtTheFileNameCountsAsTaken)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("x.png")), dir.filePath(QStringLiteral("a.png")))); // dangling
    EXPECT_EQ(Vault::uniqueAttachmentPath(dir.path(), QStringLiteral("a.png")), dir.filePath(QStringLiteral("a 1.png")));
}

// Fails on the old code: the rename followed the link, so the file ended up in `outside`.
TEST(VaultFileOps, MoveToTrashRefusesATrashFolderThatIsALink)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral(".trash"))));
    writeFile(dir.filePath(QStringLiteral("evil.desktop")), "x");
    const Vault vault(dir.path());
    QString error;
    EXPECT_FALSE(vault.moveToTrash(dir.filePath(QStringLiteral("evil.desktop")), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(QStringLiteral("evil.desktop"))));
    EXPECT_TRUE(QDir(outside.path()).isEmpty());
}

TEST(VaultFileOps, MoveToTrashRefusesALinkedFolderBelowTheTrash)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral(".trash")));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("Sub")));
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral(".trash/Sub"))));
    writeFile(dir.filePath(QStringLiteral("Sub/x.md")), "x");
    const Vault vault(dir.path());
    QString error;
    EXPECT_FALSE(vault.moveToTrash(dir.filePath(QStringLiteral("Sub/x.md")), &error));
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(QStringLiteral("Sub/x.md"))));
    EXPECT_TRUE(QDir(outside.path()).isEmpty());
}

// Fails on the old code: mkpath followed the link and made "sub" in `outside`.
TEST(VaultFileOps, CreatingInAFolderThroughALinkMakesNothingOutside)
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    ASSERT_TRUE(QFile::link(outside.path(), dir.filePath(QStringLiteral("lnk"))));
    const Vault vault(dir.path());
    QString error;
    EXPECT_TRUE(vault.createNote(QStringLiteral("lnk/sub"), QStringLiteral("N"), &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(vault.createFolder(QStringLiteral("lnk/sub"), QStringLiteral("F"), &error).isEmpty());
    EXPECT_TRUE(QDir(outside.path()).isEmpty());
}
