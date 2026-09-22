cmake_minimum_required(VERSION 3.28)

project(noteAppC
    VERSION 0.0.1
    DESCRIPTION "A Linux-native Obsidian alternative in C++"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
find_package(Qt6 6.5 QUIET COMPONENTS Test)
qt_standard_project_setup()

add_compile_options(-Wall -Wextra)

add_subdirectory(libs/vault)
add_subdirectory(libs/theme)
add_subdirectory(libs/ui)
add_subdirectory(apps/noteAppC)
add_subdirectory(tools)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
