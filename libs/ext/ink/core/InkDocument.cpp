#include "InkDocument.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace noteAppC::ink {

InkDocument::InkDocument() { m_committed = snapshot(); }

bool InkDocument::setSettings(const DocumentSettings &settings)
{
    if (!settings.isUsable())
        return false;
    m_settings = settings;
    touch();
    return true;
}

std::vector<Item>::const_iterator InkDocument::find(qint64 t) const
{
    auto it = std::lower_bound(m_items.begin(), m_items.end(), t, [](const Item &item, qint64 value) { return item.t < value; });
    return it != m_items.end() && it->t == t ? it : m_items.end();
}

std::vector<Item>::iterator InkDocument::find(qint64 t)
{
    return m_items.begin() + (std::as_const(*this).find(t) - m_items.cbegin());
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
    auto it = find(t);
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
    m_undo.push_back(std::move(m_committed));
    while (int(m_undo.size()) > kHistoryCap)
        m_undo.pop_front();
    m_redo.clear();
    m_committed = snapshot();
}

bool InkDocument::undo()
{
    if (m_revision != m_committed.revision) {
        // Live edits no commit has closed (a page grown, a stroke being drawn): undo returns to the
        // last commit and stops there. The dirty state is discarded, not pushed to redo: it was never
        // a history step (spec section 7: commit is the only snapshot point).
        restore(m_committed);
        m_committed.revision = m_revision;
        return true;
    }
    if (m_undo.empty())
        return false;
    m_redo.push_back(std::move(m_committed));
    restore(m_undo.back());
    m_committed = std::move(m_undo.back());
    m_committed.revision = m_revision;
    m_undo.pop_back();
    return true;
}

bool InkDocument::discardLiveEdits()
{
    if (m_revision == m_committed.revision)
        return false;
    const bool fileHoldsTheCommit = m_savedRevision == m_committed.revision; // saved, and not edited since the commit
    restore(m_committed);
    m_committed.revision = m_revision;
    if (fileHoldsTheCommit)
        m_savedRevision = m_revision;
    return true;
}

bool InkDocument::redo()
{
    if (m_redo.empty())
        return false;
    m_undo.push_back(std::move(m_committed));
    restore(m_redo.back());
    m_committed = std::move(m_redo.back());
    m_committed.revision = m_revision;
    m_redo.pop_back();
    return true;
}

void InkDocument::restore(const Snapshot &snapshot)
{
    m_items = snapshot.items;
    m_settings = snapshot.settings;
    m_nextT = std::max(m_nextT, snapshot.nextT); // t is never reused, not even after an undo
    reindex();
    touch(); // a new revision: the document differs from whatever was saved last
}

void InkDocument::reindex()
{
    m_grid.clear();
    for (const Item &item : m_items)
        m_grid.insert(item.t, item.bounds());
}

namespace {
bool isSheetLayout(LayoutKind layout) { return layout == LayoutKind::SemiInfinite || layout == LayoutKind::Infinite; }
} // namespace

int InkDocument::pageCount() const
{
    if (isSheetLayout(m_settings.layout))
        return 1; // one sheet, however far it has grown
    const double h = m_settings.format.height;
    if (!(h > 0))
        return 1;
    return int(std::clamp(std::ceil(m_settings.extent.height() / h - 1e-9), 1.0, double(kMaxPages)));
}

QRectF InkDocument::pageRect(int index) const
{
    if (isSheetLayout(m_settings.layout))
        return m_settings.extent;
    return QRectF(m_settings.extent.x(), m_settings.extent.y() + index * m_settings.format.height,
                  m_settings.format.width, m_settings.format.height);
}

bool InkDocument::growToContain(const QRectF &bounds)
{
    const double w = m_settings.format.width;
    const double h = m_settings.format.height;
    if (isSheetLayout(m_settings.layout)) {
        // One sheet that grows when ink pokes out of it: to the ink plus a page of room, so the pen can keep
        // going. SemiInfinite never grows left or up; Infinite does. Hostile numbers are refused like in the
        // paged layout: nothing non-finite, and no side more than kMaxPages pages long.
        const QRectF e = m_settings.extent;
        if (!bounds.isValid() || !(w > 0) || !(h > 0) || !std::isfinite(bounds.left()) || !std::isfinite(bounds.top())
            || !std::isfinite(bounds.right()) || !std::isfinite(bounds.bottom()))
            return false;
        double left = e.left(), top = e.top(), right = e.right(), bottom = e.bottom();
        if (bounds.right() > right)
            right = bounds.right() + w;
        if (bounds.bottom() > bottom)
            bottom = bounds.bottom() + h;
        if (m_settings.layout == LayoutKind::Infinite) {
            if (bounds.left() < left)
                left = bounds.left() - w;
            if (bounds.top() < top)
                top = bounds.top() - h;
        }
        if (left == e.left() && top == e.top() && right == e.right() && bottom == e.bottom())
            return false;
        if (!(right - left <= kMaxPages * w) || !(bottom - top <= kMaxPages * h))
            return false;
        m_settings.extent = QRectF(QPointF(left, top), QPointF(right, bottom));
        touch();
        return true;
    }
    // Fixed never grows. A page without height cannot be grown by whole pages (pageCount() reads it as one
    // page, so growth does too), and a bottom that is not finite or would take more than kMaxPages is
    // refused: file data cannot make this loop.
    if (m_settings.layout != LayoutKind::ContinuousVertical || !bounds.isValid() || !(h > 0) || !std::isfinite(bounds.bottom()))
        return false;
    if (!(bounds.bottom() > m_settings.extent.bottom()))
        return false;
    const double pages = std::ceil((bounds.bottom() - m_settings.extent.y()) / h - 1e-9);
    if (!(pages <= kMaxPages))
        return false;
    m_settings.extent.setHeight(std::max(m_settings.extent.height(), pages * h));
    touch();
    return true;
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
