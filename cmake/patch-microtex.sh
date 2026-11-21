#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Two changes MicroTeX's Qt backend needs, and upstream's other backends do not.
#
# 1. Font merging. TeX fonts are symbol-encoded, so Qt substitutes another font
#    for the codepoints cmex10 and friends use.
#
# 2. Glyph rasterisation. MicroTeX builds every font at 1 pt and scales the
#    painter up instead - a ~19x world transform at our text size. Qt rasterises
#    a 1 pt glyph and scales the raster, so anything with detail or size comes
#    out blank: every large operator, radical, extensible delimiter and most
#    roman glyphs. Drawing the outline through QPainterPath makes the glyph
#    scale with the transform instead of against it.
#
# 3. A malloc freed with delete. LaTeX::queryResourceLocation frees asprintf's
#    buffer with `delete`, which AddressSanitizer reports on the first formula
#    rendered: undefined behaviour in upstream's own code, patched here.
# 4. A binary search one row too far. IndexedArray::operator() starts with
#    `h = _rows`, so once the key sorts after every row it reads row _rows,
#    past the end of the table (AddressSanitizer: global-buffer-overflow in
#    FontInfo::getKern, on a matrix formula).
set -e
file="src/platform/qt/graphic_qt.cpp"

grep -q NoFontMerging "$file" || {
  perl -pi -e 's|\Q_font.setPointSizeF(size);\E|_font.setPointSizeF(size);\n  _font.setStyleStrategy(QFont::NoFontMerging);|' "$file"
  echo "patched $file: Qt font merging disabled"
}

grep -q "QPainterPath path;" "$file" || {
  perl -pi -e 's|\Q#include <QFile>\E|#include <QFile>\n#include <QPainterPath>|' "$file"
  perl -pi -e 's|\Q  _painter->drawText(QPointF(x, y), text);\E|  QPainterPath path;\n  path.addText(QPointF(x, y), _font->getQFont(), text);\n  _painter->fillPath(path, QBrush(_painter->pen().color()));|' "$file"
  perl -pi -e 's|\Q  g.getQPainter()->drawText(QPointF(x, y), _text);\E|  QPainterPath path;\n  path.addText(QPointF(x, y), _font, _text);\n  g.getQPainter()->fillPath(path, QBrush(g.getQPainter()->pen().color()));|' "$file"
  echo "patched $file: glyphs drawn as outlines, not 1 pt rasters"
}

latex="src/latex.cpp"
grep -q "free(userdata_fallback);" "$latex" || {
  perl -pi -e 's|\Q    delete userdata_fallback;\E|    free(userdata_fallback);|' "$latex"
  echo "patched $latex: asprintf's buffer freed with free()"
}

indexed="src/utils/indexed_arr.h"
grep -q "h = (int)_rows - 1" "$indexed" || {
  perl -pi -e 's|\Qint     l = 0, h = _rows;\E|int     l = 0, h = (int)_rows - 1;|' "$indexed"
  echo "patched $indexed: binary search stays inside the table"
}
