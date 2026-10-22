---
tags: [demo, graph]
---

# Graph view

What to look for: Ctrl+G (or the graph button in the header bar, or "Graph
view: Open graph view") shows every note as a node and every link as a line.
Nodes tagged `embeds` and `backlinks` or `links`, and everything under
`Library/`, are coloured by the groups in `.obsidian/graph.json`.

## Try

1. Open the graph. Hover a node: it and its neighbours light up. Scroll to
   zoom, drag the background to pan, drag a node to move it.
2. Click the gear. Four sections fold open: Filters, Groups, Display, Forces.
3. In Filters type `path:Library` or `tag:#links` to keep only matching notes.
   Switch Orphans off to hide notes nothing links to, and Unresolved links off
   to hide the grey nodes for links with no note (there is one in
   [[Links and References]]).
4. In Groups add a row, `path:Code`, pick a colour. First match wins.
5. Right-click a node: "Open" or "Open in new tab".
6. Press Escape to return to the note you came from.

The local graph is the branch icon in the right sidebar. It shows the open
note's neighbours only, with Depth (1 to 5) and Incoming/Outgoing switches in
its gear. Open [[Hub]] to see a busy one.

The settings are kept in the app's own copy of `graph.json`; the file in this
vault is only the starting point. The whole feature is Settings > Core plugins
> Graph view.
