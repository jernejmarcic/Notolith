#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <memory>

namespace noteAppC {

/// Every save of a note as a git commit - the Edit History plugin's idea,
/// with git's object store doing the diffing and packing.
///
/// The repository is bare and lives outside the vault, under
/// `$XDG_DATA_HOME/noteAppC/history/<vault digest>.git`, so the vault never
/// gains a dotfile and a vault that is itself a git repository is not
/// touched. Each note has a ref of its own (`refs/notes/<digest of its
/// vault-relative path>`) whose commits hold only that note: a save writes
/// one blob, a tree per folder level, one commit and moves one ref, however
/// big the vault is. A save whose text equals the last one records nothing.
///
/// The repository is created by the first record(), not when the history is
/// constructed: a vault that is only opened leaves nothing on disk.
///
/// Not thread-safe: the window runs every call on one worker thread.
class NoteHistory {
public:
    struct Version {
        QString id; ///< the commit, hex
        QDateTime time;
    };

    explicit NoteHistory(const QString &vaultRoot);
    ~NoteHistory();
    NoteHistory(const NoteHistory &) = delete;
    NoteHistory &operator=(const NoteHistory &) = delete;

    /// Where the repository for `vaultRoot` lives.
    static QString repositoryPathFor(const QString &vaultRoot);

    /// False only when an existing repository could not be opened.
    bool isOpen() const;
    QString error() const { return m_error; }

    /// Commits `text` as `relativePath`'s newest version, unless it is what
    /// the newest version already holds. False on failure, with error() set.
    bool record(const QString &relativePath, const QString &text, const QDateTime &when = QDateTime::currentDateTime());
    /// Newest first.
    QList<Version> versions(const QString &relativePath) const;
    /// The note as `version` held it; empty when there is no such version.
    QString read(const QString &relativePath, const QString &version) const;
    /// Moves a note's history to its new path, after a rename.
    bool rename(const QString &oldRelative, const QString &newRelative);
    /// Packs loose objects so git stores each version as a delta against its
    /// neighbour. Worth running now and then, never on the save path.
    bool pack();
    /// Loose objects waiting for pack(), for deciding when it is worth it.
    int looseObjects() const;

private:
    bool create();

    struct Repository;
    std::unique_ptr<Repository> m_repo;
    mutable QString m_error;
};

} // namespace noteAppC
