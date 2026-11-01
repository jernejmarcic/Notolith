# demo/ asset sources and licences

Every binary asset in `demo/Embeds/assets/` is copied unmodified from an
installed system package (never generated). Source files referenced as
embeds (`.py`, `.cpp`, `.json`, `.tex`) are hand-written text for this vault,
which is not an image/PDF/audio/video, so the "existing file only" rule does
not apply to them - see the note at the end of this file.

| file in `demo/Embeds/assets/` | copied from | package | licence of the file |
|---|---|---|---|
| `nvim-logo.png` | `/usr/share/pixmaps/nvim.png` | neovim 0.12.5-1.1 | The package is Apache-2.0 and LicenseRef-vim (the Vim licence). It ships no separate licence for the logo. |
| `kdoctools-banner.jpg` | `/usr/share/doc/HTML/en/kdoctools6-common/top.jpg` | kdoctools 6.30.0-1.1 | LGPL-2.0-only or LGPL-3.0-only, the package's licences. |
| `graphviz-dot-manual.pdf` | `/usr/share/doc/graphviz/dot.1.pdf` | graphviz 16.1.0-1 | EPL-1.0, the package's licence. A rendering of the `dot(1)` manual page. |
| `bell.oga` | `/usr/share/sounds/freedesktop/stereo/bell.oga` | sound-theme-freedesktop 0.8-6 | CC-BY-3.0. Credit: Dr. Richard Boulanger et al, from http://www.archive.org/details/Berklee44v12 (the theme's `CREDITS` file; the package as a whole carries six licences, this file one). |
| `example-movie.mp4` | `/usr/share/texmf-dist/tex/latex/mwe/example-movie.mp4` | texlive-latexextra 2026.1-1 (the `mwe` package) | LPPL 1.3 or later, Martin Scharrer's `mwe` package, per its TeX Live catalogue entry. Arch lists the whole of texlive-latexextra as "GPL". |
| `color-profile.icc` | `/usr/share/ghostscript/iccprofiles/ps_gray.icc` | ghostscript 10.08.0-1.1 | AGPL-3.0-or-later: the `iccprofiles` directory is part of GPL Ghostscript. |

## Attribution and licences

Each of these files is a separate work in the demo vault under its own
licence. They are not linked into or compiled into Notolith, so the
GPL-3.0-or-later of the program covers Notolith's own files and not these. The
EPL-licensed `graphviz-dot-manual.pdf`, and the others, are shipped alongside
the program as an aggregate, not combined with it. The demo vault is in the
source tree only; `cmake --install` does not copy it, and installs this file
under `share/doc/notolith/` for the record.

- `bell.oga`: "bell" from the freedesktop sound theme, copyright Dr. Richard
  Boulanger et al, licensed CC-BY-3.0 (https://creativecommons.org/licenses/by/3.0/),
  unmodified.
- The other five files are unmodified copies; their copyright holders are those
  of the packages named above.

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
