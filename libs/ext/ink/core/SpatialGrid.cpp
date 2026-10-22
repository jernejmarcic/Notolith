#include "SpatialGrid.h"

#include <algorithm>
#include <cmath>

namespace noteAppC::ink {

template <typename Visit>
void SpatialGrid::eachCell(const QRectF &area, Visit visit) const
{
    if (!area.isValid())
        return;
    const int x0 = int(std::floor(area.left() / m_cell)), x1 = int(std::floor(area.right() / m_cell));
    const int y0 = int(std::floor(area.top() / m_cell)), y1 = int(std::floor(area.bottom() / m_cell));
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
            visit(keyOf(cx, cy));
}

void SpatialGrid::insert(qint64 t, const QRectF &bounds)
{
    eachCell(bounds, [&](qint64 key) { m_cells[key].push_back(t); });
}

void SpatialGrid::remove(qint64 t, const QRectF &bounds)
{
    eachCell(bounds, [&](qint64 key) {
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
    eachCell(area, [&](qint64 key) {
        auto it = m_cells.find(key);
        if (it != m_cells.end())
            out.insert(out.end(), it->second.begin(), it->second.end());
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace noteAppC::ink
