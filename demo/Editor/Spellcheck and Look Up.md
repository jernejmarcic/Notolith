---
tags: [demo, editor, spellcheck]
---

# Spellcheck and Look up

What to look for: words the dictionary does not know get a wavy underline in
the theme's error colour, in live preview and reading view; the right-click
menu offers suggestions, adds a word to your dictionary and, on a word you
added, takes it back out; and "Look up" shows a definition in a small popover.
Spellcheck is Settings > Editor > "Spellcheck" (on by default; it exists only in
a build with hunspell) and uses the hunspell dictionaries installed for the
languages your system asks for (`LANGUAGE`, then the locale), looked for in
`/usr/share/hunspell`, `/usr/share/myspell`, `hunspell/` under each XDG data
directory and `$DICPATH`. A word is right when any loaded dictionary accepts
it, so a note that mixes two languages is checked against both. With no
dictionary installed nothing is underlined and the status bar says "Spellcheck is
on, but there is no hunspell dictionary for" your languages.

## Misspelt words

The five misspelt words in this paragraph should be underlined, and nothing
else: I will recieve the seperate parcel tomorrow, which definately occured
before teh deadline. Right-click one: the menu starts with suggestions (click
one to replace the word) and "Add "recieve" to dictionary".

## What is checked and what is not

The word under the caret is not underlined while you are still typing it, and
the check never touches these, though each is misspelt on purpose: inline code
`recieve`, a link [definately](https://example.com), a
wikilink [[Welcome]] with a typoed alias [[Welcome|teh welcome]], a formula
$x = recieve$, snake_case_wrods and names with digits like abc12def. A
fenced block, a table and the properties are not prose, so they are skipped
as a whole:

```text
recieve seperate definately
```

| word |
|---|
| recieve |

Words inside a heading, a list item, a quote and a callout are checked:

- a list item with a mispelled word
> a quoted line with a mispelled word

> [!note]
> a callout with a mispelled word

In Source mode nothing is underlined at all.

## Your own dictionary

"Add "w" to dictionary" appends the word to
`~/.config/notolith/dictionary.txt`, one word per line, and remembers it for
the next start; the underline goes at once. Add "Notolith" here, a word no
dictionary has: Notolith. Right-click it afterwards: the menu now says
"Remove "Notolith" from dictionary", and choosing it puts the underline back.
(Delete the line in the file by hand to start clean.)

## Look up

Right-click a word, or select a short phrase (one line, up to 60 characters),
and choose "Look up "word"". A popover beside the pointer says "Looking
up..." at once and fills with the definitions when a DICT dictionary server
answers (WordNet definitions and synonyms on dict.org); Escape or a click
outside closes it. Try it on the word cartography, then select the phrase
ordinary language and look that up. With the keyboard, the menu key or
Shift+F10 opens the menu at the caret.

The menu's other two window items are "Search for ..." (the vault's search
pane, from Core plugins > "Search") and "Search the web for ..." (Settings >
Editor > "Web search", `%1` is the URL-encoded text; empty removes the item).
Each item is off with its own switch.

### What leaves the machine

Nothing is sent on a right click. "Look up" opens a plain-text TCP
connection to port 2628 of the first server that answers and sends the word
and nothing else (`DEFINE * "word"`, then `QUIT`). Servers come from Settings >
Editor > "Dictionary server" (`host` or `host:port`), else `~/.dictrc`, then
`/etc/dict/dict.conf`. With the file as shipped that is `localhost`, and when
`dictd` is not running, dict.org over the internet. To stay offline install
`dictd` with a database (`dict-gcide`, `dict-wn`) and enable it, or turn off
Core plugins > "Look up words" to remove the item and the client. With no
server reachable the popover says so after a short wait.
