add_library(noteAppC_vault STATIC
    Snapshots.cpp
    Snapshots.h
    PdfRecovery.cpp
    PdfRecovery.h
    Vault.cpp
    Vault.h
    MomentFormat.cpp
    MomentFormat.h
    Templates.cpp
    Templates.h
    DailyNotes.cpp
    DailyNotes.h
    Bookmarks.cpp
    Bookmarks.h
    Workspaces.cpp
    Workspaces.h
    FileIcons.cpp
    FileIcons.h)

target_include_directories(noteAppC_vault PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_vault PUBLIC Qt6::Core noteAppC_settings)
