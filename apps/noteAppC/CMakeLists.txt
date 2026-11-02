# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_executable(noteAppC main.cpp)

target_compile_definitions(noteAppC PRIVATE NOTEAPPC_VERSION="${NOTEAPPC_VERSION_STRING}")
target_link_libraries(noteAppC PRIVATE noteAppC_ui)
# Static modules (NOTEAPPC_STATIC_MODULES): linked in.
if(NOTEAPPC_STATIC_MODULES)
    # Read at configure time: a static module's directory must be add_subdirectory'd before apps/noteAppC,
    # or this links nothing (and the build still passes, the module simply absent).
    get_property(noteappc_static_modules GLOBAL PROPERTY NOTEAPPC_STATIC_MODULE_TARGETS)
    target_link_libraries(noteAppC PRIVATE ${noteappc_static_modules})
    target_compile_definitions(noteAppC PRIVATE NOTEAPPC_STATIC_MODULES)
endif()
# Loadable modules (libs/modules) resolve the editor, theme and vault symbols
# from this executable at load time, so they must be exported.
if(NOT ANDROID)
    set_target_properties(noteAppC PROPERTIES ENABLE_EXPORTS ON)
    # No RUNPATH at all. Qt's default install RPATH ($ORIGIN:$ORIGIN/../lib) is
    # for a layout this binary does not ship in: it needs nothing from beside
    # itself (the modules are dlopen'ed by path), yet CMake pads the build
    # RUNPATH to its length with empty entries, and an empty entry means "the
    # current directory" (a planted libz.so.1 would load), and the installed
    # copy probes ../lib, a directory a user can write, before the system's.
    # Every direct dependency cost a failed open per entry. AppImage packaging
    # (linuxdeploy) writes its own RPATH.
    set_target_properties(noteAppC PROPERTIES INSTALL_RPATH "" BUILD_WITH_INSTALL_RPATH ON)
endif()
# libzmq (the notebook's, behind gnutls and p11-kit) is loaded on first use:
# zmq_lazy.cpp defines the functions the notebook calls and, ahead of libzmq on
# the link line, leaves it out of NEEDED under --as-needed (root CMakeLists.txt).
if(TARGET noteAppC_notebook AND TARGET PkgConfig::ZMQ AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(noteAppC PRIVATE zmq_lazy.cpp)
    target_link_libraries(noteAppC PRIVATE PkgConfig::ZMQ)
endif()
# The product name; the target keeps the code name every build script uses.
if(NOT ANDROID)
    set_target_properties(noteAppC PROPERTIES OUTPUT_NAME notolith)
endif()

# Qt's generated manifest would request RECORD_AUDIO, MODIFY_AUDIO_SETTINGS and
# BLUETOOTH purely because Qt Multimedia is linked (audio/video embeds play
# back, never record), and the package/label would otherwise be
# org.qtproject.example.noteAppC / noteAppC - fine as a label, wrong as an id.
# The checked-in android/AndroidManifest.xml fixes both; see docs/PLATFORMS.md.
if(ANDROID)
    set_property(TARGET noteAppC PROPERTY
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/android)
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_VERSION_NAME ${NOTEAPPC_VERSION_STRING})
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_VERSION_CODE 1)
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_MIN_SDK_VERSION 26)
    set_property(TARGET noteAppC PROPERTY QT_ANDROID_TARGET_SDK_VERSION 34)
endif()
