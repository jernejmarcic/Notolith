cmake_minimum_required(VERSION 3.28)

project(noteAppC
    VERSION 0.1.0
    DESCRIPTION "A Linux-native Obsidian alternative in C++"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++20 modules anywhere in this project - every file uses #include - so
# CMake's module scanning is pure overhead: a scan step per source and a dyndep
# file per target. It is also what crashed ninja 1.13.2 on a no-op rebuild, an
# assertion in Plan::RefreshDyndepDependents, which made a failed build look
# like a successful one to anything that only read its tail.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg Sql Concurrent Network)
# Optional: the editor can paint its viewport through OpenGL instead of raster.
find_package(Qt6 6.5 QUIET COMPONENTS OpenGLWidgets)
find_package(Qt6 6.5 QUIET COMPONENTS Test)
qt_standard_project_setup()

# The ABI tag every loadable module carries (spec section 4): same build only.
# The token is random, made once per build directory and kept in the cache: two
# builds of one commit never share a tag, and a source tarball without git needs
# no hash. Reconfiguring keeps it; a fresh build directory gets a new one.
include(GNUInstallDirs)
if(NOT DEFINED NOTEAPPC_ABI_TOKEN)
    string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef noteappc_token)
    set(NOTEAPPC_ABI_TOKEN "${noteappc_token}" CACHE INTERNAL "Per-build-directory token of the module ABI tag")
endif()
set(NOTEAPPC_ABI_TAG "${PROJECT_VERSION}-${NOTEAPPC_ABI_TOKEN}-${Qt6_VERSION}")

# The loadable modules (libs/modules) as Qt static plugins, linked into the executable instead of dlopen'ed
# from <libdir>/noteAppC/modules. Android has no module directory and loads only libraries packaged in the
# APK, so there it is the only way (docs/PLATFORMS.md, "Mobile"); a desktop build can switch it on to test
# the path. Off, the default build is unchanged: every module is a shared object. Only the modules that go
# through noteappc_add_static_module (ink, hwfont) change; the PDF and share modules stay shared, as their
# poppler and protobuf stacks are not in the Android kit (those options are off there).
if(ANDROID)
    set(noteappc_static_default ON)
else()
    set(noteappc_static_default OFF)
endif()
option(NOTEAPPC_STATIC_MODULES "Build the loadable modules as Qt static plugins linked into the executable" ${noteappc_static_default})

# Neither dependency is in the Android Qt kit, so both can be turned off for a
# mobile/cross build; NOTEAPPC_WITH_PDFVIEW (below) covers dropping poppler-qt6
# when it is installed but built against a Qt too new to link with that kit.
option(NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING "Colour fenced code blocks with KF6 SyntaxHighlighting" ON)
option(NOTEAPPC_WITH_MEDIA "Play audio and video embeds with Qt Multimedia" ON)
# Raw HTML blocks (<div>, <table>, <details>...), rendered with litehtml.
# Optional the same way: without it, or without the package, a note's HTML
# blocks stay as source - see libs/render/HtmlRenderer and its CMake next to
# the poppler block in libs/render/CMakeLists.txt.
option(NOTEAPPC_WITH_HTML "Render raw HTML blocks with litehtml" ON)
# The first Tier 1 module (docs/PLUGINS.md). Off, or without hunspell, the app
# has no spellcheck code at all and its settings row does not appear.
option(NOTEAPPC_WITH_SPELLCHECK "Underline misspelt words with hunspell" ON)
# Note history in git (libs/ext/history), with libgit2 in process.
option(NOTEAPPC_WITH_HISTORY "Keep every save of a note as a git commit (libgit2)" ON)
if(NOTEAPPC_WITH_HISTORY)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBGIT2 QUIET IMPORTED_TARGET libgit2)
    endif()
endif()
if(NOTEAPPC_WITH_SPELLCHECK)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(HUNSPELL QUIET IMPORTED_TARGET hunspell)
    endif()
