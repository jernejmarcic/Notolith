---
title: Callouts
tags: [demo, callouts]
---

# Callouts

Obsidian's own form, unchanged:

> [!note] A plain callout
> Body text.

> [!warning]+ Foldable, open
> The `+` opens it, the `-` closes it.

A listing inside one keeps its font, its panel and its highlighting:

> [!tip] With code
> ```python
> def greet(name: str) -> str:
>     return f"hello {name}"
> ```
> And text after it.

Callouts nest, each with its own look:

> [!tip] Outer
> Outer body.
> > [!info] Inner
> > Inner body, long enough to wrap so the indent shows.
> > ```cpp
> > auto n = std::make_unique<Node>(42);
> > ```
> Back to the outer body.

noteAppC's own styling rides in the `|` slot Obsidian parses and ignores, so
this note still opens there - just unstyled:

> [!note|bar=#FF5A5Fff bg=#FF5A5F26 heading-color=#FF9A9Eff] Styled inline
> One callout restyled without touching any config.

The same keys go in a `:::` container's braces:

::: {.note bar="#7CFF7Cff" bg="#7CFF7C1A" title="Styled container"}
Body text.
:::
