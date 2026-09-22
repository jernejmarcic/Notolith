---
title: Welcome to noteAppC
tags: [demo, obsidian]
created: 2026-09-22
reviewed: 2026-09-22T10:30:00
priority: 2
done: false
aliases:
  - Start here
  - Front page
---

# Welcome to noteAppC

A **Linux-native** alternative to *Obsidian*, written in C++ with
==no JavaScript==. Text can be ~~struck through~~ and `inlined as code`.

## Links and embeds

See [[Second note]], or [[Second note|the same note under another name]].
A heading link: [[Second note#Details]]. A block link: [[Second note#^anchor]].

External: [Obsidian](https://obsidian.md) and <https://commonmark.org>.
A reference link to [CommonMark][spec], and an unresolved [[Nowhere]].

[spec]: https://spec.commonmark.org "The specification"

## Lists

- First item
- Second item with `inline code`
  - Nested item
    - Deeper still
- [ ] An open task
- [x] A finished task

1. Ordered one
2. Ordered two
   1. Nested ordered

## Callouts

> [!note] This is a callout
> Callouts carry their own colour and icon.

> [!warning]- A folded warning
> Hidden until you open it.

::: tip A colon-fence container
The `:::` form from the Ideas spec renders the same way.
:::

::: details Something to unfold
Collapsed by default, opened by its title.
:::

## Table

| Feature | Status | Notes       |
| ------- | :----: | ----------: |
| Parser  | done   | md4qt       |
| Editor  | done   | QTextLayout |
| Graph   | todo   | Phase 5     |

## Code

```cpp
#| title: main.cpp
#| lines: true
#| highlight: 4-5
#include <QApplication>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    return app.exec();
}
```

```python
def greet(name: str) -> str:
    return f"hello {name}"
```

## Maths

Inline $e^{i\pi} + 1 = 0$, and a block:

$$
\int_{0}^{\infty} e^{-x^2}\,dx = \frac{\sqrt{\pi}}{2}
$$

## HTML and footnotes

Some <u>underlined</u> and <sup>raised</sup> text.[^1]

[^1]: The footnote body.

---

Tags: #demo #status/active #status/review
