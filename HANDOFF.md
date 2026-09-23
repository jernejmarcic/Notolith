# noteAppC handoff

Written 2026-09-22, brought up to date 2026-09-23, so a cold start can pick
this up without reading a transcript. `TODO.md` is the live task list;
`docs/PERFORMANCE.md` is the measured record.

## What this is

A Linux-native alternative to Obsidian in C++ with Qt 6 Widgets. No
JavaScript, no QML, no browser engine, no Electron.

- Plan and feature inventory:
  `~/Documents/AI/sessions/2026-09-22_011551_obsidian-clone-cpp-plan/`
  (`FEATURES.md` is what Obsidian does, `PLAN.md` is how this gets built).
- Session records:
  `~/Documents/AI/sessions/2026-09-22_013550_obsidian-clone-phase0/` (phases 0
  to 2) and `~/Documents/AI/sessions/2026-09-22_103114_noteappc-phase1-3-parallel/`
  (the parallel tracks, and every decision made since).

## Constraints that are not negotiable

1. **C and C++ only.** No JavaScript anywhere, which is also why Qt Widgets
   and not QML: the QML binding engine is a JS engine. No QtWebEngine.
2. **The file is the truth.** Never rewrite bytes that were not edited. CRLF
   and a BOM survive a round trip. `Vault::read`/`write` exist for this.
3. **Never create a dotfile.** Settings live in `~/.config/noteAppC`, the
   metadata cache in XDG cache keyed by vault path. A vault's `.obsidian` is
   read when present, written only if the user explicitly asks.
4. **The imitation stops at the edge of the note.** Note rendering follows
   Obsidian closely, because that is the thing being replaced. The shell is
   noteAppC's own. This changed mid-session; see "Next" below.
5. **Performance budget**, asserted in `tests/test_editor.cpp` so a regression
   fails the build: 8 ms per keystroke (a 200 kB note, a 5 000-item list and
   a 5 000-line code block, each after its parse has landed) and 300 ms to
   open a 200 kB note. Jernej's own rules on top: faster than Obsidian in
   every way; perceived speed never bought with more than 10 ms of real
   speed; nothing optional before the first paint; many CPUs, one core as the
   floor, a phone later; simplicity first, extras cost nothing when off.
6. Naming is modified camelCase: initialisms stay uppercase (`noteID`,
   `JSONdata`, `HTMLblock`), never `noteId`.

## State

All 10 test suites pass and the build is clean (the tenth, `spell`, exists
only in a build with the spellcheck module):

```
vault  markdown  index  editor  window  theme  icons  settings  spell
```

Release build on starbase-1, 2 000-note vault, `tools/bench/noteAppC-bench`:
window on screen 50.5 ms from process start, note open 1.2 / 6.9 / 2.6 / 16 ms
at 1 / 50 / 200 / 1 024 kB (above 128 kB the note is readable first and styled
when its async parse lands), keystroke 0.19-0.44 ms from 1 kB to 1 MB and
1.1 ms in a code block of any length, scroll frame 0.36 ms, one wheel notch
on screen 0.4-0.65 ms. Every fix behind those numbers, with before and after,
is in `docs/PERFORMANCE.md`.

Working today, beyond the basics (vault or loose file, the shell, live
preview / source / reading, index, find and replace):

- **Obsidian's note rendering**: per-element syntax reveal, a selection that
  never reveals syntax, inline and display maths (MicroTeX), callouts that
  nest and fold, tables, embeds, `Note > Heading` links, the properties table
  with text, number, date and list values editable in place.
- **Styling beyond Obsidian**, in [docs/STYLING.md](docs/STYLING.md):
  `callouts.ini`, `codeblocks.ini` and per-block overrides, code block labels
  that copy, tabbed code blocks, icons in four forms, custom themes from a
  CSS file or an Obsidian theme, all reloaded live.
- **Spellcheck**, the first optional module (`libs/ext/spell/`, hunspell):
  see [docs/PLUGINS.md](docs/PLUGINS.md) for the module pattern it sets.
- **Platforms**: builds and passes on the Android kit's Qt 6.7.3 with the
  optional dependencies off; see [docs/PLATFORMS.md](docs/PLATFORMS.md).

## Next

`TODO.md` has the list. The ones that are Jernej's to answer:

- three decisions in [docs/PLUGINS.md](docs/PLUGINS.md) (third-party
  plugins, Lua scripts, sidebar panes from plugins);
- run `bench-here.sh` from the brain's session directory on shuttlepod-1 and
  runabout;
- install a hunspell dictionary on starbase-1 (`hunspell-en_gb`);
- look in Obsidian at two behaviours nobody could confirm: whether the caret
  in a code block shows its fence lines, and how a block reference
  `[[Note#^id]]` is displayed.

## Performance, measured

**The full record is [docs/PERFORMANCE.md](docs/PERFORMANCE.md)**: current
numbers, every fix with its before and after, what was tried and rejected
(OpenGL, Vulkan), and the measurement pitfalls this project has already hit.
What follows is the original index finding, kept for its history.

The plan's budget is 20 000 notes cold-indexed in under 5 s. It is met, but
only after a fix: the full-text table was deleted by a non-indexed column, so
a cold build was quadratic.

| notes | before | after |
|---|---|---|
| 5 000 | 3.49 s | 0.98 s |
| 10 000 | 14.68 s | 1.57 s |
| 20 000 | 39.80 s | 3.57 s |

Reproduce it:

```sh
python3 tools/gen_bench_vault.py /tmp/bench-vault 20000
./build/tools/noteAppC-index --fresh /tmp/bench-vault
```

## Building and looking at it

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/apps/noteAppC/noteAppC <vault-or-file>
```

Under AddressSanitizer and UBSan, which found a use-after-free in a test and
a malloc/delete mismatch in MicroTeX on 2026-09-23 - worth a run after any
change to ownership or lifetimes. Wall-clock budgets fail under it, by design:

```sh
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer" \
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 QT_QPA_PLATFORM=offscreen ctest --test-dir build-asan
```

Render the window offscreen, which is how the look gets reviewed without a
display:

```sh
./build/tools/noteAppC_shot <vault> <note> out.png dark 1240 800
./build/tools/noteAppC_shot <vault> <note> out.png dark 1240 800 --source
```

`fixtures/demo-vault` is committed for exactly this: one note covering most of
the syntax, a second to link to and embed, and a `.obsidian` so the
vault-config path is exercised.

```sh
./build/tools/noteAppC_shot fixtures/demo-vault fixtures/demo-vault/Welcome.md out.png dark 1240 820
```

Use it rather than building a vault by hand, so the next person can retake the
same screenshot.

## Working with several agents in this tree

It works, with one rule: **ownership is by file and nothing overlaps.** The
split that worked was `libs/markdown`, `libs/editor`, `libs/vault`+`libs/index`,
`libs/ui`, each with its own test file, each building in its own
`build-<track>` directory, none of them running a git command. The parent
session owns `libs/theme`, `libs/settings`, `tools`, the root CMake,
`tests/CMakeLists.txt`, and every commit. Right-size the model per track:
parser and engine work needs the strong one, widgets and CRUD do not.
