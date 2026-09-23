# Colours fenced code blocks. Not in the Android Qt kit, so it is skipped
# entirely when the option is off or the package is missing - CodeHighlighter
# then returns no token formats and code still renders, monospaced and
# uncoloured, rather than failing the build.
if(NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING)
    find_package(KF6SyntaxHighlighting QUIET)
endif()

# MicroTeX, built from its sources rather than its project: see the comment on
# FetchContent in the top-level CMakeLists.
file(GLOB_RECURSE MICROTEX_SOURCES CONFIGURE_DEPENDS
    ${microtex_SOURCE_DIR}/src/atom/*.cpp
    ${microtex_SOURCE_DIR}/src/box/*.cpp
    ${microtex_SOURCE_DIR}/src/core/*.cpp
    ${microtex_SOURCE_DIR}/src/fonts/*.cpp
    ${microtex_SOURCE_DIR}/src/utils/*.cpp
    ${microtex_SOURCE_DIR}/src/res/*.cpp)
add_library(microtex STATIC
    ${MICROTEX_SOURCES}
    ${microtex_SOURCE_DIR}/src/latex.cpp
    ${microtex_SOURCE_DIR}/src/render.cpp
    ${microtex_SOURCE_DIR}/src/platform/qt/graphic_qt.cpp)
target_include_directories(microtex SYSTEM PUBLIC ${microtex_SOURCE_DIR}/src)
target_compile_definitions(microtex PUBLIC BUILD_QT)
target_link_libraries(microtex PUBLIC Qt6::Gui Qt6::Widgets PRIVATE tinyxml2::tinyxml2)
# Upstream's warnings are not ours to fix.
target_compile_options(microtex PRIVATE -w)

# Its symbol tables are read from disk at runtime, from the root passed to
# LaTeX::init.
file(COPY ${microtex_SOURCE_DIR}/res DESTINATION ${CMAKE_BINARY_DIR}/microtex)

# Its fonts, though, are named by a path relative to the working directory -
# `fonts/base/cmex10.ttf` - with a fall back to the Qt resource of the same
# name. A note app does not get to dictate its working directory, so the fonts
# are compiled in and the fall back becomes the path that always works.
file(GLOB_RECURSE MICROTEX_FONTS
    ${microtex_SOURCE_DIR}/res/fonts/*.ttf)
qt_add_resources(microtex "microtex_fonts"
    PREFIX "/"
    BASE ${microtex_SOURCE_DIR}/res
    FILES ${MICROTEX_FONTS})

add_library(noteAppC_render STATIC
    CodeHighlighter.cpp
    CodeHighlighter.h
    PdfPage.cpp
    PdfPage.h
    MathRenderer.cpp
    MathRenderer.h)

target_include_directories(noteAppC_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_render
    PUBLIC Qt6::Gui noteAppC_theme
    PRIVATE microtex)
target_compile_definitions(noteAppC_render PRIVATE
    NOTEAPPC_MICROTEX_RES="${CMAKE_BINARY_DIR}/microtex/res")

if(TARGET KF6::SyntaxHighlighting)
    target_link_libraries(noteAppC_render PRIVATE KF6::SyntaxHighlighting)
    target_compile_definitions(noteAppC_render PRIVATE NOTEAPPC_HAVE_SYNTAX_HIGHLIGHTING)
    message(STATUS "noteAppC: syntax highlighting enabled (KF6::SyntaxHighlighting)")
elseif(NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING)
    message(STATUS "noteAppC: KF6 SyntaxHighlighting not found, code blocks will be uncoloured")
else()
    message(STATUS "noteAppC: syntax highlighting disabled (NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING=OFF)")
endif()

# PDF embeds. Optional: poppler is GPL, so a permissive build can drop it and
# PdfPage then reports itself unavailable; NOTEAPPC_WITH_PDF also lets it be
# switched off when it is installed but built against a Qt the target kit
# cannot link against (the system poppler-qt6 here is built against Qt 6.11).
if(NOTEAPPC_WITH_PDF)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(POPPLER_QT6 IMPORTED_TARGET poppler-qt6)
    endif()
endif()
if(TARGET PkgConfig::POPPLER_QT6)
    # Global, because the imported target is only visible in this directory
    # and the tests need to know which build they are checking.
    set_property(GLOBAL PROPERTY NOTEAPPC_HAVE_POPPLER TRUE)
    target_link_libraries(noteAppC_render PRIVATE PkgConfig::POPPLER_QT6)
    target_compile_definitions(noteAppC_render PRIVATE NOTEAPPC_HAVE_POPPLER)
    message(STATUS "noteAppC: PDF embeds enabled (poppler-qt6)")
else()
    message(STATUS "noteAppC: poppler-qt6 not found, PDF embeds disabled")
endif()
