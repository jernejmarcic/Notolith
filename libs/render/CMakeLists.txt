find_package(KF6SyntaxHighlighting REQUIRED)

add_library(noteAppC_render STATIC
    CodeHighlighter.cpp
    CodeHighlighter.h
    PdfPage.cpp
    PdfPage.h)

target_include_directories(noteAppC_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_render
    PUBLIC Qt6::Gui noteAppC_theme
    PRIVATE KF6::SyntaxHighlighting)

# PDF embeds. Optional: poppler is GPL, so a permissive build can drop it and
# PdfPage then reports itself unavailable.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(POPPLER_QT6 IMPORTED_TARGET poppler-qt6)
endif()
if(TARGET PkgConfig::POPPLER_QT6)
    target_link_libraries(noteAppC_render PRIVATE PkgConfig::POPPLER_QT6)
    target_compile_definitions(noteAppC_render PRIVATE NOTEAPPC_HAVE_POPPLER)
    message(STATUS "noteAppC: PDF embeds enabled (poppler-qt6)")
else()
    message(STATUS "noteAppC: poppler-qt6 not found, PDF embeds disabled")
endif()
