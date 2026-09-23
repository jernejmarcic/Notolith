# Extending noteAppC

**Status: Tier 0 and Tier 1 are proposal only (nothing to build - Tier 1 is
already how `libs/ext/` works, see HANDOFF.md); Tier 2 is built, 2026-09-23**
(`libs/ext/plugins/`, `tools/plugin_host/`, `include/noteappc/`, its own
section below). Tier 3 remains a proposal. This file exists so that each
extra goes in at the lowest tier that can do the job, keeping the core small
and fast.

## What it has to satisfy

Jernej's constraints, in his words where he gave them:

- "Simplicity first, and all other extras slot in nicely and stay super fast."
  The core stays small. An extra that is not in use costs **nothing** - not a
  millisecond at startup, not a check on the keystroke path.
- C and C++ only, no JavaScript anywhere. So noteAppC will **never run
  Obsidian's community plugins**, which are JavaScript against Electron. That
  is a permanent answer, not a gap to close later.
- Preloading is fine, but only after the visually important parts are on
  screen. Nothing optional runs before the first paint.

Obsidian is the counter-example worth studying. Its plugins receive the whole
application object and can hook anything, which is why they are powerful, and
also why a vault with forty plugins takes seconds to start and why a plugin
update can break the app. The design below trades reach for predictability on
purpose.

## Four tiers, cheapest first

An extra goes in the lowest tier that can do the job.

### Tier 0: configuration, not code

Much of what Obsidian users install plugins for is appearance or small
behaviour. noteAppC already covers a lot of it with files a person edits:
`callouts.ini` for callout styles, colour schemes in `themes/`, and
`settings.ini`. The first question for any extra is whether it is really a
setting or a config file. That costs zero code and zero time.

### Tier 1: first-party modules, compiled in, off by default

Optional features that ship with noteAppC: spellcheck, the graph view, Canvas,
Bases, export, git sync. Each is a library under `libs/ext/<name>/`, behind a
CMake option, and the application links it only when the option is on:

```sh
cmake -B /tmp/noteAppC-build -DNOTEAPPC_WITH_SPELLCHECK=ON -DNOTEAPPC_WITH_GRAPH=ON
```

This is not new. It is the pattern the codebase already uses twice: PDF embeds
are compiled in only when poppler-qt6 is found, and the OpenGL viewport only
when Qt6::OpenGLWidgets is. Compiled out, a module costs literally nothing.
Compiled in, it is gated by a setting and not constructed until first used, the
way the settings page now is.

A module's settings rows appear only when it is compiled in. Today the page
shows a Spellcheck toggle with nothing behind it; under this scheme it would
not exist until the module did.

### Tier 2: runtime plugins - out-of-process, over a C ABI

**Status: built, 2026-09-23.** For third-party extras, and for first-party
ones a distribution might ship separately. What follows describes what is
actually in the tree (`libs/ext/plugins/`, `tools/plugin_host/`,
`include/noteappc/`), not a proposal - the one change from the shape sketched
above is that the C ABI now loads inside a *separate host process*, never
inside the app, because "a plugin that crashes takes the app with it" turned
out not to be a cost worth paying once it was time to actually build this.

A plugin is still a directory, unchanged from the proposal:

```
~/.local/share/noteAppC/plugins/word-stats/
  plugin.ini            what it is, what it provides, what it asks for
  libword-stats.so       loaded only when needed, in its own process
```

`plugin.ini` is INI for the same reason `settings.ini` is: a person can read
and edit it. The real shape, from `examples/plugins/word-stats/plugin.ini`:

```ini
[plugin]
id = word-stats
name = Word stats
version = 0.1.0
api = 1
engine = native      ; native | quickjs (quickjs is a seam, not built - below)
library = libword-stats.so

[provides]
commands = count-words
code-blocks = calc
events =              ; note-opened, note-saved, note-renamed - empty means none

[permissions]
read-notes = true
write-notes = false
network = false
clipboard = false
```

