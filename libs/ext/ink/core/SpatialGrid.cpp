#include "SpatialGrid.h"

#include <algorithm>
#include <cmath>

namespace noteAppC::ink {

bool SpatialGrid::indexable(const QRectF &rect)
{
    return rect.isValid() && std::isfinite(rect.left()) && std::isfinite(rect.top()) && std::isfinite(rect.width())
        && std::isfinite(rect.height());
}

SpatialGrid::Range SpatialGrid::rangeOf(const QRectF &rect) const
{
    auto clamped = [](double v) { return std::clamp(v, -kMaxCoord, kMaxCoord); };
    Range r;
    r.x0 = qint64(std::floor(clamped(rect.left()) / m_cell));
    r.y0 = qint64(std::floor(clamped(rect.top()) / m_cell));
    // The right and bottom edges are exclusive: a rect ending exactly on a cell edge does not touch the next cell.
    r.x1 = std::max(r.x0, qint64(std::ceil(clamped(rect.right()) / m_cell)) - 1);
    r.y1 = std::max(r.y0, qint64(std::ceil(clamped(rect.bottom()) / m_cell)) - 1);
    r.count = double(r.x1 - r.x0 + 1) * double(r.y1 - r.y0 + 1);
    return r;
}

template <typename Visit>
void SpatialGrid::eachCell(const Range &range, Visit visit) const
{
    for (qint64 cy = range.y0; cy <= range.y1; ++cy)
        for (qint64 cx = range.x0; cx <= range.x1; ++cx)
            visit(keyOf(cx, cy));
}

void SpatialGrid::insert(qint64 t, const QRectF &bounds)
{
    if (!indexable(bounds))
        return;
    const bool far = std::abs(bounds.left()) > kMaxCoord || std::abs(bounds.top()) > kMaxCoord
        || std::abs(bounds.right()) > kMaxCoord || std::abs(bounds.bottom()) > kMaxCoord;
    const Range range = rangeOf(bounds);
    if (far || range.count > kMaxCellsPerItem) {
        // ponytail: linear scan per query; fine while oversize items are rare (a background image,
        // a malformed rect). A coarse second grid level if documents ever hold thousands of them.
        m_oversize.emplace_back(t, bounds);
        return;
    }
    eachCell(range, [&](qint64 key) { m_cells[key].push_back(t); });
}

void SpatialGrid::remove(qint64 t, const QRectF &bounds)
{
    if (!indexable(bounds))
        return;
    if (std::erase_if(m_oversize, [t](const auto &entry) { return entry.first == t; }) > 0)
        return;
    eachCell(rangeOf(bounds), [&](qint64 key) {
        auto it = m_cells.find(key);
        if (it == m_cells.end())
            return;
        std::erase(it->second, t);
        if (it->second.empty())
            m_cells.erase(it);
    });
}

std::vector<qint64> SpatialGrid::query(const QRectF &area) const
{
    std::vector<qint64> out;
    if (!indexable(area))
        return out;
    for (const auto &[t, rect] : m_oversize)
        if (rect.intersects(area))
            out.push_back(t);
    const Range range = rangeOf(area);
    if (range.count <= std::max(double(kMaxCellsPerItem), double(m_cells.size()))) {
        eachCell(range, [&](qint64 key) {
            auto it = m_cells.find(key);
            if (it != m_cells.end())
                out.insert(out.end(), it->second.begin(), it->second.end());
        });
    } else {
        // More cells in the query than the grid holds: visit what is occupied instead (keyOf is invertible).
        for (const auto &[key, ts] : m_cells) {
            const qint64 cx = key >> 32, cy = qint32(quint32(key));
            if (cx >= range.x0 && cx <= range.x1 && cy >= range.y0 && cy <= range.y1)
                out.insert(out.end(), ts.begin(), ts.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace noteAppC::ink
