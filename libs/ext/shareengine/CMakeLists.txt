# The sharing engine (docs/SHARE.md "The engine"): device keys, the wire format,
# and the engine that speaks to other devices over the link layer, on top of the
# CRDT. No editor and no window in here. It is the notolith-share loadable module
# (docs/PLUGINS.md): the executable links none of protobuf, Abseil, libdatachannel
# or avahi, and loads this the first time a vault has a share or the user asks for
# one. The window sees it as ShareEngineBase (libs/ext/share); the per-vault store
# of shares (ShareStore) is there too, shared by both sides.
# Built only when the CRDT (libs/ext/share), the link (libs/ext/link) and sync's
# crypto and keyring wrapper (libs/ext/sync) all are - see the root
# CMakeLists.txt. Sharing therefore needs libsodium and Qt6Keychain, which
# sync needs for the same two jobs: keeping a key, and sealing with it.
add_library(noteAppC_shareengine STATIC
    ShareIdentity.h
    ShareIdentity.cpp
    ShareFrame.h
    ShareFrame.cpp
    ShareEngine.h
    ShareEngine.cpp
    share.proto)
set_target_properties(noteAppC_shareengine PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
# the generated share.pb.h lands in the binary dir
target_include_directories(noteAppC_shareengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
    $<TARGET_PROPERTY:noteAppC_sync,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_share_crdt,INTERFACE_INCLUDE_DIRECTORIES>)
protobuf_generate(TARGET noteAppC_shareengine IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
# Headers only from the host's libraries (the ink rule, libs/ext/ink): a second CrdtDoc or SyncCrypto inside
# the .so would be a second copy of the tables, and a CrdtDoc * crosses the boundary through doc(). Their
# symbols and ShareEngineBase's moc come from the executable (ENABLE_EXPORTS, ResolveAllSymbolsHint), so a
# binary that links this library - test_share - names noteAppC_sharebase, noteAppC_share_crdt and
# noteAppC_sync itself.
target_link_libraries(noteAppC_shareengine
    PUBLIC Qt6::Core Qt6::Network noteAppC_sharebase_headers noteAppC_link noteAppC_xdg_headers)

configure_file(manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/manifest.json @ONLY)
# BUILD_WITH_INSTALL_RPATH for the reason the executable gives (apps/noteAppC/CMakeLists.txt): CMake's
# computed build RPATH would carry empty entries, and an empty entry means "the current directory".
add_library(notolith-share MODULE ShareModule.cpp)
set_target_properties(notolith-share PROPERTIES
    AUTOMOC ON
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH ON)
target_include_directories(notolith-share PRIVATE ${CMAKE_CURRENT_BINARY_DIR}) # manifest.json for Q_PLUGIN_METADATA
target_link_libraries(notolith-share PRIVATE noteAppC_shareengine noteAppC_modules_headers)
install(TARGETS notolith-share LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/notolith/modules)
