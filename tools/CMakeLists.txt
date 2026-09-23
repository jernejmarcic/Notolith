qt_add_executable(noteAppC_shot shot.cpp)
target_link_libraries(noteAppC_shot PRIVATE noteAppC_ui)

add_executable(noteAppC_index_cli index_cli.cpp)
set_target_properties(noteAppC_index_cli PROPERTIES OUTPUT_NAME noteAppC-index)
target_link_libraries(noteAppC_index_cli PRIVATE noteAppC_index noteAppC_vault)

# The performance harness: the numbers every optimisation has to beat.
add_subdirectory(bench)

# The reference sync server (docs/SYNC.md), built alongside libs/ext/sync.
if(TARGET noteAppC_sync AND TARGET Qt6::HttpServer)
    add_subdirectory(sync_server)
endif()
