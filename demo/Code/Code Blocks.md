---
tags: [demo, code]
---

# Code blocks

What to look for: each fence below should syntax-highlight for its
language, show a clickable language label with copy-on-click (copying the
listing without the fences or any `#|`/attribute text), and the block with
no language should show "Copy" on hover instead of a label. Per-language
look (name, icon, colour) and per-fence overrides come from
`~/.config/notolith/codeblocks.ini`, documented in `docs/STYLING.md` - this
note works unstyled with no such file present.

## C++

```cpp
#include <memory>
#include <vector>

struct Node {
    int value;
    std::vector<std::unique_ptr<Node>> children;
};

int sum(const Node &n) {
    int total = n.value;
    for (const auto &child : n.children)
        total += sum(*child);
    return total;
}
```

## Python

```python
from dataclasses import dataclass, field


@dataclass
class Vault:
    root: str
    notes: list[str] = field(default_factory=list)

    def add(self, path: str) -> None:
        self.notes.append(path)
```

## JSON

```json
{
  "vault": "demo",
  "features": ["callouts", "bases", "canvas"],
  "maxSizeBytes": 5242880,
  "strict": false
}
```

## Bash

```bash
#!/usr/bin/env bash
set -euo pipefail

for note in demo/**/*.md; do
    echo "checking: ${note}"
done
```

## Rust

```rust
struct Note {
    title: String,
    tags: Vec<String>,
}

impl Note {
    fn slug(&self) -> String {
        self.title.to_lowercase().replace(' ', "-")
    }
}
```

## SQL

```sql
SELECT path, mtime
FROM notes
WHERE tags LIKE '%demo%'
ORDER BY mtime DESC
LIMIT 10;
```

## An unknown language

A label that is not in `codeblocks.ini` and is not a language Notolith's
highlighter knows should still show, verbatim, as its own name with no icon:

```zorp
this is not a real language
it should still render as plain monospaced text
labelled "zorp"
```

## A very long line

```text
this single line intentionally has no wrapping and keeps going for a while so the block gets a horizontal scrollbar instead of wrapping, which matters because wrapping inside a code block hides where a line actually ends and makes a diff impossible to read at a glance, so Notolith should scroll this sideways rather than reflow it
```

## Indented code (four spaces, not a fence)

    def indented():
        return "this block used four-space indentation instead of a fence"

## Tabs (Notolith's own extension)

Adjacent fences marked `tab` become one tabbed block - documented in
`docs/STYLING.md`. This still opens in Obsidian as three separate blocks.

```python tab
def open_vault(path):
    return Vault(path)
```

```cpp tab="C++"
Vault openVault(const std::string &path) { return Vault(path); }
```

```bash tab="Shell"
notolith ./demo
```
