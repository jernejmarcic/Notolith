// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <utility>
#include <vector>

#include <QString>

#include "GlyphSet.h"
#include "InkFormat.h"

namespace noteAppC::hwfont {

struct GlyphMetrics {
    double baseline;
    double xHeight;
    double ascender;
}; // document px within the sample page

/// The samples of one font: <vaultRoot>/Handwriting/<fontName>/<hex>/<n>.ink, one .ink per capture so a
/// letter can have several hand-written variants. The font name comes from the user, so it is checked
/// (isValidFontName) before any path is built from it; an invalid name stores nothing.
class GlyphStore {
public:
    GlyphStore(const QString &vaultRoot, const QString &fontName);

    /// False for an empty name, one containing '/', '\\', ".." or a control character, or one starting with '.'.
    static bool isValidFontName(const QString &fontName);
    bool isValid() const { return isValidFontName(m_fontName); }

    QString fontDirectory() const; ///< empty when the name is invalid
    int sampleCount(char32_t codepoint) const; ///< files <dir>/<hex>/<n>.ink
    QString nextSamplePath(char32_t codepoint) const; ///< the lowest free n; empty when the name is invalid
    /// Copies `sample`'s settings and items into a scratch InkDocument, sets extra["glyph"] = {codepoint,
    /// baseline, xHeight, ascender} there, and writes it with writeInk() through the vault's no-follow
    /// writer (missing folders are created). `sample` is not modified.
    bool saveSample(char32_t codepoint, const ink::InkDocument &sample, const GlyphMetrics &metrics, QString *error);
    /// (target, have), needs-more first, then by codepoint.
    std::vector<std::pair<GlyphTarget, int>> progress(const std::vector<GlyphTarget> &set) const;

    static ink::DocumentSettings samplePage(); ///< Fixed, 400 x 400, pattern None, white
    static GlyphMetrics defaultMetrics();      ///< baseline 280, xHeight 160, ascender 100 on the 400 px page

private:
    QString glyphDirectory(char32_t codepoint) const;
    QString m_vaultRoot;
    QString m_fontName;
};

} // namespace noteAppC::hwfont
