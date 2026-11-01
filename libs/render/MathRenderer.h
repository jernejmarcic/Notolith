// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QColor>
#include <QImage>
#include <QString>

#include <memory>

namespace noteAppC {

/// LaTeX, laid out by MicroTeX - a real TeX box model, not a font trick.
///
/// Obsidian writes display maths between `$$` and inline maths between `$`;
/// both arrive here as the formula text with the delimiters already stripped.
/// Everything is native C++: there is no MathJax and no KaTeX, because there
/// is no JavaScript anywhere in this application.
class MathRenderer {
public:
    MathRenderer();
    ~MathRenderer();

    /// Renders `latex` at `pointSize`, wrapped at `maxWidth` pixels. Returns a
    /// null image when the formula does not parse, which the caller shows as
    /// the raw source rather than as an error.
    QImage render(const QString &latex, qreal pointSize, const QColor &colour,
                  int maxWidth, qreal devicePixelRatio = 1.0) const;

    /// The same layout, as scalable SVG markup instead of a raster image, for
    /// a static export that wants maths to stay crisp with no JavaScript
    /// (MathJax/KaTeX) and no raster at a fixed size. Same box model, same
    /// MicroTeX call, only the paint device differs - see the .cpp. Empty
    /// string on the same failures render() returns a null image for.
    QString renderSvg(const QString &latex, qreal pointSize, const QColor &colour,
                      int maxWidth) const;

    /// Why the last render returned null. Empty when it succeeded.
    QString lastError() const;

    /// False when the resources MicroTeX needs were not found, in which case
    /// every render returns null.
    static bool isAvailable();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace noteAppC
