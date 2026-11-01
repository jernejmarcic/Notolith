// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "EpubArchive.h"

#include <QStringList>
#include <QUrl>

#include <archive.h>
#include <archive_entry.h>

namespace noteAppC {

std::optional<QString> resolveEpubPath(const QString &fromEntry, const QString &hrefIn)
{
    QString href = hrefIn.trimmed();
    const qsizetype hash = href.indexOf(QLatin1Char('#'));
    if (hash >= 0)
        href = href.left(hash);
    if (href.isEmpty())
        return std::nullopt;

    // Percent-decoded before the scheme/escape checks: "%2e%2e/" is still
    // ".." wearing a disguise.
    href = QUrl::fromPercentEncoding(href.toUtf8());
    if (href.contains(QLatin1String("://")) || href.startsWith(QLatin1String("data:"))
        || href.startsWith(QLatin1Char('/')))
        return std::nullopt; // remote, embedded, or archive-absolute: refused, see the header comment

    const QString base = fromEntry.contains(QLatin1Char('/'))
                             ? fromEntry.left(fromEntry.lastIndexOf(QLatin1Char('/')))
                             : QString();
    const QString joined = base.isEmpty() ? href : base + QLatin1Char('/') + href;

    // Walked by hand rather than trusted to QDir::cleanPath: cleanPath on
    // "a/../../b" collapses to "../b" - it removes what it can pair off but
    // is happy to leave a path that still climbs past the root, which is
    // exactly the escape this function exists to catch.
    QStringList resolved;
    for (const QString &part : joined.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            if (resolved.isEmpty())
                return std::nullopt; // climbs past the archive root
            resolved.removeLast();
            continue;
        }
        resolved << part;
    }
    if (resolved.isEmpty())
        return std::nullopt;
    return resolved.join(QLatin1Char('/'));
}

namespace {

/// RAII for an archive_read stream opened on one file, so every early return
/// below still calls archive_read_free() - libarchive has no destructor of
/// its own to lean on since it is a C library.
class ReadStream {
public:
    explicit ReadStream(const QString &path)
        : m_archive(archive_read_new())
    {
        archive_read_support_format_zip(m_archive);
        const QByteArray utf8 = path.toUtf8();
        m_ok = archive_read_open_filename(m_archive, utf8.constData(), 64 * 1024) == ARCHIVE_OK;
    }
    ~ReadStream() { archive_read_free(m_archive); }
    ReadStream(const ReadStream &) = delete;
    ReadStream &operator=(const ReadStream &) = delete;

    bool isOpen() const { return m_ok; }
    archive *handle() const { return m_archive; }
    QString errorString() const { return QString::fromUtf8(archive_error_string(m_archive)); }

private:
    archive *m_archive;
    bool m_ok = false;
};

} // namespace

bool EpubArchive::open(const QString &path, QString *error)
{
    m_names.clear();
    m_nameSet.clear();
    m_path.clear();

    ReadStream stream(path);
    if (!stream.isOpen()) {
        if (error)
            *error = stream.errorString();
        return false;
    }

    archive_entry *entry = nullptr;
    while (archive_read_next_header(stream.handle(), &entry) == ARCHIVE_OK) {
        // Directory entries carry a trailing '/' in a zip and name nothing a
        // chapter or manifest ever points at; skip them so contains()/read()
        // only ever answer for real files.
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        if (!name.isEmpty() && !name.endsWith(QLatin1Char('/'))) {
            m_names << name;
            m_nameSet.insert(name);
        }
        archive_read_data_skip(stream.handle()); // seeks past the compressed bytes, does not inflate them
    }

    if (m_names.isEmpty()) {
        if (error)
            *error = QStringLiteral("not a readable EPUB (empty or not a zip)");
        return false;
    }
    m_path = path;
    return true;
}

QByteArray EpubArchive::read(const QString &entryName, QString *error) const
{
    if (!m_nameSet.contains(entryName)) {
        if (error)
            *error = QStringLiteral("%1 is not in %2").arg(entryName, m_path);
        return {};
    }

    ReadStream stream(m_path);
    if (!stream.isOpen()) {
        if (error)
            *error = stream.errorString();
        return {};
    }

    archive_entry *entry = nullptr;
    while (archive_read_next_header(stream.handle(), &entry) == ARCHIVE_OK) {
        if (QString::fromUtf8(archive_entry_pathname(entry)) != entryName) {
            archive_read_data_skip(stream.handle());
            continue;
        }
        // Inflated in pieces: the size in the header is never what decides the allocation.
        QByteArray bytes;
        constexpr qint64 chunk = 64 * 1024;
        for (;;) {
            const qint64 room = qMin(chunk, m_entryLimit + 1 - bytes.size()); // one past the limit shows an overrun
            const qsizetype at = bytes.size();
            bytes.resize(at + room);
            const auto n = archive_read_data(stream.handle(), bytes.data() + at, size_t(room));
            bytes.resize(at + qMax<qint64>(n, 0));
            if (n <= 0)
                break; // the end, or a damaged entry: what was inflated is kept, as before
            if (bytes.size() > m_entryLimit) {
                if (error)
                    *error = QStringLiteral("%1 in %2 is larger than %3 MiB inflated, so it was not read")
                                 .arg(entryName, m_path).arg(m_entryLimit >> 20);
                return {};
            }
        }
        QMutexLocker lock(&m_budget->mutex);
        m_budget->sizes.insert(entryName, bytes.size());
        qint64 total = 0;
        for (const qint64 each : std::as_const(m_budget->sizes))
            total += each;
        if (total > m_bookLimit) {
            m_budget->sizes.remove(entryName);
            if (error)
                *error = QStringLiteral("%1 is larger than %2 MiB inflated, so %3 was not read")
                             .arg(m_path).arg(m_bookLimit >> 20).arg(entryName);
            return {};
        }
        return bytes;
    }
    if (error)
        *error = QStringLiteral("%1 vanished from %2 between open() and read()").arg(entryName, m_path);
    return {};
}

} // namespace noteAppC
