// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "StorageMigration.h"

#include "ConfigFile.h"
#include "Settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSettings>

#include <cerrno>
#include <climits>
#include <cstring>
#include <functional>
#include <memory>
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

/// Why a file did not move. `permanent`: something is in its way that another start cannot change (a file at
/// the destination, a FIFO), so only the person can settle it. `copied`: it did get to its new place, but the
/// original could not be removed, so there is one in each.
struct Refusal {
    QString why;
    bool permanent = false;
    bool copied = false;
    bool moved() const { return why.isEmpty(); }
};

Refusal inTheWay(const QString &why) { return {why, true, false}; }

/// Removes the original once its copy is in place; a directory this user cannot write keeps it.
Refusal removeOriginal(const QByteArray &source)
{
    if (::unlink(source.constData()) == 0)
        return {};
    return {failure(QStringLiteral("the copy is in place, but the original could not be removed")), true, true};
}

/// Moves a symbolic link as a link, never what it points at. The text of the link goes along unless it is
/// relative and would point somewhere else from the new directory; then it becomes the path it meant.
Refusal moveLink(const QString &from, const QString &to)
{
    const QByteArray source = from.toLocal8Bit();
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink(source.constData(), buffer, sizeof buffer - 1);
    if (length < 0)
        return {failure(QStringLiteral("the link cannot be read"))};
    QString target = QString::fromLocal8Bit(buffer, length);
    if (QDir::isRelativePath(target)) {
        const QString meant = QDir::cleanPath(QFileInfo(from).absolutePath() + QLatin1Char('/') + target);
        if (meant != QDir::cleanPath(QFileInfo(to).absolutePath() + QLatin1Char('/') + target))
            target = meant;
    }
    const QByteArray destination = to.toLocal8Bit();
    if (::symlink(target.toLocal8Bit().constData(), destination.constData()) != 0)
        return errno == EEXIST ? inTheWay(QStringLiteral("it already exists at the destination"))
                               : Refusal{failure(QStringLiteral("the link cannot be made"))};
    if (!syncDirectory(QFileInfo(to).absolutePath())) {
        ::unlink(destination.constData());
        return {QStringLiteral("it could not be written to disk")};
    }
    return removeOriginal(source);
}

