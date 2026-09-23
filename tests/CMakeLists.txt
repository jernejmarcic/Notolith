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
target_link_libraries(test_editor PRIVATE noteAppC_editor noteAppC_render Qt6::Test GTest::gtest)
get_property(noteappc_have_poppler GLOBAL PROPERTY NOTEAPPC_HAVE_POPPLER)
if(noteappc_have_poppler)
    target_compile_definitions(test_editor PRIVATE NOTEAPPC_EXPECT_PDF)
endif()
get_property(noteappc_have_litehtml GLOBAL PROPERTY NOTEAPPC_HAVE_LITEHTML)
if(noteappc_have_litehtml)
    target_compile_definitions(test_editor PRIVATE NOTEAPPC_EXPECT_HTML)
endif()
add_test(NAME editor COMMAND test_editor)

add_executable(test_window test_window.cpp)
target_link_libraries(test_window PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
add_test(NAME window COMMAND test_window)
foreach(module noteAppC_graph noteAppC_canvas noteAppC_bases)
    if(TARGET ${module})
        target_link_libraries(test_window PRIVATE ${module})
    endif()
endforeach()
set_tests_properties(window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_theme test_theme.cpp)
target_link_libraries(test_theme PRIVATE noteAppC_theme Qt6::Test GTest::gtest GTest::gtest_main)
add_test(NAME theme COMMAND test_theme)

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

add_executable(test_fuzz test_fuzz.cpp)
target_link_libraries(test_fuzz PRIVATE noteAppC_editor noteAppC_vault Qt6::Test GTest::gtest)
add_test(NAME fuzz COMMAND test_fuzz)
set_tests_properties(fuzz PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_touch test_touch.cpp)
target_link_libraries(test_touch PRIVATE noteAppC_editor Qt6::Test GTest::gtest)
add_test(NAME touch COMMAND test_touch)
set_tests_properties(touch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE noteAppC_settings GTest::gtest GTest::gtest_main)
add_test(NAME settings COMMAND test_settings)

add_executable(test_plugins test_plugins.cpp)
target_link_libraries(test_plugins PRIVATE noteAppC_vault noteAppC_ui Qt6::Test GTest::gtest)
add_test(NAME plugins COMMAND test_plugins)
set_tests_properties(plugins PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# No test reads or writes the real ~/.config/noteAppC or ~/.cache/noteAppC.
# MarkdownEditor loads callouts.ini in its constructor and Settings reads
# settings.ini, so without this a test's result depended on how the person
# running it had set up their own copy of the app - Settings.defaultsAreCompiledIn
# failed the day Jernej switched his theme to OLED. Set once here so every test,
# including ones not written yet, starts from a clean home.
get_property(noteappc_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS noteappc_tests)
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
        "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/config"
        "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/cache"
        "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/isolated-home/data")
endforeach()
