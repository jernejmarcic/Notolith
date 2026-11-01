# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
qt_add_executable(noteAppC_shot shot.cpp)
target_link_libraries(noteAppC_shot PRIVATE noteAppC_ui)
set_target_properties(noteAppC_shot PROPERTIES ENABLE_EXPORTS ON) # modules resolve host symbols here

add_executable(noteAppC_index_cli index_cli.cpp)
set_target_properties(noteAppC_index_cli PROPERTIES OUTPUT_NAME noteAppC-index)
target_link_libraries(noteAppC_index_cli PRIVATE noteAppC_index noteAppC_vault)

if(TARGET noteAppC_publish)
    add_executable(noteAppC_publish_cli publish_cli.cpp)
    set_target_properties(noteAppC_publish_cli PROPERTIES OUTPUT_NAME noteAppC-publish)
    # QApplication, not QCoreApplication - see the comment in publish_cli.cpp.
    target_link_libraries(noteAppC_publish_cli
        PRIVATE noteAppC_publish noteAppC_index noteAppC_vault Qt6::Widgets)
endif()

# The performance harness: the numbers every optimisation has to beat.
add_subdirectory(bench)

# The pen input test (spec section 10): a hand-run logger, never installed.
add_subdirectory(inkspike)

# The out-of-process plugin host (docs/PLUGINS.md, Tier 2).
if(NOTEAPPC_WITH_PLUGINS)
    add_subdirectory(plugin_host)
endif()
# The reference sync server (docs/SYNC.md), built alongside libs/ext/sync.
if(TARGET noteAppC_sync AND TARGET Qt6::HttpServer)
    add_subdirectory(sync_server)
endif()
