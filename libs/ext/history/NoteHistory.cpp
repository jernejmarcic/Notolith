#include "NoteHistory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <git2.h>

namespace noteAppC {

namespace {

QString lastError()
{
    const git_error *error = git_error_last();
    return error && error->message ? QString::fromUtf8(error->message) : QStringLiteral("unknown libgit2 error");
}

/// One ref per note, named by a digest of its path so any path is a valid
/// ref name.
QByteArray refFor(const QString &relativePath)
{
    return "refs/noteappc/"
         + QCryptographicHash::hash(relativePath.toUtf8(), QCryptographicHash::Sha1).toHex();
}

/// The one file a history commit holds: its tree has a single entry at each
/// level, whatever the note was called when it was written - so a note's
/// old versions still read after a rename moved its ref.
git_blob *onlyBlob(git_repository *repo, const git_tree *tree)
{
    const git_tree *level = tree;
    git_tree *owned = nullptr;
    git_blob *blob = nullptr;
    while (level && git_tree_entrycount(level) == 1) {
        const git_tree_entry *entry = git_tree_entry_byindex(level, 0);
        if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
            git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
            break;
        }
        git_tree *next = nullptr;
        if (git_tree_lookup(&next, repo, git_tree_entry_id(entry)) != 0)
            break;
        git_tree_free(owned);
        owned = next;
        level = next;
    }
    git_tree_free(owned);
    return blob;
}

} // namespace

struct NoteHistory::Repository {
    git_repository *repo = nullptr;
    QString path;
    ~Repository()
    {
        git_repository_free(repo);
    }
};

/// Once per process, and never shut down: a history's last owner can be a
/// save still running on the history pool, and its git_libgit2_shutdown()
/// there tore OpenSSL down while the next vault's init on the UI thread
/// was loading the system's certificates - a crash one test run in six.
/// The init is the 5 ms of a cold start that loads those certificates, so
/// warmUp() lets a worker pay it while the window is built; a static's init
/// is thread-safe, and whoever needs it first waits for it.
static void initLibgit2()
{
    static const int initialised = git_libgit2_init();
    Q_UNUSED(initialised);
}

void NoteHistory::warmUp()
{
    initLibgit2();
}

NoteHistory::NoteHistory(const QString &vaultRoot)
    : m_repo(std::make_unique<Repository>())
{
    initLibgit2();
    m_repo->path = repositoryPathFor(vaultRoot);
    // Only opened here: a vault that is merely looked at leaves nothing on
    // disk. The repository is created by the first record().
    if (QFileInfo::exists(m_repo->path)
        && git_repository_open_bare(&m_repo->repo, m_repo->path.toUtf8().constData()) != 0) {
        m_error = lastError();
        m_repo->repo = nullptr;
    }
}

NoteHistory::~NoteHistory() = default;

QString NoteHistory::repositoryPathFor(const QString &vaultRoot)
{
    const QByteArray digest = QCryptographicHash::hash(QDir(vaultRoot).absolutePath().toUtf8(),
                                                       QCryptographicHash::Sha256)
                                  .toHex()
                                  .left(16);
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
         + QStringLiteral("/noteAppC/history/") + QString::fromLatin1(digest) + QStringLiteral(".git");
}

bool NoteHistory::isOpen() const
{
    // Usable: opened, or not created yet (record() makes it). Only an
    // existing repository that failed to open is not.
    return m_repo->repo != nullptr || m_error.isEmpty();
}

bool NoteHistory::create()
{
    QDir().mkpath(m_repo->path);
    if (git_repository_init(&m_repo->repo, m_repo->path.toUtf8().constData(), 1) != 0) {
        m_error = lastError();
        m_repo->repo = nullptr;
        return false;
    }
    return true;
}

bool NoteHistory::record(const QString &relativePath, const QString &text, const QDateTime &when)
{
    if (relativePath.isEmpty() || (!m_repo->repo && !m_error.isEmpty()) || (!m_repo->repo && !create()))
        return false;
    git_repository *repo = m_repo->repo;
    const QByteArray bytes = text.toUtf8();
    const QByteArray ref = refFor(relativePath);

    // Unchanged since the newest version: nothing to record. Hashed first,
    // so an unchanged save leaves no dangling blob behind either.
    git_oid blob;
    if (git_odb_hash(&blob, bytes.constData(), size_t(bytes.size()), GIT_OBJECT_BLOB) != 0) {
        m_error = lastError();
        return false;
    }
    git_oid parentID;
    git_commit *parent = nullptr;
    if (git_reference_name_to_id(&parentID, repo, ref.constData()) == 0
        && git_commit_lookup(&parent, repo, &parentID) == 0) {
        git_tree *tree = nullptr;
        if (git_commit_tree(&tree, parent) == 0) {
            git_blob *last = onlyBlob(repo, tree);
            const bool same = last && git_oid_equal(git_blob_id(last), &blob);
            git_blob_free(last);
            git_tree_free(tree);
            if (same) {
                git_commit_free(parent);
                return true;
            }
        }
    }

    bool ok = git_blob_create_from_buffer(&blob, repo, bytes.constData(), size_t(bytes.size())) == 0;
    // A tree per folder level, innermost first, each holding the one entry.
    git_oid child = blob;
    git_filemode_t mode = GIT_FILEMODE_BLOB;
    const QStringList parts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = parts.size() - 1; ok && i >= 0; --i) {
        git_treebuilder *builder = nullptr;
        ok = git_treebuilder_new(&builder, repo, nullptr) == 0
          && git_treebuilder_insert(nullptr, builder, parts.at(i).toUtf8().constData(), &child, mode) == 0
          && git_treebuilder_write(&child, builder) == 0;
        git_treebuilder_free(builder);
        mode = GIT_FILEMODE_TREE;
    }
    git_tree *tree = nullptr;
    git_signature *signature = nullptr;
    ok = ok && git_tree_lookup(&tree, repo, &child) == 0
      && git_signature_new(&signature, "Notolith", "noteappc@localhost", when.toSecsSinceEpoch(),
                           when.offsetFromUtc() / 60)
             == 0;
    git_oid commit;
    const git_commit *parents[] = {parent};
    ok = ok && git_commit_create(&commit, repo, ref.constData(), signature, signature, "UTF-8",
                                 relativePath.toUtf8().append('\n').constData(), tree, parent ? 1 : 0,
                                 parents)
                   == 0;
    if (!ok)
        m_error = lastError();
    git_signature_free(signature);
    git_tree_free(tree);
    git_commit_free(parent);
    return ok;
}

