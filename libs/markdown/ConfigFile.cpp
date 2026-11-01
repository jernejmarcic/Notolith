// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "ConfigFile.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#include <sys/stat.h>
#include <unistd.h>

namespace noteAppC {

namespace {

/// The variable's value when it is an absolute path, else empty.
QString absoluteEnvironment(const char *name)
{
    const QString value = QString::fromLocal8Bit(qgetenv(name));
    return QDir::isAbsolutePath(value) ? QDir::cleanPath(value) : QString();
}

/// A colon-separated variable's absolute entries; the default when it is unset or empty.
QStringList absoluteEntries(const char *name, const QString &fallback)
{
    QString value = QString::fromLocal8Bit(qgetenv(name));
    if (value.isEmpty())
        value = fallback;
    QStringList entries;
    for (const QString &entry : value.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        if (QDir::isAbsolutePath(entry))
            entries << QDir::cleanPath(entry);
    }
    return entries;
}

} // namespace

QString xdgRoot(XDGroot root)
{
    if (root == XDGroot::Runtime) {
        const QString given = absoluteEnvironment("XDG_RUNTIME_DIR");
        if (!given.isEmpty())
            return given;
        // Unset, or relative and so invalid: Qt makes a directory of its own (/tmp/runtime-<user>); a relative
        // value would come back as it is, and is no answer.
        const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        return QDir::isAbsolutePath(runtime) ? QDir::cleanPath(runtime) : QString();
    }
    struct Variable {
        const char *name;
        const char *home;
    };
    static constexpr Variable variables[] = {{"XDG_CONFIG_HOME", "/.config"},
                                             {"XDG_DATA_HOME", "/.local/share"},
                                             {"XDG_STATE_HOME", "/.local/state"},
                                             {"XDG_CACHE_HOME", "/.cache"}};
    const Variable &variable = variables[static_cast<int>(root)];
    const QString value = absoluteEnvironment(variable.name);
    return value.isEmpty() ? QDir::cleanPath(QDir::homePath() + QLatin1String(variable.home)) : value;
}

QString appDir(XDGroot root, const QString &sub)
{
    const QString base = xdgRoot(root);
    if (base.isEmpty())
        return {};
    const QString dir = base + QLatin1Char('/') + kAppDirName;
    return sub.isEmpty() ? dir : dir + QLatin1Char('/') + sub;
}

QStringList xdgSearchDirs(XDGroot root)
{
    QStringList dirs{xdgRoot(root)};
    if (root == XDGroot::Config)
        dirs << absoluteEntries("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg"));
    else if (root == XDGroot::Data)
        dirs << absoluteEntries("XDG_DATA_DIRS", QStringLiteral("/usr/local/share:/usr/share"));
    return dirs;
}

bool ensurePrivateDir(const QString &path)
{
    if (path.isEmpty())
        return false; // QDir("") is the current directory
    const QString clean = QDir::cleanPath(QDir(path).absolutePath());
    if (QFileInfo(clean).isDir())
        return true;
    // Each missing component by itself: QDir::mkpath() has no way to say 0700.
    QString built;
    for (const QString &part : clean.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        built += QLatin1Char('/') + part;
        if (QFileInfo(built).isDir())
            continue;
        if (!QDir().mkdir(built, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)
            && !QFileInfo(built).isDir())
            return false;
    }
    return true;
}

bool isOwnPrivateDir(const QString &path)
{
    struct stat info;
    return !path.isEmpty() && ::lstat(path.toLocal8Bit().constData(), &info) == 0 && S_ISDIR(info.st_mode)
           && info.st_uid == ::geteuid() && (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

QString vaultDigest(const QString &root)
{
    const QString absolute = QDir(root).absolutePath();
    return QString::fromLatin1(
        QCryptographicHash::hash(absolute.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString configFilePath(const QString &name)
{
    return appDir(XDGroot::Config, name);
}

QHash<QString, QMap<QString, QString>> readINIsections(const QString &path)
{
    QHash<QString, QMap<QString, QString>> sections;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return sections;

    QString section;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed().toLower();
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || section.isEmpty())
            continue;
        QString value = line.mid(equals + 1).trimmed();
        // A trailing `; comment`, but not a `;` inside quotes or a colour.
        const qsizetype comment = value.indexOf(QLatin1String(" ;"));
        if (comment >= 0)
            value = value.left(comment).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        sections[section].insert(line.left(equals).trimmed().toLower(), value);
    }
    return sections;
}

} // namespace noteAppC
