---
tags: [demo, tables]
---

# Tables

What to look for: column alignment should match the `:---`/`:---:`/`---:`
markers, inline formatting should still work inside a cell, and the wide
table should get a horizontal scrollbar rather than squeezing the window.

## Alignment

| Left | Center | Right |
| :--- | :---: | ---: |
| a | b | c |
| left-aligned | centered | 42 |
| short | a longer centered value | 1234.50 |

## Inline formatting inside cells

| Feature | Status | Notes |
| --- | --- | --- |
| **Bold** | done | plain |
| *Italic* | done | plain |
| `Inline code` | done | plain |
| ~~Strikethrough~~ | done | plain |
| A [[Target Note\|link]] | done | escaped pipe inside the alias |
| ==Highlight== | done | plain |

## A wide table

| ID | Name | Category | Owner | Status | Priority | Created | Updated | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Vault open | Core | lane-lead | done | high | 2026-09-22 | 2026-09-23 | atomic save |
| 2 | Live preview | Editor | lane-lead | done | high | 2026-09-22 | 2026-09-23 | viewport-only layout |
| 3 | Callouts | Rendering | lane-B | done | medium | 2026-09-22 | 2026-09-22 | 13 built-in types |
| 4 | Graph view | Optional | lane-C | done | medium | 2026-09-23 | 2026-09-23 | Barnes-Hut |
| 5 | Canvas | Optional | lane-D | done | medium | 2026-09-23 | 2026-09-23 | JSON Canvas |
| 6 | Bases | Optional | lane-E | done | medium | 2026-09-23 | 2026-09-23 | table + card views |
| 7 | Embeds | Editor | lane-I | in progress | high | 2026-09-23 | 2026-09-23 | this vault documents the gaps |
