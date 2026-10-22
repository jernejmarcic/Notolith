# The handwriting-font glyph studio. This file holds the data layer (GlyphSet,
# GlyphStore); the studio view and the module target are added by Task 21.
# Links notolith-inkcore only, never noteAppC_ink: notolith-hwfont depends on
# the .ink format and the host, not on the notolith-ink module (spec section 9).
# Headers only from the host's static libraries; their symbols resolve from the
# executable that loads this (Vault is the host's, as in noteAppC_ink).
add_library(noteAppC_hwfont STATIC GlyphSet.cpp GlyphSet.h GlyphStore.cpp GlyphStore.h)
set_target_properties(noteAppC_hwfont PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(noteAppC_hwfont PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    $<TARGET_PROPERTY:noteAppC_vault,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(noteAppC_hwfont PUBLIC notolith-inkcore noteAppC_modules_headers Qt6::Widgets)
