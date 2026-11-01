// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "Templates.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "MomentFormat.h"
#include "Settings.h"
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

} // namespace

Templates::Config Templates::readConfig(const QString &vaultRoot)
{
    Config config;
    const QJsonObject json =
        readJSONobject(QDir(vaultRoot).filePath(QStringLiteral(".obsidian/templates.json")));
    if (json.contains(QLatin1String("folder")))
        config.folder = json.value(QLatin1String("folder")).toString();
    if (json.contains(QLatin1String("dateFormat"))
        && !json.value(QLatin1String("dateFormat")).toString().isEmpty())
        config.dateFormat = json.value(QLatin1String("dateFormat")).toString();
    if (json.contains(QLatin1String("timeFormat"))
        && !json.value(QLatin1String("timeFormat")).toString().isEmpty())
        config.timeFormat = json.value(QLatin1String("timeFormat")).toString();
    return config;
}

Templates::Templates(const QString &vaultRoot, Config config)
    : m_vaultRoot(vaultRoot)
    , m_config(std::move(config))
{
}

QString Templates::folderAbsolute() const
{
    if (m_vaultRoot.isEmpty())
        return Settings::templatesDirectory();
    // A folder that climbs out of the vault is the vault's root instead.
    const QString folder = Vault::pathInside(m_vaultRoot, m_config.folder);
    return m_config.folder.isEmpty() || folder.isEmpty() ? m_vaultRoot : folder;
}

// Always true for the app's own templates folder (no vault).
bool Templates::inVault(const QString &path) const
{
    return m_vaultRoot.isEmpty() || Vault::resolvesInside(m_vaultRoot, path);
}

QStringList Templates::list() const
{
    QStringList out;
    const QDir base(folderAbsolute());
    if (!base.exists())
        return out;

    QDirIterator it(base.path(), QStringList{QStringLiteral("*.md")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        if (!inVault(file))
            continue;
        QString relative = base.relativeFilePath(file);
        relative.chop(3); // ".md"
        out << relative;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

bool Templates::render(const QString &templateName, const QString &noteTitle, QString *out,
                       QString *error) const
{
    const QString path = QDir(folderAbsolute()).filePath(templateName + QStringLiteral(".md"));
    Note note;
    if (!inVault(path)) { // a template that is a link out of the vault is missing
        if (error)
            *error = QStringLiteral("%1 is outside the vault").arg(path);
        return false;
    }
    if (!Vault::read(path, note, error))
        return false;
    if (out) {
        *out = renderText(note.text, noteTitle, QDateTime::currentDateTime(), m_config.dateFormat,
                          m_config.timeFormat);
    }
    return true;
}

QString Templates::renderText(const QString &content, const QString &noteTitle,
                              const QDateTime &when, const QString &dateFormat,
                              const QString &timeFormat)
{
    static const QRegularExpression tag(
        QStringLiteral("\\{\\{\\s*(title|date|time)\\s*(?::\\s*([^}]*?)\\s*)?\\}\\}"));

    // Matched back to front so replacing one does not shift the offsets of
    // the ones still to come.
    QList<QRegularExpressionMatch> matches;
    for (auto it = tag.globalMatch(content); it.hasNext();)
        matches.append(it.next());

    QString out = content;
    for (auto match = matches.rbegin(); match != matches.rend(); ++match) {
        const QString token = match->captured(1);
        const QString explicitFormat = match->captured(2);
        QString replacement;
        if (token == QLatin1String("title"))
            replacement = noteTitle;
        else if (token == QLatin1String("date"))
            replacement = MomentFormat::format(when, explicitFormat.isEmpty() ? dateFormat : explicitFormat);
        else if (token == QLatin1String("time"))
            replacement = MomentFormat::format(when, explicitFormat.isEmpty() ? timeFormat : explicitFormat);
        out.replace(match->capturedStart(), match->capturedLength(), replacement);
    }
    return out;
}

} // namespace noteAppC
