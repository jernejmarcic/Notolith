#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace noteAppC {

/// Obsidian's file recovery: earlier versions of a note, kept outside the
/// vault so that losing the vault does not lose them too.
///
/// A snapshot is taken when a note is saved, at most once per interval per
/// note (Obsidian's defaults: five minutes apart, kept for seven days). They
/// live under `$XDG_DATA_HOME/noteAppC/recovery`, one directory per note
/// keyed by its absolute path - which, as in Obsidian, means a vault moved
/// elsewhere leaves its old snapshots behind. Plain files: a `path` file
/// naming the note, and one `<milliseconds>.md` per version.
class Snapshots {
public:
    struct Version {
        QString notePath; ///< absolute
        qint64 time = 0;  ///< milliseconds since the epoch
        QString file;     ///< where the snapshot itself is
    };

    static QString directory();

    /// Keeps `text` as a version of `notePath`, unless the newest version is
    /// younger than `minimumIntervalMs` or already holds exactly this text.
    /// True when a version was written.
    static bool record(const QString &notePath, const QString &text, qint64 minimumIntervalMs,
                       qint64 nowMs = 0);
    /// Every note with at least one version, most recently kept first.
    static QStringList notes();
    /// Newest first.
    static QList<Version> versions(const QString &notePath);
    static QString read(const Version &version);
    /// Deletes versions older than `keepMs`; returns how many went.
    static int prune(qint64 keepMs, qint64 nowMs = 0);
    /// The same for any store of this shape: `<directory>/<key>/<ms><suffix>`
    /// (PdfRecovery keeps PDFs so).
    static int pruneStore(const QString &directory, const QString &suffix, qint64 keepMs, qint64 nowMs = 0);
};

} // namespace noteAppC
