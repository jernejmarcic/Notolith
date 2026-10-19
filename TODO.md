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
      and TeX too (libs/sandbox). Left: gVisor and Firecracker (not
      installed, not approved); the WASM tier below (done).
      Blocked on Jernej, checked 2026-10-17: Firecracker 1.17.0 is in the
      Arch repos (`sudo pacman -S firecracker`), /dev/kvm is there; it also
      needs a guest kernel and rootfs image, which are downloads. gVisor
      (runsc) is not in the repos (AUR or Google's release binary only).
      Neither was approved; only wasmtime was.
      **WASM tier** (Jernej 2026-10-17 installed wasmtime 49.0.2: CLI only,
      no C API, so a `wasmtime run` subprocess). Plan (lane wasm):
      - [x] `#| sandbox: wasm` parsed (Parser, Decorations `sandboxWasm`)
            and carried through editor, cache key and badge ("wasm").
      - [x] `SandboxTier::Wasm = 2`: `wasmtime run -C cache=n --dir
            <cell>::/cell -W max-memory-size=1GiB module args`; nested in
            bwrap + seccomp; refused without bwrap, as tier 1 is (audit 5
            #1; it first ran wasmtime alone). Refused with a clear
            message when wasmtime is missing. wrapCommand treats any tier
            but Host as bubblewrap, so a kernel is never run as the user in
            this tier (and the controller never gives a wasm block one).
      - [x] `wat`/`wasm` blocks run themselves as the module, in any tier.
      - [x] Other languages in the wasm tier: the module from setting
            `notebooks.wasmModules` (`python=/path/python.wasm;...`, also a
            Settings row), its file at /cell/<cell file>; no module, refused,
            saying which. A runnable embed's folder is not granted.
      - [x] Tests: a WAT fixture printing to stdout via fd_write runs in
            tier 1 and the wasm tier; the module map exercised with a WAT
            fixture standing in for an interpreter (prints its argv);
            missing module and missing wasmtime refuse; parser, editor and
            cache-key cases.
            The no-bubblewrap case too (PATH with wasmtime only: both tiers
            refuse; seen failing with the old run-it-alone branch).
      - [x] Docs (docs/NOTEBOOKS.md); check.sh green. Honest ceiling: no
            real interpreter module (python.wasm) has run through it - none
            is installed; only WAT fixtures have. The no-bubblewrap path
            (wasmtime alone) is written but was not run: bwrap is installed
            here.
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
- [x] **An app id** (and maybe a new name: Jernej 2026-10-16 asked for name suggestions) for Flatpak, AppStream and Android:
      2026-10-17 Jernej floated "Skorja? Biblolith? Notolith?"; a web search
      found no app or software under any of the three. He picked Notolith.
      Plan (lane rename):
      - [x] Product name "Notolith" wherever a person reads it: window title,
            About page, error and settings texts naming the app, PDF creator.
      - [x] App id `io.github.jernejmarcic.Notolith`: desktop file named after
            it (Flatpak needs that), Wayland app_id via setDesktopFileName,
            icon installed under that name, metainfo, Flatpak manifest,
            Android package and label.
      - [x] Binary `notolith` (OUTPUT_NAME; the CMake target stays noteAppC),
            Arch package `notolith`, AppImage script.
      - [x] Kept as noteAppC on purpose, so nothing is lost or migrated:
            ~/.config, ~/.cache and ~/.local/share directories, the keyring
            service, command ids in hotkeys.json (`noteAppC:...`), the C++
            namespace and target names, the repo directory.
      - [x] README and packaging/README say Notolith (code name noteAppC);
            tests green; check.sh green.
      Done 2026-10-17 (lane/rename, 1b227de): `cmake --install` puts
      bin/notolith and share/{applications,icons/hicolor/scalable/apps,metainfo}/
      io.github.jernejmarcic.Notolith.*; desktop-file-validate clean,
      appstreamcli only warns about the missing homepage (as before).
      setApplicationDisplayName("Notolith") also makes Qt's xcb/wayland
      windows append " - Notolith" to other windows' titles (dialogs), the
      KDE convention; kept. Not checked: X11/XWayland WM_CLASS still comes
      from the application name noteAppC, so StartupWMClass may not group
      there (`xprop WM_CLASS` would tell; Hyprland uses the Wayland app_id).
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

- [x] "if you select a word there should be a right click menau, that can
      give you stuff like definition, etc." Word/selection context menu:
      definition, plus Obsidian-style selection actions. Done (lane wordmenu,
      6c9f09b): menu on every word, suggestions, add/remove dictionary,
      Format submenu, Look up via DICT (localhost, then dict.org), vault and
      web search; feature word-lookup.
- [x] "There should also be a file graph like obsidian". A graph view exists
      (Ctrl+G, local graph pane) but nothing on screen opens it, and its look
      against Obsidian's is unchecked. Done (lane graph, ad5d76b): header
      button, step/paint and index read off the UI thread (20k nodes: UI
      paint 0.2 ms, setIndex 0 ms blocking), positions kept across saves,
      Filters/Groups/Display/Forces card, per-vault graph.json outside the
      vault, label fade. docs/GRAPH.md.
- [x] Inline HTML formatting (`H<sub>2</sub>O`, `<mark>`, `<u>`, `<kbd>`,
      coloured `<span>`) reveals its tags while the caret is inside the
      element, like markdown syntax does in live preview (Jernej 2026-10-16,
      screenshot of the inline formatting demo).
- [x] `<script>` (and every other script-bearing thing) never runs, and a
      script element shows as source in red and bold, a warning (Jernej
      2026-10-16: "javascript should never be ran"; only HTML and CSS
      formatting is followed). Audit: no JS engine linked, litehtml executes
      nothing, on* attributes and javascript: URLs inert.
- [x] Nested `-` list items drawn with `↳` (U+21B3), per Jernej's sketch
      2026-10-16 ("nested - rows should look like this").
- [x] Bug: "Add to dictionary" on a misspelt word resets the view to the top
      of the element (Jernej 2026-10-16). Relayout keeps heights, every
      editor refreshes, ensureCursorVisible lays the caret block out first;
      test fails on the old code.
- [x] Callout title editable in place: clicking the rendered title edits the
      title text while the header stays drawn and the `> [!type|...]` marker
      stays hidden; today any click reveals the whole marker line (Jernej
      2026-10-16: "it would make more sense of the title to be editable when
      you click on it"). Done 273415f: overlay line edit over the title,
      one undo step; on a foldable callout the title edits, the arrow and
      empty header row fold.
- [x] Code block that cannot run (`json`, any language with no runner): no
      run button, no warning icon, no "does not know how to run" line
      (Jernej 2026-10-16: "That that should be run should not have a run
      object").
- [x] Run output keeps its line breaks: a rustc error printed as one run-on
      line ("Exit code 1error[E0601]...") (Jernej's screenshot 2026-10-16).
- [x] Code block header shows the block's name (its `tab`/title, else the
      language) as the bar's title, not a faint language tag on the right
      (Jernej 2026-10-16: "the top bar should be what the name of the
      codeblock is").
- [x] Tabbed code groups (```` ```python tab ````, `tab="C++"`): better tab
      UI, and editing a group shows its source (fences visible, syntax
      highlighting kept) instead of the half-rendered panels (Jernej
      2026-10-16).
- [x] Source mode is plain text: no panels, headers, run buttons or any
      rendering, only syntax highlighting to keep things distinct (Jernej
      2026-10-16: "source mode should show pure text, no rendering").
- [x] Demo pane test read the live demo/.obsidian/workspace.json, which the
      app rewrites when Jernej uses the demo; now runs on a copy with
      fixtures/demo-workspace.json. [x] done 5e6734f.
- [x] Graph labels: at the opening zoom only the current note is labelled,
      even in a 4-note vault; Obsidian labels every node at its default
      zoom. Tie the default fade threshold to the fitted zoom (found
      checking the graph merge, 2026-10-16).
  Code block items above done in lane codeblocks (aedca60).
- [x] A single fenced block being edited drew its panel and header over its
      opened fence lines (Jernej's first python screenshot): it is now plain
      source like a tab group being edited.

## Fable audit round 3, 2026-10-17

Full report: session scratchpad AUDIT3.md. Lanes G (graph), C (callout
title), L (look-up), K (edited listing chrome).

- [x] 1 HIGH: Reading view edits the note through the callout title overlay
      (pressCalloutTitle has no mode gate; check the property editor too). C
      Done; also gated fillShortRow (a short table row was filled in Reading).
- [x] 2 HIGH: GraphView::rebuild() waits for the worker job on the UI thread:
      47 ms (animate on) / 255 ms (off) at 20k nodes, on every autosave and
      local-graph note switch. Shared layout + generation, no wait. G
- [x] 3 MEDIUM: an external edit (same note in two tabs) while the title
      overlay is open drops the typed title or retitles another callout. C
- [x] 4 MEDIUM: caret in a single fenced listing drops panel, run control and
      last output; the block height jumps. Decision: keep the panel
      background and outputs, fences drawn inside the panel, no header strip;
      eligibility flip and output clearing stay tab-group-only. K
      Done (listing3): one panel spans both fence lines, run control on the
      opening fence line, outputs kept; height delta -31.5 px (header strip).
- [x] 5 MEDIUM: local graph lacks Incoming/Outgoing filters, and a comment
      wrongly says Obsidian has none. G
- [x] 6 LOW: colour-group query typing rescans every node per keystroke, no
      debounce. G
- [x] 7 LOW: graph drag test cannot fail (reads the UI snapshot only). G
- [x] 8 LOW: graph control change in the last 500 ms before a vault switch is
      lost (flush in openVault). G
- [x] 9 LOW: Look up from a keyboard-opened menu anchors the popover at the
      mouse, not the caret. L
- [x] 10 LOW: clicking into a callout title puts the caret at the end. C
- [x] 11 LOW: DictClient reply is unbounded; timeouts not settable so the
      silent-server path is untested; docs say a dead server costs 1.5 s. L
- [x] 12 LOW: colour dialog returns to an unguarded group index. G
- [x] 13 DOC (rebuild 215 -> 0.8 ms at 3k nodes; local in/out keys are own
      names localIncoming/localOutgoing, Obsidian's unconfirmed): HANDOFF "35 suites" (36); GraphView.h ~65 ms; MainWindow
      "reads the index only once shown"; graph page has no Escape exit, nodes
      no right-click menu (G adds Escape back to the note; node menu: Open,
      Open in new tab).

## Requested by Jernej 2026-10-17: meta notes

- [x] "I want [] to be purple or at least distinct like in Obsidian. I was
      mostly using them for meta notes in writing." Bare `[text]` in prose
      drawn in a link-colour token (purple) in every mode, as Obsidian does;
      not links, wikilinks, footnotes, tasks, callouts. Lane meta.
- [x] Toggleable, and the delimiters are the writer's choice ("another
      writer can select any character to behave like a meta note");
      default `[` `]`. Lane meta (scanner, settings, validation).
- [x] Meta pane + jump (Jernej's pick from the proposed workflow): right
      sidebar pane listing the note's meta notes, vault-wide toggle, a
      `meta:` search operator, Alt+] / Alt+[ next/previous. "Plan this
      feature out and think about how it may work" - Fable plans it after
      lane meta lands. Plan: scratchpad PLAN-meta-pane.md.
  - [x] A: Metadata::metaNotes, meta_notes table (schema 6),
        Indexer::setMetaNotes (forced re-parse, off = delete rows), `meta:`
        search, docs/SEARCH.md. Found while doing it, not in the plan: a warm
        start trusted rows written under other delimiters, so the cache now
        records them (state table) and re-parses on a mismatch.
  - [x] B: Alt+] / Alt+[ select whole runs (wraps, status says "Meta note
        2 of 3"), "Delete meta note" in the context menu (one undo step),
        MetaNotesPane (grouped by heading, filter, caret follow), status-bar
        count, Vault toggle running `meta:`; all absent while the setting is
        off. Merged 81b091c.
  - Known limits: pane rows clip at two lines (full run in the tooltip);
    vault mode shows one snippet per note.
  - Gotcha found: `<<x>>` with `<` delimiters is an HTML tag to the parser,
    not a run; `<< x >>` works. Documented in SEARCH.md; revisit if Jernej
    picks angle brackets.
- Design corrected by Jernej 2026-10-17: "[] is just link normally, no? I
  just use them as such does not mean that this is the correct way." So:
  bare `[text]` gets Obsidian's link-style colour for everyone (plain
  markdown styling); meta notes are a separate opt-in (default off) with the
  writer's own delimiters.
- [x] Word count settings: "can be toggled in settings to exclude
      everything within certain characters, and also toggled to ignore
      formatting". Lane meta.
- Never hide meta notes while writing: "always there until deleted".
  Lane meta merged 2026-10-17: --bracket-color (all users), editor.metaNotes
  (off by default) + editor.metaNoteOpen/Close, two count toggles; also fixed
  setMode dropping style spans in notes with footnotes. Gaps: no test of
  MainWindow::parseOptions / settings rows; publisher draws neither.
- Proposed, not picked (2026-10-17): clean export; quick insert + resolve
  menu; typed meta notes via meta.ini.

## Requested by Jernej 2026-10-17: sidebar

- [x] "Improve look and feel of the sidebar please. Just in general. Does not
      feel right, especially the gap on the left being lit up" (screenshot:
      the selected row lights the indent gutter as a separate block left of
      the item). Plan: Fable (PLAN-sidebar). Done (lane sidebar-look):
      delegate-drawn pill inside the row, themed chevrons + indent guides,
      .md hidden with type tags for other files, F2 renames through the
      vault (links follow), collapse-all + sort (files.sortOrder).
      Left: tree drag-and-drop, created-time sort.
- [x] Icons on folders and notes, "UI only, not actually saved in the file
      name". Done by Opus directly (the lane launch was refused): FileIcons
      in the app's per-vault icons.json, "Change icon..." / "Remove icon" in
      the tree menu (bundled icons or one character), tree + tabs, follows
      rename/move/delete/merge, appearance.fileIcons toggle. Merged 1d327d7.
      Left: more icons need a Lucide download (ask first); icons in the
      quick switcher and inline title; Iconize import; a test of the picker.

## Requested 2026-10-17: diagrams, and a central server

- [x] **PlantUML, TikZ (TikZJax-style) and Graphviz rendered like Mermaid**,
      when the tool is installed. Jernej: "PUML, tikjax, mermaid, should be
      renderable if the backend requrement is installed. prefereably if there
      is a c++ package that should be bundled in". Checked 2026-10-17: no
      C/C++ renderer exists for Mermaid (mmdr is Rust, already used),
      PlantUML (Java only) or TikZ (TeX only); Graphviz has a C library
      (libgvc 16.1, installed) but parsing a shared note's graph in-process
      with a C parser that has a CVE history risks the editor (and its
      unsaved text) on a crafted note, so it runs as `dot` in the sandbox
      like the rest; linking libgvc is a switch away if wanted.
      Installed here: plantuml 1.2026.8 + java, graphviz 16.1 (dot), TeX
      Live latex + dvisvgm, mmdr. Plan (lane diagrams):
      - [x] One diagram renderer for fences by language: `plantuml`/`puml`,
            `tikz`, `dot`/`graphviz`, built on MermaidBlock's pattern (pool
            thread, tool resolved on first fence and remembered, memo +
            disk cache by source/theme, source + one dim hint when the tool
            is missing or fails, nothing at startup).
      - [x] Each tool sandboxed: bwrap + seccomp, no network, a temp dir
            only (PlantUML can !include files and URLs; TeX can read
            files); PlantUML also with its SANDBOX security profile; TeX
            with -no-shell-escape. Timeouts.
      - [x] TikZ: TikZJax's block shape (`\usepackage` lines, then
            `\begin{document}...\end{document}`) and a bare tikzpicture
            both; standalone class; latex -> dvisvgm --no-fonts; readable in
            the dark theme (TikZJax's invert option, done by recolouring
            black to the text colour).
      - [x] PlantUML and Graphviz in the theme's colours where the tool
            allows (PlantUML -darkmode / skinparam, dot -Gbgcolor etc.).
      - [x] Settings: render.plantuml, render.tikz, render.graphviz (each
            off costs nothing), tool path settings like render.mermaidTool.
      - [x] Publish and PDF export draw the cached SVGs, never start a tool.
      - [x] Tests per kind (skipped when the tool is absent), cache hit,
            missing tool hint, sandbox has no network; docs/EDITOR.md.
      Done (lane diagrams, e5ee9d2..4cdf270): diagramRenderer per kind with
      shared sandbox::bubblewrapCommand; refused without bubblewrap; publish
      embeds each cached SVG as an <img> data URI (a diagram's own markup,
      e.g. TeX \special raw SVG, can never script the page).
      Follow-ups:
      - [x] (lane fixes, c9b478a; only the tool file is bound) A tool set by path (render.*Tool) outside /usr is not bound into
            the sandbox: document or bind it read-only like DirectRunner.
      - [x] (lane diagrams2, eaef779: pool per kind, 2 threads, in-flight dedupe; two hung fences of one kind still block it) A hung tool holds a pool thread up to its 20-40 s timeout;
            other fences wait behind it.
      - [x] (lane diagrams2, 1eb447c: -Xmx512m etc. + RLIMIT_DATA 2 GiB) The JVM (PlantUML) runs with no address-space limit (RLIMIT_AS
            breaks it); only the scope memory cap applies.
      - [x] (lane diagrams2, d1e384a: source as escaped alt, 2000 chars) Published diagram text is not selectable or searchable (image).
      - [x] (lane fixes, ea28195: `diagram`) Publish CSS class is still `mermaid-diagram` for every kind.
- [ ] **A central server with only a local cache, like Trilium** - Jernej:
      "is there an API, like trillium notes so a central "server" node can
      store the data with just a local cache?" Today: no. Sync
      (docs/SYNC.md) is local-first and end-to-end encrypted: the server
      stores ciphertext only and every device keeps the whole vault; there
      is no HTTP API over notes (the server cannot read them). Waiting on
      Jernej's choice of model (answered in chat 2026-10-17): on-demand
      local cache over the existing encrypted sync, a trusted plaintext
      server with an ETAPI-style REST API, or a local REST API on each
      device.

## Fable review round 6, 2026-10-17: what TODO did not have

Fable read the whole tree for bugs and missing Obsidian behaviour not listed
or deferred here. Built in waves of parallel lanes (3 at a time: 25 GB free).

Wave 1
- [x] 1. (lane utf8, 5a784bf) A note with an invalid UTF-8 byte is rewritten on its
      first edit: Vault::decode substitutes U+FFFD, autosave writes it back.
      Plan: QStringDecoder, Note::lossy; autosave and plain save refuse a
      lossy note with a status message; an explicit choice saves anyway;
      a banner on open. Test: "a\xE8b" survives typing + autosave byte-exact.
- [x] 12. (lane utf8, 5a784bf) Deletes always go to the vault's .trash; Obsidian's
      default is the system trash. Plan: setting files.deletedFiles =
      system (default, QFile::moveToTrash) / local / permanent, Obsidian's
      app.json trashOption read when present; sync pulls keep .trash.
      Done (both): Note::lossy from a stateless QStringDecoder; Vault::write
      refuses a lossy note (so link rewrites, Bases, plugins and merge
      cannot corrupt it either); autosave skips it with a status message,
      Ctrl+S asks "Save anyway?", leaving with edits offers Save/Discard.
      Deletes: files.deletedFiles system (default) / local / permanent,
      Obsidian's trashOption honoured, a failing system trash falls back to
      .trash (never permanent). Behaviour change: notes deleted in the app
      now go to the desktop trash, as in Obsidian, not the vault's .trash.
      Follow-ups:
      - [x] The indexer and other read-only readers index a lossy note's
            U+FFFD text (lane links, 2090c5f: index re-reads bad bytes as
            Latin-1; cp1252 quotes still not).
      - [x] (lane fixes, 96c93eb: retry on timeout, real-tool test no longer skips on failure) Mermaid tests time out (mmdr 5 s) when several lanes build at
            once: make them load-robust like the 1 MB open test.
- [x] 2. (lane syncfix, 10a430f; done by Opus: the agent launch was refused) A sync pull of a delete whose trash move fails still
      drops the entity, so the next scan re-creates the note everywhere.
      Plan: keep the entity on failure, retry next pull, show it in status.
      Done: the pull stops at that record (cursor and entity kept), status
      Error names the note, next cycle retries. Test ADeleteThatCannotBe-
      TrashedIsRetriedNotUndone, seen failing with the old code.
- [x] 10. (lane syncfix, 98edf26) inotify addPaths() failures are dropped silently.
      Plan: status message + periodic rescan fallback.
      Done: 30 s rescan while any folder lacks a watch, one status message;
      an unwatchable open note is reported (not polled: reopen shows it).
      Test AFolderThatCannotBeWatchedTurnsOnPolling (a 000 folder), seen
      failing with the fallback disabled.
- [x] 4. (lane move, 0afeed9) Notes and folders cannot be moved between folders.
      Plan: Vault move with renamePath's checks; renameVaultPath gains a
      target folder (links, history, tabs, icons, index follow); tree drag
      and drop (InternalMove, own dropEvent); command "Move file to..."
      with a folder picker.
- [x] 11. (lane move, 0afeed9) Case-only rename refused on case-insensitive file
      systems ("already exists").
      Done: Vault::movePath (refuses outside, hidden, into itself, clash),
      renameVaultPath split so rename and move share followPathChange
      (links, tabs, icons, history, index); tree drag and drop through the
      window only (own startDrag: the base would delete the dragged files
      from QFileSystemModel); "Move to..." menu and file-explorer:move-file
      with a folder picker. Case-only rename via std::filesystem::rename
      when the destination is the same file.
      Follow-ups:
      - [x] Relative Markdown links inside a moved note (lane links, 769dd12).
      - [x] (lane fixes, 3043c45) `<angle bracket>` Markdown link targets are stored with their
            brackets by the parser, so they never resolve as backlinks or
            follow a rename of their target.
      - [x] The real drag gesture (lane links, 8c4506a: own mime marker,
            a foreign window's drag is refused).
      - [ ] Case-only rename not run on a real case-insensitive filesystem
            (needs a mount; ask Jernej or skip).

Wave 2
- [x] 3. (lane single, 65c557b 9131e80) No single instance: opening a note from the file
      manager starts a second copy on the same index.db/journal.db.
      Plan: QLocalSocket handoff in main.cpp, QLocalServer in MainWindow;
      vault detection also via the recent-vaults list (a vault without
      .obsidian).
      Done: $XDG_RUNTIME_DIR/notolith.sock (owner-only) + a lock file held
      from before the window (Qt's listen() unlinks a live socket, so two
      racing launches would both serve); XDG_ACTIVATION_TOKEN handed over;
      miss costs ~0.1-0.3 ms. Follow-ups:
      - [ ] Wayland focus with the token not seen on a real compositor.
      - [x] (lane fixes, a2e4cd0) A note outside the vault opened by hand-over is saved in the
            layout as `../` and its restore is untested.
      - [x] (lane fixes, a2e4cd0) A different-vault hand-over has no test of its own.
- [x] 6. (lane complete, a9b48ef) `[[` completion: headings after `#`, block ids
      after `#^`, aliases inserting `[[Note|alias]]`.
      Done: index headings/block ids per note (current note read live),
      alias rows "alias -> Note", `![[` too; asked once per popup.
      Follow-ups:
      - [x] Heading text with `#`, `|`, `^` inserted verbatim (lane links,
            786b051: ObsidianSyntax::headingLinkText both sides; strip set
            from memory, not checked against Obsidian).
      - [x] (lane links2, bbd976f; published anchors of embedded headings may still shift the -2 numbering) Nested `[[N#H1#H2]]` not offered or resolved (resolver is
            single-level).
      - [x] An alias inserts the note's basename (lane links, 87af41f:
            `folder/Note` when the basename is shared).
- [x] 5. (lane workspace, 2e72085) Autosave is debounce only: continuous typing never
      saves. Plan: a 30 s max-wait timer.
- [x] 7. (lane workspace, 2e72085) Undo close tab (Ctrl+Shift+T), 20 deep.
      Follow-ups: [x] (lane fixes, d0bc503) encrypted-note skip and scroll restore not asserted.
- [x] 8. (lane workspace, 2e72085) Bench row for saveLeaf on a 1 MB note (two
      fdatasyncs on the UI thread); act only if it shows.
      Measured on NVMe btrfs/LUKS: median 6.0 ms (1 MB), worst 17.4 ms
      (likely first save + snapshot, not checked). Under 10 ms median: no
      change.

Wave 3
- [x] 9. (lane split, f90951b..1008bb9; partly) MarkdownEditor.h (2 791 lines, 46 TUs) private part behind a
      private header, so editing the editor rebuilds ~10 TUs, not 46.
      Done: 154 members + 29 nested types in MarkdownEditor_p.h (d-pointer,
      like MediaPlayback); a private-state edit rebuilds 24 TUs (28.8 s)
      instead of 50 (1:24.9); 13 includes dropped; bench unchanged.
      24 is the floor without splitting the editor: every editor .cpp uses
      d->. Follow-up:
      - [ ] ~355 private method declarations still in MarkdownEditor.h, so
            adding a helper rebuilds all 48 TUs; moving them needs Q_D-style
            Private methods (not mechanical). Do only if rebuilds hurt.

## Fable audit round 5, 2026-10-17 (rename + WASM tier)

No sandbox escape found: a note cannot pick the module, reach the network or
any file outside /cell, or get a kernel in the wasm tier.
- [x] 2. A wasmtime or module reached through a symlink outside /usr was
      bound by its target but run by the link, which does not exist inside
      bwrap. Now runs the resolved path (test ASymlinkedWasmtimeRunsInside-
      Bubblewrap, seen failing without the fix).
- [x] 3. A Haskell block in the wasm tier was rewritten into a ghci script
      first; the module now gets the block as written. No test (no Haskell
      module to run).
- [x] `--` before the module, so a module path starting with '-' is a path.
- [x] Gap: no test that the guest sees only /cell. TheGuestSeesOnlyItsOwnDirectory
      lists the guest's preopens with an embed folder asked for (seen failing
      with a second --dir).
- [x] 5. `notolith --version` printed "noteAppC 0.0.1"; now "Notolith 0.0.1".
- [x] 6. StartupWMClass is now `notolith` (X11 WM_CLASS instance is argv[0]);
      not checked with xprop.
- [x] 7. PKGBUILD conflicts/replaces noteappc, the old package name.
- [x] 8. settings.ini header comments and note-history commit author say
      Notolith. The "generated by noteAppC" markers stay: they are parsed.
- [x] 1. Without bubblewrap the wasm tier ran wasmtime alone (no netns, no
      seccomp), where tier 1 refuses. Now refuses too: Jernej's 2026-09 rule
      for tier 1 ("each refuses and says to install bubblewrap") applied to
      the new tier (asked 2026-10-17; question dismissed). The Arch package
      depends on bubblewrap. Test WithoutBubblewrapTheWasmTierRefusesAsTierOneDoes.
- [x] 4. Opening an .ipynb seeded every cell's output under the tier-1 key,
      so a `#| sandbox: wasm` or `host` cell showed no stored output. Now the
      cell's own sandbox line picks the key (host under both host and tier 1,
      since Settings decides); test ACellsOwnSandboxLineChoosesTheKey...,
      seen failing without it.

## Fable audit round 4, 2026-10-17 (scratchpad AUDIT4.md)

- [x] 1. openVault sets the indexer's vault before the vault's own settings:
      a per-vault meta setting re-parses the whole vault on every open. Not
      reachable today (editor.metaNotes is global, only Obsidian keys are per
      vault); reordered anyway, warm-open test guards it.
- [x] 2. Delimiter rows apply per keystroke: a forced vault scan + editor
      reparse per valid intermediate pair. Commit on editingFinished.
- [x] 3. updateLinksAfterRename and Bases parse without ParseOptions and erase
      meta rows of linking notes; the watcher then skips them as current.
- [x] 4. FileIcons::save on every rename/delete even with no change.
- [x] 5. Icon picker rejects multi-code-point emoji; count graphemes.
- [x] 6. Typing in a meta note repopulates every pane row per parse.
- [x] 7. setMetaNotes(off) mid-scan: DELETE contends with the writer's
      transaction on the UI thread; skip it while a scan runs.
- [x] 8. Vault-mode filter pasted raw into `meta:(...)`; quote it.
- [x] 9. `[a][` jumps to the next `]` anywhere; stop at the line end.
- [x] 10. Icon entries never pruned; drop missing paths on load.
- [x] 11. shiftRanges does not shift `uncounted`.
  Lane audit4 (f239eaf..a33e595). Tests seen failing without the fix: 2
  (4 scans vs 1), 3, 6. New tests for 4, 5, 7, 9, 10 pass and guard them
  (7: old code was right, only blocking). No test: 8, 11.
- [x] 12. For Jernej: meta notes and bare brackets share the accent colour by
      default (pinned by a test). Distinct default (orange) wanted? He picked
      "Orange meta notes": --meta-note-color = --color-orange, bare brackets
      stay the accent.

## Found 2026-10-17 (housekeeping)

- [x] 44 `/tmp/noteAppC-test-*` vaults were left in /tmp by test runs.
      QTemporaryDir cleans up on a normal exit, so these come from killed
      runs (timeouts, mutation checks) or a test that keeps its dir; find
      out which. /tmp is a 31 GB tmpfs and filled up during check.sh (other
      projects' build dirs, ~25 GB). Deleted them, and the holding copy
      /tmp/noteAppC-history-leaked from Jernej's 2026-10-16 "leaked history
      delete" decision. A full check.sh run afterwards left none: they came
      from killed runs only, no test keeps its dir.

## Fable audit round 7, 2026-10-18 (round 6 merges)

Clean: bwrap command, readOnlyBinds, publish img data URIs, single-instance
socket, undo close tab, autosave max-wait, movePath, lossy write refusal,
Latin-1 index text, syncfix retry; nothing costs at startup or per keystroke
when off.

- [x] 1. (lane relink, 1436af5) BUG, data loss: `Index::updateLinksAfterRename`
      splices at link offsets from the index, which lags a save (reindex on
      the pool, 300 ms watcher debounce). Type at the top of B, then move A
      within a second: B's prose is overwritten at stale offsets.
      Plan: the DB only picks candidate files; per file re-parse the text
      just read and take ranges/targets from that, matching by resolve()
      as the DB pass does. Test: prepend text to B on disk without
      reindexing, rename A, B intact and its link rewritten.
- [x] 2. (lane relink, 714608e) BUG: a lossy (non-UTF-8) linking note makes the
      rename's write fail and `return -1` mid-batch, leaving later notes
      with dead links. Plan: skip a lossy note (count it, name it in the
      warning), continue. Test: lossy + clean linker; clean rewritten,
      lossy bytes unchanged, result >= 0.
- [x] 3. (lane relink, 1c3a3b2) BUG vs Obsidian: `linkTargets()` drops a note name
      that is also another note's alias, so note Foo cannot be completed
      when Bar has alias Foo. Plan: keep both rows. Test in test_index.
- [x] 7. (lane relink, b1adf86) IMPROVEMENT: onIndexChanged/configureEditor query
      aliases twice plus all names on the UI thread per index change.
      Measure; if it shows, share the alias list / cache in Index.
      Measured on 20 000 notes: linkTargets 18-22 ms, aliases ~2 ms, per
      index change and per tab. Now cached until a note's names change.
      Follow-ups:
      - [x] (lane links2, 6dc99aa: Indexer::waitIdle(1000) before the rewrite; move path untested) A note whose first link to A was added since the last index run
            is not a rename candidate (same lag as before; would need a
            full scan).
      - [x] (moved to audit 8 item 12) The first list query after a name change still costs ~22 ms
            on 20 000 notes (build incrementally if it shows).
      - [x] (lane links2, b216af9) The status-bar "links in a note were not updated" path has no
            window test.
- [x] 4. (lane etc, f527ed3) RISK, security: bwrap binds all of /etc; a shared
      note's tikz fence can `\input{/etc/machine-id}` (or hostname,
      passwd) and Publish ships it in the SVG. Plan: bind only what the
      tools need (fonts, texmf, ld.so.*, alternatives, java-*, localtime,
      passwd/nsswitch only if the JVM insists) with --ro-bind-try; flip
      tests/test_diagrams.cpp:314 to assert /etc/machine-id unreadable;
      every kind still renders; notebook runner too if it shares the
      helper.
- [x] 8. (lane etc, ee97769) Tests: wall-clock bounds at test_window.cpp:780 and
      the qWait(200) EXPECT_FALSE at :5826 can flake under load; make them
      load-robust without making them unable to fail.
- [x] 5. Heading link text: we replace `#|^[]:\` and `%%` with a space;
      Obsidian may remove them instead. Check Obsidian's documented
      behaviour (help site / forum; never launch Obsidian) and match it.
      Checked 2026-10-18 (forum.obsidian.md/t/40707, /t/40724): Obsidian
      replaces punctuation with spaces, not removal (`[src](https://a.b)`
      in a heading became `src https a b`); since 0.16 it keeps more
      punctuation in links it writes; no official list exists. Space is
      right. Staff: links to headings are compared with non-alphanumerics
      stripped, so `[[N#heading!]]` resolves to `## heading?`.
      - [x] (Opus, headingKey; both tests seen failing under mutation) Resolve heading links punctuation-insensitively like Obsidian
            (compare both sides with all non-letter/digit folded to
            space), so links Obsidian 0.16+ writes with kept punctuation
            still resolve. After lane relink merges (MainWindow
            subpathOffset/jumpToHeading, PageBody).
- [x] 6. (lane syncwedge, 81a67f4: parked after 3 failures, persisted in journal meta, retried each cycle, newer record cancels, status Error names it) NOTE: a remote delete whose trash move fails permanently stops
      every later pull record each cycle (Status::Error). Defensible (never
      drop a delete); later: skip after N cycles and show it in the UI.
- [x] 9. Behaviour choice: a second launch naming another vault switches
      the running window instead of opening a second one (Obsidian opens a
      window per vault). Documented in docs/PLATFORMS.md; kept: one window
      per user is what lane single was for.

Found 2026-10-18 (lane etc report):
- [x] (lane mmdr, a2adb38; +8.5 ms per uncached diagram on the pool, 14 ms median) Mermaid's mmdr runs as a plain QProcess, outside the
      sandbox, on every shared note's ```mermaid fence (on by default);
      every other diagram tool runs in bwrap. Plan: run mmdr through
      runSandboxed (bind its canonical path when outside /usr, e.g.
      ~/.local/share/cargo/bin), keep the 5 s budget; without bwrap keep
      rendering unsandboxed (a parser of data, like poppler in-process, and
      Mermaid is on by default), with the behaviour documented. Tests with
      the fake mmdr must still assert what they did (stdin, colours).
- [x] (moved to audit 8 item 14) PluginHost still binds all of /etc: plugins are user-installed native
      code with granted permissions (network needs resolv.conf, ssl); left
      as is, outside the shared-note threat.

## Fable audit round 8, 2026-10-18 (relink .. syncwedge)

Clean: JAVA_TOOL_OPTIONS inside --clearenv; alt-text escaping; etcBinds;
findHeading linear on hostile input; diagram in-flight dedupe; link-list
cache invalidation; fresh-parse rewrite; parked-delete ordering vs local
edits; every new test can fail.

Lane sync8:
- [x] 1. (lane sync8, 4558461) BUG, data loss: after a delete is parked, a later put of a new
      entity at the same path (`!existing`, so not localChanged) overwrites
      the locally edited file. Plan: in the put branch, when no entity owns
      the path but a file exists there with a different content address,
      write the incoming as a conflict copy and keep the local file (same
      for attachments; also covers two devices creating one path). Test:
      A deletes, B parks, B edits, A recreates, B syncs: B's text kept.
- [x] 9. (lane sync8, 34b6c44) pendingDeletes() read from the journal per pulled record: load
      once per cycle, write-through.
- [x] 10. (lane sync8, 423dbed: Idle + message tooltip) A parked delete shows Status::Error forever: a Warning status
      (or Idle + message) so healthy sync does not read as broken.
- [x] 11c. (lane sync8, 5013853) Document that the 3-failure count restarts each launch.
- [x] 15. (lane sync9, 6f68676: rename held back, entity keeps its old path and takes the content; lands once the target is free) BUG, data loss: a pulled rename onto a path owned by
      a different entity (or any existing file) takes QFile::rename, which
      fails, then Vault::write overwrites the target. Plan: same rule as
      item 1: an occupied target with other content gets the incoming as a
      conflict copy; the local file stays. Test: B has note X at path P
      (its own entity), A renames Y to P; B keeps X, Y's text in a copy.
- [x] 16. (lane sync9, 1a0e9bd) Item 1 has no attachment test; add one.
- [x] 18. (lane sync10, 84580ab; partly: the reported case is the items below) BUG: B edits y.md, A meanwhile renames y.md to
      z.md and edits it; after B syncs both devices hold y.md (B's edit)
      and z.md, unmerged. Suspected: the localChanged branch merges into
      existingPath but saves the entity with remote.path, so file and
      entity disagree. Plan: confirm with a test, then move the merged file
      to remote.path (or keep the entity at the old path) consistently;
      no data loss either way.
- [x] 19. (lane sync10, 9249899: a dangling link let a pull write outside the vault) A directory (or symlink) at a pulled rename's target is not
      checked (isFile only); verify nothing is lost.
- [x] 23. (lane syncbase, 14fcef1..c2c92a9; Fable design in the session scratchpad SYNCBASE-DESIGN.md: parent/hash in records, 32-state history, merge results pushed, seq-ordered conflicts) BUG, data divergence/loss: pushed
      records carry no parent/base hash, so a record from a device that has
      not seen this device's push is applied as a fast-forward over local
      work, and a clean merge advances the base to the merged text instead
      of the sender's. Probe: OfflineEditsOnDisjointLinesOfTheSameNote-
      MergeCleanly, more rounds: A ends `alpha|beta|gamma|DELTA` (its ALPHA
      edit gone), B has the merge. Three DISABLED_ rename+edit tests in
      tests/test_sync.cpp show the same. Plan: Fable designs (parent hash
      per record and/or push the merge result; old records stay readable),
      Sonnet implements, the DISABLED_ tests and a multi-round convergence
      check on every merge test pass.
- [x] 24. (lane syncbase, 00d855c: Dice >= 0.5, unique best, <= 64 pairs) Rename + edit in one scan is seen as delete + new note (scanner
      matches renames by exact hash only): match by similarity to the
      stored base text. DISABLED_ARenameAndEditInOneScanMergesAtTheNewPath.
- [x] 25. (lane confine, a75aec7: Vault::isConfinedPath at one choke point; refused records dropped and named; scanner skips symlinks) Pulled record paths are not confined to the vault
      (`../`, absolute, under a symlinked folder; the engine never calls
      Vault::confinedNotePath) and a symlink at an entity's own path is
      written through by a plain pulled edit. Needs a device with the vault
      keys, but confine anyway: refuse such records with a status message.
- [x] 28. BUG: an attachment (or oversized note) copy whose bytes are
      already in the blob store never reaches other devices: the server keeps
      the first blob per content address, sealed under the first entity's
      noteKey; the copy's record references that address with another
      noteKey, openBlob fails and applyRecord returns silently. Seen in
      ConcurrentAttachmentEditsKeepBothVersions (weakened to match). Plan
      (Fable): blobs sealed under a key independent of the entity, or a
      per-entity address; and never drop a record silently - status line.
- [x] 29. Announce noise (syncbase): every merge announces, so 3 devices
      push 9 records for 3 edits, and a device offline across 33 edits makes
      the other push 33 merge records and take 34 counted conflicts (no loss,
      converges). Measured: the announce is load-bearing only when the
      ancestor is pruned. Plan (Fable): coalesce to one announce per entity
      per pull batch, or announce only when the sender cannot have the
      parent; keep convergence tests green.
- [x] 30. Not verified: an old journal upgraded through a real cycle (only
      the schema upgrade is tested); Dice 0.5 and the 64-pair cap untuned.
- [x] 27. A password change replaces the content key, so every keyed
      localHash/syncedHash stops matching: full re-push and "local changed"
      on every incoming record afterwards (pre-existing; found by Fable).
      Plan after syncbase: rehash the journal under the new key in
      changePassword.
- [x] 26. Confinement follow-ups: check-then-write, not openat(O_NOFOLLOW)
      (a link swapped in between wins the race); retryPendingDeletes does
      not re-check its journal path; a backslash in a Linux filename is now
      refused on pull (the scanner pushes it). Plan: O_NOFOLLOW + openat
      on the parent fd in Vault::write/raw writes; re-check in
      applyRemoteDelete; refuse backslash names on push too, with a status
      line, so both sides agree.
- [x] 17. A note created on two devices at one path conflicts again on each
      later edit until one side renames it (documented). Later: adopt one
      entity for identical paths after the first conflict.

Lane index8:
- [x] 2. (lane index8, 12a77ed: waitIdle gone; saved + in-flight paths as extra candidates) waitIdle before rename stalls the UI (debounce rebuild = full
      vault walk, up to 1 s) and re-enters every Index::changed consumer
      mid-rename; past the cap the link is missed silently. Plan: drop
      waitIdle; autosave() returns the paths it saved, Indexer exposes
      paths in flight, both added as extra candidates to
      updateLinksAfterRename. Test: rename right after a timer save; no
      Index::changed during renameVaultPath.
- [x] 3. (lane index8, 20b919f) Indexer::start gates on isRunning(), not on the last batch being
      applied; apply() then blocks on the next scan's waitForFinished and
      skips notifyChanged for the previous writes (stale panes). Plan: gate
      on m_active, drop the waitForFinished. Unit test.
- [x] 6. (lane index8, 0b847c0) updateLinksAfterRename writes notes whose text did not change
      (folder rename with sibling [[links]]: every note rewritten). Plan:
      skip unchanged text. Test: rewritten == 0, mtimes unchanged.
- [x] 7. (lane index8, a1bdbfc) One unreadable candidate (deleted externally) aborts the rewrite
      after the file moved. Plan: skip and name it like a lossy note.
- [x] 12. (lane index8, ec277e5: 16.0 -> 3.6 ms) linkTargets() rebuild ~22 ms on 20 000 notes on the UI thread
      after a name change (TODO above): derive from the in-memory names.
- [x] 13. (lane index8, 0b5aa1f: only the named folders; save to idle 440 -> ~360 ms) Every in-app save arms the watcher debounce -> rebuild() (full
      walk). Plan: reindex only the changed paths the watcher reported.
- [x] 20. (lane watch, e645dc0: DirWatcher on inotify, IN_CLOSE_WRITE; held-open writes seen on close) An external program editing a note in place (no
      temp file + rename) that is not open is never seen: QFileSystemWatcher
      directory watches report entry changes only. Obsidian sees such
      edits. Plan: inotify IN_CLOSE_WRITE/IN_MODIFY on the vault folders
      (Linux; QFileSystemWatcher stays the fallback elsewhere), feeding the
      same incremental debounce. Cost when idle: zero. Test: write in place
      to an unopened note; the index sees the new text.
- [ ] 21. A whole-vault scan in flight cannot be listed as rename
      candidates (documented ceiling). Merge-note part done: it autosaves
      first and passes the saved + in-flight notes, as a rename does.
- [x] 22. (lane watch, c8d1420: GraphView::showEvent race, 0/1500 after) WindowTest.LocalGraphFollowsTheOpenNote failed once in a full run
      and under --gtest_repeat (state leak between repeats?); investigate.

Lane misc8:
- [x] 4. (lane misc8, ee3c6b4) The unsandboxed mmdr fallback (no bwrap) has no rlimits and the
      full environment. Plan: same child modifier (RLIMIT_AS/CPU) and an
      environment of PATH + HOME=temp dir. Test with the limits fake.
- [x] 8. (lane misc8, 0b99669: dot first render 158 -> 13 ms) Sandboxed tools rebuild the fontconfig cache each first render
      (/var/cache/fontconfig not bound, HOME is a temp dir). Plan:
      --ro-bind-try /var/cache/fontconfig; time a first dot render.
- [x] 5. (lane misc8, 64e81fc) headingKey merges `C`, `C++`, `C#` (first wins): `[[N#C++]]`
      jumps to `## C`. Plan: exact headingLinkText match first, key as
      fallback; NFC before keying. Test C / C++.
- [x] 14. (lane misc8, 0a081d8) PluginHost binds all of /etc: use etcBinds plus resolv.conf,
      hosts, ssl, ca-certificates, nsswitch.conf, localtime for plugins
      with the network permission, the default list otherwise.
- [x] 11. (lane misc8, 8c1c2a8) Nits: alt text left(2000) may split a surrogate pair;
      static_assert pools/pruned arrays against DiagramKind.

## Fable round 9, 2026-10-18 (syncbase review; plan in the session scratchpad SYNC-ROUND9-PLAN.md)

- [x] F1. (lane A conflicts) BUG: a conflict seen on one side only loses
      the loser's text (masked by the announce cascade); fix: the loser
      copies its own text, announce iff merged != remote, announce an
      unrecorded local winner.
- [x] F2. (lane A) BUG: a skipped own echo leaves the base behind; a later
      delete is ignored forever. Echo advance when own == localHash; delete
      ordered by seq.
- [x] F3. (lane A) BUG: own rows pruned before their echo (33+ scans
      offline) break the echo chain; take the base from the record itself.
- [x] F4. (lane A) BUG: renaming back to a former name is ignored.
- [x] Item 29 (lane A): coalesce announces per pull batch; bound k + n.
      Merged: bound is one merge record per (device, entity, pull), so 3
      devices measured 7 pushes (was 9); F3 overlaps F2(1) (killed only
      jointly); parked-delete link guard is defence in depth (scanner
      refuses the link first).
- [x] Item 28 + F5 (lane B blob): per-entity blob address; blob fetch
      failure parks after 3 cycles with a status line, never dropped
      silently; push-side confinement (backslash names).
- [x] Item 27 (lane C contentkey): content key kept per device in journal
      meta, never changed by a password change.
- [x] Item 26 (lane D nofollow, then lane A): Vault::writeBytesNoFollow /
      writeNoteNoFollow (openat O_NOFOLLOW, renameat); engine writes use
      them; applyRemoteDelete re-checks its path. Vault part merged
      (keeps the file mode); engine call sites left for lane A.
- [x] Item 30 (lane E upgrade): old journal through a real cycle; scanner
      Dice tests; journal.db size numbers; F8 documented. 20000 notes =
      2.7 MiB journal (143 B/note); hot note 3000 saves = 204 KiB + WAL.
- [x] Flaky under load (seen in lane gates at load ~15): test_window
      inline-title rename test asserted indexer->isRunning() right after
      the save (scan may not have started yet) and a failed ASSERT left the
      lock thread joinable -> "terminate called". Fixed: qWaitFor +
      qScopeGuard join. Verify in main check.
- [x] Parked blobs are forgotten on restart (lane parked, round 10 plan; was documented): the
      cursor is past the record, so the attachment only reappears when its
      entity next changes; the status line about it vanishes too. Plan:
      a journal table parked(recordID, entityID, path, blobAddress,
      itemsKeyID, record bytes) written on park; each cycle retries the
      parked rows first (fetch + apply through applyRecord with the stored
      record), drops a row once applied or once a newer state of that
      entity arrived; unreadableWarning reads the table. Test: park, stop,
      restore the blob dir, restart, next cycle writes the file and the
      warning is gone. Mutation: skip the retry -> file absent.
- [x] Flaky under load: NotebookKernelFeatureTest
      .ATimedOutRunIsInterruptedAndADeadKernelIsReplaced. Real bug: the
      block timeout was armed at submit, so a kernel slower to start than
      the timeout got SIGINT before its handler and the run was never
      delivered (block stuck). Now armed on the kernel's "busy" for that
      request; test with a kernelspec that sleeps 1.5 s before starting
      (mutation: arm at submit -> never delivered).

## Multi windows and panes (Jernej, 2026-10-18)
Jernej: "multi windows and multi panes. like you can tile different notes in
your app, you can also detach the sidebar or note windows from the app, like
in zed or vscode, so its a child window owned by main app outside it".
Splits right/down exist already. Plan by Fable in the session scratchpad
WINDOWS-PLAN.md; items below get filled from it.
- [x] Fable plan: drag-to-tile, pop-out note windows, detachable sidebars and
      sidebar views, child windows owned by the main window (one process,
      one index), layout persisted in workspace.json, off-switch at zero cost.
      Decisions: pop-outs are parentless top-levels owned by MainWindow (a
      parented Qt::Window is an xdg transient Hyprland floats like a
      dialog); never move(); m_active stays global; feature
      "popout-windows" (off = zero objects, no acceptDrops).
- [x] Lane A (windows core, merged; open 11 ms, move tab 3.3 ms): PopoutWindow class; root-split helpers,
      collapseIfEmpty (main never empty), moveLeaf, splitBeside,
      newPopout/closePopout/deletePopout, closeEvent order; commands
      workspace:open-in-new-window / move-to-new-window + tab context menu;
      hotkeys inside pop-outs; feature switch + dockAll; docs EDITOR.md
      "Windows and splits", PLATFORMS.md Hyprland note; bench pop-out open
      (25 ms budget) and move tab (5 ms). 7 tests.
- [x] Lane B (windows persistence, merged; Leaves pop-outs + active leaf in a pop-out): workspace.json "floating" written and
      read (Obsidian shape), Obsidian's own floating node read never
      written, detached sidebar restored. 3 tests.
- [x] Lane C (detachable sidebars, merged; 4 tests + 2 strengthened): whole left/right sidebar and single
      right panes into their own window and back on close; narrow-mode,
      toggle and showRightPane guards; context menus + commands. 3 tests.
- [x] Lane D (drag-to-tile, merged): tab drag with drop zones (edges split,
      center joins, tab bar inserts), overlay, drop outside -> new window,
      right tab buttons dragged out -> detached pane. 4 tests.
- [ ] By hand on Hyprland (Jernej): pop-out tiling/floating, real drag
      between windows, focus; offscreen cannot show these.
- [ ] Obsidian command ids workspace:open-in-new-window /
      workspace:move-to-new-window unverified: check against a real
      Obsidian hotkeys.json (Jernej) or a public source.
- [ ] Escape-cancelled tab drag with the pointer outside every window
      opens a pop-out (Qt reports it as a drop outside). Plan: watch
      KeyPress Escape in the app filter while m_dragging and treat the next
      IgnoreAction as cancel; verify by hand on Hyprland (offscreen cannot
      start a drag). Obsidian cancels on Escape.
- [ ] After lanes C + D: dragging a right sidebar tab button out of the
      window detaches that pane (lane popdrag skips it: detachPane lands
      in lane popside at the same time).
- [ ] After lanes B + C: persist detached sidebars / panes (Pane::viewType
      column, file-explorer etc.), test ADetachedSidebarIsRestoredAsAFile
      ExplorerWindow. (Main-splitter 3-children assertion: done, lane C.)

## Fable sync audit round 10, 2026-10-18 (scratchpad SYNC-ROUND10.md)
Lane sync10 (in this order, one function region, one lane):
- [x] B. HIGH: CRLF/BOM notes always "locally changed" (raw-byte hash vs
      LF syncedHash): a remote delete never applies (note resurrects), F2(1)
      never fires, and the CRLF format test passes for the wrong reason.
      One note hash definition (SyncScanner::contentHash on the read text);
      writeRemote always keeps the disk format.
- [x] A. HIGH: a conflict over a disk text with no state row (upgraded
      journal, save between scan and pull) fast-forwards the other device's
      text away with no copy. Winner writes the remote as a copy there.
- [x] D. MEDIUM: conflict winner's headDigest names the loser's text, so
      its next edit merges against the wrong ancestor (spurious conflicts).
- [x] C. MEDIUM: path rule's own gate breaks with a third device (C moves
      a renamed file back) and concurrent renames never converge: pure
      renames settled by server seq, content records by sawPath.
- [x] E. LOW-MED: an announce whose blob upload fails is lost for good:
      reset localMtime so the scanner retries.
- [x] Cleanups: delete F3 fallback once B lands (redundant), delete the
      unreachable applyRemoteDelete link guard (keep its test, fix
      comment), stale AnEditBeatsADeleteInBothDirections comment, document
      F (equal-merge announce can revive a concurrently deleted note).
Lane parked (parallel with sync10):
- [x] Journal table parked(...whole record), retryParked after pullJournal,
      later record for the entity unparks; openBlob failures stay
      session-only. Tests AParkedBlobIsRetriedAfterARestart,
      ALaterRecordDropsAParkedBlob.
- [x] (Fable: all correct; scratchpad SYNC-ITEM17.md) Fable confirm sync10 decisions: (1) pre-digest journal: an unchanged
      note with empty baseDigest takes digest(base/) as its base (else a
      fast-forward overwrites its own pushed text); a disjoint edit across
      an upgrade now falls to a conflict copy. (3) in the no-row win branch
      headDigest stays remoteDigest so the other device fast-forwards.
      Gap: pure-rename lookup state(entity, parent) has no baseHasNoRow
      guard (a rename on a pre-digest journal counts a state miss).
      Residual: rename back to a former name + edit before any sync reads
      as no rename (documented).
- [x] Attachment twin of finding A on an upgraded journal: unchanged
      attachment with empty baseDigest fast-forwards over its own pushed
      bytes. Fix: digest of the disk bytes as base (lane same17).
- [x] Dead guard !(inLine && baseHasNoRow) in the path rule; parked
      paragraph: own record also unparks (lane same17).
After sync10 (needs C):
- [x] Item 17: same path created on two devices: first-to-server keeps
      the path, loser moves to "<base> (<4 hex of entityID>)" on every
      device. Equal texts (daily notes from one template, the common case):
      re-key to one entity instead of two files (coordinator decision:
      a duplicate daily note on every first sync is worse than 15 lines).

## Install on starbase-1 (Jernej, 2026-10-18: "make the app appear in the app menu")
- [x] ~/.local/bin/notolith + desktop entry + icon (SYS_CHANGES starbase-1).
      MimeType line dropped from the installed copy: with it Notolith became
      the default for .md and folders.
- [x] Packaging bug found: vendored tinyxml2 built shared (global
      BUILD_SHARED_LIBS=TRUE), install() ships only the binary, so an
      installed notolith failed with libtinyxml2.so.10 not found (the
      PKGBUILD package too, unless its build differed). Fixed: tinyxml2
      static; main check running.
- [x] (written; first run pending) Installed copy goes stale as main moves: tools/install-local.sh
      (build, stage in /tmp, copy binary + desktop + icon, no MimeType
      unless asked). Ask Jernej whether Notolith should open .md files.
- [ ] Version is 0.0.1 (CMake project + PKGBUILD), never bumped.
- [ ] ~/Documents/AI/projects/noteAppC/build/ (2.1 GB, 2026-10-17,
      gitignored) is a build dir inside Syncthing; not made by this
      session's lanes. Ask Jernej before deleting.

## Sharing a note with another person, peer to peer (Jernej, 2026-10-18)
Jernej: "Does multi user work and how? Also for multi user, can it be done so
a person can share a note and that is done in a p2p fashion?"
Today: no multi-user. One sync account = one person's devices (password or
lockbox enrolment). Roadmap 11 (CRDT per note via yrs C FFI, E2EE relay,
presence) is not built; SYNC.md's lockbox section names the hook: the same
X25519 lockbox call with a collaborator's public key, scoped to one note's key.
- [x] Jernej's choices (2026-10-18): "Live co-editing"; reach: "LAN +
      tailscale, self hosted server in the future"; transport: "Share code
      with Hearth" (extract Hearth's link layer into a library both use).
- [x] Fable plan once chosen (scratchpad SHARE-PLAN.md; lanes below).

## Canvas alignment guides (Jernej, 2026-10-19)
Jernej: "also for base, make it so that it has the auto lign stuff, like if 2
squares are above eachother they shoiud get the align helpers". Read as the
canvas (free-placed cards; Bases has no free placement). Nothing snaps today.
- [x] Fable plan: guides and snapping to other cards' edges/centres while
      moving and resizing (Obsidian's "snap to objects"), snap to grid,
      toggles, zero cost when off; plus align/distribute for a selection if
      Fable judges it worth it.
- [x] Jernej: "clicking in a note should allow ot be scrolled in the canvas".
      As in Obsidian: once a card is clicked (selected/focused), the wheel
      scrolls that card's content instead of panning the canvas; an
      unselected card still pans. Same Fable plan.
      Done (lane/canvas, c1e0ee2): docs/CANVAS.md. Align/distribute skipped
      (Obsidian's canvas has none).

## From Jernej's screenshots, 2026-10-19
- [ ] Long lines in a code block wrap; demo/Code/Code Blocks.md ("A very long
      line") says they scroll sideways, and wrapping hides where a line ends.
      MarkdownEditor.cpp layout uses WrapAtWordBoundaryOrAnywhere for fences
      unless `#| wrap: false`. Fable plan: per-block horizontal scroll (or
      no-wrap + scrollbar) matching Obsidian, cost when no long lines = 0.
- [x] A ```rust fence with no `fn main` (a struct + impl, as in the demo)
      runs rustc and shows E0601 in red. Like Haskell (no main -> ghci): no
      main -> check only (rustc --crate-type=lib --emit=metadata; gcc/g++
      -fsyntax-only for c/cpp; java without main: javac), output "checked,
      nothing to run (no main)" on success, compiler errors as today.
- [ ] With note sharing (P2P), a block can come from someone else: keep the
      run manual, show who wrote a block before its first run on this
      device, never allow `sandbox: host` for a shared note. Goes into the
      sharing plan.
- [ ] Task states beyond [ ]/[x] (Jernej 2026-10-19: "custom stuff inside
      other than just ticking, like a line, or a straight up red X ... think
      about useful additions"). Today Parser.cpp's list head regex only knows
      `[ xX]`, so `- [-] a` is plain text "[-] a". Obsidian treats any one
      char in the box as a task (themes style them). Fable plan: a state set
      (cancelled line, red X, in progress, deferred, question, important,
      star...), drawing, strike/mute text, click + menu + command to pick a
      state, search (task-todo/task-done), Tasks/Dataview status compat,
      publish/PDF export, user-editable like callouts.ini, feature off = only
      [ ]/[x] and zero cost.
- [ ] Settings > Appearance: colour of each formatting kind, display only
      (Jernej 2026-10-19: "Note title, H1, H2, ..., bold, italics, underline
      (control both the color of underline as well as text, the same with
      strikethrough) etc. (this should be visual only, no actual formatting
      lines for color, just how its rendered)"). Nothing written into notes.
      Fable plan: which kinds (title, H1-H6, bold, italic, bold-italic,
      underline text + line, strike text + line, highlight, inline code,
      links internal/external/unresolved, tags, quotes, list markers, ...),
      Obsidian's CSS variable names (--h1-color, --bold-color ...) so themes
      and snippets map, reset per colour, live preview, dark/light each.
- [ ] Note embed inside running text (demo/Embeds/Note Embeds.md "An embed
      inside running text", `![[Target Note#Details]]` mid-paragraph) draws
      an empty card with only a file icon; the same embed on its own line is
      the reference. Should show the Details section as a block card, the
      paragraph's text before and after unbroken (Jernej screenshot
      2026-10-19).
- [ ] PDF slides embed: a click a little off "Next >" / "< Previous" opens
      the PDF instead (Jernej 2026-10-19: hit box should be larger around
      Next so a slightly-off cursor does not open it by accident, like
      Apple's keyboard). Today: 70 px rects padded 4 px
      (PdfEmbedBlock.cpp handlePdfEmbedClick), the slide cell above opens
      the PDF. Fix: nav row halves are the buttons, a dead band between the
      cell and the row, and for ~1 s after a nav click the last-used button
      also owns the cell's bottom strip (repeat clicks land).
- [ ] Zoom inside an embedded PDF (Jernej 2026-10-19: "allow for zooming in
      on PDFs when in embedded mode"). Fable plan; build after lane/pdfnav
      merges (same file, PdfEmbedBlock.cpp).

## Sharing lanes (plan: scratchpad SHARE-PLAN.md, Fable 2026-10-19)
Jernej's answers 2026-10-19: CRDT "try both" (yrs via cargo approved, and a
hand-rolled native C++ CRDT, same CrdtDoc interface, compare); member "first
device then person. just do device for now"; collaborator copy at
Shared/<title>.md.
- [ ] Lane H (Hearth): node.cpp off account.h, proto/link.proto, Node
      serviceType/prefix, tools/link-files.txt
- [ ] Lane Y: CrdtDoc interface + yrs backend (cargo, Cargo.lock), test_share_crdt
- [ ] Lane R: native CrdtDoc backend on the same tests + bench vs yrs
- [ ] Lane L: tools/vendor_link.sh, libs/ext/link, NOTEAPPC_WITH_SHARE
- [ ] Lane S: ShareEngine/Session/identity/store/frame, undo-preserving
      applyExternalEdit, disk fold-in
- [ ] Lane U: commands, ShareDialog, badge, approve prompt, feature switch
- [ ] Lane P: remote carets (optional)
- [ ] Later: person identity across devices; self-hosted relay
- [ ] Implicit code per note (Jernej 2026-10-19: "in the headers of a note
      there should be something called implicit imports, or implicit code
      ... something that is a part of every other codeblock. so that in
      every codeblock you dont need imports"). Frontmatter per-language
      prelude (imports, a main wrapper) applied to every runnable block of
      that language in the note; error line numbers mapped back. Fable
      plan; build after lane/nomain merges (DirectRunner.cpp).
- [ ] Demo notes for everything that has none (Jernej 2026-10-19: "Write
      PUML, tikzjax, and any other new stuff we added text files please").
      Haiku sweep found none for: mermaid, plantuml, tikz, graphviz, fence
      `#|` options, run options/kernels/input()/completion/sandbox/output
      kinds, .ipynb, encryption, publish, slides, pop-out windows, epub,
      image annotation, meta notes, bare brackets, vim, slash commands,
      spellcheck, look up, audio recorder; partial: run languages, runnable
      file embeds, search operators, footnote pane, graph, bookmarks, page
      preview. New files only (Jernej's demo edits stay unstaged).
- [ ] Graph view "insanely jiggly" (Jernej 2026-10-19). Suspects from a
      read: velocity keeps 92% per step (damping 0.92; d3 keeps 60%,
      velocityDecay 0.4) so springs ring; and anything that wakes the
      layout while idle (every index edit sets m_settled=false). Lane
      graphcalm: measure, fix, guard with tests.
- [ ] Settings: list of file types the sidebar file tree ignores (Jernej
      2026-10-19: "there should be a list of filetypes in settings that are
      ignored by the sidebar"). Lane hidetypes: files.hiddenExtensions,
      rows hidden in the tree only (search, links, quick switcher unchanged).
