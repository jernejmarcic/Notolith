# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# plugins/ai: the first-party AI plugin (docs/PLUGINS.md, Tier 2) - Ollama,
# or an OpenAI-compatible/Anthropic HTTP API. C++ against plugin.hpp's
# wrapper (word-stats is plain C against the bare plugin.h; this plugin
# already needs Qt6::Core for QSettings/QJson, so std::function registration
# costs nothing extra over hand-writing the table).
#
# AiConfig/AiProvider are a separate static library, not folded into the
# module, so tests/test_ai_plugin.cpp can link the HTTP request/response
# logic directly and point it at a fake local server - no plugin host, no
# dlopen, no sandbox in the way for the request-shape and error-handling
# tests; only the sandbox/timeout/secret-handling tests need the real thing.
add_library(noteAppC_ai_provider STATIC AiConfig.h AiConfig.cpp AiProvider.h AiProvider.cpp)
target_include_directories(noteAppC_ai_provider PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_ai_provider PUBLIC Qt6::Core PkgConfig::CURL noteAppC_xdg)

# MODULE, not SHARED: loaded exclusively with dlopen() inside
# notolith-plugin-host, the same as word-stats.
add_library(ai MODULE plugin.cpp)
target_include_directories(ai PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ai PRIVATE noteAppC_ai_provider Qt6::Core)
set_target_properties(ai PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/ai)

# plugin.ini sits next to the built library, the same layout as an installed
# ~/.local/share/notolith/plugins/ai/ directory - PluginManifest and
# tests/test_ai_plugin.cpp both point straight at this build-tree directory.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/plugin.ini
    ${CMAKE_BINARY_DIR}/plugins/ai/plugin.ini COPYONLY)
