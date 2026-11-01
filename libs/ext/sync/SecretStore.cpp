// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "SecretStore.h"

#include <QEventLoop>
#include <QTimer>

#include <qt6keychain/keychain.h>

namespace noteAppC::secrets {

namespace {

Store &store()
{
    static Store instance;
    return instance;
}

/// Runs a QKeychain job to completion on the calling thread's own event
/// loop (SyncEngine's worker thread - see the class comment in
/// SyncEngine.h; ShareEngine's UI thread) rather than wiring up
/// finished()/error() by hand for one call site each. A watchdog timeout,
/// matching SyncClient::callRaw()'s own 15 s one: an unresponsive
/// secret-service D-Bus backend must not hang the caller forever (and, in
/// sync, since this runs inside runCycle()'s m_inCycle window, must not hang
/// the destructor's teardown either).
void runKeychainJob(QKeychain::Job *job)
{
    QEventLoop loop;
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    // Limit: on a genuine timeout the stack-allocated job in
    // the caller is destroyed with its D-Bus call still
    // in flight - a late reply is simply dropped (QKeychain does not
    // deliver into a destroyed QObject). Upgrade to a heap-allocated,
    // deleteLater()-based job if a backend that hangs this long in
    // practice turns up.
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
}

} // namespace

void setStore(Store value)
{
    store() = std::move(value);
}

bool read(const QString &service, const QString &key, QByteArray *value)
{
    const Store &s = store();
    if (s.read && s.write)
        return s.read(key, value);
    QKeychain::ReadPasswordJob job(service);
    job.setAutoDelete(false);
    job.setKey(key);
    runKeychainJob(&job);
    // An empty value is how a watchdog timeout looks (the job never
    // finished, error() is still NoError); no secret here is ever empty.
    if (job.error() != QKeychain::NoError || job.binaryData().isEmpty())
        return false;
    *value = job.binaryData();
    return true;
}

bool write(const QString &service, const QString &key, const QByteArray &value)
{
    const Store &s = store();
    if (s.read && s.write)
        return s.write(key, value);
    QKeychain::WritePasswordJob job(service);
    job.setAutoDelete(false);
    job.setKey(key);
    job.setBinaryData(value);
    runKeychainJob(&job);
    return job.error() == QKeychain::NoError;
}

bool remove(const QString &service, const QString &key)
{
    const Store &s = store();
    if (s.read && s.write)
        return s.remove && s.remove(key);
    QKeychain::DeletePasswordJob job(service);
    job.setAutoDelete(false);
    job.setKey(key);
    runKeychainJob(&job);
    return job.error() == QKeychain::NoError;
}

} // namespace noteAppC::secrets
