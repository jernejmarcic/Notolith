// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QColor>
#include <QWidget>

class QLabel;
class QShowEvent;
class QTreeWidget;

namespace noteAppC {

class Index;
class Theme;

/// Obsidian's "Outgoing links" view: the links the active note makes, grouped
/// by resolved target with the source line as context. Links that resolve to
/// nothing sit in their own "Unresolved links" group.
class OutgoingLinksPane : public QWidget {
    Q_OBJECT

public:
    explicit OutgoingLinksPane(QWidget *parent = nullptr);

    void setIndex(Index *index);
    void setTheme(const Theme &theme);
    /// Absolute path of the note to show outgoing links for; empty clears the pane.
    void setNote(const QString &path);
    void refresh();

signals:
    void noteActivated(const QString &relativePath);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void onIndexChanged();
    Index *m_index = nullptr;
    bool m_stale = false; ///< Index::changed arrived while hidden
    QString m_path;
    QColor m_faintColor;
    QLabel *m_header = nullptr;
    QTreeWidget *m_list = nullptr;
};

} // namespace noteAppC
