// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "HtmlRenderer.h"
#include "Theme.h"

#include <QAbstractScrollArea>

#include <functional>
#include <memory>

namespace noteAppC {

/// An HTML page drawn by litehtml (HtmlRenderer::Page) in a scroll area. Only the part in view is drawn, into an
/// image no bigger than the viewport, so a page of any height costs the memory of one screen. Nothing runs: no
/// script, no network; a click on a link is reported, never followed here.
class HtmlPageView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit HtmlPageView(QWidget *parent = nullptr);
    ~HtmlPageView() override;

    /// `localPath` decides every local image and CSS url(); see HtmlRenderer::render.
    void setContent(const QString &html, const QString &baseDir,
                    const std::function<QString(const QString &)> &localPath);
    void setTheme(const Theme &theme);
    bool hasPage() const { return bool(m_page); }
    int pageHeight() const;
    /// The most pixels any image this view drew into held; test-only proof that memory follows the viewport.
    qint64 largestImagePixels() const { return m_largestImage; }

signals:
    void linkActivated(const QString &target);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void relayout();
    QString linkAt(const QPoint &viewportPos) const;

    QString m_html;
    QString m_baseDir;
    std::function<QString(const QString &)> m_localPath;
    Theme m_theme;
    std::unique_ptr<HtmlRenderer::Page> m_page;
    qint64 m_largestImage = 0;
    QPoint m_pressAt;
    QString m_pressLink;
};

} // namespace noteAppC
