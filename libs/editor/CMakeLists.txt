add_library(noteAppC_editor STATIC
    BaseBlock.cpp
    BaseResult.h
    BlockRenderers.cpp
    DisplayMap.cpp
    DisplayMap.h
    EditingCommands.cpp
    FileEmbedBlock.cpp
    FindBar.cpp
    FindBar.h
    HtmlBlock.cpp
    MarkdownEditor.cpp
    MarkdownEditor.h
    PdfEmbedBlock.cpp
    PdfExport.cpp
    QueryBlock.cpp
    QueryResult.h)

set_target_properties(noteAppC_editor PROPERTIES AUTOMOC ON)

target_include_directories(noteAppC_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_editor PUBLIC Qt6::Widgets noteAppC_markdown noteAppC_theme noteAppC_render noteAppC_vault PRIVATE Qt6::Concurrent)

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
