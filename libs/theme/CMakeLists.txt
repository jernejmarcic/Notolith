add_library(noteAppC_theme STATIC
    Theme.cpp
    Theme.h
    Scheme.cpp
    Scheme.h
    CalloutStyle.cpp
    ConfigFile.cpp
    ConfigFile.h
    CalloutStyle.h
    LanguageStyle.cpp
    LanguageStyle.h
    Icons.cpp
    Icons.h)

set_target_properties(noteAppC_theme PROPERTIES AUTOMOC ON)

# Icons are Lucide (ISC), the set Obsidian itself uses - see icons/LICENSE.
qt_add_resources(noteAppC_theme "icons" PREFIX "/" FILES
    icons/arrow-left.svg
    icons/arrow-right.svg
    icons/arrow-up-right.svg
    icons/book-open.svg
    icons/bookmark.svg
    icons/branch-collapsed.svg
    icons/branch-expanded.svg
    icons/bug.svg
    icons/calendar.svg
    icons/check.svg
    icons/chevron-down.svg
    icons/chevron-right.svg
    icons/circle-alert.svg
    icons/circle-check.svg
    icons/circle-help.svg
    icons/clipboard-list.svg
    icons/code-2.svg
    icons/ellipsis.svg
    icons/external-link.svg
    icons/eye.svg
    icons/file-plus.svg
    icons/file-text.svg
    icons/file.svg
    icons/flame.svg
    icons/folder-open.svg
    icons/folder-plus.svg
    icons/folder.svg
    icons/git-fork.svg
    icons/hash.svg
    icons/info.svg
    icons/keyboard.svg
    icons/link.svg
    icons/list.svg
    icons/moon.svg
    icons/palette.svg
    icons/panel-left.svg
    icons/panel-right.svg
    icons/pencil.svg
    icons/play.svg
    icons/plus.svg
    icons/quote.svg
    icons/square.svg
    icons/search.svg
    icons/settings.svg
    icons/sliders-horizontal.svg
    icons/sun.svg
    icons/tag.svg
    icons/tags.svg
    icons/trash.svg
    icons/triangle-alert.svg
    icons/type.svg
    icons/x.svg
    icons/zap.svg)

# Network is for Icons::icon(), which fetches a remote icon URL itself; the
# root find_package asks for it, so nothing is requested twice here.

target_include_directories(noteAppC_theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_theme PUBLIC Qt6::Gui Qt6::Svg Qt6::Network)
