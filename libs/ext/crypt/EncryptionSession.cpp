// SPDX-License-Identifier: GPL-3.0-or-later
#include "EncryptionSession.h"

#include <sodium.h>

namespace noteAppC::crypt {

EncryptionSession::EncryptionSession(QObject *parent)
    : QObject(parent)
{
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, &EncryptionSession::lock);
    setAutoLockMinutes(15);
}

EncryptionSession::~EncryptionSession()
{
    // Destroyed with the window: forget everything, but nobody is left to
    // tell.
    blockSignals(true);
    lock();
}

void EncryptionSession::setAutoLockMs(int milliseconds)
{
    m_idle.setInterval(qMax(0, milliseconds));
    if (milliseconds <= 0)
        m_idle.stop();
    else if (isUnlocked())
        m_idle.start();
}

void EncryptionSession::noteActivity()
{
    if (isUnlocked() && m_idle.interval() > 0)
        m_idle.start();
}

void EncryptionSession::keep(const QString &password)
{
    QByteArray utf8 = password.toUtf8();
    m_password = static_cast<char *>(sodium_malloc(size_t(qMax<qsizetype>(1, utf8.size()))));
    if (m_password) {
        memcpy(m_password, utf8.constData(), size_t(utf8.size()));
        m_passwordSize = utf8.size();
    }
    sodium_memzero(utf8.data(), size_t(utf8.size()));
    setDumpable(false);
    if (m_idle.interval() > 0)
        m_idle.start();
}

const Key *EncryptionSession::keyFor(const QByteArray &salt, Params params)
{
    if (const auto found = m_keys.constFind(salt); found != m_keys.constEnd())
        return found->get();
    if (!m_password)
        return nullptr;
    const QString password = QString::fromUtf8(m_password, m_passwordSize);
    auto key = std::make_shared<Key>(derive(password, salt, params));
    if (!key->isValid())
        return nullptr;
    m_keys.insert(salt, key);
    return key.get();
}

bool EncryptionSession::unlock(const QByteArray &sample, QString *error)
{
    if (isUnlocked())
        return true;
    if (!available()) {
        if (error)
            *error = tr("Encryption is not available: libsodium did not start");
        return false;
    }
    const std::optional<FileHeader> head = header(sample);
    if (!sample.isEmpty() && !head) {
        if (error)
            *error = tr("Not an encrypted note, or one from a newer version");
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const QString password = m_prompt ? m_prompt(sample.isEmpty(), attempt > 0) : QString();
        if (password.isEmpty())
            return false; // cancelled: nothing to report
        if (sample.isEmpty()) {
            // A new password: the first salt of this vault.
            Key key = derive(password, newSalt(), m_params);
            if (!key.isValid())
                break;
            keep(password);
            m_current = std::make_shared<Key>(std::move(key));
            m_keys.insert(m_current->salt(), m_current);
            return true;
        }
        Key key = derive(password, head->salt, head->params);
        if (key.isValid() && crypt::open(key, sample)) {
            keep(password);
            m_current = std::make_shared<Key>(std::move(key));
            m_keys.insert(m_current->salt(), m_current);
            return true;
        }
    }
    if (error)
        *error = tr("Wrong password");
    return false;
}

std::optional<QByteArray> EncryptionSession::open(const QByteArray &file, QString *error)
{
    const std::optional<FileHeader> head = header(file);
    if (!head) {
        if (error)
            *error = tr("Not an encrypted note, or one from a newer version");
        return std::nullopt;
    }
    if (!isUnlocked() && !unlock(file, error))
        return std::nullopt;
    const Key *key = keyFor(head->salt, head->params);
    std::optional<QByteArray> plain = key ? crypt::open(*key, file) : std::nullopt;
    if (!plain && error)
        *error = tr("This note does not open with the password this session was unlocked with, or it was changed on disk");
    return plain;
}

QByteArray EncryptionSession::seal(const QByteArray &plaintext, const QByteArray &sample, QString *error)
{
    if (!isUnlocked() && !unlock(sample, error))
        return {};
    if (!m_current)
        return {};
    return crypt::seal(*m_current, plaintext);
}

bool EncryptionSession::rekey(const QString &password)
{
    if (!isUnlocked() || password.isEmpty())
        return false;
    Key key = derive(password, newSalt(), m_params);
    if (!key.isValid())
        return false;
    sodium_free(m_password);
    m_password = nullptr;
    keep(password);
    m_current = std::make_shared<Key>(std::move(key));
    m_keys.insert(m_current->salt(), m_current);
    return true;
}

void EncryptionSession::lock()
{
    m_idle.stop();
    const bool was = isUnlocked();
    if (was)
        emit aboutToLock();
    if (m_password)
        sodium_free(m_password); // zeroes it first
    m_password = nullptr;
    m_passwordSize = 0;
    m_current.reset();
    m_keys.clear();
    setDumpable(true);
    if (was)
        emit locked();
}

} // namespace noteAppC::crypt
