#pragma once

#include <functional>
#include <memory>

#include <map>

#include <QHash>
#include <QList>
#include <QPair>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "Metadata.h"
#include "Parser.h"
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
/// database already maintains. Used from the thread that opened it, but for
/// search() and a Writer: the indexer parses on a pool and writes through a
/// Writer of its own, off this thread.
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

    /// The vault open() was given, absolute.
    QString vaultRoot() const { return m_vaultRoot; }

    // --- writing -------------------------------------------------------

    /// One transaction and one changed() for many put()s.
    void beginBatch();
    void endBatch();
    void put(const Metadata &metadata, const QString &body);
    void remove(const QString &path);
    /// Every meta-note row, at once and without a scan: what turning the
    /// editor's meta notes off costs.
    void clearMetaNotes();
    /// What the meta-note rows were written with, kept in the cache so a warm
    /// start can tell they are stale: empty for none (Indexer::setMetaNotes).
    QString metaNotesKey() const;
    void setMetaNotesKey(const QString &key);
    /// What to parse a note with before put(), so its meta-note rows are kept:
    /// the options metaNotesKey() records, defaults otherwise.
    ParseOptions parseOptions() const;
    /// mtime and size of what the cache holds, for skipping unchanged files.
    bool isCurrent(const QString &path, qint64 mtime, qint64 size) const;

    /// A second connection to an open cache, for another thread: the
    /// indexer's scan writes through one. It changes the database only. The
    /// names each write returns reach the name table through setNames(), on
    /// the Index's thread, and changed() waits for notifyChanged(); WAL lets
    /// the Index read in the meantime. Paths are absolute, as for put().
    class Writer {
    public:
        Writer(const QString &databasePath, const QString &vaultRoot);
        ~Writer();
        bool isOpen() const;
        /// `asNote`: the cached row must also be of that kind (a note, or a plain file).
        bool isCurrent(const QString &path, qint64 mtime, qint64 size, bool asNote);
        /// Notes only, vault-relative, as Index::allPaths().
        QStringList allPaths();
        void begin();
        void commit();
        /// Each returns the names the file now answers to, for setNames().
        QStringList put(const Metadata &metadata, const QString &body);
        /// A non-note file, so `![[image.png]]` resolves: no metadata and no
        /// full-text content, only its names.
        QStringList putAttachment(const QString &path, qint64 mtime, qint64 size);
        void remove(const QString &path);

    private:
        struct Private;
        std::unique_ptr<Private> d;
    };
    /// What a Writer wrote for `relative`: the names it answers to, or none
    /// once it is removed.
    void setNames(const QString &relative, const QStringList &names);
    /// changed(), for a Writer's work once all of it has landed.
    void notifyChanged() { emit changed(); }

    // --- reading -------------------------------------------------------

    int noteCount() const;
    /// Rows in the full-text table. Equal to the file count when the cache is
    /// consistent; the tests watch it because nothing reads the table until
    /// the search grammar lands, so a stale row would otherwise go unnoticed.
    int contentRowCount() const;
    int attachmentCount() const;
    /// True when the index holds any file - note or attachment - ending in one of `suffixes` (lower-case, no dot).
    bool hasFileWithSuffix(const QStringList &suffixes) const;
    QStringList allPaths() const;
    QStringList allTags() const;
    /// Every property key in the vault, with the type it was first seen
    /// with and how many notes carry it - Obsidian's "All properties".
    struct PropertyCount {
        QString key;
        Property::Type type = Property::Type::Text;
        int notes = 0;
    };
    QList<PropertyCount> propertyCounts() const;

    /// Obsidian's resolution order: exact path, then shortest unique
    /// basename, then alias, then frontmatter id. `fromPath` breaks ties in
    /// favour of the linking note's own folder. Empty when unresolved.
    QString resolve(const QString &target, const QString &fromPath = {}) const;

    QList<Backlink> backlinks(const QString &path) const;
    QList<Link> outgoingLinks(const QString &path) const;
    QStringList notesWithTag(const QString &tag) const;
    /// Every tag path's note count, nested children included the way
    /// notesWithTag() counts them: `a` gets a note tagged only `a/b`. One
    /// query for the whole vault, where a count per tag was one each.
    QHash<QString, int> tagCounts() const;
    /// Quick switcher: paths whose name matches `query`, best first.
    QStringList findByName(const QString &query, int limit = 50) const;
    /// Every name a wikilink may use, for the `[[` suggester; the `aliases:`
    /// entries are left out, aliases() lists those.
    QStringList linkTargets() const;
    /// Every `aliases:` entry as {alias, note name}: the suggester offers it
    /// as `[[Note|alias]]`. The note name is its basename, or its extensionless
    /// vault path (`folder/Note`) when another note answers to that basename.
    QList<QPair<QString, QString>> aliases() const;
    /// Headings, or block ids, of the note at vault-relative `path`, in source
    /// order: what `[[Note#` and `[[Note#^` complete. With `parents` (heading
    /// texts, outermost first) only the headings nested under that chain, for
    /// `[[Note#H1#`.
    QStringList fragmentsOf(const QString &path, bool blocks, const QStringList &parents = {}) const;

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
    QList<SearchResult> search(const QString &queryText, int limit = 200, QString *error = nullptr) const
    {
        return searchIn(m_databasePath, queryText, limit, error);
    }
    /// search() on the cache at `databasePath` (databasePath()), with no Index:
    /// a worker given a copy of the path cannot race the Index reopening on
    /// another vault, nor outlive it.
    static QList<SearchResult> searchIn(const QString &databasePath, const QString &queryText, int limit = 200,
                                        QString *error = nullptr);

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
    /// The index picks the notes to open; each one's links are found again in
    /// the text read, so a note saved since it was indexed is spliced right.
    /// A note that is not valid UTF-8, or that can no longer be read (deleted
    /// since the index looked), is skipped and named in `error`; the rest go on.
    /// `extraCandidates` are notes (vault-relative, at their paths before the
    /// rename) to open besides the ones the index names: those saved since it
    /// last looked, whose links it may not hold yet.
    /// Returns the number of notes rewritten, or -1 with `error` filled.
    int updateLinksAfterRename(const QString &oldRelative, const QString &newRelative,
                               const Vault &vault, QString *error = nullptr,
                               const QStringList &extraCandidates = {});

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
    /// depends on. Its own connections, like search(), so call it from a
    /// worker thread; the three child tables are read in parallel (half the
    /// time of reading them one after the other).
    QList<BaseNote> allNotesForBases() const { return notesForBases(m_databasePath); }
    /// The same from the cache at `databasePath` (databasePath()), with no
    /// Index at all: a worker that copies the path cannot outlive or race
    /// the Index it came from.
    static QList<BaseNote> notesForBases(const QString &databasePath);

