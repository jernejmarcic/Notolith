// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

#include "MarkdownEditor.h"

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace noteAppC {

/// The choices Obsidian's Export to PDF offers: the file name as a title,
/// paper size, orientation, margin and downscale.
class ExportPdfDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExportPdfDialog(QWidget *parent = nullptr);
    MarkdownEditor::PdfOptions options() const;

private:
    QCheckBox *m_title = nullptr;
    QComboBox *m_size = nullptr;
    QComboBox *m_orientation = nullptr;
    QComboBox *m_margin = nullptr;
    QSpinBox *m_downscale = nullptr;
};

} // namespace noteAppC
