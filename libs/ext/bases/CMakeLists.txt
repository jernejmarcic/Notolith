# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# Bases, a Tier 1 module (docs/PLUGINS.md): database-like table/card views
# over a vault's notes and their properties, from a `.base` YAML file. Built
# only with NOTEAPPC_WITH_BASES on; nothing links it otherwise.
add_library(noteAppC_bases STATIC
    Value.h
    Expression.h
    Expression.cpp
    BaseFile.h
    BaseFile.cpp
    BaseQuery.h
    BaseQuery.cpp
    BaseTableModel.h
    BaseTableModel.cpp
    BaseCellDelegate.h
    BaseCellDelegate.cpp
    BaseView.h
    BaseView.cpp
    BaseEditorTable.h
    BaseEditorTable.cpp)

set_target_properties(noteAppC_bases PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_bases PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_bases
    PUBLIC Qt6::Core Qt6::Widgets noteAppC_index noteAppC_vault noteAppC_markdown
    PRIVATE Qt6::Concurrent noteAppC_theme yaml-cpp::yaml-cpp)
