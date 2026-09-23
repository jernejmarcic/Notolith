---
title: Code blocks
tags: [demo, code]
---

# Code blocks

The language label copies the listing when clicked:

```python
def greet(name: str) -> str:
    return f"hello {name}"
```

A fence can restyle its own label, after the language:

```python name="Setup script" color=#F4B400ff
print("configure")
```

Adjacent fences marked `tab` become one tabbed block:

```python tab
import noteapp
noteapp.open("vault")
```

```cpp tab="C++"
#include <noteapp.h>
int main() { noteapp::open("vault"); }
```

```bash tab="Shell"
noteapp open vault
```

A block with no language shows Copy while the pointer is over it:

```
plain text
```
