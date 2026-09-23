#pragma once

#include "BaseFile.h"

#include <QWidget>

#include <memory>
#include <optional>

class QComboBox;
class QGridLayout;
class QLabel;
class QScrollArea;
class QStackedWidget;
class QTableView;

namespace noteAppC {
class Index;
class Vault;
}

namespace noteAppC::bases {

class BaseQuery;
class BaseTableModel;

/// Renders a `.base` file: a view switcher over its `views:`, a table (a
/// `BaseTableModel` in a `QTableView`, sortable by clicking a header, a
/// `file.name` cell that opens its note) and cards (a scrolling grid, an
/// optional cover image, read-only - see the module report for why cards
/// does not share the table's cell editor).
///
/// Takes no ownership of `Index`/`Vault`, the way `BacklinksPane` takes an
/// `Index*` - MainWindow is expected to own both for the life of the window
/// and hand this widget pointers into them, the same shape every other pane
/// in `libs/ui` already uses.
class BaseView : public QWidget {
    Q_OBJECT

public:
    explicit BaseView(QWidget *parent = nullptr);
    ~BaseView() override;

    void setIndex(Index *index);
    void setVault(const Vault *vault);

    /// Parses `yamlText` (already read from `absolutePath` - this module
    /// does no file I/O of its own beyond a cell edit's own note, see
    /// BaseTableModel) and opens its first view. False with lastError() set
    /// on a YAML or schema error.
    bool open(const QString &absolutePath, const QString &yamlText);
    void clear();
    QString lastError() const { return m_lastError; }
    /// Problems seen evaluating the current view's filters/sort/formulas -
    /// each shown once, however many rows hit it; empty when nothing did.
    QStringList warnings() const;

signals:
    /// The note at `relativePath` should be opened in the editor - a
    /// double-clicked row, or the `file.name` cell.
    void noteActivated(const QString &relativePath);

private slots:
    void onViewSwitched(int index);
    void onTableActivated(const QModelIndex &index);
    void onCellEdited(const QString &relativePath);

private:
    void rebuildViewSwitcher();
    void openCurrentView();
    void rebuildCards();
    QWidget *makeCard(qsizetype rowIndex);
    /// The currently selected view's config, null before anything is open.
    const ViewConfig *view() const;

    Index *m_index = nullptr;
    const Vault *m_vault = nullptr;
    std::optional<BaseFile> m_file;
    std::unique_ptr<BaseQuery> m_query;
    QString m_lastError;
    int m_currentView = 0;

    QComboBox *m_viewSwitcher = nullptr;
    QLabel *m_status = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTableView *m_table = nullptr;
    BaseTableModel *m_tableModel = nullptr;
    QScrollArea *m_cardsArea = nullptr;
    QWidget *m_cardsContent = nullptr;
    QGridLayout *m_cardsLayout = nullptr;
};

} // namespace noteAppC::bases
