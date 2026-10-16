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

A fenced block's language label sits at the right of its header. **Click it to
copy the listing** - without the fences, the language tag or any `#|` option
lines. A block with no language shows "Copy" while the pointer is over it.

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
can retune a token. Per-language and per-fence colours are a live-editor
feature and are not published.

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
any other text ends the group. The Copy button copies the tab being shown.
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
