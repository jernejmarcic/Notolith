# The syscall filter the bubblewrap sandboxes share (SyscallFilter.h) and the TeX/diagram command line (Bubblewrap.h).
add_library(noteAppC_sandbox STATIC SyscallFilter.h SyscallFilter.cpp Bubblewrap.h Bubblewrap.cpp)
target_include_directories(noteAppC_sandbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_sandbox PUBLIC Qt6::Core)
if(TARGET PkgConfig::SECCOMP)
    target_link_libraries(noteAppC_sandbox PRIVATE PkgConfig::SECCOMP)
    target_compile_definitions(noteAppC_sandbox PRIVATE NOTEAPPC_HAVE_SECCOMP)
endif()
