# Pandoc export and import. pandoc is an optional runtime tool, never a build
# dependency: this library only prepares a note's Markdown for it, builds its
# argument lists and runs it as a QProcess. Always built; with the "pandoc"
# core plugin off nothing in here is ever called, and the tool is looked up
# only when a pandoc command is used.
add_library(noteAppC_pandoc STATIC
    Pandoc.h
    Pandoc.cpp
    PandocPrepare.h
    PandocPrepare.cpp
    PandocRunner.h
    PandocRunner.cpp)

set_target_properties(noteAppC_pandoc PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_pandoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_pandoc PUBLIC Qt6::Core noteAppC_markdown noteAppC_vault)
