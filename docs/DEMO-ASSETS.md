# demo/ asset sources and licences

Every binary asset in `demo/Embeds/assets/` is copied unmodified from an
installed system package (never generated). Source files referenced as
embeds (`.py`, `.cpp`, `.json`, `.tex`) are hand-written text for this vault,
which is not an image/PDF/audio/video, so the "existing file only" rule does
not apply to them - see the note at the end of this file.

| file in `demo/Embeds/assets/` | copied from | package | licence |
|---|---|---|---|
| `nvim-logo.png` | `/usr/share/pixmaps/nvim.png` | neovim 0.12.5-1.1 | Apache-2.0, LicenseRef-vim |
| `kdoctools-banner.jpg` | `/usr/share/doc/HTML/en/kdoctools6-common/top.jpg` | kdoctools 6.30.0-1.1 | LGPL-2.0-only, LGPL-3.0-only |
| `graphviz-dot-manual.pdf` | `/usr/share/doc/graphviz/dot.1.pdf` | graphviz 16.1.0-1 | EPL |
| `bell.oga` | `/usr/share/sounds/freedesktop/stereo/bell.oga` | sound-theme-freedesktop 0.8-6 | GPL-2.0-only, GPL-2.0-or-later, LGPL-2.0-or-later, CC-BY-SA-3.0, CC-BY-3.0, CC-BY-4.0 |
| `example-movie.mp4` | `/usr/share/texmf-dist/tex/latex/mwe/example-movie.mp4` | texlive-latexextra 2026.1-1 | GPL |
| `color-profile.icc` | `/usr/share/ghostscript/iccprofiles/ps_gray.icc` | ghostscript 10.08.0-1.1 | AGPL-3.0-or-later |

Sizes (`ls -la`): nvim-logo.png 5.9 KB, kdoctools-banner.jpg 486 B,
graphviz-dot-manual.pdf 57.9 KB (9 pages - used for every PDF page-argument
test), bell.oga 8.3 KB, example-movie.mp4 194.6 KB, color-profile.icc 416 B.
Total assets 269 KB; the whole `demo/` vault is well under 5 MB.

`graphviz-dot-manual.pdf` is a real 9-page document, so `#page=` arguments
(single, list, range, mixed, `all`) can all be shown against one file.
`color-profile.icc` is deliberately an extension noteAppC does not recognise
as image, audio, video or PDF, so it is the "unknown binary" embed (a generic
file card).

## Text embeds (not from this table)

`demo/Embeds/assets/script.py`, `main.cpp`, `data.json`, `maths-only.tex`,
`full-document.tex`, `planets.csv`, `prices-eu.csv` and `moons.tsv` are written for this vault - they are source text,
not an image/PDF/audio/video, so nothing was copied for them.

## Generated for this vault

`demo/Embeds/assets/sample.epub` is not copied from a package: it is a
two-chapter EPUB 3 written for this vault with Python's `zipfile` (the
`mimetype` entry first and stored, `META-INF/container.xml`, an OPF with a
`cover-image` item, a nav document, a stylesheet and two XHTML chapters), 8.4 KB.
Its cover is `nvim-logo.png` above, reused unmodified. `demo/Code/Notebook.ipynb`
(nbformat 4.5, written with the `nbformat` library, one stored PNG generated
in code) and the scripts in `demo/Code/assets/` (`greetings.py`,
`countdown.sh`, `square.c`) are hand-written text for the same reason as the
files under "Text embeds".
