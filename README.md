# noteAppC

A Linux-native alternative to Obsidian, written in C++ with Qt 6 Widgets.
No JavaScript, no browser engine, no Electron.

Plan and feature inventory:
`~/Documents/AI/sessions/2026-09-22_011551_obsidian-clone-cpp-plan/`.

## Status

Phase 0 (bootstrap): opens a vault, shows the file tree, edits a note in a
plain text widget, saves atomically. The real editor engine is Phase 2.

## Build

```sh
cmake -B build -G Ninja
cmake --build build
./build/apps/noteAppC/noteAppC
```

Requires `qt6-base`, `cmake`, `ninja`, a C++20 compiler, and `gtest` for the
tests (`ctest --test-dir build`).
