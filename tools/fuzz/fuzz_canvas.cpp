// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer target: a .canvas file (JSON Canvas), read and written back.

#include <QByteArray>

#include "CanvasDocument.h"

using namespace noteAppC;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CanvasDocument document;
    if (!document.parse(QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size))))
        return 0;
    document.serialize();
    for (const CanvasNode &node : document.nodes())
        document.node(node.id());
    for (const CanvasEdge &edge : document.edges())
        document.edge(edge.id());
    return 0;
}
