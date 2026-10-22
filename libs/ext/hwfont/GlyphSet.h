#pragma once

#include <vector>

#include <QString>

namespace noteAppC::hwfont {

struct GlyphTarget {
    char32_t codepoint;
    int wanted; ///< how many samples the font wants for this glyph
};

/// The default set: ASCII letters and digits, Latin-1 punctuation, and the Slovene letters
/// c s z (with caron) in both cases. Letters (Slovene ones included) want `perLetter` samples,
/// everything else `perPunctuation` (settings hwfont.samplesPerLetter / hwfont.samplesPerPunctuation).
/// Ascending by code point.
std::vector<GlyphTarget> defaultGlyphSet(int perLetter, int perPunctuation);

/// "0061": four lower-case hex digits, six when the code point needs them.
QString glyphDirectoryName(char32_t codepoint);
/// The inverse; false for anything glyphDirectoryName would not have written.
bool codepointFromDirectoryName(const QString &name, char32_t *out);

} // namespace noteAppC::hwfont