**Process model.** One `noteAppC-plugin-host --plugin <dir>` process per
*enabled* plugin, not per installed one. The app starts it lazily - after the
first paint, and only the first time something the plugin declared is
actually needed: its command run from the palette, its code-block language
scrolling into view, or an event it subscribed to firing. A plugin nobody
uses in a session never has a process at all. A crash or a hang is contained
to that one process (`libs/ext/plugins/PluginHost.cpp` restarts it, with
backoff, the next time it is needed) and never reaches the app.

**Protocol.** Length-prefixed JSON over the host's stdin/stdout -
`libs/ext/plugins/PluginProtocol.h`, a 4-byte big-endian length prefix
followed by that many bytes of compact JSON, request/response pairs matched
by id in both directions plus one-way notifications. Either side can be the
requester: the app asks the host to run a command or render a block; the host
asks the app to read/write a note, use the clipboard, or show a notice -
which is how `noteappc_host`'s synchronous C calls turn into a network-style
round trip without either process needing a thread. An image in a
`codeBlock.render` response travels as base64 inside the JSON; the framing
itself is documented and unit-tested for partial reads, an oversized frame,
and malformed JSON (`tests/test_plugins_host.cpp`).

**The C ABI - unchanged from the proposal above**, still `NOTEAPPC_PLUGIN_API
1`, still a `noteappc_host`/`noteappc_plugin` function-table pair, still
`noteappc_plugin_entry()` as the one dlsym'd symbol. The real header is
`include/noteappc/plugin.h`, with four more host functions than the sketch
had (`list_notes`, `write_note`, `clipboard_read`, `clipboard_write`) and a
`render_code_block` that can return either an image or plain text, so a
plugin without an image encoder can still answer a code block. A header-only
C++ convenience wrapper is `include/noteappc/plugin.hpp` (`noteAppC::Plugin`,
`NOTEAPPC_PLUGIN_ENTRY`) for a future plugin that wants `std::function`
instead of raw function pointers - `examples/plugins/word-stats` is plain C
against the bare header, on purpose, as the shortest proof the ABI itself
works.

**Engine abstraction, inside the host.** `tools/plugin_host/Engine.h` is an
interface with one method per plugin hook; `NativeEngine` (`dlopen`, the C
ABI above) is the only one built. `QuickJSEngine.h` documents the seam a
scripted engine would fill - quickjs-ng is not installed anywhere this
project builds, and this codebase does not fetch a dependency on its own
initiative, so it is not built. `plugin.ini`'s `engine` key already
distinguishes `native` from `quickjs`, and the CMake option
`NOTEAPPC_WITH_QUICKJS` (default `OFF`) fails the configure step with a clear
message either way it could go wrong - the library missing, or (today,
always) the engine simply not written yet.

**Capability allowlist, enforced on every request.** `plugin.ini`'s
`[permissions]` declares what a plugin *wants*: `read-notes`, `write-notes`,
`network`, `clipboard`. What it actually gets is a separate, user-controlled
*granted* set (`~/.config/noteAppC/plugins.ini`, never the vault), set when
the user enables the plugin on the Community plugins settings page. Every
host request that needs a capability is checked against the granted set
before the corresponding app callback ever runs
(`PluginHost::handleHostRequest`); a refusal is an ordinary error response
(the plugin gets `NULL`/empty back, the same as "not found" - it cannot tell
the difference, which is the point) and is logged and emitted as a signal for
the settings page.

**Sandbox and limits.** The host runs under bubblewrap when `bwrap` exists -
the same allowlist pattern as `libs/render/TexCompiler.cpp`: only `/usr`,
`/etc`, the plugin's own directory and the host binary's own directory
(read-only), a private `/tmp`, no home, every namespace unshared except
network, which is shared only when the plugin's `network` permission was
granted. `QProcess::setChildProcessModifier` sets `RLIMIT_AS` (256 MiB) and
`RLIMIT_NOFILE` (64) between fork and exec, so the limit holds whether or not
bwrap is present. A per-request timeout in the app kills a host that does not
answer; the next command or render on that plugin restarts it, after a
backoff that grows with repeated failures. The host never sees a byte of the
vault except what the protocol hands it - no file access, no path to the
vault root.

**Few extension points, each typed** - v1 has three:

| point | what a plugin gets | runs |
|---|---|---|
| command | its bare id, run through `run_command`; the palette names it `pluginID:commandID` | the host process, on demand |
| code block | a declared ```` ```lang ```` block's source, rendered to text or an RGBA image, cached by (language, source, width, theme) | off the UI thread, with a placeholder shown until it lands |
| vault event | note opened, saved, renamed - fire-and-forget | the host process, only if the plugin declared `[provides] events` |

A plugin never gets the widget tree, the raw parser, or a hook inside a
keystroke - the code-block path is exactly the machinery maths and file
embeds already use (`MarkdownEditor::setFileEmbedRenderer`'s shape), which is
also why the editor-side hook this lane could not build itself
(`setCodeBlockRenderer`) is designed to slot in next to it rather than beside
it. See "For the lead" in this section's companion report for the exact
snippet.

**Budgets that are enforced, not hoped for.** Every command and every render
is timed; a command over 100 ms or a render over 1000 ms flags the plugin
"slow" on the settings page with its actual numbers, and host start time is
recorded too. None of it runs at startup - the first process for any plugin
only exists once something it declared is used.

**Desktop only, by nature.** iOS does not let an app load code it did not
ship with, and Android only loads native libraries packaged inside the app. A
mobile noteAppC gets Tier 1 modules and nothing from this tier - see
[PLATFORMS.md](PLATFORMS.md). Tier 2 does not build on Android
(`NOTEAPPC_WITH_PLUGINS`, like the rest of `tools/`, is skipped there).

**Nothing costs anything when off.** `community-plugins` is a feature switch
like any other (`Settings::features()`), default off - Obsidian starts in
restricted mode too. Off means `PluginRegistry::scan()` does not read a
single `plugin.ini`, not just that nothing is enabled.

### Tier 3: scripts - only if wanted

Small automation without a compiler: "insert today's date in this format",
"move finished tasks to an archive note". The natural fit would be Lua: a
250 kB C library, fast, embedded by many C and C++ applications. It would run
through the same command point as Tier 2 and nothing else.

It is listed last because it is the one tier that bends the "C and C++ only"
rule - the interpreter is C, but the scripts are not. It is Jernej's call.

## What belongs in the core

Everything a note-taking app is for, and nothing it is not:

- **Core:** the editor and its three modes, live preview rendering, links,
  backlinks, tags, search, the quick switcher, themes, callouts, code
  highlighting, maths, image embeds.
- **Tier 1 candidates:** spellcheck (hunspell), graph view, Canvas, Bases,
  PDF embeds (already optional), export, git sync.

## Order of building, when it is time

1. **Done 2026-09-23.** Spellcheck is the first Tier 1 module:
   `libs/ext/spell/` (hunspell) behind `NOTEAPPC_WITH_SPELLCHECK`, on by
   default and dropped when hunspell is not found. The editor does not link
   it - the window hands the editor three callbacks (is this word correct,
   suggestions, add a word), so the editor library is the same with or
   without the module. The dictionaries load on the thread pool after the
   first paint, and the settings row exists only in a build that has the
   module. It is the pattern the next module copies.
2. **Done 2026-09-23.** Tier 2, built against a first real plugin
   (`examples/plugins/word-stats`) rather than designed with no client - see
   above. Built out-of-process from the start rather than the in-process
   `dlopen` this section originally sketched, once "a plugin that crashes
   takes the app with it" was weighed against the cost of a pipe: noteAppC's
   own speed budget (docs/PERFORMANCE.md) is about the keystroke and paint
   path, which a plugin's process never sits on, so the latency Tier 2's
   first draft ruled out for rendering was not actually the cost it looked
   like once code blocks already went through a cache and a placeholder
   either way.

## Decisions that are Jernej's

1. Scripts (Tier 3): yes with Lua, or never?
2. May a plugin add a pane to the sidebar? Doing that through a C ABI means the
   plugin hands over data - a tree of items - and the app draws it, so every
   pane looks like noteAppC. Handing over a raw `QWidget` would allow anything,
   but ties every plugin to the exact Qt build the app was compiled against.
