# Platforms

noteAppC has to be fast on many machines - Intel and AMD desktops and laptops,
old and new - and a mobile app is planned. This is what that requires of the
code, what has been checked, and what has not.

## The rules

1. **No CPU-specific build flags, ever.** The project's CMake sets none, and
   the default build is plain `-O2 -g`. Watch the packaging: CachyOS's
   `/etc/makepkg.conf` sets `-march=native`, so a package built with `makepkg`
   on a Zen 5 machine uses instructions an older Intel or AMD chip does not
   have, and crashes there with an illegal instruction. `packaging/arch/PKGBUILD`
   strips `-march` and `-mtune` from the C and C++ flags and `target-cpu` from
   `RUSTFLAGS`, which leaves the compiler's baseline `x86-64`.
2. **No architecture-specific code.** There are no SSE, AVX
   or other x86 intrinsics and no inline assembly anywhere in noteAppC or in
   the three libraries fetched at configure time (md4qt, MicroTeX, tinyxml2). An ARM build -
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

The whole process pinned to one core of that desktop with `taskset -c 0`, which
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

Only one desktop (a Ryzen 9 9900X, RTX 5080, CachyOS) has been measured.
Bench reports from other hardware - a weak laptop, an Intel iGPU, an ARM board -
are welcome: build with `CFLAGS` and `CXXFLAGS` cleared, so the binary is the one
another machine would really run, then run `noteAppC-bench` (see
[PERFORMANCE.md](PERFORMANCE.md#measuring)). Scrolling at the machine's largest
panel size (offscreen), and raster against OpenGL inside a headless `cage`
compositor, can be measured without a window appearing on a monitor.

## Launching: one process per vault

`packaging/io.github.jernejmarcic.Notolith.desktop` registers Markdown files
and folders, so a file manager runs `notolith %f`. Like Obsidian, a vault is
open in one window only, but two vaults are two windows - two processes, each
with its own indexer, autosaver and sync engine. The first launch for a vault
makes its window and listens on a socket named after the vault in the runtime
directory (`$XDG_RUNTIME_DIR/notolith`, owner-only): `instance-<16 hex digits>.sock`,
the digits being a SHA-256 of the vault's canonical path, so the same vault
spelled with a trailing slash, `..` or a symbolic link has the same name. A
later launch for that vault connects, sends the absolute path it was given
(nothing, when it was given none) with its `XDG_ACTIVATION_TOKEN`, waits up
to a second for the answer and exits 0. A launch for a vault nobody has open
starts a process of its own.

Which vault a launch is for is worked out the way the window will open it
(`SingleInstance::launchFor`): a folder, or a note inside a vault
(`resolveLaunchTarget`), is that vault; with no argument it is the vault the app
would reopen, the most recent in the settings that is still there. Two
cases have no vault of their own:

- A note outside every vault goes to the instance of the last-used vault when
  that runs, and opens beside what it has, as one window always did; else to
  the process that has no vault; else a process starts, opens the file alone,
  and serves the no-vault name (`notolith-none.sock`).
- A bare launch with no vault to reopen goes to the no-vault process, or
  starts one with an empty window.

A window that opens another vault (open folder, recent vaults, a note
handed to it from another vault) stops serving the name of the one it left and
serves the new one. When the new vault is already open in another process,
that process is handed the vault and raises itself, and this window stays as it
was. Closing the vault (or opening a loose file) hands the vault's name back and
takes the no-vault name when it is free.

A launch with no instance running pays one failed `connect()`, one lock file
(`notolith-<id>.sock.lock`, taken before the window is built and held until
exit) and, for a launch that names no vault, one read of the settings (done twice before the window: the cheap pass and the full one, about 20 microseconds each). The
lock is what makes two launches in the same moment - a multi-select "Open
with" - one window: the second finds it held, and keeps retrying the connect
for up to two seconds while the first builds its window. A crash leaves the
socket and lock files behind; the next start sees the dead owner and replaces
both.

The running window opens the path as the command line would: a note in the
open vault replaces the current tab, as following a link does; another vault
opens the way the vault menu opens it; a note outside the vault opens in the
current tab beside it, the vault staying open. A folder in the recent-vaults
list counts as a vault whether or not it holds `.obsidian` (reading the
settings once per launch that is handed a loose file). Then the window is
raised and `activateWindow()` is called, with the launcher's token put in the
environment first: Qt's Wayland plugin reads `XDG_ACTIVATION_TOKEN` in
`QWaylandXdgSurface::requestActivate()`, so the compositor can grant focus
instead of only flagging the window. Whether it does depends on the
compositor and on the file manager passing a token (the desktop file's
`StartupNotify=true` asks it to); that cannot be tested without one.

`NOTEAPPC_INSTANCE_SOCKET` replaces `$XDG_RUNTIME_DIR/notolith/instance` as the start of
every socket name: it is a prefix, not one socket. `/tmp/run/bench` makes
`/tmp/run/bench-<id>.sock` for a vault and `/tmp/run/bench-none.sock` for the
no-vault process (its directory must exist; keep the whole path under 100
bytes). Benchmarks and anything else that starts the real binary while
Notolith is in use should set it, or the launch is handed to the open window of
the same vault. Nothing in the repository's tests or benchmark scripts starts the
binary, so none sets it; the window tests set it to a directory of their own.

### Where the files live

Everything the app writes outside a vault is in one of the four XDG Base Directory
roots, under `notolith` (`kAppDirName` in `libs/markdown/ConfigFile.h`; a relative
`$XDG_*` value is invalid and ignored, as the spec says), and a directory the app has
to make is mode 0700.

| root | what |
|---|---|
| `$XDG_CONFIG_HOME/notolith` (`~/.config`) | `settings.ini`, `hotkeys.json`, `callouts.ini`, `codeblocks.ini`, `tasks.ini`, `dictionary.txt`, `plugins.ini`, `themes/`, `templates/` |
| `$XDG_DATA_HOME/notolith` (`~/.local/share`) | per vault under `vaults/<digest>/`: `bookmarks.json`, `icons.json`, `workspaces.json`; and `recovery/`, `pdf-recovery/`, `history/`, `sync/`, `share/`, user-installed `plugins/` |
| `$XDG_STATE_HOME/notolith` (`~/.local/state`) | `vaults.ini`, `window.ini` (geometry, splitter, publish choices), `epub-positions.ini`, `notolith.log`, `link.log`, per vault `vaults/<digest>/workspace.json` and `graph.json` |
| `$XDG_CACHE_HOME/notolith` (`~/.cache`) | the vault index (`vaults/<digest>/index.db`), `remote-images/`, `tex/`, `mermaid/` and the other diagram caches, `notebook/`, `epub/` |
| `$XDG_RUNTIME_DIR/notolith` | the single-instance sockets and locks (when there is no usable runtime directory, the sockets go in `$XDG_STATE_HOME/notolith/instance-*.sock` instead), `settings.lock`, `notolith.log.lock`, `link-<id>.lock`, the notebook kernels' runtime directories |

The digest is 16 hex of the SHA-256 of the vault's absolute path (`vaultDigest()`).
Read-only lookups go through `$XDG_DATA_DIRS` too: plugins in `notolith/plugins` and
MicroTeX's tables in `notolith/microtex/res`. A build from before this layout kept
everything under `noteAppC` in those roots (and window state in Qt's `noteAppC.conf`);
`migrateStorage()` (`libs/settings/StorageMigration.h`) moves it on the first start, never
overwriting a file already there, and deletes the old cache.

### What two processes share

Per user, not per vault, and what was done about it:

- `settings.ini` (`~/.config/notolith`) and `vaults.ini` (recent vaults and files,
  remembered note positions; `~/.local/state/notolith`): each process holds what it
  loaded, so `Settings::save()` writes what the object changed since it loaded on top
  of what the files hold now, under `$XDG_RUNTIME_DIR/notolith/settings.lock` (200 ms
  wait at most). Another window's change to another
  setting, a vault it opened, a note it positioned survive; a reset or a forgotten
  position stays; a setting both changed takes the later save. A setting changed
  in one window shows in another only after that other loads or saves.
- Window geometry, sidebar state, the publish dialog's choices
  (`~/.local/state/notolith/window.ini`), plugin switches, EPUB positions
  (`QSettings`): Qt merges per key under its own file lock. The geometry key is
  one per user, so the last window closed decides where the next opens.
- The log (`~/.local/state/notolith/notolith.log`): every process appends to the one
  file, each line carrying its process id. A process holds a shared lock on
  `$XDG_RUNTIME_DIR/notolith/notolith.log.lock` while it writes, and starts a fresh log
  (the old one kept as `notolith.log.old`) only when it can take the lock exclusively,
  that is when no other window has the log open.
- Sharing (`docs/SHARE.md`): a device's link is online once. A peer keeps one
  connection per identity and every process would publish the same mDNS name,
  so the share engine takes a lock per identity key (`link-<id>.lock` in
  `$XDG_RUNTIME_DIR/notolith`) and the second process says "Sharing is already running in
  another Notolith window" in the status bar instead of starting a second
  link. Stored shares, the vault's index, note history and sync journal are
  per vault digest and need nothing.
- Content-addressed caches (TeX, Mermaid, notebook outputs, EPUB images): the
  same key holds the same bytes, so two processes writing it do no harm; a
  reader can in principle meet a half-copied TeX PDF.
- Crash recovery snapshots and PDF recovery are per note path.

## Pop-out windows on Hyprland

A pop-out (`PopoutWindow`) is a parentless top-level window owned by the main
one, not a transient child: a parented `Qt::Window` becomes an `xdg_toplevel`
with a parent, which Hyprland floats over it like a dialog, whereas Obsidian,
Zed and VS Code pop-outs are independent toplevels. So a pop-out tiles like any
new window. To float them, add a window rule: they share the main window's
class (`io.github.jernejmarcic.Notolith`, from `setDesktopFileName`), so match
on the title, which for a pop-out is "<tab> - Notolith" where the main window's
is "<note>.md - <vault> - Notolith", or float the class and tile the main window
by hand. The code never calls `move()`,
which Wayland ignores for top-levels. Not checked on a real compositor: the
tests run offscreen, so the first pop-out on Hyprland is worth opening by hand.

## Mobile

There is no touch UI yet. What is already true, and what stands in the way:

**Reusable as they are.** `libs/vault`, `libs/markdown`, `libs/index`,
`libs/render`, `libs/theme` and `libs/settings` are plain C++ static libraries
with no widget code in them. A mobile app links them unchanged.

**The UI is the work.** QML is ruled out - its binding engine is a JavaScript
engine - so a mobile noteAppC is Qt Widgets on a touch screen. That works on
Android, but the editor is built for a mouse and keyboard: it needs kinetic
scrolling (`QScroller`), selection handles, touch-sized targets (table grips,
add bars and long-press cell selection already work by finger, docs/EDITOR.md
"Table handles"), and a layout
for a narrow screen with no sidebars. The shell (`libs/ui`) would be largely
new.

**The Qt version.** The Android kit (`$QT_ANDROID`, see "Build it" below) is
Qt 6.7.3; the desktop is 6.11. Anything noteAppC uses from Qt 6.8 or later will
not compile for Android, so the tree must stay buildable against 6.7.3.

**Two dependencies are not in the Android kit.** KF6 SyntaxHighlighting (code
blocks) and poppler-qt6 (PDF embeds) come from the Linux distribution, so both
are optional. `NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING` (default `ON`) and
`NOTEAPPC_WITH_PDFVIEW` (default `ON`, so poppler can be turned off even when
it is installed - the system poppler-qt6 is built against Qt 6.11 and cannot be
linked against the 6.7.3 kit) guard the `find_package` calls in
`libs/render/CMakeLists.txt` and `libs/ext/pdfview/CMakeLists.txt`. With
either off or the package missing, `CodeHighlighter`/`PdfPage` compile without
it: `CodeHighlighter::isAvailable()` and `PdfPage::isAvailable()` report
`false`, `highlight()` returns no token formats, and code blocks still render,
monospaced and uncoloured. The tests that need either skip (`GTEST_SKIP`) when
`isAvailable()` is false, and the PDF-support test asserts whichever the build
chose.

**Building against Qt 6.7.3.** `CMAKE_PREFIX_PATH` points at the Android kit's
`gcc_64` sibling build (`$QT_HOST`, a desktop Qt 6.7.3 build matching the
`android_arm64_v8a` one), with both options above `OFF`, in a separate build
directory. Every Qt module the root `CMakeLists.txt` asks for
(`Widgets Svg Sql Concurrent Network`, optional `OpenGLWidgets` and `Test`) is
in the kit. md4qt, fetched by `FetchContent`, used four Qt 6.8+ conversions
(`QString` built from a `QStringView`, and `QString(QChar::fromUcs4(c))`) that
do not compile on 6.7; `cmake/patch-md4qt.sh` replaces them with their
explicit spellings when md4qt is fetched, the way `cmake/patch-microtex.sh`
patches MicroTeX, and they compile on 6.11 too. noteAppC's own code uses no
Qt 6.8+ API.

To repeat the check:

```sh
env -u CFLAGS -u CXXFLAGS cmake -B build-qt673 -G Ninja \
    -DCMAKE_PREFIX_PATH=$QT_HOST \
    -DNOTEAPPC_WITH_SYNTAX_HIGHLIGHTING=OFF -DNOTEAPPC_WITH_PDFVIEW=OFF
cmake --build build-qt673
QT_QPA_PLATFORM=offscreen ctest --test-dir build-qt673
```

That desktop build of the Android kit's Qt proves the API surface, not that
the app runs on a phone. A debug arm64 APK, signed with the Android debug key,
builds too (see "Build it"); the touch-mode editor it needs is not written, so
this is packaging and CMake only.

**yaml-cpp.** The root `CMakeLists.txt` calls `find_package(yaml-cpp)` and
falls back to `FetchContent` (pinned to the `0.8.0` tag's commit, built as a
static library, the same pattern as md4qt and tinyxml2) when that comes up
empty, because the Android NDK sysroot has no yaml-cpp. Both paths land on the
`yaml-cpp::yaml-cpp` target. yaml-cpp's own `cmake_minimum_required` predates
what current CMake accepts, so the Android configure also needs
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`; it only affects that one fetched
dependency.

**Modules as Qt static plugins (`NOTEAPPC_STATIC_MODULES`).** Android loads only
native libraries packaged in the APK, so the loadable modules of
[PLUGINS.md](PLUGINS.md) "Tier 1b" cannot be `dlopen`'ed from
`<libdir>/notolith/modules` there. The CMake option `NOTEAPPC_STATIC_MODULES`
(default `OFF`; `ON` when `ANDROID`) builds a module as a static library
instead (`noteappc_add_static_module` in `libs/modules/CMakeLists.txt`: its one
source compiled with `QT_STATICPLUGIN`, `notolith-ink-static`,
`notolith-hwfont-static`), links it into the executable and imports it with
`Q_IMPORT_PLUGIN` in `apps/noteAppC/main.cpp`. `ModuleRegistry::scan` then also
walks `QPluginLoader::staticPlugins()` with the same IID check and the same
manifest: the module comes back with `ModuleInfo::isStatic` set and an empty
`path`, and `ensureLoaded` returns the static instance - constructed on that
first load, never by the scan. The ABI tag is still checked (it is the same
build, so it always matches). With the option off nothing changes: every
module stays a shared object, the registry finds no static plugin and pays one
walk over Qt's own (which carry another IID). `libnotolith-inkcore` is a static
library under the option (there is no directory beside the module for a shared
one to sit in), so the executable then links the stroke modeler and Abseil
itself; the `needed_exe` check drops `absl` from its list in that case.

Only the ink and hwfont modules go through the helper. The PDF viewer and share
modules stay shared objects: their poppler-qt6 and protobuf/libdatachannel
stacks are not in the Android kit and their options are off there. The ink
modules themselves are not built on Android yet either (the root
`CMakeLists.txt` gates `libs/ext/ink` on `NOT ANDROID`: Abseil and the stroke
modeler are not cross-built), so on Android today the option has no module to
link; it is the path the first one takes. Two things block lifting that gate: the root requires system Abseil
(`find_package(absl)`), which the NDK does not have, and `libs/ext/ink/core/CMakeLists.txt` forces
`INK_STROKE_MODELER_FIND_DEPENDENCIES` and writes a googletest shim that does `find_package(GTest CONFIG REQUIRED)`.
The stroke modeler's own CMake fetches a pinned Abseil and googletest when `FIND_DEPENDENCIES` is `OFF`, so the fix
is that setting under `ANDROID` and dropping `NOT ANDROID` from the gate. Checked on the desktop: a second
configure with `-DNOTEAPPC_STATIC_MODULES=ON` builds the whole tree, links
the executable with both module classes inside it and no `NEEDED` entry for
`libnotolith-ink*`, and `test_modules` lists, loads and attaches a static test
module (`tests/fakemodule/FakeStaticModule.cpp`); `test_window` and
`test_hwfont` run their ink and glyph-studio tests against the static modules.

**What is off, and why.** Four of the five options `docs/PLUGINS.md` lists as
Tier 1 have a system dependency with no arm64 build in this toolchain -
`NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING` (KF6), `NOTEAPPC_WITH_PDFVIEW` (poppler-qt6),
`NOTEAPPC_WITH_SPELLCHECK` (hunspell) and `NOTEAPPC_WITH_HTML` (litehtml) are
all passed `OFF` explicitly at configure time, rather than left to
auto-detect and fail to find anything. That is deliberate, not just tidy:
each is found through `pkg_check_modules`, which shells out to the *host's*
`pkg-config` rather than going through CMake's own sysroot-aware
`find_package` search, so an Android cross-configure risks it silently
matching the host's x86-64 `.pc` file instead of failing cleanly - a wrong
link, not a missing one. `NOTEAPPC_WITH_GRAPH`, `_CANVAS` and `_BASES` have
no external dependency (Tier 1 modules built to "work everywhere", per
`docs/PLUGINS.md`) and stay on; graph, canvas and Bases all compiled and
linked into the arm64 library without changes.

**`NOTEAPPC_WITH_MEDIA` is also off, for a different reason: it compiles
fine but androiddeployqt cannot deploy it.** Qt Multimedia's Android/FFmpeg
backend is a plugin selected at runtime through `QFactoryLoader`, and
androiddeployqt's automatic plugin detection only packages what it can infer
from static linkage - with `NOTEAPPC_WITH_MEDIA=ON` the build links
`libQt6Multimedia_arm64-v8a.so` cleanly but ships with no backend at all, so
`MediaPlayback` would report itself unavailable on every device regardless
of what the code does. Two documented fixes exist and both were tried and
rejected: `QT_ANDROID_EXTRA_PLUGINS` copies a directory into the APK as-is,
keeping the `multimedia/` subdirectory, which Qt's own flat
`lib/<abi>/libplugins_<type>_*` plugin scan then never finds; the
Multimedia-specific ELF dependency (`Qt6::QFFmpegMediaPlugin`) it is called
through also isn't declared in a non-static Qt build, so simply promoting the
`Qt6::Multimedia` CMake target to `GLOBAL` (which is otherwise the correct
fix for it being invisible outside `libs/render`'s directory scope, since
Qt's own `Qt6MultimediaTargets.cmake` imports it without `GLOBAL`) makes a
*different* Qt finalizer fail outright looking for that target.
`QT_ANDROID_DEPLOYMENT_DEPENDENCIES` (paths relative to the Qt install root)
is the documented fix for a Qt-shipped plugin androiddeployqt did not
detect, and it does place the file correctly, but it replaces the automatic
dependency list rather than adding to it - every transitive Qt library the
plugin itself needs (Core, Gui, Network, Multimedia) has to be listed by
hand or packaging fails outright, which is what happened on the one attempt:
`No platform plugin included in the deployment`, a broken APK. None of that
is worth carrying for a change that does not touch the editor or the embed
code; an audio/video embed on Android falls back to "open the file",
exactly as it already does on a desktop build without Qt Multimedia.

**Build it.** The placeholders are `$QT_ANDROID` (the Qt 6.7.3
`android_arm64_v8a` kit), `$QT_HOST` (the matching desktop Qt 6.7.3, `gcc_64`),
`$ANDROID_SDK_ROOT`, `$ANDROID_NDK_ROOT` (NDK 26.1.10909125) and `$JAVA_HOME`
(JDK 17); those are the versions the build is known to work with.

```sh
JAVA_HOME=$JAVA_HOME \
ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT \
ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT \
$QT_ANDROID/bin/qt-cmake \
    -S . -B build-android -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT \
    -DANDROID_NDK_ROOT=$ANDROID_NDK_ROOT \
    -DQT_HOST_PATH=$QT_HOST \
    -DBUILD_TESTING=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DNOTEAPPC_WITH_SYNTAX_HIGHLIGHTING=OFF -DNOTEAPPC_WITH_PDFVIEW=OFF \
    -DNOTEAPPC_WITH_SPELLCHECK=OFF -DNOTEAPPC_WITH_HTML=OFF -DNOTEAPPC_WITH_MEDIA=OFF

JAVA_HOME=$JAVA_HOME \
ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT \
ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT \
GRADLE_USER_HOME=/tmp/noteAppC-gradle \
cmake --build build-android --target apk --parallel 6
```

`GRADLE_USER_HOME` is pointed at a scratch directory on purpose, to keep the
Gradle cache out of the home directory. `BUILD_TESTING=OFF` skips `tools/` and `tests/` - the
screenshot tool, the index CLI and the bench harness are desktop developer
tools with nothing to gain from Android packaging, and GTest is not part of
this toolchain either.

Artifact:
`build-android/apps/noteAppC/android-build/build/outputs/apk/debug/android-build-debug.apk`,
18 MB, signed with the `Android Debug` certificate (`apksigner verify
--print-certs`). `aapt2 dump badging` confirms `package:
io.github.jernejmarcic.Notolith`, `sdkVersion:'26'`, `targetSdkVersion:'34'`,
a single `INTERNET` permission and `native-code: 'arm64-v8a'`. `unzip -l`
confirms `lib/arm64-v8a/libnoteAppC_arm64-v8a.so` (15 MB, every noteAppC
library statically linked into it), every linked Qt library
(Core/Gui/Widgets/Svg/Sql/Concurrent/Network/OpenGL/OpenGLWidgets), and every
plugin the app needs at runtime: `libplugins_platforms_qtforandroid`
(without it Qt cannot create a window at all), `libplugins_sqldrivers_qsqlite`
(the vault and index databases), `libplugins_imageformats_{svg,jpeg,gif,ico}`
and `libplugins_iconengines_qsvgicon` (Lucide icons and image embeds), plus
`styles`, `tls` and `networkinformation` that Qt itself always deploys on
Android. **Not run on a device** - no arm64 phone or emulator was used, so
touch input, the app's own layout at phone size, and the manifest's runtime
behaviour are all unverified.

**What a phone needs that the desktop does not: a vault location.**
`QStandardPaths::writableLocation(AppDataLocation)` resolves to the app's own
sandboxed storage under Android's scoped storage (in force since API 29,
i.e. every device this targets), not a path the user can browse to or Obsidian
vaults typically live in - "the latter directories will still be returned by
`QStandardPaths::writableLocation()` when usable, otherwise the app-private
location is returned based on the runtime Android version" (Qt's own
`QStandardPaths` docs). Reaching a vault the user already has, on shared
storage or in another app's folder, means Android's Storage Access
Framework - a system folder picker that hands back a `content://` URI, not a
filesystem path - which `Vault` does not speak today; `MANAGE_EXTERNAL_STORAGE`
(unrestricted filesystem access) is a Play Store policy risk for the scope
it asks for and is not the recommended route for a document-scoped app like
this one. Building that picker is real UI work and explicitly out of scope
here; on first run today, a vault would have to live under the app's private
`AppDataLocation`, invisible to any other app including a file manager,
which is workable for trying the build but not a real answer for someone's
existing notes.

**Plugins do not travel.** iOS does not let an app load code it did not ship
with, and Android only loads native libraries packaged inside the app. So
runtime plugins ([PLUGINS.md](PLUGINS.md), Tier 2) are desktop-only by
nature, while compiled-in modules (Tier 1) work everywhere. That is one more
reason Tier 1 comes first.

**Phones are tighter.** ARM cores, thermal throttling, a battery, and less
memory: 200 MB of peak RSS with a 1 MB note open is fine on a desktop and is
worth looking at before it is on a phone.
