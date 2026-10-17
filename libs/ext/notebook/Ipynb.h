#pragma once

// A Jupyter notebook (.ipynb, nbformat 4) as a note: markdown cells are text,
// code cells are fenced blocks in the notebook's language, and the outputs the
// file stores are seeded into the output cache the editor already reads, so
// they show (and run) through the machinery every code block has. Saving
// merges the edited text back into the file on disk: cells that did not change
// are the file's own objects (ids, metadata, outputs, unknown keys), and a
// text that is the file's own shows writes back its exact bytes.
//
// Pure data: no widgets, no kernel. The window calls readIpynb when it opens
// one and mergeIpynb when it saves (docs/NOTEBOOKS.md, "Jupyter notebooks").

#include "NotebookCache.h"
#include "NotebookSandbox.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace noteAppC::notebook {

/// `.ipynb`, any case.
bool isIpynbPath(const QString &path);

struct IpynbDocument {
    QJsonObject root;
    QString language;       ///< the fence language of its code cells, lower-case
    int nbformatMinor = 0;  ///< cells carry an `id` from 4.5 on
};
/// False, with `error`, for anything that is not nbformat 4 JSON with a cell list.
bool parseIpynb(const QByteArray &bytes, IpynbDocument *document, QString *error = nullptr);

/// The note text: cells in order, a blank line between them, code cells as
/// ```` ```<language> ```` fences (raw cells as ```` ```raw ````), two markdown
/// cells in a row told apart by an `<!-- cell -->` line.
QString ipynbMarkdown(const IpynbDocument &document);

/// Puts every code cell's stored outputs in the output cache, under the key the
/// editor looks a block up with, so opening the note shows them. A cell that
/// never ran has nothing to put. The key follows the cell's own
/// `#| sandbox:` line, as the editor's does.
void seedIpynbOutputs(const IpynbDocument &document);

/// parse + ipynbMarkdown + seedIpynbOutputs: what opening a notebook does.
bool readIpynb(const QByteArray &bytes, QString *markdown, QString *error = nullptr);

/// `markdown`, edited, merged into `fileBytes` (the notebook as it is on disk).
/// Empty (with `error`) when `fileBytes` is not a notebook. Byte for byte
/// `fileBytes` when the text is the one it shows; otherwise the same notebook
/// with its `cells` rebuilt, written the way nbformat writes JSON (one-space
/// indent, sorted keys). An edited cell keeps its id, metadata and outputs;
/// a new cell gets an empty one.
QByteArray mergeIpynb(const QByteArray &fileBytes, const QString &markdown, QString *error = nullptr);

/// nbformat's JSON style (Python's `json.dumps(indent=1, sort_keys=True,
/// ensure_ascii=False)` and a final newline), so a notebook kept in git changes
/// only where it was edited.
QByteArray dumpIpynbJSON(const QJsonObject &root);

} // namespace noteAppC::notebook
