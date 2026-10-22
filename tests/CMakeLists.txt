find_package(GTest REQUIRED)

add_executable(test_vault test_vault.cpp)
target_link_libraries(test_vault PRIVATE noteAppC_vault GTest::gtest GTest::gtest_main)
add_test(NAME vault COMMAND test_vault)

add_executable(test_markdown test_markdown.cpp)
target_link_libraries(test_markdown PRIVATE noteAppC_markdown GTest::gtest GTest::gtest_main)
add_test(NAME markdown COMMAND test_markdown)

add_executable(test_index test_index.cpp)
target_link_libraries(test_index PRIVATE noteAppC_index noteAppC_vault Qt6::Test GTest::gtest)
add_test(NAME index COMMAND test_index)

add_executable(test_search test_search.cpp)
target_link_libraries(test_search PRIVATE noteAppC_index GTest::gtest GTest::gtest_main)
add_test(NAME search COMMAND test_search)

add_executable(test_editor test_editor.cpp)
target_link_libraries(test_editor PRIVATE noteAppC_editor noteAppC_render Qt6::Concurrent Qt6::Test GTest::gtest)
get_property(noteappc_have_poppler GLOBAL PROPERTY NOTEAPPC_HAVE_POPPLER)
if(noteappc_have_poppler)
    target_compile_definitions(test_editor PRIVATE NOTEAPPC_EXPECT_PDF)
endif()
get_property(noteappc_have_litehtml GLOBAL PROPERTY NOTEAPPC_HAVE_LITEHTML)
if(noteappc_have_litehtml)
    target_compile_definitions(test_editor PRIVATE NOTEAPPC_EXPECT_HTML)
endif()
add_test(NAME editor COMMAND test_editor)
# Its ratio budgets (a 1 MB open against a 200 kB one, min of 7) failed once
# under ctest -j4 when neighbours took the CPU: alone, they hold every time.
set_tests_properties(editor PROPERTIES RUN_SERIAL TRUE)

# HTML edited where it is drawn (HtmlEdit.cpp); the minimal build runs it too.
add_executable(test_html_edit test_html_edit.cpp)
target_link_libraries(test_html_edit PRIVATE noteAppC_editor Qt6::Test GTest::gtest)
get_property(noteappc_have_gumbo GLOBAL PROPERTY NOTEAPPC_HAVE_GUMBO)
if(noteappc_have_gumbo)
    target_compile_definitions(test_html_edit PRIVATE NOTEAPPC_EXPECT_HTML_EDIT)
endif()
if(noteappc_have_litehtml)
    target_compile_definitions(test_html_edit PRIVATE NOTEAPPC_EXPECT_HTML)
endif()
add_test(NAME html_edit COMMAND test_html_edit)

# Vim mode drives the editor with key events only; no window is shown.
add_executable(test_vim test_vim.cpp)
target_link_libraries(test_vim PRIVATE noteAppC_editor noteAppC_render Qt6::Test GTest::gtest)
add_test(NAME vim COMMAND test_vim)

# Its own executable, deliberately: see test_pdfpage_race.cpp's header
# comment on why this needs to be the first thing in its process to touch
# PdfPage::render.
if(noteappc_have_poppler)
    add_executable(test_pdfpage_race test_pdfpage_race.cpp)
    target_link_libraries(test_pdfpage_race PRIVATE noteAppC_render Qt6::Widgets GTest::gtest)
    add_test(NAME pdfpage_race COMMAND test_pdfpage_race)
    set_tests_properties(pdfpage_race PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# The loadable-module tier (libs/modules): fake modules built from one or two
# sources, differing in their configure_file'd manifest and a compile flag - a
# good one, one built for another Notolith (wrong ABI tag), a twin claiming the
# same suffix, one whose manifest has no id, a Qt plugin that is not a Notolith
# module, and one whose library cannot resolve a symbol. Headers only from the
# host: no host static library is linked.
function(noteappc_fake_module target id abi suffix source)
    set(FAKE_ID "${id}")
    set(FAKE_ABI "${abi}")
    set(FAKE_SUFFIX "${suffix}")
    configure_file(fakemodule/manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/${target}/manifest.json @ONLY)
    add_library(${target} MODULE fakemodule/${source})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target})
    target_compile_definitions(${target} PRIVATE FAKE_ID="${id}" FAKE_SUFFIX="${suffix}" ${ARGN})
    target_link_libraries(${target} PRIVATE noteAppC_modules_headers Qt6::Widgets)
    set_target_properties(${target} PROPERTIES AUTOMOC ON PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fakemodules)
