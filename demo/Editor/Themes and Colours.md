---
tags: [demo, editor, styling]
---

# Themes and colours

What to look for: everything about how the app is coloured is a setting or a
small file, and none of it is written into a note.

## A theme that travels with this vault

`.obsidian/noteAppC-themes/Demo.json` is a colour scheme: a `dark` and a `light`
object that override three tokens (the background, the accent, the accent
text) and nothing else. Open Settings > Appearance, pick "Demo" in the colour
scheme list, and the window turns warm at once. Pick the old one back to undo.
Obsidian ignores the file.

The same folder may hold a loose `Name.css` or an installed Obsidian theme as
`Name/theme.css`; only the colours in them are used, never their layout. Schemes
for every vault go in `~/.config/notolith/themes/`. Edit a scheme file and the
window restyles with no restart.

## Formatting colours

Settings > Appearance, under the fonts, has a row for each kind of text:
note title, headings 1 to 6, bold, italic, highlight, inline code, links,
tags, quotes, list markers, checkboxes and footnote marks, each with a swatch for
dark and one for light. Change **bold** or a #tag in this note and it changes
at once. "Reset all formatting colours" undoes it. Fonts and the font size are
on the same page.

## Config files for callouts, code and tasks

- `~/.config/notolith/callouts.ini`: new callout types and the look of old ones,
  see [[Callouts]].
- `~/.config/notolith/codeblocks.ini` and the attributes on a fence line: code
  block colours, icons and tabs, see [[Code Blocks]].
- `~/.config/notolith/tasks.ini`: the checkbox states, see [[Lists]]. A section
  such as `[waiting]` with `char = w` adds a state.

All three reload the moment the file changes. A file is not needed: the notes
above show what each looks like with none.

## File explorer and popups

The explorer, the menus and the quick switcher take their colours from the same
tokens, so a scheme restyles them too; see [[Files and Explorer]].
