// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <optional>

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

namespace noteAppC {

/// Everything tools/sync_server persists: accounts, sessions, per-vault
/// journal and blob store, devices, itemsKeys and lockboxes - see
/// docs/SYNC.md "What the server stores and learns". SQLite plus files
/// under `--data`; the server never has a key to decrypt any of it.
///
/// One instance per process, used from the HTTP server's own thread only
/// (Qt6HttpServer dispatches routes on the thread it was created on - this
/// tool runs no worker threads).
class ServerStore {
public:
    explicit ServerStore(const QString &dataDir);
    ~ServerStore();
    ServerStore(const ServerStore &) = delete;
    ServerStore &operator=(const ServerStore &) = delete;
    bool isOpen() const;
    QString error() const { return m_error; }

    // --- accounts ----------------------------------------------------

    struct Account {
        QByteArray accountID;
        QString email;
        QByteArray salt;
        QByteArray loginHash; ///< crypto_pwhash_str(loginKey)
    };

    /// A salt for an email that may not exist - deterministic
    /// (HMAC-shaped, via SyncCrypto's own keyed hash) so the response
    /// shape does not reveal whether the account is real.
    QByteArray saltFor(const QString &email) const;
    std::optional<Account> account(const QString &email) const;
    std::optional<Account> accountByID(const QByteArray &accountID) const;
    /// False (with `error` set to "exists") if the email is already registered.
    bool createAccount(const QString &email, const QByteArray &salt, const QByteArray &loginKey,
                       QByteArray *accountID, QString *error);
    bool updatePassword(const QByteArray &accountID, const QByteArray &newSalt, const QByteArray &newLoginKey);

    /// Simple fixed-window limiter (docs/SYNC.md): more than 10 attempts
    /// against one email in 60 s is refused before the password hash is
    /// even checked. In-memory only - a restart clears it, which is fine,
    /// the window is a minute wide.
    bool loginRateLimited(const QString &email);

    // --- sessions ------------------------------------------------------

    /// Returns the raw bearer token; only its hash is stored.
    QByteArray createSession(const QByteArray &accountID);
    std::optional<QByteArray> accountForToken(const QByteArray &token) const;

    // --- vaults ----------------------------------------------------------

    QByteArray createVault(const QByteArray &accountID);
    QList<QByteArray> vaultsFor(const QByteArray &accountID) const;
    bool vaultBelongsTo(const QByteArray &vaultID, const QByteArray &accountID) const;

    // --- devices -------------------------------------------------------

    struct Device {
        QByteArray deviceID;
        QByteArray publicKey;
        bool approved = false;
        bool wantsLockbox = false; ///< enrolling without the password, so waiting on a lockbox
    };
    /// The first device ever registered for a vault is auto-approved
    /// (docs/SYNC.md "Device enrolment with lockboxes"); every one after
    /// starts pending. Idempotent re-registration keeps the existing row.
    void registerDevice(const QByteArray &vaultID, const QByteArray &deviceID, const QByteArray &publicKey,
                        bool wantsLockbox = false);
    QList<Device> devices(const QByteArray &vaultID) const;
    bool isApproved(const QByteArray &vaultID, const QByteArray &deviceID) const;

    // --- itemsKeys (wrapped with wrapKey - docs/SYNC.md path 1) --------

    struct WrappedItemsKey {
        QByteArray itemsKeyID;
        QByteArray wrapped;
    };
    bool putItemsKey(const QByteArray &vaultID, const QByteArray &itemsKeyID, const QByteArray &wrapped);
    QList<WrappedItemsKey> itemsKeys(const QByteArray &vaultID) const;
    /// Password change: replaces every row for `vaultID` atomically.
    bool replaceItemsKeys(const QByteArray &vaultID, const QList<WrappedItemsKey> &keys);

    // --- lockboxes (sealed with a device's X25519 public key) ----------

    struct Lockbox {
        QByteArray itemsKeyID;
        QByteArray sealed;
    };
    /// Also approves `forDevice` for `vaultID` - see registerDevice's
    /// comment and docs/SYNC.md: an existing device sealing something for
    /// a pending one is what "vouched for" means here.
    void putLockbox(const QByteArray &vaultID, const QByteArray &forDevice, const QByteArray &itemsKeyID,
                    const QByteArray &sealed);
    QList<Lockbox> lockboxesFor(const QByteArray &vaultID, const QByteArray &forDevice) const;

    // --- journal (docs/SYNC.md "The journal protocol") ------------------

    struct JournalRow {
        qint64 seq = 0;
        QByteArray entityID;
        QByteArray recordID;
        QByteArray deviceID;
        QByteArray itemsKeyID;
        QByteArray wrappedNoteKey;
        QByteArray payload;
    };
    /// One push, one transaction: each record either gets a fresh seq or,
    /// if (deviceID, recordID) already exists, returns the seq it was
    /// already assigned (idempotent retry).
    QList<qint64> appendJournal(const QByteArray &vaultID, const QList<JournalRow> &records);
    QList<JournalRow> journalSince(const QByteArray &vaultID, qint64 since, int limit) const;

    // --- blobs -----------------------------------------------------------

    QString blobPath(const QByteArray &vaultID, const QByteArray &address) const;
    bool putBlob(const QByteArray &vaultID, const QByteArray &address, const QByteArray &bytes);
    std::optional<QByteArray> getBlob(const QByteArray &vaultID, const QByteArray &address) const;

private:
    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_dataDir;
    QString m_error;
    QHash<QString, QList<qint64>> m_loginAttempts; ///< email -> attempt timestamps (ms), in-memory
};

} // namespace noteAppC
