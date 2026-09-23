#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QRectF>
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

    /// One run of text as litehtml actually drew it: its box in the image's
    /// own logical-pixel space (the same space `widthPixels` is measured in;
    /// divide `devicePixelRatio` back out of the image before comparing), and
    /// `[offset, offset + text.size())` into the *reading-order text* of this
    /// render() call - every run's text, concatenated in the order litehtml
    /// drew it, with one synthetic separator character inserted wherever a
    /// run starts a visibly new line from the one before (litehtml does not
    /// itself draw the whitespace between block elements, so two adjacent
    /// paragraphs would otherwise fuse into one word). That order is fixed by
    /// the document tree, not by `widthPixels` - only where a run wraps onto
    /// a new line depends on the width, not the order or content of the
    /// runs - so an offset recorded at one width still names the same text
    /// after a re-render at another. The EPUB reader (libs/ext/epub) is the
    /// one caller that asks for this: it is what turns a click into a
    /// character position, a drag into a selection, and a selection into a
    /// stable locator to persist - none of which the returned QImage alone
    /// can answer.
    struct TextRun {
        QRectF rect;
        qsizetype offset = 0;
        QString text;
        QFont font; ///< for exact intra-run hit-testing (QFontMetricsF)
    };

    /// Renders `html` wrapped to `widthPixels`. A relative `<img src>`
    /// resolves against `baseDir` (the note's own directory); a remote one
    /// draws nothing, per the class comment. Null when litehtml is not
    /// compiled in, or the markup laid out with no visible content - either
    /// way the caller falls back to showing the source, the way a formula
    /// MicroTeX cannot draw does.
    ///
    /// `extraCss`, appended after the theme's own rules, is for a caller that
    /// has CSS of the *document's* own - the EPUB reader collects a chapter's
    /// linked stylesheets itself (they live inside the book's archive, which
    /// this class knows nothing about) and passes their text through here
    /// rather than this class fetching them; a plain HTML block never sets
    /// it. Cascading after the theme means a book's own colours/typography
    /// win where it declares them, and the theme still supplies everything
    /// it does not - "the app's theme unless the book insists".
    ///
    /// `textRuns`, when not null, is filled with every run drawn - see
    /// TextRun above. Left null (the default, and what every HTML-block call
    /// still passes) this costs nothing beyond a null check per run.
    static QImage render(const QString &html, const Theme &theme, int widthPixels,
                         const QString &baseDir, qreal devicePixelRatio = 1.0,
                         const QString &extraCss = QString(), QList<TextRun> *textRuns = nullptr);

    /// False in a build configured without litehtml (NOTEAPPC_WITH_HTML off,
    /// or the package was not found).
    static bool isAvailable();

    /// The same `<script>`/`on*=` stripping render() applies to `html` before
    /// litehtml ever sees it, exposed so a caller that lays out markup
    /// through a different path - the EPUB reader sanitises a whole
    /// chapter's XHTML, not a note's HTML block - gets the identical defence
    /// in depth rather than a second copy of the same two regexes.
    static QString sanitize(const QString &html);

    /// How many times render() has actually built a litehtml document, since
    /// process start. Test-only: it is what proves a note with no HTML block
    /// never constructs one at all, and that a rendered block's image is
    /// reused rather than rebuilt on an unrelated keystroke - see
    /// HTMLBlockTest in tests/test_editor.cpp.
    static qint64 renderCount();
};

} // namespace noteAppC
