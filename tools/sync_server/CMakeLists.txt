# SPDX-License-Identifier: GPL-3.0-or-later
# The reference server for docs/SYNC.md. Built only alongside
# noteAppC_sync (libs/ext/sync) with Qt6HttpServer found - see the
# NOTEAPPC_WITH_SYNC block in the root CMakeLists.txt and the guard in
# tools/CMakeLists.txt that adds this directory.
#
# SyncCrypto's two files are compiled in directly rather than linking the
# whole noteAppC_sync library: the server only ever needs the primitives
# (hashing/verifying a login key, deriving a salt), and linking the client
# library would drag in Qt6Keychain and QNetworkAccessManager for no
# reason - a server has no OS keyring and makes no outgoing HTTP calls.
set(sync_lib_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ext/sync)
add_executable(noteAppC_sync_server
    main.cpp
    ServerStore.h
    ServerStore.cpp
    SyncServer.h
    SyncServer.cpp
    ${sync_lib_dir}/SyncCrypto.h
    ${sync_lib_dir}/SyncCrypto.cpp)
set_target_properties(noteAppC_sync_server PROPERTIES
    OUTPUT_NAME noteAppC-sync-server
    AUTOMOC ON)
target_include_directories(noteAppC_sync_server PRIVATE ${sync_lib_dir})
target_link_libraries(noteAppC_sync_server PRIVATE
    Qt6::Core Qt6::Network Qt6::HttpServer Qt6::Sql
    PkgConfig::SODIUM)
