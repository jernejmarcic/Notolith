---
tags: [demo, editor, plugins]
---

# Community plugins

What to look for: Settings > Community plugins (the switch is off by default)
loads plugins from `~/.local/share/notolith/plugins/<id>/`. Each is a
`plugin.ini` and a shared library, run in its own sandboxed process, started
only when something it declared is used, with only the permissions its
`plugin.ini` asks for and you grant. There are no JavaScript plugins.

## The example plugin

`examples/plugins/word-stats/` in the project is a small C plugin. Built and
copied to the plugins folder, it adds the command "count-words" to the palette
and a `calc` code block language. Its `plugin.ini` declares `read-notes = true`
and nothing else, so it cannot write, reach the network or touch the clipboard.

## The AI plugin

`plugins/ai/` is a first-party plugin for a local Ollama server or an
OpenAI-compatible or Anthropic API. Its commands (summarize, ask a question,
rewrite, continue writing, translate, proofread) appear in the palette once it
is installed and enabled. A fenced `ai` block uses its source as the prompt:

```ai
Say hello in three words.
```

Without the plugin that block is plain code, as above. Its settings, including
the *name* of an environment variable that holds an API key (never the key),
live in `~/.config/notolith/plugins/ai/ai.ini`, outside the vault.

Plugins are a runtime extra, so this vault cannot ship one. Build the project
and see `docs/PLUGINS.md`.
