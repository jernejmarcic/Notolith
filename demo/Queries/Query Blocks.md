---
tags: [demo, query]
---

# Query blocks

What to look for: each ` ```query ``` ` block below should show "Searching..."
briefly and then a list of matching notes with a highlighted snippet and a
match count, updating live if a matching note changes. The block's own text
is the whole search query, in Obsidian's search grammar.

## A plain word

```query
demo
```

## A tag

```query
tag:#embeds
```

## Restricted to a path

```query
path:Callouts
```

## Restricted to a file name

```query
file:Target
```

## Inside one line

```query
line:(demo vault)
```

## Inside one section

```query
section:(Overview Details)
```

## An open task

```query
task-todo:task
```

## A bad query

An empty parenthesis, which should fail to parse without crashing the note:

```query
line:()
```
