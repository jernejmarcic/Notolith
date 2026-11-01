# A Tier 1 module (docs/PLUGINS.md): Jupyter-style code execution.
# Built only with NOTEAPPC_WITH_NOTEBOOK on and libzmq found (root
# CMakeLists.txt) - PkgConfig::ZMQ is libzmq's own pkg-config name, kept as
# the target name every other optional dependency here uses (SODIUM,
# HUNSPELL, LIBGIT2...).
add_library(noteAppC_notebook STATIC
    NotebookMessage.h
    NotebookMessage.cpp
    KernelSpec.h
    KernelSpec.cpp
    NotebookSandbox.h
    NotebookSandbox.cpp
    KernelProcess.h
    KernelProcess.cpp
    KernelClient.h
    KernelClient.cpp
    NotebookCache.h
    NotebookCache.cpp
    NotebookOutputs.h
    NotebookOutputs.cpp
    Ipynb.h
    Ipynb.cpp
    NotebookController.h
    DirectRunner.h
    DirectRunner.cpp
    NotebookController.cpp)

# KernelClient is a QThread and NotebookController a QObject, both with
# signals - needs moc, like every other Q_OBJECT library under libs/.
set_target_properties(noteAppC_notebook PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_notebook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_notebook PUBLIC Qt6::Core PRIVATE PkgConfig::ZMQ noteAppC_sandbox)
