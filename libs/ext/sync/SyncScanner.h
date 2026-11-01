// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace noteAppC {

class Vault;
class SyncJournal;

/// Finds what changed in the vault since the journal's last scan
/// (docs/SYNC.md). mtime and size are checked first - the same cheap-skip
/// technique Index::isCurrent uses, kept as a separate table here rather
/// than sharing Index's (coupling this engine's lifecycle to the UI-owned
/// Index would mean depending on libs/index or libs/ui) - so a no-change pass over a large vault costs
/// one stat() per file and no content hashing at all.
class SyncScanner {
public:
    struct Change {
        enum Kind { Created, Modified, Renamed, Deleted };
        Kind kind = Created;
        QByteArray entityID; ///< empty for Created - the caller assigns one
        QString path;        ///< current path (Deleted: the last known path)
        QString oldPath;     ///< Renamed only
        QString entityType;  ///< "note" | "attachment"
        qint64 mtime = 0;
        qint64 size = 0;
        QByteArray hash; ///< content fingerprint, empty for Deleted
    };

    /// `contentKey` fingerprints file content for change detection and
    /// rename matching (SyncCrypto::contentAddress, unpadded - this never
    /// leaves the device, so padding buys nothing here).
    /// The one definition of "this file's content" for change detection: a
    /// note is its decoded text (so line endings and a BOM do not count, and
    /// the hash matches the synced one), anything else its bytes. Empty when
    /// the file cannot be read.
    static QByteArray contentHash(const QString &absolutePath, const QByteArray &contentKey);

    static QList<Change> scan(const Vault &vault, const SyncJournal &journal, const QByteArray &contentKey);
};

} // namespace noteAppC