endif()
# The second Tier 1 module: a force-directed graph view, no external
# dependencies, so unlike spellcheck there is nothing to detect - the option
# alone decides whether libs/ext/graph is built at all.
option(NOTEAPPC_WITH_GRAPH "Build the graph view (libs/ext/graph)" ON)
# Another Tier 1 module: Obsidian's infinite canvas over the open JSON
# Canvas format. No external dependency - only Qt and the editor library, to
# reuse MarkdownEditor for a node's Markdown.
option(NOTEAPPC_WITH_CANVAS "Open and edit .canvas files (JSON Canvas)" ON)
# Another Tier 1 module: database-like table/card views over a vault from a
# `.base` file (libs/ext/bases/). yaml-cpp is already a dependency of
# libs/markdown, so this carries no new external dependency.
option(NOTEAPPC_WITH_BASES "Database-like table and card views from .base files" ON)
# Another Tier 1 module: an EPUB reader (libs/ext/epub/) - libarchive reads
# the book's zip container, litehtml (through HtmlRenderer, libs/render)
# lays out and paints each chapter. Actually built only when both are found
# (see the add_subdirectory below, after libs/render has looked for
# litehtml) - turning this on with neither installed builds nothing.
option(NOTEAPPC_WITH_EPUB "Read EPUB books, with highlights and notes" ON)
if(NOTEAPPC_WITH_EPUB)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBARCHIVE QUIET IMPORTED_TARGET libarchive)
    endif()
endif()
# Another Tier 1 module: exports a vault (or its `publish: true` part) as a
# static, JavaScript-optional website - Obsidian Publish style
# (libs/ext/publish/). No new external dependency: it only draws on
# libraries this project already links (markdown, index, render, theme).
option(NOTEAPPC_WITH_PUBLISH "Export a vault as a static website (libs/ext/publish)" ON)
# Another Tier 1 module: opens a vault's PDFs in a tab instead of handing
# them to the system viewer - continuous scroll, zoom, find, outline, links,
# and annotation (highlight/underline/strike-out/ink/note/box) saved into the
# PDF itself - and the poppler half of PDF embeds in a note (libnotolith-pdfpage.so).
# Both are loadable (libs/ext/pdfview): the executable links no poppler, and a
# session that never meets a PDF never maps it. Needs poppler-qt6 (GPL); this
# option is the one switch for the viewer and the embeds, e.g. for a kit that
# has a poppler built against another Qt.
option(NOTEAPPC_WITH_PDFVIEW "Open and annotate PDFs in a tab (poppler-qt6)" ON)
# Image annotation (libs/ext/imageview): an image opens in a tab to highlight,
# draw on and note, the marks kept in a sidecar note beside it. Qt Widgets
# only - no new dependency - so it has no tool to look for, just this switch.
option(NOTEAPPC_WITH_IMAGEVIEW "Highlight, draw on and annotate images, in a sidecar note" ON)
# Handwriting (docs/INK.md, spec docs/INK-DESIGN.md):
# the ink core as a shared library and the handwritten-note tab as a loadable
# module (libs/ext/ink), the glyph studio as another (libs/ext/hwfont). Off,
# nothing ink is built and the app has no ink symbol; on, the modules are
# separate install units the base binary never links (spec section 3).
# ink-stroke-modeler is fetched and pinned in libs/ext/ink/core/CMakeLists.txt.
option(NOTEAPPC_WITH_INK "Build the ink core and the handwritten-note module (libs/ext/ink)" ON)
option(NOTEAPPC_WITH_HWFONT "Build the handwriting-font glyph studio module (libs/ext/hwfont)" ON)
# Tier 2 (docs/PLUGINS.md): runtime plugins, each loaded inside its own
# `noteAppC-plugin-host` process (tools/plugin_host), never in the app. No
# external dependency at this level - only Qt, libdl and bubblewrap when
# present; NOTEAPPC_WITH_QUICKJS (tools/plugin_host/CMakeLists.txt) is the
# separate, still-off option for a second, scripted engine. Desktop only,
# by nature (docs/PLUGINS.md): off on Android regardless of this option,
# the same as the tools directory it lives under.
option(NOTEAPPC_WITH_PLUGINS "Build the out-of-process plugin host and its app-side glue" ON)
# The first Tier 2 plugin that is not a test fixture (docs/PLUGINS.md,
# "AI plugin"): a local Ollama server, or an OpenAI-compatible/Anthropic
# HTTP API, over libcurl. Only reachable when NOTEAPPC_WITH_PLUGINS is also
# on (plugins/ai add_subdirectory, further below, guards on both); off, or
# without libcurl, and the tree contains no plugins/ai code at all - the
# same "off means it was never even considered" every other optional module
# in this file promises.
option(NOTEAPPC_WITH_AI_PLUGIN "Build the first-party AI plugin (plugins/ai)" ON)
if(NOTEAPPC_WITH_AI_PLUGIN)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CURL QUIET IMPORTED_TARGET libcurl)
    endif()
