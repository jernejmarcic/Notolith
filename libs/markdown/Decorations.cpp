// SPDX-License-Identifier: GPL-3.0-or-later
#include "Decorations.h"

namespace noteAppC {

QList<Range> Decorations::hiddenFor(const Range &block, qsizetype cursor) const
{
    // Obsidian reveals the raw source of exactly the block the cursor is in.
    if (block.isValid() && cursor >= block.start && cursor <= block.end)
        return {};

    QList<Range> hidden;
    for (const Range &range : syntax) {
        if (range.overlaps(block))
            hidden.append(range);
    }
    return hidden;
}

} // namespace noteAppC
