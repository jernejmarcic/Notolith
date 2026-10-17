# Graph view

Opened by the graph button in the header bar (right of "+"), Ctrl+G, or the
"Graph view: Open graph view" command; the sidebar's local graph is the
branch icon in the right sidebar's tabs. Both are `libs/ext/graph`, behind
`NOTEAPPC_WITH_GRAPH`, and the Core plugins switch "Graph view" hides the
button, the pane and the commands.

## Controls

The gear at the top right of the graph opens Obsidian's card: four folding
sections whose open or folded state is kept like the rest.

- **Filters** - "Search files..." (the search grammar: `path:`, `tag:`, a bare
  word matches the file name; a tag or unresolved node stays while a kept note
  links to it), Tags, Attachments, Unresolved links, Orphans; in the local
  graph also Depth (1-5) and Incoming / Outgoing links (follow links into the
  note, out of it, or both - the default).
- **Groups** - one row per colour group: a query (same grammar), a swatch that
  opens a colour dialog, and a remove button; "New group" adds one. First match
  wins; an empty query colours nothing.
- **Display** - Arrows, Text fade threshold (labels fade in with zoom; the
  slider moves where; a small vault opens labelled, a big one shows its
  labels as you zoom in), Node size, Link thickness, Animate (off: the layout
  settles, up to a quarter second a pass, before the picture moves).
- **Forces** - Center, Repel, Link force, Link distance.

Hover a node to light it and its neighbours; drag to move (the node is pinned
while held); scroll to zoom about the pointer; drag the background to pan.

## Where the settings live

Changes are written 500 ms after the last one to the app's own copy,
`<config dir>/noteAppC/vaults/<sha1 of the vault path>/graph.json`, in
Obsidian's key names (`collapse-filter`, `colorGroups[{query, color:{a, rgb}}]`,
`localJumps`, ...) plus `animate`. The vault's `.obsidian/graph.json` is only
read - as the starting point when there is no copy yet - and never written. The
global and the local graph share the file, except that the local depth
(`localJumps`) and the two link switches are the local graph's own. The switches
are stored as `localIncoming` / `localOutgoing`: those two names are this app's
own, because no real Obsidian `graph.json` was available to confirm Obsidian's.

## Threads

See the comment on `GraphView` for the contract. In short: the layout and the
frame are produced by one background job at a time; the UI thread draws the
latest image. Numbers: docs/PERFORMANCE.md, "The graph view at 20 000 notes".

## Checking it without a display

`noteAppC_shot <vault> <note> out.png dark --command graph:open --click
graphControlsToggle --click graphFiltersSection` renders it offscreen
(`--click` takes a button's object name and can repeat; section buttons are
`graph<Title>Section`).
