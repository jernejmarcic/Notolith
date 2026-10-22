#pragma once

#include <unordered_map>
#include <vector>

#include <QRectF>
#include <QtGlobal>

namespace noteAppC::ink {

/// A uniform grid of item bounds: viewport, eraser and lasso queries without a
/// walk over every item. Cells are `cell` document px; an item is listed in
/// every cell its bounds touch and reported once.
class SpatialGrid {
public:
    explicit SpatialGrid(double cell = 256.0) : m_cell(cell) {}
    void insert(qint64 t, const QRectF &bounds);
    void remove(qint64 t, const QRectF &bounds);
    /// Sorted, unique.
    std::vector<qint64> query(const QRectF &area) const;
    void clear() { m_cells.clear(); }

private:
    static qint64 keyOf(int cx, int cy) { return (qint64(cx) << 32) ^ qint64(quint32(cy)); }
    template <typename Visit> void eachCell(const QRectF &area, Visit visit) const;
    double m_cell;
    std::unordered_map<qint64, std::vector<qint64>> m_cells;
};

} // namespace noteAppC::ink
