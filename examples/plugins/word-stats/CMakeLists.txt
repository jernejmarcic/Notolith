# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# The root project() only declares CXX (every other target here is C++) -
# this is the one place in the tree that needs a C compiler, so it is
# enabled here rather than widening the top-level project() declaration for
# everything else.
enable_language(C)

# A plain C shared library against the public C ABI only - no noteAppC
# library linked, the same as any third-party plugin would build. MODULE
# rather than SHARED: this is loaded exclusively with dlopen(), never linked
# against by another target.
add_library(word-stats MODULE word_stats.c)
target_include_directories(word-stats PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(word-stats PROPERTIES
    C_STANDARD 11
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/plugins/word-stats)

# plugin.ini sits next to the built library, the same layout as an installed
# `~/.local/share/notolith/plugins/<id>/` directory - PluginManifest and the
# tests both point straight at this build-tree directory with --plugin.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/plugin.ini
    ${CMAKE_BINARY_DIR}/examples/plugins/word-stats/plugin.ini COPYONLY)
