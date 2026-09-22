add_library(noteAppC_settings STATIC
    Settings.cpp
    Settings.h)

target_include_directories(noteAppC_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_settings PUBLIC Qt6::Core Qt6::Gui)
