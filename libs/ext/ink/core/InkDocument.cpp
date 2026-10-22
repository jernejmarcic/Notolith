#include "InkDocument.h"

#include <algorithm>
#include <cmath>

namespace noteAppC::ink {

InkDocument::InkDocument() { m_committed = snapshot(); }

void InkDocument::setSettings(const DocumentSettings &settings)
{
    m_settings = settings;
    touch();
}

std::vector<Item>::iterator InkDocument::find(qint64 t)
{
    auto it = std::lower_bound(m_items.begin(), m_items.end(), t, [](const Item &item, qint64 value) { return item.t < value; });
    return it != m_items.end() && it->t == t ? it : m_items.end();
}

qint64 InkDocument::addStroke(std::shared_ptr<const Stroke> stroke, Layer layer)
{
    Item item;
    item.type = Item::Type::Stroke;
    item.layer = layer;
    item.stroke = std::move(stroke);
    return addItem(std::move(item));
}

qint64 InkDocument::addItem(Item item)
{
    item.t = m_nextT++;
    m_grid.insert(item.t, item.bounds());
    m_items.push_back(std::move(item));
    touch();
    return m_items.back().t;
}

bool InkDocument::removeItem(qint64 t)
{
    auto it = find(t);
    if (it == m_items.end())
        return false;
    m_grid.remove(t, it->bounds());
    m_items.erase(it);
    touch();
    return true;
}

bool InkDocument::replaceItem(qint64 t, Item item)
{
    auto it = find(t);
    if (it == m_items.end())
        return false;
    m_grid.remove(t, it->bounds());
    item.t = t;
    *it = std::move(item);
    m_grid.insert(t, it->bounds());
    touch();
    return true;
}

const Item *InkDocument::item(qint64 t) const
{
    auto it = const_cast<InkDocument *>(this)->find(t);
    return it == m_items.end() ? nullptr : &*it;
}

std::vector<qint64> InkDocument::itemsIntersecting(const QRectF &area) const
{
    std::vector<qint64> out;
    for (qint64 t : m_grid.query(area))
        if (const Item *found = item(t); found && found->bounds().intersects(area))
            out.push_back(t);
    return out;
}

std::vector<const Item *> InkDocument::paintOrder(const QRectF &area) const
{
    std::vector<const Item *> out;
    for (qint64 t : itemsIntersecting(area))
        out.push_back(item(t));
    std::stable_sort(out.begin(), out.end(), [](const Item *a, const Item *b) {
        return a->layer != b->layer ? a->layer < b->layer : a->t < b->t;
    });
    return out;
}

QRectF InkDocument::contentBounds() const
{
    QRectF box;
    for (const Item &item : m_items)
        box |= item.bounds();
    return box;
}

void InkDocument::commit()
{
    if (m_revision == m_committed.revision)
        return;
    m_undo.push_back(m_committed);
    while (int(m_undo.size()) > kHistoryCap)
        m_undo.pop_front();
    m_redo.clear();
    m_committed = snapshot();
}

bool InkDocument::undo()
{
    if (m_undo.empty())
        return false;
    m_redo.push_back(m_committed);
    restore(m_undo.back());
    m_undo.pop_back();
    m_committed = snapshot();
    return true;
}

bool InkDocument::redo()
{
    if (m_redo.empty())
        return false;
    m_undo.push_back(m_committed);
    restore(m_redo.back());
    m_redo.pop_back();
    m_committed = snapshot();
    return true;
}

void InkDocument::restore(const Snapshot &snapshot)
{
    m_items = snapshot.items;
    m_settings = snapshot.settings;
    m_nextT = snapshot.nextT;
    reindex();
    touch(); // a new revision: the document differs from whatever was saved last
}

void InkDocument::reindex()
{
    m_grid.clear();
    for (const Item &item : m_items)
        m_grid.insert(item.t, item.bounds());
}

int InkDocument::pageCount() const
{
    const double h = m_settings.format.height;
    return h > 0 ? std::max(1, int(std::ceil(m_settings.extent.height() / h - 1e-9))) : 1;
}

QRectF InkDocument::pageRect(int index) const
{
    return QRectF(m_settings.extent.x(), m_settings.extent.y() + index * m_settings.format.height,
                  m_settings.format.width, m_settings.format.height);
}

bool InkDocument::growToContain(const QRectF &bounds)
{
    if (m_settings.layout != LayoutKind::ContinuousVertical || !bounds.isValid())
        return false; // Fixed never grows; SemiInfinite and Infinite come with Track S1 Task 9
    bool grew = false;
    while (bounds.bottom() > m_settings.extent.bottom()) {
        m_settings.extent.setHeight(m_settings.extent.height() + m_settings.format.height);
        grew = true;
    }
    if (grew)
        touch();
    return grew;
}

void InkDocument::addPage()
{
    m_settings.extent.setHeight(m_settings.extent.height() + m_settings.format.height);
    touch();
}

void InkDocument::reset(DocumentSettings settings, std::vector<Item> items, QJsonObject extraTop)
{
    m_settings = std::move(settings);
    m_items = std::move(items);
    m_extraTop = std::move(extraTop);
    std::sort(m_items.begin(), m_items.end(), [](const Item &a, const Item &b) { return a.t < b.t; });
    m_nextT = m_items.empty() ? 1 : m_items.back().t + 1;
    reindex();
    m_undo.clear();
    m_redo.clear();
    touch();
    m_savedRevision = m_revision;
    m_committed = snapshot();
}

} // namespace noteAppC::ink
