#pragma once

// The two things a remote image leaves on disk, both outside the vault:
// the origins the person said "Always load from" (XDG state) and the
// downloaded originals (XDG cache).

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace noteAppC::remoteimage {

/// Origins (`https://host[:port]`) whose images load without a click. One per line in a file in the
/// application's state directory, mode 0600. Never read from or written to a vault.
class ImageAllowlist {
public:
    explicit ImageAllowlist(QString path);
    static QString defaultPath(); ///< `$XDG_STATE_HOME/notolith/remote-image-hosts`

    bool contains(const QString &origin) const { return m_origins.contains(origin); }
    QStringList origins() const;
    bool add(const QString &origin);
    bool clear();
    QString path() const { return m_path; }

private:
    bool save() const;
    QString m_path;
    QSet<QString> m_origins;
};

/// The original bytes of every image fetched, named by the SHA-256 of its URL, in a 0700 directory, files
/// 0600. Bytes in it are as untrusted as bytes off the wire: they still go through the decoder helper.
/// `capBytes` is the total; the oldest files go first.
class ImageCache {
public:
    explicit ImageCache(QString dir, qint64 capBytes = 100 * 1024 * 1024);
    static QString defaultDir(); ///< `$XDG_CACHE_HOME/notolith/remote-images`

    QByteArray read(const QUrl &url) const; ///< empty when there is none
    bool write(const QUrl &url, const QByteArray &bytes);
    QString pathFor(const QUrl &url) const;
    qint64 totalBytes() const;
    QString dir() const { return m_dir; }

private:
    void evict(const QString &keep) const;
    QString m_dir;
    qint64 m_cap;
};

} // namespace noteAppC::remoteimage
