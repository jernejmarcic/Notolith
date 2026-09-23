#include "BaseEditorTable.h"

#include "BaseQuery.h"

namespace noteAppC::bases {

BaseEditorTable evaluateBaseForEditor(const QString &yamlText, const QList<Index::BaseNote> &notes,
                                      const Vault *vault, int viewIndex)
{
    BaseEditorTable table;

    QString error;
    std::optional<BaseFile> file = BaseFile::parse(yamlText, &error);
    if (!file) {
        table.error = error;
        return table;
    }

    int n = 0;
    for (const ViewConfig &view : file->views()) {
        ++n;
        table.viewNames << (view.name.isEmpty() ? QStringLiteral("View %1").arg(n) : view.name);
    }
    // BaseFile::parse never returns a file with zero views (an empty
    // `views:` gets the same implicit "All notes" table Obsidian shows), so
    // table.viewNames is never empty here.
    table.shownView = (viewIndex >= 0 && viewIndex < file->views().size()) ? viewIndex : 0;

    QList<Row> rows;
    rows.reserve(notes.size());
    for (const Index::BaseNote &note : notes)
        rows << rowFromNote(note);

    BaseQuery query(*file, std::move(rows), vault);
    if (!query.openView(table.shownView, &error)) {
        table.error = error;
        return table;
    }

    const QStringList &order = query.view().order;
    table.headers.reserve(order.size());
    for (const QString &reference : order)
        table.headers << file->displayNameFor(reference);

    table.rows.reserve(int(query.resultRows().size()));
    for (const qsizetype rowIndex : query.resultRows()) {
        BaseEditorRow row;
        row.cells.reserve(order.size());
        for (const QString &reference : order)
            row.cells << query.valueOf(rowIndex, reference).toDisplayString();
        row.notePath = query.rows().at(rowIndex).notePath;
        table.rows.append(std::move(row));
    }
    return table;
}

} // namespace noteAppC::bases
