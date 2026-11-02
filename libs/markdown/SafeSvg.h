// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>

namespace noteAppC {

/// `svg` rewritten so that it names nothing outside itself. Qt's SVG plugin loads an `<image href>` from
/// any file on the disk (absolute, or beside the .svg), so a vault's or a kernel's SVG would draw
/// pictures that were never in the vault. Parsed and written again with QXmlStreamReader / Writer: the
/// DTD, entity declarations, comments and processing instructions are dropped, and an `href` or
/// `xlink:href` is kept only when it is a fragment ("#id") or a base64 data URI of a PNG, JPEG, GIF or
/// WebP picture (a nested SVG would be decoded with its own references). Empty for anything that is not
/// well-formed XML: draw nothing.
QByteArray safeSvg(const QByteArray &svg);

} // namespace noteAppC
