---
tags: [demo, callouts]
---

# Callouts

What to look for: each of Obsidian's 13 built-in types below should get its
own icon and accent colour, an alias should look identical to its canonical
type, `-`/`+` callouts should start folded/open and be clickable to toggle,
nesting should keep each level's own colour, and the two styled-inline
examples at the bottom should restyle without any `callouts.ini` file -
remove `~/.config/noteAppC/callouts.ini` (there is none by default) and they
still look right, because the style rides in the `|` slot / `:::` braces
that Obsidian itself parses and ignores.

## The 13 built-in types

> [!note]
> note - the default look other types are compared against.

> [!abstract]
> abstract - aliases below should look identical to this.

> [!summary]
> summary - alias of abstract.

> [!tldr]
> tldr - alias of abstract.

> [!info]
> info.

> [!todo]
> todo.

> [!tip]
> tip - aliases below should look identical to this.

> [!hint]
> hint - alias of tip.

> [!important]
> important - alias of tip.

> [!success]
> success - aliases below should look identical to this.

> [!check]
> check - alias of success.

> [!done]
> done - alias of success.

> [!question]
> question - aliases below should look identical to this.

> [!help]
> help - alias of question.

> [!faq]
> faq - alias of question.

> [!warning]
> warning - aliases below should look identical to this.

> [!caution]
> caution - alias of warning.

> [!attention]
> attention - alias of warning.

> [!failure]
> failure - aliases below should look identical to this.

> [!fail]
> fail - alias of failure.

> [!missing]
> missing - alias of failure.

> [!danger]
> danger - aliases below should look identical to this.

> [!error]
> error - alias of danger.

> [!bug]
> bug.

> [!example]
> example.

> [!quote]
> quote - alias below should look identical to this.

> [!cite]
> cite - alias of quote.

## Titled

> [!tip] A callout with its own title
> The `[!tip]` marker's icon and colour still apply; only the heading text changed.

## Foldable, open and closed

> [!warning]+ Open by default
> The `+` means this callout starts open and can be clicked closed.

> [!warning]- Closed by default
> The `-` means this callout starts folded; this body is hidden until it is clicked open.

## Nested, each with its own colour

> [!tip] Outer (tip)
> Outer body text before the nested callout.
> > [!danger] Inner (danger)
> > Inner body text, long enough to wrap onto a second line so the nested
> > indentation is visible against the outer callout's own left rail.
> >
> > > [!note] Innermost (note)
> > > A third level, to confirm nesting is not capped at two.
>
> Back to the outer callout's body, after an empty `>` line stepped back out
> of the nested ones - without it CommonMark would have folded this paragraph
> into the inner callout instead.

## A callout containing a code block

> [!example] Code inside a callout
> A fenced block keeps its own font, panel and highlighting inside the callout:
> ```python
> def inside_a_callout():
>     return True
> ```
> And a sentence after it, still inside the callout.

## A custom type (not one of the 13)

> [!deadline] Not a built-in type
> An unrecognised type still renders as a callout - generic styling unless a
> `[deadline]` section exists in `~/.config/noteAppC/callouts.ini`
> (`docs/STYLING.md`).

## The `:::` container form

::: tip A colon-fence callout
Obsidian does not have this form, but the "Ideas" spec this app also reads
does; it renders exactly like the `> [!tip]` form above.
:::

::: details Foldable via `:::`, closed by default
The `details` type folds like `-` does on the `>` form.
:::

## Styled without any config file (noteAppC's own extension)

The `|` slot after a callout's type is where noteAppC reads its own style
keys; Obsidian parses and ignores that slot, so this note still opens there
unstyled:

> [!note|bar=#FF5A5Fff bg=#FF5A5F26 heading-color=#FF9A9Eff] Styled inline, no config file needed
> Colour, background and heading colour set right here on the callout.

The same keys work as `:::` braces:

::: {.tip bar="#4C8DFFff" bg="#4C8DFF1A" title="Styled container, no config file needed"}
Same idea, container form.
:::
