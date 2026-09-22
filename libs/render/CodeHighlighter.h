#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QTextLayout>

#include "Theme.h"

namespace noteAppC {

/// Syntax highlighting for fenced code blocks, from KSyntaxHighlighting's
/// Kate definitions - 300+ languages, the same engine Kate and KDevelop use.
///
/// Produces `QTextLayout::FormatRange`s in the coordinates of the string it
/// was given, so the editor can hand it a block's display text and append the
/// result to the formats that block already has. No separate painting path.
class CodeHighlighter {
public:
    CodeHighlighter();
    ~CodeHighlighter();

    /// Picks the Kate theme that suits the colour scheme. Call on theme change.
    void setTheme(const Theme &theme);

    /// `language` is the info string after the opening fence ("cpp", "py",
    /// "Rust", ...). Unknown languages return no formats rather than failing.
    QList<QTextLayout::FormatRange> highlight(const QString &text, const QString &language) const;

    /// True when the language has a definition, so the caller can decide
    /// whether a header badge is worth showing.
    bool knows(const QString &language) const;

    /// Obsidian and Prism use ids Kate does not always share.
    static QString kateNameFor(const QString &infoString);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace noteAppC
