# SPDX-License-Identifier: GPL-3.0-or-later
# Publish, a Tier 1 module (docs/PLUGINS.md): exports a vault - or the
# `publish: true` part of it - as a static, JavaScript-optional website,
# Obsidian Publish style. Built only with NOTEAPPC_WITH_PUBLISH on.
#
# No new external dependency: everything it draws on (the parser, the index,
# maths/code/HTML rendering, the theme's colours and bundled icons) is
# already a library this project links elsewhere.
add_library(noteAppC_publish STATIC
    Slugs.h
    Slugs.cpp
    SafeAttachment.h
    SafeAttachment.cpp
    SafeHtml.h
    SafeHtml.cpp
    PageBody.h
    PageBody.cpp
    Publisher.h
    Publisher.cpp)

target_include_directories(noteAppC_publish PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_publish
    PUBLIC Qt6::Core noteAppC_markdown noteAppC_index noteAppC_vault noteAppC_render noteAppC_theme
    PRIVATE Qt6::Concurrent)

# Raw HTML in a note is cleaned with gumbo (litehtml's parser, as in libs/editor); without it the markup
# is written out as escaped text.
if(NOTEAPPC_WITH_HTML)
    find_path(NOTEAPPC_GUMBO_INCLUDE gumbo.h)
    find_library(NOTEAPPC_GUMBO_LIBRARY gumbo)
endif()
if(NOTEAPPC_WITH_HTML AND NOTEAPPC_GUMBO_INCLUDE AND NOTEAPPC_GUMBO_LIBRARY)
    target_include_directories(noteAppC_publish PRIVATE ${NOTEAPPC_GUMBO_INCLUDE})
    target_link_libraries(noteAppC_publish PRIVATE ${NOTEAPPC_GUMBO_LIBRARY})
    target_compile_definitions(noteAppC_publish PUBLIC NOTEAPPC_HAVE_GUMBO)
endif()
