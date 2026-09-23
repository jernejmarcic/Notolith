#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;

namespace noteAppC {

/// Obsidian's quick switcher: type, see the best matches, Enter to open.
/// The command palette is the same widget over command names.
class QuickSwitcher : public QDialog {
    Q_OBJECT

public:
    explicit QuickSwitcher(QWidget *parent = nullptr);

    /// `candidates` are ranked by Fuzzy::score as the query changes; with no
    /// query they keep the order given. `details`, when given, is one muted
    /// right-aligned text per candidate (the palette's hotkeys).
    void setCandidates(const QStringList &candidates, const QStringList &details = {});
    void setPlaceholder(const QString &text);

    /// Shows centred on the parent with an empty query.
    void popUp();

signals:
    void accepted(const QString &candidate);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refill();
    void chooseCurrent();

    QLineEdit *m_query = nullptr;
    QListWidget *m_results = nullptr;
    QStringList m_candidates;
    QStringList m_details;
};

} // namespace noteAppC
