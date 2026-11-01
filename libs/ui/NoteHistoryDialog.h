// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QDateTime>
#include <QDialog>

#include <functional>

class QCheckBox;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace noteAppC {

/// One note's saved versions, newest first: read one, see what it changed
/// against the note now, copy it, or put it back. Knows nothing of where the
/// versions come from - the git history module hands it their ids and a way
/// to read one - so libs/ui never links libgit2.
class NoteHistoryDialog : public QDialog {
    Q_OBJECT

public:
    struct Version {
        QString id;
        QDateTime time;
    };

    NoteHistoryDialog(const QString &noteName, const QList<Version> &versions,
                      std::function<QString(const QString &id)> read, const QString &current,
                      QWidget *parent = nullptr);

signals:
    void restoreRequested(const QString &text);

private:
    void showVersion();

    std::function<QString(const QString &)> m_read;
    QString m_current;
    QString m_shown; ///< the selected version's own text, whatever the preview shows
    QListWidget *m_list = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QCheckBox *m_changes = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_restore = nullptr;
};

} // namespace noteAppC
