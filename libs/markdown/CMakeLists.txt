add_library(noteAppC_markdown STATIC
    SourceMap.cpp
    SourceMap.h
    Frontmatter.cpp
    Frontmatter.h
    ObsidianSyntax.cpp
    ObsidianSyntax.h
    Metadata.h
    Parser.cpp
    Parser.h)

target_include_directories(noteAppC_markdown PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_markdown PUBLIC Qt6::Core md4qt::md4qt PRIVATE yaml-cpp)
