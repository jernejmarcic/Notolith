cmake_minimum_required(VERSION 3.28)

project(noteAppC
    VERSION 0.0.1
    DESCRIPTION "A Linux-native Obsidian alternative in C++"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg Sql Concurrent)
find_package(Qt6 6.5 QUIET COMPONENTS Test)
qt_standard_project_setup()

add_compile_options(-Wall -Wextra)

# md4qt is not packaged on Arch; pin the KDE repository by commit.
# 5.x dropped the text-plugin hook the plan assumed, so Obsidian's own syntax
# is a post-pass over the tree instead (libs/markdown/ObsidianSyntax).
include(FetchContent)
set(BUILD_MD4QT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_MD4QT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(md4qt
    GIT_REPOSITORY https://invent.kde.org/libraries/md4qt.git
    GIT_TAG e48716a41c1cf7f91b130fe3d3cc1c524e3b404a # v5.1.4
    GIT_SHALLOW FALSE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(md4qt)

# In-tree the headers sit in src/, but upstream installs them as <md4qt/*.h>.
# One symlink and the includes read the same either way.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/md4qt-include)
file(CREATE_LINK ${md4qt_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/md4qt-include/md4qt SYMBOLIC)
target_include_directories(md4qt INTERFACE ${CMAKE_BINARY_DIR}/md4qt-include)

add_subdirectory(libs/vault)
add_subdirectory(libs/markdown)
add_subdirectory(libs/index)
add_subdirectory(libs/render)
add_subdirectory(libs/editor)
add_subdirectory(libs/theme)
add_subdirectory(libs/settings)
add_subdirectory(libs/ui)
add_subdirectory(apps/noteAppC)
add_subdirectory(tools)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installing matters for one reason: a .desktop file is what lets a file
# manager open a loose .md with noteAppC, which is the whole point of
# single-file mode.
include(GNUInstallDirs)
install(TARGETS noteAppC RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/noteAppC.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
