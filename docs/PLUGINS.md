# Extending noteAppC

**Status: proposal.** Nothing here is built. It exists so that when the first
extra is added, it goes in a way that keeps the core small and fast.

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
cmake -B build -DNOTEAPPC_WITH_SPELLCHECK=ON -DNOTEAPPC_WITH_GRAPH=ON
```

This is not new. It is the pattern the codebase already uses twice: PDF embeds
are compiled in only when poppler-qt6 is found, and the OpenGL viewport only
when Qt6::OpenGLWidgets is. Compiled out, a module costs literally nothing.
Compiled in, it is gated by a setting and not constructed until first used, the
way the settings page now is.

A module's settings rows appear only when it is compiled in. Today the page
shows a Spellcheck toggle with nothing behind it; under this scheme it would
not exist until the module did.

### Tier 2: runtime plugins - shared libraries with a C ABI

For third-party extras, and for first-party ones a distribution might ship
separately. A plugin is a directory:

```
~/.local/share/noteAppC/plugins/mermaid/
  plugin.ini            what it is and what it provides
  libmermaid.so         loaded only when needed
```

`plugin.ini` is INI for the same reason `settings.ini` is: a person can read and
edit it.

```ini
[plugin]
id = mermaid
name = Mermaid diagrams
version = 1.0.0
api = 1
library = libmermaid.so

[provides]
code-blocks = mermaid
commands = mermaid.copy-svg
```

**How it stays free until used.** After the first paint, the app reads each
`plugin.ini` - a few small files - and records what the plugins declare. It
does not load a single library. A `.so` is `dlopen`ed the first time something
it declared is actually needed: a `mermaid` code block scrolls into view, or its
command is invoked. A plugin nobody uses in a session is never loaded in it.

**Why a C ABI.** A C++ interface breaks whenever the compiler, the standard
library or the Qt version differs between the app and the plugin, and it breaks
silently, as a crash. A table of C function pointers does not. Authors still
write C++; a header-only wrapper hides the C.

```c
#define NOTEAPPC_PLUGIN_API 1

typedef struct noteappc_host {      /* what the app offers a plugin */
    uint32_t api;
    const char *(*note_text)(void);
    void (*replace_range)(int64_t start, int64_t end, const char *utf8);
    void (*open_note)(const char *path);
    void (*log)(const char *utf8);
} noteappc_host;

typedef struct noteappc_plugin {    /* what a plugin offers the app */
    uint32_t api;
    int  (*init)(const noteappc_host *host);
    void (*shutdown)(void);
    int  (*run_command)(const char *command_id);
    /* Draw a fenced block of a language this plugin declared, into RGBA. */
    int  (*render_block)(const char *language, const char *source,
                         int width_px, float device_pixel_ratio,
                         noteappc_image *out);
} noteappc_plugin;

NOTEAPPC_EXPORT const noteappc_plugin *noteappc_plugin_entry(void);
```

**Few extension points, each typed.** The whole list, to start:

| point | what a plugin gets | runs |
|---|---|---|
| command | a named action for the palette and hotkeys, with the host table above | UI thread, user-initiated |
| code block | render ```` ```lang ```` blocks it declared to an image | thread pool, cached |
| file type | open an extension it declared, e.g. `.canvas` | on demand |
| vault event | note opened, saved, renamed | thread pool, never blocking |

A plugin never gets the widget tree, the raw parser, or a hook inside a
keystroke. Code blocks go through exactly the machinery maths already uses:
rendered off the UI thread, cached by source and size, and a placeholder shown
until the image arrives, the way a remote callout icon is. The editor never
waits for a plugin.

**Budgets that are enforced, not hoped for.** Every load and every hook call is
timed. A plugin whose command takes over 100 ms, or whose render takes over a
second, is named in the settings page with its numbers. Obsidian shows startup
time per plugin; noteAppC shows every hook, and none of them are on startup at
all.

**Desktop only, by nature.** iOS does not let an app load code it did not
ship with, and Android only loads native libraries packaged inside the app. A
mobile noteAppC gets Tier 1 modules and nothing from this tier - see
[PLATFORMS.md](PLATFORMS.md).

**What it does not do.** A plugin is native code running in the app's process
with the user's rights, like any program the user installs. There is no
sandbox, and a plugin that crashes takes the app with it. Out-of-process
plugins over a pipe, the way language servers work, would fix both, at the cost
of latency that rules them out for rendering. That is a later option for heavy,
untrusted extras, not the default.

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
2. Tier 2 only when there is a first real plugin to build against. An API
   designed with no client is an API designed wrong. With a mobile app
   planned, Tier 1 is also the only tier that reaches every platform, which
   is a second reason to build it first.

## Decisions that are Jernej's

1. Are third-party runtime plugins (Tier 2) wanted at all, or only first-party
   modules compiled in (Tier 1)?
2. Scripts (Tier 3): yes with Lua, or never?
3. May a plugin add a pane to the sidebar? Doing that through a C ABI means the
   plugin hands over data - a tree of items - and the app draws it, so every
   pane looks like noteAppC. Handing over a raw `QWidget` would allow anything,
   but ties every plugin to the exact Qt build the app was compiled against.
