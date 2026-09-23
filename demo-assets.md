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
Total assets 269 KB; the whole `demo/` vault is well under the 5 MB cap (see
the report for the exact `du -sb`).

Picked for size and a permissive-enough licence over the first match found:
`graphviz-dot-manual.pdf` because it is a real 9-page document, small enough
to keep the vault tiny, and lets `#page=` arguments (single, list, range,
mixed, `all`) all be demonstrated against the same file. `color-profile.icc`
is deliberately an extension noteAppC does not recognise as image, audio,
video or PDF, so it is the "unknown binary" embed - a generic file card.
No small video existed anywhere obviously free of a licence question except
the LaTeX example clip, which is GPL and tiny; nothing else under 2 MB turned
up outside game asset packs whose per-asset licence was unclear, so those
were skipped in favour of this one.

## Text embeds (not from this table)

`demo/Embeds/assets/script.py`, `main.cpp`, `data.json`, `maths-only.tex`
and `full-document.tex` are written for this vault - they are source text,
not an image/PDF/audio/video, so nothing was copied for them.
