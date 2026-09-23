#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QTextLayout>

#include <memory>

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

    /// Where highlighting stands between two lines of one listing: opaque and
    /// cheap to copy. Default-constructed is the start of a listing. Equal
    /// states highlight whatever follows identically, which is what lets an
    /// edit stop re-highlighting a listing once they agree again.
    class State {
    public:
        State();
        ~State();
        State(const State &other);
        State &operator=(const State &other);
        bool operator==(const State &other) const;

    private:
        friend class CodeHighlighter;
        struct Data;
        std::shared_ptr<const Data> d;
    };

    /// `language` is the info string after the opening fence ("cpp", "py",
    /// "Rust", ...). Unknown languages return no formats rather than failing.
    QList<QTextLayout::FormatRange> highlight(const QString &text, const QString &language) const;

    /// The same, for lines that continue a listing from `*state`; leaves
    /// `*state` where the last of them ended.
    QList<QTextLayout::FormatRange> highlight(const QString &text, const QString &language,
                                              State *state) const;

    /// True when the language has a definition, so the caller can decide
    /// whether a header badge is worth showing.
    bool knows(const QString &language) const;

    /// Obsidian and Prism use ids Kate does not always share.
    static QString kateNameFor(const QString &infoString);

    /// False in a build configured without KF6 SyntaxHighlighting (the
    /// Android Qt kit does not carry it): `highlight()` then always returns
    /// no formats and `knows()` is always false, but code still renders,
    /// monospaced and uncoloured.
    static bool isAvailable();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace noteAppC
