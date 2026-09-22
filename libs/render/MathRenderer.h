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
