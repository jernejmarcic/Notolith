# SPDX-License-Identifier: GPL-3.0-or-later
# Encrypted notes (docs/ENCRYPTION.md), a Tier 1 module: `.mdlock` files
# sealed with Argon2id and XChaCha20-Poly1305 through libsodium, decrypted
# in memory only. Built with NOTEAPPC_WITH_ENCRYPTION on and libsodium found.
add_library(noteAppC_crypt STATIC
    NoteCrypt.h
    NoteCrypt.cpp
    EncryptionSession.h
    EncryptionSession.cpp)
set_target_properties(noteAppC_crypt PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_crypt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_crypt PUBLIC Qt6::Core PRIVATE PkgConfig::SODIUM)
