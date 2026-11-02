// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace noteAppC {

class Vault;

/// Obsidian's file recovery window: find a note, pick one of its snapshots,
/// read it, then copy it out or put it back.
class RecoveryDialog : public QDialog {
    Q_OBJECT

public:
    /// `vault` only names notes relative to it; snapshots of notes elsewhere
    /// are listed by their full path.
    RecoveryDialog(const Vault &vault, QWidget *parent = nullptr);

    /// One line of a line diff: in both, only in the snapshot, or only in
    /// the note as it is now.
    struct DiffLine {
        enum class Kind { Same, Removed, Added };
        Kind kind;
        QString text;
    };
    /// `before` to `after`, line by line.
    static QList<DiffLine> lineDiff(const QStringList &before, const QStringList &after);
    /// Draws `before` to `after` into `view`: kept lines plain, removed ones
    /// on red, added ones on green. Shared with the note history dialog.
    static void showDiff(QPlainTextEdit *view, const QString &before, const QString &after);

signals:
    /// Replace `notePath`'s content with `text`: the window writes it.
    void restoreRequested(const QString &notePath, const QString &text);

private:
    void showNotes();
    void showVersions();
    void showVersion();
    /// The snapshot, or with "Show changes" its difference from the note now.
    void showPreview();

    QString m_root;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_notes = nullptr;
    QListWidget *m_versions = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_restore = nullptr;
    QCheckBox *m_changes = nullptr;
    QString m_snapshot; ///< what Copy and Restore use, whatever the preview shows
};

} // namespace noteAppC
