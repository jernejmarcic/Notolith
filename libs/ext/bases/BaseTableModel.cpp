#include "BaseTableModel.h"

#include <QApplication>
#include <QPalette>

#include "BaseQuery.h"
#include "Frontmatter.h"
#include "Index.h"
#include "Parser.h"
#include "Vault.h"

#include <QCryptographicHash>
#include <QFileInfo>

namespace noteAppC::bases {

namespace {

/// Rewrites just the value on a scalar property entry's first line, keeping
/// whatever follows (blank lines, a trailing comment line) exactly as it
/// was - the same "only the bytes that changed" rule Frontmatter::rewriteList
/// follows for a list. Refuses a block scalar (`key: |`/`key: >`): its
/// continuation lines are not something a one-line rewrite can preserve.
std::optional<QString> rewriteScalarEntry(QStringView entry, const QString &newScalar)
{
    const qsizetype firstLineEnd = entry.indexOf(u'\n');
    const QStringView firstLine = firstLineEnd < 0 ? entry : entry.left(firstLineEnd);
    const qsizetype colon = firstLine.indexOf(u':');
    if (colon <= 0)
        return std::nullopt;
    const QStringView afterColon = firstLine.mid(colon + 1).trimmed();
    if (afterColon.startsWith(u'|') || afterColon.startsWith(u'>'))
        return std::nullopt;
    const QString keyPart = firstLine.left(colon + 1).toString();
    const QString tail = firstLineEnd < 0 ? QString() : entry.mid(firstLineEnd).toString();
    return keyPart + QLatin1Char(' ') + newScalar + tail;
}

bool isNoteProperty(const QString &reference)
{
    return !reference.startsWith(QLatin1String("file.")) && !reference.startsWith(QLatin1String("formula."));
}

QString bareKeyOf(const QString &reference)
{
    return reference.startsWith(QLatin1String("note.")) ? reference.mid(5) : reference;
}

} // namespace

BaseTableModel::BaseTableModel(BaseQuery *query, Index *index, const Vault *vault, QObject *parent)
    : QAbstractTableModel(parent), m_query(query), m_index(index), m_vault(vault)
{
}

void BaseTableModel::refresh()
{
    beginResetModel();
    endResetModel();
}

int BaseTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_query) return 0;
    return int(m_query->resultRows().size()) + (hasFooter() ? 1 : 0);
}

bool BaseTableModel::hasFooter() const
{
    return m_query && !m_query->view().summaries.isEmpty();
}

bool BaseTableModel::isFooter(int row) const
{
    return hasFooter() && row == int(m_query->resultRows().size());
}

int BaseTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_query) return 0;
    return int(m_query->view().order.size());
}

QString BaseTableModel::propertyReferenceAt(int column) const
{
    const QStringList &order = m_query->view().order;
    return column >= 0 && column < order.size() ? order.at(column) : QString();
}

bool BaseTableModel::isNameColumn(int column) const
{
    const QString ref = propertyReferenceAt(column);
    return ref == QLatin1String("file.name");
}

QString BaseTableModel::notePathAt(int row) const
{
    if (!m_query || row < 0 || row >= m_query->resultRows().size())
        return {};
    return m_query->rows().at(m_query->resultRows().at(row)).notePath;
}

QVariant BaseTableModel::data(const QModelIndex &index, int role) const
{
    if (!m_query || !index.isValid())
        return {};
    if (isFooter(index.row())) {
        // The view's summaries, a last row under its columns, as Obsidian's.
        if (role == Qt::DisplayRole)
            return m_query->summaryOf(propertyReferenceAt(index.column()));
        if (role == Qt::ForegroundRole)
            return QApplication::palette().brush(QPalette::PlaceholderText);
        return {};
    }
    const qsizetype rowIndex = m_query->resultRows().at(index.row());
    const QString ref = propertyReferenceAt(index.column());
    const Value value = m_query->valueOf(rowIndex, ref);

    if (role == Qt::CheckStateRole)
        return value.type == Value::Type::Bool ? QVariant(value.boolean ? Qt::Checked : Qt::Unchecked)
                                                : QVariant();
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        if (value.type == Value::Type::Bool)
            return {}; // shown as a checkbox, not text
        return value.toDisplayString();
    }
    if (role == ChipRole) {
        if (value.type == Value::Type::List) {
            QList<BaseChip> chips;
            chips.reserve(value.list.size());
            for (const Value &item : value.list)
                chips << BaseChip{item.toDisplayString(), item.type == Value::Type::Link};
            return QVariant::fromValue(chips);
        }
        if (value.type == Value::Type::Link)
            return QVariant::fromValue(QList<BaseChip>{BaseChip{value.toDisplayString(), true}});
    }
    return {};
}

Value BaseTableModel::valueAt(int row, int column) const
{
    if (!m_query || row < 0 || row >= m_query->resultRows().size())
        return Value::empty();
    return m_query->valueOf(m_query->resultRows().at(row), propertyReferenceAt(column));
}

