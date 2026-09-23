#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QString>

namespace noteAppC {

/// Talks to tools/sync_server over HTTP(S) - the wire side of
/// docs/SYNC.md's journal protocol. Every call blocks its own worker thread
/// on a local QEventLoop until the reply lands: simpler than a callback
/// chain for what is, in practice, one batched request every so often, not
/// a per-keystroke path, and SyncClient is only ever used from
/// SyncEngine's worker thread, never the UI thread. TLS is whatever
/// `serverURL`'s scheme says (https when the server was given a
/// certificate, see tools/sync_server); tests run plain HTTP on loopback.
class SyncClient : public QObject {
    Q_OBJECT

public:
    explicit SyncClient(const QString &serverURL, QObject *parent = nullptr);

    void setToken(const QByteArray &token) { m_token = token; }
    QByteArray token() const { return m_token; }

    struct Salt {
        bool ok = false;
        QByteArray salt;
    };
    /// No auth needed - a salt is not secret (docs/SYNC.md).
    Salt fetchSalt(const QString &email);

    struct Auth {
        bool ok = false;
        QString error;
        QByteArray accountID;
        QByteArray token;
    };
    Auth registerAccount(const QString &email, const QByteArray &salt, const QByteArray &loginKey);
    Auth signIn(const QString &email, const QByteArray &loginKey);
    bool changePassword(const QByteArray &newSalt, const QByteArray &newLoginKey,
                        const QList<QPair<QByteArray, QByteArray>> &rewrappedItemsKeys);

    struct Created {
        bool ok = false;
        QByteArray id;
    };
    Created createVault();
    QList<QByteArray> listVaults();

    /// Wire shape of one journal row - see docs/SYNC.md "Record and blob
    /// formats". `seq` is set by the server; ignored on push.
    struct JournalRecord {
        qint64 seq = 0;
        QByteArray entityID;
        QByteArray recordID;
        QByteArray deviceID;
        QByteArray itemsKeyID;
        QByteArray wrappedNoteKey;
        QByteArray payload;
    };
    struct PushResult {
        bool ok = false;
        QList<qint64> seqs; ///< assigned seq per record, same order as pushed
    };
    PushResult pushJournal(const QByteArray &vaultID, const QList<JournalRecord> &records);
    struct PullResult {
        bool ok = false;
        QList<JournalRecord> records;
    };
    PullResult pullJournal(const QByteArray &vaultID, qint64 since, int limit = 500);

    bool putBlob(const QByteArray &vaultID, const QByteArray &address, const QByteArray &bytes);
    struct BlobResult {
        bool ok = false;
        QByteArray bytes;
    };
    BlobResult getBlob(const QByteArray &vaultID, const QByteArray &address);

    bool registerDevice(const QByteArray &vaultID, const QByteArray &deviceID, const QByteArray &publicKey);
    struct DeviceInfo {
        QByteArray deviceID;
        QByteArray publicKey;
    };
    QList<DeviceInfo> listDevices(const QByteArray &vaultID);

    bool putLockbox(const QByteArray &vaultID, const QByteArray &forDevice, const QByteArray &itemsKeyID,
                    const QByteArray &sealed);
    struct Lockbox {
        QByteArray itemsKeyID;
        QByteArray sealed;
    };
    QList<Lockbox> listLockboxes(const QByteArray &vaultID, const QByteArray &forDevice);

    /// itemsKeys wrapped with wrapKey (docs/SYNC.md path 1) - gated
    /// server-side to a device already marked approved for this vault.
    bool putItemsKey(const QByteArray &vaultID, const QByteArray &itemsKeyID, const QByteArray &wrapped);
    struct WrappedItemsKey {
        QByteArray itemsKeyID;
        QByteArray wrapped;
    };
    /// `asDevice` identifies the caller for the approval check; an
    /// unapproved device gets an empty list back, not an error, so a
    /// device waiting on a lockbox can poll this in the same cycle without
    /// treating "not yet approved" as a failure.
    QList<WrappedItemsKey> fetchItemsKeys(const QByteArray &vaultID, const QByteArray &asDevice);

private:
    struct Reply {
        int status = 0;
        QJsonObject json;
        QByteArray body; ///< raw bytes, for the blob endpoints
    };
    Reply call(const QByteArray &verb, const QString &path, const QJsonObject &body = {});
    Reply callRaw(const QByteArray &verb, const QString &path, const QByteArray &body,
                 const char *contentType = "application/octet-stream");

    QNetworkAccessManager m_net;
    QString m_baseURL;
    QByteArray m_token;
};

} // namespace noteAppC
