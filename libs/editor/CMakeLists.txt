add_library(noteAppC_editor STATIC
    DisplayMap.cpp
    DisplayMap.h
    MarkdownEditor.cpp
    MarkdownEditor.h)

set_target_properties(noteAppC_editor PROPERTIES AUTOMOC ON)

target_include_directories(noteAppC_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_editor PUBLIC Qt6::Widgets noteAppC_markdown noteAppC_theme)
