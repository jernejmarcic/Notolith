// Colour schemes: the token overrides a custom scheme pins, and the importer
// that lifts a palette out of an Obsidian theme's CSS.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "Scheme.h"
#include "Theme.h"

using namespace noteAppC;

namespace {

QString writeFile(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(text.toUtf8());
    file.close();
    return path;
}

} // namespace

TEST(Theme, builtinCarriesObsidiansScale)
{
    const Theme dark = Theme::builtin(Theme::Mode::Dark);
    EXPECT_EQ(dark.value(QStringLiteral("--color-base-00")), QStringLiteral("#1c1c1c"));
    EXPECT_EQ(dark.value(QStringLiteral("--background-primary")), QStringLiteral("#1c1c1c"));
    EXPECT_EQ(dark.value(QStringLiteral("--text-normal")), QStringLiteral("#dadada"));

    const Theme light = Theme::builtin(Theme::Mode::Light);
    EXPECT_EQ(light.value(QStringLiteral("--color-base-00")), QStringLiteral("#ffffff"));
    EXPECT_EQ(light.value(QStringLiteral("--text-normal")), QStringLiteral("#222222"));
}

TEST(Theme, pinnedTokenSurvivesDerivation)
{
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    theme.setToken(QStringLiteral("--background-primary"), QStringLiteral("#0f1419"));
    EXPECT_TRUE(theme.isPinned(QStringLiteral("--background-primary")));

    // Anything that rederives must leave a pinned token alone; before this
    // existed the base scale overwrote it and custom schemes did nothing.
    theme.setAccent(12, 90, 60);
    EXPECT_EQ(theme.value(QStringLiteral("--background-primary")), QStringLiteral("#0f1419"));

    theme.resetToken(QStringLiteral("--background-primary"));
    EXPECT_EQ(theme.value(QStringLiteral("--background-primary")), QStringLiteral("#1c1c1c"));
}

TEST(Theme, accentPickerMovesDerivedAccents)
{
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    const QString before = theme.value(QStringLiteral("--color-accent"));
    theme.setAccent(12, 90, 60);
    EXPECT_NE(theme.value(QStringLiteral("--color-accent")), before);
}

TEST(Theme, overridesReportOnlyWhatChanged)
{
    Theme theme = Theme::builtin(Theme::Mode::Dark);
    EXPECT_TRUE(theme.overrides().isEmpty());
    theme.setToken(QStringLiteral("--color-base-00"), QStringLiteral("#000000"));
    EXPECT_TRUE(theme.overrides().contains(QStringLiteral("--color-base-00")));
}

TEST(Scheme, normalisesEveryColourFormQtCannotParse)
{
    EXPECT_EQ(Scheme::normaliseColor(QStringLiteral("#0f1419")), QStringLiteral("#0f1419"));
    EXPECT_EQ(Scheme::normaliseColor(QStringLiteral("  #FFF  ")), QStringLiteral("#ffffff"));
    EXPECT_EQ(Scheme::normaliseColor(QStringLiteral("rgb(255, 0, 0)")), QStringLiteral("#ff0000"));
    EXPECT_EQ(Scheme::normaliseColor(QStringLiteral("hsl(0, 100%, 50%)")), QStringLiteral("#ff0000"));
    EXPECT_EQ(Scheme::normaliseColor(QStringLiteral("red")), QStringLiteral("#ff0000"));
    EXPECT_TRUE(Scheme::normaliseColor(QStringLiteral("rgba(0, 0, 0, 0.5)"))
                    .startsWith(QStringLiteral("rgba(")));
    EXPECT_TRUE(Scheme::normaliseColor(QStringLiteral("rgb(0 0 0 / 40%)"))
                    .startsWith(QStringLiteral("rgba(")));

    // Not colours: these must be dropped rather than guessed at.
    EXPECT_TRUE(Scheme::normaliseColor(QStringLiteral("16px")).isEmpty());
    EXPECT_TRUE(Scheme::normaliseColor(QStringLiteral("'Inter', sans-serif")).isEmpty());
    EXPECT_TRUE(Scheme::normaliseColor(QStringLiteral("color-mix(in oklch, red 50%, blue)"))
                    .isEmpty());
}

TEST(Scheme, importsAnObsidianThemesPalette)
{
    const QString css = QStringLiteral(R"CSS(
/* A theme, with a comment holding a stray brace { */
:root {
  --my-ink: #101820;
  --color-base-00: var(--my-ink);
  --font-text-size: 17px;
  --accent-h: 210;
}
.theme-dark {
  --color-base-00: #0f1419;
  --text-normal: rgb(200, 205, 210);
  --background-modifier-hover: rgba(255, 255, 255, 0.08);
}
.theme-light {
  --color-base-00: #fafafa;
  --text-normal: hsl(210, 20%, 15%);
}
)CSS");

    const QHash<QString, QString> dark = Scheme::parseObsidianCSS(css, Theme::Mode::Dark);
    // The mode-specific block wins over :root even though :root came first.
    EXPECT_EQ(dark.value(QStringLiteral("--color-base-00")), QStringLiteral("#0f1419"));
    EXPECT_EQ(dark.value(QStringLiteral("--text-normal")), QStringLiteral("#c8cdd2"));
    EXPECT_TRUE(dark.value(QStringLiteral("--background-modifier-hover"))
                    .startsWith(QStringLiteral("rgba(")));
    EXPECT_EQ(dark.value(QStringLiteral("--accent-h")), QStringLiteral("210"));
    // A size is not a colour and must not come across.
    EXPECT_FALSE(dark.contains(QStringLiteral("--font-text-size")));

    const QHash<QString, QString> light = Scheme::parseObsidianCSS(css, Theme::Mode::Light);
    EXPECT_EQ(light.value(QStringLiteral("--color-base-00")), QStringLiteral("#fafafa"));
    EXPECT_EQ(light.value(QStringLiteral("--text-normal")), QStringLiteral("#1f262e"));
}

