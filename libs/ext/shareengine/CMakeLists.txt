# The sharing engine (docs/SHARE.md "The engine"): device keys, the wire format,
# the per-vault store of shares and the engine that speaks to other devices
# over the link layer, on top of the CRDT. No editor and no window in here.
# Built only when the CRDT (libs/ext/share), the link (libs/ext/link) and sync's
# crypto and keyring wrapper (libs/ext/sync) all are - see the root
# CMakeLists.txt. Sharing therefore needs libsodium and Qt6Keychain, which
# sync needs for the same two jobs: keeping a key, and sealing with it.
add_library(noteAppC_shareengine STATIC
    ShareIdentity.h
    ShareIdentity.cpp
    ShareFrame.h
    ShareFrame.cpp
    ShareStore.h
    ShareStore.cpp
    ShareEngine.h
    ShareEngine.cpp
    share.proto)
set_target_properties(noteAppC_shareengine PROPERTIES AUTOMOC ON)
# the generated share.pb.h lands in the binary dir
target_include_directories(noteAppC_shareengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(TARGET noteAppC_shareengine IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(noteAppC_shareengine
    PUBLIC Qt6::Core Qt6::Network noteAppC_share_crdt noteAppC_sync noteAppC_link)
