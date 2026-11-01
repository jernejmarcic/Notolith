// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>

namespace noteAppC {

class Theme;

/// Everything a callout can be told to look like.
///
/// Obsidian gives a callout only two knobs, both CSS-only: `--callout-color`
/// as an `R, G, B` triplet and `--callout-icon`. This is noteAppC's own, and
/// it reaches the fonts, the sizes, the effects and both colours separately,
/// because callouts need that much control.
///
/// A field left unset defers: the colours fall back to the type's accent, the
/// fonts and sizes to the theme's own. Resolution is three layers, each
/// overriding the one before - the built-in type, the section in
/// `callouts.ini`, then the attributes written on the callout itself.
struct CalloutStyle {
    /// Lucide id (`alarm-clock`), or a `file://`, plain path or `https://`
    /// URL. Drawn at one size whatever it is, so a photograph and a glyph
    /// line up in the same column.
    QString icon;
    /// The rail down the left edge. Invalid means the type's accent colour.
    QColor bar;
    /// The panel fill. Invalid means the accent at a tenth opacity, which is
    /// what Obsidian does.
    QColor background;

    QString font;        ///< empty: the theme's text font
    int size = 0;        ///< pixels, 0: the theme's text size
    QColor color;        ///< invalid: the theme's normal text colour
    QFont::Weight weight = QFont::Normal;
    bool italic = false;
    bool underline = false;
    bool strikeOut = false;

    QString headingFont;
    int headingSize = 0;
    QColor headingColor; ///< invalid: the rail colour, as Obsidian titles do
    QFont::Weight headingWeight = QFont::DemiBold;
    bool headingItalic = false;
    bool headingUnderline = false;
    bool headingStrikeOut = false;

    /// Applies `key=value` attributes over whatever is already set. Unknown
    /// keys are ignored rather than rejected, so a note written for a later
    /// version still renders.
    void apply(const QMap<QString, QString> &attributes);

    /// The body font, and the title font, with the theme filling the gaps.
    QFont bodyFont(const Theme &theme) const;
    QFont titleFont(const Theme &theme) const;

    /// `#RGB`, `#RRGGBB` and `#RRGGBBAA` - CSS order, alpha last. Qt's own
    /// parser reads eight hex digits as `#AARRGGBB`, which is the other way
    /// round and would silently turn a transparent fill into a coloured one.
    /// Also takes the `rgb()`/`hsl()`/named forms Theme::parseColor knows.
    static QColor parseColor(const QString &value);
};

/// The `callouts.ini` file: one section per callout type, merged over the 13
/// built-in Obsidian types.
///
///     [deadline]
///     icon = alarm-clock
///     bar  = #FF5A5Fff
///     bg   = #FF5A5F1A
///
/// Loading a file that is not there is not an error; it is the normal case.
class CalloutStyles {
public:
    /// `~/.config/notolith/callouts.ini` unless `path` says otherwise.
    static CalloutStyles load(const QString &path = {});
    static QString defaultPath();

    /// Built-in type, then the file's section, then the callout's own
    /// attributes. `theme` supplies the accent the type falls back to.
    CalloutStyle resolve(const QString &type, const QMap<QString, QString> &attributes,
                         const Theme &theme) const;

    /// Type names the file defines, sorted. For the settings page.
    QStringList definedTypes() const;
    /// Obsidian's 13 types, then the file's others sorted: what a callout can
    /// be changed to.
    QStringList knownTypes() const;

private:
    QHash<QString, QMap<QString, QString>> m_sections;
};

} // namespace noteAppC
