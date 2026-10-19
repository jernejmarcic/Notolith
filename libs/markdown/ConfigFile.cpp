#include "ConfigFile.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

namespace noteAppC {

QString configFilePath(const QString &name)
{
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.config")
                                       : QString::fromLocal8Bit(xdg);
    return QDir(base).filePath(QStringLiteral("noteAppC/") + name);
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
