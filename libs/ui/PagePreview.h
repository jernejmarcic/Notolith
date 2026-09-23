#pragma once

#include <QFrame>
#include <QImage>
#include <QTimer>

#include <functional>

class QLabel;

namespace noteAppC {

class MarkdownEditor;
class Theme;

/// Obsidian's page preview: the linked note, rendered, in a popover beside
/// the pointer. It stays while the pointer is on the link or on the popover
/// itself, so it can be scrolled, and goes a moment after it leaves both.
class PagePreview : public QFrame {
    Q_OBJECT

public:
    explicit PagePreview(QWidget *parent = nullptr);

    /// `text` is the whole note; `offset` is where to scroll to - a heading
    /// or a block the link names - and `globalPos` the pointer.
    void showNote(const QString &text, qsizetype offset, const QPoint &globalPos);
    /// An image, or a PDF page drawn as one, fitted into the popover.
    void showImage(const QImage &image, const QPoint &globalPos);
    /// The pointer left the link: hide soon, unless it comes onto the preview.
    void leaveSoon();
    void applyTheme(const Theme &theme);
    void setLinkResolver(std::function<QString(const QString &)> resolver);
    MarkdownEditor *editor() const { return m_editor; }

signals:
    /// A link inside the preview was followed.
    void linkActivated(const QString &target);

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    /// Below and to the right of the pointer, kept on its screen.
    void place(const QPoint &globalPos, QSize size);

    MarkdownEditor *m_editor = nullptr;
    QLabel *m_image = nullptr; ///< built on the first image preview
    QTimer m_hide;
};

} // namespace noteAppC
