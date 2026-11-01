# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# The XDG directories, the INI reader and the log file: Core only, so every library down to the
# vault and the plugins can use them without the Markdown parser.
# noteAppC_xdg_headers: for a loadable module, which resolves these functions from the executable.
add_library(noteAppC_xdg_headers INTERFACE)
target_include_directories(noteAppC_xdg_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_xdg_headers INTERFACE Qt6::Core)
add_library(noteAppC_xdg STATIC ConfigFile.cpp ConfigFile.h MessageLog.cpp MessageLog.h)
target_link_libraries(noteAppC_xdg PUBLIC noteAppC_xdg_headers)
set_target_properties(noteAppC_xdg PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(noteAppC_markdown STATIC
    SourceMap.cpp
    SourceMap.h
    Frontmatter.cpp
    Frontmatter.h
    ObsidianSyntax.cpp
    ObsidianSyntax.h
    Metadata.h
    Decorations.cpp
    Decorations.h
    Parser.cpp
    Parser.h
    TableFormat.cpp
    TableFormat.h
    TaskStates.cpp
    TaskStates.h
    SafeSvg.cpp
    SafeSvg.h)

target_include_directories(noteAppC_markdown PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_markdown PUBLIC noteAppC_xdg Qt6::Core md4qt::md4qt PRIVATE yaml-cpp::yaml-cpp)
