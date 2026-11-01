#include "Bookmarks.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <optional>

#include "Settings.h"

namespace noteAppC {

namespace {

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject readJSONobject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

std::optional<BookmarkItem> parseItem(const QJsonObject &obj)
{
    BookmarkItem item;
    item.id = newId();
    item.title = obj.value(QLatin1String("title")).toString();

    const QString type = obj.value(QLatin1String("type")).toString();
    if (type == QLatin1String("file")) {
        item.type = BookmarkItem::Type::File;
        item.path = obj.value(QLatin1String("path")).toString();
        item.subpath = obj.value(QLatin1String("subpath")).toString();
    } else if (type == QLatin1String("search")) {
        item.type = BookmarkItem::Type::Search;
        item.query = obj.value(QLatin1String("query")).toString();
    } else if (type == QLatin1String("group")) {
        item.type = BookmarkItem::Type::Group;
        for (const QJsonValue &child : obj.value(QLatin1String("items")).toArray()) {
            if (auto parsed = parseItem(child.toObject()))
                item.items.append(*parsed);
        }
    } else {
        // folder / graph / url bookmarks - see the Limit note in the header.
        return std::nullopt;
    }
    return item;
}

QJsonObject toJson(const BookmarkItem &item)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), item.id);
    switch (item.type) {
    case BookmarkItem::Type::File:
        obj.insert(QStringLiteral("type"), QStringLiteral("file"));
        obj.insert(QStringLiteral("path"), item.path);
        if (!item.subpath.isEmpty())
            obj.insert(QStringLiteral("subpath"), item.subpath);
        break;
    case BookmarkItem::Type::Search:
        obj.insert(QStringLiteral("type"), QStringLiteral("search"));
        obj.insert(QStringLiteral("query"), item.query);
        break;
    case BookmarkItem::Type::Group: {
        obj.insert(QStringLiteral("type"), QStringLiteral("group"));
        QJsonArray children;
        for (const BookmarkItem &child : item.items)
            children.append(toJson(child));
        obj.insert(QStringLiteral("items"), children);
        break;
    }
    }
    if (!item.title.isEmpty())
        obj.insert(QStringLiteral("title"), item.title);
    return obj;
}

BookmarkItem *findItem(QList<BookmarkItem> &items, const QString &id)
{
    for (BookmarkItem &item : items) {
        if (item.id == id)
            return &item;
        if (item.type == BookmarkItem::Type::Group) {
            if (BookmarkItem *found = findItem(item.items, id))
                return found;
        }
    }
    return nullptr;
}

bool removeItem(QList<BookmarkItem> &items, const QString &id)
{
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (items[i].id == id) {
            items.removeAt(i);
            return true;
        }
        if (items[i].type == BookmarkItem::Type::Group && removeItem(items[i].items, id))
            return true;
    }
    return false;
}

} // namespace

const BookmarkItem *Bookmarks::find(const QString &id) const
{
    return findItem(const_cast<QList<BookmarkItem> &>(m_items), id);
}

QString Bookmarks::storagePathFor(const QString &vaultRoot)
{
    const QString root = QDir(vaultRoot).absolutePath();
    const QByteArray digest =
        QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QDir(Settings::configDirectory())
        .filePath(QStringLiteral("vaults/%1/bookmarks.json").arg(QString::fromLatin1(digest)));
}

Bookmarks::Bookmarks(const QString &vaultRoot)
    : m_vaultRoot(vaultRoot)
{
}

void Bookmarks::load()
{
    m_items.clear();

    const QString ownPath = storagePathFor(m_vaultRoot);
    const bool haveOwnCopy = QFileInfo::exists(ownPath);
    const QString sourcePath = haveOwnCopy
        ? ownPath
        : QDir(m_vaultRoot).filePath(QStringLiteral(".obsidian/bookmarks.json"));

    const QJsonObject root = readJSONobject(sourcePath);
    for (const QJsonValue &value : root.value(QLatin1String("items")).toArray()) {
        if (auto item = parseItem(value.toObject()))
            m_items.append(*item);
    }
}

bool Bookmarks::save(QString *error) const
{
    const QString path = storagePathFor(m_vaultRoot);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }

    QJsonArray array;
    for (const BookmarkItem &item : m_items)
        array.append(toJson(item));
    QJsonObject root;
    root.insert(QStringLiteral("items"), array);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

QList<BookmarkItem> &Bookmarks::targetList(const QString &groupTitle)
{
    if (!groupTitle.isEmpty()) {
        for (BookmarkItem &item : m_items) {
            if (item.type == BookmarkItem::Type::Group && item.title == groupTitle)
                return item.items;
        }
    }
    return m_items;
}

QString Bookmarks::addNote(const QString &path, const QString &title, const QString &groupTitle)
{
    BookmarkItem item;
    item.id = newId();
    item.type = BookmarkItem::Type::File;
    item.path = path;
    item.title = title;
    targetList(groupTitle).append(item);
    return item.id;
}

QString Bookmarks::addHeading(const QString &path, const QString &subpath, const QString &title,
                              const QString &groupTitle)
{
    BookmarkItem item;
    item.id = newId();
    item.type = BookmarkItem::Type::File;
    item.path = path;
    item.subpath = subpath;
    item.title = title;
    targetList(groupTitle).append(item);
    return item.id;
}

QString Bookmarks::addSearch(const QString &query, const QString &title, const QString &groupTitle)
{
    BookmarkItem item;
    item.id = newId();
    item.type = BookmarkItem::Type::Search;
    item.query = query;
    item.title = title;
    targetList(groupTitle).append(item);
    return item.id;
}

QString Bookmarks::addGroup(const QString &title)
{
    BookmarkItem item;
    item.id = newId();
    item.type = BookmarkItem::Type::Group;
    item.title = title;
    m_items.append(item);
    return item.id;
}

bool Bookmarks::remove(const QString &id)
{
    return removeItem(m_items, id);
}

bool Bookmarks::renameGroup(const QString &id, const QString &newTitle)
{
    BookmarkItem *item = findItem(m_items, id);
    if (!item || item->type != BookmarkItem::Type::Group)
        return false;
    item->title = newTitle;
    return true;
}

} // namespace noteAppC