QList<NoteHistory::Version> NoteHistory::versions(const QString &relativePath) const
{
    QList<Version> out;
    git_repository *repo = m_repo->repo;
    git_oid id;
    if (!repo || git_reference_name_to_id(&id, repo, refFor(relativePath).constData()) != 0)
        return out;
    git_commit *commit = nullptr;
    if (git_commit_lookup(&commit, repo, &id) != 0)
        return out;
    while (commit) {
        char hex[GIT_OID_MAX_HEXSIZE + 1] = {};
        git_oid_tostr(hex, sizeof hex, git_commit_id(commit));
        out.append({QString::fromLatin1(hex),
                    QDateTime::fromSecsSinceEpoch(git_commit_time(commit), QTimeZone::fromSecondsAheadOfUtc(
                                                                              git_commit_time_offset(commit) * 60))});
        git_commit *parent = nullptr;
        if (git_commit_parentcount(commit) == 0 || git_commit_parent(&parent, commit, 0) != 0)
            parent = nullptr;
        git_commit_free(commit);
        commit = parent;
    }
    return out;
}

QString NoteHistory::read(const QString &relativePath, const QString &version) const
{
    Q_UNUSED(relativePath); // each commit holds the one note, whatever its name was then
    git_repository *repo = m_repo->repo;
    git_oid id;
    git_commit *commit = nullptr;
    git_tree *tree = nullptr;
    if (!repo || git_oid_fromstr(&id, version.toLatin1().constData()) != 0
        || git_commit_lookup(&commit, repo, &id) != 0 || git_commit_tree(&tree, commit) != 0) {
        git_commit_free(commit);
        return {};
    }
    git_blob *blob = onlyBlob(repo, tree);
    const QString text = blob ? QString::fromUtf8(static_cast<const char *>(git_blob_rawcontent(blob)),
                                                  qsizetype(git_blob_rawsize(blob)))
                              : QString();
    git_blob_free(blob);
    git_tree_free(tree);
    git_commit_free(commit);
    return text;
}

bool NoteHistory::rename(const QString &oldRelative, const QString &newRelative)
{
    git_repository *repo = m_repo->repo;
    git_reference *old = nullptr;
    if (!repo || git_reference_lookup(&old, repo, refFor(oldRelative).constData()) != 0)
        return false;
    git_reference *moved = nullptr;
    // Not forced: a note already living at the new path keeps its own history.
    const bool ok = git_reference_rename(&moved, old, refFor(newRelative).constData(), 0, "rename") == 0;
    if (!ok)
        m_error = lastError();
    git_reference_free(moved);
    git_reference_free(old);
    return ok;
}

bool NoteHistory::pack()
{
    git_repository *repo = m_repo->repo;
    if (!repo)
        return false;
    git_packbuilder *builder = nullptr;
    git_revwalk *walk = nullptr;
    bool ok = git_packbuilder_new(&builder, repo) == 0 && git_revwalk_new(&walk, repo) == 0
           && git_revwalk_push_glob(walk, "refs/noteappc/*") == 0
           && git_packbuilder_insert_walk(builder, walk) == 0
           && git_packbuilder_write(builder, nullptr, 0, nullptr, nullptr) == 0;
    if (!ok)
        m_error = lastError();
    git_revwalk_free(walk);
    git_packbuilder_free(builder);
    if (!ok)
        return false;
    // ponytail: every object this class writes is reachable from a note's
    // ref (an unchanged save is hashed, never written), so once the pack
    // holds every ref's history the loose copies can all go. A repository
    // something else writes to would need a real prune instead.
    QDirIterator loose(m_repo->path + QStringLiteral("/objects"), QDir::Files, QDirIterator::Subdirectories);
    while (loose.hasNext()) {
        const QString file = loose.next();
        if (!file.contains(QLatin1String("/pack/")) && !file.contains(QLatin1String("/info/")))
            QFile::remove(file);
    }
    return true;
}

int NoteHistory::looseObjects() const
{
    int count = 0;
    QDirIterator loose(m_repo->path + QStringLiteral("/objects"), QDir::Files, QDirIterator::Subdirectories);
    while (loose.hasNext()) {
        const QString file = loose.next();
        count += !file.contains(QLatin1String("/pack/")) && !file.contains(QLatin1String("/info/"));
    }
    return count;
}

} // namespace noteAppC
