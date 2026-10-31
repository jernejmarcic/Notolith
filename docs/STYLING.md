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

## Task checkboxes

With the "Checkbox states" core plugin on (docs/EDITOR.md lists the built-in
states), `~/.config/noteAppC/tasks.ini` changes or adds states (the index command and
publishing read it too, with no editor open). One section per
state; a section named for a built-in state (`cancelled`, `dropped`,
`in-progress`, `forwarded`, `scheduled`, `question`, `important`, `star`, `idea`,
`pro`, `con`) changes it, any other name adds one:

```ini
[cancelled]
glyph = cross             ; the dash becomes a red X's shape

[waiting]
char = w                  ; exactly one character
type = todo               ; todo | in-progress | done | cancelled | non-task
glyph = char              ; box | tick | half | line | cross | arrow-right | arrow-left | star | char
color = --color-purple    ; a theme token or any CSS colour
text = mute               ; strike | mute | both | none
next = " "                ; what a click writes; quote a space
```

Rules, so a hand-edited file cannot break a note: a section with no valid
`char` that is not a built-in's name is ignored; `char` is one character that is
not a space, `[`, `]`, `\`, `x` or `X`; an unknown `type`, `glyph` or `text` word
keeps the state's value (a new state's are todo, `char` and none); an unparsable
`color` draws in the accent; `next` that is not one character is `x`; `[todo]`
and `[done]` are ignored (`[ ]` and `[x]` are Obsidian's own). Two sections
claiming one character: the one whose name sorts last wins, and it takes the
character from a built-in state. Names are lower-cased, values are not, so
`char = I` and `char = i` are two states.

The file reloads when it changes, like `callouts.ini`. The command-line tools
(`publish_cli`, `index_cli`) parse without the option, so `[-]` is text to them.

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

### Code Styler syntax

Obsidian's [Code Styler](https://github.com/mayurankv/Obsidian-Code-Styler)
plugin puts its parameters on the opening fence line; a note written for it
renders the same here. The core plugin **Advanced code blocks** (Settings >
Core plugins, on by default) reads them. Off, they are the plain text they are
in Obsidian without the plugin, and the parse never looks for them.

```markdown
    ```cpp title:"long filename.cpp" ln:27 hl:2,4-6 fold
    ```python unwrap:false
    ``` fold:"Collapsed listing"
```

