#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
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

# A footnote line with a "|" in it followed by an indented line (`[^1]: a|b` and ` l`) makes the block parser
# discard and rewind to the same line for ever, queueing a context each time until memory runs out: a note of
# a few bytes hangs the editor and the indexer (found by tools/fuzz). A real document rewinds a few times per
# line it gets past, so when the rewinds outrun the lines read by a wide margin the parser stops rewinding
# and goes on from where it is.
python3 - src/parser.cpp <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
marker = "notolith: rewind cap"
if marker in text:
    sys.exit(0)
steps = [
    ("    stream.saveState();\n\n    while (!stream.atEnd()) {\n",
     "    stream.saveState();\n    long long rewinds = 0; // " + marker + "\n\n    while (!stream.atEnd()) {\n"),
    ("[&state, &line, &stream, &empty]() -> bool {",
     "[&state, &line, &stream, &empty, &rewinds]() -> bool {"),
    ("                if (state.m_context && state.m_context->firstLineNumber() != -1) {\n",
     "                if (state.m_context && state.m_context->firstLineNumber() != -1) {\n"
     "                    if (++rewinds > 64 * (line.lineNumber() + 1) + 1024) {\n"
     "                        state.m_state = BlockState::None;\n"
     "                        return false;\n"
     "                    }\n"),
]
for old, new in steps:
    if text.count(old) != 1:
        sys.exit("patch-md4qt: src/parser.cpp is not what the rewind cap expects: " + old.strip())
    text = text.replace(old, new)
open(path, "w").write(text)
print("patched " + path)
PY

# The same note shape, a footnote with a "|" in it, followed by a blank line and a second footnote, sends
# BlockParser::process round its do-while without the line moving, adding a table row to the queue each time.
# A block that is still not done after this many turns without taking a character of the line is cut off.
python3 - src/block_parser.cpp <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
marker = "notolith: stalled blocks"
if marker in text:
    sys.exit(0)
old = """    BlockState state = BlockState::None;

    do {
        state = processOneTime(currentLine, stream, doc, parent, ctx, path, fileName, linksToParse);
    } while (!ctx.children().isEmpty() && ctx.children().head().block() && !currentLine.atEnd());
"""
new = """    BlockState state = BlockState::None;
    int stalled = 0; // """ + marker + """

    do {
        const auto before = currentLine.position();
        state = processOneTime(currentLine, stream, doc, parent, ctx, path, fileName, linksToParse);
        stalled = currentLine.position() == before ? stalled + 1 : 0;
    } while (!ctx.children().isEmpty() && ctx.children().head().block() && !currentLine.atEnd() && stalled < 64);
"""
if text.count(old) != 1:
    sys.exit("patch-md4qt: BlockParser::process in src/block_parser.cpp is not what the stall cap expects")
open(path, "w").write(text.replace(old, new))
print("patched " + path)
PY
