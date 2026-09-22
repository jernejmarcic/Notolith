#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include "Theme.h"

namespace noteAppC {

/// A colour scheme that lives on disk.
struct SchemeInfo {
    QString name;
    QString path;
    /// True for an Obsidian theme's `theme.css`, read for its colours only.
    bool obsidianCSS = false;
    /// True when the file sits in the vault rather than the user's config.
    bool inVault = false;
};

/// Custom colour schemes: noteAppC's own JSON files, and the CSS of any
/// Obsidian theme already installed in the vault.
///
/// Only colour *variables* come across from an Obsidian theme. Its layout CSS
/// does not, and cannot - QSS is not CSS. That is the documented trade in
/// PLAN.md section 8: palettes port, layouts do not.
class Scheme {
public:
    /// Directories searched for schemes, vault first so a vault can carry its
    /// own. `~/.config/noteAppC/themes` is the user-wide one.
    static QStringList searchPaths(const QString &vaultRoot = {});

    /// Every scheme found, including Obsidian themes under
    /// `<vault>/.obsidian/themes/<name>/theme.css`. Sorted by name.
    static QList<SchemeInfo> available(const QString &vaultRoot = {});

    /// Tokens a scheme defines for `mode`. A `.json` file is read as
    /// noteAppC's own format, anything else as Obsidian theme CSS. Empty with
    /// `error` filled when the file cannot be read.
    static QHash<QString, QString> load(const QString &path, Theme::Mode mode,
                                        QString *error = nullptr);

    /// The built-in scheme of that mode with the file's tokens pinned over
    /// it, so a scheme that sets three colours still gets a whole theme.
    static Theme apply(const QString &path, Theme::Mode mode, QString *error = nullptr);

    /// The importer. Every `--name: value` declaration inside a `:root`,
    /// `body`, or matching `.theme-dark` / `.theme-light` block, with `var()`
    /// resolved and every value that is not a colour dropped. Mode-specific
    /// blocks win over generic ones whatever order the file puts them in.
    static QHash<QString, QString> parseObsidianCSS(const QString &css, Theme::Mode mode);

    /// Contrast ratio of two colours, WCAG 2.1. 1.0 is identical, 21.0 is
    /// black on white.
    static double contrastRatio(const QColor &a, const QColor &b);

    /// Repairs an import that would leave the app unusable: flattens a
    /// translucent window background onto the built-in one, and drops text
    /// colours that cannot be read against the background that came with
    /// them. A theme using `color-mix()` or `light-dark()` resolves only
    /// partly, and a half-resolved palette is how you get black on black.
    static void makeUsable(Theme &theme);

    /// Writes both modes' overrides as a noteAppC scheme.
    static bool save(const QString &path, const QString &name, const Theme &dark,
                     const Theme &light, QString *error = nullptr);

    /// A CSS colour as something Qt understands: `#rrggbb`, `#rrggbbaa` or
    /// `rgba(r, g, b, a)`. Empty when the value is not a colour at all, which
    /// is how non-colour variables (sizes, fonts, `color-mix()`) are dropped.
    static QString normaliseColor(const QString &value);

private:
    static QString resolveVars(const QString &value, const QHash<QString, QString> &raw,
                               int depth = 0);
};

} // namespace noteAppC