| parameter | does |
|---|---|
| `title:x`, `title:"a b"`, `title:'a b'` | the header strip's name (`:` and `=` are the same; any order) |
| `ln:true`, `ln:false`, `ln:27` | line numbers, from 27 for a number |
| `hl:...` | highlights lines: `hl:1,3-4,foo,'bar baz',"bar and baz",/#\w{6}/` - numbers, ranges, a word, quoted text, a regular expression (PCRE, not JavaScript's flavour), comma-separated with no spaces. `{1,3-4}` after the language is the same with numbers only |
| `fold`, `fold:Folded`, `fold:"Collapsed"` | folded when the note opens; the text names the block while it is folded and has no title |
| `unwrap`, `unwrap:true`, `unwrap:false`, `wrap` | long lines scroll sideways (the default here) or wrap |
| `ignore` | the block keeps its panel and colours and loses the header strip |
| `ref:[[Note]]`, `reference:[[Note\|alias]]` | with no `title:`, the link's text is the title |
| `info:2 warn:4-6 error:'boom'` | named highlights, below |

**Named highlights.** A name is an alternative `hl`: it takes the same items
(numbers, ranges, words, quoted text, `/regex/`) and tints its lines in a colour
of its own, so `cpp info:2 warn:4-6 error:8` marks three kinds of line. The
names and colours are Settings > Editor > "Named highlights", a comma list of
`name=colour`: a theme colour (`blue`, `red`, `orange`, `yellow`, `green`,
`cyan`, `purple`, `pink`, each following the theme and so light and dark) or a
hex code (`#E5A100`, or `#E5A10040` with its own alpha). The default is
`info=blue, warn=orange, error=red, add=green, del=red`; the plugin has no
defaults of its own, these are the README's three plus the two a diff wants.
A word that is not in the list is not a parameter. A published page colours the
five default names (`hl-info` ... `hl-del` in `site.css`); other names are
classes a stylesheet of your own can colour.

With no language, leave a space after the fence (``` ` ``` fold title:x ```):
the first word is then a parameter. Without the space it is the language, as
CommonMark reads it. The parameters are hidden outside the caret's block, like
the language tag.

**Together with `#|` lines.** Both spellings fill the same options. When a key
is given both ways the `#|` line wins, being inside the block and the form
noteAppC wrote first; `#| highlight:` also reads text and `/regex/` items while
the plugin is on.

**Regular expressions** (`hl:/re/`, the named kinds, a reference's `start:` and
`end:`) run with a match budget, so a catastrophic one (`(a+)+$`) simply
matches nothing instead of stalling the editor; a pattern longer than 256
characters, or one starting with `(*`, is ignored.

**Differences from the plugin.** `unwrap:inactive` unwraps and does not wrap
while the mouse is held down. The highlights do not cover the line numbers, and
there is no gradient. `ref:` shows the link's text but the title is not
clickable. `{r title, hl=5}` (R Markdown) is not read: its first word is
`{r`, which CommonMark takes as the language. The plugin's settings
(themes, icons, language borders, excluded languages) are not read; the theme
and `codeblocks.ini` do that job. `code-styler-ignore: true` in a note's
properties turns the plugin off for that note.

### Inline code in a language

Part of the same core plugin: inline code that starts with `{language}` is
coloured as a code block of that language is.

```markdown
Use `{python} sorted(x, key=len)` or `{c++ title:'Inline' icon} std::move(x)`.
```

The `{python}` and the space after it are hidden, and come back while the caret
is in the span (like the `**` of bold). `title:` and `icon` after the language
are accepted and hidden but drawn as nothing: there is no label or icon before
inline code here. Inline code is everywhere, so the prefix is recognised
narrowly: the `{` is followed at once by a letter, the word is a language the
highlighter has (`python`, `py`, `c++`, `js`, `rust`, `sql`, `bash` ... and
their usual aliases), and nothing but `title:...` and `icon` follows it inside
the braces. `{0} items`, `{name} is required`, `{ x + y } * 2`, `{"a": 1}` and
`{mylang} code` are all plain inline code. Code that really starts with the
braces of a language takes an empty `{}` in front, which is all that is
hidden: `` `{}{python} x` `` shows `{python} x`. Published pages and PDF export
draw it the same way.

### File references

Part of the same core plugin: a fence whose language is `reference` shows a
file, or part of one, as a highlighted listing with the file's name as its
title.

```markdown
    ```reference
    file: ./src/main.py
    lang: python
    start: 10
    end: +20
    ```
```

The block's own lines are `key: value`. The target is `file`, `path`,
`filePath` or `link`: a path beside the note (`./x.py`) or from the vault's
root (`@/x.py`), a wikilink (`[[x.py]]`), a bare name the vault knows, or an
`http(s)` URL. **Only files inside the vault are shown, or opened by a click**:
the path is resolved with symlinks followed, and anything that ends up outside
(an absolute path, `../`, `@/../`, a symlink pointing out) is refused with
"Outside the vault" in the block and is never read. Copy a file in to show it.
`language` (or `lang`) overrides the language the file's
suffix names. `start` and `end` are a line number, a word (the first line
containing it) or a `/regex/` (the first line it matches, PCRE); `end` is
searched from `start` on and may be `+10`, ten lines past `start` (so lines
980 to 990 for `start: 980`, `end: +10`). With a range the title is
`name.py:10-30`. At most 500 lines are shown, with a last line saying so.

A click on the title opens the file: a note in a tab, anything else in the
system's own program, a URL in the browser. A click anywhere else puts the
caret in the block, which then shows as the lines above. A vault file is read
off the UI thread (the block says "Loading..." until it lands) and read again
when it changes on disk. A file that is not there says so in the block.

**Remote files.** A `link:` URL is fetched only while Settings > Editor >
"Fetch remote references" is on, and **that setting is off by default**: the
block then shows a notice and the title still links to the URL, and nothing is
sent anywhere. Chosen over on-by-default because a note someone sent you could
otherwise make your machine request any address; vault files are the
feature's main use and need no network. When on, the fetch is asynchronous
(15 s limit, 2 MB cap, at most 5 redirects, none from https to http), and a
copy is kept for five minutes (32 at most) for every note in the session, the
old one shown while the next one comes. **Addresses that are not the internet
are refused**: the host is resolved first, and a loopback, link-local (cloud
metadata), private, unique-local, shared (100.64/10), multicast or unspecified
address, an IPv4-mapped, IPv4-compatible (`::a.b.c.d`), NAT64 (`64:ff9b::/96`) or 6to4 one included, shows "is a local address: not fetched"
in the block. Every redirect is checked the same way. A name that answers
differently to the second lookup (DNS rebinding) is not caught. The title
shows the host (`name.py (example.com)`). Turning the setting off, or the
plugin off, forgets what was fetched and cancels what is in flight; the
excerpt is cut on a worker (the block says "Loading..." until it lands; a
`start:`/`end:` pattern is tried on the first 50 000 lines only) and coloured
once per file, range and theme, and a line over 1000 characters is cut with an
ellipsis. Shared-range (100.64.0.0/10, Tailscale) hosts stay refused. GitHub `.../blob/...` and GitLab `.../-/blob/...` links
are fetched as the raw file.

**Differences from the plugin.** The plugin downloads remote files into the
vault's plugin folder; this one keeps them in memory only, so nothing is left
on disk, and shows no repository, version or update-button in the header. The
file name is the title and `title:` on a reference block is not read. No line
numbers are drawn, the range being in the title; long lines wrap rather than
scroll. PDF export shows the same listing; a published page keeps the block as
its `key: value` lines.

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

## Formatting colours

Settings > Appearance, under the fonts, has a row for each kind of text, with a
swatch for dark and one for light: note title, headings 1 to 6, bold, italic,
underline, strikethrough, done task text, highlight, inline code, links
(internal, external, unresolved), tags, quote text, list markers, checkboxes
(fill, tick, border) and footnote marks. Underline, strikethrough, highlight,
inline code and the links have a swatch per part - text and line, text and
background. Pick a colour and the open notes change at once; a dashed swatch is
"follow the theme", and Reset on a row or "Reset all formatting colours" goes
back to it. The colours are kept in the user's `settings.ini` (`[appearance]
bold-color-dark = #ff0000`), never in a note and never in a vault: they are how
text is drawn, not part of what was written. Black (OLED) uses the dark
swatches.

They are ordinary theme tokens, so a scheme or an Obsidian theme's CSS can set
them too, and "precedence" is the settings first, then the scheme or CSS file,
then the built-in colour:

| Kind | Token(s) | Unset |
|---|---|---|
| Note title | `--inline-title-color` | the text colour |
| Heading 1 to 6 | `--h1-color` to `--h6-color` | the text colour |
| Bold, italic | `--bold-color`, `--italic-color` | the text colour |
| Underline (`<u>` only) | `--underline-color`, `--underline-decoration-color` | the text colour |
| Strikethrough (`~~`, `<s>`, `<del>`) | `--strikethrough-color`, `--strikethrough-decoration-color` | the text colour |
| Done task text | `--checklist-done-color` | `--text-muted` |
| Highlight (`==`, `<mark>`) | `--text-highlight-fg`, `--text-highlight-bg` | the page, on yellow |
| Inline code (`<code>`, `<kbd>`) | `--code-normal`, `--code-background` | the text colour, on `--background-primary-alt` |
| Links | `--link-color`, `--link-external-color`, `--link-unresolved-color` | the accent, the accent, the dimmed accent |
| Tag | `--tag-color` | the accent |
| Quote text | `--blockquote-color` | the text colour |
| List markers (bullets, numbers) | `--list-marker-color` | `--text-muted` |
| Checkbox | `--checkbox-color`, `--checkbox-marker-color`, `--checkbox-border-color` | the interactive accent, white, the border |
| Footnote marks | `--footnote-color` | the accent |

All but `--text-highlight-fg`, `--footnote-color` and the two `-decoration-`
tokens are Obsidian's names. Obsidian has no variable for a footnote, nor for
the text colour of a highlight, and none for the colour of an underline or a
strikethrough line (its `<u>` and `<s>` draw in the text colour), so those four
use the same spelling, `-decoration-color` meaning the line.

- **Unset means inherit.** Bold, italic, headings, quotes, underline and
  strikethrough have no built-in colour on purpose: a concrete colour on a bold
  word would paint over the block around it, a bold word in a callout or a
  heading would turn the plain text colour. They take a colour only when one
  is set. The kinds that already had a colour (links, tags, code, ...) keep it
  until set, so adding a swatch changes nothing about how a note looks.
- **Nesting.** A colour set on an inner kind wins over the block's: a bold
  word in a heading with its own colour is the bold colour, a link in a
  heading the link's. Bold italic (`***x***`) takes the bold colour.
- **Underline and strikethrough lines.** The line colour is separate from the
  text colour. An underline's is native (`QTextCharFormat::setUnderlineColor`).
  Qt draws a strikeout in the text colour only, so with a line colour set the
  strikeout is switched off and the editor draws the line itself, over the
  text, from the format's `kStrikeLineProperty` (`paintStrikeLines`,
  `MarkdownEditor.cpp`). Markdown has no underline; only `<u>` is underlined.
