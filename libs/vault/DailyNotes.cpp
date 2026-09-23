#include "DailyNotes.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "MomentFormat.h"
#include "Templates.h"
#include "Vault.h"

namespace noteAppC {

namespace {

QJsonObject readJSONobject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

// A daily note walk further back than this is almost certainly a missing
// note, not a gap in a real journal - the cap keeps previous/nextNote() a
// bounded number of stat() calls instead of an unbounded scan.
constexpr int kWalkLimitDays = 3660;

} // namespace

DailyNotes::Config DailyNotes::readConfig(const QString &vaultRoot)
{
    Config config;
    const QJsonObject json =
        readJSONobject(QDir(vaultRoot).filePath(QStringLiteral(".obsidian/daily-notes.json")));
    if (json.contains(QLatin1String("folder")))
        config.folder = json.value(QLatin1String("folder")).toString();
    if (json.contains(QLatin1String("format")) && !json.value(QLatin1String("format")).toString().isEmpty())
        config.format = json.value(QLatin1String("format")).toString();
    if (json.contains(QLatin1String("template")))
        config.templatePath = json.value(QLatin1String("template")).toString();
    return config;
}

DailyNotes::DailyNotes(const QString &vaultRoot, Config config)
    : m_vaultRoot(vaultRoot)
    , m_config(std::move(config))
{
}

QString DailyNotes::pathFor(const QDate &date) const
{
    const QString name = MomentFormat::format(QDateTime(date, QTime(0, 0)), m_config.format);
    const QDir folder(m_config.folder.isEmpty() ? m_vaultRoot
                                                : QDir(m_vaultRoot).filePath(m_config.folder));
    return folder.filePath(name + QStringLiteral(".md"));
}

QString DailyNotes::ensureNote(const QDate &date, QString *error) const
{
    const QString path = pathFor(date);
    if (QFileInfo::exists(path))
        return path;

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath());
        return {};
    }

    Note note;
    if (!m_config.templatePath.isEmpty()) {
        Note templateNote;
        // Obsidian stores this setting the way it stores a link target: the
        // note's path without its ".md".
        const QString templateAbsolute =
            QDir(m_vaultRoot).filePath(m_config.templatePath + QStringLiteral(".md"));
        // A missing or unreadable template is not a reason to fail creating
        // the note - it opens empty, same as Obsidian when the template file
        // has gone away.
        if (Vault::read(templateAbsolute, templateNote)) {
            const QString title = QFileInfo(path).completeBaseName();
            // {{date}}/{{time}} with no format of their own fall back to the
            // Templates plugin's own date/time format, same as Obsidian: the
            // two core plugins share one templating engine.
            const Templates::Config templateConfig = Templates::readConfig(m_vaultRoot);
            note.text = Templates::renderText(templateNote.text, title, QDateTime(date, QTime::currentTime()),
                                              templateConfig.dateFormat, templateConfig.timeFormat);
            note.crlf = templateNote.crlf;
            note.bom = templateNote.bom;
        }
    }

    if (!Vault::write(path, note, error))
        return {};
    return path;
}

QString DailyNotes::previousNote(const QDate &date) const
{
    QDate cursor = date.addDays(-1);
    for (int i = 0; i < kWalkLimitDays; ++i, cursor = cursor.addDays(-1)) {
        const QString path = pathFor(cursor);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

QString DailyNotes::nextNote(const QDate &date) const
{
    QDate cursor = date.addDays(1);
    for (int i = 0; i < kWalkLimitDays; ++i, cursor = cursor.addDays(1)) {
        const QString path = pathFor(cursor);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

} // namespace noteAppC
