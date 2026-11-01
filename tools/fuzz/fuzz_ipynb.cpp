// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// libFuzzer target: a Jupyter notebook, opened as a note and merged back
// with an edited text, as the window does on save.

#include <QByteArray>
#include <QString>

#include "Ipynb.h"

using namespace noteAppC::notebook;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));

    QString markdown;
    IpynbPictures pictures;
    if (!readIpynb(bytes, &markdown, nullptr, &pictures))
        return 0;
    pictures.scopeAt(markdown, markdown.size() / 2);
    mergeIpynb(bytes, markdown);
    mergeIpynb(bytes, markdown.mid(markdown.size() / 3));
    mergeIpynb(bytes, markdown + markdown.left(markdown.size() / 2));
    return 0;
}
