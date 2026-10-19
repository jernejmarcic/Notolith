# The text CRDT behind note sharing (docs/SHARE.md). Built only with
# NOTEAPPC_WITH_SHARE_YRS on and cargo found - see the root CMakeLists.txt.
#
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

add_library(noteAppC_share_crdt STATIC
    CrdtDoc.h
    CrdtDoc.cpp
    CrdtDocYrs.cpp)
target_include_directories(noteAppC_share_crdt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_share_crdt PUBLIC Qt6::Core PRIVATE noteAppC_yrs_ffi)
