#pragma once

// The documented seam for `engine = quickjs` (docs/PLUGINS.md, Tier 2 /
// Tier 3's Lua alternative). NOT BUILT: quickjs-ng is not installed on any
// machine this project builds on, and this codebase never fetches a
// dependency on its own initiative - only adding it to
// the root CMakeLists.txt's FetchContent block, the way md4qt/tinyxml2/
// MicroTeX/yaml-cpp are already pinned there, would change that.
//
// What plugs in here, when it is time:
//
//   class QuickJSEngine : public Engine {
//   public:
//       bool load(const PluginManifest &manifest, const noteappc_host *host,
//                 QString *error) override;
//       void unload() override;
//       int runCommand(const QString &commandID) override;
//       int renderCodeBlock(const QString &language, const QString &source,
//                           int widthPx, float devicePixelRatio,
//                           noteappc_block_result *out) override;
//       void vaultEvent(const QString &event, const QString &path) override;
//   private:
//       JSRuntime *m_runtime = nullptr;
//       JSContext *m_context = nullptr;
//   };
//
// `load()` would read `manifest.libraryPath()` as a `.js` file (plugin.ini's
// `library` key means "the entry script" for this engine, not a `.so`),
// evaluate it once, and expect it to have called three host-provided JS
// functions - `noteappc.onCommand(id, fn)`, `noteappc.onCodeBlock(lang,
// fn)`, `noteappc.onVaultEvent(fn)` - mirroring plugin.hpp's Plugin class
// almost exactly, since both exist to turn "the plugin registered these
// three things" into the same internal call shape main.cpp already drives
// through the Engine interface. The noteappc_host table (plugin.h) would be
// exposed to JS as a `noteappc.host` object built from the same four
// permission-gated groups (notes, clipboard, notice/log), via QuickJS's
// C function binding (JS_NewCFunction) - no new protocol messages, no new
// permission model, just a different way of calling the same host table a
// native plugin already gets.
//
// Selecting it: NativeEngine is the default and the only one main.cpp can
// actually construct while NOTEAPPC_WITH_QUICKJS is OFF (the default - see
// the root CMakeLists.txt option). Turning that option ON without
// quickjs-ng installed fails the CMake configure step with a clear message
// rather than a link error deep in the build; turning it on WITH the
// library installed still fails, on purpose, because QuickJSEngine.cpp does
// not exist yet - this header is the seam, not the implementation.
