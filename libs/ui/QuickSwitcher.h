#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;

namespace noteAppC {

/// Obsidian's quick switcher: type, see the best matches, Enter to open.
/// One widget, several data sources - the `[[` suggester and the Phase 4
/// command palette reuse it.
class QuickSwitcher : public QDialog {
    Q_OBJECT

public:
    explicit QuickSwitcher(QWidget *parent = nullptr);

    /// `candidates` are ranked by Fuzzy::score as the query changes.
    void setCandidates(const QStringList &candidates);
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
};

} // namespace noteAppC
