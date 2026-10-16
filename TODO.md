---
title: noteAppC work in progress
type: todo
machine: starbase-1
updated: 2026-09-22
---

# TODO

`HANDOFF.md` is the cold-start document and the long list. This file is the
work in flight.

## Where the project lives

Jernej, 2026-09-23, on moving it to `~/Projects`: "Ok, if it is its own repo
then that is fine". It is - its own `.git`, 214 commits from `a3d9c6d`
"feat: bootstrap noteAppC", never tracked by the brain repo - so it stays at
`~/Documents/AI/projects/noteAppC`.

## Roadmap: the Notes app of `~/Projects/Ideas.md`

Jernej, 2026-09-23: "In general you are expanding to building the app detialed
in ~/Projects/ideas.md with all the specified features. Put items in the TODO
in a way that most suits the work, but do not stop untill all work is done."
So noteAppC grows into the whole "Notes app" section. In the order it will be
built; each stage's detail is in its own section further down.

| # | stage | state |
|---|---|---|
| 1 | Obsidian's features (the base everything else sits on) | done; the command palette (Ctrl+P) landed 2026-09-23 |
| 2 | Embeds of everything renderable; PDF page selection and slides; `.tex` | done; nested formatted transclusion (5 levels, cycle guard) 2026-10-16 |
| 3 | Every non-core feature switchable off at run time, costing nothing when off | done: 23 switches |
| 4 | `demo/`, a vault under 5 MB that exercises every feature | 354 kB, 77 files, `test_demo` opens all of it; grows with each stage |
| 5 | Version history: snapshots (done) and git (done, 0.09 ms a save) | done |
| 6 | Publishing: the vault as a static website (Obsidian Publish style) | done: `publish:export`, marked notes only by default, off the UI thread; 20 000 notes 6.7 s. Footnote back-links and no-JS code-tab strips 2026-10-16 |
| 7 | Media annotation: highlight, draw on and annotate PDFs and EPUBs; notes extracted from the annotations | done: PDF viewer (six annotation kinds saved into the file, extract to a note, recovery copies) and EPUB reader (highlights sidecar). Image annotation 2026-10-16 (`libs/ext/imageview`, sidecar `<image> annotations.md`, feature `image-annotation` off by default) (EPUB paged mode and CSS `url()` exist: EpubView.cpp, EpubRender.cpp) |
| 8 | Extensibility: out-of-process plugin host, capability allowlist, quickjs-ng engine, public API | done but quickjs-ng: sandboxed host per plugin, C ABI, commands, code blocks, vault events, permissions page. quickjs-ng: "Decide later" |
| 9 | AI, as an optional first-party plugin: local (ollama is installed) or an API | done: `plugins/ai` (Ollama, OpenAI-compatible, Anthropic; key by env var name only), `ai` code blocks, declared per-plugin timeouts. 2026-10-16: typed question (host `prompt` call, "AI: Ask a question") |
| 10 | Local-first sync to an optional server: entity-change journal and content hashes, E2EE (libsodium: Argon2id, XChaCha20-Poly1305, secretstream, 3-tier keys, X25519 lockboxes), client-side search only | done: engine, reference server (tools/sync_server), window wiring (sign in per session). Left: first full sync of 20 000 notes unmeasured, lockbox approval UI, key rotation |
| 11 | Real-time collaboration: a CRDT per note body, an E2EE blind relay (secsync's model), presence | todo; Jernej 2026-09-23: "yrs via C FFI" (ask before cargo fetches it: source, size) |
| 12 | Cross-platform: Linux and Android build; Windows, macOS, iOS, BSD need toolchains this machine lacks | partly blocked |
| 13 | Jupyter-style code execution, tiered sandbox, `#|` options, sidecar output, runnable file embeds ("when everything is said and done") | merged 2026-09-23: kernels over ZeroMQ, bubblewrap tier, output cache, run control, commands; 2026-10-01: shared kernels, rich output, more `#|` options, stale output, timeouts. completion/inspect built (NotebookComplete.cpp). 2026-10-16: editable `.ipynb`, `input()`/getpass prompts. Left: tiers 2-3 (not installed) |
| 14 | A profiling and optimisation pass over everything ("after ALL is done") | last |

Standing decisions the spec left open, kept as they are unless Jernej says
otherwise: **Qt Widgets, not Qt Quick** - the spec prefers Quick for GPU
composition, but Quick means QML, whose binding engine is JavaScript, and the
C/C++-only rule came first; raster measured faster than the OpenGL viewport
on this machine (docs/PERFORMANCE.md), which stays available as an option.
**JavaScript only in plugins**, in their own process, never in the app.

## Done this session

- [x] Establish true state: clean tree at `669af14`, all 7 suites green on
      starbase-1 (committing was blocked all session by the ssh signing key;
      fixed separately, see facts/system/SYS_CHANGES.md)
- [x] **MicroTeX glyphs**. Diagnosed the "large operators do not draw" gap
      recorded in `669af14`. Not font coverage: MicroTeX builds every font at
      1 pt and scales the painter ~19x, so Qt rasterised a 1 pt glyph and
      blew up the raster. Drawing outlines through `QPainterPath` fixes every
      operator, radical, delimiter, matrix bracket and roman glyph.
      `cmake/patch-microtex.sh` carries it.

## Resumed 2026-09-23, night (after the session limit)

- [x] The other session's four unfinished lanes finished and merged:
      slash commands (keys now handled by the editor itself; wired, off by
      default), paste and drop (a drop needs its drag accepted first; the
      window saves into the attachment folder), runnable `![[script.py]]`
      embeds with `eval`/`echo`/`sandbox` (an embed laid out before the
      runner now becomes runnable), footnotes in the editor (its
      FootnoteMarks.cpp had never been written) and the Footnotes view.
- [x] A note's `#| sandbox: host` runs outside the sandbox only when
      Settings allows it (off by default).
- [x] test_window crashed one run in six in libcrypto: libgit2 was
      initialised and shut down per note history, racing across threads.
- [x] A table command outside a table failed the every-command test.

## Merged 2026-09-23, evening

- [x] Lanes J (publish), K1 (PDF viewer), P (plugins) and S (sync) merged
      and wired; both builds green, 24 and 15 suites.
- [x] A clicked `[[Note#Heading]]` opens at the heading, `[[x.pdf#page=3]]`
      on page 3, `[[#Heading]]` in the note; web and mail links open in
      their apps (other schemes refused). Neither worked before.
- [x] `QTRY_*` in a gtest test passed on timeout; `tests/WaitFor.h` fails it.
- [x] Plugin manifests lost every list with more than one item (QSettings).
- [x] test_sync wrote device keys into the real OS keyring and waited 15 s
      per engine on it; an in-memory store in tests, 197 s -> 5 s.
- [x] `[[Note#^block]]` and `[[#^block]]` open at the block, the preview too.
- [x] Footnotes: parsed (refs, inline `^[..]`, definitions, numbering; the
      bodies' links and emphasis read at last), and a published page lists
      them. A `:::` fence's attributes are no longer tags. Index schema 5.
- [x] Images, audio and video open in a tab of their own.
- [x] **After the notebook lane merges (it owns libs/editor now):**
  - [x] Lists (Jernej 2026-09-23): nested markers no longer overlap their
        text, "-"/"*"/"+" drawn distinctly, Word-style 1.1/1.2 numbering as
        a setting ("Outline numbering"), lists nest inside quotes/callouts.
  - [x] Footnotes in the editor: references as superscript numbers, the
        definitions as Obsidian shows them (reading view: a list at the end).
  - [x] Paste and drop: an image pasted (Ctrl+V) or a file dropped into a
        note is saved per `files.attachmentFolder` (Obsidian's "/", "./",
        "./sub", "folder" forms) as "Pasted image YYYYMMDDHHmmss.png", and
        `![[...]]` inserted. Built since (libs/editor/PasteDrop.cpp, MainWindow attachment saver).
- [x] **Tables aligned in the source** (Jernej 2026-09-23: "Tables should be
      auto aligned to look good, even in plain markdown, by adding spaces and
      other table characters"). `formatTable` (libs/markdown/TableFormat):
      display-width padding (CJK 2, combining 0), delimiter row redrawn with
      its colons, outer pipes, short rows filled. Runs when the caret leaves
      a table it edited (only that table; one undo step; "Align tables"
      setting), and as "Format table" / "Format all tables". A table's
      source is monospace and wraps by the character so pipes stay in line.
      His screenshot was the caret-inside case: the table dropped to source.
- [x] **Click to edit** (Jernej 2026-09-23: "Clicking on an element should
      open it to be editable, like in obsidian"). Measured every element:
      display maths and HTML never took a click (fixed: offsetAt skipped
      blocks without a text layout); tables are now edited in place, cell by
      cell, as in Obsidian (TableCells.cpp). Callouts, quotes, headings,
      rules, links, embeds, code, tasks already opened on a click.
  - [x] Table row and column commands (insert, delete, move), palette and
        right-click menu.
  - [x] Table cells: a selection is drawn inside the rendered table (cells
        crossed whole are tinted); a short row draws its missing cells, and a
        click or Tab into one fills the row in first.
- [x] Sync asks the password every session: keep the derived keys in the
      keyring instead. Jernej 2026-10-16: "Use keyring" (lane sync).
- [x] Lockbox approval: a person approves a device enrolling by lockbox,
      comparing a fingerprint shown on both; before, every device the server
      listed was sealed the keys (a server could list its own). 9647a92.
- [x] Publish from the window: one dialog for folder, site name, home
      note, which notes and theme, remembered per vault (d397dc3).

## Reported by Jernej 2026-09-24, from the real app

- [x] "Not juypter kernel, should use system itself, or in a docker, most
      offecient way": blocks run with the system's own tools, sandboxed
      (DirectRunner); Jupyter only for a language with no recipe. Docker is
      not installed, so not built. docs/NOTEBOOKS.md.
- [x] The run control and its error icon covered the language label.
- [x] "Bad covering title" / "Cant edit the PDF embedd" / "When editing make
      sure that the rendered displayed element is not rendered, whjen editing
      the line that causes that": one bug - a block being edited kept its
      rendered PDF (or video, or card) state from before and painted it over
      its own source and the blocks below. Every layout now starts clean.

## Found 2026-09-24, fixed 2026-09-25

- [x] **test_demo crashed once in poppler** under a parallel ctest (1 in
      several full runs; 0 in 40 lone runs): SIGSEGV jumping to a garbage
      address from `GfxXYZ2DisplayTransforms` <- `GfxState` <- `Gfx` <-
      `Page::displaySlice` <- `Poppler::Page::renderToImage` <-
      `PdfPage::render` (libs/render/PdfPage.cpp). Root cause pinned down by
      reading poppler 26.08.0's own GfxState.cc (this system's poppler
      package, confirmed built with lcms2/USE_CMS via `ldd`): every render
      builds a `GfxState`, and both `GfxState::sRGBProfile` and
      `GfxXYZ2DisplayTransforms::XYZProfile` are process-global
      `std::shared_ptr` statics, each lazily filled in by an *unguarded*
      `if (!profile) profile = ...;` in the respective constructor - not a
      thread_local, not behind any mutex, shared by every thread in the
      process regardless of which `Poppler::Document` (or how many) each
      thread owns. PdfPage.cpp's thread-local Document-per-thread cache -
      exactly what poppler-qt6's own docs ask for - does not cover this: it
      keeps Documents from being shared unsafely, but these two statics sit
      *underneath* every Document. The first renders across several worker
      threads (test_demo's vault walk fans PDF renders out over
      `QThreadPool::globalInstance()` via `QtConcurrent::run`, one call per
      embed/page) can race on writing the same shared_ptr, tearing its
      internal pointer; a later read through it jumps to a garbage address -
      exactly the observed crash site. Not a per-thread-Document lifetime bug
      and not GlobalParams - it is poppler's own color-management globals.
      No core dump was left to inspect (`coredumpctl list test_demo` found
      nothing on this machine on 2026-09-25; systemd-coredump's retention had
      already rotated it out, crowded out by much larger Wine/Steam dumps
      logged the same day).
      Fixed in libs/render/PdfPage.cpp: `warmUpColorManagement()`, gated by a
      `std::once_flag`, runs one tiny (1x1) render alone the first time any
      thread reaches `PdfPage::render`; every other call - concurrent or not,
      then and for the rest of the process - finds both statics already set
      and never written to again, so reading them needs no lock of ours.
      Regression test: `tests/test_pdfpage_race.cpp` (new `test_pdfpage_race`
      ctest target, poppler builds only) fires 16 threads at
      `PdfPage::render` released together on a spin-gate, as the *first*
      thing to touch PdfPage in that process - deterministic given the fix,
      since `call_once` genuinely serializes the one racy window. Full
      suite: 100% passed (both the normal and the minimal build), test_demo
      alone x30 all passed, full `ctest -j8` x3 - 2 of 3 all green, 1 of 3
      had an unrelated `index` (libs/index/Indexer) flake under `-j8`
      contention that does not reproduce standalone and touches no
      poppler/PdfPage code - pre-existing, not part of this diff, left for
      whoever picks up Indexer next.

## Encrypted notes, requested by Jernej 2026-09-24

"on disk encryped notes that can be unlcoked wiht a passowrd. real on disk
encryption only is decrypted in RAM only. Need to enter password either when
cklicking the note or on startup (make this a toogle)". Decided with him:
**noteAppC's own format, Argon2id** (not Meld Encrypt's PBKDF2 .mdenc), and in
the on-open mode the password lasts **until locked or the app closes**.

- [x] `libs/ext/crypt` (NOTEAPPC_WITH_ENCRYPTION, libsodium): `.mdlock` files,
      header (magic, version, Argon2id params, salt, nonce) authenticated as
      AD, XChaCha20-Poly1305 body; keys and password in sodium_malloc memory.
- [x] Session: unlock on first open or at vault open (`encryption.unlockAtStartup`),
      auto-lock after idle minutes, "Lock encrypted notes"; lock saves and
      closes every encrypted tab. No core dumps while unlocked.
- [x] Never plaintext on disk: no file-recovery snapshot, no git history, no
      notebook output cache, no plugin code blocks or selection reads for an
      encrypted note; save writes ciphertext atomically.
- [x] Commands: encrypt this note, decrypt this note, new encrypted note,
      lock, change password (re-encrypts every `.mdlock`).
- [x] `[[Secret]]` still resolves after `Secret.md` became `Secret.mdlock`.
- [x] docs/ENCRYPTION.md: format, threat model, what it cannot do (swap,
      old plaintext in history/snapshots/backups before encrypting).

## Callouts, requested by Jernej 2026-09-22

Decided with him: **named styles in config, inline overrides**, carried in
**Obsidian's own metadata slot** so notes stay portable.

    > [!deadline] Ship noteAppC
    > [!deadline|bg=#00FF0022] one-off override
    ::: {.deadline bg="#00FF0022" title="..."}

Style file `~/.config/noteAppC/callouts.ini`, one section per type, merged
over the 13 built-in Obsidian types:

| key | meaning |
|---|---|
| `icon` | Lucide name, or a `file://` / `https://` URL. Always drawn at one size. |
| `bar` | left rail colour, `#RRGGBBAA` |
| `bg` | panel fill, `#RRGGBBAA` |
| `font`, `size`, `color`, `style` | body text |
| `heading-font`, `heading-size`, `heading-color`, `heading-style` | title row |

`style` takes any of `bold italic underline strikethrough`, space separated.

- [x] `CalloutStyle` in `libs/theme`: the struct, the built-in table, the INI
      loader, `#RRGGBBAA` parsing
- [x] Parser: read `|key=value` attributes out of the marker into
      `CalloutSpan::attributes`; pass the `:::` form's existing attributes
      through the same field
- [x] Editor: resolve style (built-in -> config -> inline) and paint it -
      rail, fill, icon, both fonts, both colours, both sizes, effects
- [x] Icon from a URL: local file directly, remote fetched once into the XDG
      cache and drawn from there, never blocking a repaint - `Icons::notifier()`
      signals `iconReady` so the widget repaints when it lands
- [x] **Nesting.** The parser already had it right; the editor did not. Every
      callout that overlaps a block is now resolved with it, the layout's
      per-line indent (the hook lists already used) steps each level in, and a
      nested callout keeps its title in the layout where the format pass
      styles it - which avoided teaching DisplayMap to replace a range.
      `hideQuoteMarkers` was also only ever hiding one `>` per line.
- [x] **Code blocks inside a callout.** The listing takes its monospace font
      and its tokens from the style pass, and a real panel is painted behind
      it. The highlighter is fed the *displayed* text, because the `>` that
      starts each line is hidden and the offset shift is not uniform.
- [x] A callout page in `fixtures/demo-vault` so the look can be retaken

## Found while looking, not yet scheduled

- [x] **Inline `$...$` maths never renders** - it is parsed and styled purple
      but only display maths is laid out, in `layoutMath`, which takes a whole
      block. Needs an inline image run inside a `QTextLayout` line, which the
      editor has no mechanism for yet; block-level embeds have the same limit.

## Performance, requested by Jernej 2026-09-22

"Optimal, GPU accelerated, and on CPU only still ultra fast. Much better than
Obsidian." Measured first, then changed - a budget nobody measured is not a
budget, which this project has already learned once on the index.

- [x] **Measured.** `tools/bench/` builds `noteAppC-bench`: cold start, note
      open, keystroke latency, scroll frame time and peak RSS, median of
      several runs, `--json` for diffing a later run against this baseline.
      On starbase-1, 2 000-note vault, after the backlinks fix below:

      | metric | median |
      |---|---|
      | cold start to first painted frame | 199 ms |
      | note open, 1 kB / 50 kB / 200 kB | 1.4 / 9.4 / 53 ms |
      | note open, 1 MB | 822 ms |
      | keystroke, relaid and painted, 200 kB | 0.29 ms (budget 8 ms) |
      | scroll frame, 5 000-line note | 0.57 ms median, 1.64 ms worst |
      | peak RSS | 201 MB |

- [x] **Backlinks no longer scan the vault.** `Index::backlinks()` read every
      non-external link in the database and called `resolve()` on each, so
      opening one note cost time proportional to the whole vault. Links now
      carry `tname`, the folded last component of the target, indexed - the
      only thing a target that resolves to a note can be guaranteed to share
      with it. Opening a 1 kB note on a 2 000-note vault went 14.6 ms -> 1.4 ms,
      and no longer depends on vault size at all. Schema version 3.
- [x] Measured against Obsidian 1.14.4 on the same generated vaults, 2026-10-16:
      docs/PERFORMANCE.md "Against Obsidian" (noteAppC ahead on every row:
      cold index 1.07 s vs 36.7 s on 20 000 notes, RSS 214 vs 1541 MB).
      Found: TagsPane::refresh runs one notesWithTag query per tag on the UI
      thread (4.1 s warm open on a 5 000-note vault with 4 500 tags).
- [x] TagsPane::refresh stall on tag-heavy vaults: compute tag counts in one
      grouped query or off the UI thread; add a ~4 500-tag note inside the
      vault to tools/bench.
- [x] **Opening a note was quadratic, twice over.** Found with perf, not
      guessed. `ObsidianSyntax::scan` ran the lazy `%%...%%` pattern once per
      unmasked region - 16 273 of them on a 1 MB note - and each run with no
      match walks to the end of the text: parse 1 992 ms -> 142 ms. And
      `OutgoingLinksPane::refresh` re-read the note from disk, re-decoded it
      and rebuilt a `SourceMap` **once per link**, which perf put at 30% of
      the whole run.
- [x] **A big note is shown before it is parsed.** Above 128 kB the text goes
      up as plain paragraphs and the parse runs on the thread pool. Time to a
      readable 1 MB note: 146 ms -> 11.3 ms. `plainParagraphs()` makes it
      possible; without it an unparsed note is one block a megabyte long.
- [x] **The settings page builds when opened**, not twice before the window
      appears: constructor 27.5 ms -> 15.4 ms.
- [x] **Show the window, then read the vault.** Shell on screen 194.5 ms ->
      50.5 ms, at a real cost of 8.8 ms - inside the 10 ms limit, but only
      just, and recorded in the commit so it is not rediscovered.
- [x] **The O(document) passes on every keystroke**, 2026-09-23 (fix 11 in
      `docs/PERFORMANCE.md`): `SourceMap` moves line starts instead of
      rescanning, `updateLayout()` makes one pass and finds the viewport by
      binary search, list items are looked up per block. 1 MB 0.71 -> 0.44 ms.
      Left: `shiftRanges` and the placement pass, both still O(blocks).
- [x] **GPU: implemented, measured, off by default.**
      `MarkdownEditor::setGPUacceleration` swaps the viewport for a
      `QOpenGLWidget`, and `noteAppC-bench --gpu` compares the two on screen
      with vsync off (rendering into a QImage would have measured a
      framebuffer readback instead). On an RTX 5080 the whole scroll sweep
      costs ~420 ms on raster and ~467 ms on OpenGL, so GL is about 11%
      slower overall - the glyph cache is the usual reason for text - while
      holding a steadier tail, 5 ms worst frame against 12 ms. A setting, not
      a decision, because the tail may matter more on weaker hardware.
- [x] Guards that cannot quietly stop guarding:
      `ParserPerfTest::ParsingStaysLinearInFileSize` measures `parseAll`
      directly (5.0x text, 5.6x time) and
      `MarkdownEditorTest::OpeningAMegabyteNoteIsImmediate` measures time to
      readable. The first version of this lived in the editor test and was
      about to start passing for the wrong reason once the parse moved off
      the UI thread.

## Still open

- [x] **Inline `$...$` maths never renders.** Parsed and styled, never laid
      out. Needs an inline image run inside a `QTextLayout` line. The trick
      that avoids teaching DisplayMap to substitute text: hide all but one
      character of the formula and give that character an absolute letter
      spacing equal to the image width, then paint the image at its x.
- [x] **`Note > Heading` links.** `[[Note#Heading]]` reads "Note > Heading"
      and `[[#Heading]]` reads "Heading", as the Obsidian forum confirms for
      both views. No DisplayMap substitution needed: the "#" stays, invisible
      and as wide as " > ", which is painted over it.
- [x] How Obsidian displays a block reference `[[Note#^id]]` is not recorded
      anywhere checked; shown as written until someone looks. 2026-10-16:
      Obsidian shows `[[Note#^id]]` as written, `[[#^id]]` as "^id", and
      `![[Note#^id]]` embeds only that block, untitled; ours matches.
- [x] Spellcheck: the first Tier 1 module, `libs/ext/spell/` (hunspell),
      behind `NOTEAPPC_WITH_SPELLCHECK`. Wavy underline on prose (not code,
      links, tags, maths, or the word being typed), right click for
      suggestions and "Add to dictionary" (`~/.config/noteAppC/dictionary.txt`).
      Dictionaries follow the system languages and load after the first paint.
- [x] **Jernej: starbase-1 has no hunspell dictionary**, so spellcheck shows
      a status message and underlines nothing. Installed 2026-09-25
      (hunspell, hunspell-en_gb); en_GB loads and flags "sentance". A bare
      "en" after en_GB no longer loads en_AG, a symlink to the same files. `sudo pacman -S hunspell-en_gb`
      for the en_GB locale. Slovenian is not in the Arch repositories; its
      `sl_SI.aff`/`.dic` from LibreOffice's dictionaries work dropped into
      `~/.local/share/hunspell/`, with `sl` added to `LANGUAGE`.
- [x] **Verify first**: a listing inside a callout wraps here. The claim
      that Obsidian scrolls it was mine and unchecked; the session notes say
      Obsidian's live preview wraps too, and only its reading view is thought
      to scroll. Nothing to change until someone looks. 2026-10-16: Obsidian
      wraps it; ours already did (test added).
- [x] Property values: text, number, date and date-time edit in place (click
      the value; Enter or clicking away keeps it, Escape drops it). Written
      through yaml-cpp, quoted only when needed, and refused when it would
      change the type or touch anything else on the line.
- [x] List properties: a chip's cross removes it, the space after the chips
      adds one. Rewritten in the style written (block or flow), other items
      kept byte for byte; a list with a comment in it stays read-only.
- [x] Editing an existing list item in place: click its text.
- [x] Measure against Obsidian itself: see the 2026-10-16 entry above.

## Requested 2026-09-23

- [x] Vulkan: answered with numbers in `docs/PERFORMANCE.md`. Buildable here
      (Qt 6.11, `QRhiWidget`) but it is a renderer of our own, not a switch,
      and raster fits this machine's 3440x1440 240 Hz panel with room.
- [x] Document every optimisation: `docs/PERFORMANCE.md`.
- [x] Plugins: a proposal in `docs/PLUGINS.md`. **Three decisions are
      Jernej's** (third-party plugins at all, Lua scripts, sidebar panes).
- [x] Many machines, Intel and AMD, multi-core target with one core as the
      floor: `docs/PLATFORMS.md`. No `-march` anywhere, no x86-only code.
- [ ] **Run `bench-here.sh` on shuttlepod-1 and runabout.** In the brain's
      session directory for this work. Jernej runs it; results sync back.
- [x] Scroll by blitting (`QWidget::scroll`) instead of repainting the whole
      viewport: a wheel notch 1.0 -> 0.4-0.65 ms on screen (fix 12).
- [x] Measure the Wayland shared-memory buffer transfer the bench cannot see:
      0.28-0.31 ms of compositor CPU per raster frame at 1180x760 against
      0.065 ms for OpenGL, in cage (docs/PERFORMANCE.md). Raster still wins
      in total. Hyprland's own figure would need a window on a real monitor.
- [x] Mobile groundwork: build the libraries against the Android kit's Qt
      6.7.3 to find any 6.8+ API; make KF6 SyntaxHighlighting optional the
      way poppler already is.
- [x] `PKGBUILD` overrides CachyOS's `-march=native` (it strips it).

## Requested 2026-09-23, queued

Jernej's rule: a new feature goes here and the current task carries on.

### In flight

- [x] **Inline `$...$` maths** - the lane stopped before the fallback for an
      unrenderable formula; `UnrenderableInlineMathKeepsItsSourceVisible`
      fails. Finish it before anything else in the editor lands.
- [x] Lane: Nerd Font glyphs in `Icons::icon()`, and `LanguageStyles`
      (`codeblocks.ini`) mirroring `CalloutStyles`.
- [x] Lane: navigation - leaving settings on any navigation, single-click
      folders, back/forward with mouse side buttons and Alt+arrows, caret and
      scroll remembered per note, reopen the last note, reveal the open note
      in the tree, Ctrl+, for settings.
- [x] `Settings.defaultsAreCompiledIn` read the real `~/.config/noteAppC` and
      failed once Jernej picked the OLED theme. Every settings test now runs
      against a private config directory.

### Editor, after the maths

- [x] **Selecting a line reveals markdown syntax; Obsidian does not.** Today
      the whole block goes raw when the caret enters it
      (`Decorations::hiddenFor`), even while a selection is being dragged. Two
      rules: never reveal during a non-empty selection, since text shifting
      under the mouse mid-drag is the worst version of this; and with a caret,
      reveal only the markers of the element the caret is inside, not the
      whole block.
- [x] **Code blocks use the callout style format**, in Jernej's words "the
      format that has been written for callouts":
      - named styles per language in `~/.config/noteAppC/codeblocks.ini`
        (`name`, `icon`, `color`, `aliases`, `inherits`);
      - per-block overrides after the language on the fence,
        `` ```python name="Setup" icon=U+E73C ``, parsed by the same
        `parseFenceSpec` callouts use. CommonMark takes the first word of an
        info string as the language, so Obsidian still highlights it;
      - the language name on the side of the header, **clickable, copying the
        block's code to the clipboard**, with a moment of "Copied" feedback;
      - icons in every form callouts take: built-in (Lucide), Nerd Font glyph,
        local file, URL.
- [x] **Code block tabs.** Adjacent fences, with only blank lines between,
      that each carry a `tab` attribute group into one block with a tab strip:
      `` ```python tab="Python" `` then `` ```cpp tab="C++" ``. A bare `tab`
      labels the tab with the language's style name. Any other text between
      them ends the group. Obsidian shows them as separate blocks, unchanged.
- [x] **Callout gaps**, all verified 2026-09-23 - all done but 4, which is
      the "Verify first" item above and needs Obsidian to compare with:
      1. a remote `https://` icon never appears until something else repaints:
         nothing listens to `Icons::notifier()`'s `iconReady`;
      2. editing `callouts.ini` needs a restart - it is read once, and a
         comment in `MarkdownEditor.h` wrongly says it reloads on theme change.
         Watch the file, and `codeblocks.ini` with it;
      3. nested callouts cannot fold - only the outermost has a handle;
      4. a listing inside a callout wraps - see "Verify first" above;
      5. Nerd Font glyph icons (arrives with the icon lane).

### Next, after tabs

- [x] **The word count was O(document) on every keystroke** - the shell
      recounted the whole note on `textChanged`, while the parser already
      counted it on the worker thread and the editor threw that away. It now
      comes with each parse. Keystroke through the window, 200 kB: 0.33 ms ->
      0.21 ms, which also shows the app-wide event filter costs nothing
      measurable.

### Later

- [x] **Latent: decorations other than syntax, styles, blocks and properties
      were not shifted** between a keystroke and the reparse. Closed with
      fix 11: every decoration kind and the cached maps of the blocks below
      an edit now move with the text. `ShiftTest` guards it.

- [x] **Custom themes from a CSS file** - done in 256d913. Corrected: a picker
      already exists on the settings page and already lists Obsidian themes.
      The real gaps, verified: `Scheme::searchPaths` uses `QStandardPaths`,
      which resolves to `~/.config/noteAppC/noteAppC/themes` rather than the
      documented `~/.config/noteAppC/themes`, so a theme put where the docs
      say is never found; a loose `.css` file is not picked up, only an
      Obsidian `<name>/theme.css` folder; nothing reloads on change.
- [x] Mobile groundwork: KF6 SyntaxHighlighting and poppler are optional
      (`NOTEAPPC_WITH_SYNTAX_HIGHLIGHTING`, `NOTEAPPC_WITH_PDF`). Against the
      Android kit's Qt 6.7.3, vault, theme, settings and render build and pass.
- [x] **The whole tree builds and passes on Qt 6.7.3**, the Android kit's
      version, after patching md4qt's four 6.8-only conversions at fetch
      (`cmake/patch-md4qt.sh`). noteAppC's own code uses no 6.8+ API.
- [x] **The actual Android build** (Sonnet lane, 2026-09-23): `android/`
      (manifest, CMake wiring), yaml-cpp found via `find_package` with a
      FetchContent fallback (needed for Android - see
      `build(deps)` commit), 18 MB debug arm64 APK built and its contents
      checked. NOTEAPPC_WITH_MEDIA also off there: Qt Multimedia links, but
      androiddeployqt's plugin auto-detection cannot see its runtime-loaded
      backend, and the two documented fixes (`QT_ANDROID_EXTRA_PLUGINS`,
      `QT_ANDROID_DEPLOYMENT_DEPENDENCIES`) either land the plugin somewhere
      Qt's loader never scans or require hand-listing every transitive Qt
      dependency and broke the whole package on the one attempt made - not
      worth it for a lane that does not touch the editor. Detail in
      `docs/PLATFORMS.md`. A touch mode for the editor is still open, and
      the APK has not run on a device.
- [x] **Typing in a long code block was over budget.** Release, keystroke
      plus paint: 100 / 1 000 / 5 000 lines went 2.7 / 26 / 158 ms ->
      1.10 / 1.10 / 1.16 ms. Fences over 32 lines are 32-line blocks, the
      highlighter carries `CodeHighlighter::State` piece to piece and stops
      once it converges, and its formats are cached per Kate format. Written
      up as fix 10 in `docs/PERFORMANCE.md`; guarded by `CodeBlockPerfTest`
      and `CodePieceTest`.
- [x] A long listing inside a callout is still part of one layout. A
      callout is now split into 32-line pieces like a fence; typing in a
      2000-line listing inside one went from 88.85 ms to 0.70 ms a key
      (`CodeBlockPerfTest.TypingInALongListingInsideACalloutStaysInBudget`).
      ponytail: the highlighter's state is not carried across a callout's
      pieces, so a construct spanning a piece boundary restarts there.
- [x] **Verify first: does the caret in a code block show its fences?**
      Here it does not - md4qt's code block covers the listing alone, so the
      two fence lines sit in the gap blocks either side and are revealed only
      with the caret on them, which also means the language tag can only be
      edited from its own line. I believe Obsidian shows both fences as soon
      as the caret is anywhere inside; check before changing anything.
      2026-10-16: it does; ours now shows both fences with the caret anywhere inside.
- [x] Mobile groundwork, the `-march=native` PKGBUILD guard, scroll by
      blitting, the Wayland buffer measurement - each done, see above.

## Requested 2026-09-23, afternoon: embeds

Jernej: "make sure that the inline ![[]] links work, for things such as
imanges, PDFs, and basically everything that can be rendered in a sensical
way", and for PDFs "arguments inside the link that can display a specific PDF
page, or display the pages in a specific manner. Such as displaying mutiple
plages, all pages, certain pages, pages in a range. Also make it, instead of
scrolliong through pages, it can make them like a slide".

- [x] Every `![[...]]` kind (lane I, merged 9faaafe): notes (whole, heading,
      block), images, PDFs, audio, video, source files, a card for any other
      file (binary content detected, never shown as text); several embeds in
      one paragraph render as rows. `.base` embeds are the base block's own
      table fed the file; `.canvas` embeds are the canvas drawn to an image.
- [x] PDF page arguments (`libs/render/PdfArgs`): `#page=3`, `2,5,9`,
      `3-7`, mixed, `all`, `#height=`, `layout=column|slides|grid`,
      `columns=`; no arguments is the whole document as slides.
- [x] `.tex`: maths alone through MicroTeX; a document compiled by pdflatex
      off the UI thread, cached by content, `-no-shell-escape`, rlimits, and
      in a bubblewrap sandbox that sees only the system and its own
      directory (4038601).
- [x] An embedded note is drawn formatted: its slice through a reading-view
      editor into an image a turn after layout (445bf8d).
- [x] An embed inside running text trails its line as a row. Now inline:
      the embed's "!" stays as an invisible carrier whose letter spacing
      reserves the cell's width (the inline-maths trick), and a line holding
      a taller cell grows to fit it. Several on a line sit side by side at
      their own size and wrap like words; the caret inside one shows its
      source.
- [x] A canvas embed is a picture, not interactive: each embed now keeps its
      own hidden, persistent CanvasView (Ctrl+wheel zooms about the pointer,
      a drag pans, a card click opens it, empty-space double-click opens the
      `.canvas` file); plain wheel is declined so the note still scrolls.
      Re-render measured at 0.58 ms on a 50-card canvas.
- [x] `![[x.base#View]]` now renders the named view: the embed's `#fragment`
      is threaded through as a view name (MarkdownEditor's m_baseRunner,
      ensureBaseSearch/deliverBaseResults keyed on yamlText+view,
      evaluateBaseForEditor's new viewName param); an unmatched name falls
      back to the first view, and a ```base fence (no fragment) is unaffected.
- [x] PDF placeholders size every page on the UI thread once per file:
      measured 2.5 ms for 500 pages (poppler 26.08), so it stays.

## Requested 2026-09-23, afternoon: history and switches

Jernej: "make it have built in git integration (for storing note hostories as
git diffs) if that is efficient enough (kind of like the editz files in
obsidian, which is a plugin)", and "Everything but the core features, should
be decoupled enough that they can be turned off and increase performance IF
the features existing decreases it".

- [x] Note history in git: every save a commit, stored outside the vault (no
      dotfile in it), per-note history browsable with diffs and restorable.
      0.093 ms a save; libgit2 in process, on one thread of its own.
- [x] Every non-core feature switchable off at run time (Obsidian's core
      plugins page, and the vault's `.obsidian/core-plugins.json` read),
      and when off it costs nothing: not built, not connected, not refreshed.
  - [x] `Settings::features()`: 18 switches, `plugins.<camelCase>`; both forms
        of `core-plugins.json` (the old array form cannot turn off bases,
        which came after it). On the Core plugins page, only what the build has.
  - [x] Side panes built and destroyed by `applyFeatures()` - search,
        backlinks, tags, outgoing links, outline, bookmarks, local graph -
        their tabs kept in order; unlinked mentions skip their search.
  - [x] Graph view, page preview (now built at the first hover), word count,
        file recovery snapshots and pruning, canvas and base files, note
        history. A feature's commands are disabled while it is off.
  - [x] Measured, 2 000 notes: cold start 81 ms on, 53 ms off. Note open was
        10x slower with the panes on at 1 MB; fixed (PERFORMANCE.md fix 17).
  - [x] Query blocks follow Search, base blocks and `.base` embeds Bases,
        canvas embeds Canvas; HTML blocks, PDF, LaTeX, audio and video embeds
        have switches of their own. Spellcheck already had one.
  - [x] Found by the measurement: the indexer wrote every note to SQLite on
        the UI thread. The scan now runs on its own thread and connection
        (PERFORMANCE.md fix 18): 20 000-note cold start 277 to 65 ms.

## Requested 2026-09-23, afternoon: a demo vault, and a last pass

Jernej: "create a small vault. Like max 5MB in total size, that should serve
to test all features. This does not have to be in TMP, and should be called
demo. You can actually built in now if you want. But for more extreme testing
work in tmp." And: "after ALL is done, see if you can topimize anything".

- [x] `demo/` in the project: a vault under 5 MB with a note (or file) for
      every feature - rendering, links and embeds of every kind, PDFs with
      page arguments, `.tex`, canvases, bases, `query` and `base` blocks,
      HTML, callouts, code styles, maths, media, daily notes, templates,
      bookmarks, `.obsidian` config (hotkeys, graph, workspace), and later
      notebook blocks. Grown as each feature lands; big and extreme vaults
      stay generated under /tmp.
- [x] Once everything else is done: a profiling and optimisation pass over
      the whole app, measured before and after (docs/PERFORMANCE.md). 2026-10-16
      pass: fixes 22-29 (tags 9.4 s -> 37 ms, table Tab 330 -> 2 ms, notebook
      open 360 -> 38 ms, resize/fold 1.3 -> 0.3 ms).
  - [x] Tags rebuilds while hidden (37 -> 0 ms), first window 41.5 -> 36.3 ms,
        `allNotesForBases` 65 -> 33 ms (perf2, 2026-10-16).
  - [x] Left: `Indexer::rewatch` walks the dirs on the UI thread (~6.7 ms per
        window); the scan should hand its directory list over (docs/PERFORMANCE.md).
        Done: first window 36.3 -> 28 ms; new folders now watched too.
  - [x] Note open and memory: `rebuildBlocks` built 1.6 kB views into an
        unreserved vector (1 MB open 38.6 -> 11.9 ms, RSS 262 -> 233 MB).
  - [x] Qt warnings in the bench: empty-path read in the outgoing-links
        pane, zero font sizes from default themes in PDF/EPUB/slides views.
  - [x] `noteAppC-bench --phase NAME` to profile one phase at a time.
  - [x] `Index::noteCount` in the open profile: the bench's own
        `waitForIndex` polling, untimed; the app emits `changed` once a scan.
  - [x] 50 kB open is 7.1 ms of md4qt parse (synchronous below 128 kB).
        Profiled: flat, md4qt internals (ParagraphParser::process 3.9%,
        ObsidianSyntax::scan 0.8%); no change worth its risk.
  - [x] Cold start, pdfview and search phases: no regression (cold start
        57 -> 44 ms; search, unlinked mentions, settings pass within noise;
        500-page PDF opens in 6.1 ms, scrolls at 0.79 ms a frame).
  - [x] 2026-10-01, quiet machine: 0.95 s. No regression; the readings below were load.
        Cold index, 20 000 notes, read 1.24 s against a documented 0.98 s
        with another build running: re-measure on a quiet machine
        (`noteAppC-bench --notes 20000 --phase start`) before chasing it.
        2026-09-25 evening: 1.30 s, but a game held 7 of 24 cores; still
        not a fair reading. Compare against 8fe9ee9^ side by side when idle.

## Requested 2026-09-23, afternoon: notebooks, last of all

Jernej: "when everything is said and done, implement the juypter notebook like
features that I are fouind in the ~/Projects/ideas.md spec" (the Notes app
section, "Code execution (Jupyter-style, tiered sandbox)"), and "Work
atuonomously"; "If there are blockers due to stuff not being insatlled, just
work on work that isn't blocked".

What the spec asks for:
- [x] Run a code block through a **Jupyter kernel**: a client of the
      messaging protocol (ZeroMQ, five sockets), so any installed kernel is a
      language. The frontend stays language-agnostic. (libs/ext/notebook)
- [x] **Per-block options as `#|` lines inside the fence** (Quarto's
      hash-pipe): valid comments in the block's language, so the `.md` stays
      clean CommonMark.
- [x] **Output cached in a sidecar**, never in the `.md` (Quarto's freeze
      model): shown under the block, kept across launches.
- [x] **Runnable code-file embeds**: `![[file.py]]` runs the file and shows its
      cached output below.
- [ ] **Tiered sandbox**, chosen per block by trust: tier 0 host-direct
      (opt-in), tier 1 bubblewrap/nsjail with seccomp and Landlock (the
      desktop default); gVisor, Firecracker and WASM tiers later (server and
      mobile).
      Built: tier 0 (`#| sandbox: host`, honoured only when Settings allows)
      and tier 1 bubblewrap, now with a seccomp denylist (libseccomp,
      optional at build time; bwrap `--seccomp`): no new namespaces, mounts,
      ptrace, keyring, bpf, io_uring, module loading or TIOCSTI. Landlock
      left out on purpose: the mount and network namespaces already cover
      what it would restrict. The same filter now covers the plugin host
      and TeX too (libs/sandbox). Left: the server/mobile tiers, blocked:
      no wasmtime, wasmer, runsc or firecracker on starbase-1.
- [x] **Without bubblewrap, everything that runs in a sandbox runs as the
      user, silently**: plugins (PluginHost::sandboxArguments), TeX
      (TexCompiler) and tier 1 code blocks (wrapCommand) all fell back to
      the host. Now each refuses and says to install bubblewrap; no setting,
      since a block that wants the host has tier 0 (which Settings must
      allow). The Arch package depends on bubblewrap.
- [x] Check what is installed before building on it (libzmq, a kernel such as
      ipykernel, bubblewrap); leave anything missing for Jernej.

## Requested 2026-09-23: the rest of Obsidian

Jernej, on the list of what is not built yet: "Command pallete is ok fo rlater,
the rest should be done". So everything below, the command palette deferred.
Built in waves of parallel lanes, each in its own `/tmp` worktree, builds under
`/tmp` with capped jobs (starbase-1 froze once and swappiness is still 100).

### Wave 1
- [x] **Search** (lane A): Obsidian's search grammar - words, "phrases", OR,
      -negation, ( ), path: file: tag: line: block: section: task:
      task-todo: task-done: content: match-case: ignore-case:, /regex/ - over
      the FTS5 table; a search pane (first sidebar tab, `global-search:open`,
      Ctrl+Shift+F) off the UI thread; a tag or a bookmarked search opens it;
      a result opens its note at the match. 20 000 notes: a common word
      6.4 ms, a rare one 0.05 ms. `[property:value]` landed (Search.cpp PropertyNode); `block:`
      is paragraph-level, not md4qt's blocks; `/regex/` is PCRE2, not JS.
- [x] **Unlinked mentions** in the backlinks pane (lane A), 0.3 ms per note.
- [x] Search snippet for a frontmatter tag highlighted the whole frontmatter;
      now the tag's own word.
- [x] **Outline** pane: the note's headings as a tree, the caret's in bold, click to jump.
- [x] **Bookmarks**: notes, headings, searches, groups; reads
      `.obsidian/bookmarks.json`, keeps its own copy outside the vault.
- [x] **Daily notes**: today's note from folder, format and template,
      `.obsidian/daily-notes.json` honoured.
- [x] **Templates**: insert from the templates folder, `{{title}}`
      `{{date}}` `{{time}}` and `{{date:FORMAT}}`.
- [x] **Graph view** (lane C), a Tier 1 module like spellcheck: global
      (`graph:open`, Ctrl+G, a main-area page built on first use) and local
      (a sidebar tab following the note), Barnes-Hut layout with d3-force's
      laws, 5 000 nodes: frame 9.5 ms, step 5.5 ms; `graph.json` options;
      hidden views do not re-read the index. The lane first found the 55 GB
      crash: Euler integration diverging to inf, and a quadtree that never
      stops halving inf.
- [x] Graph colour groups are search queries now (OR, negation, groups,
      path:, file:, tag:, regex), matched once per rebuild, not per frame.
- [x] A build with every optional module OFF, to prove the `#ifdef`s: it
      found one (the local graph tab); `tools/check.sh` runs it every time.
- [x] **Hotkeys**: every window command is an action named by its Obsidian
      id, the editor's shortcuts are a table of commands (`runCommand`,
      `setKeyBindings`), keys come from noteAppC's `hotkeys.json` over the
      vault's Obsidian one over the built-in ones, and Settings > Hotkeys
      rebinds, shows clashes and resets. The palette sits on this later.

### Wave 2
- [x] File recovery: a snapshot on save at most every 5 minutes per note,
      kept 7 days (Obsidian's defaults, both settings), under
      `$XDG_DATA_HOME/noteAppC/recovery`, vault notes only; a window to find
      a note, read a version, copy it or restore it, and "Show changes"
      against the note as it is now (a line diff).
- [x] Page preview: the linked note rendered in a popover, opened at the
      heading or block the link names; Ctrl held while editing, plain hover
      in reading view, as Obsidian. Links inside it can be followed.
      Images and PDF pages (first, or `#page=N`) preview as pictures.
- [x] Audio and video embeds: a player card (play/pause, seek bar, clock;
      video frames painted into the block), Qt Multimedia optional behind
      `NOTEAPPC_WITH_MEDIA` - without it a click opens the file. No poster
      frame before play yet.
- [x] `query` blocks, on the wave 1 search (lane F): results, match counts
      and highlighted snippets in the note, off the UI thread, cached per
      query, refreshed on index changes; a result opens its note at the
      match. 0.10 ms per unrelated keystroke with one on screen.
- [x] Obsidian frames an embedded search with a header ("N results",
      collapse, sort): there, checked on the demo vault 2026-09-23.
- [x] PDF export of the reading view (`workspace:export-pdf`): Obsidian's
      options - file name as title, A3/A4/A5/Legal/Letter/Tabloid,
      orientation, margin presets, downscale - light theme on white paper,
      pages broken between blocks.

### Wave 3
- [x] Tabs and splits: each tab its own editor and history; new tab, close,
      close others, next/previous, go to tab 1-8/last, split right/down,
      nested; a split closes with its last tab; deleting a note closes its
      tabs; the same note in two splits follows a save.
- [x] Workspaces: the layout reopens with the vault (Obsidian's
      workspace.json read until noteAppC saves its own, never written);
      background tabs load when first shown; named layouts - save, load,
      delete, save-and-load.
- [x] Canvas (JSON Canvas, lane D): model that keeps unknown keys and the
      exact bytes when untouched, pan/zoom, groups, edges, Markdown and file
      nodes rendered by the editor, editing with undo; opens in a tab, framed.
      1 000 nodes pan in 4-5 ms.
- [x] Bases (lane E): `.base` filters, formulas, table and card views, sort,
      group, limit, an expression language in C++; editing a cell writes
      only that property. Opens in a tab. 20 000 rows filter+sort in 71 ms.
- [x] HTML blocks through litehtml (lane F), behind `NOTEAPPC_WITH_HTML`:
      rendered once per block into a cached image in the theme's colours;
      scripts, `on*`, iframes, objects and embeds never run or load; remote
      images are not fetched. Inline `b i u s kbd code mark sup sub` and
      `span style="color:"` map onto text formats. A container of our own:
      Qt Creator's qlitehtml is GPL-3, so it was read, not vendored.
- [x] HTML left out: gradients, rounded corners, per-side borders and list
      numerals now drawn. Left: radii with mismatched sides; double, groove,
      ridge, inset, outset borders draw solid. 2026-10-16: all drawn (render lane).
- [x] `canvas:new-file` ("Create new canvas"): an empty `{}` Untitled.canvas.
- [x] A new-base command: `bases:new-file`, "Bases: Create new base" (2879964).
- [x] ` ```base ` blocks inside notes, on `BaseView`.
- [x] Bases `summaries:` - every built-in (Average ... Unique), a footer row in
      the base view and in a base block.
- [x] Bases list functions: `filter`/`map`/`reduce` with `value`, `index`,
      `acc`; list literals; `flat`; lists of any values; a file's own
      summary formulas over `values` (`values.mean().round(3)`).
- [x] Bases left out: link/file/image values, list-property cells, cards
      read-only, sort not saved back - all done in 124974e and 725fa93.
- [x] Canvas controls as Obsidian's: a drag on empty canvas draws a
      selection box, the wheel pans (Shift sideways), Ctrl+wheel zooms,
      Space- or middle-drag pans; a group carries the cards inside it.
- [x] A file card's subpath shows that section (`#Heading`, `#^block`), as Obsidian.
- [x] Middle-click a tree row to open it in a new tab.
- [x] **Decided (Jernej 2026-09-23, "Follow in place"):** Obsidian opens a link in a new tab on Ctrl+click, but
      in noteAppC's editing view Ctrl+click is how a link is followed at all
      (a plain click places the caret). Which should Ctrl+click be?
- [x] Only the focused split's tab is accented, as in Obsidian.
- [x] One note in several tabs is one document: every edit (and undo) is
      made in the others, a new tab adopts the unsaved text, one save is
      every view's, closing one view asks nothing.

### Wave 5: Obsidian's other core plugins (found missing 2026-09-23)
- [x] Random note (`random-note`): open a random note.
- [x] Unique note creator (`zk-prefixer`): a note named by a timestamp.
- [x] Properties view (`properties`): every property in the vault, with counts;
      search gained `[property]` / `[property:value]` for its clicks.
- [x] Slides (`slides`): present a note, slides split at `---`.
- [x] Note composer (`note-composer`): merge a note into another; extract the
      selection into a new note, leaving a link or an embed.
- [x] Audio recorder (`audio-recorder`): record into an attachment, embed it.
- [x] Slash commands (`slash-command`): "/" in the editor lists commands; off by
      default, as Obsidian ships it.
- [x] Footnotes view: a pane of the note's footnotes; the editor draws footnotes
      as numbers and reading view lists them at the end.
- Not built: Web viewer (it needs a web engine, and C/C++-only rules that
  out), Format converter and Markdown importer (one-off migration tools).

### For Jernej to decide
- [x] **A licence.** GPL-3.0-or-later (Jernej 2026-10-16): LICENSE, PKGBUILD,
      AppStream, README. Was: none chosen; the PKGBUILD said `LicenseRef-unknown`
      and the AppStream file states none. Bundled: md4qt and MicroTeX (MIT),
      tinyxml2 (zlib), Lucide icons (ISC); poppler (GPL) is linked when on.
- [ ] **An app id** (and maybe a new name: Jernej 2026-10-16 asked for name suggestions) for Flatpak, AppStream and Android:
      `io.github.jernejmarcic.noteAppC`, following basket's, unconfirmed.
- [x] **An app icon.** Hand-written SVG, libs/theme/icons/noteAppC.svg (Jernej 2026-10-16: "Draw an SVG"). Was: The desktop file borrows `accessories-text-editor`;
      drawing one is image making, which is off unless you say otherwise.
- [x] **Autosave.** Jernej 2026-09-23: "Autosave like Obsidian". A note or
      canvas saves 2 s after the last edit and whenever it is left or closed;
      nothing asks. `files.autosave` (on) keeps the old prompts for anyone
      who turns it off.

### Wave 4
- [x] `PKGBUILD` without `-march` (`packaging/arch`): built here into /tmp,
      its `check()` passes all suites under makepkg's flags. AppStream
      metainfo installed and validated.
- [-] AppImage and Flatpak recipes. Deferred: Jernej 2026-10-16 "no flatpak/appimage for now". Neither can be built here without
      installing tools (flatpak-builder, linuxdeploy), which is yours to do.
- [x] A local check script in place of CI (there is no remote):
      `sh tools/check.sh [--bench]` - the release build and all suites (perf
      guards and the new seeded round-trip fuzz among them), then a build
      with every optional module off and its suites.
- [x] The Android build (lane G): an 18 MB debug arm64 APK, package
      `io.github.jernejmarcic.noteAppC`, minSdk 26, built from the 6.7.3 kit;
      spellcheck, PDF, highlighting, HTML and media off there (see
      docs/PLATFORMS.md). Not run on a device; a vault on a phone needs a
      Storage Access Framework folder picker, not built.
- [x] Touch mode, first part: momentum scrolling where a touchscreen
      exists (a mouse drag still selects), long-press selects a word, both
      sidebars fold below 720 px and come back.
- [x] Touch mode, rest: selection handles after a long press, finger-sized
      hit targets while a touch press is handled, sidebars as drawers over
      the note below 720 px. Not tried on a real touchscreen yet.


## Requested 2026-10-01
Jernej: "Give it some ollama and A.I integrations using the OpenAI API. Such
as translating text via LLM", "Add VIM keybindings", "Make sure the multiple
editors works, and all metadata is properly saved and stuff, make sure the
encryption works", "all kinds of things that should be rendered are rendered
inline", "For code add the code running (the jupyter features) all features
from ~/Projects/Ideas.md", then "make sure everything is as optimized and as
optimal as it can be".
- [x] AI: `translate` (selection, or the whole note into a new note),
      `proofread`; ai.ini `translate-to`, `translate-from`, `translate-model`;
      TranslateGemma's own prompt when the model is translategemma; an
      OpenAI-compatible server with no `api-key-env` (Ollama's /v1) sends no key.
- [x] Vim keybindings in the editor (off by default, `editor.vimMode`; docs/EDITOR.md).
      2026-10-01 later: text objects, registers, marks, macros, block visual,
      `%`, `>` `<`, `:s`, `*` `#`. Ctrl+v in normal mode is block visual now.
      2026-10-16: `"1`-`"9`, `it`/`at`, visual `J r ~ u U`, `N%`, macro stop on
      a failed motion. Left: `l`/`h` at a line edge do not fail a macro.
- [x] Notebook gaps against Ideas.md (docs/NOTEBOOKS.md). Completion and
      inspect built 2026-10-01. 2026-10-16: `.ipynb` and `input()` done. Left: tiers 2-3.
- [x] Audit (tests in test_window/test_sync/test_vault): split panes, metadata
      byte-exact through every save path, encryption leaves no plaintext on
      disk, every embed kind renders inline. Fixed: outside changes to an open
      note were overwritten by the next autosave; restore updated one pane
      only; a plugin's note.activeText leaked an encrypted note's plaintext.
      Left: Mermaid (done 2026-10-02, mmdr); EPUB
      embed is a card (no cover API); every atomic save resets birthTime, so
      Bases' file.ctime is the last save (writing in place is the cure, at the
      cost of a torn file on a crash - yours to decide); a note renamed or
      moved outside the app is not followed; a note deleted then recreated
      outside is not re-watched until its next open.
- [x] Last: optimisation pass. One quiet 20 000-note bench after all merges:
      cold start 32 ms, cold index 0.95 s, 1 MB open 11.9 ms, keystroke
      0.2-0.5 ms (code block 1.2 ms), scroll frame 0.45 ms, RSS 227 MB - all
      far inside budget. Keystroke medians drifted up a little against stage
      14 (200 kB 0.22 -> 0.25 ms, code block 1.10 -> 1.19 ms), not chased. Bases'
      62 ms note read is already on the pool.

## Requested 2026-10-02
- [x] HTML in live preview edited where it is drawn (docs/EDITOR.md "HTML edited in place"; 50-row HTML table keystroke 1.33 ms). Jernej: "For HTML I want
      rendering of it like in obsidian, but unlike obsidian I want it inline
      editable just like the markdown"; "if its a HTML table it should be
      editable like an actual table element (like in excel for example,
      clicking on cells, etc.)". Tags hide like Markdown markers and show
      under the caret; an HTML table is edited cell by cell like a Markdown
      one; what the editor cannot lay out itself stays litehtml.
      2026-10-02 later: HTML headings in outline/index/links, cell style= kept
      editable (alignment drawn), vim steps over entities, Markdown entities
      shown decoded. 2026-10-16: cell colour, background, bold drawn. Entities open into source under the caret, HTML headings are embed
      and canvas targets (2026-10-16); `![[Note#Heading]]` and the canvas section
      picker see Markdown headings only; entity decoder uses md4qt's private
      MD::s_entityMap (check on md4qt bumps).
- [x] Mermaid. Jernej: "add mermaid as toggleable, or make it load after the
      app, remember I want speed". `render.mermaid` (on), through mmdr on the
      pool, cached on disk and in memory, nothing at startup (docs/EDITOR.md).
      mmdr 0.3.1 installed (~/.local/share/cargo/bin, 2026-10-02). 2026-10-16
      (lane mermaid2): `-c` colours honoured, run as `mmdr -i - -c cfg`, tool found
      in cargo dirs, publish/PDF inline the cached SVG. Left: a light published
      site shows the dark-first cached diagram (RenderEnv has no light mode). Disk cache pruned (30 days / 50 MB) on first use.

## Requested 2026-10-16 (from the running app)
- [x] Caret ran far taller than its line ("Cursor blinker really big"): now the
      line's own height; the blink repaints only the caret.
- [x] Inline title: "Cant click on title to change it like in Obsidian" - click
      the title over the note, edit, Enter/focus-out renames the file (links
      updated like any rename), Esc cancels.
- [x] Lists: Enter after a list item starts the next item ("Pressing return
      after a counter ... does not spawn another like it should"); numbers
      continue ("Numbers should continue logically"); Enter on an empty item
      ends the list; Tab indents and restarts numbering.
- [x] Bullet glyph per marker: "I also want different ones (-,*,=) to render
      differently ... be creative with that one, setting should have a way to
      map these". (`=` is no CommonMark marker; `-` `*` `+` are.)
- [x] Nested ordered numbers shown hierarchically, "first number.second
      number so this would be 1.2."; toggleable. The number's terminator shown
      as `.` or `:` "or literally anything", a setting. Display only: the file
      keeps CommonMark `1.`.
- [x] MarkdownEditorTest.OpeningAMegabyteNoteIsImmediate fails under load 16
      (ratio 8.6 vs limit 5.0), passes alone: make it load-robust (best of N,
      or compare against a same-run baseline) so lane-checks stay meaningful.
      Now best of three per size.
- [x] ASan leftovers (bughunt 2026-10-16): plugin host exits under ASan,
      probably its 256 MiB RLIMIT_AS (skip the limit when built with
      -fsanitize=address); test_ai_plugin.cpp:756 callback captures a dead
      local when the first run() fails (stack-use-after-return). Timing guards
      fail under ASan by nature (skip them when sanitized).
- [x] Demo walk: opening a .base or .canvas leaves a second "Library" pane in
      later views; check whether that is wanted workspace state. 2026-10-16:
      wanted, demo/.obsidian/workspace.json puts Library.base in a right split.
- [x] Tables leftovers: (`/table` now offers Insert table first, 2026-10-16); source-mode Tab/Enter
      within 20 ms of typing does a synchronous whole-note reparse (hitch on a
      note above the async-parse threshold); row/column drag handles, hover
      add-row/column buttons, rectangular cell selection.
- [x] Test binaries run by hand (not via ctest) wrote into the real
      ~/.local/share/noteAppC/history: 3558 repos there, 24 from tonight.
      tests/IsolatedHome.cpp now sets the XDG homes before main in every test
      binary; test_lists moved above the ctest isolation block. Cleaning the
      old repos is Jernej's call (his real vault's repo is in the same dir).
- [x] History creates a git repo the moment a vault opens, even with nothing
      to commit; create it on the first snapshot instead.
- [x] Perf-budget tests fail under load 16+ (canvas pan, bases 20k, epub,
      ListPerf, kernel completion): best-of-N like the 1 MB open test, so
      parallel lane-checks stay meaningful.
- [x] Complex code leftovers: Publisher builds themes with Theme::builtin(), so
      a published site gets no theme `syntax`/`--code-*` token colours yet; a
      listing nested in a callout gets ini/theme colours only (no fence attrs).
- [x] Notebook leftovers: no window-level test drives input() end to end;
      the plugin askUser dialog is untested; `.ipynb` outputs are seeded on
      the GUI thread at every read (big PNGs cost time); run outputs are not
      written back into the .ipynb; the vault index ignores .ipynb.
- [x] Notebook stdin race (found merging 2026-10-16): input_request dropped
      before the stdin handshake; KernelClient now waits for it.
- [x] Tables later: drag a bar to add several rows/columns, autoscroll a wide
      table while dragging a column, touch, handles on HTML tables. Done
      2026-10-16 (simple HTML grids only). Paste into an HTML cell rectangle
      and clearing a cell's inner spaces done 2026-10-16 too.
- [x] Image annotation is off by default (it replaces the plain image tab).
      Jernej 2026-10-16: "Keep off".
- [x] Lists leftovers: if Enter still fails on a bare list in the running app,
      find what owns Return (popup, hotkey); offscreen it continues. Per-level
      glyph variation for nested bullets. Fence check reads the debounced
      decorations (a fence typed within the parse delay is not seen yet).
- [x] Refine the look ("Make look prettier" on a popup menu; "the general
      design is fine, I just want refinements"): menus, popups, small polish.
- [x] Tables: "Tables should be autogenerated, and autoaligned so they look like
      the aligned ascii art when in source mode". Typing a header row then
      Enter makes the separator and the next row (Advanced Tables style);
      source mode pads the columns so the pipes line up.
- [x] Status bar like Obsidian's ("the bottom thing should be like in
      obsidian"): backlinks, properties, mode icon, words, characters; the
      mode icon opens Reading / Source mode / Live Preview with a check.
- [x] Overlapping blocks: two `:::` callouts in a row overlap ("Fix overlapping
      elements and such"); a check over every demo note that no block rects
      intersect (bughunt lane).
- [x] Hover over a rendered element (callout, embed, table, rendered block)
      shows the `</>` edit icon, as Obsidian does: "when hovering over rendered
      elements the code icon should appear to edit the code".
- [x] Code blocks, optional "complex mode" beside the default: "It should
      allow for tabs, icons, custom code color, custom bg color, custom tree
      sitter color, etc. It should work like the super custom callouts kind of,
      at least follow the same philosophy". tree-sitter 0.26 is installed with
      c, lua, markdown, query, vim grammars only.
- [x] Study how Obsidian and Zed work with REA, to make the editor better
      (Jernej's suggestion); findings feed the lanes, behaviour only, no code
      copied.
- [x] Code block tab click relays out the whole note (`selectCodeTab` calls
      `invalidateLayouts()`); invalidate only the tab group (complex-code lane).
- [x] Code hover repaints the whole viewport (`m_hoveredCodeBlock` path); repaint
      only the old and new block rects (hover-edit lane).
- [x] Obsidian `--code-*` and Zed-style `syntax.*` keys as one theme contract, so
      imported Obsidian themes colour code (complex-code lane).
- [x] Tables like Obsidian: insert-table command and slash entry, alignment
      submenu, duplicate row/column, sort by column (tables lane). Later: row and
      column drag handles, hover add-row/column buttons, rectangular cell
      selection.
- [x] Status bar word/character count: 200 ms debounce, counts the selection
      when there is one, skips frontmatter. Today: counts come with each parse,
      characters are the whole file incl. frontmatter.
- [x] Popup leftovers: mode-menu check is a text mark in the shortcut column
      (QSS cannot put the indicator right of an icon); QMessageBox/QInputDialog
      windows are not rounded.
- [x] Callout context menu: Edit / Copy / Change type / Remove, once the hover
      edit "Edit block source" item exists.
- [x] `DisplayMap::toDisplay` is linear (four loops); binary search like
      `toSource`. Small gain; whoever touches DisplayMap next.
- [x] Audit the 19 `invalidateLayouts()` callers; narrow each to the blocks it
      changed, if bench resize/fold rows are over 10 ms.

## Fable audit 2026-10-16 evening

- [x] Quick switcher: Enter with no match creates the note, Shift+Enter
      always creates (Obsidian); palette and merge picker keep no create path
      (lane switcher).
- [x] MomentFormat: moment.js tokens `h hh H m s a` (`{{time:h:mm a}}`
      rendered literally) (lane moment).
- [x] Frontmatter: a string `tags:` splits on commas and spaces, a string
      `aliases:` on commas, as Obsidian reads them; the property text stays
      (lane moment).
- [x] `.markdown` files were half notes (isNote yes, links/templates no);
      Obsidian treats only `.md` as notes, so they become attachments
      (lane moment).
- [x] Search `tag:/regex/` ignores case like every other field unless
      `match-case:` (lane search).
- [x] Test TableFormat's East-Asian/combining/emoji display width (lane
      search).
- [x] test_bases segfaulted ~1 in 3 (found by the search lane): pool tasks
      held a raw Index* that could die or reopen under them. Workers now
      take a copy of the cache path (Index::notesForBases / searchIn).
- [x] Obsidian's "Default location for new notes" (`newFileLocation`,
      `newFileFolderPath`) read and in Settings > Files; New note, Ctrl+N
      and the quick switcher's create follow it (left by the switcher lane).

## Fable audit round 2, 2026-10-16

- [x] Rename while a linking note is open and modified: the rewritten links
      are lost to the editor's stale text on the next autosave. Save open
      notes before a rename (lane rename).
- [x] Folder rename rewrites no path-form links (`[[Projects/Plan]]`,
      `[x](Projects/Plan.md)`); Obsidian does (lane rename).
- [x] A failed quiet autosave is never retried, and closing then silently
      refuses; retry on the next pause, fall back to the Save/Discard box
      (lane autosave).
- [x] CanvasRender spins a nested QEventLoop inside paintEvent for a big
      text node (re-entrancy, up to 500 ms on the UI thread); finish the
      parse synchronously instead; same in PdfExport; the canvas-embed
      singleShot holds a raw MarkdownEditor* (lane canvasparse).
- [x] SyncJournal::writeBase truncates in place (torn base -> conflict copy
      instead of a merge); QSaveFile (lane sync).
- [x] Licence: GPL-3.0-or-later (Jernej 2026-10-16). LICENSE, PKGBUILD,
      AppStream, README.
- [x] Delete the leaked test history repos (Jernej 2026-10-16: only those
      whose vault no longer exists). Done: 3555 moved to
      /tmp/noteAppC-history-leaked (gone at reboot); kept Lancaster, demo and
      one 2026-09-23 repo holding Sensors.md.

## Requested by Jernej 2026-10-16 night

- [ ] "if you select a word there should be a right click menau, that can
      give you stuff like definition, etc." Word/selection context menu:
      definition, plus Obsidian-style selection actions (plan: Fable, PLAN-word-graph Plan 1).
- [ ] "There should also be a file graph like obsidian". A graph view exists
      (Ctrl+G, local graph pane) but nothing on screen opens it, and its look
      against Obsidian's is unchecked (plan: Fable).
- [x] Inline HTML formatting (`H<sub>2</sub>O`, `<mark>`, `<u>`, `<kbd>`,
      coloured `<span>`) reveals its tags while the caret is inside the
      element, like markdown syntax does in live preview (Jernej 2026-10-16,
      screenshot of the inline formatting demo).
- [x] `<script>` (and every other script-bearing thing) never runs, and a
      script element shows as source in red and bold, a warning (Jernej
      2026-10-16: "javascript should never be ran"; only HTML and CSS
      formatting is followed). Audit: no JS engine linked, litehtml executes
      nothing, on* attributes and javascript: URLs inert.
- [ ] Nested `-` list items drawn with `↳` (U+21B3), per Jernej's sketch
      2026-10-16 ("nested - rows should look like this").
- [x] Bug: "Add to dictionary" on a misspelt word resets the view to the top
      of the element (Jernej 2026-10-16). Relayout keeps heights, every
      editor refreshes, ensureCursorVisible lays the caret block out first;
      test fails on the old code.
- [ ] Callout title editable in place: clicking the rendered title edits the
      title text while the header stays drawn and the `> [!type|...]` marker
      stays hidden; today any click reveals the whole marker line (Jernej
      2026-10-16: "it would make more sense of the title to be editable when
      you click on it").
- [ ] Code block that cannot run (`json`, any language with no runner): no
      run button, no warning icon, no "does not know how to run" line
      (Jernej 2026-10-16: "That that should be run should not have a run
      object").
- [ ] Run output keeps its line breaks: a rustc error printed as one run-on
      line ("Exit code 1error[E0601]...") (Jernej's screenshot 2026-10-16).
- [ ] Code block header shows the block's name (its `tab`/title, else the
      language) as the bar's title, not a faint language tag on the right
      (Jernej 2026-10-16: "the top bar should be what the name of the
      codeblock is").
- [ ] Tabbed code groups (```` ```python tab ````, `tab="C++"`): better tab
      UI, and editing a group shows its source (fences visible, syntax
      highlighting kept) instead of the half-rendered panels (Jernej
      2026-10-16).
- [ ] Source mode is plain text: no panels, headers, run buttons or any
      rendering, only syntax highlighting to keep things distinct (Jernej
      2026-10-16: "source mode should show pure text, no rendering").
- [ ] Demo pane test read the live demo/.obsidian/workspace.json, which the
      app rewrites when Jernej uses the demo; now runs on a copy with
      fixtures/demo-workspace.json. [x] done 5e6734f.
