// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <functional>

#include <QByteArray>
#include <QString>

namespace noteAppC::secrets {

/// Where secrets are kept: the OS keyring unless a store is set here. A test
/// sets an in-memory one, so a run never writes to the user's real keyring,
/// nor waits out a secret-service backend that does not answer. Process-wide;
/// set it before anything reads. `remove` may stay empty. The service name
/// picks the keyring entry group (sync and sharing keep theirs apart); an
/// in-memory store sees only the key, so tests must keep keys distinct.
struct Store {
    std::function<bool(const QString &key, QByteArray *value)> read;
    std::function<bool(const QString &key, const QByteArray &value)> write;
    std::function<bool(const QString &key)> remove;
};
void setStore(Store store);

/// False when the entry is absent, empty (how a watchdog timeout looks; no
/// secret is ever empty) or the keyring refuses. Each call runs a keyring job
/// to completion on the calling thread's own event loop, with a 15 s watchdog.
bool read(const QString &service, const QString &key, QByteArray *value);
bool write(const QString &service, const QString &key, const QByteArray &value);
bool remove(const QString &service, const QString &key);

} // namespace noteAppC::secrets
