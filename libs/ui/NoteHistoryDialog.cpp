// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "NoteHistoryDialog.h"

#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "RecoveryDialog.h"

namespace noteAppC {

NoteHistoryDialog::NoteHistoryDialog(const QString &noteName, const QList<Version> &versions,
                                     std::function<QString(const QString &)> read, const QString &current,
                                     QWidget *parent)
    : QDialog(parent)
    , m_read(std::move(read))
    , m_current(current)
{
    setObjectName(QStringLiteral("noteHistoryDialog"));
    setWindowTitle(tr("History of %1").arg(noteName));

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("noteHistoryVersions"));
    for (const Version &version : versions) {
        auto *item = new QListWidgetItem(QLocale().toString(version.time, QLocale::ShortFormat), m_list);
        item->setData(Qt::UserRole, version.id);
    }
    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("noteHistoryPreview"));
    m_preview->setReadOnly(true);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_changes = new QCheckBox(tr("Show changes"), this);
    m_changes->setObjectName(QStringLiteral("noteHistoryShowChanges"));
    m_copy = new QPushButton(tr("Copy to clipboard"), this);
    m_restore = new QPushButton(tr("Restore"), this);
    m_restore->setObjectName(QStringLiteral("noteHistoryRestore"));
    auto *close = new QPushButton(tr("Close"), this);

    auto *split = new QSplitter(this);
    split->addWidget(m_list);
    split->addWidget(m_preview);
    split->setStretchFactor(1, 1);
    split->setSizes({200, 560});
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_changes);
    buttons->addStretch(1);
    buttons->addWidget(m_copy);
    buttons->addWidget(m_restore);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(split, 1);
    layout->addLayout(buttons);
    resize(820, 560);

    connect(m_list, &QListWidget::currentRowChanged, this, &NoteHistoryDialog::showVersion);
    connect(m_changes, &QCheckBox::toggled, this, &NoteHistoryDialog::showVersion);
    connect(m_copy, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(m_shown); });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Restore"),
                                  tr("Replace the note with this version? What it holds now is "
                                     "kept in its history too."))
            == QMessageBox::Yes)
            emit restoreRequested(m_shown);
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    else
        m_preview->setPlainText(tr("No saved versions yet."));
    m_copy->setEnabled(m_list->count() > 0);
    m_restore->setEnabled(m_list->count() > 0);
}

void NoteHistoryDialog::showVersion()
{
    const QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    m_shown = m_read(item->data(Qt::UserRole).toString());
    if (m_changes->isChecked())
        RecoveryDialog::showDiff(m_preview, m_shown, m_current);
    else
        m_preview->setPlainText(m_shown);
}

} // namespace noteAppC
