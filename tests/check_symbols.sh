#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej
# usage: check_symbols.sh <executable> <module.so>...
# Fails when a module defines a strong (non-inline, non-template) noteAppC symbol that the executable defines too: a
# host static library linked into the module, so the process holds two copies of its tables (a second Settings, CrdtDoc,
# SyncCrypto). A static archive adds no NEEDED entry, which is why check_needed.sh cannot see it. Deterministic, no
# process started. Symbols the module imports from the executable are undefined in the module and do not count.
# Scope: global strong symbols only (nm T D B R S G). Local and weak (inline, template, vague-linkage) symbols are not
# compared - a duplicated library is caught by its strong tables and functions, and weak ones merge at load by design.
exe=$1
shift
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
strong() { nm --defined-only "$1" | awk '$2 ~ /^[TDBRSG]$/ && $3 ~ /8noteAppC/ {print $3}' | sort -u; }
strong "$exe" > "$tmp/host" || exit 2
status=0
for module in "$@"; do
    strong "$module" > "$tmp/module" || exit 2
    both=$(comm -12 "$tmp/host" "$tmp/module")
    if [ -n "$both" ]; then
        echo "$module defines noteAppC symbols the executable defines too (a host library linked in):"
        printf '%s\n' "$both" | c++filt | head -20
        status=1
    fi
done
exit $status
