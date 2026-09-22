add_library(noteAppC_theme STATIC
    Theme.cpp
    Theme.h
    Icons.cpp
    Icons.h)

set_target_properties(noteAppC_theme PROPERTIES AUTOMOC ON)

# Icons are Lucide (ISC), the set Obsidian itself uses - see icons/LICENSE.
qt_add_resources(noteAppC_theme "icons" PREFIX "/" FILES
    icons/folder-open.svg
    icons/file-plus.svg
    icons/sun.svg
    icons/moon.svg
    icons/settings.svg
    icons/search.svg
    icons/panel-left.svg
    icons/x.svg
    icons/chevron-right.svg
    icons/chevron-down.svg
    icons/branch-collapsed.svg
    icons/branch-expanded.svg)

target_include_directories(noteAppC_theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_theme PUBLIC Qt6::Gui Qt6::Svg)
