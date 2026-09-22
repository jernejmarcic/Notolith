find_package(KF6SyntaxHighlighting REQUIRED)

add_library(noteAppC_render STATIC
    CodeHighlighter.cpp
    CodeHighlighter.h)

target_include_directories(noteAppC_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_render
    PUBLIC Qt6::Gui noteAppC_theme
    PRIVATE KF6::SyntaxHighlighting)
