// Colour schemes: the token overrides a custom scheme pins, and the importer
// that lifts a palette out of an Obsidian theme's CSS.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "CalloutStyle.h"
#include "LanguageStyle.h"
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

TEST(Scheme, findsThemesInTheDirectorySettingsDocuments)
{
    QTemporaryDir xdgHome;
    ASSERT_TRUE(xdgHome.isValid());
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", xdgHome.path().toUtf8());

    // Settings::themesDirectory() documents ~/.config/noteAppC/themes (or
    // $XDG_CONFIG_HOME/noteAppC/themes if that is set). Scheme::searchPaths()
    // used to ask QStandardPaths::AppConfigLocation instead, which - since
    // main.cpp sets both the organisation and the application name to
    // "noteAppC" - names .../noteAppC/noteAppC/themes, one directory too deep.
    // A theme placed exactly where the docs say must be found here, not just
    // in a directory nobody was told about.
    const QString documented = QDir(xdgHome.path()).filePath(QStringLiteral("noteAppC/themes"));
    writeFile(QDir(documented).filePath(QStringLiteral("Mine.json")),
              QStringLiteral(R"({"name":"Mine","dark":{"--color-base-00":"#0f1419"}})"));

    const QList<SchemeInfo> schemes = Scheme::available();
    ASSERT_EQ(schemes.size(), 1);
    EXPECT_EQ(schemes.at(0).name, QStringLiteral("Mine"));
    EXPECT_EQ(schemes.at(0).path, QDir(documented).filePath(QStringLiteral("Mine.json")));

    qputenv("XDG_CONFIG_HOME", previous);
}

TEST(Scheme, findsALooseCSSFileNotJustAnObsidianThemeFolder)
{
    QTemporaryDir vault;
    // Obsidian only ever installs a theme as <name>/theme.css; a person
    // dropping a bare .css file into the themes directory must still get a
    // scheme out of it, named after the file and read the same way.
    writeFile(vault.filePath(QStringLiteral(".obsidian/noteAppC-themes/Loose.css")),
              QStringLiteral(".theme-dark { --color-base-00: #202020; }"));

    const QList<SchemeInfo> schemes = Scheme::available(vault.path());
    ASSERT_EQ(schemes.size(), 1);
    EXPECT_EQ(schemes.at(0).name, QStringLiteral("Loose"));
    EXPECT_TRUE(schemes.at(0).obsidianCSS);

    const Theme theme = Scheme::apply(schemes.at(0).path, Theme::Mode::Dark);
    EXPECT_EQ(theme.value(QStringLiteral("--background-primary")), QStringLiteral("#202020"));
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

TEST(ThemeTest, OledIsTrueBlackAndCountsAsDark)
{
    const Theme oled = Theme::builtin(Theme::Mode::Oled);

    EXPECT_TRUE(oled.isDark());
    EXPECT_TRUE(oled.isOled());
    EXPECT_EQ(oled.mode(), Theme::Mode::Oled);
    // The note background is the whole point: an unlit pixel on an OLED panel.
    EXPECT_EQ(oled.color(QStringLiteral("--background-primary")), QColor(Qt::black));
    // Panels lift just off black, or the interface would lose all structure.
    EXPECT_NE(oled.color(QStringLiteral("--background-secondary")), QColor(Qt::black));
    EXPECT_LT(oled.color(QStringLiteral("--background-secondary")).lightness(), 40);
    // Text stays readable, and the derived tokens took the dark branch.
    EXPECT_GT(oled.color(QStringLiteral("--text-normal")).lightness(), 200);
    EXPECT_GT(oled.color(QStringLiteral("--text-normal")).lightness(),
              oled.color(QStringLiteral("--text-muted")).lightness());

    EXPECT_EQ(Theme::nameOf(Theme::Mode::Oled), QStringLiteral("oled"));
    EXPECT_EQ(Theme::modeFromName(QStringLiteral("oled")), Theme::Mode::Oled);
    EXPECT_EQ(Theme::modeFromName(QStringLiteral("light")), Theme::Mode::Light);
    EXPECT_EQ(Theme::modeFromName(QStringLiteral("nonsense")), Theme::Mode::Dark);
}

// --- callout styles -------------------------------------------------------

TEST(CalloutStyle, HexAlphaIsCSSorder)
{
    // Qt reads eight hex digits as #AARRGGBB. CSS, and callouts.ini, put the
    // alpha last, so a transparent fill must not come back opaque and wrong.
    const QColor translucent = CalloutStyle::parseColor(QStringLiteral("#FF5A5F26"));
    EXPECT_EQ(translucent.red(), 0xFF);
    EXPECT_EQ(translucent.green(), 0x5A);
    EXPECT_EQ(translucent.blue(), 0x5F);
    EXPECT_EQ(translucent.alpha(), 0x26);

    // The shorter forms, and the CSS functions Theme already knew, still work.
    EXPECT_EQ(CalloutStyle::parseColor(QStringLiteral("#f00")), QColor(255, 0, 0));
    EXPECT_EQ(CalloutStyle::parseColor(QStringLiteral("#00FF00")), QColor(0, 255, 0));
    EXPECT_EQ(CalloutStyle::parseColor(QStringLiteral("rgba(0, 0, 255, 0.5)")).blue(), 255);
    EXPECT_FALSE(CalloutStyle::parseColor(QStringLiteral("not a colour")).isValid());
}

TEST(CalloutStyle, ThreeLayersResolveInOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("callouts.ini"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(
        "[deadline]\n"
        "icon = zap        ; a trailing comment is not part of the value\n"
        "bar = #FF5A5Fff\n"
        "bg = #FF5A5F26\n"
        "size = 17\n"
        "style = bold italic\n"
        "heading-size = 20\n"
        "\n"
        "[calm]\n"
        "inherits = deadline\n"
        "bar = #4C8DFFff\n"
        "style = normal\n");
    file.close();

    const CalloutStyles styles = CalloutStyles::load(path);
    EXPECT_EQ(styles.definedTypes(), QStringList({QStringLiteral("calm"),
                                                  QStringLiteral("deadline")}));

    const Theme theme = Theme::builtin(Theme::Mode::Dark);

    // Layer one only: a built-in type is untouched by a file that omits it.
    const CalloutStyle note = styles.resolve(QStringLiteral("note"), {}, theme);
    EXPECT_EQ(note.icon, Theme::calloutIcon(QStringLiteral("note")));
    EXPECT_EQ(note.bar, theme.calloutColor(QStringLiteral("note")));
    EXPECT_EQ(note.size, 0); // defers to the theme

    // Layer two: the file wins over the built-in type.
    const CalloutStyle deadline = styles.resolve(QStringLiteral("deadline"), {}, theme);
    EXPECT_EQ(deadline.icon, QStringLiteral("zap"));
    EXPECT_EQ(deadline.bar, QColor(0xFF, 0x5A, 0x5F));
    EXPECT_EQ(deadline.background.alpha(), 0x26);
    EXPECT_EQ(deadline.size, 17);
    EXPECT_EQ(deadline.weight, QFont::Bold);
    EXPECT_TRUE(deadline.italic);
    EXPECT_EQ(deadline.headingSize, 20);
    // A heading with no colour of its own takes the rail's, as Obsidian does.
    EXPECT_EQ(deadline.headingColor, deadline.bar);

    // `inherits` carries the parent's fonts; the child's own keys still win,
    // and `normal` clears an effect the parent set rather than adding to it.
    const CalloutStyle calm = styles.resolve(QStringLiteral("calm"), {}, theme);
    EXPECT_EQ(calm.icon, QStringLiteral("zap"));
    EXPECT_EQ(calm.size, 17);
    EXPECT_EQ(calm.headingSize, 20);
    EXPECT_EQ(calm.bar, QColor(0x4C, 0x8D, 0xFF));
    EXPECT_FALSE(calm.italic);
    EXPECT_EQ(calm.weight, QFont::Normal);

    // Layer three: what the callout itself says beats the file.
    QMap<QString, QString> inline_;
    inline_.insert(QStringLiteral("bg"), QStringLiteral("#00FF0022"));
    inline_.insert(QStringLiteral("icon"), QStringLiteral("flame"));
    const CalloutStyle overridden = styles.resolve(QStringLiteral("deadline"), inline_, theme);
    EXPECT_EQ(overridden.icon, QStringLiteral("flame"));
    EXPECT_EQ(overridden.background, QColor(0x00, 0xFF, 0x00, 0x22));
    EXPECT_EQ(overridden.bar, QColor(0xFF, 0x5A, 0x5F)); // untouched by the override
    EXPECT_EQ(overridden.size, 17);
}

TEST(CalloutStyle, MissingFileIsNormal)
{
    // Nobody has to create callouts.ini, so a file that is not there resolves
    // to the built-in types rather than failing.
    const CalloutStyles styles =
        CalloutStyles::load(QStringLiteral("/nonexistent/callouts.ini"));
    EXPECT_TRUE(styles.definedTypes().isEmpty());
    const Theme theme = Theme::builtin(Theme::Mode::Dark);
    const CalloutStyle warning = styles.resolve(QStringLiteral("warning"), {}, theme);
    EXPECT_EQ(warning.bar, theme.calloutColor(QStringLiteral("warning")));
    EXPECT_TRUE(warning.background.isValid());
    EXPECT_LT(warning.background.alpha(), 255);
}

// --- language styles --------------------------------------------------------

TEST(LanguageStyle, AliasesAndInheritsResolveLikeCallouts)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("codeblocks.ini"));
    writeFile(path,
             QStringLiteral(
                 "[python]\n"
                 "name = Python           ; a trailing comment is not part of the value\n"
                 "icon = U+E73C\n"
                 "color = #3776ABff\n"
                 "aliases = py, python3\n"
                 "\n"
                 "[cython]\n"
                 "inherits = python\n"
                 "name = \"Cython\"\n"));

    const LanguageStyles styles = LanguageStyles::load(path);
    EXPECT_EQ(styles.definedLanguages(),
             QStringList({QStringLiteral("cython"), QStringLiteral("python")}));

    // The section itself, case-insensitively.
    const LanguageStyle python = styles.resolve(QStringLiteral("Python"));
    EXPECT_EQ(python.name, QStringLiteral("Python"));
    EXPECT_EQ(python.icon, QStringLiteral("U+E73C"));
    EXPECT_EQ(python.color, QColor(0x37, 0x76, 0xAB, 0xff));

    // An alias reaches the same section.
    const LanguageStyle byAlias = styles.resolve(QStringLiteral("py"));
    EXPECT_EQ(byAlias.name, QStringLiteral("Python"));
    EXPECT_EQ(byAlias.icon, QStringLiteral("U+E73C"));
    const LanguageStyle byAlias2 = styles.resolve(QStringLiteral("PYTHON3"));
    EXPECT_EQ(byAlias2.name, QStringLiteral("Python"));

    // `inherits` carries the parent's icon and colour; the child's own name
    // still wins.
    const LanguageStyle cython = styles.resolve(QStringLiteral("cython"));
    EXPECT_EQ(cython.name, QStringLiteral("Cython"));
    EXPECT_EQ(cython.icon, QStringLiteral("U+E73C"));
    EXPECT_EQ(cython.color, QColor(0x37, 0x76, 0xAB, 0xff));
}

TEST(LanguageStyle, ColourAlphaIsCSSorder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("codeblocks.ini"));
    writeFile(path, QStringLiteral("[rust]\ncolor = #FF5A5F26\n"));

    const LanguageStyle rust = LanguageStyles::load(path).resolve(QStringLiteral("rust"));
    // #RRGGBBAA, alpha last, the same rule CalloutStyle::parseColor enforces
    // - reused here rather than reimplemented.
    EXPECT_EQ(rust.color.red(), 0xFF);
    EXPECT_EQ(rust.color.green(), 0x5A);
    EXPECT_EQ(rust.color.blue(), 0x5F);
    EXPECT_EQ(rust.color.alpha(), 0x26);
}

TEST(LanguageStyle, UnknownAndEmptyLanguageGetPlainDefaults)
{
    const LanguageStyles styles = LanguageStyles::load(QStringLiteral("/nonexistent/codeblocks.ini"));
    EXPECT_TRUE(styles.definedLanguages().isEmpty());

    const LanguageStyle unknown = styles.resolve(QStringLiteral("Brainfuck"));
    EXPECT_EQ(unknown.name, QStringLiteral("Brainfuck")); // exactly as given, case kept
    EXPECT_TRUE(unknown.icon.isEmpty());
    EXPECT_FALSE(unknown.color.isValid());

    const LanguageStyle empty = styles.resolve(QString());
    EXPECT_TRUE(empty.name.isEmpty());
    EXPECT_TRUE(empty.icon.isEmpty());
    EXPECT_FALSE(empty.color.isValid());
}

TEST(LanguageStyle, MissingFileIsNormal)
{
    // Nobody has to create codeblocks.ini, so a missing file resolves every
    // language to its plain default rather than failing.
    const LanguageStyles styles = LanguageStyles::load(QStringLiteral("/nonexistent/codeblocks.ini"));
    EXPECT_TRUE(styles.definedLanguages().isEmpty());
    const LanguageStyle python = styles.resolve(QStringLiteral("python"));
    EXPECT_EQ(python.name, QStringLiteral("python"));
    EXPECT_TRUE(python.icon.isEmpty());
    EXPECT_FALSE(python.color.isValid());
}

TEST(CalloutStyle, ColorIsTheBodyTextNotTheRail)
{
    // The format Jernej chose: `color` is the text, `bar` is the rail. The two
    // were once crossed, so a rail came out in the colour meant for words.
    CalloutStyle style;
    style.bar = QColor(0x11, 0x22, 0x33);
    QMap<QString, QString> attributes;
    attributes.insert(QStringLiteral("color"), QStringLiteral("#E6E6E6ff"));
    style.apply(attributes);
    EXPECT_EQ(style.color, QColor(0xE6, 0xE6, 0xE6));
    EXPECT_EQ(style.bar, QColor(0x11, 0x22, 0x33)) << "color must leave the rail alone";

    attributes.clear();
    attributes.insert(QStringLiteral("bar"), QStringLiteral("#FF0000ff"));
    style.apply(attributes);
    EXPECT_EQ(style.bar, QColor(255, 0, 0));
    EXPECT_EQ(style.color, QColor(0xE6, 0xE6, 0xE6));
}


// --- code block colours (ComplexCodeTest) ------------------------------------

TEST(ComplexCodeTest, TokenStyleParsesColourAndFlags)
{
    const auto full = TokenStyle::parse(QStringLiteral("#98C379 Italic bold"));
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->color, QColor(0x98, 0xC3, 0x79));
    EXPECT_TRUE(full->italic);
    EXPECT_TRUE(full->bold);
    EXPECT_FALSE(full->underline);

    const auto flagsOnly = TokenStyle::parse(QStringLiteral("underline"));
    ASSERT_TRUE(flagsOnly.has_value());
    EXPECT_FALSE(flagsOnly->color.isValid());
    EXPECT_TRUE(flagsOnly->underline);

    EXPECT_FALSE(TokenStyle::parse(QString()).has_value());
    EXPECT_FALSE(TokenStyle::parse(QStringLiteral("#98C379 wobbly")).has_value())
        << "an unknown word drops the rule rather than guessing";
}

