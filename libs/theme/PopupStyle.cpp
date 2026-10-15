#include "PopupStyle.h"

#include <QAbstractItemView>
#include <QDialog>
#include <QMenu>
#include <QStyleFactory>
#include <QWidget>

namespace noteAppC {

PopupStyle::PopupStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
{
}

void PopupStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    if (!widget->isWindow())
        return;
    const Qt::WindowType type = widget->windowType();
    if ((type != Qt::Popup && type != Qt::ToolTip))
        return;
    // A window already shown cannot change its flags without a flicker, and
    // a polish after show() is the style being switched, not a popup opening.
    if (widget->isVisible())
        return;
    widget->setAttribute(Qt::WA_TranslucentBackground);
    // Translucent switches the system background off, and with it the QSS
    // background a plain QDialog would have painted: ask for it back.
    widget->setAttribute(Qt::WA_NoSystemBackground, false);
    widget->setAttribute(Qt::WA_StyledBackground);
    widget->setWindowFlag(Qt::FramelessWindowHint);
    widget->setWindowFlag(Qt::NoDropShadowWindowHint);
    // A list's viewport paints a square background of its own over the
    // rounded frame.
    if (auto *view = qobject_cast<QAbstractItemView *>(widget))
        view->viewport()->setAutoFillBackground(false);
}

} // namespace noteAppC
