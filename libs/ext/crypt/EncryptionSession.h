// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include "NoteCrypt.h"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

namespace noteAppC::crypt {

/// The unlocked state of one vault's encrypted notes for this run of the
/// app: the password (to derive a key for any file's salt) and the keys
/// derived so far, all in sodium-guarded memory, gone on lock().
///
/// Asking for the password is the window's (a dialog); this class decides
/// when to ask and checks the answer against a real file.
class EncryptionSession : public QObject {
    Q_OBJECT

public:
    /// `creating` true: no encrypted note exists to check against, so this
    /// sets the password (the dialog asks for it twice). Empty cancels.
    using PasswordPrompt = std::function<QString(bool creating, bool retry)>;

    explicit EncryptionSession(QObject *parent = nullptr);
    ~EncryptionSession() override;

    void setPrompt(PasswordPrompt prompt) { m_prompt = std::move(prompt); }
    /// A new password asked through the same prompt; empty when cancelled.
    QString askNewPassword() const { return m_prompt ? m_prompt(true, false) : QString(); }
    /// Params for keys this session creates. Tests lower them.
    void setParams(Params params) { m_params = params; }
    /// Minutes without activity before it locks by itself; 0 never.
    void setAutoLockMinutes(int minutes) { setAutoLockMs(qMax(0, minutes) * 60'000); }
    void setAutoLockMs(int milliseconds);
    /// Something the person did: keeps an unlocked session from timing out.
    void noteActivity();

    bool isUnlocked() const { return m_password != nullptr; }

    /// Unlocks with a password checked against `sample`, an encrypted file
    /// of this vault - or, with `sample` empty, sets a new one. True when
    /// unlocked; false when cancelled or three tries were wrong.
    bool unlock(const QByteArray &sample, QString *error = nullptr);
    /// Plaintext of an encrypted file, unlocking first if need be.
    std::optional<QByteArray> open(const QByteArray &file, QString *error = nullptr);
    /// An encrypted file for `plaintext`, under this session's current key
    /// (`sample` checks the password if it must unlock first; empty sets a
    /// new one). Empty on cancel.
    QByteArray seal(const QByteArray &plaintext, const QByteArray &sample, QString *error = nullptr);
    /// A new password and salt for everything sealed from here on; the
    /// caller re-seals each file with it. Requires an unlocked session.
    bool rekey(const QString &password);

    /// Forgets the password and every key.
    void lock();

signals:
    /// About to lock, the keys still here: the last chance to save what is
    /// open.
    void aboutToLock();
    /// Locked, by lock() or by the idle timer: every decrypted note must
    /// leave memory now.
    void locked();

private:
    const Key *keyFor(const QByteArray &salt, Params params);
    void keep(const QString &password);

    PasswordPrompt m_prompt;
    Params m_params;
    char *m_password = nullptr;     ///< UTF-8, sodium_malloc'd
    qsizetype m_passwordSize = 0;
    std::shared_ptr<Key> m_current; ///< what new files are sealed with, also in m_keys
    QHash<QByteArray, std::shared_ptr<Key>> m_keys; ///< by salt
    QTimer m_idle;
};

} // namespace noteAppC::crypt
