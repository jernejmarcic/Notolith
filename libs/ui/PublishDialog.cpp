#include "PublishDialog.h"

#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace noteAppC {

PublishDialog::PublishDialog(const Choices &remembered, const QStringList &notes, QWidget *parent)
    : QDialog(parent)
    , m_notes(notes)
{
    setObjectName(QStringLiteral("publishDialog"));
    setWindowTitle(tr("Publish"));

    m_folder = new QLineEdit(remembered.folder, this);
    m_folder->setObjectName(QStringLiteral("publishFolder"));
    m_folder->setPlaceholderText(tr("A folder outside the vault, or a subfolder of it"));
    auto *browse = new QPushButton(tr("Browse..."), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Publish to folder"), m_folder->text());
        if (!folder.isEmpty())
            m_folder->setText(folder);
    });
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);

    m_siteName = new QLineEdit(remembered.siteName, this);
    m_siteName->setObjectName(QStringLiteral("publishSiteName"));

    m_homeNote = new QLineEdit(remembered.homeNote, this);
    m_homeNote->setObjectName(QStringLiteral("publishHomeNote"));
    m_homeNote->setPlaceholderText(tr("None: a list of every published note"));
    // Completion over the vault's notes; 20 000 paths is a small model.
    auto *completer = new QCompleter(notes, m_homeNote);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    m_homeNote->setCompleter(completer);

    m_onlyMarked = new QRadioButton(tr("Only notes with publish: true"), this);
    m_everyNote = new QRadioButton(tr("Every note, except publish: false"), this);
    (remembered.onlyMarked ? m_onlyMarked : m_everyNote)->setChecked(true);
    auto *which = new QVBoxLayout;
    which->addWidget(m_onlyMarked);
    which->addWidget(m_everyNote);

    m_theme = new QComboBox(this);
    m_theme->addItem(tr("Dark"));
    m_theme->addItem(tr("Light"));
    m_theme->addItem(tr("Both, as the reader's system prefers"));
    m_theme->setCurrentIndex(qBound(0, remembered.theme, 2));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_publish = buttons->addButton(tr("Publish"), QDialogButtonBox::AcceptRole);
    m_publish->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_folder, &QLineEdit::textChanged, this, &PublishDialog::updateAccept);
    connect(m_homeNote, &QLineEdit::textChanged, this, &PublishDialog::updateAccept);

    auto *layout = new QFormLayout(this);
    layout->addRow(tr("Folder"), folderRow);
    layout->addRow(tr("Site name"), m_siteName);
    layout->addRow(tr("Home page"), m_homeNote);
    layout->addRow(tr("Notes"), which);
    layout->addRow(tr("Theme"), m_theme);
    layout->addRow(buttons);
    updateAccept();
}

void PublishDialog::updateAccept()
{
    // A home page that is not a note would publish a site with no index.
    const QString home = m_homeNote->text().trimmed();
    m_publish->setEnabled(!m_folder->text().trimmed().isEmpty()
                          && (home.isEmpty() || m_notes.contains(home)));
}

PublishDialog::Choices PublishDialog::choices() const
{
    Choices choices;
    choices.folder = m_folder->text().trimmed();
    choices.siteName = m_siteName->text().trimmed();
    choices.homeNote = m_homeNote->text().trimmed();
    choices.onlyMarked = m_onlyMarked->isChecked();
    choices.theme = m_theme->currentIndex();
    return choices;
}

} // namespace noteAppC
