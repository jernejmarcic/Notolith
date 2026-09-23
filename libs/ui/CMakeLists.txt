# The window lives in a library so the widget smoke test can drive it.
add_library(noteAppC_ui STATIC
    ExportPdfDialog.cpp
    ExportPdfDialog.h
    MainWindow.cpp
    MainWindow.h
    RecoveryDialog.cpp
    RecoveryDialog.h
    QuickSwitcher.cpp
    QuickSwitcher.h
    BacklinksPane.cpp
    BacklinksPane.h
    TagsPane.cpp
    TagsPane.h
    OutgoingLinksPane.cpp
    OutgoingLinksPane.h
    PagePreview.cpp
    PagePreview.h
    OutlinePane.cpp
    OutlinePane.h
    BookmarksPane.cpp
    BookmarksPane.h
    TemplatePicker.cpp
    TemplatePicker.h
    SettingsPage.cpp
    SettingsPage.h
    ToggleSwitch.cpp
    ToggleSwitch.h)

set_target_properties(noteAppC_ui PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_ui PUBLIC Qt6::Widgets noteAppC_vault noteAppC_theme noteAppC_index
    noteAppC_editor noteAppC_settings)
target_compile_definitions(noteAppC_ui PRIVATE NOTEAPPC_VERSION="${PROJECT_VERSION}")

if(TARGET noteAppC_spell)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_spell Qt6::Concurrent)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_SPELLCHECK)
endif()
