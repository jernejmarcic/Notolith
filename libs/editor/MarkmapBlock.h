// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <functional>

#include "MarkdownEditor.h"
#include "Theme.h"

namespace noteAppC {

/// A ```` ```markmap ```` fence drawn as a mind map (libs/render/Markmap.h has
/// the tree, the layout and the SVG). The renderer MarkdownEditor::
/// setCodeBlockRenderer takes, like MermaidBlock.h's, so the fence, its
/// placeholder, its cache and "caret inside shows the source" are
/// CodeBlockHook.cpp's, and registering nothing is how Markmap is off.
///
/// Unlike the diagram kinds there is no tool: a map is laid out on a pool
/// thread of its own (never the UI thread; milliseconds, but a thousand nodes
/// are not nothing) and rasterised at the editor's width and pixel ratio.
/// Nothing is kept on disk: publish and PDF export draw a map from its text
/// themselves (MermaidCache.h's diagramCachedSVG).
///
/// The picture answers a click on a node's circle: the answer carries a
/// hotspot for each circle and `activate`, which sets that node's fold the
/// other way from how that picture showed it (so a second view of the same
/// note, still showing the old picture, agrees) and draws again. The folding is the renderer's view state, by the fence's
/// text: shared by every editor showing it, never written to the note, gone
/// when the text changes. `theme` is asked on the UI thread, for each fence
/// the editor lays out.
MarkdownEditor::CodeBlockRenderer markmapRenderer(std::function<Theme()> theme);

} // namespace noteAppC
