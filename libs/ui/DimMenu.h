#pragma once

#include <QAction>
#include <QMenu>
#include <QStyle>
#include <QStyleOptionMenuItem>

namespace noteAppC {

/// A menu whose entries marked with the property `dimmed` are drawn as disabled yet stay enabled, so a
/// click still arrives: the entry of a feature that cannot run here, whose click says why. Qt has no
/// "looks disabled" state; this draws the one it has.
class DimMenu : public QMenu {
public:
    using QMenu::QMenu;

    static bool isDimmed(const QAction *action) { return action && action->property("dimmed").toBool(); }

    /// The option the style would paint `action` with.
    QStyleOptionMenuItem optionFor(const QAction *action) const
    {
        QStyleOptionMenuItem option;
        initStyleOption(&option, action);
        return option;
    }

protected:
    void initStyleOption(QStyleOptionMenuItem *option, const QAction *action) const override
    {
        QMenu::initStyleOption(option, action);
        if (isDimmed(action)) {
            option->state &= ~QStyle::State_Enabled;
            option->palette.setCurrentColorGroup(QPalette::Disabled);
        }
    }
};

} // namespace noteAppC
