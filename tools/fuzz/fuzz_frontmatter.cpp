// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
// libFuzzer target: a note's YAML front matter (properties), read and
// rewritten the ways the properties editor does.

#include <QString>
#include <QStringList>

#include "Frontmatter.h"

using namespace noteAppC;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QString text = QString::fromUtf8(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    const QStringList lines = text.split(QLatin1Char('\n'));

    const Range block = Frontmatter::find(text);
    Metadata metadata;
    Frontmatter::parse(text, block, metadata);
    // A fence is only found at the top; try the text as a bare block too.
    Frontmatter::parse(text, Range{0, text.size()}, metadata);

    Frontmatter::rewriteList(lines.first(), lines.mid(1));
    Frontmatter::inferType(lines);
    for (auto type : {Property::Type::Text, Property::Type::Number, Property::Type::Date,
                      Property::Type::DateTime, Property::Type::Checkbox})
        Frontmatter::scalarFor(text, type);
    return 0;
}
