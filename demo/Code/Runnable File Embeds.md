---
tags: [demo, code, embeds, notebooks]
---

# Runnable file embeds

What to look for: a `![[...]]` embed of a source file whose language can run
(py, js, sh, rb, pl, lua, c, cpp, hs, java, rs and a few more) is drawn as the
file's listing in a labelled panel, like any source embed, and also has a run
control at the right of its header, with the output under it, exactly as a
fenced code block has. Settings > Core plugins > "Notebooks" decides whether
it does (on by default); off, the embed is the plain listing. The file runs
under the same sandbox as a block: no network, none of your files, a
throwaway `HOME`, with the file's own folder readable. There are no `#|`
options for a file on disk (it has no fence), so the run is always sandboxed
and never in a kernel unless Settings asks for shared kernels. The scripts are
in `assets/`, beside this note.

## A whole file

Run it: `one`, `two`, `three`, `sum 6`, `done`, one per line.

![[greetings.py]]

## A line range

`#L2-L3` narrows the embed to lines 2 and 3, and the label in the header
changes to `greetings.py:2-3`. What runs is what is shown: the two lines, so
only `one` and `two` are printed.

![[greetings.py#L2-L3]]

## One line

`#L5` is a single line, labelled `greetings.py:5`. Output: `sum 6`.

![[greetings.py#L5]]

## A range without the second L

`#L4-6` works the same as `#L4-L6` (the label reads `greetings.py:4-6`).
Output: `three`, `sum 6`, `done`.

![[greetings.py#L4-6]]

## A range that runs past the end

A last line beyond the file is clamped to the file's end, so this shows lines
5 to the last line:

![[greetings.py#L5-L99]]

## A shell script

`.sh` is run by bash. Output: `3`, `2`, `1`, `liftoff`.

![[countdown.sh]]

The loop alone, `#L3` (a shell `for` on one line):

![[countdown.sh#L3]]

## A C file

Compiled in a temporary directory with `gcc`, then run. Output: four lines,
`1 squared is 1` to `4 squared is 16`.

![[square.c]]

## Compare with a source embed that is not runnable

`data.json` has no language to run, so it is the plain panel with no run
control:

![[data.json]]
