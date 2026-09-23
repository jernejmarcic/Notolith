add_library(noteAppC_editor STATIC
    BaseBlock.cpp
    BaseResult.h
    BlockRenderers.cpp
    DisplayMap.cpp
    DisplayMap.h
    EditingCommands.cpp
    CodeBlockHook.cpp
    FileEmbedBlock.cpp
    FindBar.cpp
    FindBar.h
    FootnoteMarks.cpp
    HtmlBlock.cpp
    MarkdownEditor.cpp
    MarkdownEditor.h
    NotebookBlock.cpp
    NotebookEmbed.cpp
    PasteDrop.cpp
    TableCells.cpp
    TouchInput.cpp
    PdfEmbedBlock.cpp
    PdfExport.cpp
    QueryBlock.cpp
    QueryResult.h
    SlashCommands.cpp)

set_target_properties(noteAppC_editor PROPERTIES AUTOMOC ON)

target_include_directories(noteAppC_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Qt6::Svg is NotebookBlock.cpp's own: an image/svg+xml kernel output
# (matplotlib's default figure format, among others) is rasterised with
# QSvgRenderer rather than shown as raw markup or skipped.
# noteAppC_index for Fuzzy::score alone (SlashCommands.cpp): it depends on
# neither noteAppC_editor nor noteAppC_ui, so linking it here is not a cycle,
# and it is the command palette's own matcher rather than a second one.
target_link_libraries(noteAppC_editor PUBLIC Qt6::Widgets noteAppC_markdown noteAppC_theme noteAppC_render noteAppC_vault noteAppC_index PRIVATE Qt6::Concurrent Qt6::Svg)

# An OpenGL viewport, so the GPU question can be answered with a measurement
# rather than an opinion. Optional: a build without Qt6::OpenGLWidgets simply
# reports the option as unavailable and keeps the raster path.
if(TARGET Qt6::OpenGLWidgets)
    target_link_libraries(noteAppC_editor PRIVATE Qt6::OpenGLWidgets)
    target_compile_definitions(noteAppC_editor PRIVATE NOTEAPPC_HAVE_OPENGLWIDGETS)
    message(STATUS "noteAppC: OpenGL viewport available")
else()
    message(STATUS "noteAppC: Qt6::OpenGLWidgets not found, raster viewport only")
endif()
