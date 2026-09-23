# PDF viewing and annotation, the third Tier 1 module (docs/PLUGINS.md):
# opens a vault's PDFs in a tab, reading and marking them up like the rest of
# the app rather than handing them to the system viewer. Needs poppler-qt6
# for more than embeds do (annotations, text extraction, outline, links).
#
# The root CMakeLists only adds this directory once libs/render's own
# pkg_check_modules has already found poppler-qt6 (NOTEAPPC_HAVE_POPPLER, a
# GLOBAL property - PkgConfig's own IMPORTED_TARGET is directory-scoped and
# does not reach here). That target is re-discovered below rather than
# reused, for the same reason libs/render's own comment gives: cheap, and
# every optional module already does its own detection this way.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(POPPLER_QT6 QUIET IMPORTED_TARGET poppler-qt6)
endif()

add_library(noteAppC_pdfview STATIC
    PdfAnnotationCommands.cpp
    PdfAnnotationCommands.h
    PdfDocument.cpp
    PdfDocument.h
    PdfExtract.cpp
    PdfExtract.h
    PdfPagesArea.cpp
    PdfPagesArea.h
    PdfRecovery.cpp
    PdfRecovery.h)

set_target_properties(noteAppC_pdfview PROPERTIES AUTOMOC ON)

target_include_directories(noteAppC_pdfview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_pdfview PUBLIC Qt6::Widgets Qt6::Concurrent
    noteAppC_render noteAppC_editor noteAppC_theme noteAppC_vault
    PRIVATE PkgConfig::POPPLER_QT6)
