// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "InlineTitleEdit.h"

#include <QKeyEvent>

namespace noteAppC {

InlineTitleEdit::InlineTitleEdit(QWidget *parent)
    : QLineEdit(parent)
{
    setFrame(false); // the look is the QSS in Theme; no box until it is typed in
}

void InlineTitleEdit::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        emit cancelled();
        return;
    }
    if (event->key() == Qt::Key_Down) {
        emit moveDown();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

} // namespace noteAppC
