#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// A note as it sits in memory: text normalised to LF, plus the byte-level
/// details of the file it came from so that saving restores them exactly.
/// Non-negotiable 1 of the plan: APP never rewrites bytes it did not edit.
struct Note {
    QString text;      ///< LF line endings, no byte-order mark
    bool crlf = false; ///< the file on disk used CRLF
    bool bom = false;  ///< the file on disk started with a UTF-8 BOM
};

/// A vault is a directory of Markdown files, the same thing Obsidian means by
/// the word. Stateless beyond its root path: the index and the watcher arrive
/// in Phase 1.
class Vault {
public:
    Vault() = default;
    explicit Vault(const QString &rootPath);

    bool isValid() const;
    QString root() const { return m_root; }
    /// Directory basename, which is what Obsidian shows as the vault name.
    QString name() const;
    /// True when Obsidian has been here, so its settings can be read later.
    bool hasObsidianConfig() const;

    /// Extensions APP opens in the editor. Obsidian only treats `.md` as a
    /// note; `.markdown` is accepted on read because some vaults carry it.
    static bool isNote(const QString &path);

    /// Path relative to the vault root, for display and for link resolution.
    QString relative(const QString &absolutePath) const;
    QString absolute(const QString &relativePath) const;
    bool contains(const QString &absolutePath) const;
    /// The absolute path of the note `relativePath` names, for a caller
    /// that is not trusted with the rest of the disk (a plugin): empty when
    /// it is not a note, or when "..", an absolute path or a symlink would
    /// take it out of the vault. It need not exist, but its folder must.
    QString confinedNotePath(const QString &relativePath) const;

    /// Reads UTF-8, normalising CRLF to LF and stripping a BOM. On failure
    /// returns false and fills `error`.
    static bool read(const QString &path, Note &out, QString *error = nullptr);

    /// Writes through a temporary file and rename(2), restoring the line
    /// endings and BOM recorded in `note`.
    static bool write(const QString &path, const Note &note, QString *error = nullptr);

    /// Round-trip helper used by the tests: the exact bytes `note` would write.
    static QByteArray encode(const Note &note);
    static Note decode(const QByteArray &bytes);

    // --- file operations -------------------------------------------------

    /// Creates an empty note - or, with another extension, a file holding
    /// `text` (a new canvas is `{}`). `folderRelative` may be empty for the
    /// vault root. Uniquifies like Obsidian: "Untitled", "Untitled 1".
    /// Returns the absolute path, or an empty string with `error` filled.
    QString createNote(const QString &folderRelative, const QString &baseName,
                       QString *error = nullptr, const QString &extension = QStringLiteral(".md"),
                       const QString &text = QString()) const;

    /// Same uniquifying rule, for a folder.
    QString createFolder(const QString &parentRelative, const QString &baseName,
                         QString *error = nullptr) const;

    /// Renames a file or folder in place. `newBaseName` carries no path and, for
    /// a note, no extension. Refuses a name that already exists or that holds
    /// Obsidian's invalid characters (`# ^ [ ] | : \ /`).
    bool renamePath(const QString &absolutePath, const QString &newBaseName,
                    QString *newAbsolutePath, QString *error = nullptr) const;

    /// Moves to the vault's own `.trash/`, the way Obsidian's default does,
    /// keeping the relative folder structure and suffixing on collision.
    bool moveToTrash(const QString &absolutePath, QString *error = nullptr) const;

    // --- ignore filters ---------------------------------------------------

    /// True when `relativePath` matches a filter from `.obsidian/app.json`'s
    /// `userIgnoreFilters` (read when this Vault was constructed), or - for a
    /// vault with no filters configured at all - looks like `.obsidian/`,
    /// `.git/`, `.trash/` or a `build*/` directory. The first three are
    /// already caught by Indexer's own dot-prefix rule; this covers the rest.
    bool isIgnored(const QString &relativePath) const;

private:
    void loadIgnoreFilters();

    QString m_root;
    QStringList m_ignorePrefixes;
    QList<QRegularExpression> m_ignoreRegexes;
    bool m_filtersConfigured = false;
};

} // namespace noteAppC
