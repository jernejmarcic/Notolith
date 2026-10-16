#pragma once

#include <QProxyStyle>

namespace noteAppC {

/// Fusion, plus rounded popups. The QSS gives a menu, a tooltip or a suggester
/// a border-radius, but a popup window is a rectangle: the corners stay square
/// unless the window itself is translucent and frameless. QStyleSheetStyle
/// polishes through this style, so one polish() covers every QMenu,
/// QCompleter list and tooltip in the application, the ones made later too.
class PopupStyle : public QProxyStyle
{
public:
    PopupStyle();

    void polish(QWidget *widget) override;
    using QProxyStyle::polish;
};

} // namespace noteAppC
