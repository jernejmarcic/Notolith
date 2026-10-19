#pragma once

#include <QProxyStyle>

namespace noteAppC {

/// Fusion, plus rounded popups. The QSS gives a menu, a tooltip or a suggester
/// a border-radius, but a popup window is a rectangle: the corners stay square
/// unless the window itself is translucent and frameless. QStyleSheetStyle
/// polishes through this style, so one polish() covers every QMenu,
/// QCompleter list and tooltip in the application, the ones made later too.
///
/// A checked menu item gets Obsidian's check at the menu's right edge. QSS
/// styles a menu item itself (the style's drawControl is never asked), and its
/// `QMenu::indicator` can only sit left of the icon, so the style adds a
/// transparent overlay to each menu that paints the checks.
class PopupStyle : public QProxyStyle
{
public:
    PopupStyle();

    void polish(QWidget *widget) override;
    using QProxyStyle::polish;
};

/// Touch: a finger dragging `viewport` scrolls it with momentum. Only where
/// a touchscreen exists, so a desktop with a mouse keeps drag-to-select.
/// PopupStyle grabs it for every scroll area not inside another one; a
/// widget that swaps its viewport grabs the new one itself.
void grabTouchScroll(QWidget *viewport);

} // namespace noteAppC