endif()
# Another Tier 1 module: local-first sync to an optional server, end-to-end
# encrypted (docs/SYNC.md). Needs libsodium for the crypto and Qt6Keychain
# for the device identity key it keeps in the OS keyring; without either the
# option is silently off, same as spellcheck without hunspell.
option(NOTEAPPC_WITH_SYNC "Build sync to an optional end-to-end encrypted server (libs/ext/sync)" ON)
if(NOTEAPPC_WITH_SYNC)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
    endif()
    find_package(Qt6Keychain QUIET)
    # The reference server (tools/sync_server) only, never the client
    # library - a desktop build has no business linking an HTTP server.
    find_package(Qt6 6.5 QUIET COMPONENTS HttpServer)
endif()
# Encrypted notes (docs/ENCRYPTION.md): `.mdlock` files decrypted in memory
# only. Needs libsodium, the same as sync; without it the option is off.
option(NOTEAPPC_WITH_ENCRYPTION "Build encrypted notes (libs/ext/crypt)" ON)
if(NOTEAPPC_WITH_ENCRYPTION AND NOT TARGET PkgConfig::SODIUM)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
    endif()
endif()
# The last Tier 1 module: Jupyter-style code execution in notes
# (libs/ext/notebook) - a client of the Jupyter messaging protocol over
# ZeroMQ, so any installed kernel (ipykernel, a Julia or R kernel...) is a
# language, never Python running inside this app. Needs libzmq; without it
# the option is silently off, the same as spellcheck without hunspell.
option(NOTEAPPC_WITH_NOTEBOOK "Run code blocks through a Jupyter kernel (libs/ext/notebook)" ON)
if(NOTEAPPC_WITH_NOTEBOOK)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZMQ QUIET IMPORTED_TARGET libzmq)
    endif()
endif()

# Note sharing (docs/SHARE.md), first piece: the text CRDT behind one
# interface, with a Native C++ backend (always built) and the Rust yrs library
# behind a small C surface (libs/ext/share/yrs-ffi). Only the yrs one needs
# cargo; without it the option is silently off, like spellcheck without hunspell.
option(NOTEAPPC_WITH_SHARE_YRS "Build the yrs text CRDT backend for note sharing (libs/ext/share)" ON)
if(NOTEAPPC_WITH_SHARE_YRS AND NOT ANDROID)
    find_program(NOTEAPPC_CARGO cargo)
endif()

# Note sharing, the link layer (libs/ext/link, vendored from Hearth: identity,
# peer-to-peer Node over libdatachannel with mDNS discovery). Needs libdatachannel,
# avahi-client, protobuf, nlohmann_json and libsodium; if any is missing the
# option is silently off, like spellcheck without hunspell.
option(NOTEAPPC_WITH_SHARE "Build the peer-to-peer link layer for note sharing (libs/ext/link)" ON)
if(NOTEAPPC_WITH_SHARE AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AVAHI QUIET IMPORTED_TARGET avahi-client)
        if(NOT TARGET PkgConfig::SODIUM)
            pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
        endif()
    endif()
    find_package(Protobuf CONFIG QUIET)
    find_package(LibDataChannel QUIET)
    find_package(nlohmann_json QUIET)
