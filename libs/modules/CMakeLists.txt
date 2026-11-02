# SPDX-License-Identifier: GPL-3.0-or-later
# The loadable-module tier (docs/PLUGINS.md, "Tier 1b"):
# the plugin interface a module implements, the ModuleHost seam struct the
# window fills, the ModuleView widget base, and the registry that discovers
# modules by metadata and loads one on first use.
#
# Two targets on purpose. noteAppC_modules_headers is INTERFACE: a module
# links it for the headers alone and resolves ModuleView's metaobject from
# the executable (ENABLE_EXPORTS), so there is exactly one copy. noteAppC_ui
# links the STATIC noteAppC_modules, which carries the moc and the registry.
add_library(noteAppC_modules_headers INTERFACE)
target_include_directories(noteAppC_modules_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_modules_headers INTERFACE Qt6::Widgets)

configure_file(ModuleABI.h.in ${CMAKE_BINARY_DIR}/generated/ModuleABI.h @ONLY)
add_library(noteAppC_modules STATIC
    ModuleHost.h
    ModuleRegistry.cpp
    ModuleRegistry.h
    NotolithModule.h)
set_target_properties(noteAppC_modules PROPERTIES AUTOMOC ON)
target_link_libraries(noteAppC_modules PUBLIC noteAppC_modules_headers noteAppC_settings noteAppC_theme noteAppC_vault)
target_include_directories(noteAppC_modules PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(noteAppC_modules PRIVATE
    NOTEAPPC_MODULE_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/notolith/modules")

# Q_PLUGIN_METADATA FILE "manifest.json" puts the manifest (the ABI tag) into moc's output. moc reruns when the
# manifest changes, but the object that includes the .moc is judged stale before moc has run, so a version bump
# relinked it with the old tag until a second build. Making the manifest an object dependency of the module's
# source marks it stale in the first. Call it in the directory that creates the module.
#   noteappc_module_manifest_dependency(<module source> <manifest.json>)
function(noteappc_module_manifest_dependency source manifest)
    set_property(SOURCE ${source} APPEND PROPERTY OBJECT_DEPENDS ${manifest})
endfunction()

# A module as a Qt static plugin (NOTEAPPC_STATIC_MODULES): the module's one source file compiled with
# QT_STATICPLUGIN into a static library, which the executable links and imports with Q_IMPORT_PLUGIN.
# ModuleRegistry::scan lists it through QPluginLoader::staticPlugins(); nothing
# is constructed until the first ensureLoaded. The caller configure_file's manifest.json into its own binary
# directory first, as for the shared build (Q_PLUGIN_METADATA FILE "manifest.json").
#   noteappc_add_static_module(<target> <source> <library the module needs>)
# The target is named in the NOTEAPPC_STATIC_MODULE_TARGETS global property for the executable to link.
function(noteappc_add_static_module target source library)
    add_library(${target} STATIC ${source})
    set_target_properties(${target} PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(${target} PRIVATE QT_STATICPLUGIN)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(${target} PUBLIC ${library})
    noteappc_module_manifest_dependency(${source} ${CMAKE_CURRENT_BINARY_DIR}/manifest.json)
    set_property(GLOBAL APPEND PROPERTY NOTEAPPC_STATIC_MODULE_TARGETS ${target})
endfunction()