TEST(Scheme, resolvesVarIndirectionAndFallbacks)
{
    const QString css = QStringLiteral(
        ":root { --a: #123456; --b: var(--a); --c: var(--missing, #abcdef); "
        "--d: var(--b); }");
    const QHash<QString, QString> tokens = Scheme::parseObsidianCSS(css, Theme::Mode::Dark);
    EXPECT_EQ(tokens.value(QStringLiteral("--b")), QStringLiteral("#123456"));
    EXPECT_EQ(tokens.value(QStringLiteral("--c")), QStringLiteral("#abcdef"));
    EXPECT_EQ(tokens.value(QStringLiteral("--d")), QStringLiteral("#123456"));
}

TEST(Scheme, partialSchemeKeepsTheRestOfTheBuiltIn)
{
    QTemporaryDir dir;
    const QString path = writeFile(dir.filePath(QStringLiteral("Ayu.json")),
                                   QStringLiteral(R"({"name":"Ayu","dark":{"--color-base-00":"#0f1419"}})"));

    QString error;
    const Theme theme = Scheme::apply(path, Theme::Mode::Dark, &error);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(theme.value(QStringLiteral("--color-base-00")), QStringLiteral("#0f1419"));
    EXPECT_EQ(theme.value(QStringLiteral("--background-primary")), QStringLiteral("#0f1419"));
    // Untouched tokens still come from the built-in scheme.
    EXPECT_EQ(theme.value(QStringLiteral("--text-normal")), QStringLiteral("#dadada"));
}

TEST(Scheme, savesAndReloadsWhatTheUserChanged)
{
    QTemporaryDir dir;
    Theme dark = Theme::builtin(Theme::Mode::Dark);
    dark.setToken(QStringLiteral("--color-base-00"), QStringLiteral("#0f1419"));
    Theme light = Theme::builtin(Theme::Mode::Light);
    light.setToken(QStringLiteral("--color-base-00"), QStringLiteral("#fffdf7"));

    const QString path = dir.filePath(QStringLiteral("Mine.json"));
    QString error;
    ASSERT_TRUE(Scheme::save(path, QStringLiteral("Mine"), dark, light, &error)) << error.toStdString();

    EXPECT_EQ(Scheme::load(path, Theme::Mode::Dark).value(QStringLiteral("--color-base-00")),
              QStringLiteral("#0f1419"));
    EXPECT_EQ(Scheme::load(path, Theme::Mode::Light).value(QStringLiteral("--color-base-00")),
              QStringLiteral("#fffdf7"));
}

TEST(Scheme, findsVaultSchemesAndInstalledObsidianThemes)
{
    QTemporaryDir vault;
    writeFile(vault.filePath(QStringLiteral(".obsidian/noteAppC-themes/Mine.json")),
              QStringLiteral(R"({"name":"Mine","dark":{}})"));
    writeFile(vault.filePath(QStringLiteral(".obsidian/themes/Minimal/theme.css")),
              QStringLiteral(".theme-dark { --color-base-00: #000000; }"));

    const QList<SchemeInfo> schemes = Scheme::available(vault.path());
    ASSERT_EQ(schemes.size(), 2);
    EXPECT_EQ(schemes.at(0).name, QStringLiteral("Mine"));
    EXPECT_FALSE(schemes.at(0).obsidianCSS);
    EXPECT_EQ(schemes.at(1).name, QStringLiteral("Minimal"));
    EXPECT_TRUE(schemes.at(1).obsidianCSS);
    EXPECT_TRUE(schemes.at(1).inVault);

    // An installed Obsidian theme applies like any other scheme.
    const Theme theme = Scheme::apply(schemes.at(1).path, Theme::Mode::Dark);
    EXPECT_EQ(theme.value(QStringLiteral("--background-primary")), QStringLiteral("#000000"));
}

TEST(Scheme, repairsAnImportThatWouldBeUnreadable)
{
    QTemporaryDir dir;
    // A theme that resolved only partly: near-black text on a dark ground,
    // which is what Shimmering Focus produced before this guard existed.
    const QString path = writeFile(
        dir.filePath(QStringLiteral("Broken/theme.css")),
        QStringLiteral(".theme-dark { --background-primary: #1c1c1c; --text-normal: #040506; }"));

    const Theme theme = Scheme::apply(path, Theme::Mode::Dark);
    EXPECT_GT(Scheme::contrastRatio(theme.color(QStringLiteral("--text-normal")),
                                    theme.color(QStringLiteral("--background-primary"))),
              4.5);
}

TEST(Scheme, flattensATranslucentWindowBackground)
{
    QTemporaryDir dir;
    const QString path = writeFile(
        dir.filePath(QStringLiteral("Glass/theme.css")),
        QStringLiteral(".theme-dark { --background-primary: rgba(16, 14, 23, 0.4); }"));

    const Theme theme = Scheme::apply(path, Theme::Mode::Dark);
    EXPECT_EQ(theme.color(QStringLiteral("--background-primary")).alpha(), 255);
}

TEST(Scheme, contrastRatioMatchesWCAG)
{
    EXPECT_NEAR(Scheme::contrastRatio(QColor(Qt::black), QColor(Qt::white)), 21.0, 0.01);
    EXPECT_NEAR(Scheme::contrastRatio(QColor(Qt::white), QColor(Qt::white)), 1.0, 0.01);
}