- **Live update.** A change that touches only these tokens lays out again the
  blocks on screen and nothing else: heights and the scroll position stay.
  (`QTextLayout::setFormats` throws away a layout's lines, so a colour change
  cannot recolour an already laid out block in place.) Any other change to the
  theme - a size, a font, the accent, the text colour - is a full relayout as
  before.
- **Publish and PDF.** The published site's `site.css` declares a token only
  where a scheme or the settings set it, and styles `strong`, `em`, `del`, `u`,
  `mark`, headings, inline code, links, tags, quotes, list markers and done
  tasks from them, falling back to what the page did before; light and dark
  each get their own. A PDF export draws in the light theme with the person's
  light colours on it.
- Not coloured: comments (`%%`), which nothing draws differently, and math,
  which is one image in the text colour.

## The file explorer

The sidebar needed a better look and feel, especially the gap on the left
being lit up.

- **One pill per row.** `QTreeView` paints an item's background over the indent
  columns left of the row as well, and `show-decoration-selected: 0` only picks
  which colour fills them, so a QSS `::item:selected` or `::item:hover`
  background always lit the gutter. Those two states are `transparent` in the
  stylesheet and `FileRowDelegate` (`libs/ui/FileTree.cpp`) draws the pill
  itself inside the row rectangle: selected (the accent at 0.16, 0.12 in light),
  hovered, or the keyboard-focus row. `WindowTest` samples the gutter pixels of a
  selected and a hovered row to keep it that way.
