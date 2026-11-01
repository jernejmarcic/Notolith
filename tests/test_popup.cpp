// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// PopupStyle's check mark: a checked menu item shows Obsidian's check at the
// menu's right edge, drawn by the style because QSS cannot place an indicator
// there. Offscreen, with the real theme's QSS on.

#include <gtest/gtest.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QImage>
#include <QMenu>
#include <QTemporaryDir>

#include "Icons.h"
#include "PopupStyle.h"
#include "Theme.h"

using namespace noteAppC;

namespace {

/// How many pixels of `image` inside `rect` are not the colour at its top-left corner.
int inkIn(const QImage &image, QRect rect)
{
    const QRgb background = image.pixel(rect.topLeft());
    int ink = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x)
            ink += image.pixel(x, y) != background;
    }
    return ink;
}

} // namespace

TEST(PopupStyleTest, ACheckedMenuItemShowsItsCheckAtTheRightEdge)
{
    QApplication::setStyle(new PopupStyle);
    qApp->setStyleSheet(Theme::builtin(Theme::Mode::Dark).styleSheet());

    QMenu menu;
    auto *group = new QActionGroup(&menu);
    QAction *first = menu.addAction(QStringLiteral("Reading"));
    QAction *second = menu.addAction(QStringLiteral("Source mode"));
    for (QAction *action : {first, second}) {
        action->setCheckable(true);
        action->setIcon(Icons::icon(QStringLiteral("code"), QColor(Qt::white), 16));
        group->addAction(action);
    }
    first->setChecked(true);
    menu.show();
    QApplication::processEvents();

    const auto rightEdge = [&](QAction *action) {
        const QRect row = menu.actionGeometry(action);
        return QRect(row.right() - 22, row.top() + 2, 16, row.height() - 4);
    };
    const auto leftHalf = [&](QAction *action) {
        const QRect row = menu.actionGeometry(action);
        return QRect(row.left() + 2, row.top() + 2, row.width() / 2, row.height() - 4);
    };
    QImage picture = menu.grab().toImage();
    EXPECT_GT(inkIn(picture, rightEdge(first)), 8) << "the check, right of the label";
    EXPECT_EQ(inkIn(picture, rightEdge(second)), 0) << "an unchecked item has nothing there";

    // Moving the check moves it, and the labels do not shift.
    const QImage before = picture.copy(leftHalf(first));
    second->setChecked(true);
    QApplication::processEvents();
    picture = menu.grab().toImage();
    EXPECT_EQ(inkIn(picture, rightEdge(first)), 0);
    EXPECT_GT(inkIn(picture, rightEdge(second)), 8);
    EXPECT_EQ(picture.copy(leftHalf(first)), before) << "same label, same icon: the check takes no room from them";
    menu.hide();
    qApp->setStyleSheet(QString());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir cacheHome; // icons read XDG_CACHE_HOME on every call
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
