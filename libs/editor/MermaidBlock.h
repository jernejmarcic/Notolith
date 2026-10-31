#pragma once

#include <functional>

#include <QImage>
#include <QString>

#include "MarkdownEditor.h"
#include "MermaidCache.h"
#include "Theme.h"

namespace noteAppC {

/// A ```` ```mermaid ```` fence drawn as its diagram. Not an editor feature
/// of its own: this is the renderer MarkdownEditor::setCodeBlockRenderer
/// takes, so the fence, its placeholder, its cache by width/theme/source and
/// "caret inside shows the source" are CodeBlockHook.cpp's, and registering
/// nothing is how Mermaid is off.
///
/// The diagram is drawn by an external tool, `mmdr` (the pure-Rust
/// mermaid-rs-renderer crate): `mmdr -i - -c config.json` with the source on
/// stdin and the SVG on stdout, on a thread pool - never the UI thread. The
/// config carries the app's colours (MermaidCache.h), which mmdr honours, so
/// the SVG is already in the theme. The SVGs are kept in MermaidCache's
/// directory, which publish and PDF export read without the tool. Nothing is
/// looked up or started until a fence is first laid out: the renderer is a
/// closure over a small state, and the tool's path is resolved by the first
/// job. A tool that is missing is remembered for the life of the renderer
/// (make a new one when the setting changes), so typing never probes for it.
///
/// Answers, all through CodeBlockResult: the diagram as an image rasterised
/// at `width` and the display's pixel ratio (so it is crisp at any scale);
/// or Markdown with the source in a `text` fence and one line saying why
/// not (tool missing, tool failed, timeout) - the source shown with a
/// dimmed hint, never a hole.
///
/// `tool` is the render.mermaidTool setting: a bare name looked up on PATH,
/// then in $CARGO_HOME/bin, ~/.cargo/bin and ~/.local/share/cargo/bin, or a
/// path. `theme` is asked on the UI thread for each fence the editor lays
/// out, to colour the diagram with.
MarkdownEditor::CodeBlockRenderer mermaidRenderer(const QString &tool, std::function<Theme()> theme);

/// The same for the other kinds (MermaidCache.h's DiagramKind), through the
/// same machinery (a pool thread, the tool resolved by the first job and
/// remembered, memo, disk cache, source and one dim hint on a failure);
/// mermaidRenderer is the Mermaid case. What differs is the tool and how it
/// is run. PlantUML, TikZ and Graphviz run in bubblewrap's sandbox with the
/// syscall filter (sandbox/Bubblewrap.h): no network, only a temporary
/// directory writable, nothing of the user's home visible, and never
/// without it - no bwrap, no diagram, a hint. `tool` is the setting (a name
/// on PATH or a path; for TikZ the `latex` that dvisvgm sits next to or on
/// PATH), so it has to be something under /usr, which is all the sandbox sees.
/// TikZ takes TikZJax's block (`\usepackage` lines, `\begin{document}...
/// \end{document}`) or a bare tikzpicture; the theme's colours reach
/// PlantUML (--dark-mode), Graphviz (default colours, the user's own
/// attributes win) and TikZ (black becomes the text colour on a dark theme).
MarkdownEditor::CodeBlockRenderer diagramRenderer(DiagramKind kind, const QString &tool, std::function<Theme()> theme);

/// `svg` drawn at `width` logical pixels (never wider than it is) and `ratio` device pixels to each, or a
/// null image when it cannot be drawn. `logicalScale`, when given, is set to the logical pixels one SVG
/// unit came to (what a click in the image is turned back into the SVG's own coordinates by).
QImage rasteriseSVG(const QByteArray &svg, int width, qreal ratio, qreal *logicalScale = nullptr);

/// A renderer that only reads MermaidCache.h's directory and answers at once
/// (the editor still posts the answer, so the caller processes events): the
/// diagram if one was made for this source, in `theme`'s colours or any, else
/// an empty answer, which leaves the fence as its listing. For PDF export,
/// which must not start a tool.
MarkdownEditor::CodeBlockRenderer mermaidCachedRenderer(Theme theme);

/// mermaidCachedRenderer for any kind.
MarkdownEditor::CodeBlockRenderer diagramCachedRenderer(DiagramKind kind, Theme theme);

} // namespace noteAppC
