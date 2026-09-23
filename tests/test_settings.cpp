// Settings live in ~/.config, a vault only layers over them, and opening a
// stray Markdown file must leave nothing behind.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "Hotkeys.h"
#include <QKeySequence>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>

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

/// Every test in this binary starts from an empty, private config directory.
/// Without it, anything that calls Settings::load() reads the developer's own
/// ~/.config/noteAppC - which is how defaultsAreCompiledIn came to fail the
/// moment someone picked the OLED theme in the real app. Tests that need a
/// config of their own still point XDG_CONFIG_HOME somewhere else themselves.
class PrivateConfigHome : public ::testing::Environment {
public:
    void SetUp() override
    {
        ASSERT_TRUE(m_home.isValid());
        m_previous = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8());
    }
    void TearDown() override { qputenv("XDG_CONFIG_HOME", m_previous); }

private:
    QTemporaryDir m_home;
    QByteArray m_previous;
};

[[maybe_unused]] const auto *privateConfigHome =
    ::testing::AddGlobalTestEnvironment(new PrivateConfigHome);

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

TEST(Settings, aVaultsCorePluginsSwitchFeaturesInEitherForm)
{
    // Today's object form: what it names is set, what it leaves out stays on.
    QTemporaryDir current;
    writeFile(current.filePath(QStringLiteral(".obsidian/core-plugins.json")),
              QStringLiteral(R"({"graph":false,"canvas":true,"file-explorer":true})"));
    const Settings now = Settings::load(current.path());
    EXPECT_FALSE(now.boolean(QStringLiteral("plugins.graph")));
    EXPECT_TRUE(now.boolean(QStringLiteral("plugins.canvas")));
    EXPECT_TRUE(now.boolean(QStringLiteral("plugins.outline")));

    // The old array form lists what was on, so the rest was off - all but
    // what came after it, and noteAppC's own, which no vault ever set.
    QTemporaryDir old;
    writeFile(old.filePath(QStringLiteral(".obsidian/core-plugins.json")),
              QStringLiteral(R"(["file-explorer","global-search","backlink"])"));
    const Settings then = Settings::load(old.path());
    EXPECT_TRUE(then.boolean(QStringLiteral("plugins.globalSearch")));
    EXPECT_TRUE(then.boolean(QStringLiteral("plugins.backlink")));
    EXPECT_FALSE(then.boolean(QStringLiteral("plugins.graph")));
    EXPECT_FALSE(then.boolean(QStringLiteral("plugins.pagePreview")));
    EXPECT_TRUE(then.boolean(QStringLiteral("plugins.bases")));
    EXPECT_TRUE(then.boolean(QStringLiteral("plugins.noteHistory")));
    EXPECT_TRUE(then.isFromVault(QStringLiteral("plugins.graph")));
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

TEST(Settings, NotePositionSurvivesASaveAndLoadAndEvictsPastFifty)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());

    Settings settings = Settings::load();
    EXPECT_FALSE(settings.notePosition(QStringLiteral("/vaults/mine/Note.md")).has_value());

    settings.setNotePosition(QStringLiteral("/vaults/mine/Note.md"), 42, 900);
    ASSERT_TRUE(settings.save());

    Settings reloaded = Settings::load();
    const auto position = reloaded.notePosition(QStringLiteral("/vaults/mine/Note.md"));
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->cursor, 42);
    EXPECT_EQ(position->scroll, 900);
    EXPECT_EQ(reloaded.rememberedNotes().value(0), QStringLiteral("/vaults/mine/Note.md"));

    // Recording the same note again replaces its entry rather than adding a
    // second one, and moves it back to the front.
    reloaded.setNotePosition(QStringLiteral("/other/Note.md"), 1, 1);
    reloaded.setNotePosition(QStringLiteral("/vaults/mine/Note.md"), 7, 3);
    EXPECT_EQ(reloaded.notePosition(QStringLiteral("/vaults/mine/Note.md"))->cursor, 7);
    EXPECT_EQ(reloaded.rememberedNotes().value(0), QStringLiteral("/vaults/mine/Note.md"));
    EXPECT_EQ(reloaded.rememberedNotes().size(), 2);

    // The cap keeps the file from growing without bound across a long-lived
    // vault: the 51st note pushes the oldest out.
    Settings capped = Settings::load();
    for (int i = 0; i < 51; ++i)
        capped.setNotePosition(QStringLiteral("/vaults/mine/Note%1.md").arg(i), i, i);
    EXPECT_EQ(capped.rememberedNotes().size(), 50);
    EXPECT_EQ(capped.rememberedNotes().first(), QStringLiteral("/vaults/mine/Note50.md"));
    EXPECT_FALSE(capped.notePosition(QStringLiteral("/vaults/mine/Note0.md")).has_value());
}

