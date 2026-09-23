---
tags: [demo, unicode]
---

# Unicode and scripts

What to look for: every script below should render with correct shaping and
direction - right-to-left text should actually flow right to left and mix
correctly with the Latin punctuation around it, combining marks should sit
on their base character rather than as a separate glyph, and the long
compound word should wrap rather than overflow the readable line width.

## CJK

Chinese: 你好，世界。这是一个笔记应用程序的示例文本，用来测试中日韩文字的渲染。
Japanese: こんにちは世界。これはノートアプリのテスト用テキストです。
Korean: 안녕하세요 세계. 이것은 노트 애플리케이션의 테스트 텍스트입니다.

## Right-to-left

Arabic: مرحبا بالعالم. هذا نص تجريبي لتطبيق تدوين الملاحظات، يُستخدم لاختبار
الكتابة من اليمين إلى اليسار.

Hebrew: שלום עולם. זהו טקסט לדוגמה עבור יישום רישום הערות, המשמש לבדיקת כתיבה
מימין לשמאל.

Mixed direction: the English word "Obsidian" inside an Arabic sentence -
هذا التطبيق يُسمى Obsidian وهو برنامج لتدوين الملاحظات - should keep
"Obsidian" reading left-to-right while the rest of the line flows right-to-left.

## Combining marks

Base letters with combining diacritics, composed at render time rather than
pre-composed: e + combining acute = é (é), a + combining ring = å
(å), n + combining tilde = ñ (ñ). Zalgo-style stacked combining
marks, several on one base character: Z̴̢̛̪̘̣̈́̏͐͜a̶̡̧̛̗͙̜͐̈́̚l̸̰̣̥̈́̽̅g̷̱̈́̑ő̸̼̞.

## Long words

German compound words can get very long and should wrap rather than force
horizontal scrolling:
Donaudampfschifffahrtsgesellschaftskapitänswitwenrentenversicherungsanstalt.
A long URL-like unbroken token should also wrap rather than overflow:
https://example.com/a/very/long/path/that/keeps/going/and/going/and/does/not/naturally/break/anywhere/obvious.

## Filenames

Two notes in this folder push filenames past plain ASCII: [[File With Spaces In Its Name]]
and [[Café Déjà Vu]]. Both should open, index, link and search normally.
