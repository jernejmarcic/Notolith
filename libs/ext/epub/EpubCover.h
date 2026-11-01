// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QImage>
#include <QString>

namespace noteAppC {

/// An EPUB's cover image, for a card that stands in for the book (the
/// editor's embed). Lives beside EpubArchive rather than in EpubDocument
/// because the editor cannot link the reader (the reader links the editor):
/// this reads two small XML files and one image, nothing else of the book.
///
/// The cover is the OPF manifest item with properties="cover-image" (EPUB 3),
/// else the item `<meta name="cover" content="id">` names (EPUB 2). Null when
/// the file is not an EPUB, declares none, or the image does not decode.
/// Blocking; the caller runs it off the UI thread. `maxSide` bounds the
/// longer edge of the result so a 4000 px scan is never held for a thumbnail.
QImage epubCover(const QString &path, int maxSide = 256);

} // namespace noteAppC
