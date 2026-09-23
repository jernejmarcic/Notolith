#include "EpubPosition.h"

#include <QCryptographicHash>
#include <QDir>
#include <QSettings>

namespace noteAppC {

QString EpubPositionStore::iniPath()
{
    // Same rule Settings::configDirectory() uses, deliberately duplicated
    // rather than called: XDG straight from the environment, not
    // QStandardPaths (which needs an application name set and so answers
    // differently in a test binary than in the app - the exact trap that
    // rule avoids). Same directory as settings.ini/vaults.ini either way.
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    const QString base =
        xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.config") : QString::fromLocal8Bit(xdg);
    return QDir(base).filePath(QStringLiteral("noteAppC/epub-positions.ini"));
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
    settings.endGroup();
    return position;
}

void EpubPositionStore::set(const QString &bookPath, const EpubPosition &position)
{
    QSettings settings(iniPath(), QSettings::IniFormat);
    const QString group =
        QStringLiteral("book_%1")
            .arg(QString::fromLatin1(QCryptographicHash::hash(bookPath.toUtf8(), QCryptographicHash::Sha1).toHex()));
    settings.beginGroup(group);
    settings.setValue(QStringLiteral("path"), bookPath); // for a human reading the file, not read back
    settings.setValue(QStringLiteral("spineIndex"), position.spineIndex);
    settings.setValue(QStringLiteral("scroll"), position.scrollWithinChapter);
    settings.setValue(QStringLiteral("fontPercent"), position.fontPercent);
    settings.endGroup();
}

} // namespace noteAppC
