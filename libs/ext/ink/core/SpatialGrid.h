// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <unordered_map>
#include <utility>
#include <vector>

#include <QRectF>
#include <QtGlobal>

namespace noteAppC::ink {

/// A uniform grid of item bounds: viewport, eraser and lasso queries without a
/// walk over every item. Cells are `cell` document px; an item is listed in
/// every cell its bounds touch and reported once.
///
/// Bounds come from files, so the grid bounds its own work:
///  - a rect that is not finite (or has no area) is not indexed, and a query with
///    one returns nothing. Point queries (an eraser at one position) must be padded
///    to a non-empty rect, as QRectF::intersects needs an area too;
///  - a rect that spans more than kMaxCellsPerItem cells, or lies beyond +-kMaxCoord,
///    goes on a short "oversize" list that every query scans (exact rect test);
///  - a query that spans more cells than the grid holds walks the occupied cells.
class SpatialGrid {
public:
    static constexpr int kMaxCellsPerItem = 1024;
    static constexpr double kMaxCoord = 1e8; // document px; beyond it a rect is oversize

    explicit SpatialGrid(double cell = 256.0) : m_cell(cell) {}
    void insert(qint64 t, const QRectF &bounds);
    void remove(qint64 t, const QRectF &bounds);
    /// Sorted, unique. Cell granularity: callers re-test the real bounds.
    std::vector<qint64> query(const QRectF &area) const;
    void clear()
    {
        m_cells.clear();
        m_oversize.clear();
    }

private:
    struct Range {
        qint64 x0, x1, y0, y1;
        double count; ///< double: the product can overflow 64 bits
    };
    static bool indexable(const QRectF &rect);
    Range rangeOf(const QRectF &rect) const;
    static qint64 keyOf(qint64 cx, qint64 cy) { return (cx << 32) ^ qint64(quint32(cy)); }
    template <typename Visit> void eachCell(const Range &range, Visit visit) const;
    double m_cell;
    std::unordered_map<qint64, std::vector<qint64>> m_cells;
    std::vector<std::pair<qint64, QRectF>> m_oversize;
};

} // namespace noteAppC::ink
