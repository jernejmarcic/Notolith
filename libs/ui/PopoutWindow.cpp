// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "PopoutWindow.h"

#include <QCloseEvent>
#include <QVBoxLayout>

namespace noteAppC {

PopoutWindow::PopoutWindow(Kind kind, QWidget *content)
    : QWidget(nullptr, Qt::Window)
    , m_kind(kind)
    , m_content(content)
{
    setObjectName(QStringLiteral("popoutWindow"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);
    // The owner may delete the content (a feature switched off deletes its pane):
    // the window has nothing left to show then. Queued: destroyed() is emitted
    // while the content is still among this window's children but no longer a
    // whole widget, and a close (a hide, a restyle) walks the children.
    connect(content, &QObject::destroyed, this, [this] { close(); }, Qt::QueuedConnection);
}

PopoutWindow::~PopoutWindow()
{
    // The children go with this window; that is no close to report.
    if (m_content)
        disconnect(m_content, &QObject::destroyed, this, nullptr);
}

void PopoutWindow::closeEvent(QCloseEvent *event)
{
    bool veto = false;
    emit closing(this, &veto);
    veto ? event->ignore() : event->accept();
}

} // namespace noteAppC
