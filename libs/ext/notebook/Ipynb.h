// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A Jupyter notebook (.ipynb, nbformat 4) as a note: markdown cells are text,
// code cells are fenced blocks in the notebook's language, and the outputs the
// file stores are seeded into the output cache the editor already reads, so
// they show (and run) through the machinery every code block has. Saving
// merges the edited text back into the file on disk: cells that did not change
// are the file's own objects (ids, metadata, outputs, unknown keys), and a
// text that is the file's own shows writes back its exact bytes. A cell that
// ran (or was cleared) since the file was read takes its outputs and count
// from the run the window kept for it (IpynbRuns).
//
// Pure data: no widgets, no kernel. The window calls readIpynb when it opens
// one and mergeIpynb when it saves (docs/NOTEBOOKS.md, "Jupyter notebooks").

#include "KernelSpec.h"
#include "NotebookCache.h"
#include "NotebookSandbox.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QSet>
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

/// What a run of a code cell left, by the output-cache key it ran under: held by the window from the run's end
/// until the notebook is saved. A result with `hasRun` false is a clear (the cell's outputs go, its count is null).
struct IpynbRuns {
    QHash<QString, NotebookRunResult> results;
    bool isEmpty() const { return results.isEmpty(); }
};

/// The pictures a notebook's markdown cells carry (`attachments`, referenced as `![x](attachment:name)`), and
/// what it takes to say which cell of a text edited since is which. Two cells attach different pictures under one
/// name (JupyterLab calls every pasted screenshot `image.png`), so a picture is "<cell>/<name>", `cell` being
/// the index among the cells the file had.
class IpynbPictures {
public:
    IpynbPictures() = default;
    explicit IpynbPictures(const IpynbDocument &document);

    bool isEmpty() const { return m_images.isEmpty(); }
    /// "<cell>/<name>" -> the image's bytes; only pictures (an attachment of another type is not one).
    const QHash<QString, QByteArray> &images() const { return m_images; }
    /// "<cell>/" for a picture named in `markdown` at `offset`: `cell` is the file's cell that the one of the text
    /// holding `offset` is (the pairing a save keeps ids, outputs and attachments by). Empty for a cell that is
    /// new, so it shows none. The pairing of the last text asked about is kept.
    QString scopeAt(const QString &markdown, qsizetype offset) const;

private:
    QHash<QString, QByteArray> m_images;
    QStringList m_types;   ///< the file's cells, as the text shows them
    QStringList m_sources;
    QString m_language;
    mutable QString m_text;           ///< the text the next two are for
    mutable QList<qsizetype> m_starts; ///< where each of its cells starts
    mutable QList<qsizetype> m_paired; ///< the file's cell it pairs with, or -1
};

/// parse + ipynbMarkdown + seedIpynbOutputs: what opening a notebook does.
/// `pictures`, when given, receives the notebook's IpynbPictures.
bool readIpynb(const QByteArray &bytes, QString *markdown, QString *error = nullptr, IpynbPictures *pictures = nullptr);

/// `markdown`, edited, merged into `fileBytes` (the notebook as it is on disk).
/// Empty (with `error`) when `fileBytes` is not a notebook. Byte for byte
/// `fileBytes` when the text is the one it shows; otherwise the same notebook
/// with its `cells` rebuilt, written the way nbformat writes JSON (one-space
/// indent, sorted keys). An edited cell keeps its id, metadata and outputs;
/// a new cell gets an empty one.
QByteArray mergeIpynb(const QByteArray &fileBytes, const QString &markdown, QString *error = nullptr);
/// The same, and the code cells that ran (or were cleared) since the file was read take what `runs` holds for
/// the output-cache key they ran under: its outputs and count, or none and null for a clear. Every other cell
/// keeps the file's own bytes, since what the editor shows of an output is not all the file said. Not the file's
/// bytes when `runs` is not empty, even for an unedited text (unless nothing came out different).
QByteArray mergeIpynb(const QByteArray &fileBytes, const QString &markdown, const IpynbRuns &runs,
                      QString *error = nullptr);

/// A new, empty notebook as a file: nbformat 4.5, one empty code cell with an
/// id, the kernelspec `kernel` names (the default python3's), or - none found -
/// only `language_info.name = python`.
QByteArray newIpynb(const KernelSpec *kernel = nullptr);

/// nbformat's JSON style (Python's `json.dumps(indent=1, sort_keys=True,
/// ensure_ascii=False)` and a final newline), so a notebook kept in git changes
/// only where it was edited.
QByteArray dumpIpynbJSON(const QJsonObject &root);

} // namespace noteAppC::notebook
