#pragma once

#include <functional>

#include <QByteArray>
#include <QDialog>
#include <QPointer>

#include "ShareEngine.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace noteAppC {

/// Who has a shared note and how to let someone in: the invitation code with
/// Copy and New invite (the owner's), every member with their fingerprint, the
/// safety number both devices show, and whether they are connected; Revoke
/// (the owner's) and Stop sharing here. Follows the engine while it is open.
class ShareDialog : public QDialog {
    Q_OBJECT

public:
    /// Done with this code: the window stops sharing the note on this device.
    static constexpr int StopSharing = 2;

    /// `inviteCode` is shown when there is one to show (just created); the
    /// owner can always mint a new one.
    ShareDialog(ShareEngine *engine, QByteArray shareID, const QString &inviteCode, QWidget *parent = nullptr);

    /// Who answers "remove this device?", in place of the question; for the tests.
    void setConfirmForTests(std::function<bool(const QString &name)> confirm);
    QString inviteCode() const;

private:
    void refresh();
    void revokeSelected();

    QPointer<ShareEngine> m_engine; ///< the dialog closes when the engine goes (sharing switched off)
    QByteArray m_id;
    bool m_owner = false;
    QPlainTextEdit *m_code = nullptr;
    QPushButton *m_copy = nullptr;
    QTreeWidget *m_members = nullptr;
    QPushButton *m_revoke = nullptr;
    std::function<bool(const QString &)> m_confirm;
};

/// The invitation code, checked as it is typed: what is wrong with it shows
/// under the field, and Join is only offered for a code that parses. `join`
/// is the window's; its error, if any, shows the same way and the dialog stays.
class JoinShareDialog : public QDialog {
    Q_OBJECT

public:
    explicit JoinShareDialog(std::function<bool(const QString &code, QString *error)> join, QWidget *parent = nullptr);
    QString error() const;

private:
    void check();

    std::function<bool(const QString &, QString *)> m_join;
    QLineEdit *m_code = nullptr;
    QLabel *m_error = nullptr;
    QPushButton *m_ok = nullptr;
};

} // namespace noteAppC
