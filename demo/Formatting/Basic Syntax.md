---
tags: [demo, formatting]
---

# Basic syntax

What to look for: every inline style below should be visually distinct in
both live preview and reading view, and the raw markers (`**`, `~~`, `==`,
`` ` ``) should hide in live preview except right at the caret.

## Emphasis and strong

*Italic with asterisks* and _italic with underscores_ should look the same.
**Bold with asterisks** and __bold with underscores__ should look the same.
***Bold and italic together*** combines both. A **bold sentence with *italic
inside* it** nests correctly.

## Strikethrough and highlight

~~This text is struck through.~~ ==This text is highlighted.== Obsidian
supports both natively; a regression here usually means the span parser
stopped closing one of the two-character markers correctly.

## Inline code

Use `inline code` for short snippets, like `noteAppC::Vault::read()`. A
backtick pair containing a literal backtick needs doubling: ``code with a `
backtick inside``.

## Escapes

Characters can be escaped so they render literally instead of as markup:
\*not italic\*, \[not a link\], \`not code\`, \# not a heading, 1\. not a
list item. The backslash itself is escaped as \\.

## Comments

%%This is an Obsidian comment and must not render at all, in either live
preview or reading view.%% Text before and after a comment%%like this one%%
should still read as one sentence once the comment is hidden.

## Footnotes

Footnotes carry extra detail without breaking the flow of a sentence.[^1]
Obsidian also supports inline footnotes.^[An inline footnote defined right
where it is used.] A second reference to the same note works too.[^1]

[^1]: The footnote body, which can itself contain **formatting** and a
    [[Target Note|link]].

## Horizontal rules

Three or more of `-`, `*` or `_` on their own line all make a rule:

---

***

___

## Line breaks

Obsidian's default is "strict line breaks off": every single newline is a
line break, so this line stands under the one above it.
Turn "Strict line breaks" on and the lines of a paragraph join into one,
unless a line ends with two trailing spaces:  
like this one, which ends with two spaces.

A blank line always starts a new paragraph, regardless of the setting:

This is a new paragraph.

## HTML entities

Named and numeric entities decode: &amp; &lt; &gt; &copy; &mdash; &hellip;
&#65; &#x2764;. A raw `<` or `&` that is not part of an entity or a tag is
left alone: 3 < 5 and Q&A.
