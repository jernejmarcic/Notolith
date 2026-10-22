---
tags: [demo, editor, settings]
---

# Settings tour

What to look for: Settings (Ctrl+, or "Open settings") has seven pages. A change
applies at once; nothing needs a restart. Each row below points at a note that
shows its effect.

| page | what is there | see |
| --- | --- | --- |
| Appearance | colour scheme, fonts and size, inline title, file icons, formatting colours | [[Themes and Colours]], [[Files and Explorer]] |
| Editor | default view, line length, line numbers, auto-pairs, vim keys, spellcheck, dictionary server, folding, code block colours, list numbers and bullets, table handles, meta notes, word count options, tab width, GPU painting | [[Modes and Folding]], [[Vim Mode]], [[List Display]], [[Table Editing]], [[Meta Notes]], [[Spellcheck and Look Up]] |
| Files and links | attachment folder, where new notes go, hidden file types, encrypted note lock, autosave, deleted files, wikilinks, link updates, file recovery interval and a "Snapshots" View button | [[Files and Explorer]], [[Encrypted Notes]], [[Note History]] |
| Core plugins | one switch per feature, canvas snapping, sync account, notebook options, diagram switches and the tool each one runs (Mermaid, PlantUML, TikZ, Graphviz), daily note and template formats | [[Feature Tour]] |
| Community plugins | the switch for third-party plugins and the list | [[Community Plugins]] |
| Hotkeys | every command, a filter box, rebinding, a clash shown on both rows | [[Navigation]] |
| About | the version, a line about the app and the path of the settings file | none |

A few things worth doing once:

1. Hotkeys: type "reading" in the filter and see Ctrl+E on "Toggle reading view".
2. Core plugins: switch Graph view off and watch the graph button leave the header
   bar, then switch it back on.
3. Files and links: set "Save automatically" off and edit a note; closing its tab
   now asks first. Set it back.
4. Editor: turn "Line numbers" on and view a note in source mode.

Vault settings that Obsidian also reads live in `.obsidian/` (this vault has
`app.json`, `appearance.json`, `core-plugins.json`, `hotkeys.json` and more).
The app's own extras are kept outside the vault in `~/.config/noteAppC/`.
