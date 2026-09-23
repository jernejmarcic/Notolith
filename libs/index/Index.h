#pragma once

#include <functional>

#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "Metadata.h"
#include "Search.h"

namespace noteAppC {

class Vault;

/// One resolved reference from one note to another.
struct Backlink {
    QString fromPath;  ///< note holding the link
    QString context;   ///< the source line, for the backlinks pane
    Link link;
};

/// The metadata cache: SQLite with FTS5, one database per vault under
/// $XDG_CACHE_HOME, the way Obsidian also keeps its cache outside the vault.
///
/// Owns link resolution too, because resolving needs the same name table the
/// database already maintains. Single threaded: parsing happens on a pool,
/// every write lands here.
class Index : public QObject {
    Q_OBJECT

public:
    explicit Index(QObject *parent = nullptr);
    ~Index() override;

    /// Opens (and migrates) the cache for `vaultRoot`. False on failure, with
    /// the reason in lastError().
    bool open(const QString &vaultRoot);
    void close();
    bool isOpen() const;
    QString lastError() const { return m_error; }
    QString databasePath() const { return m_databasePath; }

    /// Where the cache for a vault lives; public so the tests can point it
    /// somewhere temporary.
    static QString cachePathFor(const QString &vaultRoot);

    // --- writing -------------------------------------------------------

    void beginBatch();
    void endBatch();
    void put(const Metadata &metadata, const QString &body);
    /// Records a non-note file so `![[image.png]]` resolves. Attachments carry
    /// no metadata and no full-text content, only their names.
    void putAttachment(const QString &path, qint64 mtime, qint64 size);
    void remove(const QString &path);
    /// mtime and size of what the cache holds, for skipping unchanged files.
    bool isCurrent(const QString &path, qint64 mtime, qint64 size) const;

    // --- reading -------------------------------------------------------

    int noteCount() const;
    /// Rows in the full-text table. Equal to the file count when the cache is
    /// consistent; the tests watch it because nothing reads the table until
    /// the search grammar lands, so a stale row would otherwise go unnoticed.
    int contentRowCount() const;
    int attachmentCount() const;
    QStringList allPaths() const;
    QStringList allTags() const;

    /// Obsidian's resolution order: exact path, then shortest unique
    /// basename, then alias, then frontmatter id. `fromPath` breaks ties in
    /// favour of the linking note's own folder. Empty when unresolved.
    QString resolve(const QString &target, const QString &fromPath = {}) const;

    QList<Backlink> backlinks(const QString &path) const;
    QList<Link> outgoingLinks(const QString &path) const;
    QStringList notesWithTag(const QString &tag) const;
    /// Quick switcher: paths whose name matches `query`, best first.
    QStringList findByName(const QString &query, int limit = 50) const;
    /// Every name a wikilink may use, for the `[[` suggester.
    QStringList linkTargets() const;

    // --- search (libs/index/Search.h has the grammar and the evaluator) ---

    /// Obsidian's search grammar against the full-text table: FTS5 narrows to
    /// candidates (SearchQuery::ftsExpression()), then every candidate is
    /// confirmed and located with SearchQuery::evaluate() against the text
    /// `put()` was given, so offsets land in the same coordinate space as
    /// every other range in this codebase. Safe to call from any thread other
    /// than the one that opened this Index: it never touches `m_db`, opening
    /// its own short-lived connection instead, so a search running on a
    /// worker thread cannot race the indexer's writes. `limit` caps the
    /// number of notes returned; a note may itself carry many matches. Empty
    /// (or all-whitespace) queries return no results without touching the
    /// database. On a parse error, returns no results and fills `error`.
    QList<SearchResult> search(const QString &queryText, int limit = 200, QString *error = nullptr) const;

    /// Obsidian's "Unlinked mentions": notes that mention this note's title
    /// or one of its aliases as plain text without a `[[...]]`/`[...](...)`
    /// to it. Built on the same search engine as search() - one quoted-OR
    /// query over the note's own linkNames() - with any occurrence that
    /// falls inside a link this note's backlinks() already accounts for
    /// masked out, so a note that both links here and separately mentions
    /// the bare name shows only the second kind here. Unlike search(), only
    /// for the thread that opened this Index: backlinks() reads `m_db`. It
    /// costs 0.3 ms on a 20 000-note vault (noteAppC-bench), so the pane
    /// calls it directly.
    QList<SearchResult> unlinkedMentions(const QString &path, int limit = 200) const;

