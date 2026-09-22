# The window lives in a library so the widget smoke test can drive it.
add_library(noteAppC_ui STATIC
    MainWindow.cpp
    MainWindow.h
    QuickSwitcher.cpp
    QuickSwitcher.h
    BacklinksPane.cpp
    BacklinksPane.h
    TagsPane.cpp
    TagsPane.h
    OutgoingLinksPane.cpp
    OutgoingLinksPane.h)

set_target_properties(noteAppC_ui PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_ui PUBLIC Qt6::Widgets noteAppC_vault noteAppC_theme noteAppC_index noteAppC_editor)
