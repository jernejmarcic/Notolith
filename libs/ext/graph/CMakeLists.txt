# SPDX-License-Identifier: GPL-3.0-or-later
# Graph view, a Tier 1 module (docs/PLUGINS.md): the widget links
# noteAppC_index to read the vault and noteAppC_theme to paint in the current
# colours, but nothing in the rest of the app links it back - a build with
# NOTEAPPC_WITH_GRAPH off has none of this code at all.
add_library(noteAppC_graph STATIC
    GraphLayout.cpp
    GraphLayout.h
    GraphView.cpp
    GraphView.h)

target_include_directories(noteAppC_graph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_graph PUBLIC Qt6::Widgets noteAppC_index noteAppC_theme PRIVATE Qt6::Concurrent)
