// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "GlyphSet.h"

#include <algorithm>

namespace noteAppC::hwfont {

namespace {
constexpr char32_t kLatin1Punctuation[] = {0xA1, 0xA7, 0xAB, 0xB6, 0xB7, 0xBB, 0xBF}; // inverted !, section, guillemets, pilcrow, middle dot, inverted ?
constexpr char32_t kSlovene[] = {0x10D, 0x161, 0x17E, 0x10C, 0x160, 0x17D};          // c s z and C S Z with caron

bool isASCIIletter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
} // namespace

std::vector<GlyphTarget> defaultGlyphSet(int perLetter, int perPunctuation)
{
    std::vector<GlyphTarget> set;
    const auto addLetter = [&](char32_t c) { set.push_back({c, perLetter}); };
    const auto addOther = [&](char32_t c) { set.push_back({c, perPunctuation}); };
    for (char32_t c = 0x21; c <= 0x7E; ++c)
        isASCIIletter(c) ? addLetter(c) : addOther(c); // digits and ASCII punctuation
    for (const char32_t c : kLatin1Punctuation)
        addOther(c);
    for (const char32_t c : kSlovene)
        addLetter(c);
    std::sort(set.begin(), set.end(), [](const GlyphTarget &a, const GlyphTarget &b) { return a.codepoint < b.codepoint; });
    return set;
}

bool isValidCodepoint(char32_t codepoint)
{
    return codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
}

QString glyphDirectoryName(char32_t codepoint)
{
    return QStringLiteral("%1").arg(uint(codepoint), codepoint > 0xFFFF ? 6 : 4, 16, QLatin1Char('0'));
}

bool codepointFromDirectoryName(const QString &name, char32_t *out)
{
    if (name.size() != 4 && name.size() != 6)
        return false;
    bool ok = false;
    const uint value = name.toUInt(&ok, 16);
    if (!ok || !isValidCodepoint(value) || glyphDirectoryName(value) != name) // canonical only: lower case, no sign, no padding
        return false;
    if (out)
        *out = value;
    return true;
}

} // namespace noteAppC::hwfont
