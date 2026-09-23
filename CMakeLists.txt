cmake_minimum_required(VERSION 3.28)

project(noteAppC
    VERSION 0.0.1
    DESCRIPTION "A Linux-native Obsidian alternative in C++"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++20 modules anywhere in this project - every file uses #include - so
# CMake's module scanning is pure overhead: a scan step per source and a dyndep
# file per target. It is also what crashed ninja 1.13.2 on a no-op rebuild, an
# assertion in Plan::RefreshDyndepDependents, which made a failed build look
# like a successful one to anything that only read its tail.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg Sql Concurrent Network)
# Optional: the editor can paint its viewport through OpenGL instead of raster.
find_package(Qt6 6.5 QUIET COMPONENTS OpenGLWidgets)
find_package(Qt6 6.5 QUIET COMPONENTS Test)
qt_standard_project_setup()

# Neither dependency is in the Android Qt kit, so both can be turned off for a
# mobile/cross build; NOTEAPPC_WITH_PDF also covers dropping poppler-qt6 when
# it is installed but built against a Qt too new to link with that kit.
option(NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING "Colour fenced code blocks with KF6 SyntaxHighlighting" ON)
option(NOTEAPPC_WITH_PDF "Embed PDFs with poppler-qt6" ON)

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
    # Four implicit conversions Qt 6.8 allows and 6.7 - the Android kit - does
    # not; see the script.
    PATCH_COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/cmake/patch-md4qt.sh
    EXCLUDE_FROM_ALL)
# MicroTeX renders the LaTeX in $$...$$ and $...$. It is not packaged, and it
# wants tinyxml2, which is not installed either - both are vendored and pinned.
# tinyxml2 is two files under the MIT licence; MicroTeX is MIT too.
set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tinyxml2
    GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
    GIT_TAG 10.0.0
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(tinyxml2)

# MicroTeX populated but not added as a subproject: its own CMakeLists insists
# on a system tinyxml2 and also builds a sample app. SOURCE_SUBDIR points at a
# directory with no CMakeLists.txt, so FetchContent only downloads it and
# libs/render builds the pieces it needs.
# The patch turns Qt font merging off in MicroTeX's backend. TeX fonts are
# symbol-encoded, so with merging on Qt substitutes another font for the
# codepoints cmex10 and friends use, and every large operator, radical and
# delimiter comes out blank.
FetchContent_Declare(microtex
    GIT_REPOSITORY https://github.com/NanoMichael/MicroTeX.git
    GIT_TAG 0e3707f6dafebb121d98b53c64364d16fefe481d # master, 2024-08-06
    SOURCE_SUBDIR src
    PATCH_COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/cmake/patch-microtex.sh
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(microtex)

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
