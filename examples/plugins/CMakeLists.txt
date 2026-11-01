# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# Example Tier 2 plugins (docs/PLUGINS.md). Each one also doubles as a test
# fixture - see tests/test_plugins_host.cpp - so every plugin here builds
# straight into a directory that already looks like an installed plugin
# (plugin.ini next to its library), not just the library on its own.
add_subdirectory(word-stats)
