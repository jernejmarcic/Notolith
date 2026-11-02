// SPDX-License-Identifier: GPL-3.0-or-later
#include "PasswordDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

namespace noteAppC {

PasswordDialog::PasswordDialog(bool creating, bool retry, QWidget *parent)
    : QDialog(parent)
    , m_creating(creating)
{
    setObjectName(QStringLiteral("passwordDialog"));
    setWindowTitle(creating ? tr("Set the password for encrypted notes") : tr("Unlock encrypted notes"));

    auto *layout = new QFormLayout(this);
    auto *intro = new QLabel(creating ? tr("Encrypted notes open with this password. It is not stored "
                                           "anywhere: forget it and they cannot be opened again.")
                             : retry ? tr("That password did not open them. Try again.")
                                     : tr("Enter the password for this vault's encrypted notes."),
                             this);
    intro->setWordWrap(true);
    layout->addRow(intro);

    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("password"));
    m_password->setEchoMode(QLineEdit::Password);
    layout->addRow(tr("Password"), m_password);
    if (creating) {
        m_repeat = new QLineEdit(this);
        m_repeat->setObjectName(QStringLiteral("passwordRepeat"));
        m_repeat->setEchoMode(QLineEdit::Password);
        layout->addRow(tr("Again"), m_repeat);
        connect(m_repeat, &QLineEdit::textChanged, this, &PasswordDialog::updateAccept);
    }
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addRow(m_hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_ok = buttons->addButton(creating ? tr("Set password") : tr("Unlock"), QDialogButtonBox::AcceptRole);
    m_ok->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_password, &QLineEdit::textChanged, this, &PasswordDialog::updateAccept);
    layout->addRow(buttons);
    updateAccept();
}

PasswordDialog::~PasswordDialog()
{
    // QLineEdit keeps its text in a QString; overwrite it before it is freed.
    for (QLineEdit *field : {m_password, m_repeat}) {
        if (field)
            field->setText(QString(field->text().size(), QLatin1Char(' ')));
    }
}

void PasswordDialog::updateAccept()
{
    const QString password = m_password->text();
    bool ok = !password.isEmpty();
    QString hint;
    if (m_creating) {
        if (password.size() < kMinimumLength) {
            ok = false;
            hint = tr("At least %1 characters; a few unrelated words are better than one clever one.")
                       .arg(kMinimumLength);
        } else if (m_repeat->text() != password) {
            ok = false;
            hint = m_repeat->text().isEmpty() ? QString() : tr("The two do not match.");
        }
    }
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
    m_ok->setEnabled(ok);
}

QString PasswordDialog::password() const
{
    return m_password->text();
}

QString PasswordDialog::ask(QWidget *parent, bool creating, bool retry)
{
    PasswordDialog dialog(creating, retry, parent);
    return dialog.exec() == QDialog::Accepted ? dialog.password() : QString();
}

} // namespace noteAppC
