# The window lives in a library so the widget smoke test can drive it.
add_library(noteAppC_ui STATIC
    DictClient.cpp
    DictClient.h
    ExportPdfDialog.cpp
    FileTree.cpp
    FileTree.h
    InlineTitleEdit.cpp
    InlineTitleEdit.h
    InkTextBoxes.cpp
    InkTextBoxes.h
    ExportPdfDialog.h
    PublishDialog.cpp
    PasswordDialog.cpp
    PasswordDialog.h
    PublishDialog.h
    MainWindow.cpp
    PopoutWindow.cpp
    PopoutWindow.h
    MainWindow.h
    SingleInstance.cpp
    SingleInstance.h
    RecoveryDialog.cpp
    RecoveryDialog.h
    NoteHistoryDialog.cpp
    NoteHistoryDialog.h
    QuickSwitcher.cpp
    QuickSwitcher.h
    BacklinksPane.cpp
    BacklinksPane.h
    FootnotesPane.cpp
    FootnotesPane.h
    PropertiesPane.cpp
    SlidesView.cpp
    SlidesView.h
    PropertiesPane.h
    TagsPane.cpp
    TagsPane.h
    OutgoingLinksPane.cpp
    OutgoingLinksPane.h
    PagePreview.cpp
    PagePreview.h
    OutlinePane.cpp
    OutlinePane.h
    MetaNotesPane.cpp
    MetaNotesPane.h
    BookmarksPane.cpp
    BookmarksPane.h
    TemplatePicker.cpp
    TemplatePicker.h
    SearchPane.cpp
    SearchPane.h
    SettingsPage.cpp
    SettingsPage.h
    ToggleSwitch.cpp
    ToggleSwitch.h)

set_target_properties(noteAppC_ui PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Qt6::Concurrent used to arrive only with the optional spell module; SearchPane
# needs it unconditionally to run Index::search() off the UI thread.
target_link_libraries(noteAppC_ui PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::Network noteAppC_vault noteAppC_theme
    noteAppC_index noteAppC_editor noteAppC_settings noteAppC_modules)
# InkTextBoxes inflates a gzip .ink file for the index (the host reads it without loading the ink module).
find_package(ZLIB REQUIRED)
target_link_libraries(noteAppC_ui PRIVATE ZLIB::ZLIB)
target_compile_definitions(noteAppC_ui PRIVATE NOTEAPPC_VERSION="${PROJECT_VERSION}")

if(TARGET noteAppC_spell)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_spell)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_SPELLCHECK)
endif()

if(TARGET noteAppC_graph)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_graph)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_GRAPH)
endif()

if(TARGET noteAppC_canvas)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_canvas)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_CANVAS)
endif()

if(TARGET noteAppC_bases)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_bases)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_BASES)
endif()

if(TARGET noteAppC_crypt)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_crypt)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_ENCRYPTION)
endif()

if(TARGET noteAppC_notebook)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_notebook)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_NOTEBOOK)
endif()

if(TARGET noteAppC_sync)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_sync)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_SYNC)
endif()

# Sharing a note (docs/SHARE.md): the engine's header is in MainWindow.h, hence PUBLIC.
if(TARGET noteAppC_shareengine)
    target_sources(noteAppC_ui PRIVATE ShareSession.cpp ShareSession.h ShareDialog.cpp ShareDialog.h)
    target_link_libraries(noteAppC_ui PUBLIC noteAppC_shareengine)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_SHARE)
endif()

if(TARGET noteAppC_plugins)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_plugins)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_PLUGINS)
endif()

if(TARGET noteAppC_publish)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_publish)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_PUBLISH)
endif()

if(TARGET noteAppC_imageview)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_imageview)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_IMAGEVIEW)
endif()

if(TARGET noteAppC_epub)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_epub)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_EPUB)
endif()

if(TARGET noteAppC_history)
    target_link_libraries(noteAppC_ui PRIVATE noteAppC_history)
    target_compile_definitions(noteAppC_ui PUBLIC NOTEAPPC_HAVE_HISTORY)
endif()
