add_library(noteAppC_vault STATIC
    Snapshots.cpp
    Snapshots.h
    Vault.cpp
    Vault.h)

target_include_directories(noteAppC_vault PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_vault PUBLIC Qt6::Core)
