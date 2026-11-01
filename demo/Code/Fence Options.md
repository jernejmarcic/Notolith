---
tags: [demo, code]
---

# Fence options

What to look for: a fenced block may begin with lines that start `#|` (or
`//|`, `--|` and so on, the language's own comment mark followed by `|`), each
`key: value`. Notolith reads them and hides those lines in live preview, so
the listing shows only the code; in Obsidian they stay visible as ordinary
comments, so the note is still valid there. Each block below sets one option
and says what the block should look like. Putting the caret inside a block
shows its option lines again as text, with no header strip, gutter or caption,
so it can be edited. The language's colours, icon and label can also be
changed per language and per fence, see "Attributes on the fence line" at the
end and `docs/STYLING.md`.

## title

`#| title: ...` (Quarto's `filename:` means the same) replaces the language
name in the header strip. This one should read "scripts/greet.py" at the left
of the strip, with no "python":

```python
#| title: scripts/greet.py
def greet(name):
    return f"hello, {name}"

print(greet("vault"))
```

The value may be quoted when it has special characters:

```python
#| filename: "my notes: draft.py"
print("the quotes are not part of the title")
```

## caption

`#| caption: ...` puts an italic line under the block, inside its panel:

```python
#| caption: Listing 1 - squares of the first five numbers
print([n * n for n in range(1, 6)])
```

## lines

`#| lines: true` adds a line-number gutter counting from 1; `#| lines: 10`
counts from 10; `false` or `0` is no gutter. The first gutter should read 1 to
4, the second 10 to 13:

```python
#| lines: true
a = 1
b = 2
c = a + b
print(c)
```

```python
#| lines: 10
a = 1
b = 2
c = a + b
print(c)
```

## highlight

`#| highlight: 2,4-5` tints lines 2, 4 and 5 of the listing, counted without
the option lines. Here lines 2, 4 and 5 should be shaded (`total = 0`,
`total += n` and `print(total)`), and the number past the end is ignored:

```python
#| highlight: 2,4-5,99
numbers = [3, 1, 4, 1, 5]
total = 0
for n in numbers:
    total += n
    print(total)
```

A highlight and a gutter together, the gutter reading 1 to 3 and line 3
(`return w * h`) tinted:

```python
#| lines: true
#| highlight: 3
def area(w, h):
    # only the next line is tinted
    return w * h
```

## wrap

A listing with `#|` options does not reflow by default: a long line scrolls
sideways. `#| wrap: true` wraps it at the width of the column instead. The
first block below should have a horizontal scrollbar on its one long line, the
second should show the same line broken over several rows:

```python
#| title: wrap off
text = "a single long line that keeps going well past the right edge of the text column so that it either scrolls sideways or wraps, depending on the option above it"
```

```python
#| title: wrap on
#| wrap: true
text = "a single long line that keeps going well past the right edge of the text column so that it either scrolls sideways or wraps, depending on the option above it"
```

## fold

`#| fold: true` draws the block folded: only its header strip, with a fold
arrow at the right. Click the strip (or the arrow) to open it, again to fold
it. A block with any `#|` option can be folded by hand the same way, whatever
this says; the option only sets the starting state. This one starts folded:

```python
#| title: Setup (folded)
#| fold: true
import os
print("configured")
```

## eval

`#| eval: false` means the block is never run: there is no run control at all
(the triangle at the right of the strip), although python is installed. The
block below should have none, the one after it should have one:

```python
#| eval: false
print("this never runs")
```

```python
print("this one can run")
```

## echo

`#| echo: false` hides the listing in live preview and shows only the run
control and the output, as a chart-only cell in Quarto does. Run it and only
"visible output" appears under an otherwise empty block. The caret inside the
block (or Source mode) reveals the code:

```python
#| echo: false
print("visible output")
```

## output

`#| output: false` runs the block and keeps the result, but draws no output
under it. The listing and run control stay. Run it; nothing appears:

```python
#| output: false
print("you will not see this")
```

## include

`#| include: false` is `echo: false` and `output: false` together: nothing of
the block is drawn but its run control, and the run still happens. Useful for
a setup block:

```python
#| include: false
setup_done = True
```

## cache

Outputs are normally kept (in the XDG cache, never in the note) so that
reopening the note shows each block's last result. `#| cache: false` keeps
nothing: run this one, close the note and open it again, and its output is
gone while the other block's output is still there:

```python
#| cache: false
print("not remembered")
```

```python
print("remembered")
```

## timeout

`#| timeout: 2` stops a run after 2 seconds (the default is 30). Run it: it
ends with a "Timed out" error saying "stopped after 2 s" instead of printing:

```bash
#| timeout: 2
sleep 20
echo "never printed"
```

## kernel

`#| kernel: on` runs the block in the language's Jupyter kernel, `off` forces
the system's own tool even when Settings turns "Share one kernel between a
note's code blocks" on, and a name such as `python3` picks that kernelspec.
See "Running Code" for kernels at work. This block asks for the plain tool:

```python
#| kernel: off
print("a fresh python3 process, no kernel")
```

## sandbox

Blocks run in a sandbox with no network and none of your files, always. `#| sandbox: wasm` runs
the block as a WebAssembly module under `wasmtime`. With no interpreter module
set for python in `notebooks.wasmModules`, this block should refuse to run and
say "no WebAssembly module set for "python" blocks":

```python
#| sandbox: wasm
print("needs a python.wasm")
```

## Other comment marks

The option mark is the language's own line-comment token, so options work in
languages whose comments are not `#`. Both blocks below should show a title
and hide their option line:

```cpp
//| title: main.cpp
#include <iostream>
int main() { std::cout << "hello\n"; }
```

```sql
--| title: query.sql
--| lines: true
SELECT path FROM notes WHERE tags LIKE '%demo%';
```

A `#|` line in a language that comments with `//` is not an option, only
text. This block should keep its first line visible and have no title:

```cpp
#| title: ignored
int answer = 42;
```

## Where the lines may be

Only at the very top, one after another. The first line that is not an option
ends them, so the second `#|` line below is just a comment and the title is
the first one:

```python
#| title: first one wins
print("start")
#| title: not an option, only a comment
```

## Keys with no meaning

Quarto's `warning`, `error` and other keys are accepted and ignored, which
hides their lines and changes nothing else. The listing below should show only
the `print`:

```python
#| warning: false
#| error: true
print("the two option lines are hidden and do nothing")
```

## Attributes on the fence line

Not `#|` options but part of the same family: everything after the language on
the opening fence line. CommonMark takes only the first word as the language,
so the block still highlights in Obsidian. `name`, `icon` and `color` change
the label in the header strip; this one should read "Setup script" in the
given colour, with an icon in front if the icon font has that glyph:

```python name="Setup script" icon=U+E73C color=#F4B400ff
print("a styled label")
```

`bg`, `fg`, `border` and `syntax.<capture>` restyle the panel and the tokens
(`syntax.keyword` with a colour and `bold`, `italic` or `underline`). This
block should have a dark blue panel, a thin blue border and bold purple
keywords:

```python bg=#112233ff fg=#eeeeee border=#3776AB syntax.keyword="#C678DD bold"
def double(n):
    return n * 2
```

Settings > Editor "Code block colours" off draws every block as if none of
this were set.
