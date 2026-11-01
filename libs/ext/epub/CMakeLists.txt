# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# EPUB reader, a Tier 1 module (docs/PLUGINS.md): parses EPUB 2/3 (libarchive
# for the zip container, QXmlStreamReader for container.xml/OPF/NCX/nav) and
# renders each chapter's XHTML through litehtml via HtmlRenderer
# (libs/render) - the same renderer a note's raw HTML block already uses,
# extended (see HtmlRenderer.h) rather than duplicated. Links noteAppC_vault
# to read/write the highlights sidecar with the same byte-fidelity rule every
# other note gets, and noteAppC_editor only to reuse FindBar (UI-only, no
# dependency on MarkdownEditor's text model) for find-in-book.
# The zip access and the cover reader stand alone (no editor, no render): the
# editor's EPUB embed card links just this, the reader links it too.
add_library(noteAppC_epubcover STATIC EpubArchive.cpp EpubArchive.h EpubCover.cpp EpubCover.h)
target_include_directories(noteAppC_epubcover PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_epubcover PUBLIC Qt6::Gui PRIVATE noteAppC_theme PkgConfig::LIBARCHIVE)

add_library(noteAppC_epub STATIC
    EpubDocument.cpp
    EpubDocument.h
    EpubHighlights.cpp
    EpubHighlights.h
    EpubPosition.cpp
    EpubPosition.h
    EpubView.cpp
    EpubView.h
    EpubRender.cpp
    EpubSelection.cpp)

set_target_properties(noteAppC_epub PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_epub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_epub
    PUBLIC Qt6::Widgets noteAppC_theme noteAppC_epubcover
    PRIVATE noteAppC_render noteAppC_vault noteAppC_editor Qt6::Concurrent PkgConfig::LIBARCHIVE)
