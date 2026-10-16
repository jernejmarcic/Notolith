#pragma once

// What a kernel's output messages turn into, shared by the live run
// (NotebookController) and a stored one (Ipynb.cpp): both speak Jupyter's
// display bundles and streams.

#include "NotebookCache.h"

#include <QJsonObject>
#include <QString>

namespace noteAppC::notebook {

/// Colour escapes removed: a traceback line arrives with them, noteAppC draws its own.
QString stripAnsi(const QString &text);
/// A carriage return rewrites its line (tqdm): only what follows the last one stays.
QString resolveCarriageReturns(QString text);
/// What a notebook shows of one display bundle (`data` of display_data or
/// execute_result), richest first: a picture, HTML, LaTeX and Markdown, then
/// text. Nothing recognised: an empty Text output, which the caller drops.
NotebookOutput outputFromMimeBundle(const QJsonObject &data);

} // namespace noteAppC::notebook
