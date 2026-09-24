#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace noteAppC {

/// The password for encrypted notes: once to unlock them, twice when it is
/// being set. Nothing typed here is kept by the dialog once it closes.
class PasswordDialog : public QDialog {
    Q_OBJECT

public:
    /// `creating`: a new password, typed twice. `retry`: the last one was wrong.
    PasswordDialog(bool creating, bool retry, QWidget *parent = nullptr);
    ~PasswordDialog() override;
    QString password() const;

    /// The dialog run modally; empty when cancelled.
    static QString ask(QWidget *parent, bool creating, bool retry);

    static constexpr int kMinimumLength = 8;

private:
    void updateAccept();

    bool m_creating = false;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_repeat = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_ok = nullptr;
};

} // namespace noteAppC
