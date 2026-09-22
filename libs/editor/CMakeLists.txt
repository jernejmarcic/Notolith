add_library(noteAppC_editor STATIC
    DisplayMap.cpp
    DisplayMap.h)

target_include_directories(noteAppC_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_editor PUBLIC Qt6::Gui noteAppC_markdown)
