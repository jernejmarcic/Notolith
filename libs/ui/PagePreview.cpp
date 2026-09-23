#include "PagePreview.h"

#include <QGuiApplication>
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
    m_editor->setNoteText(text);
    m_editor->setCursorOffset(offset); // scrolls there; reading view draws no caret

    // Below and to the right of the pointer, kept on the screen it is on.
    QRect frame(globalPos + QPoint(12, 16), QSize(kWidth, kHeight));
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
    setStyleSheet(QStringLiteral("QFrame#pagePreview { background: %1; border: 1px solid %2; "
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