- **Names.** A note shows without `.md`; any other file shows without its
  extension and carries it as a small upper-case tag at the right (`BASE`,
  `CANVAS`, `PDF`). Folders show their name. A row cut off by the sidebar width
  shows the full name as a tooltip.
- **Chevrons and indent guides** are drawn by `FileTree::drawBranches` from the
  tokens, so they follow the theme (the old SVGs were a fixed grey). The guide
  is a faint 1px line under the parent's chevron, always visible; Obsidian also
  brightens the hovered folder's guide, which is not done.
- **Row height** is fixed by the delegate (26px at the default 13px) and does
  not depend on the row, so a 20 000 note vault never asks the model per row.
- **Tokens**, under Obsidian's names so a scheme or an imported theme can set
  them: `--nav-item-size`, `--nav-item-padding`, `--nav-item-weight`,
  `--nav-item-color-hover`, `--nav-item-background-hover/-active/-selected`,
  `--nav-collapse-icon-color` (and `-collapsed`), `--nav-indentation-guide-color`
  and `-width`, `--nav-file-tag-size/-color/-background`, `--nav-heading-color`.
- **The header** has a sort menu (name A to Z / Z to A, modified new to old / old
  to new; the choice is `files.sortOrder`, per vault) and Collapse all.
  QFileSystemModel has no created-time column and does not keep folders first on
  a modified-time sort, so there is no created order.
