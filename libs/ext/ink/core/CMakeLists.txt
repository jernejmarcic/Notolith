# The ink core (spec section 7): the model, the .ink format, pen input through
# ink-stroke-modeler, the outline renderer and the canvas widget. A SHARED
# library on purpose: notolith-ink.so and notolith-hwfont.so both link it and
# find it next to themselves (RPATH $ORIGIN); the base app never does.
find_package(ZLIB REQUIRED) # the gzip sniff (spec section 5): qUncompress reads no gzip header

# Static archives linked into a shared library need PIC - set before the
# modeler and its Abseil are declared, so they inherit it.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INK_STROKE_MODELER_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# Abseil is the system one (20260817 on Arch) and required: the root CMakeLists
# checked find_package(absl) before adding this directory. The modeler is
# told to use it and brings none of its own. If the modeler does not build
# against the system Abseil, the ink core stays off until the modeler pin is
# moved (spec section 13).
set(INK_STROKE_MODELER_FIND_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_FIND_ABSL ON CACHE BOOL "" FORCE)
# The modeler's CMake asks find_package(googletest) even with its tests off, and no
# distro ships a "googletest" config (the system one is GTest). Hand it a stub that
# defers to the GTest the rest of the tree already uses, so nothing is fetched.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/googletest-shim/googletest-config.cmake
    "find_package(GTest CONFIG REQUIRED)\n")
set(googletest_DIR ${CMAKE_CURRENT_BINARY_DIR}/googletest-shim CACHE PATH "" FORCE)
include(FetchContent)
FetchContent_Declare(ink_stroke_modeler
    GIT_REPOSITORY https://github.com/google/ink-stroke-modeler.git
    GIT_TAG f2388813b0b25bc3e33d143d369a8367ab2e30c8 # main, 2026-10-21; Apache-2.0
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(ink_stroke_modeler)

# Shared, unless the modules are static plugins: then there is no directory beside them to find it in.
if(NOTEAPPC_STATIC_MODULES)
    set(noteappc_inkcore_kind STATIC)
else()
    set(noteappc_inkcore_kind SHARED)
endif()
add_library(notolith-inkcore ${noteappc_inkcore_kind}
    InkCanvas.cpp
    InkCanvas.h
    InkCanvasSelection.cpp
    InkDocument.cpp
    InkDocument.h
    InkFormat.cpp
    InkFormat.h
    InkInput.cpp
    InkInput.h
    InkTools.cpp
    InkTools.h
    InkTypes.cpp
    InkTypes.h
    PagePatterns.cpp
    PagePatterns.h
    SpatialGrid.cpp
    SpatialGrid.h
    StrokeOutline.cpp
    StrokeOutline.h
    TileCache.cpp
    TileCache.h)
set_target_properties(notolith-inkcore PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME notolith-inkcore
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH ON)
# Not BUILD_RPATH_USE_ORIGIN, and not CMake's computed build RPATH: with the shared Abseil
# (whose libraries carry RUNPATH $ORIGIN) both come out as a RUNPATH of empty entries
# ("-Wl,-rpath,:::::::"), and an empty entry means "the current directory": a planted
# libz.so.1 would load. $ORIGIN is the library's own directory, in the build tree
# (modules/) and installed alike, which is where the modules that link it sit.
target_include_directories(notolith-inkcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(notolith-inkcore
    PUBLIC Qt6::Widgets Qt6::Concurrent
    PRIVATE InkStrokeModeler::stroke_modeler ZLIB::ZLIB)
# The modeler sets its include root directory-wide, not on its target, so name it here:
# its headers are included as <ink_stroke_modeler/...>.
target_include_directories(notolith-inkcore PRIVATE ${ink_stroke_modeler_SOURCE_DIR})
if(NOT NOTEAPPC_STATIC_MODULES)
    install(TARGETS notolith-inkcore LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/notolith/modules)
endif()
