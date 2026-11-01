# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
add_library(noteAppC_settings STATIC
    Hotkeys.cpp
    Hotkeys.h
    Settings.cpp
    Settings.h
    StorageMigration.cpp
    StorageMigration.h)

target_include_directories(noteAppC_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_settings PUBLIC Qt6::Core Qt6::Gui noteAppC_xdg)
# FormatColours.h is header-only: the table of formatting colours, shared with
# the theme without linking it.
target_include_directories(noteAppC_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../theme)
