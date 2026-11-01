# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# The handwriting-font glyph studio: the data layer (GlyphSet, GlyphStore), the studio view
# (GlyphStudio) as a static library, and the notolith-hwfont module that registers it.
# Links notolith-inkcore only, never noteAppC_ink: notolith-hwfont depends on
# the .ink format and the host, not on the notolith-ink module (spec section 9).
# Headers only from the host's static libraries; their symbols resolve from the
# executable that loads this (Vault, Settings and Theme are the host's, as in noteAppC_ink).
add_library(noteAppC_hwfont STATIC GlyphSet.cpp GlyphSet.h GlyphStore.cpp GlyphStore.h)
target_sources(noteAppC_hwfont PRIVATE GlyphStudio.cpp GlyphStudio.h)
set_target_properties(noteAppC_hwfont PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(noteAppC_hwfont PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    $<TARGET_PROPERTY:noteAppC_settings,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_theme,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_vault,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(noteAppC_hwfont PUBLIC notolith-inkcore noteAppC_modules_headers Qt6::Widgets)

# The module itself, built like notolith-ink (libs/ext/ink/CMakeLists.txt): its own install unit that
# links the core library the package notolith-ink installs next to it. No manifest "dependencies" key
# (a dependency check would only repeat the loader): with notolith-ink missing, the load fails with the dynamic linker's message.
configure_file(manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/manifest.json @ONLY)
if(NOTEAPPC_STATIC_MODULES)
    noteappc_add_static_module(notolith-hwfont-static HwfontModule.cpp noteAppC_hwfont)
else()
    add_library(notolith-hwfont MODULE HwfontModule.cpp)
    set_target_properties(notolith-hwfont PROPERTIES
        AUTOMOC ON
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON)
    target_include_directories(notolith-hwfont PRIVATE ${CMAKE_CURRENT_BINARY_DIR}) # manifest.json for Q_PLUGIN_METADATA
    target_link_libraries(notolith-hwfont PRIVATE noteAppC_hwfont)
    install(TARGETS notolith-hwfont LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/notolith/modules)
endif()
