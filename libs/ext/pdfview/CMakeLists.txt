# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# PDF viewing and annotation, the third Tier 1 module (docs/PLUGINS.md), and the
# poppler half of PDF embeds: opens a vault's PDFs in a tab, reading and marking
# them up like the rest of the app rather than handing them to the system viewer.
#
# Two libraries are built here (NOTEAPPC_WITH_PDFVIEW), and the base app links neither:
#   libnotolith-pdfpage.so  poppler's page count, size and render behind the C face
#                           libs/render/PdfPage.cpp resolves (PDF embeds in a note,
#                           the hover preview), callable from any thread. No host
#                           symbols, so it loads in a test binary as well as the app.
#   notolith-pdfview.so     the tab: PdfView as a Notolith module. Links the one above.
# Needs poppler-qt6, which is GPL; without it neither is built and PdfPage reports
# itself unavailable. NOTEAPPC_WITH_PDFVIEW (root CMakeLists.txt) switches both off,
# e.g. for a kit whose Qt poppler was not built against.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(POPPLER_QT6 QUIET IMPORTED_TARGET poppler-qt6)
endif()
if(NOT TARGET PkgConfig::POPPLER_QT6)
    message(STATUS "noteAppC: poppler-qt6 not found, PDF embeds and the PDF viewer are not built")
    return()
endif()
message(STATUS "noteAppC: PDF embeds and viewer enabled (poppler-qt6)")

configure_file(manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/manifest.json @ONLY)

# The poppler half of PdfPage, a SHARED library next to the module.
add_library(notolith-pdfpage SHARED PdfPageImpl.cpp PdfPageImpl.h)
set_target_properties(notolith-pdfpage PROPERTIES
    OUTPUT_NAME notolith-pdfpage
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH ON)
target_include_directories(notolith-pdfpage
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}/generated
            $<TARGET_PROPERTY:noteAppC_render,INTERFACE_INCLUDE_DIRECTORIES>) # PdfPageFace.h, headers only
target_link_libraries(notolith-pdfpage PUBLIC Qt6::Gui PRIVATE PkgConfig::POPPLER_QT6)
install(TARGETS notolith-pdfpage LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/notolith/modules)

# The tab as a static library the module links; test_pdfview and the bench link it too, without dlopen.
# Headers only from the host's static libraries: a second copy of Settings, or of PdfPage, inside the
# .so would be a second set of tables. The symbols resolve from the executable that loads the module
# (ENABLE_EXPORTS, ResolveAllSymbolsHint), so the two binaries that link this library name the host
# libraries themselves. The same rule as the PDF page library above.
add_library(noteAppC_pdfview STATIC
    PdfAnnotationCommands.cpp
    PdfAnnotationCommands.h
    PdfDocument.cpp
    PdfDocument.h
    PdfExtract.cpp
    PdfExtract.h
    PdfPagesArea.cpp
    PdfPagesArea.h
    PdfView.cpp
    PdfView.h)

set_target_properties(noteAppC_pdfview PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)

target_include_directories(noteAppC_pdfview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    $<TARGET_PROPERTY:noteAppC_settings,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_theme,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_vault,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_editor,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_markdown,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:noteAppC_render,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(noteAppC_pdfview PUBLIC Qt6::Widgets Qt6::Concurrent noteAppC_modules_headers notolith-pdfpage
    PRIVATE PkgConfig::POPPLER_QT6)

# The module itself. $ORIGIN finds libnotolith-pdfpage.so beside it, installed and in the build tree alike;
# BUILD_WITH_INSTALL_RPATH for the reason the executable gives (apps/noteAppC/CMakeLists.txt): CMake's
# computed build RPATH would carry empty entries, and an empty entry means "the current directory".
add_library(notolith-pdfview MODULE PdfModule.cpp)
set_target_properties(notolith-pdfview PROPERTIES
    AUTOMOC ON
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/modules
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH ON)
target_include_directories(notolith-pdfview PRIVATE ${CMAKE_CURRENT_BINARY_DIR}) # manifest.json for Q_PLUGIN_METADATA
target_link_libraries(notolith-pdfview PRIVATE noteAppC_pdfview)
install(TARGETS notolith-pdfview LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/notolith/modules)
