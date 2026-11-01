// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "PagePreview.h"

#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>

#include "MarkdownEditor.h"
#include "Theme.h"

namespace noteAppC {

namespace {

constexpr int kWidth = 520;
constexpr int kHeight = 400;
/// Long enough to move the pointer from the link onto the preview.
constexpr int kGraceMs = 300;

} // namespace

PagePreview::PagePreview(QWidget *parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint)
{
    setObjectName(QStringLiteral("pagePreview"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFrameShape(QFrame::StyledPanel);
    resize(kWidth, kHeight);

    m_editor = new MarkdownEditor(this);
    m_editor->setMode(MarkdownEditor::Mode::Reading);
    connect(m_editor, &MarkdownEditor::linkActivated, this, [this](const QString &target) {
        hide();
        emit linkActivated(target);
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->addWidget(m_editor);

    m_hide.setSingleShot(true);
    m_hide.setInterval(kGraceMs);
    connect(&m_hide, &QTimer::timeout, this, &QWidget::hide);
}

void PagePreview::showNote(const QString &text, qsizetype offset, const QPoint &globalPos)
{
    m_hide.stop();
    if (m_image)
        m_image->hide();
    m_editor->show();
    m_editor->setNoteText(text);
    m_editor->setCursorOffset(offset); // scrolls there; reading view draws no caret
    place(globalPos, QSize(kWidth, kHeight));
}

void PagePreview::showImage(const QImage &image, const QPoint &globalPos)
{
    m_hide.stop();
    if (!m_image) {
        m_image = new QLabel(this);
        m_image->setAlignment(Qt::AlignCenter);
        layout()->addWidget(m_image);
    }
    m_editor->hide();
    m_editor->setNoteText(QString()); // nothing of the last note kept around
    const QSize room(kWidth - 2, kHeight - 2);
    const QImage fitted = image.width() > room.width() || image.height() > room.height()
                              ? image.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                              : image;
    m_image->setPixmap(QPixmap::fromImage(fitted));
    m_image->show();
    place(globalPos, fitted.size() + QSize(2, 2));
}

void PagePreview::place(const QPoint &globalPos, QSize size)
{
    QRect frame(globalPos + QPoint(12, 16), size);
    if (QScreen *screen = QGuiApplication::screenAt(globalPos)) {
        const QRect area = screen->availableGeometry();
        if (frame.right() > area.right())
            frame.moveRight(qMax(area.left(), globalPos.x() - 12));
        if (frame.bottom() > area.bottom())
            frame.moveBottom(qMax(area.top(), globalPos.y() - 16));
    }
    setGeometry(frame);
    show();
    raise();
}

void PagePreview::leaveSoon()
{
    if (isVisible() && !underMouse())
        m_hide.start();
}

void PagePreview::applyTheme(const Theme &theme)
{
    m_editor->setTheme(theme);
    setStyleSheet(QStringLiteral("QFrame#pagePreview, QFrame#lookupPopover { background: %1; border: 1px solid %2; "
                                 "border-radius: 6px; }")
                      .arg(theme.color(QStringLiteral("--background-primary")).name(),
                           theme.color(QStringLiteral("--background-modifier-border")).name()));
}

void PagePreview::setLinkResolver(std::function<QString(const QString &)> resolver)
{
    m_editor->setLinkResolver(std::move(resolver));
}

void PagePreview::enterEvent(QEnterEvent *event)
{
    m_hide.stop();
    QFrame::enterEvent(event);
}

void PagePreview::leaveEvent(QEvent *event)
{
    m_hide.start();
    QFrame::leaveEvent(event);
}

} // namespace noteAppC
