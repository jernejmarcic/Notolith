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

- [ ] **Inline `$...$` maths never renders** - it is parsed and styled purple
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
- [ ] The three O(document) passes that still run on **every keystroke**:
      `MarkdownEditor.cpp` rebuilds the whole `SourceMap`, `shiftRanges` walks
      every block and decoration, and `updateLayout()` makes two more full
      passes. 0.26 ms at 200 kB and 0.66 ms at 1 MB, so it is nobody's
      problem yet - but it is the last thing in the editor that is
      proportional to the file rather than to the edit.
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

- [ ] **Inline `$...$` maths never renders.** Parsed and styled, never laid
      out. Needs an inline image run inside a `QTextLayout` line. The trick
      that avoids teaching DisplayMap to substitute text: hide all but one
      character of the formula and give that character an absolute letter
      spacing equal to the image width, then paint the image at its x.
- [ ] **The editor half of `Note > Heading` links.** The parser now records
      `anchorHash` and `anchorText`; DisplayMap still only hides, never
      replaces.
- [ ] `editor.spellcheck` has no dictionary behind it. hunspell is in PLAN.md
      and is not a dependency yet.
- [ ] A listing inside a callout wraps; Obsidian scrolls it.
- [ ] Property values other than the checkbox are still read-only.
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
- [ ] Scroll by blitting (`QWidget::scroll`) instead of repainting the whole
      viewport - the first thing to do before any Vulkan work.
- [ ] Measure the Wayland shared-memory buffer transfer the bench cannot see.
- [ ] Mobile groundwork: build the libraries against the Android kit's Qt
      6.7.3 to find any 6.8+ API; make KF6 SyntaxHighlighting optional the
      way poppler already is.
- [ ] Future `PKGBUILD` must override CachyOS's `-march=native`.

## Requested 2026-09-23, queued

Jernej's rule: a new feature goes here and the current task carries on.

### In flight

- [ ] **Inline `$...$` maths** - the lane stopped before the fallback for an
      unrenderable formula; `UnrenderableInlineMathKeepsItsSourceVisible`
      fails. Finish it before anything else in the editor lands.
- [ ] Lane: Nerd Font glyphs in `Icons::icon()`, and `LanguageStyles`
      (`codeblocks.ini`) mirroring `CalloutStyles`.
- [ ] Lane: navigation - leaving settings on any navigation, single-click
      folders, back/forward with mouse side buttons and Alt+arrows, caret and
      scroll remembered per note, reopen the last note, reveal the open note
      in the tree, Ctrl+, for settings.
- [x] `Settings.defaultsAreCompiledIn` read the real `~/.config/noteAppC` and
      failed once Jernej picked the OLED theme. Every settings test now runs
      against a private config directory.

### Editor, after the maths

- [ ] **Selecting a line reveals markdown syntax; Obsidian does not.** Today
      the whole block goes raw when the caret enters it
      (`Decorations::hiddenFor`), even while a selection is being dragged. Two
      rules: never reveal during a non-empty selection, since text shifting
      under the mouse mid-drag is the worst version of this; and with a caret,
      reveal only the markers of the element the caret is inside, not the
      whole block.
- [ ] **Code blocks use the callout style format**, in Jernej's words "the
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
- [ ] **Code block tabs.** Adjacent fences, with only blank lines between,
      that each carry a `tab` attribute group into one block with a tab strip:
      `` ```python tab="Python" `` then `` ```cpp tab="C++" ``. A bare `tab`
      labels the tab with the language's style name. Any other text between
      them ends the group. Obsidian shows them as separate blocks, unchanged.
- [ ] **Callout gaps**, all verified 2026-09-23:
      1. a remote `https://` icon never appears until something else repaints:
         nothing listens to `Icons::notifier()`'s `iconReady`;
      2. editing `callouts.ini` needs a restart - it is read once, and a
         comment in `MarkdownEditor.h` wrongly says it reloads on theme change.
         Watch the file, and `codeblocks.ini` with it;
      3. nested callouts cannot fold - only the outermost has a handle;
      4. a listing inside a callout wraps where Obsidian scrolls it;
      5. Nerd Font glyph icons (arrives with the icon lane).

### Next, after tabs

- [x] **The word count was O(document) on every keystroke** - the shell
      recounted the whole note on `textChanged`, while the parser already
      counted it on the worker thread and the editor threw that away. It now
      comes with each parse. Keystroke through the window, 200 kB: 0.33 ms ->
      0.21 ms, which also shows the app-wide event filter costs nothing
      measurable.

### Later

- [ ] **Latent: decorations other than syntax, styles, blocks and properties
      are not shifted between a keystroke and the reparse** - callouts,
      maths, embeds, list items, tables, code fences. Invisible today because
      blocks below an edit keep their cached layout; it would show if such a
      block were laid out inside the 20 ms debounce. Fixing it adds to the
      per-keystroke O(document) passes, so do it together with making those
      passes proportional to the edit. `ShiftTest` guards the visible case.

- [ ] **Custom themes from a CSS file** (lane running). Corrected: a picker
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
- [ ] The actual Android build, and a touch mode for the editor.
- [ ] A 5 000-line fenced code block is still one block, so typing in it
      relays every line, like lists did before they were split. Measure it.
- [ ] Mobile groundwork, the `-march=native` PKGBUILD guard, scroll by
      blitting, the Wayland buffer measurement - see above.
