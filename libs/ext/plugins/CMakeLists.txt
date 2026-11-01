# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# Tier 2 (docs/PLUGINS.md): runtime plugins, loaded only inside a separate
# `notolith-plugin-host` process, never in the app. Split into two targets so
# the host tool can link "only Qt6::Core and a small protocol library"
# rather than the app-side QProcess
# orchestration it has no use for.

# The framing (PluginProtocol) and plugin.ini parsing (PluginManifest) that
# both the app and every plugin host need. Qt6::Core only - no widgets, no
# process management - so tools/plugin_host stays small and has nothing in it
# that could need a display.
add_library(noteAppC_plugin_protocol STATIC
    PluginProtocol.cpp
    PluginProtocol.h
    PluginManifest.cpp
    PluginManifest.h)
target_include_directories(noteAppC_plugin_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(noteAppC_plugin_protocol PUBLIC Qt6::Core)
# A plugin may be granted the network namespace only in a build with NOTEAPPC_WITH_NETWORK (PluginHost.cpp).
if(NOTEAPPC_WITH_NETWORK)
    target_compile_definitions(noteAppC_plugin_protocol PUBLIC NOTEAPPC_HAVE_NETWORK)
endif()

# The app-side orchestration: one PluginHost (a sandboxed QProcess) per
# enabled plugin, owned by a PluginRegistry. Only the app links this - the
# host tool builds its own engine (tools/plugin_host/Engine.h) on top of the
# protocol library instead.
add_library(noteAppC_plugins STATIC
    PluginHost.cpp
    PluginHost.h
    PluginRegistry.cpp
    PluginRegistry.h)
# PluginHost is a QObject with signals (stateChanged, permissionRefused) -
# needs moc, the same as every other Q_OBJECT library in libs/ (see
# noteAppC_ui, noteAppC_editor, noteAppC_index's CMakeLists.txt).
set_target_properties(noteAppC_plugins PROPERTIES AUTOMOC ON)
target_link_libraries(noteAppC_plugins PUBLIC noteAppC_plugin_protocol Qt6::Core PRIVATE noteAppC_sandbox noteAppC_xdg)
# Where the installed notolith-plugin-host lives (tools/plugin_host installs it there); PluginHost::hostBinaryPath.
target_compile_definitions(noteAppC_plugins PRIVATE NOTEAPPC_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}/notolith")
# Where PluginRegistry also looks for plugins: the AI plugin installs there (the root CMakeLists.txt).
target_compile_definitions(noteAppC_plugins PRIVATE NOTEAPPC_PLUGINS_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/notolith/plugins")