endfunction()
noteappc_fake_module(fake_good fake "${NOTEAPPC_ABI_TAG}" fake FakeModule.cpp)
noteappc_fake_module(fake_badabi fake-badabi "0.0.0-deadbeef-6.0.0" fake FakeModule.cpp)
noteappc_fake_module(fake_twin fake-twin "${NOTEAPPC_ABI_TAG}" fake FakeModule.cpp)
noteappc_fake_module(fake_noid "" "${NOTEAPPC_ABI_TAG}" fakenoid FakeModule.cpp)
noteappc_fake_module(fake_notmodule fake-notmodule "${NOTEAPPC_ABI_TAG}" fakenotmod NotAModule.cpp)
noteappc_fake_module(fake_unresolved fake-unresolved "${NOTEAPPC_ABI_TAG}" fakeunres FakeModule.cpp FAKE_UNRESOLVED)
set(NOTEAPPC_FAKE_MODULE_TARGETS fake_good fake_badabi fake_twin fake_noid fake_notmodule fake_unresolved)

add_executable(test_modules test_modules.cpp)
target_link_libraries(test_modules PRIVATE noteAppC_modules Qt6::Widgets GTest::gtest)
target_compile_definitions(test_modules PRIVATE NOTEAPPC_FAKE_MODULES_DIR="${CMAKE_CURRENT_BINARY_DIR}/fakemodules")
set_target_properties(test_modules PROPERTIES ENABLE_EXPORTS ON) # the fake module resolves ModuleView here
add_dependencies(test_modules ${NOTEAPPC_FAKE_MODULE_TARGETS})
# The no-load gate is its own ctest entry (M1): in a process where another test had already
# mapped fake_good, "the scan did not load it" would prove nothing. The filter is in the
# command, so it wins over the GTEST_FILTER the run commands pass: run each entry whole.
add_test(NAME modules_gate COMMAND test_modules --gtest_filter=ModuleGateTest.*)
add_test(NAME modules COMMAND test_modules --gtest_filter=-ModuleGateTest.*)
set_tests_properties(modules_gate modules PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_window test_window.cpp)
target_link_libraries(test_window PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
# AStoredValueOfAModuleSettingIsAdoptedWhenTheModuleLoads is its own entry: it needs fake.width
# unregistered when the window is made, and an earlier test in the same process registers it,
# after which the stored value is read without adoptStored(). The filters are in the commands:
# run each entry whole.
set(noteappc_window_adopt WindowTest.AStoredValueOfAModuleSettingIsAdoptedWhenTheModuleLoads)
add_test(NAME window COMMAND test_window --gtest_filter=-${noteappc_window_adopt})
add_test(NAME window_modules_adopt COMMAND test_window --gtest_filter=${noteappc_window_adopt})
# Modules load into the test's process: export the host symbols, and point the
# registry at the build's modules (the fake ones; Task 8 adds the real ones).
set_target_properties(test_window PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_window ${NOTEAPPC_FAKE_MODULE_TARGETS})
set_property(TEST window window_modules_adopt APPEND PROPERTY ENVIRONMENT "NOTEAPPC_MODULE_DIRS=${CMAKE_CURRENT_BINARY_DIR}/fakemodules")
target_compile_definitions(test_window PRIVATE NOTEAPPC_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures")
foreach(module noteAppC_graph noteAppC_canvas noteAppC_bases noteAppC_epub noteAppC_imageview)
    if(TARGET ${module})
        target_link_libraries(test_window PRIVATE ${module})
    endif()
endforeach()
if(TARGET noteAppC_sync)
    target_compile_definitions(test_window PRIVATE NOTEAPPC_EXPECT_SYNC)
    target_link_libraries(test_window PRIVATE noteAppC_sync) # for the in-memory secret store, see main()
endif()
# A real plugin in a real host process, through the window's own wiring.
if(TARGET noteAppC_plugins)
    target_compile_definitions(test_window PRIVATE
        NOTEAPPC_WORD_STATS_PLUGIN_DIR="${CMAKE_BINARY_DIR}/examples/plugins/word-stats"
        NOTEAPPC_PLUGIN_HOST_PATH="$<TARGET_FILE:noteAppC-plugin-host>")
    add_dependencies(test_window noteAppC-plugin-host word-stats)
    # The AI plugin asks a question through the window's own dialog (plugins.cpp's askUser).
    if(TARGET ai)
        target_compile_definitions(test_window PRIVATE NOTEAPPC_AI_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins/ai")
        add_dependencies(test_window ai)
    endif()
endif()

# Mermaid fences through a fake tool: the renderer alone, in an editor, and by the window's setting.
add_executable(test_mermaid test_mermaid.cpp)
target_link_libraries(test_mermaid PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
add_test(NAME mermaid COMMAND test_mermaid)
set_tests_properties(mermaid PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# PlantUML, TikZ and Graphviz fences with the real tools in their sandbox (skipped where a tool is missing).
add_executable(test_diagrams test_diagrams.cpp)
target_link_libraries(test_diagrams PRIVATE noteAppC_ui noteAppC_sandbox Qt6::Test GTest::gtest)
add_test(NAME diagrams COMMAND test_diagrams)
set_tests_properties(diagrams PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_demo test_demo.cpp)
target_link_libraries(test_demo PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
target_compile_definitions(test_demo PRIVATE NOTEAPPC_DEMO_VAULT_PATH="${CMAKE_SOURCE_DIR}/demo"
                                            NOTEAPPC_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures")
foreach(module noteAppC_graph noteAppC_canvas noteAppC_bases noteAppC_epub noteAppC_imageview)
    if(TARGET ${module})
        target_link_libraries(test_demo PRIVATE ${module})
    endif()
endforeach()
add_test(NAME demo COMMAND test_demo)
set_tests_properties(demo PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST window window_modules_adopt APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The DICT client talks to a QTcpServer on loopback, never to a real server.
add_executable(test_lookup test_lookup.cpp)
target_link_libraries(test_lookup PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
add_test(NAME lookup COMMAND test_lookup)

add_executable(test_theme test_theme.cpp)
target_link_libraries(test_theme PRIVATE noteAppC_theme Qt6::Test GTest::gtest GTest::gtest_main)
add_test(NAME theme COMMAND test_theme)

add_executable(test_popup test_popup.cpp)
target_link_libraries(test_popup PRIVATE noteAppC_theme Qt6::Widgets Qt6::Test GTest::gtest)
add_test(NAME popup COMMAND test_popup)
set_tests_properties(popup PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_icons test_icons.cpp)
target_link_libraries(test_icons PRIVATE noteAppC_theme Qt6::Test GTest::gtest GTest::gtest_main)
add_test(NAME icons COMMAND test_icons)
set_tests_properties(icons PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(TARGET noteAppC_spell)
    add_executable(test_spell test_spell.cpp)
    target_link_libraries(test_spell PRIVATE noteAppC_spell GTest::gtest GTest::gtest_main)
    add_test(NAME spell COMMAND test_spell)
endif()

if(TARGET noteAppC_graph)
    add_executable(test_graph test_graph.cpp)
    target_link_libraries(test_graph PRIVATE noteAppC_graph noteAppC_index noteAppC_vault Qt6::Test GTest::gtest)
    add_test(NAME graph COMMAND test_graph)
    set_tests_properties(graph PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET noteAppC_canvas)
    add_executable(test_canvas test_canvas.cpp)
    target_link_libraries(test_canvas PRIVATE noteAppC_canvas Qt6::Test GTest::gtest)
    add_test(NAME canvas COMMAND test_canvas)
    set_tests_properties(canvas PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET noteAppC_bases)
    add_executable(test_bases test_bases.cpp)
    target_link_libraries(test_bases PRIVATE noteAppC_bases noteAppC_index noteAppC_vault Qt6::Test GTest::gtest)
    add_test(NAME bases COMMAND test_bases)
    set_tests_properties(bases PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET noteAppC_epub)
    add_executable(test_epub test_epub.cpp)
    # noteAppC_editor: for Lifeline.h (task 1's off-thread chapter render -
    # EpubView.h now includes it), the same reason noteAppC_render/vault are
    # already listed here directly rather than relied on transitively
    # through noteAppC_epub's own PRIVATE link to them.
    target_link_libraries(test_epub PRIVATE noteAppC_epub noteAppC_render noteAppC_vault noteAppC_editor
                                            PkgConfig::LIBARCHIVE Qt6::Test GTest::gtest)
    add_test(NAME epub COMMAND test_epub)
    set_tests_properties(epub PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET noteAppC_publish)
    add_executable(test_publish test_publish.cpp)
    target_link_libraries(test_publish
        PRIVATE noteAppC_publish noteAppC_index noteAppC_vault Qt6::Concurrent Qt6::Test Qt6::Widgets GTest::gtest)
    add_test(NAME publish COMMAND test_publish)
    set_tests_properties(publish PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET noteAppC_sync AND TARGET Qt6::HttpServer)
    # Builds tools/sync_server's two files straight into the test binary,
    # the same way tools/sync_server/CMakeLists.txt builds SyncCrypto's two
    # files straight into the server: the test wants a real in-process
    # server (the actual route/store code, not a fake), on the same
    # process as the two SyncEngines it drives, over a real loopback
    # socket - not a second executable to manage.
    set(sync_server_dir ${CMAKE_SOURCE_DIR}/tools/sync_server)
    add_executable(test_sync
        test_sync.cpp
        ${sync_server_dir}/ServerStore.h
        ${sync_server_dir}/ServerStore.cpp
        ${sync_server_dir}/SyncServer.h
        ${sync_server_dir}/SyncServer.cpp)
    target_include_directories(test_sync PRIVATE ${sync_server_dir})
    target_link_libraries(test_sync PRIVATE
        noteAppC_sync noteAppC_vault Qt6::HttpServer Qt6::Test GTest::gtest)
    set_target_properties(test_sync PROPERTIES AUTOMOC ON)
    add_test(NAME sync COMMAND test_sync)
    set_tests_properties(sync PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Stage 13 (docs/PLUGINS.md, Tier 1): the Jupyter kernel protocol client,
# built only when NOTEAPPC_WITH_NOTEBOOK is on and libzmq was found. The
# end-to-end cases skip themselves (GTEST_SKIP) when no kernel is
# installed, so the minimal environment still passes; TIMEOUT is generous
# because a kernel process really does take on the order of a second to
# import and become ready.
if(TARGET noteAppC_share_crdt)
    add_executable(test_share_crdt test_share_crdt.cpp)
    target_link_libraries(test_share_crdt PRIVATE noteAppC_share_crdt Qt6::Core GTest::gtest GTest::gtest_main)
    add_test(NAME share_crdt COMMAND test_share_crdt)
endif()

# The vendored link layer's own tests (Hearth's, unchanged), loopback only; the
# mDNS cases skip without avahi-daemon. XDG_STATE_HOME keeps the debug log out of
# the real home. TIMEOUT: Node shutdown hung once in Hearth (docs/SHARE.md).
if(TARGET noteAppC_link)
    add_executable(test_link ${CMAKE_SOURCE_DIR}/libs/ext/link/net/test_node.cpp
        ${CMAKE_SOURCE_DIR}/libs/ext/link/net/test_tailnet.cpp)
    target_link_libraries(test_link PRIVATE noteAppC_link GTest::gtest GTest::gtest_main)
    add_test(NAME link COMMAND test_link)
    set_tests_properties(link PROPERTIES TIMEOUT 600
        ENVIRONMENT "XDG_STATE_HOME=${CMAKE_CURRENT_BINARY_DIR}/link-state")
    # The copy must still be a Hearth commit; skipped (77) without a checkout.
    set(NOTEAPPC_HEARTH_DIR "$ENV{HOME}/Documents/AI/projects/hearth" CACHE PATH
        "Hearth checkout that tools/vendor_link.sh --check compares libs/ext/link with")
    add_test(NAME link_vendor_check
        COMMAND sh ${CMAKE_SOURCE_DIR}/tools/vendor_link.sh --check ${NOTEAPPC_HEARTH_DIR})
    set_tests_properties(link_vendor_check PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# The sharing engine (docs/SHARE.md "The engine") over loopback: engines and bare
# link nodes in one process, in-memory secret store, mDNS off, port 0. XDG_DATA_HOME
# and XDG_STATE_HOME keep the share store's default location and the link log out
# of the real home.
if(TARGET noteAppC_shareengine)
    add_executable(test_share test_share.cpp)
    target_link_libraries(test_share PRIVATE noteAppC_shareengine Qt6::Test GTest::gtest)
    add_test(NAME share COMMAND test_share)
    set_tests_properties(share PROPERTIES TIMEOUT 300
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/share-data;XDG_STATE_HOME=${CMAKE_CURRENT_BINARY_DIR}/share-state")
endif()

# Sharing through the window: two MainWindows on two temporary vaults, loopback only
# (docs/SHARE.md "In the window"). The XDG directories are temporary ones, see main().
if(TARGET noteAppC_shareengine AND TARGET noteAppC_ui)
    add_executable(test_share_window test_share_window.cpp)
    target_link_libraries(test_share_window PRIVATE noteAppC_ui noteAppC_shareengine Qt6::Test GTest::gtest)
    add_test(NAME share_window COMMAND test_share_window)
    set_tests_properties(share_window PROPERTIES TIMEOUT 600 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET noteAppC_crypt)
    add_executable(test_crypt test_crypt.cpp)
    target_link_libraries(test_crypt PRIVATE noteAppC_crypt Qt6::Test GTest::gtest)
    add_test(NAME crypt COMMAND test_crypt)
    set_tests_properties(crypt PROPERTIES TIMEOUT 120)
endif()

if(TARGET noteAppC_notebook)
    add_executable(test_notebook test_notebook.cpp)
    target_link_libraries(test_notebook PRIVATE noteAppC_notebook noteAppC_sandbox Qt6::Test GTest::gtest)
    target_compile_definitions(test_notebook PRIVATE NOTEAPPC_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures")
    add_test(NAME notebook COMMAND test_notebook)
    set_tests_properties(notebook PROPERTIES TIMEOUT 240)
endif()

add_executable(test_fuzz test_fuzz.cpp)
target_link_libraries(test_fuzz PRIVATE noteAppC_editor noteAppC_vault Qt6::Test GTest::gtest)
add_test(NAME fuzz COMMAND test_fuzz)
set_tests_properties(fuzz PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_touch test_touch.cpp)
target_link_libraries(test_touch PRIVATE noteAppC_editor Qt6::Test GTest::gtest)
add_test(NAME touch COMMAND test_touch)
set_tests_properties(touch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(TARGET noteAppC_history)
    add_executable(test_history test_history.cpp)
    target_link_libraries(test_history PRIVATE noteAppC_history Qt6::Core GTest::gtest GTest::gtest_main)
    add_test(NAME history COMMAND test_history)
endif()

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE noteAppC_settings GTest::gtest GTest::gtest_main)
# The two SettingsModuleTest cases register keys and a feature in Settings' process-wide
# tables (M1): alone in their process they cannot leak into another test. Run each entry whole.
add_test(NAME settings COMMAND test_settings --gtest_filter=-SettingsModuleTest.*)
add_test(NAME settings_modules COMMAND test_settings --gtest_filter=SettingsModuleTest.*)

add_executable(test_plugins test_plugins.cpp)
target_link_libraries(test_plugins PRIVATE noteAppC_vault noteAppC_ui Qt6::Test GTest::gtest)
add_test(NAME plugins COMMAND test_plugins)
set_tests_properties(plugins PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(TARGET noteAppC_imageview)
    add_executable(test_imageview test_imageview.cpp)
    target_link_libraries(test_imageview PRIVATE noteAppC_imageview Qt6::Test GTest::gtest)
    add_test(NAME imageview COMMAND test_imageview)
    set_tests_properties(imageview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET noteAppC_pdfview)
    add_executable(test_pdfview test_pdfview.cpp)
    target_link_libraries(test_pdfview PRIVATE noteAppC_pdfview Qt6::Test GTest::gtest)
    add_test(NAME pdfview COMMAND test_pdfview)
    set_tests_properties(pdfview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# The runtime plugin host (docs/PLUGINS.md, Tier 2) - libs/ext/plugins plus a
# real noteAppC-plugin-host process and the word-stats example/fixture
# plugin, built by the tree only when NOTEAPPC_WITH_PLUGINS is on.
if(TARGET noteAppC_plugins)
    add_executable(test_plugins_host test_plugins_host.cpp)
    target_link_libraries(test_plugins_host PRIVATE noteAppC_plugins noteAppC_plugin_protocol Qt6::Test GTest::gtest)
    target_compile_definitions(test_plugins_host PRIVATE
        NOTEAPPC_WORD_STATS_PLUGIN_DIR="${CMAKE_BINARY_DIR}/examples/plugins/word-stats")
    add_dependencies(test_plugins_host noteAppC-plugin-host word-stats)
    add_test(NAME plugins_host COMMAND test_plugins_host)
    set_tests_properties(plugins_host PROPERTIES
        ENVIRONMENT "NOTEAPPC_PLUGIN_HOST=$<TARGET_FILE:noteAppC-plugin-host>"
        TIMEOUT 60)
endif()

# The AI plugin (plugins/ai, docs/PLUGINS.md's "AI plugin"): AiProvider's
# request/response logic against a real in-process fake server, plus the
# real libai.so through a real sandboxed host process. Needs Qt6::HttpServer
# for the fake server regardless of whether NOTEAPPC_WITH_SYNC (the other
# consumer of that component) is on.
find_package(Qt6 6.5 QUIET COMPONENTS HttpServer)
if(TARGET ai AND TARGET Qt6::HttpServer)
    add_executable(test_ai_plugin test_ai_plugin.cpp)
    target_include_directories(test_ai_plugin PRIVATE ${CMAKE_SOURCE_DIR}/plugins/ai)
    target_link_libraries(test_ai_plugin PRIVATE
        noteAppC_ai_provider noteAppC_plugins noteAppC_plugin_protocol Qt6::HttpServer Qt6::Test GTest::gtest)
    target_compile_definitions(test_ai_plugin PRIVATE
        NOTEAPPC_AI_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins/ai")
    add_dependencies(test_ai_plugin noteAppC-plugin-host ai)
    add_test(NAME ai_plugin COMMAND test_ai_plugin)
    set_tests_properties(ai_plugin PROPERTIES
        ENVIRONMENT "NOTEAPPC_PLUGIN_HOST=$<TARGET_FILE:noteAppC-plugin-host>"
        TIMEOUT 120)
endif()

# Pipe tables typed and edited as source (EditingCommands.cpp, TableCells.cpp): keys go in as real key events.
add_executable(test_tables test_tables.cpp)
target_link_libraries(test_tables PRIVATE noteAppC_editor noteAppC_render Qt6::Test GTest::gtest)
add_test(NAME tables COMMAND test_tables)

# List editing and display (EditingCommands.cpp, BulletGlyphs.cpp): keys go in as real key events.
add_executable(test_lists test_lists.cpp)
target_link_libraries(test_lists PRIVATE noteAppC_editor noteAppC_render Qt6::Test GTest::gtest)
add_test(NAME lists COMMAND test_lists)

# No test reads or writes the real ~/.config/noteAppC or ~/.cache/noteAppC, and none
# sees the modules installed on the machine (NOTEAPPC_MODULE_DIRS, below).
# MarkdownEditor loads callouts.ini in its constructor and Settings reads
# settings.ini, so without this a test's result depended on how the person
# running it had set up their own copy of the app - Settings.defaultsAreCompiledIn
# failed the day Jernej switched his theme to OLED. Set once here so every test,
# including ones not written yet, starts from a clean home. Register every
# test above this block: a test added below it runs against the real home.
get_property(noteappc_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS noteappc_tests)
    get_property(noteappc_test_environment TEST ${test} PROPERTY ENVIRONMENT)
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
        "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/config"
        "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/cache"
        "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/data")
    # M8: what is installed on the machine never reaches a test. A test that wants modules
    # (window: the fake ones) has set NOTEAPPC_MODULE_DIRS above this block; every other
    # gets a directory that does not exist, so a scan finds nothing.
    if(NOT noteappc_test_environment MATCHES "NOTEAPPC_MODULE_DIRS=")
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
            "NOTEAPPC_MODULE_DIRS=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/modules")
    endif()
endforeach()

# The same homes for a test binary run by hand (IsolatedHome.cpp runs before main).
get_property(noteappc_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS noteappc_targets)
    if(target MATCHES "^test_")
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/IsolatedHome.cpp)
        target_compile_definitions(${target} PRIVATE
            NOTEAPPC_TEST_HOME="${CMAKE_CURRENT_BINARY_DIR}/isolated-home")
    endif()
endforeach()
