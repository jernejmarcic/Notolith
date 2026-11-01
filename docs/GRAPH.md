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
Right-click a note or attachment node for "Open" and "Open in new tab". Escape
on the graph page returns to the note it was opened from (the local graph in the
sidebar ignores it).

## Where the settings live

Changes are written 500 ms after the last one to the app's own copy,
`$XDG_STATE_HOME/notolith/vaults/<digest>/graph.json` (the digest is 16 hex of the SHA-256 of the vault path), in
Obsidian's key names (`collapse-filter`, `colorGroups[{query, color:{a, rgb}}]`,
`localJumps`, ...) plus `animate`. The vault's `.obsidian/graph.json` is only
read - as the starting point when there is no copy yet - and never written. The
global and the local graph share the file, except that the local depth
(`localJumps`) and the two link switches are the local graph's own. The switches
are stored as `localIncoming` / `localOutgoing`: those two names are this app's
own, because no real Obsidian `graph.json` was available to confirm Obsidian's.

## Why it holds still

The layout follows d3-force, the engine under Obsidian's graph: 60% of the
velocity survives a step (it was 92%), a spring's strength is divided by the
smaller of its ends' link counts and the pull is split by degree (a flat
strength made a 40-link hub 40x too stiff), and a wake only reheats to 0.3. A
dragged node holds an alpha target of 0.3 while held and 0 after. Measured on a
200-node ring: 32% of node moves between steps 20 and 150 reversed direction
before, 0.1% now, and it comes to rest in 175 steps instead of 277. An index
update that leaves the visible nodes and links as they were (an autosave of any
other note) no longer replaces the layout, so a settled graph stays still; a
real change keeps survivors in place and reheats gently. How it feels live
cannot be checked offscreen.

## Threads

See the comment on `GraphView` for the contract. In short: the layout and the
frame are produced by one background job at a time; the UI thread draws the
latest image. A rebuild (note switch, index change, filter) never waits for the
job: it swaps in a fresh layout and a new generation, the superseded job stops
after its current step and its output is dropped. So the UI thread's cost is the
rebuild itself (about 1 ms at 3000 nodes), not a job's 30-250 ms. Numbers: docs/PERFORMANCE.md, "The graph view at 20 000 notes".

## Checking it without a display

`noteAppC_shot <vault> <note> out.png dark --command graph:open --click
graphControlsToggle --click graphFiltersSection` renders it offscreen
(`--click` takes a button's object name and can repeat; section buttons are
`graph<Title>Section`).