- **Hidden file types.** Settings > Files and links > "Hidden file types"
  (`files.hiddenExtensions`, empty by default) lists extensions the tree leaves
  out: `png, .tar.gz` - comma or space separated, case and a leading dot do not
  matter, and a multi-part one such as `tar.gz` matches the end of the name.
  Folders are never hidden. It is the file list only: search, links, the quick
  switcher, the graph and embeds still see those files, and a hidden note opened
  from search opens normally (revealing it in the tree quietly does nothing, as
  there is no row). Rows are hidden with `QTreeView::setRowHidden` as
  `QFileSystemModel` loads a folder, and every loaded folder is re-walked when
  the setting changes, so it applies live; no proxy model sits between the tree
  and the file model, and an empty list costs nothing.
- **Renaming** with F2 or the context menu opens an inline editor in the row
  (no extension shown). It never writes to the model: the new name goes through
  the vault, so links, history, plugins and open tabs follow, exactly as with
  the inline title. Dragging files in the tree is not supported (a drop through
  QFileSystemModel would rename around the vault).
- **Icons.** "Change icon..." in a row's context menu picks one of the bundled
  icons (searchable, each shown) or takes one typed character; "Remove icon"
  clears it. The icon shows left of the name in the tree and on the note's tabs,
  in `--text-normal` and `--text-muted`. UI only: kept in
  `<config dir>/noteAppC/vaults/<digest>/icons.json` next to the bookmarks, never
  in the vault or a file name; a rename or move takes it along (a folder's
  children too), a delete drops it. Settings > Appearance > "Icons on files and
  folders" hides them all and keeps them. Not shown in the quick switcher or the
  inline title; Obsidian's Iconize data is not imported.

## Bare brackets and meta notes

Two tokens, both overridable from a theme, a scheme or a snippet:

- `--bracket-color` - a bare `[text]` that is not a link, in every mode;
  `--text-accent` (a link's colour) by default.
- `--meta-note-color` - a meta note (the `editor.metaNotes` setting), delimiters
  included; `--color-orange` by default, so notes to yourself stand apart from
  bare brackets and links. It wins over `--bracket-color` where the two are the
  same run.

```css
.theme-dark { --bracket-color: var(--color-purple); --meta-note-color: var(--color-yellow); }
```

What counts as a bracket and a note is in [EDITOR.md](EDITOR.md#bare-brackets).

## Popups, the status bar and the inline title

The general design is fine; these are refinements.

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
- **The inline title** is a line edit styled as the heading (its colour is
  `--inline-title-color`, see Formatting colours): click to put a caret in it. Enter or leaving it renames the note through the same path as
  the file tree (links, history, open tabs, index), Esc or a refused name puts
  the old one back, Down or Enter moves into the note. A name with a `/` or
  another character Obsidian refuses, a leading dot, nothing, or one a note
  already has is refused with a status-bar notice. A note outside a vault is
  not renamed from here. `noteAppC_shot` takes `--popup <menu name>`,
  `--tooltip <text>` and `--keys <text>` to show these, and
  `NOTEAPPC_SHOT_SCREEN=1` for the window system's own composite.
