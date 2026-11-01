#include "Hotkeys.h"

#include "ConfigFile.h"
#include "Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace noteAppC {

namespace {

/// Obsidian names keys the way the browser does; Qt spells a few of them
/// differently.
const QHash<QString, QString> &keyNames()
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("ArrowUp"), QStringLiteral("Up")},
        {QStringLiteral("ArrowDown"), QStringLiteral("Down")},
        {QStringLiteral("ArrowLeft"), QStringLiteral("Left")},
        {QStringLiteral("ArrowRight"), QStringLiteral("Right")},
        {QStringLiteral("Enter"), QStringLiteral("Return")},
        {QStringLiteral(" "), QStringLiteral("Space")},
        {QStringLiteral("Escape"), QStringLiteral("Esc")},
        {QStringLiteral("Delete"), QStringLiteral("Del")},
        {QStringLiteral("PageUp"), QStringLiteral("PgUp")},
        {QStringLiteral("PageDown"), QStringLiteral("PgDown")},
    };
    return names;
}

QJsonObject readObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void merge(Hotkeys::Bindings &into, const QJsonObject &object)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray())
            into.insert(it.key(), Hotkeys::fromJSON(it.value().toArray()));
    }
}

} // namespace

QString Hotkeys::filePath()
{
    return QDir(Settings::configDirectory()).filePath(QStringLiteral("hotkeys.json"));
}

Hotkeys::Bindings Hotkeys::load(const QString &vaultRoot)
{
    Bindings bindings;
    if (!vaultRoot.isEmpty())
        merge(bindings, readObject(QDir(vaultRoot).filePath(QStringLiteral(".obsidian/hotkeys.json"))));
    merge(bindings, readObject(filePath()));
    return bindings;
}

Hotkeys::Bindings Hotkeys::loadOwn()
{
    Bindings bindings;
    merge(bindings, readObject(filePath()));
    return bindings;
}

bool Hotkeys::save(const Bindings &bindings)
{
    QJsonObject object;
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it)
        object.insert(it.key(), toJSON(it.value()));
    const QString path = filePath();
    ensurePrivateDir(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

QList<QKeySequence> Hotkeys::fromJSON(const QJsonArray &keys)
{
    QList<QKeySequence> sequences;
    for (const QJsonValue &value : keys) {
        const QJsonObject key = value.toObject();
        QStringList parts;
        for (const QJsonValue &modifier : key.value(QStringLiteral("modifiers")).toArray()) {
            const QString name = modifier.toString();
            if (name == QLatin1String("Mod") || name == QLatin1String("Ctrl"))
                parts << QStringLiteral("Ctrl");
            else if (name == QLatin1String("Shift") || name == QLatin1String("Alt")
                     || name == QLatin1String("Meta"))
                parts << name;
        }
        const QString raw = key.value(QStringLiteral("key")).toString();
        if (raw.isEmpty())
            continue;
        parts << keyNames().value(raw, raw.size() == 1 ? raw.toUpper() : raw);
        const QKeySequence sequence = QKeySequence::fromString(parts.join(QLatin1Char('+')),
                                                               QKeySequence::PortableText);
        if (!sequence.isEmpty() && sequence[0].key() != Qt::Key_unknown)
            sequences << sequence;
    }
    return sequences;
}

QJsonArray Hotkeys::toJSON(const QList<QKeySequence> &keys)
{
    QJsonArray array;
    for (const QKeySequence &sequence : keys) {
        if (sequence.isEmpty())
            continue;
        const QKeyCombination combination = sequence[0];
        QJsonArray modifiers;
        // Obsidian writes Ctrl as "Mod", which is what keeps a file portable
        // between Linux and a Mac.
        if (combination.keyboardModifiers().testFlag(Qt::ControlModifier))
            modifiers << QStringLiteral("Mod");
        if (combination.keyboardModifiers().testFlag(Qt::ShiftModifier))
            modifiers << QStringLiteral("Shift");
        if (combination.keyboardModifiers().testFlag(Qt::AltModifier))
            modifiers << QStringLiteral("Alt");
        if (combination.keyboardModifiers().testFlag(Qt::MetaModifier))
            modifiers << QStringLiteral("Meta");
        QString key = QKeySequence(combination.key()).toString(QKeySequence::PortableText);
        for (auto it = keyNames().constBegin(); it != keyNames().constEnd(); ++it) {
            if (it.value() == key) {
                key = it.key();
                break;
            }
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("modifiers"), modifiers);
        entry.insert(QStringLiteral("key"), key);
        array << entry;
    }
    return array;
}

} // namespace noteAppC