    /// Rewrites every wikilink and Markdown link that resolves to `oldRelative`
    /// so it points at `newRelative`, preserving each link's own form: an alias
    /// stays (`[[old|shown]]` -> `[[new|shown]]`), a subpath stays
    /// (`[[old#heading]]`), an embed stays an embed, a Markdown link keeps its
    /// percent-encoding, and a link written as a shortest-unique basename stays a
    /// basename when that is still unique. Only the link's bytes change.
    /// Returns the number of notes rewritten, or -1 with `error` filled.
    int updateLinksAfterRename(const QString &oldRelative, const QString &newRelative,
                               const Vault &vault, QString *error = nullptr);

    // --- Bases (libs/ext/bases) -----------------------------------------
    // One read-only block for the optional Bases module (NOTEAPPC_WITH_BASES).
    // Nothing above this point reads or writes it, and it changes no table.

    /// One note as a base needs it: the file facts and frontmatter properties
    /// already in the cache, plus its own tags and outgoing link targets.
    /// `mtime`/`size` come straight from `files`; there is no `ctime` column,
    /// so `file.ctime` is left to whoever builds the row from the vault (a
    /// base that never asks for it never pays for a stat() the cache does
    /// not keep).
    struct BaseNote {
        QString path; ///< vault-relative
        qint64 mtime = 0;
        qint64 size = 0;
        QList<Property> properties;
        QStringList tags;
        QStringList linkTargets; ///< raw text of each outgoing link's target
    };

    /// Every indexed note (not attachment) with its properties, tags and
    /// outgoing link targets, for libs/ext/bases to filter and sort in
    /// memory. Four queries total - files, props, tags, links - never one
    /// per note, the same shape the 20 000-note indexing budget already
    /// depends on.
    QList<BaseNote> allNotesForBases() const;

signals:
    void changed();

private:
    bool createSchema();
    /// Drops every table, for when the cache's schema version is not ours.
    void dropSchema();
    void loadNames();
    void indexNames(qint64 fileId, const QString &path, const QStringList &names);
    void forgetNames(const QString &path);
    /// The new text for one link's target, keeping its basename-vs-path shape
    /// (a basename only when it stays unique) and any leading "./"/".md".
    QString rebasedTarget(const QString &oldTarget, const QString &oldRelative,
                          const QString &newRelative) const;
    bool basenameStillUnique(const QString &basename, const QString &excludePath) const;

    /// The engine behind search() and unlinkedMentions(): narrows the FTS5
    /// table, loads whatever metadata the query needs, and calls `onMatch`
    /// for every one of the first `limit` candidates that evaluate() judges
    /// a match. See Index.cpp for why it opens its own connection.
    void eachMatch(const SearchQuery &query, int limit,
                   const std::function<void(NoteContext &&, SearchOutcome &&)> &onMatch) const;

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_databasePath;
    QString m_vaultRoot;
    QString m_error;
    bool m_inBatch = false;

    /// lower-cased name -> vault-relative paths that answer to it.
    QHash<QString, QStringList> m_names;
    /// vault-relative path -> the names it registered, so a remove is exact.
    QHash<QString, QStringList> m_pathNames;

    // --- graph view (libs/ext/graph) ----------------------------------
    // Added for the optional graph module. Kept in its own block at the end
    // of the class, separate from everything above, since another engineer
    // edits the rest of Index in parallel.
public:
    /// One note or attachment as the graph needs it - no ranges, no
    /// properties, just enough to build a node.
    struct GraphFileInfo {
        QString path; ///< vault-relative
        bool isAttachment = false;
        QStringList tags;
    };

    /// One internal link as the graph needs it. `toPath` is resolve()'s
    /// answer, empty when the link does not resolve to anything in the
    /// vault; `targetName` is the raw target text, for labelling that case.
    struct GraphLinkInfo {
        QString fromPath;
        QString toPath;
        QString targetName;
    };

    struct GraphData {
        QList<GraphFileInfo> files;
        QList<GraphLinkInfo> links;
    };

    /// Every note/attachment and every internal link in the vault, in two
    /// queries - no per-note round trips. GraphView (libs/ext/graph) turns
    /// this into nodes and edges and does its own filtering; Index does
    /// neither.
    GraphData graphData() const;
};

} // namespace noteAppC
