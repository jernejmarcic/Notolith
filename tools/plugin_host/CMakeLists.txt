# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# notolith-plugin-host: one process per enabled plugin (docs/PLUGINS.md,
# Tier 2). Deliberately thin - Qt6::Core, the protocol library, and libdl -
# so a plugin's crash or hang is contained to this binary and costs the app
# nothing but the pipe it was talking over.
add_executable(noteAppC-plugin-host
    main.cpp
    Engine.h
    NativeEngine.cpp
    NativeEngine.h
    HostIO.cpp
    HostIO.h)
target_link_libraries(noteAppC-plugin-host PRIVATE noteAppC_plugin_protocol Qt6::Core ${CMAKE_DL_LIBS})
# Next to the executable in a build tree, in libexec when installed (PluginHost::hostBinaryPath looks in both).
set_target_properties(noteAppC-plugin-host PROPERTIES
    OUTPUT_NAME notolith-plugin-host
    SKIP_BUILD_RPATH OFF INSTALL_RPATH ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/noteAppC)
install(TARGETS noteAppC-plugin-host RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/notolith)
