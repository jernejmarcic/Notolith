# Handwriting: the core library (core/) that the ink and hwfont modules link.
add_subdirectory(core)

# The tab (InkView) as a static library the module links; Track S1's bench and
# index tests link it too, without dlopen.
# Headers only from the host's static libraries: a second copy of Settings
# inside the .so would consult its own defaults() map, not the one the host
# filled with registerDefaults. The symbols resolve from the executable.
add_library(noteAppC_ink STATIC InkView.cpp InkView.h)
set_target_properties(noteAppC_ink PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(noteAppC_ink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    $<TARGET_PROPERTY:noteAppC_settings,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_theme,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_vault,INTERFACE_INCLUDE_DIRECTORIES>
    # InkView draws a text box with the host's MarkdownEditor (headers only: the editor's symbols
    # come from the executable like the others', never from a second copy in the module).
    $<TARGET_PROPERTY:noteAppC_editor,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_markdown,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_render,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(noteAppC_ink PUBLIC notolith-inkcore noteAppC_modules_headers Qt6::Widgets)

# The module itself: only this target is a Qt plugin. Host symbols (Settings,
# Theme, Vault, ModuleView's vtable) resolve from the executable at load time
# (ENABLE_EXPORTS on every host binary, ResolveAllSymbolsHint in the registry).
configure_file(manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/manifest.json @ONLY)
if(NOTEAPPC_STATIC_MODULES)
    noteappc_add_static_module(notolith-ink-static InkModule.cpp noteAppC_ink)
else()
    add_library(notolith-ink MODULE InkModule.cpp)
    # libnotolith-inkcore.so installs next to this module (core/CMakeLists.txt), so $ORIGIN finds it
    # installed and in the build tree alike. BUILD_WITH_INSTALL_RPATH, not BUILD_RPATH_USE_ORIGIN or
    # CMake's computed build RPATH: with the shared Abseil those come out as a RUNPATH of empty entries,
    # and an empty entry means "the current directory" (the same reasoning as notolith-inkcore).
    set_target_properties(notolith-ink PROPERTIES
        AUTOMOC ON
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON)
    target_include_directories(notolith-ink PRIVATE ${CMAKE_CURRENT_BINARY_DIR}) # manifest.json for Q_PLUGIN_METADATA
    target_link_libraries(notolith-ink PRIVATE noteAppC_ink)
    install(TARGETS notolith-ink LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/noteAppC/modules)
endif()
