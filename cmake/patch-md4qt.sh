#!/bin/sh
# md4qt v5.1.4 builds a QString from a QStringView and from a QChar-from-UCS-4
# implicitly, which Qt 6.8 allows and Qt 6.7 does not. The Android Qt kit is
# 6.7.3, so without this the parser - and everything above it - cannot be built
# for a phone. Each replacement is the explicit spelling of the same conversion,
# and compiles on 6.11 as well.
set -e

replace() {
    file="$1"; from="$2"; to="$3"
    grep -qF -- "$to" "$file" && return 0      # already patched
    grep -qF -- "$from" "$file" || { echo "patch-md4qt: '$from' not in $file" >&2; exit 1; }
    python3 - "$file" "$from" "$to" <<'PY'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
open(path, 'w').write(text.replace(old, new))
PY
    echo "patched $file"
}

replace src/text_stream.h \
    'return QString(view().sliced(pos, len)).replace(' \
    'return view().sliced(pos, len).toString().replace('
replace src/atx_heading_parser.cpp \
    'auto s = QString(currentLine.view());' \
    'auto s = currentLine.view().toString();'
replace src/utils.cpp \
    'const auto value = QString(QChar::fromUcs4(c));' \
    'const auto value = QString::fromUcs4(&c, 1);'
