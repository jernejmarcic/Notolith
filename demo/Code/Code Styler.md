---
tags: [demo, code]
---

# Code Styler

What to look for: the core plugin **Advanced code blocks** (Settings > Core
plugins) reads the syntax of Obsidian's Code Styler plugin, so a note written
for it looks the same here. Put the caret inside a block to see what was
written; turn the plugin off and every block below is plain text again.

## Fence line parameters

A title, line numbers from 10, and lines 2 and 4-5 highlighted:

```cpp title:"main file.cpp" ln:10 hl:2,4-5
#include <iostream>
int main() {
    std::cout << "hello\n";
    int answer = 42;
    return answer;
}
```

Folded when the note opens, with its own placeholder (click the header):

```python fold:"Collapsed listing"
def greet(name):
    return f"hello {name}"
```

## Named highlights

`info`, `warn` and `error` tint their lines in the colours of Settings >
Editor > Named highlights. Lines 1, 3 and 4:

```js info:1 warn:3 error:4
const ok = true;
const quiet = 1;
const careful = 2;
throw new Error("boom");
```

## Inline code in a language

Sorting is `{python} sorted(words, key=len)` and the prefix is hidden until the
caret is in it; `plain inline code` is unchanged.

## File references

A file of this vault, whole, with its name as the title (click the title to
open the file; the caret in the block shows the lines below):

```reference
file: ./assets/greetings.py
```

Only `main` and the three lines after it, the start found by a word:

```reference
file: ./assets/square.c
start: main
end: +3
```

A web link shows a notice, since fetching remote files is not in the default
build (`-DNOTEAPPC_WITH_NETWORK=ON`, docs/OFFLINE.md):

```reference
link: https://github.com/mayurankv/Obsidian-Code-Styler/blob/main/README.md
start: "Codeblock Parameters"
end: +12
```
