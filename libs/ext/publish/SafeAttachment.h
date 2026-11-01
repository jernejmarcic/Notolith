// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QByteArray>
#include <QString>

namespace noteAppC::publish::SafeAttachment {

/// "image", "audio", "video" or "pdf" for a file suffix (lower case, no dot) of a kind a published site may
/// carry, empty for any other. Passive formats only: nothing a browser runs or lays out as a page.
QString kindOfSuffix(const QString &suffix);

/// Whether the file `relativePath` holding `bytes` may be copied into a site: the suffix is a passive kind
/// (kindOfSuffix) and the first bytes are that format, never the name alone. An SVG is rewritten in `bytes` to a
/// copy with no script, handler, style, foreign content or outside reference. False for anything else.
bool prepare(const QString &relativePath, QByteArray &bytes);

} // namespace noteAppC::publish::SafeAttachment
