#pragma once

#include <functional>

#include <QString>

#include "MarkdownEditor.h"
#include "Theme.h"

namespace noteAppC {

/// A ```` ```mermaid ```` fence drawn as its diagram. Not an editor feature
/// of its own: this is the renderer MarkdownEditor::setCodeBlockRenderer
/// takes, so the fence, its placeholder, its cache by width/theme/source and
/// "caret inside shows the source" are CodeBlockHook.cpp's, and registering
/// nothing is how Mermaid is off.
///
/// The diagram is drawn by an external tool, `mmdr` (the pure-Rust
/// mermaid-rs-renderer crate), run as `mmdr -i in.mmd -o out.svg -c cfg.json`
/// on a thread pool - never the UI thread. Nothing is looked up or started
/// until a fence is first laid out: the renderer is a closure over a small
/// state, and the tool's path is resolved by the first job. A tool that is
/// missing is remembered for the life of the renderer (make a new one when
/// the setting changes), so typing never probes for it again.
///
/// Answers, all through CodeBlockResult: the diagram as an image rasterised
/// at `width` and the display's pixel ratio (so it is crisp at any scale);
/// or Markdown with the source in a `text` fence and one line saying why
/// not (tool missing, tool failed, timeout) - the source shown with a
/// dimmed hint, never a hole.
///
/// `tool` is the render.mermaidTool setting: a bare name looked up on PATH
/// and then in ~/.cargo/bin, or a path. `theme` is asked on the UI thread
/// for each fence the editor lays out, to colour the diagram with.
MarkdownEditor::CodeBlockRenderer mermaidRenderer(const QString &tool, std::function<Theme()> theme);

/// $XDG_CACHE_HOME/noteAppC/mermaid, where the SVGs mmdr made are kept by a
/// hash of the source and the colours asked for.
QString mermaidCacheDirectory();

/// Drops the SVGs in `directory` not used for `maxDays` days, then the least
/// recently used until what is left is within `maxBytes`. A cache hit touches
/// the file's modification time, which is what "used" means here. The renderer
/// calls it once per process with 30 days and 50 MB, on its worker thread
/// after the first diagram has been delivered.
void pruneMermaidCache(const QString &directory, int maxDays, qint64 maxBytes);

} // namespace noteAppC
