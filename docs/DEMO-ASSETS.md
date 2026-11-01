# demo/ asset sources and licences

Every binary asset in `demo/Embeds/assets/` is either copied unmodified from an
installed system package, or made for this vault: `sample.epub` and
`gradient.png` (the same PNG as the one stored in `demo/Code/Notebook.ipynb`)
are made for it. Source files referenced as embeds (`.py`, `.cpp`, `.json`,
`.tex`) are hand-written text for this vault, which is not an image, PDF, audio
or video, so the "existing file only" rule does not apply to them - see the
note at the end of this file.

An asset is in the demo only when its source states its licence. A file whose
package gives no licence for that file is not shipped.

## Copied from a package

| file in `demo/Embeds/assets/` | copied from | package | licence of the file |
|---|---|---|---|
| `graphviz-dot-manual.pdf` | `/usr/share/doc/graphviz/dot.1.pdf` | graphviz 16.1.0-2 | Eclipse Public License 2.0, the package's licence (`/usr/share/doc/graphviz/COPYING`). A rendering of the `dot(1)` manual page. |
| `bell.oga` | `/usr/share/sounds/freedesktop/stereo/bell.oga` | sound-theme-freedesktop 0.8-6 | CC-BY-3.0. Credit: Dr. Richard Boulanger et al, from http://www.archive.org/details/Berklee44v12 (the theme's `CREDITS` file; the package as a whole carries six licences, this file one). |
| `example-movie.mp4` | `/usr/share/texmf-dist/tex/latex/mwe/example-movie.mp4` | texlive-latexextra 2026.1-1 (the `mwe` package) | LPPL 1.3 or later, Martin Scharrer's `mwe` package: its `mwe.sty` header says "This work may be distributed and/or modified under the conditions of the LaTeX Project Public License, either version 1.3 of this license or (at your option) any later version." Arch lists the whole of texlive-latexextra as "GPL". |
| `color-profile.icc` | `/usr/share/ghostscript/iccprofiles/ps_gray.icc` | ghostscript 10.08.0-1.1 | AGPL-3.0-or-later. The package's `/usr/share/licenses/ghostscript/LICENSE` says: "The files in the base, psi, lib, toolbin, examples, doc, man and iccprofiles directories (folders), and pcl, xps and gpdl directories (folders), if included, and any subdirectories (sub-folders) thereof are part of GPL Ghostscript." and "GPL Ghostscript is free software; you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version." |

The copies were compared with the installed package files on 2026-11-01.
`graphviz-dot-manual.pdf` was copied again from graphviz 16.1.0-2, because the
copy made from 16.1.0-1 no longer matched the installed file.

## Made for this vault

| file | what | licence |
|---|---|---|
| `gradient.png` | a 120 by 60 pixel colour gradient generated in code, 14 618 bytes. The same bytes are stored as a cell output in `demo/Code/Notebook.ipynb`. | The project's own, GPL-3.0-or-later like the rest of the repository. |
| `sample.epub` | see "Generated for this vault" below. Its cover is `gradient.png`. | The project's own, GPL-3.0-or-later. |

## Checksums

SHA-256 of every binary in `demo/Embeds/assets/` as committed:

| file | bytes | sha256 |
|---|---|---|
| `graphviz-dot-manual.pdf` | 59 250 | `f6e6f1850082b08793e29f71a0fd388942c8be9f4c415b70c748c2628c0d2cf9` |
| `bell.oga` | 8 495 | `7bb1ae73f3db55d99ea1826f114ce161002ac71879ad4649d9e001bc4efb1bdc` |
| `example-movie.mp4` | 194 607 | `6ae61bde7f8023c4cf1a2e7415f255d9548455d5d00bd1d7f785e9b8e8d6835b` |
| `color-profile.icc` | 416 | `184719ed5960bc9350022c724e3c35be20569fc3b15be3e41cd42d219d4672a2` |
| `gradient.png` | 14 618 | `a6e6e6e7cdd71e709f30f7e4f29b5bc723e435ed5891ae43edd9c50540405497` |
| `sample.epub` | 17 159 | `06b21360e89e8abc72b9712a68c95c5750e26f4b57bd9a1549e8f40a6f153f62` |

The first four match the installed package files named above.

## Attribution and licences

Each of the package files is a separate work in the demo vault under its own
licence. They are not linked into or compiled into Notolith, so the
GPL-3.0-or-later of the program covers Notolith's own files and not these. The
EPL-licensed `graphviz-dot-manual.pdf`, and the others, are shipped alongside
the program as an aggregate, not combined with it. The demo vault is in the
source tree only; `cmake --install` does not copy it, and installs this file
under `share/doc/notolith/` for the record.

- `bell.oga`: "bell" from the freedesktop sound theme, copyright Dr. Richard
  Boulanger et al, licensed CC-BY-3.0 (https://creativecommons.org/licenses/by/3.0/),
  unmodified.
- The other three package files are unmodified copies; their copyright holders
  are those of the packages named above.

Total assets about 290 KB; the whole `demo/` vault is well under 5 MB.

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
`cover-image` item, a nav document, a stylesheet and two XHTML chapters), 17 KB.
Its cover is `gradient.png` above, reused unmodified. `demo/Code/Notebook.ipynb`
(nbformat 4.5, written with the `nbformat` library, one stored PNG generated
in code: the same bytes as `gradient.png`) and the scripts in
`demo/Code/assets/` (`greetings.py`, `countdown.sh`, `square.c`) are
hand-written text for the same reason as the files under "Text embeds".
