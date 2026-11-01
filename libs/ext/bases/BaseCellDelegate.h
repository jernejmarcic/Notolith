// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QStyledItemDelegate>

namespace noteAppC::bases {

/// Paints a List or Link cell (BaseTableModel::ChipRole) as rounded chips -
/// Obsidian's own look for a list property, and how a Link cell shows as a
/// link - instead of QStyledItemDelegate's plain joined text. A cell with no
/// chips (BaseTableModel::ChipRole invalid) falls straight through to the
/// base class, which is also still what creates/commits the editor: chips
/// only change how a cell is *painted*, not how it is edited (see
/// BaseTableModel's own doc comment for that path).
class BaseCellDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace noteAppC::bases
