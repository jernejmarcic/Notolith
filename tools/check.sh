#!/bin/sh
# Everything a CI would run, run here instead (there is no remote):
#
#   1. the release build and every suite - the performance guards and the
#      round-trip fuzz are tests, so they run with them;
#   2. a build with every optional module off and its suites, which is what
#      proves each module's #ifdef still holds;
#   3. with --bench, the benchmark on a generated 20 000-note vault.
#
#   sh tools/check.sh [--bench]
#
# Builds go to /tmp: ~/Documents is synced to other machines. Each step runs
# in a memory-capped scope where systemd is there, so a runaway test is
# killed alone instead of taking the session with it. CFLAGS and CXXFLAGS are
# dropped for the configure: a distribution's -march=native must not leak in.
set -eu

src=$(cd "$(dirname "$0")/.." && pwd)
cores=$(nproc)
jobs=$((cores > 8 ? 8 : cores))

cap() {
    # A cgroup where systemd will make one. A user manager can refuse a
    # transient scope - starbase-1's did, on and off, for an afternoon - and
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
check /tmp/noteAppC-minimal \
    -DNOTEAPPC_WITH_SYNTAX_HIGHLIGHTING=OFF -DNOTEAPPC_WITH_PDF=OFF -DNOTEAPPC_WITH_MEDIA=OFF \
    -DNOTEAPPC_WITH_SPELLCHECK=OFF -DNOTEAPPC_WITH_GRAPH=OFF -DNOTEAPPC_WITH_CANVAS=OFF \
    -DNOTEAPPC_WITH_BASES=OFF -DNOTEAPPC_WITH_EPUB=OFF -DNOTEAPPC_WITH_PUBLISH=OFF \
    -DNOTEAPPC_WITH_PDFVIEW=OFF

if [ "${1:-}" = "--bench" ]; then
    echo "== bench"
    (cd /tmp && QT_QPA_PLATFORM=offscreen cap /tmp/noteAppC-build/tools/bench/noteAppC-bench --notes 20000)
fi

echo "all green"