signals:
    void changed();

private:
    bool createSchema();
    /// Drops every table, for when the cache's schema version is not ours.
    void loadNames();
    void rememberNames(const QString &path, const QStringList &names);
    void forgetNames(const QString &path);
    /// The new text for one link's target, keeping its basename-vs-path shape
    /// (a basename only when it stays unique) and any leading "./"/".md".
    QString rebasedTarget(const QString &oldTarget, const QString &oldRelative,
                          const QString &newRelative) const;
    /// True when no note but `excludePath` and `alsoExclude` (the renamed note
    /// before and after: a moved note rewritten early is already indexed at
    /// its new path) answers to `basename`.
    bool basenameStillUnique(const QString &basename, const QString &excludePath,
                             const QString &alsoExclude = {}) const;
    /// PRAGMA user_version of the database at `databasePath`; 0 when it
    /// cannot be read.
    static int storedVersion(const QString &databasePath);

    /// The engine behind search() and unlinkedMentions(): narrows the FTS5
    /// table, loads whatever metadata the query needs, and calls `onMatch`
    /// for every one of the first `limit` candidates that evaluate() judges
    /// a match. See Index.cpp for why it opens its own connection.
    static void eachMatch(const QString &databasePath, const SearchQuery &query, int limit,
                          const std::function<void(NoteContext &&, SearchOutcome &&)> &onMatch);

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
    /// every name any path registered -> how many paths did, in order, so
    /// linkTargets() needs no sort.
    std::map<QString, int> m_nameCounts;
    /// linkTargets() and aliases() cost ~4 ms on a 20 000-note vault and run
    /// on every index change; dropped when a note's names change (an alias is
    /// one of them) or a note goes, so a plain edit of the text keeps them.
    mutable QStringList m_linkTargetsCache;
    mutable QList<QPair<QString, QString>> m_aliasesCache;
    mutable bool m_linkTargetsValid = false;
    mutable bool m_aliasesValid = false;

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

    /// What graphDataIn() needs from the Index: the cache's path and the
    /// link-name table (implicitly shared, so taking one is a refcount bump
    /// and the Index may change under a worker that holds it).
    struct GraphSnapshot {
        QString databasePath;
        QString vaultRoot;
        QHash<QString, QStringList> names;
    };
    GraphSnapshot graphSnapshot() const;

    /// graphData() for a worker thread: its own database connection, no
    /// Index. Takes the snapshot rather than the Index so the UI thread is
    /// free to keep writing (same pattern as searchIn()).
    static GraphData graphDataIn(const GraphSnapshot &snapshot);

    /// resolve() over an explicit name table, for graphDataIn().
    static QString resolveIn(const QHash<QString, QStringList> &names, const QString &vaultRoot,
                             const QString &target, const QString &fromPath);
};

} // namespace noteAppC
