#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace noteAppC {

/// An href found inside a book resolved to an archive entry name, or nothing
/// when it is remote, embedded, archive-absolute or climbs past the root
/// (EpubDocument::resolveArchivePath forwards here).
std::optional<QString> resolveEpubPath(const QString &fromEntry, const QString &href);

/// Read-only access to an EPUB's zip container through libarchive.
///
/// open() makes one sequential pass that reads every entry's *header* only -
/// archive_read_data_skip() seeks past a zip member's compressed bytes rather
/// than inflating them, so indexing a book costs a directory walk, not a
/// decompression of it. Nothing is decompressed until read() asks for one
/// entry by name, which reopens the stream and skips forward to it: exactly
/// the entries a reader actually visits (the container, the OPF, the
/// chapters near the reading position - see EpubDocument) are ever inflated,
/// which is what keeps opening a book independent of the book's total size.
///
/// A book's bytes are never written back (Vault's rule, same reason): this
/// class only ever calls archive_read_*, never archive_write_*.
class EpubArchive {
public:
    EpubArchive() = default;

    /// Indexes every entry name. False with `error` set when `path` cannot be
    /// opened as a zip at all (not an EPUB, or corrupt).
    bool open(const QString &path, QString *error = nullptr);
    bool isValid() const { return !m_names.isEmpty(); }

    QString path() const { return m_path; }
    const QStringList &entryNames() const { return m_names; }
    bool contains(const QString &entryName) const { return m_nameSet.contains(entryName); }

    /// The bytes of one entry. Empty with `error` set when `entryName` is not
    /// in the archive, or it could not be reopened (the file went away, or
    /// permissions changed, between open() and this call).
    QByteArray read(const QString &entryName, QString *error = nullptr) const;

private:
    QString m_path;
    QStringList m_names;   // archive order
    QSet<QString> m_nameSet;
};

} // namespace noteAppC
