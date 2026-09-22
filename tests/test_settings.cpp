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
    EXPECT_TRUE(Settings::configFile().endsWith(QStringLiteral(".conf")));
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
