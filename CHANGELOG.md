# Changelog

## 0.1.0-beta.1

The first public release, a beta: expect bugs, and back up your vault.

- Markdown editor with live preview, source and reading modes; wikilinks, embeds, callouts, properties, tags and `query` blocks.
- Opens and works with Obsidian vaults.
- SQLite full-text index, backlinks, outline, graph view, canvas, bases, daily
  notes, templates, workspaces, file recovery.
- PDF viewer with annotation, image annotation, EPUB reader, spellcheck,
  Git-backed note history, static-site publishing, pandoc export and import.
- Code blocks, Jupyter kernels, Mermaid, PlantUML, TikZ and Graphviz, all run
  in a bubblewrap and seccomp sandbox, with a trust gate for vaults.
- The default build makes no network connections and has no encryption.
  Sync, note sharing, encrypted notes, the AI plugin and remote content are
  CMake options (`docs/OFFLINE.md`).
- Previews: plugins, sync, sharing and encrypted notes. Handwriting is not in this release.
- Arch Linux package (`packaging/arch/PKGBUILD`).
- Images are PNG, JPEG, GIF, BMP, WebP, AVIF and SVG, told by their bytes; other formats (HEIC, TIFF, PSD...) show as a missing attachment.
- A note that is a symbolic link is not written through; saving from the editor replaces the link with a regular file.
- Installed modules and helper programs live under /usr/lib/notolith/; custom themes in a vault go in .obsidian/notolith-themes (the old folder is still read).
