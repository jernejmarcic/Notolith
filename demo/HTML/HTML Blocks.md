---
tags: [demo, html]
---

# HTML blocks

What to look for: raw HTML below renders through litehtml
(`NOTEAPPC_WITH_HTML`), cached as an image in the theme's own colours; the
`<script>` block must never execute - there is no way for this note to
prove that visually except that nothing bad happens, so check the console/log
for any sign it ran. Without `NOTEAPPC_WITH_HTML` built, expect these blocks
to show as plain unrendered HTML text instead.

## A styled div

<div style="border: 1px solid #888; padding: 12px; border-radius: 6px; background: rgba(128,128,128,0.08);">
  <strong>A styled box.</strong> Border, padding, radius and a translucent
  background, all inline styles.
</div>

## A table

<table>
  <tr><th>Column A</th><th>Column B</th><th></th></tr>
  <tr><td>1</td><td>one</td><td></td></tr>
  <tr><td>2</td><td>two</td><td></td></tr>
</table>

## Details and summary

<details>
  <summary>Click to expand</summary>
  <p>Hidden content, shown once the disclosure triangle is opened.</p>
</details>

## Inline formatting elements

Inline HTML formatting inside a paragraph: <b>bold</b>, <i>italic</i>,
<u>underline</u>, <s>strikethrough</s>, <kbd>Ctrl+K</kbd>, <code>code</code>,
<mark>marked</mark>, H<sub>2</sub>O, E=mc<sup>2</sup>, and
<span style="color: #e05252;">coloured text</span>.

## A script that must not run

<script>document.title = "HTML BLOCK RAN - THIS SHOULD NEVER APPEAR";</script>

If the window's title changed to the text above, raw HTML is executing
script content, which must never happen.
