---
tags: [demo, code, notebooks]
---

# Running code

What to look for: a fenced block whose language something installed can run
has a run control (a triangle) at the right of its header strip. Click it, or
press Ctrl+Shift+Return with the caret in the block, and the output appears
under the block: text in the normal colour, stderr and warnings in orange,
errors with their name (`ZeroDivisionError: division by zero`), pictures, SVG,
Markdown, LaTeX and HTML as what they are. The output is kept in the XDG
cache, never in the note, so reopening this note shows the last result of each
block without running anything. Edit a block's code after it ran and the old
output stays, dimmed and marked "Outdated", until it runs again.

This is Settings > Core plugins > "Notebooks" (on by default). By default a
block is one short process of the language's own tool, in a sandbox: it
shares nothing with other blocks, has no network and sees none of your files.
Nothing runs when a note opens; the "Notebooks: ..." commands in the command
palette run the block at the caret, every block in the note, or every block
down to the caret. A block whose language nothing can run, or whose tool is
not installed, has no run control at all.

## One block per language

Each block prints a line that names its language, so the output tells you
which tool ran. Expected output is in the line after each block.

```python
import sys
print("python", sys.version_info.major)
2 + 3
```

Output: `python 3`, then `5` (a Python block prints the value of its last bare
expression, as a notebook cell does).

```bash
echo "bash $((6 * 7))"
```

Output: `bash 42`.

```javascript
console.log("javascript", [1, 2, 3].map(x => x * 2).join(","));
```

Output: `javascript 2,4,6` (run by `node`).

```ruby
puts "ruby #{(1..5).sum}"
```

Output: `ruby 15`.

```perl
print "perl ", 2 ** 10, "\n";
```

Output: `perl 1024`.

```lua
print("lua", #"notolith")
```

Output: `lua` and `8` separated by a tab.

```c
#include <stdio.h>

int main(void)
{
    printf("c %d\n", 6 * 7);
    return 0;
}
```

Output: `c 42` (compiled with `gcc` in a temporary directory, then run).

```cpp
#include <iostream>

int main()
{
    std::cout << "cpp " << 6 * 7 << '\n';
}
```

Output: `cpp 42` (`g++ -std=c++20`).

```rust
fn main() {
    println!("rust {}", 6 * 7);
}
```

Output: `rust 42` (`rustc`).

```java
public class Main {
    public static void main(String[] args) {
        System.out.println("java " + 6 * 7);
    }
}
```

Output: `java 42` (the single-file source launcher of `java`).

```haskell
main :: IO ()
main = putStrLn ("haskell " ++ show (product [1 .. 5]))
```

Output: `haskell 120`. A Haskell block with a `main` is a program and runs with
`runghc`; one without is a `ghci` session that prints the value of every
expression:

```haskell
map (* 2) [1, 2, 3]
product [1 .. 5]
```

Output: `[2,4,6]` and `120`.

## Checked, not run

A C, C++, Rust or Java block with no `main` cannot be a program, so it is only
type-checked (`gcc -fsyntax-only`, `rustc --crate-type=lib --emit=metadata`,
`javac`). Run these two: each should say "Checked, no errors. Nothing to run:
this block has no main." and nothing else.

```rust
fn add(a: i32, b: i32) -> i32 {
    a + b
}
```

```c
static int square(int n)
{
    return n * n;
}
```

A block with a real error is not hidden by this: the compiler's message shows
as the error.

```c
int main(void)
{
    return undefined_name;
}
```

## Errors and stderr

The error is named the way its traceback names it, with the traceback under
it. Expect `ZeroDivisionError` and "division by zero":

```python
print("before")
1 / 0
```

A run that works but writes to stderr shows that text in orange next to the
normal output:

```python
import sys
print("normal")
print("a warning", file=sys.stderr)
```

## Time limit

A run stops after 30 seconds, or after `#| timeout:` seconds, and ends with a
"Timed out" error saying how long it ran ("stopped after 2 s"). A running
block shows a stop square where its run control was; a click on it stops the
run by hand, as does "Notebooks: Interrupt the kernel".

```bash
#| timeout: 2
echo "starting"
sleep 20
echo "never printed"
```

## The sandbox

Blocks run under bubblewrap: the system's `/usr` and a few `/etc` files
read-only, an empty `/tmp`, no network, none of your files, and a `HOME` that
is the block's own throwaway directory. Run this one. Each line should say
"refused" and name the reason, and `HOME` should be a temporary directory, not
your home:

```python
import os, socket

print("HOME:", os.environ["HOME"])

for path in ("/etc/machine-id", os.path.expanduser("~/.bashrc"), "/root"):
    try:
        os.listdir(path) if os.path.isdir(path) else open(path).read()
        print("READ", path)
    except OSError as error:
        print("refused:", path, "-", error.strerror)

try:
    socket.create_connection(("1.1.1.1", 53), timeout=3)
    print("NETWORK REACHABLE")
except OSError as error:
    print("refused: network -", error.strerror)
```

The same from the shell. `ls` has nothing of yours to list, and the machine
id and the network are not there:

```bash
echo "HOME is $HOME"
ls "$HOME" | wc -l
cat /etc/machine-id 2>&1 | head -1
(echo > /dev/tcp/1.1.1.1/53) 2>&1 | head -1
```

