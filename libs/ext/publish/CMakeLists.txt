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
    PageBody.h
    PageBody.cpp
    Publisher.h
    Publisher.cpp)

target_include_directories(noteAppC_publish PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_publish
    PUBLIC Qt6::Core noteAppC_markdown noteAppC_index noteAppC_vault noteAppC_render noteAppC_theme
    PRIVATE Qt6::Concurrent)
