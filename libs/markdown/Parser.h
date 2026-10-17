#pragma once

#include <QString>

#include "Decorations.h"
#include "Metadata.h"
#include "SourceMap.h"

namespace noteAppC {

/// What a parse is asked to find beyond the Markdown itself. All off by
/// default, so the indexer and publisher pay nothing for them; the editor sets
/// what the note's settings say.
struct ParseOptions {
    /// A bare `[text]` that is not a link, footnote, task or callout marker:
    /// StyleSpan::Kind::BareBracket, which draws like a link would.
    bool brackets = false;

    /// Meta notes: a run of prose between a pair of delimiters, `[like this]`,
    /// for a writer's notes to themselves. Decorations::metaNotes holds the
    /// runs and StyleSpan::Kind::MetaNote draws them. With the brackets, a run
    /// is a meta note instead of a bare bracket.
    bool metaNotes = false;
    QString open = QStringLiteral("[");  ///< 1-3 characters
    QString close = QStringLiteral("]"); ///< 1-3 characters; may equal `open`

    /// The word count: leaves out everything between `excludeOpen` and
    /// `excludeClose`, delimiters included (found the way meta notes are, so
    /// a link's brackets are not a pair); and, with `ignoreFormatting`, counts
    /// only rendered words - not Markdown markers, link targets, HTML tags,
    /// list numbers or `%%` comments, text and all.
    bool excludeBetween = false;
    QString excludeOpen = QStringLiteral("[");
    QString excludeClose = QStringLiteral("]");
    bool ignoreFormatting = false;
};

/// Empty when `open`/`close` can delimit meta notes, else a sentence saying
/// why not: each is 1-3 characters, no spaces or backslash, and none of
/// `* _ ~ = ` $ % # |`, which are Markdown syntax. `anySyntax` allows those
/// (the word count's exclusion has no use for the rule).
QString metaNoteDelimitersError(const QString &open, const QString &close, bool anySyntax = false);

/// One Markdown parser for the whole application - editor, reading view and
/// indexer - so the three can never disagree about what a note says.
///
/// md4qt handles CommonMark 0.31.2 and the GitHub extensions; everything
/// Obsidian adds on top is a pass over the source afterwards
/// (see ObsidianSyntax). Instances are cheap but not thread safe: give each
/// indexer thread its own.
class Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;

    /// Metadata and decorations from one parse. The editor wants both; the
    /// indexer only ever looks at the metadata.
    struct Result {
        Metadata metadata;
        Decorations decorations;
    };

    /// `path` is only used for messages and for the returned metadata.
    Metadata parse(const QString &text, const QString &path = {},
                   const ParseOptions &options = {}) const;
    /// Delimiters that fail metaNoteDelimitersError() find nothing.
    Result parseAll(const QString &text, const QString &path = {},
                    const ParseOptions &options = {}) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

/// The cells of one table row, GFM's way: the line, after any quote or
/// callout markers, split at every unescaped "|", each cell without the
/// spaces around it. An empty cell is an empty range inside its gap, where
/// typing into it lands - md4qt puts one on the pipe that closes it.
QList<Range> tableRowCells(const QString &text, qsizetype lineStart, qsizetype lineEnd);

/// Words in `text`, as the note's word count counts them: runs of letters,
/// digits, `_` and `'`.
int wordsIn(QStringView text);

/// The lines `#Heading` names - an ATX heading or an HTML `<h1>`-`<h6>`
/// whose text (tags removed, entities decoded) equals `wanted`, case
/// ignored - from its line to the next heading at its level or above, or the
/// end of the text. Headings inside a code fence, a comment or the frontmatter
/// do not count. An invalid Range when there is none. A scan of the lines,
/// not a parse: the embed and the canvas call it on notes they only read.
Range headingSection(const QString &text, const QString &wanted);

} // namespace noteAppC
