// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "ToggleSwitch.h"

#include <QPainter>

#include "Theme.h"

namespace noteAppC {

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setTheme(Theme::builtin());
}

void ToggleSwitch::setTheme(const Theme &theme)
{
    m_onColor = theme.color(QStringLiteral("--interactive-accent"));
    m_offColor = theme.color(QStringLiteral("--background-modifier-border"));
    m_knobColor = theme.color(QStringLiteral("--text-on-accent"));
    update();
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(36, 20);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF track(0.5, 0.5, width() - 1.0, height() - 1.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(isChecked() ? m_onColor : m_offColor);
    painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);

    const qreal diameter = track.height() - 4;
    const qreal x = isChecked() ? track.right() - diameter - 2 : track.left() + 2;
    painter.setBrush(isChecked() ? m_knobColor : palette().window().color());
    painter.drawEllipse(QRectF(x, track.top() + 2, diameter, diameter));
}

} // namespace noteAppC
