---
tags: [demo, diagrams, tikz]
---

# TikZ diagrams

What to look for: each ` ```tikz ` fence below should be drawn as a vector
picture, the way the TikZJax plugin does in Obsidian: either TikZJax's block
(`\usepackage` and `\usetikzlibrary` lines, then
`\begin{document} ... \end{document}`) or a bare `tikzpicture`. Notolith puts
the fence into a `standalone` document (the `tikz` class option is already
on, so `\usepackage{tikz}` is not needed and is harmless), runs
`latex -no-shell-escape` and then `dvisvgm --no-fonts --exact-bbox`, both
inside bubblewrap's sandbox (no network, no home directory, 20 seconds each),
and draws the SVG. It needs TeX Live's `latex` and `dvisvgm`; `dvisvgm` is
looked for next to `latex`, then on `PATH`. Without them, or without `bwrap`,
a fence shows its source with one dim line under it ("TikZ: install latex to
render"). Settings > Core plugins > "TikZ diagrams" turns it off, and then a
fence is an ordinary code block. The first diagram takes a second or two; a
repeated one is read from the cache.

On a dark theme black (written out, or the SVG's implicit default) becomes the
theme's text colour, so line drawings stay visible; other colours keep their
value. The picture sits on a rectangle in the theme's background.

## TikZJax's block, with a library

A preamble of `\usetikzlibrary`, then the document. Expect three circled
nodes joined in a loop by arrows with the `arrows.meta` library's heads:

```tikz
\usetikzlibrary{arrows.meta, positioning}
\begin{document}
\begin{tikzpicture}[node distance=2cm, every node/.style={draw, circle}]
  \node (a) {A};
  \node (b) [right of=a] {B};
  \node (c) [below of=a] {C};
  \draw[-{Stealth}] (a) -- (b);
  \draw[-{Stealth}] (b) -- (c);
  \draw[-{Stealth}] (c) -- (a);
\end{tikzpicture}
\end{document}
```

## A bare tikzpicture

No preamble and no `\begin{document}`; the picture is all there is:

```tikz
\begin{tikzpicture}
  \draw[thick] (0,0) circle (1);
  \draw (-1,0) -- (1,0);
  \draw (0,-1) -- (0,1);
  \node at (0,-1.5) {a circle and its axes};
\end{tikzpicture}
```

## Colours

The red and blue below keep their colours on any theme; the black outline and
text follow the theme (light text on a dark theme):

```tikz
\begin{tikzpicture}
  \filldraw[fill=red!70, draw=red!40!black] (0,0) rectangle (2,1);
  \draw[blue, very thick] (0,1.5) -- (2,1.5);
  \node at (1,-0.5) {black text};
\end{tikzpicture}
```

## pgfplots

Packages from the TeX installation work, here `pgfplots` for a function plot.
Expect axes labelled x and y with a parabola:

```tikz
\usepackage{pgfplots}
\pgfplotsset{compat=1.18}
\begin{document}
\begin{tikzpicture}
  \begin{axis}[xlabel=$x$, ylabel=$y$, width=7cm, height=5cm]
    \addplot[domain=-2:2, samples=40, thick] {x^2};
  \end{axis}
\end{tikzpicture}
\end{document}
```

## A picture with an error

`\nonsense` is not a command. TeX stops at it; the fence shows its source in a
listing and one dim line from TeX's own first error:
"TikZ: Undefined control sequence."

```tikz
\begin{tikzpicture}
  \draw (0,0) -- (1,1) \nonsense;
\end{tikzpicture}
```

## What is not accepted: a document class

The fence is already placed in a `standalone` document, so a
`\documentclass` line of its own would be a second one. TeX refuses it, and
the fence shows its source with the hint "TikZ: LaTeX Error: Two
\documentclass or \documentstyle commands." Write the preamble without it, as
above:

```tikz
\documentclass{article}
\begin{document}
\begin{tikzpicture}
  \draw (0,0) -- (1,1);
\end{tikzpicture}
\end{document}
```
