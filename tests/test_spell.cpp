#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStringEncoder>
#include <QTemporaryDir>

#include "SpellChecker.h"

using namespace noteAppC;

namespace {

/// A dictionary in a temporary directory reached through $DICPATH, and a
/// config home of its own, so nothing here depends on what the machine has
/// installed or on what an earlier run added.
class SpellTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_dictionaries.isValid());
        ASSERT_TRUE(m_config.isValid());
        m_oldDicpath = qgetenv("DICPATH");
        m_oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("DICPATH", m_dictionaries.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
        write("xx_TEST.aff", "SET UTF-8\nTRY esianrtolcdugmphbyfvkwzESIANRTOLCDUGMPHBYFVKWZ\n");
        write("xx_TEST.dic", "4\nhello\nworld\nnote\n\xc5\xa0kofja\n");
    }
    void TearDown() override
    {
        qputenv("DICPATH", m_oldDicpath);
        qputenv("XDG_CONFIG_HOME", m_oldConfig);
    }
    void write(const char *name, const QByteArray &bytes)
    {
        QFile file(QDir(m_dictionaries.path()).filePath(QString::fromLatin1(name)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }
    QTemporaryDir m_dictionaries;
    QTemporaryDir m_config;

private:
    QByteArray m_oldDicpath;
    QByteArray m_oldConfig;
};

} // namespace

TEST_F(SpellTest, ChecksAndSuggests)
{
    const SpellChecker spell({QStringLiteral("xx_TEST")});
    ASSERT_EQ(spell.languages(), QStringList{QStringLiteral("xx_TEST")});
    EXPECT_TRUE(spell.isCorrect(QStringLiteral("hello")));
    EXPECT_TRUE(spell.isCorrect(QStringLiteral("Škofja")));
    EXPECT_FALSE(spell.isCorrect(QStringLiteral("helo")));
    EXPECT_TRUE(spell.suggestions(QStringLiteral("helo")).contains(QStringLiteral("hello")));
}

TEST_F(SpellTest, ABareLanguageFindsARegion)
{
    EXPECT_EQ(SpellChecker({QStringLiteral("xx")}).languages(), QStringList{QStringLiteral("xx_TEST")});
    EXPECT_TRUE(SpellChecker({QStringLiteral("zz")}).languages().isEmpty());
    // With nothing to check against, nothing is wrong.
    EXPECT_TRUE(SpellChecker({QStringLiteral("zz")}).isCorrect(QStringLiteral("helo")));
}

TEST_F(SpellTest, ABareLanguageAfterItsRegionLoadsNothingMore)
{
    // en_GB then en, as QLocale::uiLanguages() lists them: "en" found
    // en_AG, on Arch a symlink to the en_GB files, and loaded them twice.
    write("xx_AA.aff", "SET UTF-8\n");
    write("xx_AA.dic", "1\nhello\n");
    EXPECT_EQ(SpellChecker({QStringLiteral("xx_TEST"), QStringLiteral("xx")}).languages(),
              QStringList{QStringLiteral("xx_TEST")});
}

TEST_F(SpellTest, AnAddedWordIsKeptForNextTime)
{
    {
        SpellChecker spell({QStringLiteral("xx_TEST")});
        EXPECT_FALSE(spell.isCorrect(QStringLiteral("noteAppC")));
        spell.addWord(QStringLiteral("noteAppC"));
        EXPECT_TRUE(spell.isCorrect(QStringLiteral("noteAppC")));
    }
    EXPECT_TRUE(QFile::exists(QDir(m_config.path()).filePath(QStringLiteral("noteAppC/dictionary.txt"))));
    EXPECT_TRUE(SpellChecker({QStringLiteral("xx_TEST")}).isCorrect(QStringLiteral("noteAppC")));
}

TEST_F(SpellTest, ADictionaryInALegacyEncodingWorks)
{
    // Older dictionaries, Slovenian among them, are ISO-8859-2, and hunspell
    // wants words in the dictionary's own encoding.
    QStringEncoder latin2("ISO-8859-2");
    if (!latin2.isValid())
        GTEST_SKIP() << "this Qt cannot convert ISO-8859-2";
    write("yy_TEST.aff", "SET ISO8859-2\n");
    write("yy_TEST.dic", QByteArray("2\n") + QByteArray(latin2.encode(QStringLiteral("Škofja"))) + "\nloka\n");
    const SpellChecker spell({QStringLiteral("yy_TEST")});
    ASSERT_EQ(spell.languages(), QStringList{QStringLiteral("yy_TEST")});
    EXPECT_TRUE(spell.isCorrect(QStringLiteral("Škofja")));
    EXPECT_FALSE(spell.isCorrect(QStringLiteral("Skofja")));
    EXPECT_TRUE(spell.suggestions(QStringLiteral("Skofja")).contains(QStringLiteral("Škofja")));
}