// --- hotkeys ------------------------------------------------------------------

TEST(Hotkeys, ReadsObsidiansFormat)
{
    // Obsidian's own spelling: "Mod" is Ctrl on Linux, keys named as a browser
    // names them.
    const QJsonArray keys = QJsonDocument::fromJson(R"([
        {"modifiers": ["Mod", "Shift"], "key": "B"},
        {"modifiers": ["Alt"], "key": "ArrowUp"},
        {"modifiers": ["Mod"], "key": "Enter"},
        {"modifiers": ["Mod"], "key": ","}
    ])").array();
    const QList<QKeySequence> read = noteAppC::Hotkeys::fromJSON(keys);
    ASSERT_EQ(read.size(), 4);
    EXPECT_EQ(read.at(0), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    EXPECT_EQ(read.at(1), QKeySequence(Qt::ALT | Qt::Key_Up));
    EXPECT_EQ(read.at(2), QKeySequence(Qt::CTRL | Qt::Key_Return));
    EXPECT_EQ(read.at(3), QKeySequence(Qt::CTRL | Qt::Key_Comma));
    // And back, in the same spelling, so the file stays Obsidian's.
    EXPECT_EQ(noteAppC::Hotkeys::fromJSON(noteAppC::Hotkeys::toJSON(read)), read);
    EXPECT_EQ(noteAppC::Hotkeys::toJSON(read).at(1).toObject().value(QStringLiteral("key")).toString(),
              QStringLiteral("ArrowUp"));
}

TEST(Hotkeys, noteAppCsOwnFileWinsOverTheVaultsAndIsTheOnlyOneWritten)
{
    QTemporaryDir home;
    QTemporaryDir vault;
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    ASSERT_TRUE(QDir(vault.path()).mkpath(QStringLiteral(".obsidian")));
    const QString obsidian = QDir(vault.path()).filePath(QStringLiteral(".obsidian/hotkeys.json"));
    {
        QFile file(obsidian);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(R"({"editor:toggle-bold": [{"modifiers": ["Mod", "Shift"], "key": "B"}],
                       "app:open-settings": [{"modifiers": ["Mod"], "key": "P"}]})");
    }
    const QByteArray before = [&] {
        QFile file(obsidian);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();

    noteAppC::Hotkeys::Bindings own;
    own.insert(QStringLiteral("app:open-settings"), {QKeySequence(Qt::CTRL | Qt::Key_Semicolon)});
    own.insert(QStringLiteral("editor:toggle-italics"), {}); // unbound
    ASSERT_TRUE(noteAppC::Hotkeys::save(own));

    const noteAppC::Hotkeys::Bindings bindings = noteAppC::Hotkeys::load(vault.path());
    EXPECT_EQ(bindings.value(QStringLiteral("editor:toggle-bold")),
              QList{QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)}) << "the vault's own key";
    EXPECT_EQ(bindings.value(QStringLiteral("app:open-settings")),
              QList{QKeySequence(Qt::CTRL | Qt::Key_Semicolon)}) << "noteAppC's file wins";
    EXPECT_TRUE(bindings.contains(QStringLiteral("editor:toggle-italics")));
    EXPECT_TRUE(bindings.value(QStringLiteral("editor:toggle-italics")).isEmpty()) << "unbound";
    EXPECT_EQ(noteAppC::Hotkeys::loadOwn().size(), 2);

    QFile file(obsidian);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), before) << "the vault's file is never written";
    qputenv("XDG_CONFIG_HOME", previous);
}

