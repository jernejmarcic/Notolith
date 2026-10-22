#pragma once

#include <memory>
#include <vector>

#include <QPainterPath>

#include "InkTypes.h"

/// Pure functions over the ink model that the tools use (Tasks 12-15 add to it).
namespace noteAppC::ink {

struct SplitResult {
    bool hit = false;
    std::vector<std::shared_ptr<const Stroke>> pieces; ///< the surviving runs, in order, with the style; hit with no pieces: the stroke is gone
};

/// The eraser's split mode. Segments whose outline meets `eraser` are cut out; every maximal run of
/// consecutive untouched segments (two points or more) survives as a new stroke.
SplitResult splitStroke(const Stroke &stroke, const QPainterPath &eraser);

} // namespace noteAppC::ink
