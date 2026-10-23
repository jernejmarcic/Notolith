# The text CRDT behind note sharing (docs/SHARE.md). The Native backend is
# plain C++ on Qt6::Core and is always built; the Yrs backend is added when
# NOTEAPPC_WITH_SHARE_YRS is on and cargo was found - see the root
# CMakeLists.txt. Both are behind the one CrdtDoc interface.
add_library(noteAppC_share_crdt STATIC
    CrdtDoc.h
    CrdtDoc.cpp
    CrdtDocNative.cpp)
target_include_directories(noteAppC_share_crdt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_share_crdt PUBLIC Qt6::Core)

# The surface of the sharing engine the window sees (ShareEngineBase.h) and the per-vault store of
# shares (ShareStore.h, Qt only). The engine itself is libs/ext/shareengine, a loadable module; two
# targets, as libs/modules has: the module links the INTERFACE one for the headers and resolves the
# moc and the store from the executable, which links the STATIC one - so there is exactly one copy.
add_library(noteAppC_sharebase_headers INTERFACE)
target_include_directories(noteAppC_sharebase_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_sharebase_headers INTERFACE Qt6::Core)
add_library(noteAppC_sharebase STATIC
    ShareEngineBase.h
    ShareStore.h
    ShareStore.cpp)
set_target_properties(noteAppC_sharebase PROPERTIES AUTOMOC ON)
# Whole archive: the share module resolves CrdtDoc (create, available) from the executable, which itself
# calls only the virtuals, so a plain archive link would leave the factory and both backends out of it.
target_link_libraries(noteAppC_sharebase PUBLIC noteAppC_sharebase_headers
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,noteAppC_share_crdt>")

if(NOTEAPPC_WITH_SHARE_YRS AND NOTEAPPC_CARGO)
# yrs-ffi is a tiny Rust crate over the yrs library (Cargo.lock pins every
# crate); cargo builds it into the build directory, as a static library this
# one links. Network is touched only if cargo's own cache lacks the crates.
set(yrs_target_dir ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(yrs_lib ${yrs_target_dir}/release/${CMAKE_STATIC_LIBRARY_PREFIX}notolith_yrs_ffi${CMAKE_STATIC_LIBRARY_SUFFIX})
file(GLOB yrs_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/yrs-ffi/src/*.rs)
add_custom_command(
    OUTPUT ${yrs_lib}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${yrs_target_dir}
            ${NOTEAPPC_CARGO} build --release --locked --jobs 4
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/yrs-ffi/Cargo.toml
    DEPENDS ${yrs_sources}
            ${CMAKE_CURRENT_SOURCE_DIR}/yrs-ffi/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/yrs-ffi/Cargo.lock
    COMMENT "Building yrs-ffi with cargo"
    VERBATIM)
add_custom_target(noteAppC_yrs_ffi_build DEPENDS ${yrs_lib})

find_package(Threads REQUIRED)
add_library(noteAppC_yrs_ffi STATIC IMPORTED GLOBAL)
set_target_properties(noteAppC_yrs_ffi PROPERTIES IMPORTED_LOCATION ${yrs_lib})
# What `cargo rustc -- --print native-static-libs` asks for on Linux.
target_link_libraries(noteAppC_yrs_ffi INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
add_dependencies(noteAppC_yrs_ffi noteAppC_yrs_ffi_build)

target_sources(noteAppC_share_crdt PRIVATE CrdtDocYrs.cpp)
target_compile_definitions(noteAppC_share_crdt PRIVATE NOTEAPPC_HAVE_YRS)
target_link_libraries(noteAppC_share_crdt PRIVATE noteAppC_yrs_ffi)
endif()
