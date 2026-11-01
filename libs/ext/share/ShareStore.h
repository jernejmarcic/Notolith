#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QString>

namespace noteAppC {

/// One shared note as this device remembers it.
struct ShareRecord {
    QByteArray shareID;   ///< 16 random bytes
    QByteArray owner;     ///< the owner's device key, 32
    QByteArray key;       ///< the share key at `epoch`, 32
    QByteArray members;   ///< the owner-signed MemberList message
    quint32 epoch = 1;
    int backend = 0;      ///< 1 Yrs, 2 Native (share.proto)
    quint64 clientID = 0; ///< this replica's CRDT id, below 2^53
    QString relativePath; ///< vault-relative path of the note ("" until the window has put it somewhere)
    QString title;
    bool owned = false;   ///< this device is the owner
    QStringList hosts;    ///< a joiner's "host:port"s of the owner, from the invite: dialled again on every start
};

/// The shares of one vault on this device, under one directory:
/// `<shareID hex>/share.json` (the record), `state.bin` (the CRDT's encoded
/// state) and `base.md` (the text the file on disk and the CRDT last agreed
/// on). Files are written whole through QSaveFile with mode 0600. The key sits
/// beside a plaintext state and a plaintext note: the same threat model as the
/// note itself; this protects the wire, not the disk.
class ShareStore {
public:
    explicit ShareStore(QString directory);

    /// `$XDG_DATA_HOME/notolith/share/<16 hex of sha256 of the vault path>`,
    /// the same digest shape as sync's and history's per-vault directories.
    static QString directoryFor(const QString &vaultRoot);

    /// Every readable, well-formed record (a broken one is skipped).
    QList<ShareRecord> all() const;
    bool save(const ShareRecord &record, const QByteArray &state, const QString &base);
    QByteArray state(const QByteArray &shareID) const;
    QString base(const QByteArray &shareID) const;
    void remove(const QByteArray &shareID);

    const QString &directory() const { return m_directory; }

private:
    QString shareDirectory(const QByteArray &shareID) const;
    QString m_directory;
};

} // namespace noteAppC
