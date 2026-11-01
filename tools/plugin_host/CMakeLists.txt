# noteAppC-plugin-host: one process per enabled plugin (docs/PLUGINS.md,
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
