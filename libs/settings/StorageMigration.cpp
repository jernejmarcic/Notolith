#include "StorageMigration.h"

#include "ConfigFile.h"
#include "Settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSettings>

#include <cerrno>
#include <climits>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace noteAppC {

namespace {

bool lexists(const QString &path)
{
    return QFileInfo(path).isSymLink() || QFileInfo::exists(path);
}

/// Writes what `path` holds to disk, so a copy is complete before the original goes.
bool syncFile(const QString &path)
{
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

/// Writes the directory entries of `directory` to disk: a file is not there after a crash until they are.
bool syncDirectory(const QString &directory)
{
    const int fd = ::open(directory.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

QString failure(const QString &what)
{
    return what + QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(errno)) + QLatin1Char(')');
}

/// Moves a symbolic link as a link, never what it points at. The text of the link goes along unless it is
/// relative and would point somewhere else from the new directory; then it becomes the path it meant.
QString moveLink(const QString &from, const QString &to)
{
    const QByteArray source = from.toLocal8Bit();
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink(source.constData(), buffer, sizeof buffer - 1);
    if (length < 0)
        return failure(QStringLiteral("the link cannot be read"));
    QString target = QString::fromLocal8Bit(buffer, length);
    if (QDir::isRelativePath(target)) {
        const QString meant = QDir::cleanPath(QFileInfo(from).absolutePath() + QLatin1Char('/') + target);
        if (meant != QDir::cleanPath(QFileInfo(to).absolutePath() + QLatin1Char('/') + target))
            target = meant;
    }
    const QByteArray destination = to.toLocal8Bit();
    if (::symlink(target.toLocal8Bit().constData(), destination.constData()) != 0)
        return errno == EEXIST ? QStringLiteral("it already exists at the destination")
                               : failure(QStringLiteral("the link cannot be made"));
    if (!syncDirectory(QFileInfo(to).absolutePath())) {
        ::unlink(destination.constData());
        return QStringLiteral("it could not be written to disk");
    }
    ::unlink(source.constData());
    return {};
}

/// Moves one file to `to`, which must not exist; empty when it moved, else why it did not (and it is then
/// exactly where it was). A hard link and an unlink are an atomic rename that refuses to replace anything;
/// across file systems it is a copy, an fsync of the copy and of its directory, and a rename into place, and
/// only then does the original go. Only regular files and symbolic links move: a FIFO, a socket or a device
/// is none of the app's, and opening one to copy it could wait forever.
QString moveFile(const QString &from, const QString &to)
{
    const QByteArray source = from.toLocal8Bit();
    const QByteArray target = to.toLocal8Bit();
    struct stat info;
    if (::lstat(source.constData(), &info) != 0)
        return failure(QStringLiteral("it cannot be read"));
    if (!S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode))
        return QStringLiteral("it is not a regular file");
    if (lexists(to))
        return QStringLiteral("it already exists at the destination");
    if (!ensurePrivateDir(QFileInfo(to).absolutePath()))
        return QStringLiteral("the destination directory cannot be made");
    if (S_ISLNK(info.st_mode))
        return moveLink(from, to);
    if (::link(source.constData(), target.constData()) == 0) {
        ::unlink(source.constData());
        return {};
    }
    if (errno == EEXIST)
        return QStringLiteral("it already exists at the destination");
    const QString partial = to + QStringLiteral(".migrating");
    QFile::remove(partial);
    QFile original(from);
    if (!original.copy(partial)) {
        QFile::remove(partial);
        return QStringLiteral("it could not be copied: ") + original.errorString();
    }
    if (!syncFile(partial)) {
        QFile::remove(partial);
        return QStringLiteral("the copy could not be written to disk");
    }
    if (lexists(to) || ::rename(partial.toLocal8Bit().constData(), target.constData()) != 0) {
        QFile::remove(partial);
        return QStringLiteral("it already exists at the destination");
    }
    if (!syncDirectory(QFileInfo(to).absolutePath())) {
        ::unlink(target.constData());
        return QStringLiteral("the copy could not be written to disk");
    }
    ::unlink(source.constData());
    return {};
}

/// What to do with one file of an old directory.
struct Route {
    enum Kind { Move, Drop, Keep };
    QString to;
    Kind kind = Move;
};
Route moveTo(const QString &to) { return {to, Route::Move}; }
Route drop() { return {{}, Route::Drop}; }
Route keep() { return {{}, Route::Keep}; }

struct Mover {
    int moved = 0;
    int kept = 0;
    QStringList *lines = nullptr;
    QStringList *warnings = nullptr;

    void say(const QString &line)
    {
        if (lines)
            lines->append(line);
        else
            qInfo().noquote() << line;
    }

    void warn(const QString &line)
    {
        if (warnings)
            warnings->append(line);
        else
            qWarning().noquote() << line;
    }

    /// `path` stays where it is, and the person is told: nothing is moved to a place it is not read from.
    void keep(const QString &path, const QString &why)
    {
        ++kept;
        warn(QStringLiteral("storage: kept %1 where it is: %2").arg(path, why));
    }

    void file(const QString &from, const QString &to)
    {
        if (!lexists(from))
            return; // another start moved it first
        const QString why = moveFile(from, to);
        if (why.isEmpty()) {
            ++moved;
            say(QStringLiteral("storage: moved %1 to %2").arg(from, to));
        } else {
            keep(from, why);
        }
    }

    /// Every file under `from`, to `route(relative path)`.
    void tree(const QString &from, const std::function<Route(const QString &)> &route)
    {
        const QDir root(from);
        const QFileInfoList entries =
            root.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &entry : entries) {
            if (entry.isDir() && !entry.isSymLink()) {
                tree(entry.absoluteFilePath(), [&](const QString &relative) {
                    return route(entry.fileName() + QLatin1Char('/') + relative);
                });
                continue;
            }
            const Route where = route(entry.fileName());
            if (where.kind == Route::Drop)
                QFile::remove(entry.absoluteFilePath());
            else if (where.kind == Route::Move)
                file(entry.absoluteFilePath(), where.to);
        }
    }
};

/// Removes `path` and the directories below it that hold nothing; stops at one that does.
void removeEmptyTree(const QString &path)
{
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &entry : entries)
        removeEmptyTree(entry.absoluteFilePath());
    QDir().rmdir(path);
}

