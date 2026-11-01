# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# libFuzzer targets for the parsers that read a vault's files. Opt-in
# (NOTEAPPC_FUZZ), clang only, never installed. README.md, Development, has how to run them.
add_library(noteAppC_fuzz_flags INTERFACE)
target_link_options(noteAppC_fuzz_flags INTERFACE -fsanitize=fuzzer)

function(noteappc_fuzz_target name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE noteAppC_fuzz_flags ${ARGN})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

noteappc_fuzz_target(fuzz_markdown noteAppC_markdown)
noteappc_fuzz_target(fuzz_frontmatter noteAppC_markdown)
noteappc_fuzz_target(fuzz_csv noteAppC_editor)
noteappc_fuzz_target(fuzz_obsidian noteAppC_vault noteAppC_settings)
if(TARGET noteAppC_canvas)
    noteappc_fuzz_target(fuzz_canvas noteAppC_canvas)
endif()
if(TARGET noteAppC_notebook)
    noteappc_fuzz_target(fuzz_ipynb noteAppC_notebook)
endif()
if(TARGET noteAppC_bases)
    noteappc_fuzz_target(fuzz_bases noteAppC_bases)
endif()
