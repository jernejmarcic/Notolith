qt_add_executable(noteAppC main.cpp)

target_compile_definitions(noteAppC PRIVATE NOTEAPPC_VERSION="${PROJECT_VERSION}")
target_link_libraries(noteAppC PRIVATE noteAppC_ui)
