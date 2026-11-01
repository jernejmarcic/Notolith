// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "EpubPosition.h"

#include "ConfigFile.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace noteAppC {

QString EpubPositionStore::iniPath()
{
    // A reading position is state, beside vaults.ini.
    return appDir(XDGroot::State, QStringLiteral("epub-positions.ini"));
}

std::optional<EpubPosition> EpubPositionStore::get(const QString &bookPath)
{
    QSettings settings(iniPath(), QSettings::IniFormat);
    // A '/' in a QSettings key is a group separator, and a book's absolute
    // path is nothing but those - hash it into one opaque group name instead
    // of escaping every character that would otherwise need it.
    const QString group =
        QStringLiteral("book_%1")
            .arg(QString::fromLatin1(QCryptographicHash::hash(bookPath.toUtf8(), QCryptographicHash::Sha1).toHex()));
    if (!settings.childGroups().contains(group))
        return std::nullopt;

    settings.beginGroup(group);
    EpubPosition position;
    position.spineIndex = settings.value(QStringLiteral("spineIndex"), 0).toInt();
    position.scrollWithinChapter = settings.value(QStringLiteral("scroll"), 0).toInt();
    position.fontPercent = settings.value(QStringLiteral("fontPercent"), 100).toInt();
    position.paged = settings.value(QStringLiteral("paged"), false).toBool();
    settings.endGroup();
    return position;
}

void EpubPositionStore::set(const QString &bookPath, const EpubPosition &position)
{
    ensurePrivateDir(QFileInfo(iniPath()).absolutePath());
    QSettings settings(iniPath(), QSettings::IniFormat);
    const QString group =
        QStringLiteral("book_%1")
            .arg(QString::fromLatin1(QCryptographicHash::hash(bookPath.toUtf8(), QCryptographicHash::Sha1).toHex()));
    settings.beginGroup(group);
    settings.setValue(QStringLiteral("path"), bookPath); // for a human reading the file, not read back
    settings.setValue(QStringLiteral("spineIndex"), position.spineIndex);
    settings.setValue(QStringLiteral("scroll"), position.scrollWithinChapter);
    settings.setValue(QStringLiteral("fontPercent"), position.fontPercent);
    settings.setValue(QStringLiteral("paged"), position.paged);
    settings.endGroup();
}

} // namespace noteAppC
