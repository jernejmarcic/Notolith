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

# macOS: a .app bundle with our own Info.plist (bundle id as in the Linux desktop file, a numeric version as Apple
# requires, so no "-beta" suffix) and an .icns made from the app's SVG. The icon needs rsvg-convert (librsvg) and
# iconutil at configure time; without them the bundle is built iconless rather than failing.
if(APPLE)
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_ICON_FILE Notolith)
    set(noteappc_iconset ${CMAKE_CURRENT_BINARY_DIR}/Notolith.iconset)
    set(noteappc_icns ${CMAKE_CURRENT_BINARY_DIR}/Notolith.icns)
    find_program(NOTEAPPC_RSVG rsvg-convert)
    find_program(NOTEAPPC_ICONUTIL iconutil)
    if(NOTEAPPC_RSVG AND NOTEAPPC_ICONUTIL)
        file(MAKE_DIRECTORY ${noteappc_iconset})
        # The sizes iconutil expects: 16 to 512 pt, each also at 2x.
        foreach(size 16 32 128 256 512)
            math(EXPR double "${size} * 2")
            execute_process(COMMAND ${NOTEAPPC_RSVG} -w ${size} -h ${size}
                    -o ${noteappc_iconset}/icon_${size}x${size}.png ${CMAKE_SOURCE_DIR}/libs/theme/icons/noteAppC.svg)
            execute_process(COMMAND ${NOTEAPPC_RSVG} -w ${double} -h ${double}
                    -o ${noteappc_iconset}/icon_${size}x${size}@2x.png ${CMAKE_SOURCE_DIR}/libs/theme/icons/noteAppC.svg)
        endforeach()
        execute_process(COMMAND ${NOTEAPPC_ICONUTIL} -c icns -o ${noteappc_icns} ${noteappc_iconset}
            RESULT_VARIABLE noteappc_iconutil_result)
    endif()
    if(EXISTS ${noteappc_icns})
        target_sources(noteAppC PRIVATE ${noteappc_icns})
        set_source_files_properties(${noteappc_icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    else()
        message(WARNING "no .icns built (rsvg-convert and iconutil are needed): the macOS bundle has no icon")
    endif()
    set_target_properties(noteAppC PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in)
endif()
