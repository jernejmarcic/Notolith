#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QString>

namespace noteAppC {

/// Raw block-level HTML (`<div>`, `<table>`, `<details>`, `<img>`, `<kbd>`,
/// `<span style="...">`...) rasterised by litehtml
/// (github.com/litehtml/litehtml, BSD-3-Clause) in the theme's own colours
/// and fonts.
///
/// Kept behind this small surface, the way PdfPage keeps poppler out of the
/// editor: litehtml's headers, and the option that makes it optional
/// (NOTEAPPC_WITH_HTML), stay entirely inside this one translation unit.
/// libs/editor never sees a single litehtml type.
///
/// Nothing this class draws ever runs a script or reaches the network.
/// litehtml has no script engine of its own - there is no JavaScript
/// anywhere in this application - and `render` strips `<script>` bodies
/// outright and hides `<iframe>`/`<object>`/`<embed>` before the markup ever
/// reaches litehtml, so their content can never end up on screen even as
/// literal text. A remote `<img src="https://...">` is never fetched; only a
/// path under `baseDir` loads. See the .cpp for how, and for why this is a
/// container written from litehtml's own document_container.h rather than
/// adapted from Qt Creator's qlitehtml.
class HtmlRenderer {
public:
    /// The pieces of the editor's theme a rendered HTML block needs, so this
    /// class never has to know about noteAppC::Theme.
    struct Theme {
        QColor text;
        QColor background;
        QColor link;
        QColor border;
        QColor codeBackground;
        QFont textFont;
        QFont codeFont;
    };

    /// Renders `html` wrapped to `widthPixels`. A relative `<img src>`
    /// resolves against `baseDir` (the note's own directory); a remote one
    /// draws nothing, per the class comment. Null when litehtml is not
    /// compiled in, or the markup laid out with no visible content - either
    /// way the caller falls back to showing the source, the way a formula
    /// MicroTeX cannot draw does.
    static QImage render(const QString &html, const Theme &theme, int widthPixels,
                         const QString &baseDir, qreal devicePixelRatio = 1.0);

    /// False in a build configured without litehtml (NOTEAPPC_WITH_HTML off,
    /// or the package was not found).
    static bool isAvailable();

    /// The `<script>`/`on*=` stripping render() already applies before ever
    /// handing markup to litehtml, exposed for callers that want sanitised
    /// HTML *text* rather than a rasterised image - the static site exporter
    /// (libs/ext/publish) is the first one. Available unconditionally, even
    /// in a build with litehtml itself missing: it is a text transform, not
    /// a render.
    static QString sanitizeHtml(const QString &html);

    /// How many times render() has actually built a litehtml document, since
    /// process start. Test-only: it is what proves a note with no HTML block
    /// never constructs one at all, and that a rendered block's image is
    /// reused rather than rebuilt on an unrelated keystroke - see
    /// HTMLBlockTest in tests/test_editor.cpp.
    static qint64 renderCount();
};

} // namespace noteAppC
