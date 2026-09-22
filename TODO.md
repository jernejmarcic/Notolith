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
