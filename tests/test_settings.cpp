// Settings live in ~/.config, a vault only layers over them, and opening a
// stray Markdown file must leave nothing behind.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "Settings.h"

using namespace noteAppC;

namespace {

void writeFile(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(text.toUtf8());
}

} // namespace

TEST(Settings, defaultsAreCompiledIn)
{
    const Settings settings = Settings::load();
    EXPECT_EQ(settings.text(QStringLiteral("appearance.theme")), QStringLiteral("dark"));
    EXPECT_TRUE(settings.boolean(QStringLiteral("editor.readableLineLength")));
    EXPECT_EQ(settings.integer(QStringLiteral("editor.contentWidth")), 700);
    EXPECT_FALSE(settings.isCustomised(QStringLiteral("appearance.theme")));
}

TEST(Settings, configLivesUnderXDG)
{
    EXPECT_TRUE(Settings::configDirectory().contains(QStringLiteral("noteAppC")));
    EXPECT_TRUE(Settings::configFile().endsWith(QStringLiteral(".ini")));
}

TEST(Settings, userValueOutranksTheDefaultAndSurvivesASave)
{
    QTemporaryDir home;
    QStandardPaths::setTestModeEnabled(false);
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());

    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("editor.contentWidth"), 900);
    EXPECT_EQ(settings.integer(QStringLiteral("editor.contentWidth")), 900);
    EXPECT_TRUE(settings.isCustomised(QStringLiteral("editor.contentWidth")));

    QString error;
    ASSERT_TRUE(settings.save(&error)) << error.toStdString();
    EXPECT_EQ(Settings::load().integer(QStringLiteral("editor.contentWidth")), 900);

    // Only what changed is written, so a later change of default still lands.
    settings.reset(QStringLiteral("editor.contentWidth"));
    ASSERT_TRUE(settings.save(&error)) << error.toStdString();
    EXPECT_EQ(Settings::load().integer(QStringLiteral("editor.contentWidth")), 700);
}

TEST(Settings, aVaultLayersItsObsidianConfigOverTheDefaults)
{
    QTemporaryDir vault;
    writeFile(vault.filePath(QStringLiteral(".obsidian/app.json")),
              QStringLiteral(R"({"readableLineLength":false,"showLineNumber":true,)"
                             R"("strictLineBreaks":true,"useMarkdownLinks":true,)"
                             R"("defaultViewMode":"preview","tabSize":2})"));
    writeFile(vault.filePath(QStringLiteral(".obsidian/appearance.json")),
              QStringLiteral(R"({"theme":"moonstone","accentColor":"#ff0000","baseFontSize":18})"));

    const Settings settings = Settings::load(vault.path());
    EXPECT_FALSE(settings.boolean(QStringLiteral("editor.readableLineLength")));
    EXPECT_TRUE(settings.boolean(QStringLiteral("editor.strictLineBreaks")));
    EXPECT_EQ(settings.integer(QStringLiteral("editor.tabWidth")), 2);
    EXPECT_EQ(settings.text(QStringLiteral("editor.defaultMode")), QStringLiteral("reading"));
    // useMarkdownLinks is the inverse of ours.
    EXPECT_FALSE(settings.boolean(QStringLiteral("links.useWikilinks")));
    EXPECT_EQ(settings.text(QStringLiteral("appearance.theme")), QStringLiteral("light"));
    EXPECT_EQ(settings.integer(QStringLiteral("appearance.accentHue")), 0);
    EXPECT_EQ(settings.integer(QStringLiteral("appearance.baseFontSize")), 18);
    EXPECT_TRUE(settings.isFromVault(QStringLiteral("editor.strictLineBreaks")));
    EXPECT_FALSE(settings.isFromVault(QStringLiteral("editor.autoPairs")));
}

TEST(Settings, savingNeverWritesIntoAVault)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    QTemporaryDir vault;

    Settings settings = Settings::load(vault.path());
    settings.setValue(QStringLiteral("editor.contentWidth"), 820);
    ASSERT_TRUE(settings.save());

    // Not one file beside the notes: no .obsidian, no dotfile of ours.
    const QStringList left = QDir(vault.path())
                                 .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    EXPECT_TRUE(left.isEmpty()) << left.join(QLatin1Char(',')).toStdString();
}

TEST(LaunchTarget, aDirectoryIsAVault)
{
    QTemporaryDir dir;
    const LaunchTarget target = resolveLaunchTarget(dir.path());
    EXPECT_EQ(target.mode, OpenMode::Vault);
    EXPECT_EQ(target.vaultRoot, QFileInfo(dir.path()).absoluteFilePath());
    EXPECT_TRUE(target.filePath.isEmpty());
}

TEST(LaunchTarget, aLooseFileOpensAloneWithNoVault)
{
    QTemporaryDir dir;
    writeFile(dir.filePath(QStringLiteral("stray.md")), QStringLiteral("# Hello\n"));

    const LaunchTarget target = resolveLaunchTarget(dir.filePath(QStringLiteral("stray.md")));
    EXPECT_EQ(target.mode, OpenMode::SingleFile);
    EXPECT_TRUE(target.vaultRoot.isEmpty());
    EXPECT_TRUE(target.filePath.endsWith(QStringLiteral("stray.md")));
}

TEST(LaunchTarget, aFileInsideAVaultOpensTheWholeVault)
{
    QTemporaryDir dir;
    writeFile(dir.filePath(QStringLiteral(".obsidian/app.json")), QStringLiteral("{}"));
    writeFile(dir.filePath(QStringLiteral("notes/deep/Note.md")), QStringLiteral("# Hi\n"));

    const LaunchTarget target =
        resolveLaunchTarget(dir.filePath(QStringLiteral("notes/deep/Note.md")));
    EXPECT_EQ(target.mode, OpenMode::Vault);
    EXPECT_EQ(target.vaultRoot, QFileInfo(dir.path()).absoluteFilePath());
    EXPECT_TRUE(target.filePath.endsWith(QStringLiteral("Note.md")));
}

TEST(LaunchTarget, nothingNamedIsAnEmptyWindow)
{
    EXPECT_EQ(resolveLaunchTarget(QString()).mode, OpenMode::Empty);
}

TEST(Settings, theConfigDirectoryHasAShape)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());

    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("editor.contentWidth"), 820);
    ASSERT_TRUE(settings.save());

    EXPECT_TRUE(Settings::configFile().endsWith(QStringLiteral("noteAppC/settings.ini")));
    EXPECT_TRUE(Settings::indexFile().endsWith(QStringLiteral("noteAppC/vaults.ini")));
    EXPECT_TRUE(QFileInfo(Settings::themesDirectory()).isDir());
    EXPECT_TRUE(QFileInfo(Settings::templatesDirectory()).isDir());
}

