// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// A note as it sits in memory: text normalised to LF, plus the byte-level
/// details of the file it came from so that saving restores them exactly.
/// Invariant: the app never rewrites bytes it did not edit.
struct Note {
    QString text;      ///< LF line endings, no byte-order mark
    bool crlf = false; ///< the file on disk used CRLF
    bool bom = false;  ///< the file on disk started with a UTF-8 BOM
    /// The file on disk was not valid UTF-8: `text` holds U+FFFD where it was
    /// not, so writing it back would change bytes nobody edited.
    /// Vault::write() refuses a lossy note until the caller clears this.
    bool lossy = false;
    /// A file with both CRLF and LF lines: its text as read, endings and all,
    /// and `text` as it was then. encode() keeps those bytes for every line
    /// before and after what was edited.
    QString mixed, mixedText;
};

/// A vault is a directory of Markdown files, the same thing Obsidian means by
/// the word. Stateless beyond its root path: the index and the watcher live
/// elsewhere.
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
    /// note; a `.markdown` file is an attachment, as it is there.
    static bool isNote(const QString &path);
    /// A note encrypted on disk, `Name.mdlock` (libs/ext/crypt). Not a note
    /// to anything that reads files: only the window opens one, in memory.
    static bool isEncryptedNote(const QString &path);

    /// Path relative to the vault root, for display and for link resolution.
    QString relative(const QString &absolutePath) const;
    QString absolute(const QString &relativePath) const;
    bool contains(const QString &absolutePath) const;
    /// The absolute path of the note `relativePath` names, for a caller
    /// that is not trusted with the rest of the disk (a plugin): empty when
    /// it is not a note, or when "..", an absolute path or a symlink would
    /// take it out of the vault. It need not exist, but its folder must.
    QString confinedNotePath(const QString &relativePath) const;
    /// `relative` joined to `root` and cleaned, for a folder or file a vault's own
    /// settings name (daily notes, templates): empty when it is absolute or would
    /// climb out of the root with "..". Lexical only - links are not looked at.
    static QString pathInside(const QString &root, const QString &relative);
    /// Whether `relativePath`, from outside this device (a sync record), names
    /// somewhere inside the vault that a write may go to: relative, with no
    /// empty, "." / ".." or dot-prefixed segment, no backslash or NUL, and its
    /// nearest existing ancestor really inside the vault (a symlinked folder
    /// out of it is not). The file itself may not be a symlink unless
    /// `leafMayBeLink`. Nothing need exist.
    bool isConfinedPath(const QString &relativePath, bool leafMayBeLink = false) const;
    /// Whether `absolutePath`, links followed, really is inside the vault (its
    /// nearest existing ancestor is, when it does not exist yet). Unlike
    /// contains(), a symlink out of the vault does not pass.
    bool resolvesInside(const QString &absolutePath) const;
    /// The same against any `root`, for the settings that hold a root and not a
    /// Vault (daily notes, templates). False when `root` does not exist.
    static bool resolvesInside(const QString &root, const QString &absolutePath);
    /// Where a new attachment for `notePath` goes, by Obsidian's
    /// attachmentFolderPath: "" or "/" the vault root, "./" the note's own
    /// folder, "./sub" a folder beneath it, anything else a folder in the
    /// vault. Never outside the vault, a linked folder included (the root then).
    /// Not created here.
    QString attachmentFolderFor(const QString &setting, const QString &notePath) const;
    /// `folder`/`fileName`, or with " 1", " 2" ... inserted before the
    /// extension when that already exists - the same collision rule
    /// createNote() and the audio recorder use. Never touches the disk.
    static QString uniqueAttachmentPath(const QString &folder, const QString &fileName);

    /// Reads UTF-8, normalising CRLF to LF and stripping a BOM; invalid bytes
    /// become U+FFFD and set `Note::lossy`. On failure returns false and
    /// fills `error`.
    static bool read(const QString &path, Note &out, QString *error = nullptr);

    /// Writes through a temporary file and rename(2), restoring the line
    /// endings and BOM recorded in `note`. Fails for a `lossy` note.
    static bool write(const QString &path, const Note &note, QString *error = nullptr);
    /// The same atomic replace for bytes already encoded - an encrypted note.
    static bool writeBytes(const QString &path, const QByteArray &bytes, QString *error = nullptr);

    /// For a path from outside this device (a sync record): writes
    /// `relativePath` under the vault root walking it with openat(O_NOFOLLOW),
    /// so a symbolic link on any folder of the way makes it fail instead of
    /// leading out of the vault. Missing folders are created. A link as the
    /// file itself is replaced, never followed. Same temp file + fsync +
    /// rename as write(); the temp file is dot-prefixed and removed on failure.
    /// Linux only. `relativePath` should already pass isConfinedPath().
    bool writeBytesNoFollow(const QString &relativePath, const QByteArray &bytes, QString *error = nullptr) const;
    /// Copies `source` over `target` through a temporary file beside it and a rename, so a failed copy leaves
    /// the earlier `target` as it was (an export replacing a file).
    static bool copyReplacing(const QString &source, const QString &target, QString *error = nullptr);
    /// A folder held open by descriptor: every write, read and remove through it lands in the folder that was
    /// opened, whatever its name is made to point at afterwards. open() refuses a link as the folder itself.
    /// Move-only; the descriptor is closed with it.
    class HeldFolder {
    public:
        HeldFolder() = default;
        HeldFolder(HeldFolder &&other) noexcept;
        HeldFolder &operator=(HeldFolder &&other) noexcept;
        HeldFolder(const HeldFolder &) = delete;
        HeldFolder &operator=(const HeldFolder &) = delete;
        ~HeldFolder();
        /// Opens `path` with O_DIRECTORY|O_NOFOLLOW; not open (and `*error` set) when it cannot be.
        static HeldFolder open(const QString &path, QString *error = nullptr);
        bool isOpen() const { return m_fd >= 0; }
        int fd() const { return m_fd; }
        /// Where the held folder really is now (links resolved), asked of the descriptor and not of a name.
        QString realPath() const;

    private:
        explicit HeldFolder(int fd);
        int m_fd = -1;
    };
    /// Whether `relativePath` is a clean relative path: no empty, "." or ".." segment (so not absolute either),
    /// no backslash, no NUL. Lexical only.
    static bool isCleanRelativePath(const QString &relativePath);
    /// writeBytesNoFollow() against any `root` (the publish output folder), which need not be a vault. `sync` off
    /// skips the fsync, for a folder of many files that can be made again.
    static bool writeBytesNoFollowUnder(const QString &root, const QString &relativePath, const QByteArray &bytes,
                                        QString *error = nullptr, bool sync = true);
    static bool writeBytesNoFollowUnder(const HeldFolder &root, const QString &relativePath, const QByteArray &bytes,
                                        QString *error = nullptr, bool sync = true);
    /// Reads the regular file `relativePath` under `root` through a held folder descriptor: a link on any
    /// folder of the way, or as the file itself, makes it fail, and what is read is the file that was opened
    /// (fstat), not a path looked at before. False when it is missing, not a regular file or larger than
    /// `maxBytes` (when not negative); `*out` is then empty.
    static bool readFileNoFollowUnder(const QString &root, const QString &relativePath, QByteArray *out,
                                      qint64 maxBytes = -1);
    static bool readFileNoFollowUnder(const HeldFolder &root, const QString &relativePath, QByteArray *out,
                                      qint64 maxBytes = -1);
    /// Removes the file `relativePath` under `root` without following a link: one on any folder of the way means
    /// nothing is removed, and a link as the file itself is removed, not what it points at. False when nothing
    /// was removed; `*error` is then set only when that is a refusal and not a file that was not there.
    static bool removeNoFollowUnder(const QString &root, const QString &relativePath, QString *error = nullptr);
    static bool removeNoFollowUnder(const HeldFolder &root, const QString &relativePath, QString *error = nullptr);
    /// encode() then writeBytesNoFollow(); fails for a `lossy` note.
    bool writeNoteNoFollow(const QString &relativePath, const Note &note, QString *error = nullptr) const;

    /// Round-trip helper used by the tests: the exact bytes `note` would write.
    static QByteArray encode(const Note &note);
    /// `note` as its file reads back once written: a mixed-ending note's
    /// remembered bytes follow the save (the next one diffs against this).
    static Note asWritten(const Note &note) { return note.mixed.isEmpty() ? note : decode(encode(note)); }
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
    /// Obsidian's invalid characters (`# ^ [ ] | : \ /`). `warning`, when given, says what did not go with a
    /// rename that succeeded (an image's annotations file).
    bool renamePath(const QString &absolutePath, const QString &newBaseName,
                    QString *newAbsolutePath, QString *error = nullptr, QString *warning = nullptr) const;

    /// True when the two paths differ in letter case alone.
    static bool isCaseOnlyChange(const QString &oldPath, const QString &newPath);

    /// Moves a file or folder into `targetFolder` (absolute, inside the vault),
    /// keeping its name. Refuses a clash, a hidden target (`.trash`,
    /// `.obsidian`), and a folder moved into itself or its own subfolder.
    /// Moving into the folder it is already in succeeds and changes nothing.
    bool movePath(const QString &absolutePath, const QString &targetFolder,
                  QString *newAbsolutePath, QString *error = nullptr, QString *warning = nullptr) const;

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
