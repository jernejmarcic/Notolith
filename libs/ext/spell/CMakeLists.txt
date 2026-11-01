# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# Spellcheck, the first Tier 1 module (docs/PLUGINS.md): built only with
# NOTEAPPC_WITH_SPELLCHECK on and hunspell found, and the editor never links
# it - the window hands the editor three callbacks instead.
add_library(noteAppC_spell STATIC
    SpellChecker.cpp
    SpellChecker.h)

target_include_directories(noteAppC_spell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_spell
    PUBLIC Qt6::Core noteAppC_theme
    PRIVATE PkgConfig::HUNSPELL)
