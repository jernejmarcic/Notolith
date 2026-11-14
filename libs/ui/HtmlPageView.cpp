// SPDX-License-Identifier: GPL-3.0-or-later
#include "HtmlPageView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

namespace noteAppC {

HtmlPageView::HtmlPageView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("htmlPageView"));
    setFrameShape(QFrame::NoFrame);
    viewport()->setMouseTracking(true);
    verticalScrollBar()->setSingleStep(40);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}

HtmlPageView::~HtmlPageView() = default;

void HtmlPageView::setContent(const QString &html, const QString &baseDir,
                              const std::function<QString(const QString &)> &localPath)
{
    m_html = html;
    m_baseDir = baseDir;
    m_localPath = localPath;
    relayout();
}

void HtmlPageView::setTheme(const Theme &theme)
{
    m_theme = theme;
    relayout();
}

int HtmlPageView::pageHeight() const
{
    return m_page ? m_page->height() : 0;
}

void HtmlPageView::relayout()
{
    if (m_html.isEmpty() || viewport()->width() <= 0) {
        m_page.reset();
    } else {
        HtmlRenderer::Theme theme;
        theme.text = m_theme.color(QStringLiteral("--text-normal"));
        theme.background = m_theme.color(QStringLiteral("--background-primary"));
        theme.link = m_theme.color(QStringLiteral("--text-accent"));
        theme.border = m_theme.color(QStringLiteral("--background-modifier-border"));
        theme.codeBackground = m_theme.color(QStringLiteral("--background-primary-alt"));
        theme.textFont = m_theme.textFont();
        theme.codeFont = m_theme.monospaceFont();
        m_page = HtmlRenderer::layout(m_html, theme, viewport()->width(), m_baseDir, QString(), m_localPath);
    }
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, qMax(0, pageHeight() - viewport()->height()));
    viewport()->update();
}

void HtmlPageView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout(); // width re-wraps the page; height moves the scroll range
}

void HtmlPageView::paintEvent(QPaintEvent *)
{
    QPainter out(viewport());
    out.fillRect(viewport()->rect(), m_theme.color(QStringLiteral("--background-primary")));
    if (!m_page)
        return;
    const qreal ratio = devicePixelRatioF();
    const QSize size = viewport()->size();
    QImage image(size * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    m_largestImage = qMax(m_largestImage, qint64(image.width()) * image.height());
    const int top = verticalScrollBar()->value();
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.scale(ratio, ratio);
        painter.translate(0, -top);
        m_page->draw(painter, QRect(0, top, size.width(), size.height()));
    }
    out.drawImage(0, 0, image);
}

QString HtmlPageView::linkAt(const QPoint &viewportPos) const
{
    return m_page ? m_page->linkAt(viewportPos + QPoint(0, verticalScrollBar()->value())) : QString();
}

void HtmlPageView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressAt = event->position().toPoint();
        m_pressLink = linkAt(m_pressAt);
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void HtmlPageView::mouseMoveEvent(QMouseEvent *event)
{
    viewport()->setCursor(linkAt(event->position().toPoint()).isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
    QAbstractScrollArea::mouseMoveEvent(event);
}

void HtmlPageView::mouseReleaseEvent(QMouseEvent *event)
{
    // A click: released on the link it was pressed on.
    if (event->button() == Qt::LeftButton && !m_pressLink.isEmpty()
        && linkAt(event->position().toPoint()) == m_pressLink)
        emit linkActivated(m_pressLink);
    m_pressLink.clear();
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void HtmlPageView::keyPressEvent(QKeyEvent *event)
{
    QScrollBar *bar = verticalScrollBar();
    switch (event->key()) {
    case Qt::Key_Down: bar->triggerAction(QAbstractSlider::SliderSingleStepAdd); break;
    case Qt::Key_Up: bar->triggerAction(QAbstractSlider::SliderSingleStepSub); break;
    case Qt::Key_PageDown:
    case Qt::Key_Space: bar->triggerAction(QAbstractSlider::SliderPageStepAdd); break;
    case Qt::Key_PageUp: bar->triggerAction(QAbstractSlider::SliderPageStepSub); break;
    case Qt::Key_Home: bar->setValue(0); break;
    case Qt::Key_End: bar->setValue(bar->maximum()); break;
    default: QAbstractScrollArea::keyPressEvent(event); return;
    }
}

} // namespace noteAppC
