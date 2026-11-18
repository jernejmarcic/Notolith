// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QWidget>

class QListWidget;
class QLabel;
class QSqlQueryModel;
class QTableView;

namespace noteAppC {

/// An SQLite database (.db, .sqlite, .sqlite3, .db3) shown read-only: its tables and views on the left, the
/// rows of the chosen one on the right, fetched as they scroll into sight. The connection is opened read-only,
/// so nothing here can write to the file.
class DatabaseView : public QWidget
{
    Q_OBJECT
public:
    explicit DatabaseView(QWidget *parent = nullptr);
    ~DatabaseView() override;

    /// Whether `path` is named like a database this view opens.
    static bool isDatabasePath(const QString &path);

    /// False with `error` when `path` is not an SQLite database or cannot be read.
    bool load(const QString &path, QString *error);
    const QString &path() const { return m_path; }
    /// The tables and views, in the order the list shows them.
    QStringList tables() const;
    /// Shows `table`'s rows. False when there is no such table.
    bool showTable(const QString &table);
    QSqlQueryModel *model() const { return m_model; }

private:
    QString m_path;
    QString m_connection;
    QListWidget *m_tables = nullptr;
    QTableView *m_rows = nullptr;
    QLabel *m_status = nullptr;
    QSqlQueryModel *m_model = nullptr;
};

} // namespace noteAppC
