#pragma once

#include <QColor>
#include <QHash>
#include <QPalette>
#include <QSet>
#include <QString>

#include "LanguageStyle.h"

namespace noteAppC {

/// Colour scheme. Tokens are keyed by Obsidian's own CSS variable names
/// (`--background-primary`, `--text-muted`, ...) so that a palette lifted from
/// an Obsidian theme drops straight in, and so the Phase 4 importer has
/// nothing to translate.
///
/// The built-in schemes carry Obsidian's documented default values verbatim.
/// Values that Obsidian derives with `color-mix()` or `calc()` are computed
/// here instead, because Qt Style Sheets has neither.
class Theme {
public:
    /// Oled is a true-black variant of Dark: every background collapses to
    /// #000000 so an OLED panel leaves those pixels unlit. It behaves as a
    /// dark scheme everywhere else - `isDark()` is true for it.
    enum class Mode { Dark, Light, Oled };

    /// Dark is the default, as in Obsidian and as Jernej asked.
    static Theme builtin(Mode mode = Mode::Dark);

    Mode mode() const { return m_mode; }
    /// Same tokens, pins and fonts: what applying either would draw alike.
    bool operator==(const Theme &) const = default;
    /// Equal in mode, fonts and every token but those in `ignored` (pins are
    /// not compared, they draw nothing): says whether a recolour is all that
    /// changed.
    bool sameExcept(const Theme &other, const QSet<QString> &ignored) const;
    bool isDark() const { return m_mode != Mode::Light; }
    bool isOled() const { return m_mode == Mode::Oled; }

    /// The settings spelling of a mode, and back. Unknown text reads as Dark.
    static QString nameOf(Mode mode);
    static Mode modeFromName(const QString &name);

    /// Raw token value, empty when the token is unknown.
    QString value(const QString &token) const;
    QColor color(const QString &token) const;

    /// Any CSS colour Qt's own parser does not take: `rgb()`, `rgba()`,
    /// `hsl()`, `hsla()`, `transparent`, alongside the hex and named forms it
    /// does. Invalid when the value is not a colour, which is how the scheme
    /// importer tells a colour from a size.
    static QColor parseColor(const QString &value);

    /// Pins a token. A pinned token is never recomputed from the base scale,
    /// which is what lets a custom scheme override a semantic colour like
    /// `--background-primary` instead of only the greys it is derived from.
    void setToken(const QString &token, const QString &value);
    /// Pins every token in `tokens`, then rederives the rest once.
    void setTokens(const QHash<QString, QString> &tokens);
    /// Unpins a token, so the next derivation computes it again.
    void resetToken(const QString &token);
    bool isPinned(const QString &token) const { return m_pinned.contains(token); }
    /// Tokens that differ from the built-in scheme of the same mode.
    QHash<QString, QString> overrides() const;

    /// Accent colour, rebuilt from `--accent-h/s/l` like Obsidian's picker.
    void setAccent(int hue, int saturationPercent, int lightnessPercent);

    /// Token colours every code block starts from: Obsidian's `--code-keyword`,
    /// `--code-string`, ... (so an imported theme's code colours land), then
    /// the `--syntax-<capture>` tokens a scheme's `syntax` object becomes,
    /// which win. Empty for the built-in schemes - a block then keeps the
    /// highlighter's own colours.
    SyntaxStyles syntaxStyles() const;

    /// Qt Style Sheet for the whole application, generated from the tokens.
    QString styleSheet() const;
    /// Palette for the stock widgets QSS does not reach (dialogs, menus).
    QPalette palette() const;

    QFont uiFont() const;
    QFont textFont() const;
    QFont monospaceFont() const;

    /// Family overrides from the settings page. An empty family means "use the
    /// built-in list", which is how a reset works; a family fontconfig does
    /// not have is ignored rather than substituted, so a typo does not
    /// silently change the look of the whole application.
    void setFontOverrides(const QString &interfaceFamily, const QString &textFamily,
                          const QString &monospaceFamily);
    QString interfaceFontOverride() const { return m_interfaceFont; }
    QString textFontOverride() const { return m_textFont; }
    QString monospaceFontOverride() const { return m_monospaceFont; }

    /// True when fontconfig actually has this family, so the settings page can
    /// say so instead of letting a typo quietly do nothing.
    static bool hasFamily(const QString &family);

    /// First family in `families` that fontconfig actually has, else the last.
    static QString firstAvailableFamily(const QStringList &families);

    /// Accent colour Obsidian gives a callout type; falls back to blue.
    QColor calloutColor(const QString &type) const;
    /// Lucide icon name for a callout type.
    static QString calloutIcon(const QString &type);

private:
    void applyDerivedTokens();

    Mode m_mode = Mode::Dark;
    QHash<QString, QString> m_tokens;
    /// Tokens a scheme or the user set by hand; derivation leaves them alone.
    QSet<QString> m_pinned;
    QString m_interfaceFont;
    QString m_textFont;
    QString m_monospaceFont;
};

} // namespace noteAppC