/// Moves one file to `to`, which must not exist; says why when it did not (and it is then exactly where it
/// was). A hard link and an unlink are an atomic rename that refuses to replace anything;
/// across file systems it is a copy, an fsync of the copy and of its directory, and a rename into place, and
/// only then does the original go. Only regular files and symbolic links move: a FIFO, a socket or a device
/// is none of the app's, and opening one to copy it could wait forever.
Refusal moveFile(const QString &from, const QString &to)
{
    const QByteArray source = from.toLocal8Bit();
    const QByteArray target = to.toLocal8Bit();
    struct stat info;
    if (::lstat(source.constData(), &info) != 0)
        return {failure(QStringLiteral("it cannot be read"))};
    if (!S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode))
        return inTheWay(QStringLiteral("it is not a regular file"));
    if (lexists(to))
        return inTheWay(QStringLiteral("it already exists at the destination"));
    if (!ensurePrivateDir(QFileInfo(to).absolutePath()))
        return {QStringLiteral("the destination directory cannot be made")};
    if (S_ISLNK(info.st_mode))
        return moveLink(from, to);
    if (::link(source.constData(), target.constData()) == 0)
        return removeOriginal(source);
    if (errno == EEXIST)
        return inTheWay(QStringLiteral("it already exists at the destination"));
    const QString partial = to + QStringLiteral(".migrating");
    QFile::remove(partial);
    QFile original(from);
    if (!original.copy(partial)) {
        QFile::remove(partial);
        return {QStringLiteral("it could not be copied: ") + original.errorString()};
    }
    if (!syncFile(partial)) {
        QFile::remove(partial);
        return {QStringLiteral("the copy could not be written to disk")};
    }
    if (lexists(to)) {
        QFile::remove(partial);
        return inTheWay(QStringLiteral("it already exists at the destination"));
    }
    if (::rename(partial.toLocal8Bit().constData(), target.constData()) != 0) {
        const int why = errno;
        QFile::remove(partial);
        errno = why;
        return {failure(QStringLiteral("the copy could not be put in place"))};
    }
    if (!syncDirectory(QFileInfo(to).absolutePath())) {
        ::unlink(target.constData());
        return {QStringLiteral("the copy could not be written to disk")};
    }
    return removeOriginal(source);
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
    int kept = 0; // not moved, and another start may manage it
    QStringList *lines = nullptr;
    QStringList *warnings = nullptr;
    /// What an earlier start found in its way and told the person about: path -> why. Left alone from then on.
    QHash<QString, QString> told;
    /// What is in the way as of this start: the first time said, after that carried over silently.
    QHash<QString, QString> inTheWay;
    int newlyInTheWay = 0;

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
    /// What is `permanent` is told once and then left alone; the rest is tried again at the next start.
    /// `to` is where it was going: it is said with the reason, and kept on the marker, so the person knows
    /// which file to put it over.
    void keep(const QString &path, const QString &reason, bool permanent = false, const QString &to = {})
    {
        const QString why = to.isEmpty() ? reason : reason + QStringLiteral(" (new place: ") + to + QLatin1Char(')');
        if (!permanent) {
            ++kept;
        } else if (told.contains(path)) {
            inTheWay.insert(path, told.value(path));
            return;
        } else {
            inTheWay.insert(path, why);
            ++newlyInTheWay;
        }
        warn(QStringLiteral("storage: kept %1 where it is: %2").arg(path, why));
    }

    void file(const QString &from, const QString &to)
    {
        if (!lexists(from))
            return; // another start moved it first
        if (told.contains(from)) {
            keep(from, {}, true, to);
            return;
        }
        const Refusal refusal = moveFile(from, to);
        if (refusal.moved() || refusal.copied) {
            ++moved;
            say(QStringLiteral("storage: moved %1 to %2").arg(from, to));
        }
        if (!refusal.moved())
            keep(from, refusal.why, refusal.permanent, to);
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
        mover.keep(oldFile, QStringLiteral("the directory for window.ini cannot be made"), false, target);
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
            mover.keep(oldFile, QStringLiteral("window.ini could not be written"), false, target);
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

/// The files an earlier start left alone, from the lines under the marker's first: "path<TAB>why".
QHash<QString, QString> readAttention()
{
    QHash<QString, QString> told;
    QFile marker(markerPath());
    if (!marker.open(QIODevice::ReadOnly))
        return told;
    marker.readLine();
    while (!marker.atEnd()) {
        const QString line = QString::fromUtf8(marker.readLine()).trimmed();
        const qsizetype tab = line.lastIndexOf(QLatin1Char('\t'));
        if (tab > 0)
            told.insert(line.left(tab), line.mid(tab + 1));
    }
    return told;
}

/// Where the lock that serialises two starts' migrations lives: the runtime directory, else the state directory,
/// whichever is this user's own and closed to others (the same rule as the instance sockets). Empty when neither.
QString migrationLockPath()
{
    for (const QString &directory : {appDir(XDGroot::Runtime), appDir(XDGroot::State)}) {
        if (ensurePrivateDir(directory) && isOwnPrivateDir(directory))
            return directory + QStringLiteral("/migrate.lock");
    }
    return {};
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
    // Two starts at the same instant would each move half of the files and find the other's in the way. One
    // goes first; the other waits until the files are settled and moves nothing: the first one may already have a
    // window open, and whatever it left is for a later start with no window. A dead owner, or a lock file with no
    // owner in it (a crash between create and write) older than the stale time, makes the lock stale; a live
    // owner keeps its file locked, so a long run is not taken over.
    // Without a place for it, or if it cannot be made, the run goes on unlocked, as it did before.
    std::unique_ptr<QLockFile> lock;
    if (const QString path = migrationLockPath(); !path.isEmpty()) {
        lock = std::make_unique<QLockFile>(path);
        lock->setStaleLockTime(5 * 60 * 1000);
        if (!lock->tryLock(0) && lock->error() == QLockFile::LockFailedError) {
            // ponytail: a start that waits this long gives up; the files wait for the next one.
            if (!lock->tryLock(60000) && lock->error() == QLockFile::LockFailedError) {
                const QString line = QStringLiteral("storage: another start is moving the files of the previous "
                                                    "version; they are left for the next start");
                if (warnings)
                    warnings->append(line);
                else
                    qWarning().noquote() << line;
            }
            return 0;
        }
        if (storageMigrationNeed() == MigrationNeed::None)
            return 0;
    }
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
    mover.told = readAttention();

    // A directory that is a symbolic link (a synced dotfiles folder) is the person's arrangement: its files
    // are not pulled out of it, and the person is told what is left to do.
    const auto owned = [&](const QString &directory) {
        if (!QFileInfo(directory).isSymLink())
            return true;
        mover.keep(directory,
                   QStringLiteral("it is a symbolic link, and its files are not moved out of it; move them to the "
                                  "notolith directories by hand"),
                   true);
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

    // The marker says whether a later start has anything to try again, and what it is to leave alone.
    const QString where = [&] {
        QStringList names;
        for (auto it = mover.inTheWay.cbegin(); it != mover.inTheWay.cend(); ++it)
            names.append(it.key() + QLatin1Char('\t') + it.value());
        names.sort();
        return names.join(QLatin1Char('\n'));
    }();
    QByteArray status;
    if (mover.kept)
        status = "incomplete: some files stayed in the old directories, tried again at the next start";
    else if (!mover.inTheWay.isEmpty())
        status = "attention: files stay in the old directories for the person to settle; Notolith does not try them "
                 "again unless this file is deleted";
    else
        status = "complete: the older noteAppC directories were moved here by this build";
    bool written = false;
    if (ensurePrivateDir(newState)) {
        QSaveFile mark(markerPath());
        if (mark.open(QIODevice::WriteOnly)) {
            mark.write(status + '\n');
            if (!where.isEmpty())
                mark.write(where.toUtf8() + '\n');
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
    if (mover.newlyInTheWay) {
        mover.warn(QStringLiteral("storage: %1 file(s) of the previous version stay in the old noteAppC directories, "
                                  "and Notolith will not touch them or try them again; the lines above say where each "
                                  "stays, where its new place is and why. To use an old file, put it in its new "
                                  "place yourself. To have Notolith try them again, delete %2, which also lists them")
                       .arg(mover.newlyInTheWay)
                       .arg(markerPath()));
    }
    return mover.moved;
}

} // namespace noteAppC
