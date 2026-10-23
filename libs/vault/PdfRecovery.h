#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// The PDF equivalent of `Snapshots` (libs/vault/Snapshots.h): a copy of a
/// PDF's bytes kept outside the vault every time PdfDocument::save() is
/// about to overwrite it, so the user's file is never the only copy of
/// itself. Not the same store as `Snapshots` - that one takes `QString`
/// note text, and a PDF is binary - but the same directory shape and the
/// same recovery page would read it: `$XDG_DATA_HOME/noteAppC/pdf-recovery/
/// <sha1 of the absolute path>/<milliseconds>.pdf`, plus a `path` file
/// naming the original.
class PdfRecovery {
public:
    struct Version {
        QString pdfPath; ///< absolute
        qint64 time = 0; ///< milliseconds since the epoch
        QString file; ///< where the snapshot itself is
    };

    static QString directory();

    /// Reads `pdfPath`'s current bytes off disk and keeps them as a version,
    /// unless the newest version already holds exactly those bytes, or is
    /// younger than `minimumIntervalMs`. False when there was nothing to
    /// read (a brand new file has no "previous" to protect) or nothing
    /// written changed. `nowMs` is a test seam, as in Snapshots.
    static bool record(const QString &pdfPath, qint64 minimumIntervalMs = 0, qint64 nowMs = 0);
    /// Newest first.
    static QList<Version> versions(const QString &pdfPath);
    static QByteArray read(const Version &version);
    /// Deletes versions older than `keepMs`, as Snapshots::prune does.
    static int prune(qint64 keepMs, qint64 nowMs = 0);
};

} // namespace noteAppC
