# Pandoc: export and import

The core plugin `pandoc` (Settings > Core plugins > Pandoc) turns the open note into Word, OpenDocument,
EPUB, HTML, LaTeX, RTF, PowerPoint or Beamer slides - or any other format pandoc writes - and turns a
`.docx`, `.odt`, `.epub`, `.html`, `.rtf`, `.tex` (and `.org`, `.rst`, `.textile`) file into a new note.

pandoc itself is a separate program, never a build dependency: install it (`pacman -S pandoc`,
<https://pandoc.org/installing.html>) or give its path in Settings. It is looked for only when a pandoc
command is used or the settings are opened, never at start-up, and with the plugin off nothing in
`libs/ext/pandoc` is called.

## Commands

All in the command palette; the same formats are in the file explorer's context menu of a note
("Export with pandoc"), which also has "Import with pandoc" on an importable file. Dragging an importable
file from a file manager onto the file explorer imports it into the folder it was dropped on.

| command | |
|---|---|
| `pandoc:export-docx` ... `-odt -epub -html -latex -rtf -pptx -beamer` | "Pandoc: Export as Word (.docx)" and so on |
| `pandoc:export-other` | asks for a pandoc output format name (`rst`, `org`, `markdown+smart`, ...); a name that is not letters, digits, `_`, `+` and `-` is refused, so it can never be read as an option |
| `pandoc:import` | picks a file; the note is made in the new-note folder |

With no pandoc the plugin is still listed, off and greyed, in Core plugins; the commands are still in the
palette and the file explorer's menu (drawn in the disabled colour, with a tooltip, but still clickable); activating any of them - the switch, a menu entry, a command - opens a
dialog saying it depends on pandoc, where it looked, how to install it, and a button to the path
setting. Once pandoc is found the switch works again.

## Settings

| key | |
|---|---|
| `pandoc.path` | empty: `pandoc` on PATH (and `~/.local/bin`, `~/.cabal/bin`, `/usr/local/bin`); a name or a full path |
| `pandoc.extraArguments` | added to every export after Notolith's own (an import takes none: a `--toc` has no business in an imported note), split like a shell would (double quotes group), run by no shell |
| `pandoc.referenceDoc` | a `.docx` whose styles an exported `.docx` takes (`--reference-doc`) |

## Export

The note is turned into the CommonMark pandoc reads (`-f commonmark_x-smart`: divs, spans, footnotes, `$`
maths, tables, task lists; no smart punctuation) and sent to pandoc's standard input; the output goes to
the file you chose. Nothing passes through a shell: the program and an argument list go to `QProcess`.
The run is asynchronous with a cancel button and a two-minute limit; on failure pandoc's own stderr is shown.
A note with no `title` property is titled by its name (`--metadata=title=`).

What `libs/ext/pandoc/PandocPrepare.cpp` changes (code, maths and the frontmatter are never touched; it
works on the parser's own ranges, so a `[[x]]` in a code fence stays):

| Notolith | pandoc gets |
|---|---|
| `[[Note\|alias]]`, `[[Note#Heading]]` | `[alias](<relative/path/Note.md#heading-id>)`; no such note: only the text |
| `[[#Heading]]` | `[Heading](#heading-id)`, the id pandoc gives (GitHub's rule); `[[#^block]]`: its text |
| `[[https://x\|y]]` | `[y](<https://x>)` |
| `![[pic.png]]`, `![[pic.png\|300]]`, `\|300x200`, `\|alt text` | `![alt](<absolute path>){width=300 height=200}` |
| `![[Other note]]` | the note's text **inlined** in place, without its frontmatter; `#Heading` its section, `#^id` its block; three levels deep, a cycle becomes a link |
| `![[file.pdf]]`, `![[data.csv]]`, in the middle of a line | a link to the file |
| `> [!note]+ Title` and its body | the quote with `**Title**` as its first paragraph (the type's name when there is no title); the type is not kept |
| `:::note title="x"` | `::: {.callout .callout-note}` with `**x**` first |
| `==text==` | `[text]{.mark}` (Word: yellow highlight, HTML: `<mark>`, LaTeX: `\hl`) |
| `%%comment%%`, `^block-id` | dropped |
| ` ```mermaid ` (also plantuml, tikz, dot) | the picture, when the editor has already drawn it (never started again), as a PNG; otherwise the listing |
| `$x$`, `$$..$$`, footnotes, tasks, tables | as written |

Images are resolved the way the editor does (index, then beside the note, the vault root, the attachment
folder) and passed as absolute paths, with the note's folder, the vault root and the attachment folder as
`--resource-path` (one flag each, so a folder with a `:` in its name is safe) for the note's own
`![](relative.png)`. A target that is a note of the vault, or the note itself, is refused, and a Markdown writer's
suggested name is `Note (gfm).md`, never the note's own.

Formats: `docx` (`--reference-doc` if set), `odt`, `epub3`, `html5 --standalone --embed-resources`
(one file, images inside, maths as MathML), `latex --standalone`, `rtf --standalone`, `pptx`,
`beamer --standalone` (`.tex`).

**Network.** Nothing in Notolith asks pandoc to fetch anything. But a note's own *remote* image
(`![](https://...)`) is fetched by pandoc itself when the output has to contain it (docx, odt, epub,
`--embed-resources` HTML). An import fetches nothing: it runs under `--sandbox` (below).

**Local files.** An image the note names is read by pandoc from wherever the path points, and embedded in
docx, odt, epub and `--embed-resources` HTML - inside the vault or not, and whatever the file really is (pandoc
does not check that `![](notes.txt)` is a picture). Notolith resolves `![[...]]` through the vault, but a note's
own `![](/any/path)` or `![](../outside.png)` is passed on as written, so an export holds whatever its note
points at: treat an exported file as you would the note, and read a note from someone else before exporting it.

**Where the time goes.** The preparation above (parsing the note, inlining the notes it embeds, resolving links
and images) runs on the UI thread before pandoc starts; pandoc itself is another process, off the UI thread, with
a cancel button. For a normal note the preparation is milliseconds; a note that embeds a great many notes, or
embeds them deeply, can hold the window for a moment. Moving it to a worker needs the index to be read from one,
and has not been measured as a problem.

**Lossy:** the callout type and folding; `%%` comments and block ids by design; a note embedded in the middle
of a line becomes a link; links to other notes are relative file links (live in HTML, dead in Word);
Obsidian's "every newline is a line break" is not applied (a single newline joins lines, as CommonMark has
it); tags are plain text; a diagram the editor never drew is a listing; embedded canvases, bases, queries
and notebooks are links or listings.

## Import

`pandoc -f <reader> -t gfm-tex_math_gfm+tex_math_dollars --wrap=none --standalone --extract-media=. --sandbox in -o out.md`
run in a scratch folder. (Plain `gfm+tex_math_dollars` writes maths as GitHub's `` $`x`$ ``, which a note
does not render; `-tex_math_gfm` turns that off.) Only when pandoc has succeeded is the note made, in the
vault's own way: `createNote` numbers a taken name ("Name 1"), so nothing is ever overwritten. The media
pandoc extracted are copied into the attachment folder of the vault settings (never over a file), and
`![](media/x.png)` / `<img ... style="width:2in">` become `![[x.png|192]]`; pandoc's
`<span class="mark">` becomes `==text==`. Everything pandoc extracted (docx and epub `media/`, ODT `Pictures/`, an HTML file's hashed names) is copied, not `media/` alone.
The file imported is never written to. The document's
title and author arrive as frontmatter.

**The sandbox.** A document may come from anyone, and without `--sandbox` a `.tex` with `\input{/home/me/secret}`,
an rst `.. include::` or an org `#+INCLUDE` would put that file's text into the note, and an HTML `<img>` could
pull a file or a URL in. `--sandbox` stops all three. The trade-off is that pandoc then reads no picture beside an
HTML or LaTeX file either; Notolith copies those itself, only picture files (png, jpg, gif, svg, webp, bmp, avif)
named by a *relative* path that, after symlinks, is inside the document's own folder. Remote pictures stay links.
An import never uses the extra-arguments setting. (The export cannot run sandboxed: this pandoc build then fails
to find its own docx template.) A picture that cannot be copied is reported once, after the note is made, and the
note says so where it would have been instead of linking a file that is not there.

**Pandoc itself runs inside the same bubblewrap and seccomp sandbox as code blocks** (no network, no home directory,
a private `/tmp` and `/dev/shm`, CPU, memory and file-size limits, a 1 GiB cgroup where systemd has a user manager, a
timeout). A link in the scratch directory, where pandoc's output is looked for, is never taken for it. An import sees only the one file it was given. An
export sees the note's folder, the vault, its attachment folder and the reference document read-only, and writes to a
scratch directory that is copied to the chosen target when pandoc succeeds. Without `bwrap` and libseccomp neither
runs; there is no unsandboxed path.

**Several files** dropped at once are imported one after another; cancelling one stops the rest.

**Lossy:** comments, tracked changes, headers and footers, styles beyond headings, lists, tables,
emphasis and code; tables that Markdown cannot express become HTML; an image's size is its width only.

## Cost

Off: no action is enabled, nothing is looked up, the file explorer takes no foreign drops. On: one action
object per command and a few `stat` calls when a menu, the palette or Settings is opened; pandoc itself
starts only for an export or import. The library is a static archive of four small files.

## Tests

`tests/test_pandoc.cpp` (ctest `pandoc`): the pre-conversion as pure functions, the argument list for
every format, import rewriting, media copying, the process runner (failure, cancel, timeout), a missing
pandoc, and - when pandoc is installed, else skipped - a note -> docx -> Markdown round trip.
`FeaturesTest.PandocMissing*` in `tests/test_window.cpp`: the missing-pandoc state of the plugin
switch, the commands and the dialog.