TEST(ComplexCodeTest, IniKeysResolveAndInherit)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("codeblocks.ini"));
    writeFile(path,
              QStringLiteral("[python]\n"
                             "bg = #1E2A3Aff        ; panel fill\n"
                             "fg = #D8DEE9\n"
                             "border = #3776AB80\n"
                             "syntax.keyword = #C678DD\n"
                             "syntax.function.builtin = #61AFEF bold\n"
                             "syntax.bogus = nonsense\n"
                             "aliases = py\n"
                             "[cython]\n"
                             "inherits = python\n"
                             "bg = #000000ff\n"));
    const LanguageStyles styles = LanguageStyles::load(path);

    const LanguageStyle python = styles.resolve(QStringLiteral("py"));
    EXPECT_EQ(python.bg, QColor(0x1E, 0x2A, 0x3A));
    EXPECT_EQ(python.fg, QColor(0xD8, 0xDE, 0xE9));
    EXPECT_EQ(python.border.alpha(), 0x80);
    EXPECT_EQ(python.syntax.size(), 2) << "the unparsable value is dropped";
    EXPECT_TRUE(python.syntax.value(QStringLiteral("function.builtin")).bold);
    EXPECT_TRUE(python.hasColours());

    const LanguageStyle cython = styles.resolve(QStringLiteral("cython"));
    EXPECT_EQ(cython.bg, QColor(0, 0, 0)) << "the child's own key wins";
    EXPECT_EQ(cython.fg, python.fg) << "and the rest comes from the parent";
    EXPECT_EQ(cython.syntax.size(), 2);

    EXPECT_FALSE(styles.resolve(QStringLiteral("rust")).hasColours())
        << "an unconfigured language has no colour knob set";
}

