# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič

# The link layer for note sharing (docs/SHARE.md): device identity and a
# peer-to-peer Node (libdatachannel, mDNS over avahi). No Qt. core/, net/ and
# proto/ are a vendored copy of the link layer from the author's separate
# Hearth chat project; this file is the only Notolith-owned part. Built
# only when the root CMakeLists.txt found every dependency
# (NOTEAPPC_WITH_SHARE); its tests are registered in tests/CMakeLists.txt.
add_library(noteAppC_link STATIC
    core/identity.cpp
    core/debuglog.cpp
    net/node.cpp
    net/tailnet.cpp
    proto/link.proto)
# linked into the notolith-share module (libs/ext/shareengine), a shared object
set_target_properties(noteAppC_link PROPERTIES POSITION_INDEPENDENT_CODE ON)
# include root for "core/..." and "net/..."; the generated link.pb.h lands in the binary dir
target_include_directories(noteAppC_link PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(TARGET noteAppC_link IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(noteAppC_link PUBLIC protobuf::libprotobuf PkgConfig::SODIUM
    LibDataChannel::LibDataChannel PkgConfig::AVAHI nlohmann_json::nlohmann_json)