TEST(Settings, theFileIsWhatAPersonWouldHaveWritten)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());

    Settings settings = Settings::load();
    settings.setValue(QStringLiteral("editor.contentWidth"), 820);
    settings.setValue(QStringLiteral("editor.readableLineLength"), false);
    settings.setValue(QStringLiteral("appearance.theme"), QStringLiteral("light"));
    ASSERT_TRUE(settings.save());

    QFile file(Settings::configFile());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(file.readAll());

    // Grouped, kebab-cased, unquoted, and bare true/false.
    EXPECT_TRUE(text.contains(QStringLiteral("[editor]")));
    EXPECT_TRUE(text.contains(QStringLiteral("content-width = 820")));
    EXPECT_TRUE(text.contains(QStringLiteral("readable-line-length = false")));
    EXPECT_TRUE(text.contains(QStringLiteral("[appearance]")));
    EXPECT_TRUE(text.contains(QStringLiteral("theme = light")));
    // Nothing the app remembers for itself: that lives in vaults.ini. The
    // header mentions the word, so look for the setting, not the word.
    EXPECT_FALSE(text.contains(QStringLiteral("recent-vaults =")));
    EXPECT_FALSE(text.contains(QStringLiteral("recent-files =")));
    EXPECT_FALSE(text.contains(QStringLiteral("[state]")));
}

TEST(Settings, handEditsAndCommentsSurviveTheAppWritingTheFile)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    QDir().mkpath(Settings::configDirectory());

    // A file as a person would leave it, comments and all.
    writeFile(Settings::configFile(), QStringLiteral(
        "# my notes app\n"
        "\n"
        "[editor]\n"
        "# I like a wide column\n"
        "content-width = 1100\n"
        "\n"
        "[something-else]\n"
        "# the app has never heard of this\n"
        "keep-me = yes\n"));

    Settings settings = Settings::load();
    EXPECT_EQ(settings.integer(QStringLiteral("editor.contentWidth")), 1100);

    settings.setValue(QStringLiteral("appearance.theme"), QStringLiteral("light"));
    ASSERT_TRUE(settings.save());

    QFile file(Settings::configFile());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(file.readAll());

    EXPECT_TRUE(text.contains(QStringLiteral("# my notes app")));
    EXPECT_TRUE(text.contains(QStringLiteral("# I like a wide column")));
    EXPECT_TRUE(text.contains(QStringLiteral("content-width = 1100")));
    // A section the app knows nothing about is left exactly as it was.
    EXPECT_TRUE(text.contains(QStringLiteral("[something-else]")));
    EXPECT_TRUE(text.contains(QStringLiteral("keep-me = yes")));
    EXPECT_TRUE(text.contains(QStringLiteral("theme = light")));
}

TEST(Settings, whatTheAppRemembersGoesInItsOwnFile)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());

    Settings settings = Settings::load();
    settings.noteOpened(QStringLiteral("/vaults/mine"), QStringLiteral("/vaults/mine/Note.md"));
    ASSERT_TRUE(settings.save());

    EXPECT_EQ(Settings::load().recentVaults().value(0), QStringLiteral("/vaults/mine"));
    EXPECT_EQ(Settings::load().recentFiles().value(0), QStringLiteral("/vaults/mine/Note.md"));

    QFile index(Settings::indexFile());
    ASSERT_TRUE(index.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_TRUE(QString::fromUtf8(index.readAll()).contains(QStringLiteral("/vaults/mine")));
}
