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
/// Nothing is done when the history is constructed: libgit2 is set up and the
/// repository opened by the first call that needs them, on the worker, and
/// the repository is created by the first record(), so a vault that is only
/// opened leaves nothing on disk and costs nothing.
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

    /// How many times libgit2's set-up was asked for on the application's
    /// main thread (it loads OpenSSL: 3-4 ms, +6 MB). For the tests.
    static int mainThreadSetupsForTests();

    /// Where the repository for `vaultRoot` lives.
    static QString repositoryPathFor(const QString &vaultRoot);

    /// False only when an existing repository could not be opened. Opens it
    /// (and sets libgit2 up) if nothing has yet: call it on the worker.
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
    /// libgit2 and the repository, on the first call that needs them: a
    /// history constructed and never used costs nothing.
    void open() const;

    struct Repository;
    std::unique_ptr<Repository> m_repo;
    mutable QString m_error;
};

} // namespace noteAppC
