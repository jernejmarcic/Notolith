# Platforms

noteAppC has to be fast on many machines - Intel and AMD desktops and laptops,
old and new - and a mobile app is planned. This is what that requires of the
code, what has been checked, and what has not.

## The rules

1. **No CPU-specific build flags, ever.** The project's CMake sets none, and
   the default build is plain `-O2 -g`. Watch the packaging: CachyOS's
   `/etc/makepkg.conf` sets `-march=native`, so a package built with `makepkg`
   on a Zen 5 machine uses instructions an older Intel or AMD chip does not
   have, and crashes there with an illegal instruction. A future `PKGBUILD`
   must override it, with baseline `x86-64` or at most `x86-64-v2`.
2. **No architecture-specific code.** Checked 2026-09-23: there are no SSE, AVX
   or other x86 intrinsics and no inline assembly anywhere in noteAppC or in
   the three vendored libraries (md4qt, MicroTeX, tinyxml2). An ARM build -
   a Raspberry Pi, an ARM laptop, a phone - has nothing in its way from the
   code itself.
3. **Raster painting is the default.** It behaves the same on every GPU and
   driver, including none. The OpenGL viewport is a setting, measured slower on
   average, and on an Intel iGPU or a Mesa driver it would be a different
   measurement again. See [PERFORMANCE.md](PERFORMANCE.md).
4. **Multi-core is the target; one core is the floor.** Nearly every machine
   this runs on has several cores, and the thread pool - the async parse, the
   indexer - is built around that. A very old single-core machine must still
   work, and nothing more is asked of it.

## One core, measured

The whole process pinned to one core of starbase-1 with `taskset -c 0`, which
takes away everything the thread pool gives. This is a floor, not a target, and
it is still a fast core - a genuinely old CPU would be slower again.

| what | all cores | one core |
|---|---|---|
| cold start to first frame | 64.6 ms | 127.8 ms |
| note open, 50 kB | 6.8 ms | 16.5 ms |
| note open, 1 MB (readable) | 10.9 ms | 22.6 ms |
| keystroke, 200 kB, median / p95 | 0.22 / 0.28 ms | 0.24 / 1.89 ms |
| keystroke, 1 MB, median / p95 | 0.57 / 0.66 ms | 0.90 / 2.64 ms |
| scroll frame, median / worst | 0.36 / 0.62 ms | 0.38 / 3.50 ms |

The medians barely move. The tails do, because the background parse now
competes for the same core as the keystroke - which is exactly what a weak
dual-core laptop will see. Every tail is still inside the 8 ms budget.

## Measuring on another machine

Only starbase-1 has been measured. The session directory
`sessions/2026-09-22_163000_noteappc-callouts-and-perf/` in the second brain
holds `noteAppC.bundle` and `bench-here.sh`. On any CachyOS or Arch machine:

```sh
sh ~/Documents/AI/sessions/2026-09-22_163000_noteappc-callouts-and-perf/bench-here.sh
```

It installs nothing - a missing dependency gets the `pacman` line printed - and
builds with `CFLAGS` and `CXXFLAGS` cleared, so the binary is the one another
machine would really run. Results land beside the script as `bench-<host>.*`
and sync back. It also measures scrolling at that machine's largest panel size
(offscreen), and raster against OpenGL inside a headless `cage` compositor, so
no window ever appears on a monitor.

Worth running on, in this order: shuttlepod-1 (the 2-in-1, likely the weakest
x86 machine and the one most likely to have an Intel iGPU), then runabout.
torpedo is a Raspberry Pi 5 running Debian, which would be the best stand-in
for a phone's CPU, but the script is Arch-only and torpedo is a server - it
would need its own dependency list and a decision to install build tools there.

## Mobile

Not started. What is already true, and what stands in the way:

**Reusable as they are.** `libs/vault`, `libs/markdown`, `libs/index`,
`libs/render`, `libs/theme` and `libs/settings` are plain C++ static libraries
with no widget code in them. A mobile app links them unchanged.

**The UI is the work.** QML is ruled out - its binding engine is a JavaScript
engine - so a mobile noteAppC is Qt Widgets on a touch screen. That works on
Android, but the editor is built for a mouse and keyboard: it needs kinetic
scrolling (`QScroller`), selection handles, touch-sized targets, and a layout
for a narrow screen with no sidebars. The shell (`libs/ui`) would be largely
new.

**The Qt version.** The Android kit on starbase-1, in
`~/Documents/AI/.toolchains/`, is Qt 6.7.3; the desktop is 6.11. Anything
noteAppC uses from Qt 6.8 or later will not compile for Android. Nothing has
checked this yet.

**Two dependencies are not in the Android kit.** KF6 SyntaxHighlighting (code
blocks) and poppler-qt6 (PDF embeds) come from the Linux distribution. For
Android they must be cross-built or made optional. Poppler already is
optional; syntax highlighting is `REQUIRED` and would need the same treatment.

**Plugins do not travel.** iOS does not let an app load code it did not ship
with, and Android only loads native libraries packaged inside the app. So
runtime plugins ([PLUGINS.md](PLUGINS.md), Tier 2) are desktop-only by
nature, while compiled-in modules (Tier 1) work everywhere. That is one more
reason Tier 1 comes first.

**Phones are tighter.** ARM cores, thermal throttling, a battery, and less
memory: 200 MB of peak RSS with a 1 MB note open is fine on a desktop and is
worth looking at before it is on a phone.
