// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "BaseCellDelegate.h"

#include "BaseTableModel.h"

#include <QPainter>

namespace noteAppC::bases {

void BaseCellDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QVariant chipsData = index.data(BaseTableModel::ChipRole);
    if (!chipsData.canConvert<QList<BaseChip>>()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    const QList<BaseChip> chips = chipsData.value<QList<BaseChip>>();

    painter->save();
    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());

    const QFontMetrics fm = option.fontMetrics;
    constexpr int padX = 6, gap = 4, margin = 4;
    const int chipHeight = fm.height() + 4;
    int x = option.rect.left() + margin;
    const int y = option.rect.top() + (option.rect.height() - chipHeight) / 2;
    const int rightLimit = option.rect.right() - margin;
    painter->setRenderHint(QPainter::Antialiasing);

    for (const BaseChip &chip : chips) {
        const int textWidth = fm.horizontalAdvance(chip.text);
        const QRect chipRect(x, y, textWidth + padX * 2, chipHeight);
        if (chipRect.right() > rightLimit) {
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            painter->drawText(QRect(x, y, rightLimit - x, chipHeight), Qt::AlignVCenter, QStringLiteral("..."));
            break;
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(chip.isLink ? option.palette.color(QPalette::Highlight).lighter(160)
                                      : option.palette.color(QPalette::Button));
        painter->drawRoundedRect(chipRect, chipHeight / 2.0, chipHeight / 2.0);
        painter->setPen(chip.isLink ? option.palette.color(QPalette::Highlight).darker(120)
                                    : option.palette.color(QPalette::ButtonText));
        painter->drawText(chipRect, Qt::AlignCenter, chip.text);
        x = chipRect.right() + gap;
    }
    painter->restore();
}

} // namespace noteAppC::bases
