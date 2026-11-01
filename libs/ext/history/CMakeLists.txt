# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# Note history in git, a Tier 1 module (docs/PLUGINS.md): built only with
# NOTEAPPC_WITH_HISTORY on and libgit2 found. Nothing else links libgit2.
add_library(noteAppC_history STATIC
    NoteHistory.cpp
    NoteHistory.h)

target_include_directories(noteAppC_history PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_history
    PUBLIC Qt6::Core
    PRIVATE PkgConfig::LIBGIT2 noteAppC_xdg)