QString sha1Of(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

/// The window state an older build kept in Qt's own `noteAppC.conf`, into `window.ini`. The publish dialog's
/// choices were keyed by SHA-1 of the vault; they follow `rekey` to the new key, and those of a vault it does
/// not know keep the key they have, so nothing is lost. The old file goes only once the new one is written.
void migrateWindowState(Mover &mover, const QString &oldFile, const QHash<QString, QString> &rekey)
{
    const QString target = appDir(XDGroot::State, QStringLiteral("window.ini"));
    if (!ensurePrivateDir(QFileInfo(target).absolutePath())) {
        mover.keep(oldFile, QStringLiteral("the directory for window.ini cannot be made"));
        return;
    }
    {
        const QSettings old(oldFile, QSettings::IniFormat);
        QSettings fresh(target, QSettings::IniFormat);
        for (const QString &key : old.allKeys()) {
            QString newKey = key;
            if (key.startsWith(QLatin1String("publish/"))) {
                const QStringList parts = key.split(QLatin1Char('/'));
                if (parts.size() >= 3 && rekey.contains(parts.at(1)))
                    newKey = QStringLiteral("publish/") + rekey.value(parts.at(1)) + QLatin1Char('/')
                             + parts.mid(2).join(QLatin1Char('/'));
            }
            if (!fresh.contains(newKey))
                fresh.setValue(newKey, old.value(key));
        }
        fresh.sync();
        if (old.status() != QSettings::NoError || fresh.status() != QSettings::NoError) {
            mover.keep(oldFile, QStringLiteral("window.ini could not be written"));
            return;
        }
    }
    ++mover.moved;
    QFile::remove(oldFile);
    mover.say(QStringLiteral("storage: copied window state from %1 to %2").arg(oldFile, target));
}

QString markerPath()
{
    return appDir(XDGroot::State, QStringLiteral(".migrated"));
}

} // namespace

MigrationNeed storageMigrationNeed()
{
    const QString name = QStringLiteral("noteAppC");
    QFile marker(markerPath());
    const bool marked = marker.open(QIODevice::ReadOnly);
    if (marked && !marker.readLine().startsWith("incomplete"))
        return MigrationNeed::None;
    for (const XDGroot root : {XDGroot::Config, XDGroot::Data, XDGroot::State, XDGroot::Cache}) {
        if (QFileInfo(xdgRoot(root) + QLatin1Char('/') + name).isDir())
            return marked ? MigrationNeed::Retry : MigrationNeed::Fresh;
    }
    return MigrationNeed::None;
}

