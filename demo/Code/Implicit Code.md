---
tags: [demo, code]
implicit-python: |
  import math
implicit-c: |
  #include <stdio.h>
  #include <math.h>
implicit-rust: |
  use std::collections::HashMap;
implicit-rust-main: |
  fn main() {
  {{code}}
  }
---

# Implicit code

What to look for: the note's properties (the table at the top) hold
`implicit-python`, `implicit-c`, `implicit-rust` and `implicit-rust-main`,
shown read-only. Every runnable block below gets the code for its language
when you run it, so none of them imports anything or (for the Rust case)
writes a `main`. Blocks that get implicit code show `+implicit` beside the
run control, and copying a block copies only what the listing shows. Nothing
runs when the note opens; use the run control. Running needs the language's
own tool (`python3`, `gcc`, `rustc`).

## Python: imports you did not write

`math` is imported by the note, not by the block, so this prints the value of
tau (6.283185307179586).

```python
math.tau
```

## C: the prelude above a block with its own main

The block has its own `main`, so it is not wrapped; it only gets the two
`#include` lines. It prints `7.0`.

```c
int main(void) {
    printf("%.1f\n", sqrt(49));
    return 0;
}
```

## Rust: a block with no main goes into the wrapper

No `fn main` here, so the block goes where `{{code}}` is in
`implicit-rust-main`, and `HashMap` comes from `implicit-rust`. It prints `1`.

```rust
let mut m = HashMap::new();
m.insert(1, 2);
println!("{}", m.len());
```

## C: imports only, no main, still only checked

Nothing wraps C here (there is no `implicit-c-main`), so a block without a
`main` is type-checked with the includes above it and not run. The result
reads "Checked, no errors. Nothing to run: this block has no main."

```c
double hypot2(double a, double b) {
    return sqrt(a * a + b * b);
}
```

## Opting out

`#| implicit: false` leaves the note's implicit code off this one block, so
it does not see `math` and fails with a `NameError`. The marker is also not
shown beside the run control.

```python
#| implicit: false
math.pi
```

## Errors count from the block's own first line

The note's prelude is one line, but the error below points at line 2 of the
block, not line 3 of the file that ran. An error in the implicit code itself
would read "implicit code, line 1".

```python
x = 1
x / 0
```
