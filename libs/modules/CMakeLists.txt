# The loadable-module tier (docs/PLUGINS.md, "Tier 1.5"; spec section 4):
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
    NOTEAPPC_MODULE_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/noteAppC/modules")