TEST(ComplexCodeTest, ASchemeCarriesASyntaxObject)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("Mine.json"));
    writeFile(path,
              QStringLiteral(R"({"name":"Mine",
                  "syntax":{"keyword":"#C678DD bold","function.builtin":"#61AFEF"},
                  "dark":{"--color-red":"#ff0000","syntax":{"keyword":"#111111"}},
                  "light":{"--color-red":"#aa0000"}})"));

    const SyntaxStyles dark = Scheme::apply(path, Theme::Mode::Dark).syntaxStyles();
    EXPECT_EQ(dark.size(), 2);
    EXPECT_EQ(dark.value(QStringLiteral("keyword")).color, QColor(0x11, 0x11, 0x11))
        << "the mode's own object refines the shared one";
    EXPECT_EQ(dark.value(QStringLiteral("function.builtin")).color, QColor(0x61, 0xAF, 0xEF));

    const SyntaxStyles light = Scheme::apply(path, Theme::Mode::Light).syntaxStyles();
    EXPECT_TRUE(light.value(QStringLiteral("keyword")).bold);

    // Saved and read back, it is still there.
    const QString saved = QDir(dir.path()).filePath(QStringLiteral("Saved.json"));
    ASSERT_TRUE(Scheme::save(saved, QStringLiteral("Saved"), Scheme::apply(path, Theme::Mode::Dark),
                             Scheme::apply(path, Theme::Mode::Light)));
    EXPECT_EQ(Scheme::apply(saved, Theme::Mode::Dark).syntaxStyles().size(), 2);

    EXPECT_TRUE(Theme::builtin(Theme::Mode::Dark).syntaxStyles().isEmpty())
        << "the built-in schemes leave the highlighter's own colours alone";
}

TEST(Theme, navTokensExistInDarkAndLight)
{
    const Theme dark = Theme::builtin(Theme::Mode::Dark);
    const Theme light = Theme::builtin(Theme::Mode::Light);
    for (const char *token :
         {"--nav-item-background-hover", "--nav-item-background-active", "--nav-item-background-selected",
          "--nav-collapse-icon-color", "--nav-collapse-icon-color-collapsed",
          "--nav-indentation-guide-color", "--nav-file-tag-color", "--nav-file-tag-background",
          "--nav-heading-color", "--nav-item-color-hover"}) {
        EXPECT_TRUE(dark.color(QLatin1String(token)).isValid()) << token << " dark";
        EXPECT_TRUE(light.color(QLatin1String(token)).isValid()) << token << " light";
    }
    EXPECT_NE(dark.color(QStringLiteral("--nav-indentation-guide-color")),
              light.color(QStringLiteral("--nav-indentation-guide-color")));
    EXPECT_EQ(dark.value(QStringLiteral("--nav-item-size")).toInt(), 13);
}
