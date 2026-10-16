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
    /// Obsidian's "find or create": Enter with no match, or Shift+Enter with
    /// any, emits createRequested(typed text). Off by default - the command
    /// palette and the pickers built on this class have nothing to create.
    void setCreateAllowed(bool allowed) { m_createAllowed = allowed; }

    /// Shows centred on the parent with an empty query.
    void popUp();

signals:
    void accepted(const QString &candidate);
    void createRequested(const QString &name);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refill();
    void chooseCurrent();
    void requestCreate();

    QLineEdit *m_query = nullptr;
    QListWidget *m_results = nullptr;
    QStringList m_candidates;
    QStringList m_details;
    bool m_createAllowed = false;
};

} // namespace noteAppC
