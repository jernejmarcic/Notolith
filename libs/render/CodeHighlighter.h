#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QTextFormat>
#include <QTextLayout>

#include <memory>

#include "Theme.h"

namespace noteAppC {

/// Token colours for one code block, resolved the way Zed resolves a theme's
/// `syntax` object: a token has a dotted capture name (`function.builtin`) and
/// takes the style of the longest dotted prefix some entry names - `function`
/// covers it when `function.builtin` is absent, `functionx` never does.
///
/// Built from layers, most specific first (fence, codeblocks.ini section,
/// theme): the first layer with a matching prefix decides, so a fence's
/// `syntax.keyword` also beats a theme's `keyword.control`. An empty map, or
/// no map at all, leaves the highlighter's own colours untouched.
class SyntaxMap {
public:
    SyntaxMap() = default;
    explicit SyntaxMap(QList<SyntaxStyles> layers);

    /// A map that names no colour but still has the highlighter tag every
    /// token with its capture name, for a caller (the publisher) that wants
    /// the same token segmentation from two runs of which one is mapped.
    static SyntaxMap tagging();

    bool isEmpty() const { return m_signature == 0; }
    /// Same signature, same colours: what the highlighter keys its caches by.
    quint64 signature() const { return m_signature; }

    /// Null when no layer names a prefix of `capture`. `matched`, when given,
    /// gets the key that did (`keyword` for `keyword.control`).
    const TokenStyle *resolve(const QString &capture, QString *matched = nullptr) const;

    /// The capture a Kate `Theme::TextStyle` stands for (as an int, so this
    /// header needs no KSyntaxHighlighting). Empty for plain text, and in a
    /// build without the library.
    static QString captureOf(int kateTextStyle);

    /// The capture name a mapped format was made from, empty for a format that
    /// came out of an unmapped run. Lets the publisher write `tok-*` classes.
    static QString captureOf(const QTextCharFormat &format);

private:
    QList<SyntaxStyles> m_layers;
    quint64 m_signature = 0;
};

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
    /// `map`, when it names anything, recolours tokens; null (or empty) is
    /// exactly the output there was before maps existed.
    QList<QTextLayout::FormatRange> highlight(const QString &text, const QString &language,
                                              const SyntaxMap *map = nullptr) const;

    /// The same, for lines that continue a listing from `*state`; leaves
    /// `*state` where the last of them ended.
    QList<QTextLayout::FormatRange> highlight(const QString &text, const QString &language,
                                              State *state, const SyntaxMap *map = nullptr) const;

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