Without `bwrap` installed no block runs at all ("code runs only inside
bubblewrap's sandbox"); it is never run unsandboxed on the quiet.

### Running as you

`#| sandbox: host` asks to run with your own files and network. It only counts
when Settings > Core plugins "Let notes run code outside the sandbox" is on
(it is off by default, so a shared note cannot take your files); off, the
block runs sandboxed as if it had not asked. With the setting off this prints
a temporary directory; with it on, your real home:

```bash
#| sandbox: host
echo "HOME is $HOME"
```

## Jupyter kernels

A kernel keeps variables alive between blocks. Settings > Core plugins "Share
one kernel between a note's code blocks" (off by default) puts every block of
a note in the language's kernel; `#| kernel: on` does it for one block, and the
blocks below ask for it, so they work with the setting off. It needs the
language's Jupyter kernel (Python: `ipykernel`; the kernels found are read from
the kernelspec directories). A note gets one kernel per language, started by
its first block ("Started the python kernel" in the status bar) and ended when
the window closes. A block run this way shows its execution count beside the
run control, `[1]`, and `[*]` while it runs.

### Variables shared between two blocks

Run the first, then the second: the second prints `42`. Run only the second in
a fresh window and it fails with `NameError: name 'counter' is not defined`.
Run the second again and its count goes up; the first block's number does not.

```python
#| kernel: on
counter = 41
```

```python
#| kernel: on
counter + 1
```

"Notebooks: Restart the kernel" forgets `counter`; "Notebooks: Restart the
kernel and run every code block" starts over and runs them top to bottom.
Another note has its own kernel, so `counter` is not defined there.

### input()

The kernel asks for a line through a one-line prompt that appears under the
block, with the kernel's own prompt text ("Your name: "). Type a name and press
Enter: the block goes on and prints it. Esc interrupts the run instead.

```python
#| kernel: on
name = input("Your name: ")
print("hello,", name)
```

Outside a kernel there is no terminal behind the block, so the same call
fails. Expect `EOFError`:

```python
#| kernel: off
input("Your name: ")
```

### Rich output

Each block displays one kind of output. They are generated with the standard
library only (no matplotlib or pandas needed, though a matplotlib figure and a
pandas table come back the same way). Expected: a picture of a colour
gradient 160 by 80 px;

```python
#| kernel: on
import struct, zlib
from IPython.display import Image, display

def png(width, height):
    rows = b"".join(
        b"\x00" + b"".join(bytes((x * 255 // width, y * 255 // height, 160)) for x in range(width))
        for y in range(height))
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))

display(Image(data=png(160, 80)))
```

a drawn circle and a line of text, as SVG;

```python
#| kernel: on
from IPython.display import SVG, display

display(SVG(data='<svg xmlns="http://www.w3.org/2000/svg" width="200" height="80">'
                 '<circle cx="40" cy="40" r="30" fill="#e03131"/>'
                 '<text x="85" y="46" font-size="20" fill="#1c7ed6">SVG output</text></svg>'))
```

a small table, as HTML (drawn by the same engine as HTML blocks);

```python
#| kernel: on
from IPython.display import HTML, display

display(HTML("<table border='1' cellpadding='4'>"
             "<tr><th>note</th><th>links</th></tr>"
             "<tr><td>Hub</td><td>4</td></tr>"
             "<tr><td>Target Note</td><td>3</td></tr></table>"))
```

a formula, as LaTeX (drawn like a `$$` block);

```python
#| kernel: on
from IPython.display import Latex, display

display(Latex(r"e^{i\pi} + 1 = 0"))
```

and bold text, a code span and a list, as Markdown:

```python
#| kernel: on
from IPython.display import Markdown, display

display(Markdown("**Bold**, *italic* and `code`.\n\n- first\n- second"))
```

Outputs come in the order they were produced. A carriage return rewrites its
line, so this prints a progress counter that ends on `100%`, and `clear_output`
drops what came before it, so `gone` never stays:

```python
#| kernel: on
import time
from IPython.display import clear_output

for n in (25, 50, 75, 100):
    print(f"{n}%", end="\r", flush=True)
    time.sleep(0.2)
print()
print("gone")
clear_output()
print("kept")
```

## Completion and documentation

With a kernel already running for the language (run the two `counter` blocks
above first), put the caret at the end of `bit_` in the block below and press
Ctrl+Space: the kernel's completions list `bit_count` and `bit_length`; Enter
or Tab takes one. Put the caret on `counter` and press Shift+Tab for its
documentation in a small box under the caret (Esc closes it). With no kernel
running, Ctrl+Space says so and starts nothing. The block is only a place to
type; running it would fail.

```python
#| kernel: on
counter.bit_
```

## A language nothing can run

No recipe and no Jupyter kernel for this one, so its block has no run control
and no output row, only the highlighted listing:

```zorp
this has no run control
```

## A notebook file

[[Notebook.ipynb]] is a small Jupyter notebook in this folder. With the
Notebooks plugin on it opens as a note (with it off it is refused like any
file that is not a note): its markdown cells are the text, its code cells
are `python` blocks, and the outputs stored in the file (text, a result, a
PNG and an HTML table) show under their blocks at once. Edit a cell and save:
only that cell changes in the file, and the rest of the notebook is kept byte
for byte. Running a block there does not write its new output back into the
file.

## Clearing

"Notebooks: Clear the output of every code block" removes every output in the
note and what was kept for it in the cache.
