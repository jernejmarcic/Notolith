#!/bin/sh
# MicroTeX's Qt backend draws glyphs with QPainter::drawText and leaves font
# merging on. TeX fonts are symbol-encoded, so Qt substitutes another font for
# the codepoints cmex10 and friends use, and every large operator, radical and
# delimiter comes out blank. Two lines upstream has no reason to add for its
# other backends.
set -e
file="src/platform/qt/graphic_qt.cpp"
grep -q NoFontMerging "$file" && exit 0
sed -i 's|_font.setPointSizeF(size);|_font.setPointSizeF(size);\n  _font.setStyleStrategy(QFont::NoFontMerging);|' "$file"
echo "patched $file: Qt font merging disabled"
