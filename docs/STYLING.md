# Styling callouts and code blocks

Callouts and code blocks share one style system: named styles in a config file,
overridable on any single block, written so a note still opens in Obsidian. Both
files live in `~/.config/noteAppC/` (or under `$XDG_CONFIG_HOME`), both are
optional, and both are reread the moment they change - no restart.

## The shared rules

- **Three layers**, each overriding the one before: what is built in, the
  file's section for that type or language, then what the block itself says.
- **Colours are `#RRGGBBAA`**, alpha last as in CSS. `#RGB` and `#RRGGBB` work
  too, as do `rgb()`, `rgba()`, `hsl()` and named colours. Note that Qt on its
  own reads eight hex digits the other way round, as `#AARRGGBB`; noteAppC does
  not.
- **Icons** take any of four forms, always drawn at one size:
  - a [Lucide](https://lucide.dev) name: `zap`, `pencil`, `circle-help`;
  - a Nerd Font glyph, as `U+E73C` or the character itself - drawn with the
    first installed font whose family name contains "Nerd Font", and left out
    entirely if there is none, rather than drawn as a box;
  - a local file, as a path or `file://` URL: `.svg`, `.png`, `.jpg`, or
    `.webp` where Qt's image formats plugin is installed (`qt6-imageformats`);
  - an `https://` URL, fetched once into `~/.cache/noteAppC/icons` and shown
    when it arrives. It never holds up drawing the note.
- **The INI format**: one `[section]` per type or language, `key = value`,
  `;` or `#` for a comment line, ` ; comment` after a value, and double quotes
  around a value that needs them. Section and key names are case-insensitive.
- **`inherits = other`** makes a section start from another one.

## Callouts

The Obsidian form works unchanged:

```markdown
> [!note] A title
> Body.

> [!warning]- Folded until clicked
> Body.
```

`~/.config/noteAppC/callouts.ini` styles a type - one of Obsidian's thirteen,
or a new one of your own:

```ini
[deadline]
icon = alarm-clock
bar = #FF5A5Fff          ; the rail down the left edge
bg = #FF5A5F1A           ; the panel
heading-font = Inter
heading-size = 15
heading-color = #FF9A9Eff
heading-style = bold
font = Iosevka
size = 14
color = #E6E6E6ff        ; the body text
style = italic

[calm]
inherits = deadline
bar = #4C8DFFff
style = normal           ; clears what deadline set
```

`style` and `heading-style` take any of `bold`, `semibold`, `light`, `italic`,
`underline`, `strikethrough`, in any order; `normal` clears them.

One callout can override its style in the slot after the type, which Obsidian
already parses and ignores - so the note still opens there, just unstyled:

```markdown
> [!deadline|bg=#00FF0022 icon=flame] Just this one
> Body.
```

The `:::` container takes the same keys in braces:

```markdown
::: {.deadline bg="#00FF0022" title="Just this one"}
Body.
:::
```

Callouts nest, each with its own style, and a nested one folds with `+` or `-`
like any other. An empty `>` line is what steps back out of a nested callout:
without it, CommonMark joins the next `>` line to the nested one's paragraph.

## Code blocks

A fenced block with a language has a header strip that names it: the
`#| title:` if it has one, else its style name - the language as written, or
the `name="..."` the fence gives - with the style's icon in front. A block in a
tab group has its tabs for names instead. At the right of the strip, **"Copy"
appears while the pointer is over the block; click it to copy the listing**
(without the fences, the language tag or any `#|` option lines). A run control
sits beside it, only for a language something can run (a tool installed, or a
Jupyter kernel); a block with no language has no strip and shows "Copy" in its
corner.

The caret in a tab group, or Source mode, shows the plain text instead: no
strip, panel or run control, the listing still highlighted.

`~/.config/noteAppC/codeblocks.ini` styles a language:

```ini
[python]
name = Python            ; the label; default is the language as written
icon = U+E73C
color = #3776ABff        ; the label and its icon
aliases = py, python3    ; other spellings that mean this section

[cpp]
name = C++
icon = U+E61D
aliases = c++, cxx
```

With no section, a language is labelled exactly as written and has no icon.

One block can override that on its fence, after the language. CommonMark takes
only the info string's first word as the language, so Obsidian still
highlights it:

```markdown
    ```python name="Setup script" icon=U+E73C color=#F4B400ff
    print("configure")
    ```
```

### Colours

A block's panel, plain text, border and token colours are the same kind of
knob as the label colour: set them per language in `codeblocks.ini`, or on one
fence.

```ini
[python]
bg = #1E2A3Aff                    ; panel fill       (default: the theme's code background)
fg = #D8DEE9ff                    ; plain code text  (default: the normal text colour)
border = #3776AB80                ; 1 px panel border (default: none)
syntax.keyword = #C678DD
syntax.string  = #98C379 italic
syntax.comment = #5C6370 italic
syntax.function.builtin = #61AFEF bold
```

```markdown
    ```python bg=#112233ff fg=#eee border=#3776AB syntax.keyword="#C678DD bold"
    print("configure")
    ```
```

A `syntax.*` value is a colour followed by any of `bold`, `italic`, `underline`
(a colour is optional, so `italic` alone keeps the highlighter's colour). No
size or font: the monospace metrics never change. `inherits` carries these
keys too, and a fence's keys win over its section's.

**Capture names.** Each token gets a dotted capture name from the highlighter,
and takes the style of the *longest dotted prefix* some key names:
`function.builtin` beats `function` for a built-in, `function` covers it when
there is no `function.builtin`, and `functionx` never matches `function`.
The names Kate's highlighter produces:

| capture | Kate style | | capture | Kate style |
|---|---|---|---|---|
| `keyword` | Keyword | | `number` | DecVal, BaseN, Float |
| `keyword.control` | ControlFlow | | `constant` | Constant |
| `operator` | Operator | | `comment` | Comment, CommentVar |
| `function` | Function | | `comment.doc` | Documentation |
| `function.builtin` | BuiltIn | | `type` | DataType |
| `function.special` | Extension | | `namespace` | Import |
| `variable` | Variable | | `attribute` | Attribute, Annotation |
| `string` | String, VerbatimString | | `preproc` | Preprocessor |
| `string.special` | Char, SpecialChar | | `label` | RegionMarker |
| `string.regex` | SpecialString | | `hint` `warning` `error` | Information, Warning, Alert/Error |
| `primary` | Others | | | |

A tree-sitter backend would feed its `highlights.scm` capture names into this
same map, so a config written today keeps working when one is added; there is
none yet, since Kate already covers 300+ languages without a new dependency.

**A theme's defaults.** The layer under a block's own keys is the theme's. A
`.json` scheme may carry a top-level `syntax` object (and one inside `dark` or
`light`) with the same dotted keys, Zed's shape:

```json
{ "name": "Mine", "syntax": { "keyword": "#C678DD bold", "function.builtin": "#61AFEF" } }
```

An imported Obsidian theme lands here too: `--code-keyword`, `--code-string`,
`--code-comment`, `--code-function`, `--code-value` (numbers),
`--code-property` (variables), `--code-operator`, `--code-punctuation` and
`--code-tag` are read as the same-named captures. Resolution is layer by layer,
nearest first: the fence, then the `codeblocks.ini` section, then the theme,
and within a layer the longest prefix; whatever none of them names keeps the
highlighter's colour.

A listing nested in a callout or quote takes the language's section and the
theme, but has no fence of its own to carry attributes.

**Switching it off.** `Code block colours` on the settings page
(`editor.complexCodeBlocks`, on by default) off draws every block as if none of
this were configured. With nothing configured it makes no difference at all.

**Publishing.** A site built from a theme that names token colours gets a
`--code-<capture>` CSS variable and a `.tok-<capture>` rule per token kind the
page uses (dots become dashes: `.tok-function-builtin`), so a page's own CSS
can retune a token. The colours come from the vault's active scheme
(`appearance.scheme`), read once per mode on the publishing worker (dark and
light), so `Publish: Export the vault as a website` carries the same token
colours the editor shows; with a built-in scheme, which names none, the page
keeps the highlighter's own colours. Per-language and per-fence colours are a
live-editor feature and are not published.

**Publishing footnotes.** `[^1]` and inline `^[...]` references link to their
definition in the list at the foot of the page. Every reference has its own id
(`fnref-1`, `fnref-1-2`, ...), so ids stay unique, and each definition ends
with one text back-arrow per reference (`&#8617;&#xFE0E;`, never an emoji).
References inside an embedded note stay plain numbers: its list is not on the
page.

### Tabs

Two or more fences in a row, each marked `tab`, become one block with a tab
strip:

```markdown
    ```python tab
    import noteapp
    ```

    ```cpp tab="C++"
    #include <noteapp.h>
    ```
```

A bare `tab` takes the language's name and icon from `codeblocks.ini`;
`tab="..."` names it yourself. Only blank lines may come between the fences -
any other text ends the group. The Copy button copies the tab being shown. The
shown tab is drawn attached to its listing; with the caret anywhere in the group
every member is plain source (see "Code blocks").
**Publishing.** A group publishes as a tab strip that needs no JavaScript: one
radio input, label and panel per tab (`.code-tabs`, `.code-tab-input`,
`.code-tab-label`, `.code-tab-panel` in `site.css`), the first tab selected,
each label named by `tab="..."`, else the language as written, else "Tab N".
Keyboard arrows move between the tabs (they are a radio group); a lone `tab`
fence publishes as an ordinary block.
With the caret inside the group every fence shows as written, so you can edit
any of them. Obsidian shows the same note as separate blocks.

## Themes

Pick a colour scheme on the settings page's Appearance tab. A theme file goes
in `~/.config/noteAppC/themes/` (or under `$XDG_CONFIG_HOME`), or in a vault's
own `.obsidian/themes/` or `.obsidian/noteAppC-themes/` to travel with that
vault. Three forms are read from any of those directories:

- **A noteAppC scheme**: `Mine.json`, a `dark` and/or `light` object of token
  overrides - only the tokens a scheme wants to change from the built-in
  theme, keyed exactly as the CSS tokens below.
- **A loose CSS file**: `Mine.css`, named after the file and read the same way
  as an Obsidian theme's CSS below.
- **An installed Obsidian theme**: `Mine/theme.css`, the folder Obsidian
  itself installs a community theme as.

Either CSS form maps onto noteAppC's colour tokens only - every `--name:
value` a `:root`, `body`, `.theme-dark` or `.theme-light` block declares,
colours normalised and `var()` resolved, plus the three `--accent-h`/`-s`/`-l`
numbers Obsidian's own accent picker writes. A font, a size, a layout rule -
anything that is not a colour - is dropped rather than guessed at. A noteAppC
`.json` scheme is not filtered the same way and may set a typography token
directly. Either way a theme's layout does not come across, and cannot: Qt
Widgets are styled with QSS, not CSS, and QSS has no equivalent for an
Obsidian theme's layout.

Like `callouts.ini` and `codeblocks.ini`, a theme reloads the moment its file
changes - edit the selected scheme and the window restyles with no restart -
and adding or removing a file in a themes directory updates the settings
page's list while it is open.

## Popups, the status bar and the inline title

Jernej, 2026-10-16: "the general design is fine, I just want refinements".

- **Rounded popups.** A QSS `border-radius` on a menu, a tooltip or the `[[`
  suggester only rounds the frame; the popup window is still a rectangle.
  `libs/theme/PopupStyle` (Fusion in a `QProxyStyle`, installed by the app and
  by `noteAppC_shot`) makes every popup-type window frameless and translucent
  when it is polished, so one place covers menus, combo lists, tooltips, the
  suggesters and the quick switcher, the ones made later too. A plain `QDialog`
  popup needs its styled background asked for back, which the style does.
  Shadows are not drawn: a translucent popup has no window-manager shadow.
- **Fields.** Inputs on the settings page and in dialogs are rounded fields
  with a 1px border that turns accent on focus; combo lists and spin buttons
  follow. Buttons show an accent border on focus. All from tokens.
- **The status bar** reads like Obsidian's: backlinks, properties, the mode
  icon, words, characters. Backlinks opens the backlinks pane; properties
  scrolls the note to its properties. The pencil (or book, or code) icon is the
  current mode; its menu has Reading, Source mode and Live Preview with a check
  at the right edge (drawn by `PopupStyle`, which puts a check there for every
  checked item of every menu: QSS styles a menu item itself and its
  `QMenu::indicator` can only sit left of the icon). The word and character counts come with each parse and
  cost nothing per keystroke; the backlink count is one indexed query, a
  moment after the index changes. Words and characters follow the `word-count`
  core plugin switch.
- **The inline title** is a line edit styled as the heading: click to put a
  caret in it. Enter or leaving it renames the note through the same path as
  the file tree (links, history, open tabs, index), Esc or a refused name puts
  the old one back, Down or Enter moves into the note. A name with a `/` or
  another character Obsidian refuses, a leading dot, nothing, or one a note
  already has is refused with a status-bar notice. A note outside a vault is
  not renamed from here. `noteAppC_shot` takes `--popup <menu name>`,
  `--tooltip <text>` and `--keys <text>` to show these, and
  `NOTEAPPC_SHOT_SCREEN=1` for the window system's own composite.
