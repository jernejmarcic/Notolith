#pragma once

#include <QStringList>
#include <QWidget>

#include <functional>

#include "MarkdownEditor.h"
#include "Theme.h"

class QLabel;

namespace noteAppC {

/// Obsidian's Slides: a note presented one slide at a time, the slides split
/// at its horizontal rules, each drawn in reading view at a size that fills
/// the screen. Arrow keys, Page Up/Down, Space and Backspace step; Home and
/// End jump; Escape ends it.
class SlidesView : public QWidget {
    Q_OBJECT

public:
    explicit SlidesView(QWidget *parent = nullptr);

    /// The note's slides: what lies between its `---` rules, frontmatter
    /// left out, code blocks and callouts kept whole.
    static QStringList split(const QString &text);

    void setTheme(const Theme &theme);
    void setLinkResolver(std::function<QString(const QString &)> resolver);
    void setFeatures(const MarkdownEditor::Features &features);
    void present(const QStringList &slides, int first = 0);

    int current() const { return m_current; }
    int count() const { return int(m_slides.size()); }
    MarkdownEditor *editor() const { return m_editor; }

signals:
    void finished();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void showSlide(int index);
    void applyScale();

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    QStringList m_slides;
    int m_current = 0;
    int m_scaledFor = 0; ///< the text size m_editor was last given
    MarkdownEditor *m_editor = nullptr;
    QLabel *m_counter = nullptr;
};

} // namespace noteAppC
