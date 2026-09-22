#pragma once

#include <QColor>
#include <QHash>
#include <QPalette>
#include <QString>

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
    enum class Mode { Dark, Light };

    /// Dark is the default, as in Obsidian and as Jernej asked.
    static Theme builtin(Mode mode = Mode::Dark);

    Mode mode() const { return m_mode; }
    bool isDark() const { return m_mode == Mode::Dark; }

    /// Raw token value, empty when the token is unknown.
    QString value(const QString &token) const;
    QColor color(const QString &token) const;
    void setToken(const QString &token, const QString &value);

    /// Accent colour, rebuilt from `--accent-h/s/l` like Obsidian's picker.
    void setAccent(int hue, int saturationPercent, int lightnessPercent);

    /// Qt Style Sheet for the whole application, generated from the tokens.
    QString styleSheet() const;
    /// Palette for the stock widgets QSS does not reach (dialogs, menus).
    QPalette palette() const;

    QFont uiFont() const;
    QFont textFont() const;
    QFont monospaceFont() const;

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
};

} // namespace noteAppC
