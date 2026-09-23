qt_add_executable(noteAppC_shot shot.cpp)
target_link_libraries(noteAppC_shot PRIVATE noteAppC_ui)

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
