#include "StorageMigration.h"

#include "ConfigFile.h"
#include "Settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QSettings>

#include <cerrno>
#include <functional>
#include <fcntl.h>
#include <unistd.h>

namespace noteAppC {

namespace {

bool lexists(const QString &path)
{
    return QFileInfo(path).isSymLink() || QFileInfo::exists(path);
}

/// Writes what `path` holds to disk, so a copy is complete before the original goes.
void syncFile(const QString &path)
{
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ::fsync(fd);
    ::close(fd);
}

/// Moves one file to `to`, which must not exist. A hard link and an unlink are an atomic rename that
/// refuses to replace anything; across file systems it is a copy, an fsync and a rename into place.
bool moveFile(const QString &from, const QString &to)
{
    if (lexists(to) || !ensurePrivateDir(QFileInfo(to).absolutePath()))
        return false;
    const QByteArray source = from.toLocal8Bit();
    const QByteArray target = to.toLocal8Bit();
    if (::link(source.constData(), target.constData()) == 0) {
        ::unlink(source.constData());
        return true;
    }
    if (errno == EEXIST)
        return false;
    const QString partial = to + QStringLiteral(".migrating");
    QFile::remove(partial);
    if (!QFile::copy(from, partial))
        return false;
    syncFile(partial);
    if (lexists(to) || ::rename(partial.toLocal8Bit().constData(), target.constData()) != 0) {
        QFile::remove(partial);
        return false;
    }
    ::unlink(source.constData());
    return true;
}

struct Mover {
    int moved = 0;
    QStringList *lines = nullptr;

    void say(const QString &line)
    {
        if (lines)
            lines->append(line);
        else
            qInfo().noquote() << line;
    }

    void file(const QString &from, const QString &to)
    {
        if (moveFile(from, to)) {
            ++moved;
            say(QStringLiteral("storage: moved %1 to %2").arg(from, to));
        }
    }

    /// Every file under `from`, to `route(relative path)`; an empty answer deletes the file.
    void tree(const QString &from, const std::function<QString(const QString &)> &route)
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
            const QString to = route(entry.fileName());
            if (to.isEmpty())
                QFile::remove(entry.absoluteFilePath());
            else
                file(entry.absoluteFilePath(), to);
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
/// choices were keyed by SHA-1 of the vault; they follow `rekey`, and a vault it does not know loses them.
void migrateWindowState(Mover &mover, const QString &oldFile, const QHash<QString, QString> &rekey)
{
    const QString target = appDir(XDGroot::State, QStringLiteral("window.ini"));
    if (!ensurePrivateDir(QFileInfo(target).absolutePath()))
        return;
    {
        const QSettings old(oldFile, QSettings::IniFormat);
        QSettings fresh(target, QSettings::IniFormat);
        for (const QString &key : old.allKeys()) {
            QString newKey = key;
            if (key.startsWith(QLatin1String("publish/"))) {
                const QStringList parts = key.split(QLatin1Char('/'));
                if (parts.size() < 3 || !rekey.contains(parts.at(1)))
                    continue;
                newKey = QStringLiteral("publish/") + rekey.value(parts.at(1)) + QLatin1Char('/')
                         + parts.mid(2).join(QLatin1Char('/'));
            }
            if (!fresh.contains(newKey))
                fresh.setValue(newKey, old.value(key));
        }
        fresh.sync();
    }
    ++mover.moved;
    QFile::remove(oldFile);
    mover.say(QStringLiteral("storage: copied window state from %1 to %2").arg(oldFile, target));
}

} // namespace

int migrateStorage(QStringList *lines)
{
    const QString name = QStringLiteral("noteAppC");
    const QString oldConfig = xdgRoot(XDGroot::Config) + QLatin1Char('/') + name;
    const QString oldData = xdgRoot(XDGroot::Data) + QLatin1Char('/') + name;
    const QString oldState = xdgRoot(XDGroot::State) + QLatin1Char('/') + name;
    const QString oldCache = xdgRoot(XDGroot::Cache) + QLatin1Char('/') + name;
    const QString marker = appDir(XDGroot::State, QStringLiteral(".migrated"));
    if (QFileInfo::exists(marker))
        return 0;
    const bool anything = QFileInfo(oldConfig).isDir() || QFileInfo(oldData).isDir()
                          || QFileInfo(oldState).isDir() || QFileInfo(oldCache).isDir();
    if (!anything)
        return 0;

    const QString newConfig = appDir(XDGroot::Config);
    const QString newData = appDir(XDGroot::Data);
    const QString newState = appDir(XDGroot::State);
    Mover mover;
    mover.lines = lines;

    // The recent vaults first: a state file of a vault is named by a hash of its path, and these are the
    // only paths there are to hash.
    mover.file(oldConfig + QStringLiteral("/vaults.ini"), newState + QStringLiteral("/vaults.ini"));
    QHash<QString, QString> rekey; // SHA-1 of the path (the old key) -> vaultDigest
    for (const QString &vault : Settings::load().recentVaults()) {
        for (const QString &spelling : {vault, QDir::cleanPath(vault)})
            rekey.insert(sha1Of(spelling), vaultDigest(vault));
    }

    // The window state does not follow the file-by-file rule: it is converted, not moved.
    const QString oldWindow = oldConfig + QStringLiteral("/noteAppC.conf");
    if (QFileInfo::exists(oldWindow))
        migrateWindowState(mover, oldWindow, rekey);

    mover.tree(oldConfig, [&](const QString &relative) -> QString {
        if (relative == QLatin1String(".save.lock"))
            return {}; // held by a process, gone with it
        if (relative == QLatin1String("epub-positions.ini"))
            return newState + QLatin1Char('/') + relative;
        if (relative.startsWith(QLatin1String("vaults/"))) {
            const QStringList parts = relative.split(QLatin1Char('/')); // vaults/<key>/<file>
            if (parts.size() == 3) {
                if (parts.at(2) == QLatin1String("workspace.json") && parts.at(1).size() == 16)
                    return newState + QLatin1Char('/') + relative;
                if (parts.at(2) == QLatin1String("graph.json")) {
                    const QString digest = parts.at(1).size() == 16 ? parts.at(1) : rekey.value(parts.at(1));
                    return digest.isEmpty() ? QString() : newState + QStringLiteral("/vaults/") + digest + QStringLiteral("/graph.json");
                }
            }
            return newData + QLatin1Char('/') + relative; // bookmarks.json, icons.json, workspaces.json
        }
        return newConfig + QLatin1Char('/') + relative;
    });

    mover.tree(oldData, [&](const QString &relative) -> QString {
        // A lock a process held and the log of the link layer: nothing to keep.
        if (relative.startsWith(QLatin1String("share/link-")) && relative.endsWith(QLatin1String(".lock")))
            return {};
        if (relative == QLatin1String("share/link.log"))
            return {};
        return newData + QLatin1Char('/') + relative;
    });

    mover.tree(oldState, [&](const QString &relative) { return newState + QLatin1Char('/') + relative; });

    // Rebuilt on use, so not worth moving.
    if (QFileInfo(oldCache).isDir()) {
        QDir(oldCache).removeRecursively();
        mover.say(QStringLiteral("storage: removed the old cache %1").arg(oldCache));
    }

    for (const QString &old : {oldConfig, oldData, oldState})
        removeEmptyTree(old);

    if (ensurePrivateDir(newState)) {
        QFile mark(marker);
        if (mark.open(QIODevice::WriteOnly))
            mark.write("Older noteAppC directories were moved here by this build.\n");
    }
    return mover.moved;
}

} // namespace noteAppC
