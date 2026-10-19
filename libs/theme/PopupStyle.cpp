#include "PopupStyle.h"

#include "Icons.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QInputDevice>
#include <QScroller>
#include <QAction>
#include <QDialog>
#include <QEvent>
#include <QPainter>
#include <QMenu>
#include <QStyleFactory>
#include <QWidget>

namespace noteAppC {

namespace {

/// Covers its menu, lets every click through, and draws the check of each
/// checked row at the right edge, in the menu's text colour.
class MenuChecks : public QWidget
{
public:
    explicit MenuChecks(QMenu *menu)
        : QWidget(menu)
        , m_menu(menu)
    {
        setObjectName(QStringLiteral("menuChecks"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        menu->installEventFilter(this);
        setGeometry(menu->rect());
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
            setGeometry(m_menu->rect());
            raise();
            break;
        case QEvent::ActionChanged:
        case QEvent::ActionAdded:
        case QEvent::ActionRemoved:
            update();
            break;
        default:
            break;
        }
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        constexpr int size = 14;
        QPainter painter(this);
        for (QAction *action : m_menu->actions()) {
            if (!action->isChecked() || !action->isVisible() || action->isSeparator())
                continue;
            const QRect row = m_menu->actionGeometry(action);
            const QRect box = QStyle::alignedRect(layoutDirection(), Qt::AlignRight | Qt::AlignVCenter,
                                                  QSize(size, size), row.adjusted(0, 0, -8, 0));
            const QPalette::ColorGroup group = action->isEnabled() ? QPalette::Normal : QPalette::Disabled;
            painter.drawPixmap(box, Icons::icon(QStringLiteral("check"), m_menu->palette().color(group, QPalette::Text), size)
                                        .pixmap(QSize(size, size), devicePixelRatioF()));
        }
    }

private:
    QMenu *m_menu;
};

} // namespace

void grabTouchScroll(QWidget *viewport)
{
    const QList<const QInputDevice *> devices = QInputDevice::devices();
    if (std::none_of(devices.cbegin(), devices.cend(), [](const QInputDevice *device) {
            return device->type() == QInputDevice::DeviceType::TouchScreen;
        }))
        return;
    QScroller::grabGesture(viewport, QScroller::TouchGesture);
    // Qt waits for 5 mm of travel before the page moves at all, which reads
    // as the page sticking under the finger at the start of every drag.
    QScroller *scroller = QScroller::scroller(viewport);
    QScrollerProperties properties = scroller->scrollerProperties();
    properties.setScrollMetric(QScrollerProperties::DragStartDistance, 0.002);
    scroller->setScrollerProperties(properties);
}

PopupStyle::PopupStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
{
}

void PopupStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    // A scroll area inside another (a block in a note, a list in a dialog
    // row) leaves the finger to the one around it: grabbed, it would take a
    // drag that starts on it even with nothing of its own to scroll.
    if (auto *area = qobject_cast<QAbstractScrollArea *>(widget)) {
        QWidget *outer = area->parentWidget();
        while (outer && !qobject_cast<QAbstractScrollArea *>(outer))
            outer = outer->parentWidget();
        if (!outer)
            grabTouchScroll(area->viewport());
    }
    if (auto *menu = qobject_cast<QMenu *>(widget); menu && !menu->findChild<QWidget *>(QStringLiteral("menuChecks"), Qt::FindDirectChildrenOnly))
        new MenuChecks(menu);
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
