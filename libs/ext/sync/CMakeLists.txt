# SPDX-License-Identifier: GPL-3.0-or-later
# Sync, a Tier 1 module (docs/PLUGINS.md, docs/SYNC.md): local-first sync to an optional, end-to-end encrypted
# server. Built only with NOTEAPPC_WITH_SYNC on and both libsodium and
# Qt6Keychain found - see the root CMakeLists.txt.
add_library(noteAppC_sync STATIC
    SecretStore.h
    SecretStore.cpp
    SyncCrypto.h
    SyncCrypto.cpp
    SyncJournal.h
    SyncJournal.cpp
    SyncScanner.h
    SyncScanner.cpp
    SyncMerge.h
    SyncMerge.cpp
    SyncClient.h
    SyncClient.cpp
    SyncEngine.h
    SyncEngine.cpp)

set_target_properties(noteAppC_sync PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_sync
    PUBLIC Qt6::Core Qt6::Network Qt6::Sql noteAppC_vault
    PRIVATE PkgConfig::SODIUM Qt6Keychain::Qt6Keychain)
