# SPDX-License-Identifier: GPL-3.0-or-later
# Canvas, the second Tier 1 module (docs/PLUGINS.md): JSON Canvas 1.0 read,
# write, and an infinite pan/zoom view over it. Links the editor library only
# to reuse MarkdownEditor for rendering a node's Markdown the way a note pane
# does - no new external dependency, per the option below.
add_library(noteAppC_canvas STATIC
    CanvasDocument.cpp
    CanvasDocument.h
    CanvasCommands.cpp
    CanvasCommands.h
    CanvasView.cpp
    CanvasView.h
    CanvasRender.cpp
    CanvasEditing.cpp)

set_target_properties(noteAppC_canvas PROPERTIES AUTOMOC ON)

target_include_directories(noteAppC_canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_canvas PUBLIC Qt6::Widgets noteAppC_editor noteAppC_theme)
