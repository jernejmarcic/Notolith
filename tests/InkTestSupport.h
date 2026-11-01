// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// Synthetic pen and touch input and a paint-rect recorder for the ink tests.
#include <QApplication>
#include <QEvent>
#include <QPaintEvent>
#include <QPointingDevice>
#include <QRect>
#include <QTabletEvent>
#include <QWidget>
#include <QtTest/QTest>

namespace inktest {

inline QPointingDevice *stylus(QPointingDevice::PointerType type = QPointingDevice::PointerType::Pen)
{
    static QPointingDevice pen(QStringLiteral("test pen"), 1, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                               QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 2);
    static QPointingDevice eraser(QStringLiteral("test eraser"), 2, QInputDevice::DeviceType::Stylus,
                                  QPointingDevice::PointerType::Eraser,
                                  QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 2);
    return type == QPointingDevice::PointerType::Eraser ? &eraser : &pen;
}

inline QPointingDevice *finger()
{
    // Registered with the window system, or the offscreen platform drops QTest's touch events.
    static QPointingDevice *touch = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
    return touch;
}

inline void tablet(QWidget *w, QEvent::Type type, QPointF pos, double pressure, qint64 ms,
                   Qt::MouseButtons buttons = Qt::LeftButton,
                   QPointingDevice::PointerType pointer = QPointingDevice::PointerType::Pen)
{
    QTabletEvent event(type, stylus(pointer), pos, w->mapToGlobal(pos.toPoint()), pressure, 0, 0, 0, 0, 0, Qt::NoModifier,
                       type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton, buttons);
    event.setTimestamp(quint64(ms));
    QApplication::sendEvent(w, &event);
}

/// A pen stroke from `a` to `b` in widget px, 10 samples 5 ms apart, starting at `ms`, at a constant raw `pressure`.
inline void penStroke(QWidget *w, QPointF a, QPointF b, qint64 ms, Qt::MouseButtons buttons = Qt::LeftButton,
                      QPointingDevice::PointerType pointer = QPointingDevice::PointerType::Pen, double pressure = 0.5)
{
    tablet(w, QEvent::TabletPress, a, pressure, ms, buttons, pointer);
    for (int i = 1; i < 9; ++i)
        tablet(w, QEvent::TabletMove, a + (b - a) * (i / 9.0), pressure, ms + i * 5, buttons, pointer);
    tablet(w, QEvent::TabletRelease, b, pressure, ms + 45, Qt::NoButton, pointer);
}

/// Records the rect of every paint event `watched` receives: what a repaint
/// actually covered, without a test hook in the widget under test.
class PaintRects : public QObject {
public:
    explicit PaintRects(QWidget *watched) : QObject(watched) { watched->installEventFilter(this); }
    QList<QRect> rects;
    QRect united() const
    {
        QRect all;
        for (const QRect &r : rects)
            all |= r;
        return all;
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            rects << static_cast<QPaintEvent *>(event)->rect();
        return false;
    }
};

} // namespace inktest