endif()
if(NOTEAPPC_WITH_SHARE AND NOT ANDROID AND TARGET PkgConfig::AVAHI AND TARGET PkgConfig::SODIUM
        AND TARGET protobuf::libprotobuf AND TARGET LibDataChannel::LibDataChannel
        AND TARGET nlohmann_json::nlohmann_json)
    set(NOTEAPPC_HAVE_LINK ON)
endif()

# Optional: the syscall filter every bubblewrap sandbox gets (libs/sandbox).
# Without it the sandboxes are bubblewrap's namespaces alone.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SECCOMP QUIET IMPORTED_TARGET libseccomp)
endif()

add_compile_options(-Wall -Wextra)

# Link only the libraries a binary refers to. CMake hands every executable the
# whole of a dependency's link interface (protobuf's lists 79 Abseil libraries,
# Qt's lists libraries its own code does not touch), and each one named is a
# library the loader maps and relocates before main(): 27 Abseil libraries nobody
# calls were 13 ms of every start.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_link_options(-Wl,--as-needed)
endif()

# md4qt is not packaged on Arch; pin the KDE repository by commit.
# 5.x dropped the text-plugin hook the design assumed, so Obsidian's own syntax
# is a post-pass over the tree instead (libs/markdown/ObsidianSyntax).
include(FetchContent)
set(BUILD_MD4QT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_MD4QT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(md4qt
    GIT_REPOSITORY https://invent.kde.org/libraries/md4qt.git
    GIT_TAG e48716a41c1cf7f91b130fe3d3cc1c524e3b404a # v5.1.4
    GIT_SHALLOW FALSE
    # Four implicit conversions Qt 6.8 allows and 6.7 - the Android kit - does
    # not; see the script.
    PATCH_COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/cmake/patch-md4qt.sh
    EXCLUDE_FROM_ALL)
# MicroTeX renders the LaTeX in $$...$$ and $...$. It is not packaged, and it
# wants tinyxml2, which is not installed either - both are vendored and pinned.
# tinyxml2 is two files under the MIT licence; MicroTeX is MIT too.
set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Static whatever BUILD_SHARED_LIBS says: install() ships only the binary, so a
# shared vendored tinyxml2 left the installed notolith unable to start.
set(tinyxml2_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tinyxml2
    GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
    GIT_TAG 10.0.0
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(tinyxml2)

# MicroTeX populated but not added as a subproject: its own CMakeLists insists
# on a system tinyxml2 and also builds a sample app. SOURCE_SUBDIR points at a
# directory with no CMakeLists.txt, so FetchContent only downloads it and
# libs/render builds the pieces it needs.
# The patch turns Qt font merging off in MicroTeX's backend. TeX fonts are
# symbol-encoded, so with merging on Qt substitutes another font for the
# codepoints cmex10 and friends use, and every large operator, radical and
# delimiter comes out blank.
FetchContent_Declare(microtex
    GIT_REPOSITORY https://github.com/NanoMichael/MicroTeX.git
    GIT_TAG 0e3707f6dafebb121d98b53c64364d16fefe481d # master, 2024-08-06
    SOURCE_SUBDIR src
    PATCH_COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/cmake/patch-microtex.sh
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(microtex)

# The metainfo checked as it is in the tree, every run (ECM's appstreamtest
# checked only what an install had listed). No homepage yet,
# so that one warning is information.
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
    add_test(NAME appstreamtest
             COMMAND ${APPSTREAMCLI} validate --no-net --override=url-homepage-missing=info
                     ${CMAKE_CURRENT_SOURCE_DIR}/packaging/io.github.jernejmarcic.Notolith.metainfo.xml)
endif()

# md4qt's own CMakeLists pulls in ECM's KDECMakeSettings, whose translation
# helper runs `git rev-parse @{u}` / `git remote get-url origin` on this repo
# and prints git errors on every configure. Nothing here needs ECM.
set(CMAKE_DISABLE_FIND_PACKAGE_ECM ON)
FetchContent_MakeAvailable(md4qt)
unset(CMAKE_DISABLE_FIND_PACKAGE_ECM)

# In-tree the headers sit in src/, but upstream installs them as <md4qt/*.h>.
# One symlink and the includes read the same either way.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/md4qt-include)
file(CREATE_LINK ${md4qt_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/md4qt-include/md4qt SYMBOLIC)
target_include_directories(md4qt INTERFACE ${CMAKE_BINARY_DIR}/md4qt-include)

# yaml-cpp: frontmatter and Bases parsing (libs/markdown, libs/ext/bases).
# Found the normal way - the namespaced yaml-cpp::yaml-cpp target is what
# both consumers link against. The Android kit carries no yaml-cpp at all, so
# when find_package fails this falls back to building it from source, pinned
# like md4qt and tinyxml2 above; upstream's own CMakeLists only defines the
# bare "yaml-cpp" target, so that path is given the same namespaced alias.
find_package(yaml-cpp QUIET)
if(NOT TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG f7320141120f720aecc4c32be25586e7da9eb978 # 0.8.0
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(yaml-cpp)
    if(TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
        add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
    endif()
    message(STATUS "noteAppC: yaml-cpp not found on the system, building it from source (0.8.0)")
endif()

add_subdirectory(libs/vault)
add_subdirectory(libs/markdown)
add_subdirectory(libs/index)
add_subdirectory(libs/sandbox)
add_subdirectory(libs/render)
add_subdirectory(libs/editor)
add_subdirectory(libs/theme)
add_subdirectory(libs/settings)
add_subdirectory(libs/modules)
if(TARGET PkgConfig::HUNSPELL)
    add_subdirectory(libs/ext/spell)
endif()
if(TARGET PkgConfig::LIBGIT2)
    add_subdirectory(libs/ext/history)
endif()
if(NOTEAPPC_WITH_GRAPH)
    add_subdirectory(libs/ext/graph)
endif()
if(NOTEAPPC_WITH_CANVAS)
    add_subdirectory(libs/ext/canvas)
endif()
if(NOTEAPPC_WITH_BASES)
    add_subdirectory(libs/ext/bases)
endif()
get_property(noteappc_have_litehtml GLOBAL PROPERTY NOTEAPPC_HAVE_LITEHTML)
if(NOTEAPPC_WITH_EPUB AND TARGET PkgConfig::LIBARCHIVE AND noteappc_have_litehtml)
    add_subdirectory(libs/ext/epub)
    # The editor's EPUB embed card reads the cover through the reader's own
    # zip code; linked here because libs/editor is configured before the
    # reader, which itself links the editor.
    target_link_libraries(noteAppC_editor PRIVATE noteAppC_epubcover)
    target_compile_definitions(noteAppC_editor PRIVATE NOTEAPPC_HAVE_EPUBCOVER)
endif()
if(NOTEAPPC_WITH_PUBLISH)
    add_subdirectory(libs/ext/publish)
endif()
# Export and import through the pandoc program, if the person has it (never a build dependency).
if(NOT ANDROID)
    add_subdirectory(libs/ext/pandoc)
endif()
if(NOTEAPPC_WITH_PDFVIEW AND NOT ANDROID)
    add_subdirectory(libs/ext/pdfview)
endif()
if(NOTEAPPC_WITH_IMAGEVIEW)
    add_subdirectory(libs/ext/imageview)
endif()
if(NOTEAPPC_WITH_INK AND NOT ANDROID)
    # System Abseil is required: there is no bundled copy. Without it the ink
    # core and the note module are not built, and the rest of the app is unchanged.
    find_package(absl QUIET)
    if(absl_FOUND)
        add_subdirectory(libs/ext/ink)
    else()
        message(STATUS "Abseil not found: the ink core and the handwritten-note module are not built (install abseil-cpp)")
        set(NOTEAPPC_WITH_INK OFF)
    endif()
endif()
# Added only when the directory exists.
if(NOTEAPPC_WITH_HWFONT AND TARGET notolith-inkcore AND IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs/ext/hwfont)
    add_subdirectory(libs/ext/hwfont)
endif()
if(NOTEAPPC_WITH_PLUGINS)
    add_subdirectory(libs/ext/plugins)
endif()
if(TARGET PkgConfig::SODIUM AND TARGET Qt6Keychain::Qt6Keychain)
    add_subdirectory(libs/ext/sync)
endif()
if(NOTEAPPC_WITH_ENCRYPTION AND TARGET PkgConfig::SODIUM)
    add_subdirectory(libs/ext/crypt)
endif()
if(NOTEAPPC_WITH_NOTEBOOK AND TARGET PkgConfig::ZMQ)
    add_subdirectory(libs/ext/notebook)
endif()
# The Native CRDT backend needs only Qt, so this is built everywhere (Android
# too); the yrs one is added inside when NOTEAPPC_CARGO is set.
add_subdirectory(libs/ext/share)
if(NOTEAPPC_HAVE_LINK)
    add_subdirectory(libs/ext/link)
endif()
if(TARGET noteAppC_share_crdt AND TARGET noteAppC_link AND TARGET noteAppC_sync)
    add_subdirectory(libs/ext/shareengine) # the notolith-share module: not linked into the executable
endif()
add_subdirectory(libs/ui)
add_subdirectory(apps/noteAppC)
# The screenshot tool, the index CLI, the bench harness and the plugin host
# (tools/plugin_host) are desktop developer/runtime tools, not part of the
# app bundle itself; on Android they would need their own packaging for no
# benefit - and Tier 2 plugins are desktop-only by nature besides - so they
# simply do not build there.
if(NOT ANDROID)
    add_subdirectory(tools)
    if(NOTEAPPC_WITH_PLUGINS)
        add_subdirectory(examples/plugins)
        if(NOTEAPPC_WITH_AI_PLUGIN AND TARGET PkgConfig::CURL)
            add_subdirectory(plugins/ai)
        endif()
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installing matters for one reason: a .desktop file is what lets a file
# manager open a loose .md with noteAppC, which is the whole point of
# single-file mode - meaningless on Android, where qt_add_executable() makes
# noteAppC a MODULE target instead of an executable and this same
# install(TARGETS) would need a LIBRARY DESTINATION it has no use for.
if(NOT ANDROID)
    include(GNUInstallDirs)
    install(TARGETS noteAppC RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/io.github.jernejmarcic.Notolith.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES libs/theme/icons/noteAppC.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME io.github.jernejmarcic.Notolith.svg)
    # What software centres and Flatpak read to describe the app.
    install(FILES packaging/io.github.jernejmarcic.Notolith.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    # The AI plugin ships as part of the package, the same layout
    # ~/.local/share/noteAppC/plugins/ai/ uses (docs/PLUGINS.md) so a
    # distribution can symlink or copy it into place post-install; the
    # registry itself only ever scans $XDG_DATA_HOME today
    # (PluginRegistry::pluginsRoot()), so this alone does not make a
    # packaged install self-enable the plugin - that is still the user
    # opting in from the Community plugins settings page, same as any
    # plugin they installed by hand.
    if(TARGET ai)
        install(TARGETS ai LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/noteAppC/plugins/ai)
        install(FILES plugins/ai/plugin.ini
            DESTINATION ${CMAKE_INSTALL_DATADIR}/noteAppC/plugins/ai)
    endif()
endif()
