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

add_executable(test_editor test_editor.cpp)
target_link_libraries(test_editor PRIVATE noteAppC_editor Qt6::Test GTest::gtest)
add_test(NAME editor COMMAND test_editor)

add_executable(test_window test_window.cpp)
target_link_libraries(test_window PRIVATE noteAppC_ui Qt6::Test GTest::gtest)
add_test(NAME window COMMAND test_window)
set_tests_properties(window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
