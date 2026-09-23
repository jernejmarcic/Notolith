---
title: noteAppC work in progress
type: todo
machine: starbase-1
updated: 2026-09-22
---

# TODO

`HANDOFF.md` is the cold-start document and the long list. This file is the
work in flight.

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
- [ ] Measure against Obsidian itself on the same vault. Not installed here,
      so the comparison is still unmade - these are noteAppC's own numbers.
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
- [ ] How Obsidian displays a block reference `[[Note#^id]]` is not recorded
      anywhere checked; shown as written until someone looks.
- [x] Spellcheck: the first Tier 1 module, `libs/ext/spell/` (hunspell),
      behind `NOTEAPPC_WITH_SPELLCHECK`. Wavy underline on prose (not code,
      links, tags, maths, or the word being typed), right click for
      suggestions and "Add to dictionary" (`~/.config/noteAppC/dictionary.txt`).
      Dictionaries follow the system languages and load after the first paint.
- [ ] **Jernej: starbase-1 has no hunspell dictionary**, so spellcheck shows
      a status message and underlines nothing. `sudo pacman -S hunspell-en_gb`
      for the en_GB locale. Slovenian is not in the Arch repositories; its
      `sl_SI.aff`/`.dic` from LibreOffice's dictionaries work dropped into
      `~/.local/share/hunspell/`, with `sl` added to `LANGUAGE`.
- [ ] **Verify first**: a listing inside a callout wraps here. The claim
      that Obsidian scrolls it was mine and unchecked; the session notes say
      Obsidian's live preview wraps too, and only its reading view is thought
      to scroll. Nothing to change until someone looks.
- [x] Property values: text, number, date and date-time edit in place (click
      the value; Enter or clicking away keeps it, Escape drops it). Written
      through yaml-cpp, quoted only when needed, and refused when it would
      change the type or touch anything else on the line.
- [x] List properties: a chip's cross removes it, the space after the chips
      adds one. Rewritten in the style written (block or flow), other items
      kept byte for byte; a list with a comment in it stays read-only.
- [x] Editing an existing list item in place: click its text.
- [ ] Measure against Obsidian itself. Not installed here.

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
- [ ] **Callout gaps**, all verified 2026-09-23 - 1, 2, 3 and 5 done; 4 open:
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
- [ ] The actual Android build (Sonnet lane running 2026-09-23: arm64 debug APK
      with the workspace kit, CMake and docs only), and a touch mode for the editor.
- [x] **Typing in a long code block was over budget.** Release, keystroke
      plus paint: 100 / 1 000 / 5 000 lines went 2.7 / 26 / 158 ms ->
      1.10 / 1.10 / 1.16 ms. Fences over 32 lines are 32-line blocks, the
      highlighter carries `CodeHighlighter::State` piece to piece and stops
      once it converges, and its formats are cached per Kate format. Written
      up as fix 10 in `docs/PERFORMANCE.md`; guarded by `CodeBlockPerfTest`
      and `CodePieceTest`.
- [ ] A long listing inside a callout is still part of one layout.
- [ ] **Verify first: does the caret in a code block show its fences?**
      Here it does not - md4qt's code block covers the listing alone, so the
      two fence lines sit in the gap blocks either side and are revealed only
      with the caret on them, which also means the language tag can only be
      edited from its own line. I believe Obsidian shows both fences as soon
      as the caret is anywhere inside; check before changing anything.
- [ ] Mobile groundwork, the `-march=native` PKGBUILD guard, scroll by
      blitting, the Wayland buffer measurement - see above.

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
      6.4 ms, a rare one 0.05 ms. Left: `[property:value]` syntax; `block:`
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
      a note, read a version, copy it or restore it. No "show changes" diff
      yet.
- [x] Page preview: the linked note rendered in a popover, opened at the
      heading or block the link names; Ctrl held while editing, plain hover
      in reading view, as Obsidian. Links inside it can be followed.
      Notes only for now - Obsidian also previews images and PDFs.
- [x] Audio and video embeds: a player card (play/pause, seek bar, clock;
      video frames painted into the block), Qt Multimedia optional behind
      `NOTEAPPC_WITH_MEDIA` - without it a click opens the file. No poster
      frame before play yet.
- [x] `query` blocks, on the wave 1 search (lane F): results, match counts
      and highlighted snippets in the note, off the UI thread, cached per
      query, refreshed on index changes; a result opens its note at the
      match. 0.10 ms per unrelated keystroke with one on screen.
- [ ] Obsidian frames an embedded search with a header ("N results",
      collapse, sort); the block shows the results alone.
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
- [ ] HTML left out: gradients are a flat first stop, borders one colour
      and no radii, list markers always decimal.
- [x] `canvas:new-file` ("Create new canvas"): an empty `{}` Untitled.canvas.
- [ ] A new-base command: Obsidian's id is not in the 1.1.15 list, find it.
- [ ] ` ```base ` blocks inside notes, on `BaseView`.
- [ ] Bases left out: `list.filter/map/reduce`, link/file/image values,
      `summaries:`, list-property cells, cards read-only, sort not saved back.
- [ ] Canvas left out: dragging a group does not move what is in it; a file
      node's subpath does not scroll to the heading; plain drag pans, Obsidian
      rubber-bands (Shift here).
- [x] Middle-click a tree row to open it in a new tab.
- [ ] **For Jernej:** Obsidian opens a link in a new tab on Ctrl+click, but
      in noteAppC's editing view Ctrl+click is how a link is followed at all
      (a plain click places the caret). Which should Ctrl+click be?
- [x] Only the focused split's tab is accented, as in Obsidian.
- [ ] One note edited in two splits at once diverges; last save wins.
      Obsidian edits one document from every view.

### For Jernej to decide
- [ ] **A licence.** None is chosen; the PKGBUILD says `LicenseRef-unknown`
      and the AppStream file states none. Bundled: md4qt and MicroTeX (MIT),
      tinyxml2 (zlib), Lucide icons (ISC); poppler (GPL) is linked when on.
- [ ] **An app id** for Flatpak, AppStream and Android:
      `io.github.jernejmarcic.noteAppC`, following basket's, unconfirmed.
- [ ] **An app icon.** The desktop file borrows `accessories-text-editor`;
      drawing one is image making, which is off unless you say otherwise.
- [ ] **Autosave.** Obsidian saves on its own a moment after typing stops;
      noteAppC asks on close ("Save changes to X?"). Tabs make that prompt
      come more often. Autosave like Obsidian?

### Wave 4
- [x] `PKGBUILD` without `-march` (`packaging/arch`): built here into /tmp,
      its `check()` passes all suites under makepkg's flags. AppStream
      metainfo installed and validated.
- [ ] AppImage and Flatpak recipes. Neither can be built here without
      installing tools (flatpak-builder, linuxdeploy), which is yours to do.
- [x] A local check script in place of CI (there is no remote):
      `sh tools/check.sh [--bench]` - the release build and all suites (perf
      guards and the new seeded round-trip fuzz among them), then a build
      with every optional module off and its suites.
- [ ] The Android build - lane G running in `/tmp/noteAppC-android`.
- [x] Touch mode, first part: momentum scrolling where a touchscreen
      exists (a mouse drag still selects), long-press selects a word, both
      sidebars fold below 720 px and come back.
- [ ] Touch mode, rest: selection handles, touch-sized hit targets (task
      boxes, fold arrows, chips), sidebars as drawers over the note.

