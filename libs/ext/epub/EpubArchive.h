// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>

#include <QHash>
#include <QMutex>

#include <memory>
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

    /// What one entry may inflate to, and what all the entries of one book read so far may add up to
    /// (each entry counted once, however often it is read). A zip says how big an entry is, and may lie:
    /// read() inflates in pieces and stops at the limit, whatever the header says.
    static constexpr qint64 kMaxEntryBytes = qint64(64) << 20;
    static constexpr qint64 kMaxBookBytes = qint64(512) << 20;
    /// What the header walk in open() accepts: entries and the bytes of their names. A zip
    /// of millions of tiny entries is refused before it fills the index.
    static constexpr qint64 kMaxEntries = 100'000;
    static constexpr qint64 kMaxNameBytes = qint64(16) << 20;
    /// Lower limits, for a test.
    void setLimits(qint64 entryBytes, qint64 bookBytes) { m_entryLimit = entryBytes; m_bookLimit = bookBytes; }

private:
    struct Budget {
        QMutex mutex;
        QHash<QString, qint64> sizes; ///< entry name -> bytes it inflated to
    };
    std::shared_ptr<Budget> m_budget = std::make_shared<Budget>(); ///< shared by copies: one book
    qint64 m_entryLimit = kMaxEntryBytes;
    qint64 m_bookLimit = kMaxBookBytes;
    QString m_path;
    QStringList m_names;   // archive order
    QSet<QString> m_nameSet;
};

} // namespace noteAppC
