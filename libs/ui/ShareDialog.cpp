#include "ShareDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ShareEngine.h"

namespace noteAppC {

ShareDialog::ShareDialog(ShareEngine *engine, QByteArray shareID, const QString &inviteCode, QWidget *parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_id(std::move(shareID))
{
    setObjectName(QStringLiteral("shareDialog"));
    setWindowTitle(tr("Share"));
    for (const ShareRecord &record : engine->shares())
        if (record.shareID == m_id)
            m_owner = record.owned;

    auto *layout = new QVBoxLayout(this);
    if (m_owner) {
        auto *hint = new QLabel(tr("Send this code to the people who should edit the note with you. Each device "
                                   "still has to be approved here, and the code expires."),
                                this);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        m_code = new QPlainTextEdit(inviteCode, this);
        m_code->setObjectName(QStringLiteral("shareInviteCode"));
        m_code->setReadOnly(true);
        m_code->setPlaceholderText(tr("No code shown: make a new one to invite someone"));
        m_code->setMaximumHeight(m_code->fontMetrics().lineSpacing() * 5);
        layout->addWidget(m_code);
        auto *row = new QHBoxLayout;
        m_copy = new QPushButton(tr("Copy"), this);
        m_copy->setObjectName(QStringLiteral("shareCopy"));
        auto *fresh = new QPushButton(tr("New invite"), this);
        fresh->setObjectName(QStringLiteral("shareNewInvite"));
        fresh->setToolTip(tr("A new code; the previous one stops working"));
        row->addWidget(m_copy);
        row->addWidget(fresh);
        row->addStretch(1);
        layout->addLayout(row);
        connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_code->toPlainText()); });
        connect(fresh, &QPushButton::clicked, this, [this] {
            if (!m_engine)
                return;
            m_code->setPlainText(m_engine->invite(m_id));
            m_copy->setEnabled(!m_code->toPlainText().isEmpty());
        });
        m_copy->setEnabled(!inviteCode.isEmpty());
    }

    auto *you = new QLabel(tr("This device: %1").arg(engine->fingerprint()), this);
    you->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(you);
    m_members = new QTreeWidget(this);
    m_members->setObjectName(QStringLiteral("shareMembers"));
    m_members->setRootIsDecorated(false);
    m_members->setHeaderLabels({tr("Device"), tr("Fingerprint"), tr("Safety number"), tr("Status")});
    m_members->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_members->setToolTip(tr("Read the safety number to each other: both devices show the same one"));
    layout->addWidget(m_members, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (m_owner) {
        m_revoke = buttons->addButton(tr("Remove device"), QDialogButtonBox::ActionRole);
        m_revoke->setObjectName(QStringLiteral("shareRevoke"));
        m_revoke->setEnabled(false);
        connect(m_revoke, &QPushButton::clicked, this, &ShareDialog::revokeSelected);
        connect(m_members, &QTreeWidget::itemSelectionChanged, this, [this] {
            const QTreeWidgetItem *item = m_members->currentItem();
            m_revoke->setEnabled(m_engine && item && item->data(0, Qt::UserRole).toByteArray() != m_engine->publicKey());
        });
    }
    QPushButton *stop = buttons->addButton(tr("Stop sharing here"), QDialogButtonBox::DestructiveRole);
    stop->setObjectName(QStringLiteral("shareStop"));
    stop->setToolTip(tr("This device stops following the note; the others keep it"));
    connect(stop, &QPushButton::clicked, this, [this] { done(StopSharing); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(engine, &ShareEngine::membersChanged, this, [this](const QByteArray &id) {
        if (id == m_id)
            refresh();
    });
    connect(engine, &ShareEngine::peersChanged, this, [this](const QByteArray &id) {
        if (id == m_id)
            refresh();
    });
    connect(engine, &QObject::destroyed, this, &QDialog::reject);
    refresh();
    resize(640, 420);
}

void ShareDialog::setConfirmForTests(std::function<bool(const QString &)> confirm)
{
    m_confirm = std::move(confirm);
}

QString ShareDialog::inviteCode() const
{
    return m_code ? m_code->toPlainText() : QString();
}

void ShareDialog::refresh()
{
    if (!m_engine)
        return;
    const QByteArray selected = m_members->currentItem() ? m_members->currentItem()->data(0, Qt::UserRole).toByteArray()
                                                         : QByteArray();
    m_members->clear();
    QByteArray owner;
    for (const ShareRecord &record : m_engine->shares())
        if (record.shareID == m_id)
            owner = record.owner;
    for (const auto &[key, name] : m_engine->members(m_id)) {
        const bool me = key == m_engine->publicKey();
        QString status = me ? tr("this device") : m_engine->isConnected(key) ? tr("here") : tr("away");
        if (key == owner)
            status += tr(", owner");
        auto *item = new QTreeWidgetItem(m_members, {name, ShareEngine::fingerprintOf(key),
                                                     me ? QString() : m_engine->safetyNumberWith(key), status});
        item->setData(0, Qt::UserRole, key);
        if (key == selected)
            m_members->setCurrentItem(item);
    }
}

void ShareDialog::revokeSelected()
{
    const QTreeWidgetItem *item = m_members->currentItem();
    if (!item)
        return;
    const QByteArray key = item->data(0, Qt::UserRole).toByteArray();
    const QString name = item->text(0);
    bool sure = false;
    if (m_confirm) {
        sure = m_confirm(name);
    } else {
        QMessageBox ask(QMessageBox::Warning, tr("Remove device"),
                        tr("Remove %1 from this note? It keeps what it already has, and gets nothing new.").arg(name),
                        QMessageBox::NoButton, this);
        ask.setTextFormat(Qt::PlainText); // the name comes from the other device
        QPushButton *remove = ask.addButton(tr("Remove"), QMessageBox::DestructiveRole);
        ask.setDefaultButton(ask.addButton(QMessageBox::Cancel));
        ask.exec();
        sure = ask.clickedButton() == remove;
    }
    if (sure && m_engine)
        m_engine->revoke(m_id, key);
}

JoinShareDialog::JoinShareDialog(std::function<bool(const QString &, QString *)> join, QWidget *parent)
    : QDialog(parent)
    , m_join(std::move(join))
{
    setObjectName(QStringLiteral("joinShareDialog"));
    setWindowTitle(tr("Join a shared note"));
    auto *layout = new QVBoxLayout(this);
    auto *label = new QLabel(tr("Paste the invitation code you were sent:"), this);
    layout->addWidget(label);
    m_code = new QLineEdit(this);
    m_code->setObjectName(QStringLiteral("joinCode"));
    m_code->setPlaceholderText(QStringLiteral("notolith-share:..."));
    layout->addWidget(m_code);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("joinError"));
    m_error->setWordWrap(true);
    QFont bold = m_error->font();
    bold.setBold(true);
    m_error->setFont(bold);
    m_error->hide();
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_ok = buttons->addButton(tr("Join"), QDialogButtonBox::AcceptRole);
    m_ok->setObjectName(QStringLiteral("joinOK"));
    m_ok->setDefault(true);
    m_ok->setEnabled(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_ok, &QPushButton::clicked, this, [this] {
        QString why;
        if (m_join(m_code->text().trimmed(), &why)) {
            accept();
            return;
        }
        m_error->setText(why);
        m_error->show();
    });
    connect(m_code, &QLineEdit::textChanged, this, &JoinShareDialog::check);
    layout->addWidget(buttons);
    resize(520, sizeHint().height());
}

QString JoinShareDialog::error() const
{
    return m_error->isVisible() ? m_error->text() : QString();
}

void JoinShareDialog::check()
{
    const QString code = m_code->text().trimmed();
    QString why;
    const bool ok = !code.isEmpty() && ShareEngine::parseInvite(code, &why).has_value();
    m_ok->setEnabled(ok);
    m_error->setText(code.isEmpty() ? QString() : why);
    m_error->setVisible(!ok && !code.isEmpty());
}

} // namespace noteAppC
