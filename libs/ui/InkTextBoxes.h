#pragma once

#include <optional>

#include <QByteArray>
#include <QString>

namespace noteAppC {

/// The text of an ink note (a `.ink` file), as the index reads it without loading the ink module: the Markdown of
/// every text box, in file order, joined by a blank line. Plain or gzip JSON of the `.ink` v1 schema
/// (docs/INK.md, libs/ext/ink/core/InkFormat.h). An empty string is a note with no text boxes; `nullopt` is a
/// file that is not an ink file (truncated, damaged, foreign, inflated past the cap), which is not indexed.
/// Bounded for hostile files: the inflated size and the extracted text are capped.
std::optional<QString> inkTextBoxes(const QByteArray &bytes);

} // namespace noteAppC
