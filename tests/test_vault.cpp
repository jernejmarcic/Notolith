#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
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