Qt::ItemFlags BaseTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    if (isFooter(index.row()))
        return Qt::ItemIsEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const QString ref = propertyReferenceAt(index.column());
    if (m_index && m_vault && isNoteProperty(ref)) {
        const qsizetype rowIndex = m_query->resultRows().at(index.row());
        const Value value = m_query->valueOf(rowIndex, ref);
        if (value.type == Value::Type::Bool)
            f |= Qt::ItemIsUserCheckable;
        else
            f |= Qt::ItemIsEditable;
    }
    return f;
}

QVariant BaseTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || !m_query)
        return {};
    const QString ref = propertyReferenceAt(section);
    return m_query->file().displayNameFor(ref);
}

int BaseTableModel::columnOf(const QString &propertyReference) const
{
    for (int column = 0; column < columnCount(); ++column) {
        if (propertyReferenceAt(column) == propertyReference)
            return column;
    }
    return -1;
}

void BaseTableModel::sort(int column, Qt::SortOrder order)
{
    if (!m_query || column < 0)
        return;
    beginResetModel();
    m_query->sortBy(propertyReferenceAt(column),
                    order == Qt::AscendingOrder ? SortDirection::Ascending : SortDirection::Descending);
    endResetModel();
}

bool BaseTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!m_query || !index.isValid() || !m_index || !m_vault)
        return false;
    if (role != Qt::EditRole && role != Qt::CheckStateRole)
        return false;

    const QString ref = propertyReferenceAt(index.column());
    if (!isNoteProperty(ref))
        return false;

    const QString newText = role == Qt::CheckStateRole
        ? (value.toInt() == Qt::Checked ? QStringLiteral("true") : QStringLiteral("false"))
        : value.toString();

    const QString relativePath = notePathAt(index.row());
    QString error;
    if (!writePropertyEdit(relativePath, bareKeyOf(ref), newText, &error))
        return false;

    emit noteEdited(relativePath);
    return true;
}

bool BaseTableModel::writePropertyEdit(const QString &relativePath, const QString &key,
                                       const QString &newText, QString *error)
{
    const QString absolutePath = m_vault->absolute(relativePath);
    if (m_beforeWrite)
        m_beforeWrite(absolutePath);
    Note note;
    if (!Vault::read(absolutePath, note, error))
        return false;

    const Parser parser;
    Metadata metadata = parser.parse(note.text, absolutePath);

    Property *target = nullptr;
    for (Property &property : metadata.properties) {
        if (property.key.compare(key, Qt::CaseInsensitive) == 0) { target = &property; break; }
    }
    if (!target) {
        if (error) *error = QStringLiteral("'%1' is not a frontmatter property on this note yet").arg(key);
        return false;
    }

    std::optional<QString> rewritten;
    if (target->type == Property::Type::List) {
        // The default cell editor hands back one comma-joined line (see
        // BaseTableModel::data()'s EditRole) - split it back into items and
        // rewrite through the same list path the properties panel itself
        // uses (Frontmatter::rewriteList), so only this property's bytes
        // change. A comma inside an item cannot round-trip this way; that
        // is the corner this table editor cuts, not a general list editor.
        QStringList raws;
        for (const QString &part : newText.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const std::optional<QString> scalar = Frontmatter::scalarFor(part.trimmed(), Property::Type::Text);
            if (!scalar) {
                if (error) *error = QStringLiteral("\"%1\" is not a valid list item").arg(part.trimmed());
                return false;
            }
            raws << *scalar;
        }
        rewritten = Frontmatter::rewriteList(
            QStringView(note.text).mid(target->range.start, target->range.length()), raws);
        if (!rewritten) {
            if (error) *error = QStringLiteral("'%1' cannot be rewritten from the table (not a plain list)").arg(key);
            return false;
        }
    } else {
        const std::optional<QString> scalar = Frontmatter::scalarFor(newText, target->type);
        if (!scalar) {
            if (error) *error = QStringLiteral("\"%1\" is not a valid value for '%2'").arg(newText, key);
            return false;
        }
        rewritten = rewriteScalarEntry(QStringView(note.text).mid(target->range.start, target->range.length()), *scalar);
        if (!rewritten) {
            if (error) *error = QStringLiteral("'%1' cannot be rewritten from the table (a multi-line value)").arg(key);
            return false;
        }
    }

    note.text.replace(target->range.start, target->range.length(), *rewritten);
    if (!m_vault->writeNoteNoFollow(relativePath, note, error))
        return false;

    Metadata fresh = parser.parse(note.text, absolutePath, m_index->parseOptions()); // keeps its meta-note rows
    const QFileInfo info(absolutePath);
    fresh.mtime = info.lastModified().toMSecsSinceEpoch();
    fresh.size = info.size();
    fresh.hash = QCryptographicHash::hash(note.text.toUtf8(), QCryptographicHash::Sha1);
    m_index->put(fresh, note.text);
    return true;
}

} // namespace noteAppC::bases
