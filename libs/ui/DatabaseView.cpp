// SPDX-License-Identifier: GPL-3.0-or-later
#include "DatabaseView.h"

#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTableView>
#include <QUuid>
#include <QVBoxLayout>

namespace noteAppC {

DatabaseView::DatabaseView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("databaseView"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *splitter = new QSplitter(this);
    m_tables = new QListWidget(splitter);
    m_tables->setObjectName(QStringLiteral("databaseTables"));
    m_rows = new QTableView(splitter);
    m_rows->setObjectName(QStringLiteral("databaseRows"));
    m_rows->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_rows->setAlternatingRowColors(true);
    m_rows->verticalHeader()->setDefaultSectionSize(m_rows->fontMetrics().height() + 6);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({180, 600});
    m_status = new QLabel(this);
    m_status->setContentsMargins(8, 4, 8, 4);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_status);
    connect(m_tables, &QListWidget::currentTextChanged, this, [this](const QString &table) { showTable(table); });
}

DatabaseView::~DatabaseView()
{
    delete m_model; // its query holds the connection
    m_model = nullptr;
    if (!m_connection.isEmpty()) {
        QSqlDatabase::database(m_connection, false).close();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool DatabaseView::isDatabasePath(const QString &path)
{
    for (const char *suffix : {".db", ".sqlite", ".sqlite3", ".db3"}) {
        if (path.endsWith(QLatin1String(suffix), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

bool DatabaseView::load(const QString &path, QString *error)
{
    // The header, before SQLite sees it: an empty or foreign .db is refused rather than opened as a new database.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    if (file.read(16) != QByteArray("SQLite format 3\0", 16)) {
        *error = tr("not an SQLite database");
        return false;
    }
    file.close();

    m_path = path;
    m_connection = QStringLiteral("databaseView-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!db.open()) {
        *error = db.lastError().text();
        return false;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type IN ('table', 'view') "
                                   "AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name"))) {
        *error = query.lastError().text();
        return false;
    }
    while (query.next())
        m_tables->addItem(query.value(0).toString());
    m_model = new QSqlQueryModel(this);
    m_rows->setModel(m_model);
    if (m_tables->count() > 0)
        m_tables->setCurrentRow(0);
    else
        m_status->setText(tr("No tables"));
    return true;
}

QStringList DatabaseView::tables() const
{
    QStringList names;
    for (int i = 0; i < m_tables->count(); ++i)
        names << m_tables->item(i)->text();
    return names;
}

bool DatabaseView::showTable(const QString &table)
{
    if (!m_model || !tables().contains(table))
        return false;
    if (!m_tables->currentItem() || m_tables->currentItem()->text() != table) {
        m_tables->setCurrentItem(m_tables->findItems(table, Qt::MatchExactly).first());
        return true; // currentTextChanged brings it back here
    }
    QString quoted = table;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    const QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    m_model->setQuery(QStringLiteral("SELECT * FROM \"%1\"").arg(quoted), db);
    if (m_model->lastError().isValid()) {
        m_status->setText(m_model->lastError().text());
        return true;
    }
    QSqlQuery count(QStringLiteral("SELECT count(*) FROM \"%1\"").arg(quoted), db);
    const qlonglong rows = count.next() ? count.value(0).toLongLong() : m_model->rowCount();
    m_status->setText(tr("%1: %n row(s), read-only", nullptr, int(qMin<qlonglong>(rows, INT_MAX))).arg(table));
    return true;
}

} // namespace noteAppC
