---
tags: [demo, embeds]
---

# Source file embeds

What to look for: each file below should show as a labelled, syntax
highlighted panel with its own content, the same treatment a fenced code
block gets, rather than a plain download link. A file over Notolith's
512 KB inline cap would show its header and a "too large to show inline"
message instead - none of these are anywhere near that size.

## Python

![[script.py]]

## C++

![[main.cpp]]

## JSON

![[data.json]]

## A line-range embed

Obsidian's own `#L2-L4` syntax narrows a source embed to just those lines:

![[script.py#L1-L3]]