int migrateStorage(QStringList *lines, QStringList *warnings)
{
    if (storageMigrationNeed() == MigrationNeed::None)
        return 0;
    const QString name = QStringLiteral("noteAppC");
    const QString oldConfig = xdgRoot(XDGroot::Config) + QLatin1Char('/') + name;
    const QString oldData = xdgRoot(XDGroot::Data) + QLatin1Char('/') + name;
    const QString oldState = xdgRoot(XDGroot::State) + QLatin1Char('/') + name;
    const QString oldCache = xdgRoot(XDGroot::Cache) + QLatin1Char('/') + name;

    const QString newConfig = appDir(XDGroot::Config);
    const QString newData = appDir(XDGroot::Data);
    const QString newState = appDir(XDGroot::State);
    Mover mover;
    mover.lines = lines;
    mover.warnings = warnings;

    // A directory that is a symbolic link (a synced dotfiles folder) is the person's arrangement: its files
    // are not pulled out of it, and the person is told what is left to do.
    const auto owned = [&](const QString &directory) {
        if (!QFileInfo(directory).isSymLink())
            return true;
        mover.keep(directory, QStringLiteral("it is a symbolic link, and its files are not moved out of it; "
                                             "move them to the notolith directories by hand"));
        return false;
    };
    const bool configMoves = owned(oldConfig);
    const bool dataMoves = owned(oldData);
    const bool stateMoves = owned(oldState);

    // The recent vaults first: a state file of a vault is named by a hash of its path, and these are the
    // only paths there are to hash. When vaults.ini could not move (the destination has one already), the
    // old one is read too.
    const QString oldIndex = oldConfig + QStringLiteral("/vaults.ini");
    if (configMoves)
        mover.file(oldIndex, newState + QStringLiteral("/vaults.ini"));
    QStringList recent = Settings::load().recentVaults();
    if (configMoves && QFileInfo::exists(oldIndex))
        recent += Settings::recentVaultsFrom(oldIndex);
    QHash<QString, QString> rekey; // SHA-1 of the path (the old key) -> vaultDigest
    for (const QString &vault : std::as_const(recent)) {
        for (const QString &spelling : {vault, QDir::cleanPath(vault)})
            rekey.insert(sha1Of(spelling), vaultDigest(vault));
    }

    if (configMoves) {
        // The window state does not follow the file-by-file rule: it is converted, not moved.
        const QString oldWindow = oldConfig + QStringLiteral("/noteAppC.conf");
        if (QFileInfo::exists(oldWindow))
            migrateWindowState(mover, oldWindow, rekey);

        mover.tree(oldConfig, [&](const QString &relative) -> Route {
            if (relative == QLatin1String(".save.lock"))
                return drop(); // held by a process, gone with it
            // Handled above, by name: what is still here could not move, and has no other place.
            if (relative == QLatin1String("vaults.ini") || relative == QLatin1String("noteAppC.conf"))
                return keep();
            if (relative == QLatin1String("epub-positions.ini"))
                return moveTo(newState + QLatin1Char('/') + relative);
            if (relative.startsWith(QLatin1String("vaults/"))) {
                const QStringList parts = relative.split(QLatin1Char('/')); // vaults/<key>/<file>
                if (parts.size() == 3) {
                    if (parts.at(2) == QLatin1String("workspace.json") && parts.at(1).size() == 16)
                        return moveTo(newState + QLatin1Char('/') + relative);
                    if (parts.at(2) == QLatin1String("graph.json")) {
                        // A vault the recent list does not know keeps the key it has.
                        return moveTo(newState + QStringLiteral("/vaults/") + rekey.value(parts.at(1), parts.at(1))
                                      + QStringLiteral("/graph.json"));
                    }
                }
                return moveTo(newData + QLatin1Char('/') + relative); // bookmarks.json, icons.json, workspaces.json
            }
            return moveTo(newConfig + QLatin1Char('/') + relative);
        });
    }

    if (dataMoves) {
        mover.tree(oldData, [&](const QString &relative) -> Route {
            // A lock a process held and the log of the link layer: nothing to keep.
            if (relative.startsWith(QLatin1String("share/link-")) && relative.endsWith(QLatin1String(".lock")))
                return drop();
            if (relative == QLatin1String("share/link.log"))
                return drop();
            return moveTo(newData + QLatin1Char('/') + relative);
        });
    }

    if (stateMoves)
        mover.tree(oldState, [&](const QString &relative) { return moveTo(newState + QLatin1Char('/') + relative); });

    // Rebuilt on use, so not worth moving. A link is not followed into whatever it names.
    if (QFileInfo(oldCache).isSymLink()) {
        mover.say(QStringLiteral("storage: left the old cache %1, a symbolic link").arg(oldCache));
    } else if (QFileInfo(oldCache).isDir()) {
        QDir(oldCache).removeRecursively();
        mover.say(QStringLiteral("storage: removed the old cache %1").arg(oldCache));
    }

    for (const QString &old : {oldConfig, oldData, oldState}) {
        if (!QFileInfo(old).isSymLink())
            removeEmptyTree(old);
    }

    // The marker says whether a later start has anything to try again.
    bool written = false;
    if (ensurePrivateDir(newState)) {
        QSaveFile mark(markerPath());
        if (mark.open(QIODevice::WriteOnly)) {
            mark.write(mover.kept ? "incomplete: some files stayed in the old directories, tried again at the next start\n"
                                  : "complete: the older noteAppC directories were moved here by this build\n");
            written = mark.commit();
        }
    }
    if (mover.kept) {
        mover.warn(QStringLiteral("storage: %1 file(s) of the previous version could not be moved and stay in the "
                                  "old noteAppC directories; Notolith tries again at the next start")
                       .arg(mover.kept));
    } else if (!written) {
        mover.warn(QStringLiteral("storage: the migration marker could not be written; Notolith checks again at "
                                  "the next start"));
    }
    return mover.moved;
}

} // namespace noteAppC
