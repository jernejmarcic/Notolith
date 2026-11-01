// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QSet>
#include <QString>

#include <functional>
#include <vector>

namespace noteAppC {

class Theme;

/// One colour a person can set for how a kind of text is drawn. `token` is the
/// Obsidian CSS variable it lands in; the setting is `appearance.<key>Dark` or
/// `...Light`, empty meaning "follow the theme". Display only: nothing here is
/// ever written to a note.
struct FormatColour {
    const char *token;
    const char *key;
    const char *part; ///< "Text", "Line", ... when a row holds several; else empty
};

/// One row of Settings > Appearance: a kind of text and the colours it owns.
struct FormatRow {
    const char *label;
    const char *description;
    std::vector<FormatColour> colours;
};

// Header-only so the settings library, which does not link the theme, can
// make a default for every key from the same table.
inline const std::vector<FormatRow> &formatRows()
{
    static const std::vector<FormatRow> rows = {
        {"Note title", "The title above a note, when it is shown.",
         {{"--inline-title-color", "inlineTitleColor", ""}}},
        {"Heading 1", "", {{"--h1-color", "h1Color", ""}}},
        {"Heading 2", "", {{"--h2-color", "h2Color", ""}}},
        {"Heading 3", "", {{"--h3-color", "h3Color", ""}}},
        {"Heading 4", "", {{"--h4-color", "h4Color", ""}}},
        {"Heading 5", "", {{"--h5-color", "h5Color", ""}}},
        {"Heading 6", "", {{"--h6-color", "h6Color", ""}}},
        {"Bold", "Bold italic takes this colour.", {{"--bold-color", "boldColor", ""}}},
        {"Italic", "", {{"--italic-color", "italicColor", ""}}},
        {"Underline", "Only <u> is underlined; Markdown has no underline.",
         {{"--underline-color", "underlineColor", "Text"},
          {"--underline-decoration-color", "underlineDecorationColor", "Line"}}},
        {"Strikethrough", "~~text~~, <s> and <del>.",
         {{"--strikethrough-color", "strikethroughColor", "Text"},
          {"--strikethrough-decoration-color", "strikethroughDecorationColor", "Line"}}},
        {"Done task text", "", {{"--checklist-done-color", "checklistDoneColor", ""}}},
        {"Highlight", "==text== and <mark>.",
         {{"--text-highlight-fg", "textHighlightFg", "Text"},
          {"--text-highlight-bg", "textHighlightBg", "Background"}}},
        {"Inline code", "`code`, <code> and <kbd>.",
         {{"--code-normal", "codeNormal", "Text"}, {"--code-background", "codeBackground", "Background"}}},
        {"Links", "",
         {{"--link-color", "linkColor", "Internal"},
          {"--link-external-color", "linkExternalColor", "External"},
          {"--link-unresolved-color", "linkUnresolvedColor", "Unresolved"}}},
        {"Tag", "", {{"--tag-color", "tagColor", ""}}},
        {"Quote text", "", {{"--blockquote-color", "blockquoteColor", ""}}},
        {"List markers", "Bullets and numbers.", {{"--list-marker-color", "listMarkerColor", ""}}},
        {"Checkbox", "",
         {{"--checkbox-color", "checkboxColor", "Fill"},
          {"--checkbox-marker-color", "checkboxMarkerColor", "Tick"},
          {"--checkbox-border-color", "checkboxBorderColor", "Border"}}},
        {"Footnote marks", "", {{"--footnote-color", "footnoteColor", ""}}},
    };
    return rows;
}

inline QString formatColourKey(const FormatColour &colour, bool dark)
{
    return QStringLiteral("appearance.") + QLatin1String(colour.key)
           + (dark ? QLatin1String("Dark") : QLatin1String("Light"));
}

inline const QSet<QString> &formatColourTokens()
{
    static const QSet<QString> tokens = [] {
        QSet<QString> set;
        for (const FormatRow &row : formatRows())
            for (const FormatColour &colour : row.colours)
                set.insert(QLatin1String(colour.token));
        return set;
    }();
    return tokens;
}


/// Pins the user's colours for `theme`'s mode over whatever the scheme set -
/// user setting, then scheme, then built-in. `text` reads one setting key.
void applyUserColours(Theme &theme, const std::function<QString(const QString &key)> &text);

} // namespace noteAppC
