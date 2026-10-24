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
# PdfPage resolves libnotolith-pdfpage.so from the module directories (libs/render/PdfPage.cpp): this test
# points at the build's, and needs it built. No host symbols in that library, so a bare test binary loads it.
if(TARGET notolith-pdfpage)
    target_compile_definitions(test_editor PRIVATE NOTEAPPC_EXPECT_PDF)
    add_dependencies(test_editor notolith-pdfpage)
endif()
get_property(noteappc_have_litehtml GLOBAL PROPERTY NOTEAPPC_HAVE_LITEHTML)
if(noteappc_have_litehtml)
    target_compile_definitions(test_editor PRIVATE NOTEAPPC_EXPECT_HTML)
endif()
add_test(NAME editor COMMAND test_editor)
if(TARGET notolith-pdfpage)
    set_tests_properties(editor PROPERTIES ENVIRONMENT "NOTEAPPC_MODULE_DIRS=${CMAKE_BINARY_DIR}/modules")
endif()
# Its ratio budgets (a 1 MB open against a 200 kB one, min of 7) failed once
# under ctest -j4 when neighbours took the CPU: alone, they hold every time.
set_tests_properties(editor PROPERTIES RUN_SERIAL TRUE)

# The CSV/TSV parser behind `![[data.csv]]`: pure text in, rows out.
add_executable(test_csv test_csv.cpp)
target_link_libraries(test_csv PRIVATE noteAppC_editor GTest::gtest GTest::gtest_main)
add_test(NAME csv COMMAND test_csv)

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
if(TARGET notolith-pdfpage)
    add_executable(test_pdfpage_race test_pdfpage_race.cpp)
    target_link_libraries(test_pdfpage_race PRIVATE noteAppC_render Qt6::Widgets GTest::gtest)
    add_dependencies(test_pdfpage_race notolith-pdfpage)
    add_test(NAME pdfpage_race COMMAND test_pdfpage_race)
    set_tests_properties(pdfpage_race PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;NOTEAPPC_MODULE_DIRS=${CMAKE_BINARY_DIR}/modules")
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
# NOTEAPPC_STATIC_MODULES (R12, Android): the same discovery for a module linked into the executable. The test
# module is a Qt static plugin in test_modules itself, imported there (Q_IMPORT_PLUGIN) like main.cpp imports ink.
if(NOTEAPPC_STATIC_MODULES)
    set(FAKE_ID fake-static)
    set(FAKE_ABI "${NOTEAPPC_ABI_TAG}")
    set(FAKE_SUFFIX fakestatic)
    configure_file(fakemodule/manifest-static.json.in ${CMAKE_CURRENT_BINARY_DIR}/fake_static/manifest.json @ONLY)
    add_library(fake_static STATIC fakemodule/FakeStaticModule.cpp)
    set_target_properties(fake_static PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
    target_include_directories(fake_static PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/fake_static)
    target_compile_definitions(fake_static PRIVATE QT_STATICPLUGIN FAKE_ID="${FAKE_ID}" FAKE_SUFFIX="${FAKE_SUFFIX}")
    target_link_libraries(fake_static PUBLIC noteAppC_modules_headers Qt6::Widgets)
endif()
set(NOTEAPPC_FAKE_MODULE_TARGETS fake_good fake_badabi fake_twin fake_noid fake_notmodule fake_unresolved)

add_executable(test_modules test_modules.cpp)
target_link_libraries(test_modules PRIVATE noteAppC_modules Qt6::Widgets GTest::gtest)
target_compile_definitions(test_modules PRIVATE NOTEAPPC_FAKE_MODULES_DIR="${CMAKE_CURRENT_BINARY_DIR}/fakemodules")
set_target_properties(test_modules PROPERTIES ENABLE_EXPORTS ON) # the fake module resolves ModuleView here
add_dependencies(test_modules ${NOTEAPPC_FAKE_MODULE_TARGETS})
if(NOTEAPPC_STATIC_MODULES)
    target_link_libraries(test_modules PRIVATE fake_static)
    target_compile_definitions(test_modules PRIVATE NOTEAPPC_STATIC_FAKE)
endif()
if(TARGET notolith-ink) # the R2 case: the real module, copied away from its core library
    target_compile_definitions(test_modules PRIVATE NOTEAPPC_INK_MODULE_PATH="$<TARGET_FILE:notolith-ink>")
    add_dependencies(test_modules notolith-ink)
endif()
# The no-load gate is its own ctest entry (M1): in a process where another test had already
# mapped fake_good, "the scan did not load it" would prove nothing. The filter is in the
# command, so it wins over the GTEST_FILTER the run commands pass: run each entry whole.
add_test(NAME modules_gate COMMAND test_modules --gtest_filter=ModuleGateTest.*)
add_test(NAME modules COMMAND test_modules --gtest_filter=-ModuleGateTest.*)
set_tests_properties(modules_gate modules PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_autosave test_autosave.cpp)
target_link_libraries(test_autosave PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
target_compile_definitions(test_autosave PRIVATE NOTEAPPC_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures")
add_test(NAME autosave COMMAND test_autosave)

add_executable(test_window test_window.cpp)
target_link_libraries(test_window PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
# AStoredValueOfAModuleSettingIsAdoptedWhenTheModuleLoads is its own entry: it needs fake.width
# unregistered when the window is made, and an earlier test in the same process registers it,
# after which the stored value is read without adoptStored(). The test skips itself unless this
# entry's environment says so, which leaves the `window` entry without a filter of its own:
# GTEST_FILTER focuses it. This entry's filter is in its command: run it whole.
set(noteappc_window_adopt WindowTest.AStoredValueOfAModuleSettingIsAdoptedWhenTheModuleLoads)
add_test(NAME window COMMAND test_window)
add_test(NAME window_modules_adopt COMMAND test_window --gtest_filter=${noteappc_window_adopt})
set_tests_properties(window_modules_adopt PROPERTIES ENVIRONMENT "NOTEAPPC_WINDOW_ADOPT_ENTRY=1")
# Modules load into the test's process: export the host symbols, and point the
# registry at the build's modules (the fake ones; Task 8 adds the real ones).
set_target_properties(test_window PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_window ${NOTEAPPC_FAKE_MODULE_TARGETS})
# The real ink module sits behind the fake ones: ${CMAKE_BINARY_DIR}/modules holds notolith-ink.so
# and the libnotolith-inkcore.so it needs (neither exists with NOTEAPPC_WITH_INK off).
set_property(TEST window window_modules_adopt APPEND PROPERTY ENVIRONMENT
    "NOTEAPPC_MODULE_DIRS=${CMAKE_CURRENT_BINARY_DIR}/fakemodules:${CMAKE_BINARY_DIR}/modules")
if(TARGET notolith-ink)
    add_dependencies(test_window notolith-ink)
    target_compile_definitions(test_window PRIVATE NOTEAPPC_HAVE_INK_MODULE) # the ink tests in test_window.cpp
    # The ink tests read the canvas of the module's view (document(), tool(), zoom()): the core library is
    # the one the module loads (same soname, found in modules/), never a second copy of InkView.
    target_link_libraries(test_window PRIVATE notolith-inkcore)
    set_property(TARGET test_window APPEND PROPERTY BUILD_RPATH "${CMAKE_BINARY_DIR}/modules")
elseif(TARGET notolith-ink-static) # NOTEAPPC_STATIC_MODULES: the same tests, the module a static plugin of test_window
    target_compile_definitions(test_window PRIVATE NOTEAPPC_HAVE_INK_MODULE NOTEAPPC_STATIC_INK)
    target_link_libraries(test_window PRIVATE notolith-ink-static notolith-inkcore)
endif()
# The PDF viewer module sits in the same directory (libs/ext/pdfview): the window tests that open a PDF, and the
# ones that check what is mapped before one is opened, need it built.
if(TARGET notolith-pdfview)
    add_dependencies(test_window notolith-pdfview)
    target_compile_definitions(test_window PRIVATE NOTEAPPC_HAVE_PDFVIEW_MODULE)
endif()
# What a start maps (lane F): poppler and the share stack are modules, so the executable lists none of their
# libraries and a session that never uses them maps none. One process per test: see test_lazymodules.cpp.
add_executable(test_lazymodules test_lazymodules.cpp)
target_link_libraries(test_lazymodules PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
if(TARGET noteAppC_sync) # the in-memory secret store: the share module's identity never reaches the real keyring
    target_link_libraries(test_lazymodules PRIVATE noteAppC_sync)
endif()
set_target_properties(test_lazymodules PROPERTIES ENABLE_EXPORTS ON)
function(noteappc_lazy_test name filter)
    add_test(NAME ${name} COMMAND test_lazymodules --gtest_filter=LazyModules.${filter})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${ARGN}")
endfunction()
if(TARGET notolith-pdfview)
    add_dependencies(test_lazymodules notolith-pdfview)
    set(noteappc_lazy_modules "NOTEAPPC_MODULE_DIRS=${CMAKE_BINARY_DIR}/modules")
    noteappc_lazy_test(lazy_pdf_start PdfNothingMappedUntilAPdfIsOpened "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_pdf_embed PdfEmbedResolvesWithoutThePluginFace "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_pdf_prescan PdfOpensBeforeAnyScan "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_pdf_restore PdfTabRestoredFromTheWorkspaceOpens "${noteappc_lazy_modules}")
    # No NOTEAPPC_MODULE_DIRS here: the loop at the end of this file gives it a directory that does not exist.
    noteappc_lazy_test(lazy_pdf_missing PdfWithoutTheModuleIsNotAViewFile)
    noteappc_lazy_test(lazy_pdf_restore_missing PdfTabIsKeptInTheWorkspaceWithoutTheModule)
    noteappc_lazy_test(lazy_pdf_kept_focus KeptPdfTabIgnoresFocusAndEndsWhenANoteOpensInIt)
    list(APPEND noteappc_exe_forbidden "poppler")
    list(APPEND noteappc_symbol_modules $<TARGET_FILE:notolith-pdfview> $<TARGET_FILE:notolith-pdfpage>)
    # A module takes the host's symbols from the executable: it must not carry a copy of a host library.
    add_test(NAME needed_pdfview COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_needed.sh $<TARGET_FILE:notolith-pdfview>
        "libnoteAppC_" "libnotolith-ink")
    add_test(NAME needed_pdfpage COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_needed.sh $<TARGET_FILE:notolith-pdfpage>
        "libnoteAppC_")
endif()
if(TARGET notolith-share)
    add_dependencies(test_lazymodules notolith-share)
    set(noteappc_lazy_modules "NOTEAPPC_MODULE_DIRS=${CMAKE_BINARY_DIR}/modules")
    noteappc_lazy_test(lazy_share_idle ShareNothingMappedWhileTheVaultHoldsNoShare "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_share_stored ShareLoadsWhenTheVaultHoldsAShare "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_share_asked ShareLoadsWhenAskedFor "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_share_prescan ShareAskedForBeforeAnyScan "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_share_off ShareOffMapsNothing "${noteappc_lazy_modules}")
    noteappc_lazy_test(lazy_share_missing ShareWithoutTheModuleSaysWhatIsMissing)
    list(APPEND noteappc_exe_forbidden "protobuf" "datachannel" "avahi")
    if(NOT TARGET notolith-ink-static) # a static ink module links the stroke modeler, and so Abseil, into the executable
        list(APPEND noteappc_exe_forbidden "absl")
    endif()
    list(APPEND noteappc_symbol_modules $<TARGET_FILE:notolith-share>)
    add_test(NAME needed_share COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_needed.sh $<TARGET_FILE:notolith-share>
        "libnoteAppC_")
endif()
# The link-time side of the same fact, with no process started: the executable names none of the libraries
# a module brings.
# What NEEDED cannot see: a host static library linked into a module defines the same noteAppC symbols as the
# executable (a second Settings, CrdtDoc, SyncCrypto). The self-test hands the script the executable twice, which
# overlaps everywhere, and must fail: a check that cannot fail proves nothing.
if(noteappc_symbol_modules)
    add_test(NAME symbols_modules COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.sh $<TARGET_FILE:noteAppC>
        ${noteappc_symbol_modules})
    add_test(NAME symbols_selftest COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.sh $<TARGET_FILE:noteAppC>
        $<TARGET_FILE:noteAppC>)
    set_tests_properties(symbols_selftest PROPERTIES WILL_FAIL TRUE)
endif()
if(noteappc_exe_forbidden)
    add_test(NAME needed_exe COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_needed.sh $<TARGET_FILE:noteAppC>
        ${noteappc_exe_forbidden})
endif()

# Track S1b's glyph studio (libs/ext/hwfont): S1b creates the directory and
# tests/test_hwfont.cpp; until noteAppC_hwfont exists this block does nothing.
# test_hwfont drives the studio view and the module through the registry, so it
# links the host (noteAppC_ui) and the static hwfont library only: the module
# is loaded from ${CMAKE_BINARY_DIR}/modules, never linked. IsolatedHome.cpp is
# added to every test_* target by the loop at the end of this file.
if(TARGET noteAppC_hwfont)
    add_executable(test_hwfont test_hwfont.cpp)
    target_link_libraries(test_hwfont PRIVATE noteAppC_hwfont noteAppC_ui Qt6::Test GTest::gtest)
    set_target_properties(test_hwfont PROPERTIES ENABLE_EXPORTS ON BUILD_RPATH "${CMAKE_BINARY_DIR}/modules")
    add_test(NAME hwfont COMMAND test_hwfont)
    set_tests_properties(hwfont PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;NOTEAPPC_MODULE_DIRS=${CMAKE_BINARY_DIR}/modules")
    if(TARGET notolith-hwfont)
        add_dependencies(test_hwfont notolith-hwfont)
    elseif(TARGET notolith-hwfont-static) # NOTEAPPC_STATIC_MODULES: the module is a static plugin of this executable
        target_link_libraries(test_hwfont PRIVATE notolith-hwfont-static)
        target_compile_definitions(test_hwfont PRIVATE NOTEAPPC_STATIC_HWFONT)
    endif()
endif()
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

# ```markmap fences: the tree and the SVG (the pure part), then the editor's renderer and its folding, PDF
# export, the window's core plugin and its command.
add_executable(test_markmap test_markmap.cpp)
target_link_libraries(test_markmap PRIVATE noteAppC_render Qt6::Widgets Qt6::Svg GTest::gtest)
add_test(NAME markmap COMMAND test_markmap)
set_tests_properties(markmap PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(test_markmap_block test_markmap_block.cpp)
target_link_libraries(test_markmap_block PRIVATE noteAppC_ui Qt6::Test Qt6::Svg GTest::gtest)
add_test(NAME markmap_block COMMAND test_markmap_block)
set_tests_properties(markmap_block PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# PlantUML, TikZ and Graphviz fences with the real tools in their sandbox (skipped where a tool is missing).
add_executable(test_diagrams test_diagrams.cpp)
target_link_libraries(test_diagrams PRIVATE noteAppC_ui noteAppC_sandbox Qt6::Test GTest::gtest)
add_test(NAME diagrams COMMAND test_diagrams)
set_tests_properties(diagrams PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Display maths sharing a paragraph, backslash escapes, the formula previewed under its source.
add_executable(test_math test_math.cpp)
target_link_libraries(test_math PRIVATE noteAppC_editor noteAppC_render Qt6::Test GTest::gtest)
add_test(NAME math COMMAND test_math)
set_tests_properties(math PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
# The demo vault's PDF opens in the PDF viewer module and its embeds render through libnotolith-pdfpage.so.
if(TARGET notolith-pdfview)
    target_compile_definitions(test_demo PRIVATE NOTEAPPC_HAVE_PDFVIEW)
    set_target_properties(test_demo PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(test_demo notolith-pdfview)
    set_property(TEST demo APPEND PROPERTY ENVIRONMENT "NOTEAPPC_MODULE_DIRS=${CMAKE_BINARY_DIR}/modules")
endif()
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
    # The engine takes the host's CrdtDoc, store and crypto as headers only (the ink rule); here they are linked in.
    target_link_libraries(test_share PRIVATE noteAppC_shareengine noteAppC_sharebase noteAppC_share_crdt noteAppC_sync
        Qt6::Test GTest::gtest)
    # The wire format of the build before Presence gained positions, as package notolith.share.v0, to read
    # what the new build sends the way the old one does (share_v0/share_v0.proto).
    target_sources(test_share PRIVATE share_v0/share_v0.proto)
    target_include_directories(test_share PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    protobuf_generate(TARGET test_share IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/share_v0
        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME share COMMAND test_share)
    set_tests_properties(share PROPERTIES TIMEOUT 300
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/share-data;XDG_STATE_HOME=${CMAKE_CURRENT_BINARY_DIR}/share-state")
endif()

# Sharing through the window: two MainWindows on two temporary vaults, loopback only
# (docs/SHARE.md "In the window"). The XDG directories are temporary ones, see main().
# The engine is the notolith-share module, loaded into the test's process from ${CMAKE_BINARY_DIR}/modules: the
# test links the window and the static ShareEngineBase, never the engine (a second copy beside the module's).
if(TARGET notolith-share AND TARGET noteAppC_ui)
    add_executable(test_share_window test_share_window.cpp)
    target_link_libraries(test_share_window PRIVATE noteAppC_ui noteAppC_sync Qt6::Test GTest::gtest)
    set_target_properties(test_share_window PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(test_share_window notolith-share)
    add_test(NAME share_window COMMAND test_share_window)
    set_tests_properties(share_window PROPERTIES TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;NOTEAPPC_MODULE_DIRS=${CMAKE_BINARY_DIR}/modules")
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
    # The library takes the host's as headers only (libs/ext/pdfview/CMakeLists.txt): named here.
    target_link_libraries(test_pdfview PRIVATE noteAppC_pdfview noteAppC_render noteAppC_editor noteAppC_theme
        noteAppC_vault noteAppC_modules Qt6::Test GTest::gtest)
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

# Word keys (Ctrl+Backspace/Delete/Left/Right, and with Shift): real key events into the editor.
add_executable(test_wordkeys test_wordkeys.cpp)
target_link_libraries(test_wordkeys PRIVATE noteAppC_editor noteAppC_render Qt6::Test GTest::gtest)
add_test(NAME wordkeys COMMAND test_wordkeys)

# The ink core (libs/ext/ink/core): model, format, outline, input, canvas.
if(TARGET notolith-inkcore)
    add_executable(test_ink test_ink.cpp)
    find_package(ZLIB REQUIRED) # the gzip helper in the format tests (imported targets are directory-scoped)
    target_link_libraries(test_ink PRIVATE notolith-inkcore Qt6::Test GTest::gtest ZLIB::ZLIB)
    target_compile_definitions(test_ink PRIVATE NOTEAPPC_INK_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/fixtures/ink/golden")
    set_target_properties(test_ink PROPERTIES ENABLE_EXPORTS ON BUILD_RPATH "${CMAKE_BINARY_DIR}/modules")
    add_test(NAME ink COMMAND test_ink)
    set_tests_properties(ink PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # The position and overlay maths again at device pixel ratio 2 (PADD): the offscreen platform honours QT_SCALE_FACTOR.
    add_test(NAME ink_dpr2 COMMAND test_ink --gtest_filter=InkCanvasDprTest.*)
    set_tests_properties(ink_dpr2 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")
    # A fractional ratio: a cell is 341.33 logical px wide, still a whole 512 device px.
    add_test(NAME ink_dpr15 COMMAND test_ink --gtest_filter=InkCanvasDprTest.*)
    set_tests_properties(ink_dpr15 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5")
    # The tab itself (InkView, the static library the module links), without dlopen: what it does about a
    # file that changes under it and about a newer file. The host's Settings, Theme, Vault and ModuleView come from noteAppC_modules.
    if(TARGET noteAppC_ink)
        add_executable(test_inkview test_inkview.cpp)
        # noteAppC_editor: InkView draws a text box with MarkdownEditor, which the module takes from the host.
        target_link_libraries(test_inkview PRIVATE noteAppC_ink noteAppC_modules noteAppC_editor Qt6::Test GTest::gtest)
        set_target_properties(test_inkview PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}/modules")
        add_test(NAME inkview COMMAND test_inkview)
        set_tests_properties(inkview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

# The host-side .ink text-box reader (R1): JSON only, no module, no inkcore.
find_package(ZLIB REQUIRED)
add_executable(test_inktextboxes test_inktextboxes.cpp)
target_link_libraries(test_inktextboxes PRIVATE noteAppC_ui Qt6::Test GTest::gtest_main ZLIB::ZLIB)
add_test(NAME inktextboxes COMMAND test_inktextboxes)

# An .ink note in the index after a rename (the host reader plus inkcore, to prove the file still opens).
if(TARGET notolith-inkcore)
    add_executable(test_inkindex test_inkindex.cpp)
    target_link_libraries(test_inkindex PRIVATE noteAppC_ui noteAppC_index noteAppC_vault notolith-inkcore Qt6::Test GTest::gtest)
    set_target_properties(test_inkindex PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}/modules")
    add_test(NAME inkindex COMMAND test_inkindex)
endif()

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
        "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/data"
        "XDG_RUNTIME_DIR=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/run"
        "NOTEAPPC_INSTANCE_SOCKET=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/run/n")
    # M8: what is installed on the machine never reaches a test. A test that wants modules
    # (window: the fake ones) has set NOTEAPPC_MODULE_DIRS above this block; every other
    # gets a directory that does not exist, so a scan finds nothing.
    if(NOT noteappc_test_environment MATCHES "NOTEAPPC_MODULE_DIRS=")
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
            "NOTEAPPC_MODULE_DIRS=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/modules")
    endif()
endforeach()

# The same homes for a test binary run by hand (IsolatedHome.cpp runs before main). It also gives each
# process a runtime directory of its own for the single-instance sockets, so none can reach a running Notolith.
get_property(noteappc_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS noteappc_targets)
    if(target MATCHES "^test_")
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/IsolatedHome.cpp)
        target_compile_definitions(${target} PRIVATE
            NOTEAPPC_TEST_HOME="${CMAKE_CURRENT_BINARY_DIR}/isolated-home")
    endif()
endforeach()
