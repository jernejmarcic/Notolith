// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "ExportPdfDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSpinBox>

namespace noteAppC {

ExportPdfDialog::ExportPdfDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("exportPdfDialog"));
    setWindowTitle(tr("Export to PDF"));

    m_title = new QCheckBox(this);
    m_title->setChecked(true);
    m_size = new QComboBox(this);
    const QList<QPair<QString, QPageSize::PageSizeId>> sizes = {
        {QStringLiteral("A3"), QPageSize::A3},       {QStringLiteral("A4"), QPageSize::A4},
        {QStringLiteral("A5"), QPageSize::A5},       {tr("Legal"), QPageSize::Legal},
        {tr("Letter"), QPageSize::Letter},           {tr("Tabloid"), QPageSize::Tabloid},
    };
    for (const auto &[name, id] : sizes)
        m_size->addItem(name, int(id));
    m_size->setCurrentIndex(1);
    m_orientation = new QComboBox(this);
    m_orientation->addItem(tr("Portrait"), int(QPageLayout::Portrait));
    m_orientation->addItem(tr("Landscape"), int(QPageLayout::Landscape));
    m_margin = new QComboBox(this);
    m_margin->addItem(tr("Default"), 25.4);
    m_margin->addItem(tr("Small"), 12.7);
    m_margin->addItem(tr("None"), 0.0);
    m_downscale = new QSpinBox(this);
    m_downscale->setRange(10, 100);
    m_downscale->setValue(100);
    m_downscale->setSuffix(QStringLiteral(" %"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(tr("Export to PDF"), QDialogButtonBox::AcceptRole)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QFormLayout(this);
    layout->addRow(tr("Include file name as title"), m_title);
    layout->addRow(tr("Page size"), m_size);
    layout->addRow(tr("Orientation"), m_orientation);
    layout->addRow(tr("Margin"), m_margin);
    layout->addRow(tr("Downscale"), m_downscale);
    layout->addRow(buttons);
}

MarkdownEditor::PdfOptions ExportPdfDialog::options() const
{
    MarkdownEditor::PdfOptions options;
    options.includeTitle = m_title->isChecked();
    options.pageSize = QPageSize(QPageSize::PageSizeId(m_size->currentData().toInt()));
    options.orientation = QPageLayout::Orientation(m_orientation->currentData().toInt());
    options.marginMillimetres = m_margin->currentData().toDouble();
    options.downscalePercent = m_downscale->value();
    return options;
}

} // namespace noteAppC
