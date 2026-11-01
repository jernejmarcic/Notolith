// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "Theme.h"

namespace noteAppC {

/// The SVGs `mmdr` made for ```` ```mermaid ```` fences, kept on disk so the
/// editor draws one without starting the tool again and so publish and PDF
/// export (which never run it) can still show a diagram the editor has made.
/// It lives here, not in libs/editor, because libs/ext/publish does not link
/// the editor. MermaidBlock.cpp (the editor's renderer) writes it.
///
/// PlantUML, TikZ and Graphviz fences are kept the same way, each kind in a
/// directory of its own; the mermaid* functions are the Mermaid case of the
/// diagram* ones. A ```` ```markmap ```` fence is the odd one out: no tool, its
/// SVG is made in-process (Markmap.h), so diagramCachedSVG draws it from the
/// source, in the theme asked, and nothing of it is kept on disk.

enum class DiagramKind { Mermaid, PlantUML, TikZ, Graphviz, Markmap };

/// The fence languages a kind answers to (lower case).
QStringList diagramLanguages(DiagramKind kind);

/// What a kind's cache key and tool are given of `theme`: Mermaid's
/// mermaidConfig; for the others {"dark", "text", "background"} as JSON, and
/// for Markmap also the colours and fonts of its map (Markmap.h's MarkmapLook).
QByteArray diagramConfig(DiagramKind kind, const Theme &theme);

/// $XDG_CACHE_HOME/notolith/<mermaid|plantuml|tikz|dot> (a mind map is drawn from its text, never cached).
QString diagramCacheDirectory(DiagramKind kind);

/// mermaidCachePath for any kind.
QString diagramCachePath(DiagramKind kind, const QString &source, const QByteArray &config);

/// mermaidCachedSVG(source, theme) for any kind; for Markmap the map drawn
/// from `source` in `theme`'s colours (needs a QGuiApplication, for the fonts).
QByteArray diagramCachedSVG(DiagramKind kind, const QString &source, const Theme &theme);

/// The config JSON handed to `mmdr -c`: Mermaid's own `theme` and
/// `themeVariables` shape, in `theme`'s colours. mmdr 0.3.1 honours it (the
/// colours land in the SVG, background included), so a diagram drawn for a
/// theme already is that theme's colour and needs no re-colouring.
QByteArray mermaidConfig(const Theme &theme);

/// $XDG_CACHE_HOME/notolith/mermaid.
QString mermaidCacheDirectory();

/// Where the SVG for `source` drawn with `config` is, named by a hash of each: another
/// scheme is another picture. The file may not exist.
QString mermaidCachePath(const QString &source, const QByteArray &config);

/// The cached SVG at `path`, or empty. A hit counts as a use (touches the
/// file's modification time) for the prune.
QByteArray mermaidCachedSVG(const QString &path);

/// The cached SVG for `source` in `theme`'s colours, else the most recently
/// used one for that source in any colours, else empty: publish and PDF
/// export draw what the editor made, never start the tool. Never run on a
/// hot path (a directory listing when the exact file is missing).
QByteArray mermaidCachedSVG(const QString &source, const Theme &theme);

/// Keeps `svg` at `path` (atomically). False when the directory or file could not be written.
bool mermaidCacheStore(const QString &path, const QByteArray &svg);

/// Drops the SVGs in `directory` not used for `maxDays` days, then the least
/// recently used until what is left is within `maxBytes`. The editor's
/// renderer calls it once per process with 30 days and 50 MB, on its worker
/// thread after the first diagram has been delivered.
void pruneMermaidCache(const QString &directory, int maxDays, qint64 maxBytes);

} // namespace noteAppC
