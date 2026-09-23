#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "SourceMap.h" // Range: UTF-16 offsets, the way QString indexes

namespace noteAppC {

/// One occurrence a query found, an offset pair into the note's raw text
/// (UTF-16 code units), the same coordinate space `Heading::range` and
/// friends already use. A field like `path:` or `file:` that matches without
/// pointing at a position in the text contributes no SearchMatch at all - the
/// note still matches, it just has nothing in the body to underline.
struct SearchMatch {
    qsizetype offset = 0;
    qsizetype length = 0;
};

/// Everything one note contributed to a search: every match (offsets into the
/// note's own raw text), and a snippet built around the first one for the
/// results list to show without the caller re-reading the note.
struct SearchResult {
    QString path; ///< vault-relative
    QList<SearchMatch> matches; ///< offsets into the note's raw text
    QString snippet;
    QList<SearchMatch> snippetHighlights; ///< offsets into `snippet`, for the UI to bold
    int matchCount = 0;
};

/// The slice of Metadata that field searches (`tag:`, `section:`, `task:`,
/// ...) need, addressed by a plain-old struct rather than the index's own
/// Backlink/Metadata types so this file has no SQL and no vault dependency -
/// it is pure text-and-ranges, which is what makes it unit-testable without a
/// database.
struct NoteTag {
    QString name; ///< without the "#", as Metadata::Tag stores it
    Range range;
};
struct NoteHeading {
    int level = 1;
    Range range; ///< the whole heading line, as Metadata::Heading stores it
};
struct NoteTask {
    bool done = false;
    Range range; ///< the whole list item, as Metadata::Task stores it
};
struct NoteProperty {
    QString key;
    QStringList values; ///< a list property's items, or the one value
};

/// One note's raw text plus the pieces of its metadata a field search reads.
/// `text` is the same string `Index::put()` was given as `body` - the file's
/// own bytes, decoded - so an offset here means the same thing it means
/// everywhere else in this codebase.
struct NoteContext {
    QString path; ///< vault-relative
    QString text;
    QList<NoteTag> tags;
    QList<NoteHeading> headings;
    QList<NoteTask> tasks;
    QList<NoteProperty> properties;
};

/// Which of a note's metadata tables a parsed query actually reads, so
/// Index::search() can skip loading the ones it does not - most queries are a
/// handful of bare words and never touch tags, headings or tasks at all, and
/// those tables can be as large as the vault.
struct FieldNeeds {
    bool tags = false;
    bool headings = false;
    bool tasks = false;
    bool properties = false;

    FieldNeeds operator|(const FieldNeeds &other) const
    {
        return {tags || other.tags, headings || other.headings, tasks || other.tasks,
                properties || other.properties};
    }
};

/// The result of testing one query against one note.
struct SearchOutcome {
    bool matched = false;
    QList<SearchMatch> matches;
};

/// Opaque parse-tree node; defined in Search.cpp. A forward declaration is
/// enough here because SearchQuery only ever holds a shared_ptr to one - the
/// deleter shared_ptr captures at construction (inside Search.cpp, where the
/// type is complete) is all destruction ever needs.
class SearchQueryNode;

/// A parsed Obsidian-style search query: bare words (AND by default), "quoted
/// phrases", OR, -negation, (parentheses), /regular expressions/, and the
/// file: path: content: tag: line:(...) block:(...) section:(...) task:
/// task-todo: task-done: match-case: ignore-case: operators, and
/// [property] / [property:value], documented at
/// https://obsidian.md/help/plugins/search.
///
/// Deliberately independent of SQL and of Index: parsing and evaluation are
/// pure functions over a string and a NoteContext, which is what lets
/// tests/test_search.cpp exercise every operator without a database. Index
/// supplies the other half - using ftsExpression() to narrow SQLite's FTS5
/// table to candidates, then calling evaluate() on each to confirm the match
/// and locate it precisely.
class SearchQuery {
public:
    SearchQuery() = default;

    /// Parses `text`. On failure the returned query isValid() == false and
    /// error() explains why (an unterminated quote or regex, an unmatched
    /// paren, an unknown field, a value missing after ':').
    static SearchQuery parse(const QString &text);

    bool isValid() const { return m_root != nullptr; }
    QString error() const { return m_error; }

    /// A conservative SQLite FTS5 MATCH expression: any note this query can
    /// possibly match also satisfies this expression, so it is always safe to
    /// use as a WHERE clause before confirming with evaluate(). Empty when no
    /// useful restriction could be derived (a bare `-word`, or a query that is
    /// only path:/file:/tag:/task: fields, or a bare /regex/) - the caller
    /// should then look at every note rather than filter on nothing.
    QString ftsExpression() const;

    /// Which metadata tables evaluate() will read for this query, so a caller
    /// backed by a database can skip loading the ones it never touches.
    FieldNeeds fieldNeeds() const;

    /// True with every match (and its offsets) when `note` satisfies the
    /// query, case sensitivity defaulting as `caseSensitivity` for any term
    /// not itself wrapped in match-case:/ignore-case:.
    SearchOutcome evaluate(const NoteContext &note,
                           Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive) const;

private:
    std::shared_ptr<const SearchQueryNode> m_root;
    QString m_error;
};

/// A snippet of text plus where, inside that snippet, to highlight - the
/// offsets in `matches` shifted into the snippet's own coordinates, which is
/// why this is a function rather than plain string slicing.
struct SearchSnippet {
    QString text;
    QList<SearchMatch> highlights; ///< offsets into `text`
};

/// A window of `text` centred on the first of `matches` with `length > 0`
/// (or, with none, the start of the text - a path/file-only match has
/// nowhere else to point), newlines flattened to spaces so it prints on one
/// line. Every match from `matches` that falls inside the window is carried
/// over as a highlight. Shared with BacklinksPane's unlinked mentions, which
/// build their context the same way search results do.
SearchSnippet buildSnippet(const QString &text, const QList<SearchMatch> &matches, int radius = 60);

} // namespace noteAppC
