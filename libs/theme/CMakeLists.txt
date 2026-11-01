add_library(noteAppC_theme STATIC
    PopupStyle.cpp
    PopupStyle.h
    Theme.cpp
    Theme.h
    Scheme.cpp
    Scheme.h
    CalloutStyle.cpp
    FormatColours.cpp
    FormatColours.h
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
    icons/arrow-up-down.svg
    icons/arrow-up-right.svg
    icons/book-open.svg
    icons/noteAppC.svg
    icons/bookmark.svg
    icons/brackets.svg
    icons/branch-collapsed.svg
    icons/branch-expanded.svg
    icons/bug.svg
    icons/calendar.svg
    icons/check.svg
    icons/chevron-down.svg
    icons/chevron-right.svg
    icons/chevrons-down-up.svg
    icons/circle-alert.svg
    icons/circle-check.svg
    icons/circle-help.svg
    icons/code.svg
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
    icons/pin.svg
    icons/spin-up.svg
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

# Network is for Icons::icon(), which fetches a remote icon URL itself, and only with NOTEAPPC_WITH_NETWORK:
# the default build links no network class into the theme.

target_include_directories(noteAppC_theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# noteAppC_markdown for ConfigFile (Core only, shared with TaskStates).
target_link_libraries(noteAppC_theme PUBLIC Qt6::Gui Qt6::Widgets Qt6::Svg noteAppC_markdown)
if(NOTEAPPC_WITH_NETWORK)
    target_link_libraries(noteAppC_theme PUBLIC Qt6::Network)
    target_compile_definitions(noteAppC_theme PUBLIC NOTEAPPC_HAVE_NETWORK)
endif()
