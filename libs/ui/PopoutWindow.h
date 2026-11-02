// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QWidget>

namespace noteAppC {

/// A top-level window MainWindow owns but that is not a child of it: Obsidian's
/// pop-out. It only frames `content` and reports a close; what a close means
/// (saving tabs, docking a sidebar back) is MainWindow's to decide.
///
/// Parentless on purpose: a parented Qt::Window is an xdg transient on
/// Wayland, which a compositor floats over its parent like a dialog.
class PopoutWindow : public QWidget {
    Q_OBJECT

public:
    /// What the window holds: tabs in splits, a whole sidebar, or one sidebar pane.
    enum class Kind { Leaves, Sidebar, Pane };

    PopoutWindow(Kind kind, QWidget *content);
    ~PopoutWindow() override;

    Kind kind() const { return m_kind; }
    QWidget *content() const { return m_content; }

signals:
    /// From closeEvent(); the owner sets `*veto` to keep the window open.
    void closing(PopoutWindow *self, bool *veto);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    Kind m_kind;
    QPointer<QWidget> m_content;
};

} // namespace noteAppC
