// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <optional>

#include <QByteArray>
#include <QtGlobal>
#include <QString>

namespace noteAppC {

/// What the index will read of one .ink file, in bytes, plain or inflated. Far below inkcore's 64 MiB limit for
/// opening one file: the index reads every file of a vault at once, and a "2 MB heavy note" clears this eight times.
constexpr qint64 kInkIndexMaxBytes = qint64(16) << 20;

/// The text of an ink note (a `.ink` file), as the index reads it without loading the ink module: the Markdown of
/// every text box, in file order, joined by a blank line. Plain or gzip JSON of the `.ink` v1 schema
/// (docs/INK.md, libs/ext/ink/core/InkFormat.h). An empty string is a note with no text boxes; `nullopt` is a
/// file that is not an ink file (truncated, damaged, foreign, inflated past the cap), which is not indexed.
/// Bounded for hostile files: the input, the inflated size and the extracted text are capped, and at most two
/// files are parsed at once. A file inkcore's readInk would refuse (trailing data after the gzip member, no valid
/// "version") is refused here too, so nothing shows in search that cannot be opened.
std::optional<QString> inkTextBoxes(const QByteArray &bytes);

} // namespace noteAppC
