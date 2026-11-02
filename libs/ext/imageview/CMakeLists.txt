# SPDX-License-Identifier: GPL-3.0-or-later
# Image annotation, a Tier 1 module (docs/PLUGINS.md): opens an image in a tab
# to highlight, draw on and note it. The image file is never touched; the
# marks live in a Markdown sidecar beside it - the EPUB reader's highlights
# pattern - and "Extract annotations to a note" is that sidecar. Qt Widgets
# and the vault's byte-faithful reader/writer only: no new dependency.
add_library(noteAppC_imageview STATIC
    ImageAnnotations.cpp
    ImageAnnotations.h
    ImageView.cpp
    ImageView.h)

set_target_properties(noteAppC_imageview PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_imageview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_imageview PUBLIC Qt6::Widgets noteAppC_theme noteAppC_vault)
