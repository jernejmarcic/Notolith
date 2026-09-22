#pragma once

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

    /// Reads UTF-8, normalising CRLF to LF and stripping a BOM. On failure
    /// returns false and fills `error`.
    static bool read(const QString &path, Note &out, QString *error = nullptr);

    /// Writes through a temporary file and rename(2), restoring the line
    /// endings and BOM recorded in `note`.
    static bool write(const QString &path, const Note &note, QString *error = nullptr);

    /// Round-trip helper used by the tests: the exact bytes `note` would write.
    static QByteArray encode(const Note &note);
    static Note decode(const QByteArray &bytes);

private:
    QString m_root;
};

} // namespace noteAppC
