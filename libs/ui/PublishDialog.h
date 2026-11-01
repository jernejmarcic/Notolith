// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QDialog>

class QButtonGroup;
class QComboBox;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace noteAppC {

/// Everything a publish asks before it runs, in one dialog: where the site
/// goes, its name, which note is its home page, which notes go on it, and
/// light, dark or both. Plain values, so the window stores them per vault.
class PublishDialog : public QDialog {
    Q_OBJECT

public:
    struct Choices {
        QString folder;
        QString siteName;
        QString homeNote;       ///< vault-relative; empty is a generated list of notes
        bool onlyMarked = true; ///< Obsidian Publish shares nothing until it is picked
        int theme = 2;          ///< 0 dark, 1 light, 2 both (the reader's own preference)
    };

    /// `notes` are the vault's note paths, relative, offered for the home page.
    PublishDialog(const Choices &remembered, const QStringList &notes, QWidget *parent = nullptr);
    Choices choices() const;

private:
    void updateAccept();

    QLineEdit *m_folder = nullptr;
    QLineEdit *m_siteName = nullptr;
    QLineEdit *m_homeNote = nullptr;
    QRadioButton *m_onlyMarked = nullptr;
    QRadioButton *m_everyNote = nullptr;
    QComboBox *m_theme = nullptr;
    QPushButton *m_publish = nullptr;
    QStringList m_notes;
};

} // namespace noteAppC
