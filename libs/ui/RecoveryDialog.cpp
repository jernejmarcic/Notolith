#include "RecoveryDialog.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "Snapshots.h"
#include "Vault.h"

namespace noteAppC {

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kFileRole = Qt::UserRole + 1;

} // namespace

RecoveryDialog::RecoveryDialog(const Vault &vault, QWidget *parent)
    : QDialog(parent)
    , m_root(vault.isValid() ? vault.root() : QString())
{
    setObjectName(QStringLiteral("recoveryDialog"));
    setWindowTitle(tr("File recovery"));
    resize(980, 600);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Find a note..."));
    m_filter->setClearButtonEnabled(true);
    m_notes = new QListWidget(this);
    m_notes->setObjectName(QStringLiteral("recoveryNotes"));
    m_versions = new QListWidget(this);
    m_versions->setObjectName(QStringLiteral("recoveryVersions"));
    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("recoveryPreview"));
    m_preview->setReadOnly(true);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_copy = new QPushButton(tr("Copy to clipboard"), this);
    m_restore = new QPushButton(tr("Restore"), this);
    auto *close = new QPushButton(tr("Close"), this);
    m_copy->setEnabled(false);
    m_restore->setEnabled(false);

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_filter);
    leftLayout->addWidget(m_notes, 1);

    auto *split = new QSplitter(this);
    split->addWidget(left);
    split->addWidget(m_versions);
    split->addWidget(m_preview);
    split->setStretchFactor(2, 1);
    split->setSizes({260, 200, 520});

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_copy);
    buttons->addWidget(m_restore);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(split, 1);
    layout->addLayout(buttons);

    connect(m_filter, &QLineEdit::textChanged, this, &RecoveryDialog::showNotes);
    connect(m_notes, &QListWidget::currentRowChanged, this, &RecoveryDialog::showVersions);
    connect(m_versions, &QListWidget::currentRowChanged, this, &RecoveryDialog::showVersion);
    connect(m_copy, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(m_preview->toPlainText()); });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        const QListWidgetItem *note = m_notes->currentItem();
        if (!note)
            return;
        const QString path = note->data(kPathRole).toString();
        if (QMessageBox::question(this, tr("Restore"),
                                  tr("Replace the current content of %1 with this snapshot? What "
                                     "it holds now is kept as a snapshot too.")
                                      .arg(note->text()))
            != QMessageBox::Yes)
            return;
        emit restoreRequested(path, m_preview->toPlainText());
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    showNotes();
}

void RecoveryDialog::showNotes()
{
    const QString needle = m_filter->text().trimmed();
    m_notes->clear();
    for (const QString &path : Snapshots::notes()) {
        const QString shown = !m_root.isEmpty() && path.startsWith(m_root + QLatin1Char('/'))
                                  ? QDir(m_root).relativeFilePath(path)
                                  : path;
        if (!needle.isEmpty() && !shown.contains(needle, Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(shown, m_notes);
        item->setData(kPathRole, path);
    }
    if (m_notes->count() > 0)
        m_notes->setCurrentRow(0);
    else
        showVersions();
}

void RecoveryDialog::showVersions()
{
    m_versions->clear();
    const QListWidgetItem *note = m_notes->currentItem();
    if (note) {
        for (const Snapshots::Version &version : Snapshots::versions(note->data(kPathRole).toString())) {
            auto *item = new QListWidgetItem(
                QLocale().toString(QDateTime::fromMSecsSinceEpoch(version.time), QLocale::ShortFormat),
                m_versions);
            item->setData(kFileRole, version.file);
            item->setData(kPathRole, version.time);
        }
    }
    if (m_versions->count() > 0)
        m_versions->setCurrentRow(0);
    else
        showVersion();
}

void RecoveryDialog::showVersion()
{
    const QListWidgetItem *item = m_versions->currentItem();
    const QListWidgetItem *note = m_notes->currentItem();
    if (!item || !note) {
        m_preview->clear();
        m_copy->setEnabled(false);
        m_restore->setEnabled(false);
        return;
    }
    Snapshots::Version version;
    version.notePath = note->data(kPathRole).toString();
    version.time = item->data(kPathRole).toLongLong();
    version.file = item->data(kFileRole).toString();
    m_preview->setPlainText(Snapshots::read(version));
    m_copy->setEnabled(true);
    m_restore->setEnabled(true);
}

} // namespace noteAppC
