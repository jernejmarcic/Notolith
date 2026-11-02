// SPDX-License-Identifier: GPL-3.0-or-later
#include "RecoveryDialog.h"

#include <vector>

#include <QCheckBox>
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
#include <QTextCharFormat>
#include <QTextCursor>
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

    m_changes = new QCheckBox(tr("Show changes"), this);
    m_changes->setObjectName(QStringLiteral("recoveryShowChanges"));
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
    buttons->addWidget(m_changes);
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
            [this] { QGuiApplication::clipboard()->setText(m_snapshot); });
    connect(m_changes, &QCheckBox::toggled, this, &RecoveryDialog::showPreview);
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
        emit restoreRequested(path, m_snapshot);
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
        m_snapshot.clear();
        m_preview->clear();
        m_copy->setEnabled(false);
        m_restore->setEnabled(false);
        return;
    }
    Snapshots::Version version;
    version.notePath = note->data(kPathRole).toString();
    version.time = item->data(kPathRole).toLongLong();
    version.file = item->data(kFileRole).toString();
    m_snapshot = Snapshots::read(version);
    m_copy->setEnabled(true);
    m_restore->setEnabled(true);
    showPreview();
}

void RecoveryDialog::showPreview()
{
    const QListWidgetItem *note = m_notes->currentItem();
    if (!m_changes->isChecked() || !note) {
        m_preview->setPlainText(m_snapshot);
        return;
    }
    Note current;
    Vault::read(note->data(kPathRole).toString(), current); // a deleted note diffs against nothing
    showDiff(m_preview, m_snapshot, current.text);
}

void RecoveryDialog::showDiff(QPlainTextEdit *view, const QString &before, const QString &after)
{
    const QList<DiffLine> diff = lineDiff(before.split(QLatin1Char('\n')), after.split(QLatin1Char('\n')));
    view->clear();
    QTextCursor cursor(view->document());
    QTextCharFormat same;
    QTextCharFormat removed;
    removed.setBackground(QColor(220, 60, 60, 70));
    QTextCharFormat added;
    added.setBackground(QColor(60, 180, 90, 70));
    bool first = true;
    for (const DiffLine &line : diff) {
        if (!first)
            cursor.insertText(QStringLiteral("\n"), same);
        first = false;
        switch (line.kind) {
        case DiffLine::Kind::Same: cursor.insertText(QStringLiteral("  ") + line.text, same); break;
        case DiffLine::Kind::Removed: cursor.insertText(QStringLiteral("- ") + line.text, removed); break;
        case DiffLine::Kind::Added: cursor.insertText(QStringLiteral("+ ") + line.text, added); break;
        }
    }
    view->moveCursor(QTextCursor::Start);
}

QList<RecoveryDialog::DiffLine> RecoveryDialog::lineDiff(const QStringList &before, const QStringList &after)
{
    // The common head and tail first: a snapshot differs from its note in a
    // few places, so what is left in the middle is small.
    qsizetype head = 0;
    while (head < before.size() && head < after.size() && before.at(head) == after.at(head))
        ++head;
    qsizetype tail = 0;
    while (tail < before.size() - head && tail < after.size() - head
           && before.at(before.size() - 1 - tail) == after.at(after.size() - 1 - tail))
        ++tail;
    const QStringList a = before.mid(head, before.size() - head - tail);
    const QStringList b = after.mid(head, after.size() - head - tail);

    QList<DiffLine> out;
    for (qsizetype i = 0; i < head; ++i)
        out.append({DiffLine::Kind::Same, before.at(i)});
    // Limit: an LCS table over the changed middle, capped at four million
    // cells; past that the middle shows as removed then added. Myers' O(ND)
    // when snapshots that different turn out to matter.
    constexpr qsizetype kCap = 4'000'000;
    if (!a.isEmpty() && !b.isEmpty() && (a.size() + 1) * (b.size() + 1) <= kCap) {
        const qsizetype rows = a.size() + 1;
        const qsizetype cols = b.size() + 1;
        std::vector<quint32> lcs(size_t(rows * cols), 0);
        for (qsizetype i = a.size() - 1; i >= 0; --i) {
            for (qsizetype j = b.size() - 1; j >= 0; --j) {
                lcs[size_t(i * cols + j)] = a.at(i) == b.at(j)
                                                ? lcs[size_t((i + 1) * cols + j + 1)] + 1
                                                : std::max(lcs[size_t((i + 1) * cols + j)], lcs[size_t(i * cols + j + 1)]);
            }
        }
        qsizetype i = 0;
        qsizetype j = 0;
        while (i < a.size() && j < b.size()) {
            if (a.at(i) == b.at(j)) {
                out.append({DiffLine::Kind::Same, a.at(i)});
                ++i;
                ++j;
            } else if (lcs[size_t((i + 1) * cols + j)] >= lcs[size_t(i * cols + j + 1)]) {
                out.append({DiffLine::Kind::Removed, a.at(i++)});
            } else {
                out.append({DiffLine::Kind::Added, b.at(j++)});
            }
        }
        for (; i < a.size(); ++i)
            out.append({DiffLine::Kind::Removed, a.at(i)});
        for (; j < b.size(); ++j)
            out.append({DiffLine::Kind::Added, b.at(j)});
    } else {
        for (const QString &line : a)
            out.append({DiffLine::Kind::Removed, line});
        for (const QString &line : b)
            out.append({DiffLine::Kind::Added, line});
    }
    for (qsizetype i = before.size() - tail; i < before.size(); ++i)
        out.append({DiffLine::Kind::Same, before.at(i)});
    return out;
}

} // namespace noteAppC
