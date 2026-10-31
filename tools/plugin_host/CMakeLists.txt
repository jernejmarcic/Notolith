# noteAppC-plugin-host: one process per enabled plugin (docs/PLUGINS.md,
# Tier 2). Deliberately thin - Qt6::Core, the protocol library, and libdl -
# so a plugin's crash or hang is contained to this binary and costs the app
# nothing but the pipe it was talking over.
add_executable(noteAppC-plugin-host
    main.cpp
    Engine.h
    NativeEngine.cpp
    NativeEngine.h
    QuickJSEngine.h
    HostIO.cpp
    HostIO.h)
target_link_libraries(noteAppC-plugin-host PRIVATE noteAppC_plugin_protocol Qt6::Core ${CMAKE_DL_LIBS})

# The documented seam (QuickJSEngine.h), not an implementation: turning this
# on always fails the configure step right now, with a message that says
# why, rather than a confusing link error deep into the build. This
# codebase never fetches a dependency on its own - see QuickJSEngine.h.
option(NOTEAPPC_WITH_QUICKJS "Load Tier 2 plugins written in JS via quickjs-ng (not implemented yet)" OFF)
if(NOTEAPPC_WITH_QUICKJS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(QUICKJS QUIET IMPORTED_TARGET quickjs-ng)
    endif()
    if(NOT TARGET PkgConfig::QUICKJS)
        message(FATAL_ERROR
            "NOTEAPPC_WITH_QUICKJS is ON but quickjs-ng was not found via pkg-config. "
            "noteAppC never fetches a dependency on its own initiative - "
            "install quickjs-ng yourself first, or turn this option back OFF.")
    endif()
    message(FATAL_ERROR
        "NOTEAPPC_WITH_QUICKJS: quickjs-ng was found, but QuickJSEngine.cpp does not "
        "exist yet - tools/plugin_host/QuickJSEngine.h documents the seam it would fill. "
        "Turn this option back OFF until that engine is actually written.")
endif()
