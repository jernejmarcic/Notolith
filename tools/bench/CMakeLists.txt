# Self-contained: the parent build wires this in with one line,
#     add_subdirectory(bench)
# added to tools/CMakeLists.txt, next to the noteAppC_shot and
# noteAppC_index_cli targets it already builds the same way.
qt_add_executable(noteAppC-bench Bench.cpp)
target_link_libraries(noteAppC-bench PRIVATE noteAppC_ui Qt6::Test)

# EPUB open/scroll rows, only when the module is built (see the root
# CMakeLists.txt) - the same guarded-link shape tests/CMakeLists.txt uses.
if(TARGET noteAppC_epub)
    target_link_libraries(noteAppC-bench PRIVATE noteAppC_epub PkgConfig::LIBARCHIVE)
    target_compile_definitions(noteAppC-bench PRIVATE NOTEAPPC_HAVE_EPUB)
endif()

# So the tool can find gen_bench_vault.py without guessing the working
# directory the caller ran it from.
target_compile_definitions(noteAppC-bench PRIVATE
    NOTEAPPC_GEN_VAULT_SCRIPT="${CMAKE_SOURCE_DIR}/tools/gen_bench_vault.py")
