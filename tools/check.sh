#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jernej Marčič
# Everything a CI would run, run locally:
#
#   1. the default build (no networking, no encryption: docs/OFFLINE.md) and
#      every suite - the performance guards and the round-trip fuzz are tests,
#      so they run with them;
#   2. the same with every networked and encrypting option on, which is what
#      keeps those modules compiling and tested;
#   3. a build with every optional module off and its suites, which is what
#      proves each module's #ifdef still holds;
#   4. with --bench, the benchmark on a generated 20 000-note vault.
#
#   sh tools/check.sh [--bench]
#
# Builds go to /tmp, outside the source tree. Each step runs
# in a memory-capped scope where systemd is there, so a runaway test is
# killed alone instead of taking the session with it. CFLAGS and CXXFLAGS are
# dropped for the configure: a distribution's -march=native must not leak in.
#
# A test whose real tool (bwrap, latex, java, a Jupyter kernel...) is missing
# is skipped, not failed, unless NOTEAPPC_REQUIRE_TOOLS=1 is exported first -
# set it on a machine that has them all, or a green run can hide skipped
# sandbox tests. It is not forced here: a machine without the tools could
# never pass.
set -eu

[ "${NOTEAPPC_REQUIRE_TOOLS:-}" = 1 ] || echo "note: NOTEAPPC_REQUIRE_TOOLS is not 1, so tests that need a missing tool skip silently"

src=$(cd "$(dirname "$0")/.." && pwd)
cores=$(nproc)
jobs=$((cores > 8 ? 8 : cores))

cap() {
    # A cgroup where systemd will make one. A user manager can refuse a
    # transient scope, on and off, and
    # systemd-run then exits 1 without running anything; an address-space
    # limit per process still stops one runaway. A build that really failed
    # exits 1 too, and running it again once changes nothing.
    if command -v systemd-run >/dev/null 2>&1; then
        status=0
        systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0 "$@" || status=$?
        [ "$status" -eq 1 ] || return "$status"
    fi
    (ulimit -v 12000000 && "$@")
}

check() {
    dir=$1
    shift
    echo "== $dir"
    env -u CFLAGS -u CXXFLAGS cmake -S "$src" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
    cap cmake --build "$dir" -j"$jobs"
    QT_QPA_PLATFORM=offscreen cap ctest --test-dir "$dir" --output-on-failure
}

check /tmp/noteAppC-build
check /tmp/noteAppC-networked \
    -DNOTEAPPC_WITH_NETWORK=ON -DNOTEAPPC_WITH_SYNC=ON -DNOTEAPPC_WITH_ENCRYPTION=ON \
    -DNOTEAPPC_WITH_SHARE=ON -DNOTEAPPC_WITH_SHARE_YRS=ON -DNOTEAPPC_WITH_AI_PLUGIN=ON
check /tmp/noteAppC-minimal \
    -DNOTEAPPC_WITH_SYNTAX_HIGHLIGHTING=OFF -DNOTEAPPC_WITH_MEDIA=OFF \
    -DNOTEAPPC_WITH_SPELLCHECK=OFF -DNOTEAPPC_WITH_GRAPH=OFF -DNOTEAPPC_WITH_CANVAS=OFF \
    -DNOTEAPPC_WITH_BASES=OFF -DNOTEAPPC_WITH_EPUB=OFF -DNOTEAPPC_WITH_PUBLISH=OFF \
    -DNOTEAPPC_WITH_PDFVIEW=OFF -DNOTEAPPC_WITH_IMAGEVIEW=OFF -DNOTEAPPC_WITH_PLUGINS=OFF -DNOTEAPPC_WITH_AI_PLUGIN=OFF \
    -DNOTEAPPC_WITH_SYNC=OFF -DNOTEAPPC_WITH_NOTEBOOK=OFF -DNOTEAPPC_WITH_SHARE_YRS=OFF -DNOTEAPPC_WITH_SHARE=OFF \
    -DNOTEAPPC_WITH_ENCRYPTION=OFF -DNOTEAPPC_WITH_HTML=OFF -DNOTEAPPC_WITH_HISTORY=OFF \
    -DNOTEAPPC_WITH_INK=OFF -DNOTEAPPC_WITH_HWFONT=OFF

if [ "${1:-}" = "--bench" ]; then
    echo "== bench"
    (cd /tmp && QT_QPA_PLATFORM=offscreen cap /tmp/noteAppC-build/tools/bench/noteAppC-bench --notes 20000)
fi

echo "all green"
