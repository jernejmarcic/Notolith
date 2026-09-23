#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QPageLayout>
#include <QPageSize>
#include <QPixmap>
#include <QPointer>
#include <QSizeF>
#include <QTextLayout>
#include <QTimer>

#include <functional>
#include <memory>
#include <span>
#include <optional>
#include <vector>

class QCompleter;
class QFileSystemWatcher;

#include "CodeHighlighter.h"
#include "Lifeline.h"
#include "MathRenderer.h"
#include "FindBar.h"
#include "Decorations.h"
#include "DisplayMap.h"
#include "Parser.h"
#include "CalloutStyle.h"
#include "LanguageStyle.h"
#include "BaseResult.h"
#include "QueryResult.h"
#include "Theme.h"
#include "PdfArgs.h"
#include "TexCompiler.h"

class QLineEdit;
class QMenu;

namespace noteAppC {

/// How far a callout inside a callout steps in. Wide enough for its own icon,
/// which is drawn in the space this opens up. Shared, because the layout sets
/// the indent and the renderers draw into it.
constexpr qreal kCalloutIndent = 26;

/// The editor engine: one widget, three modes, no drift between them.
///
/// Every top-level block gets a `QTextLayout` over a *display string* built by
/// a DisplayMap. In live preview a block that does not hold the cursor hides
/// its syntax markers; the block the cursor is in shows its raw source, which
/// is exactly what Obsidian does. Source mode hides nothing; reading view
/// hides everything and takes no cursor.
///
/// `QTextLayout` brings HarfBuzz shaping, bidi and line breaking, so this
/// class only has to decide what string to lay out where.
class MarkdownEditor : public QAbstractScrollArea {
    Q_OBJECT

public:
    enum class Mode { LivePreview, Source, Reading };

    explicit MarkdownEditor(QWidget *parent = nullptr);
    ~MarkdownEditor() override;

    void setTheme(const Theme &theme);
    void setMode(Mode mode);

    /// Opens an editor over the value of the property whose line holds
    /// `sourceOffset`, as clicking the value does. False when that value
    /// cannot be rewritten safely: a list, a checkbox (which toggles), or one
    /// that is quoted across lines, carries a comment, or spans several lines.
    bool editPropertyAt(qsizetype sourceOffset);
    /// For a list property: opens an editor after its items that adds one,
    /// or removes item `item`. False when the list cannot be rewritten
    /// safely - see Frontmatter::rewriteList.
    bool addListItemAt(qsizetype sourceOffset);
    bool removeListItemAt(qsizetype sourceOffset, int item);
    /// Opens an editor over item `item` of a list property, to change it.
    bool editListItemAt(qsizetype sourceOffset, int item);
    Mode mode() const { return m_mode; }

    /// Replaces the buffer and clears undo. Returns the caret to the top.
    void setNoteText(const QString &text);
    QString noteText() const { return m_text; }

    bool isModified() const { return m_modified; }
    void setModified(bool modified);

    /// Line numbers in a gutter, the way Obsidian shows them in source mode.
    void setLineNumbers(bool on);
    bool lineNumbers() const { return m_lineNumbers; }

    void setReadableLineLength(bool on);
    void setContentWidth(int pixels);

    /// CommonMark's rule: every newline is a line break. Off, a single newline
    /// inside a paragraph joins, which is what Obsidian does by default.
    void setStrictLineBreaks(bool on);
    bool strictLineBreaks() const { return m_strictLineBreaks; }

    /// A faint vertical rule at each level of list indentation.
    void setIndentGuides(bool on);
    bool indentGuides() const { return m_indentGuides; }
    /// Nested ordered lists numbered as Word's multilevel lists are - 1.,
    /// then 1.1, 1.2, then 1.2.1 - rather than each list counting on its own.
    void setOutlineNumbering(bool on);
    bool outlineNumbering() const { return m_outlineNumbering; }
    /// What an ordered item's number reads as drawn (for the tests too).
    QString listLabel(const ListItemSpan &item) const;

    /// Spaces a Tab inserts where it is not indenting a list item.
    void setTabWidth(int spaces);
    int tabWidth() const { return m_tabWidth; }

    /// Whether Ctrl+K on a selection writes `[[Note]]` or `[Note](Note.md)`.
    void setUseWikilinks(bool on);

    /// Spelling, from the spellcheck module when it is compiled in and
    /// switched on. The editor only asks; it never links a dictionary. An
    /// empty one - no `isCorrect` - turns underlining off.
    struct Spelling {
        std::function<bool(const QString &)> isCorrect;
        std::function<QStringList(const QString &)> suggestions;
        std::function<void(const QString &)> addWord;
    };
    void setSpelling(Spelling spelling);

    /// Types `text` at the caret, replacing any selection, as one undo step:
    /// how a template or a snippet goes in.
    void insertAtCaret(const QString &text) { insertText(text); }
    /// The menu a right click on a misspelt word at `sourceOffset` opens:
    /// its suggestions, then "Add to dictionary". Null anywhere else. Built
    /// rather than shown, so it can be tested.
    std::unique_ptr<QMenu> spellingMenuAt(qsizetype sourceOffset);
    bool useWikilinks() const { return m_useWikilinks; }

    /// Paints the viewport through OpenGL instead of Qt's raster engine.
    ///
    /// Whether this is faster is a question for the benchmark, not for
    /// intuition: the GL paint engine uploads a glyph cache and re-uploads it
    /// whenever the cache is evicted, which a page of text can do on its own.
    /// Swapping the viewport tears down and rebuilds the backing widget, so
    /// this is meant to be set once at construction, not toggled per frame.
    void setGPUacceleration(bool on);
    bool GPUacceleration() const { return m_GPUacceleration; }
    /// True when this build can do it at all: Qt6::OpenGLWidgets present, and
    /// a platform that can give the window a GL context.
    static bool GPUaccelerationAvailable();

    /// Obsidian's Export to PDF options, with its defaults.
    struct PdfOptions {
        bool includeTitle = true;
        QPageSize pageSize = QPageSize(QPageSize::A4);
        QPageLayout::Orientation orientation = QPageLayout::Portrait;
        qreal marginMillimetres = 25.4; ///< "Default", about an inch
        int downscalePercent = 100;
    };
    /// Renders `text` in reading view, light theme, onto paper, page by page.
    /// A page breaks between blocks; only a block taller than a page is cut.
    /// `resolver` resolves links and embeds as the window's does.
    static bool exportPdf(const QString &path, const QString &text, const QString &title,
                          std::function<QString(const QString &)> resolver,
                          const PdfOptions &options, QString *error = nullptr);
    /// Where each page starts, given every block's top and height: a block
    /// that does not fit starts the next page, one taller than a page runs on
    /// across as many as it takes.
    static QList<qreal> pageStarts(const QList<QPair<qreal, qreal>> &blocks, qreal pageHeight);

    /// Words in the note, frontmatter excluded, as of the last parse; -1 while
    /// a large note's first parse is still out. Counted on the parse thread,
    /// not per keystroke - wordCountChanged says when it moves.
    int wordCount() const { return m_wordCount; }
    /// Source offset of the caret, for tests and for the status bar.
    qsizetype cursorOffset() const { return m_cursor; }
    /// The selected source text, exactly as it is in the file - markers
    /// included, since that is what a copy has to carry.
    QString selectedText() const;
    void setCursorOffset(qsizetype offset);

    /// What the widget currently shows, block by block. The quickest way to
    /// assert live-preview behaviour, and useful when debugging a layout.
    QString displayText() const;
    /// The character format laid out at `sourceOffset`, merged from every
    /// range covering it; empty when its block has no layout yet. The way to
    /// assert what colour a token got without comparing pixels.
    QTextCharFormat formatAt(qsizetype sourceOffset) const;

    /// Height of the whole note as it is currently laid out. What the scroll
    /// bar is built from, and the only honest way to assert block spacing.
    qreal documentHeight() const { return m_totalHeight; }

    /// Pretends the reader clicked into the block holding `offset`, so it
    /// reveals its source. Used by the tests and by "reveal on open".
    void enterBlockAt(qsizetype offset);
    /// Shows tab `member` of the code tab group containing `sourceOffset`.
    /// What a click on a tab does; public for tests and for a keyboard command.
    void selectCodeTab(qsizetype sourceOffset, int member);

    /// One rendered row of a ```` ```query ```` block's results: `rect` in
    /// viewport coordinates, matching what a click there activates - public
    /// so the tests can click a specific result without reimplementing its
    /// layout by hand the way firstCheckboxPoint does for a fixed one.
    struct QueryResultRow {
        QRectF rect;
        QString path;
        qsizetype offset = 0;
    };
    /// The rendered rows of the query block containing `offset`. Empty when
    /// that block is not a query block, is still searching, found nothing,
    /// or has the caret inside it and is showing raw source instead.
    QList<QueryResultRow> queryRowsAt(qsizetype offset) const;

    /// The size of the cached image a rendered HTML block containing
    /// `offset` painted, for the tests - empty when that block is not
    /// rendered HTML (not a Html block, litehtml unavailable, or the caret
    /// is inside it and it is showing raw source instead).
    QSize htmlImageSizeAt(qsizetype offset) const;

    /// Colour a link at `sourceOffset` is painted in, for the tests. Invalid
    /// when there is no link there.
    QColor linkColourAt(qsizetype sourceOffset) const;

    /// Link target under `viewportPoint`, empty when there is none.
    QString linkAt(const QPoint &viewportPoint) const;

    /// Names the `[[` suggester offers. Refreshed from the index.
    void setLinkTargets(const QStringList &targets);

    /// Turns a link target into an absolute path, or an empty string when it
    /// does not resolve. Supplied by the window, which owns the index.
    void setLinkResolver(std::function<QString(const QString &)> resolver);
    /// The index changed, so a link drawn as pointing nowhere may point
    /// somewhere now, or the reverse. Relays out only if one did.
    void refreshLinkStatus();

    /// Delivers the results of one ```query``` block's search, in the order
    /// found; called from any thread. See setQueryRunner.
    using QueryDeliver = std::function<void(QueryResults)>;
    /// Runs `query` and calls `deliver` exactly once with the results, from
    /// any thread - the editor marshals back onto its own thread before
    /// touching a block, so the window never has to. Supplied by the window,
    /// which owns the index; the editor never links libs/index (the module
    /// boundary docs/PLUGINS.md sets out). A search over 20 000 notes takes
    /// about 6 ms (docs/PERFORMANCE.md), but this is still asynchronous:
    /// layout must never wait on it, so a block shows "Searching..." until
    /// `deliver` is called.
    void setQueryRunner(std::function<void(const QString &query, const QueryDeliver &deliver)> runner);
    /// The index changed: every rendered ```query``` block's search is
    /// dropped from the cache and asked again. Debounced, so a burst of
    /// index updates (Index::changed can fire once per save) costs one
    /// search per visible block, not one per change.
    void refreshQueries();

    /// The header Obsidian draws over an embedded search's results: the
    /// result count, and where the collapse toggle and the sort control are
    /// - in viewport coordinates, the way queryRowsAt exposes result rows,
    /// so a test can click either without re-deriving the block's layout by
    /// hand. Default-constructed (empty rects, empty countText) when
    /// `offset` is not inside a rendered ```query``` block, or its search
    /// has not answered yet - there is nothing to count or sort until then.
    struct QueryHeader {
        QRectF collapseRect;
        QRectF sortRect;
        bool collapsed = false;
        bool sortByName = false; ///< false: the order the search delivered ("best match")
        QString countText;       ///< "N results", already pluralised
    };
    QueryHeader queryHeaderAt(qsizetype offset) const;
    /// Shows or hides the result rows of the ```query``` block containing
    /// `offset`, leaving its header in place - what a click on the header's
    /// collapse toggle does. Remembered for the block's own source position
    /// for the life of this editor (like a fold - see m_folds), never
    /// written to the note.
    void toggleQueryCollapsed(qsizetype offset);
    bool queryCollapsedAt(qsizetype offset) const;
    /// Swaps the result order between the search's own ("best match") order
    /// and file name (Obsidian's search pane sort options include "File name
    /// (A to Z)" - help.obsidian.md/plugins/search - which is the one that
    /// still means something once the note text itself is gone). Remembered
    /// the same way toggleQueryCollapsed is.
    void cycleQuerySort(qsizetype offset);

    /// One clickable row of a rendered ```` ```base ```` block's table, in
    /// viewport coordinates - only a row's first cell (`file.name`, by
    /// convention the view's first `order:` entry) opens its note, mirroring
    /// BaseTableModel::isNameColumn's choice. Public so the tests can click
    /// a specific row without reimplementing the table layout by hand.
    struct BaseRowRect {
        QRectF rect;
        QString path;
    };
    QList<BaseRowRect> baseRowsAt(qsizetype offset) const;

    /// Delivers one `.base` file evaluated to a plain table, exactly once;
    /// called from any thread - see setQueryRunner, whose contract this
    /// mirrors field for field.
    using BaseDeliver = std::function<void(BaseTableData)>;
    /// Runs `yamlText` (a ```` ```base ```` block's own content) and calls
    /// `deliver` exactly once with the result, from any thread. Supplied by
    /// the window, which owns the index and vault; the editor never links
    /// libs/ext/bases (docs/PLUGINS.md's module boundary) - see
    /// bases::evaluateBaseForEditor, which is what a real runner calls.
    /// Layout never waits on it: a block shows "Loading..." until `deliver`
    /// is called, the same contract setQueryRunner keeps for a search.
    void setBaseRunner(std::function<void(const QString &yamlText, const BaseDeliver &deliver)> runner);
    /// The index changed: every rendered ```` ```base ```` block's table is
    /// dropped from the cache and evaluated again. Debounced like
    /// refreshQueries().
    void refreshBases();

    // --- feature switches ---------------------------------------------------
    // Jernej: "Everything but the core features, should be decoupled enough
    // that they can be turned off ... IF the features existing decreases
    // [performance]". Every optional block renderer is gated by one of these;
    // off means the block shows as source (or, for a file embed, a plain
    // link) and none of that feature's code runs or allocates - no cache, no
    // worker thread, no bytes read that would not be read anyway.
    struct Features {
        bool queryBlocks = true; ///< ```` ```query ```` blocks
        bool baseBlocks = true;  ///< ```` ```base ```` blocks and `.base` embeds
        bool HTMLblocks = true;  ///< raw HTML blocks through litehtml
        bool PDFembeds = true;   ///< `![[x.pdf]]`
        bool texEmbeds = true;   ///< `![[x.tex]]`
        bool mediaEmbeds = true; ///< `![[x.mp3]]` / `![[x.mp4]]`
        bool maths = true;       ///< `$...$` and `$$...$$`
        bool fileEmbeds = true;  ///< notes, `.canvas`, and any other file's card
        bool operator==(const Features &) const = default;
    };
    void setFeatures(const Features &features);
    const Features &features() const { return m_features; }

    /// A file kind the editor has no built-in renderer for - `.canvas` is
    /// the reason this exists: the canvas module lives in libs/ext and the
    /// editor must not link it (docs/PLUGINS.md's module boundary), so the
    /// window registers a renderer instead, the same shape setQueryRunner
    /// and setBaseRunner already use. `deliver` may be called from any
    /// thread, at most once per call; an embed shows a small placeholder
    /// sized to `width` until it does. Suffixes are matched case-insensitive,
    /// without the dot; a null renderer unregisters them.
    using FileEmbedRenderer = std::function<void(const QString &path, const QString &subpath,
                                                  int width, const std::function<void(QImage)> &deliver)>;
    void setFileEmbedRenderer(const QStringList &suffixes, FileEmbedRenderer renderer);

    /// A fenced code block in a language a plugin renders: shown as the
    /// renderer's answer instead of a listing. The same contract as
    /// setFileEmbedRenderer - `deliver` from any thread, at most once, a
    /// placeholder until then - cached by language, width, theme and source.
    /// `text` is drawn as Markdown, `image` as it is; neither falls back to
    /// the listing. Languages match case-insensitive; a null renderer
    /// unregisters them. `theme` is "dark" or "light".
    struct CodeBlockResult {
        bool isMarkdown = true;
        QString text;
        QImage image;
    };
    using CodeBlockRenderer =
        std::function<void(const QString &language, const QString &source, int width, const QString &theme,
                           const std::function<void(CodeBlockResult)> &deliver)>;
    void setCodeBlockRenderer(const QStringList &languages, CodeBlockRenderer renderer);

    /// A PDF (or compiled `.tex`) embed's state, for the tests - a click on
    /// its previous/next controls is exercised through pdfSlideStep rather
    /// than a synthesised QMouseEvent, the same way toggleQueryCollapsed
    /// stands in for clicking a query header.
    struct PdfEmbedInfo {
        bool isPdfEmbed = false;
        QString error;       ///< a bad argument, a missing file, or a failed .tex compile
        int pageCount = 0;   ///< pages in the underlying PDF, 0 if it could not be opened
        int cellCount = 0;   ///< pages actually laid out (column/grid: shown at once)
        PdfEmbedArgs::Layout layout = PdfEmbedArgs::Layout::Slides;
        int slideIndex = 0;  ///< slides layout only
        int slideCount = 0;
    };
    PdfEmbedInfo pdfEmbedInfoAt(qsizetype offset) const;
    /// +1 / -1: what a click on the slide layout's next/previous control
    /// does. Remembered per block for the editor's lifetime, like a query's
    /// collapsed state - never written to the note.
    void pdfSlideStep(qsizetype offset, int direction);

    /// For the tests: whether the block containing `offset` rendered as a
    /// file card (icon, name, size) - the fallback for a binary file, or any
    /// file with no richer renderer.
    bool isFileCardAt(qsizetype offset) const;
    /// How many embeds the block containing `offset` rendered as trailing
    /// cells - see layoutEmbedRow's comment. 0 outside such a block.
    int embedCellCountAt(qsizetype offset) const;
    /// For the tests: the raw text a block containing `offset` drew through
    /// its own substituted QTextLayout - a source-file listing, a note
    /// embed's panel, a `.tex` compile's error log, a query block's message
    /// - none of which flow through view.map the way ordinary note text
    /// does, so displayText() (which reads only the map) cannot see them.
    /// Empty when that block has no such layout.
    QString panelTextAt(qsizetype offset) const;
    /// The size an embed drawn as an image (a formatted note, a canvas)
    /// shows at; empty while it is text or a placeholder. For the tests.
    QSize embedImageSizeAt(qsizetype offset) const;

    /// Closes brackets and quotes as you type and wraps a selection instead of
    /// replacing it. Obsidian's `autoPairBrackets` and `autoPairMarkdown` as
    /// one switch, on by default the way Obsidian ships them.
    void setAutoPairs(bool on) { m_autoPairs = on; }
    bool autoPairs() const { return m_autoPairs; }

    // --- editing commands -------------------------------------------------
    // Bound to the same keys Obsidian uses; public so the Phase 4 command
    // palette can reach them without a second implementation.

    /// One editing command, under the id Obsidian gives it where it has one,
    /// so a `hotkeys.json` written for Obsidian rebinds it here as well.
    struct Command {
        QString id;
        QString name;
        QList<QKeySequence> keys; ///< the built-in binding
    };
    static const QList<Command> &commands();
    /// Runs the command with this id. False when there is no such command.
    /// An edit another view of the same note made: the text follows, this
    /// view's caret keeps its place in it, and its own undo history - which
    /// no longer describes the text - is dropped.
    void applyExternalEdit(qsizetype at, qsizetype removed, const QString &inserted);
    bool runCommand(const QString &id);
    /// Keys by command id, over the built-in ones; an empty list unbinds.
    /// Undo, redo, copy, cut, paste and select all are not commands: they
    /// keep their standard keys, as they do in Obsidian.
    void setKeyBindings(const QHash<QString, QList<QKeySequence>> &bindings);

    void toggleBold();      ///< Ctrl+B
    void toggleItalic();    ///< Ctrl+I
    void toggleHighlight(); ///< Ctrl+Shift+H
    void insertLink();      ///< Ctrl+K
    void toggleTaskAtCursor(); ///< Ctrl+Enter
    /// Alt+Up / Alt+Down. Moves every line the selection touches.
    void moveLines(int direction);
    void duplicateLines(); ///< Ctrl+Shift+D

    // --- find and replace -------------------------------------------------

    /// Ctrl+F, or Ctrl+H for the replace row as well.
    void showFind(bool withReplace);
    void hideFind();
    bool findVisible() const;
    void setFindQuery(const QString &pattern, bool caseSensitive, bool regex);
    /// +1 for the next match, -1 for the previous; wraps round, as the bar does.
    void findStep(int direction);
    int findMatchCount() const { return int(m_find.matches.size()); }
    /// Zero-based index of the current match, -1 when there is none.
    int findCurrentIndex() const { return m_find.current; }
    /// False when a regex query does not compile; the field turns red.
    bool findPatternValid() const { return m_find.valid; }
    void replaceCurrent(const QString &with);
    /// Returns how many matches were replaced.
    int replaceAll(const QString &with);

    // --- folding ----------------------------------------------------------
    // A `> [!type]-` callout, a `:::` container and a `#| fold: true` code
    // block all fold the same way; so does the properties table. `offset` is
    // anywhere inside the block.

    void toggleFoldAt(qsizetype sourceOffset);
    bool isFoldedAt(qsizetype sourceOffset) const;

    /// The frontmatter properties, as the last parse read them. The properties
    /// table renders from these, and the window's future property pane will.
    const QList<Property> &properties() const { return m_properties; }

    /// The note's headings, as the last parse read them, in source order. The
    /// outline pane builds its tree from these instead of re-parsing.
    const QList<Heading> &headings() const { return m_headings; }

signals:
    /// The word count moved. It is published with each parse, so it lags a
    /// keystroke by the parse debounce instead of costing the keystroke.
    void wordCountChanged(int words);
    /// A parse landed, so headings() reflects the current text. Fired even
    /// when the heading list did not actually change - cheap to ignore, and
    /// the outline pane only rebuilds a QTreeWidget from it.
    void headingsChanged();
    void textChanged();
    /// Every change to the text, as one edit - typing, a paste, an undo (a
    /// whole-text replacement) - for another view of the same note to make
    /// with applyExternalEdit(). Not for setNoteText or applyExternalEdit.
    void edited(qsizetype at, qsizetype removed, const QString &inserted);
    void modificationChanged(bool modified);
    void linkActivated(const QString &target);
    void cursorMoved(qsizetype offset);
    /// The pointer came to rest on a link to a note, the way Obsidian's page
    /// preview wants it: plain hover in reading view, with Ctrl held while
    /// editing. `globalPos` is where the pointer is.
    void linkHovered(const QString &target, const QPoint &globalPos);
    /// It left that link, or let go of it by leaving the editor.
    void linkHoverEnded();
    /// A result in a rendered ```query``` block was clicked: `relativePath`
    /// is the note it matched, `offset` the first match inside it.
    void queryResultActivated(const QString &relativePath, qsizetype offset);
    /// A row in a rendered ```` ```base ```` block's table was clicked:
    /// `relativePath` is the note behind it. No offset - a base row opens a
    /// note, it does not point at a place inside it the way a search match
    /// does.
    void baseRowActivated(const QString &relativePath);

protected:
    void paintEvent(QPaintEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    /// Selects the word around `offset`, for a double-click or a long press.
    void selectWordAt(qsizetype offset);
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    /// One row of the frontmatter properties table: the key and its type icon
    /// on the left, the typed value on the right. Rectangles are relative to
    /// the block's own origin, like a table Cell's.
    struct PropertyRow {
        int property = 0;     ///< index into `m_properties`
        QRectF rect;          ///< the whole row
        QRectF control;       ///< the checkbox, when the value is one
        QList<QRectF> chips;  ///< one box per item of a list value
        QList<QRectF> removers; ///< each chip's "x", when the list can be changed
        QRectF adder;           ///< the space after the chips that adds an item
    };

    /// One cell of a rendered table.
    struct Cell {
        Range source;
        DisplayMap map;
        std::unique_ptr<QTextLayout> layout;
        QRectF rect;
        Qt::Alignment alignment = Qt::AlignLeft;
        int row = 0;
        int column = 0;
    };

    /// One top-level block: the unit that is laid out, cached and revealed.
    /// Blocks outside the viewport carry an estimated height and no layout,
    /// which is what keeps a 200 kB note responsive.
    /// A callout painted inside another block's layout, with the look it
    /// resolved to.
    struct NestedCallout {
        const CalloutSpan *span = nullptr;
        CalloutStyle style;
    };

    struct BlockView {
        BlockSpan span;
        DisplayMap map;
        std::unique_ptr<QTextLayout> layout;
        qreal y = 0;
        qreal height = 0;
        bool active = false;
        bool laidOut = false;
        bool heightEstimated = false;
        /// Indices into the sorted decoration lists that touch this block, so
        /// laying one out never walks the whole document.
        int firstStyle = 0;
        int lastStyle = 0;
        int firstSyntax = 0;
        int lastSyntax = 0;
        /// Filled instead of `layout` when a table renders as a grid.
        std::vector<Cell> cells;
        int headerRows = 0;
        /// A table keeps its columns at their natural width; when the total
        /// exceeds the readable column the table scrolls sideways inside its
        /// own block, exactly as it does in Obsidian.
        qreal tableWidth = 0;
        qreal tableScroll = 0;
        /// Set when this block is a callout, so painting knows its colour,
        /// icon and title without looking the span up again.
        const CalloutSpan *callout = nullptr;
        /// The callout's resolved look: the built-in type, then callouts.ini,
        /// then whatever the callout itself says. Resolved before the block is
        /// laid out, because it can change the body font and so the metrics.
        CalloutStyle calloutStyle;
        /// Callouts nested inside this block, outer first. Only the outermost
        /// callout is a block of its own; the rest are painted into this one's
        /// layout, which is why they are resolved before it is built.
        std::vector<NestedCallout> nested;
        /// A fence in a tab group that is not the tab being shown, or the
        /// blank lines between two of them: no height, nothing painted.
        bool tabHidden = false;
        /// Set on the fence a tab group is showing: it paints the strip.
        /// An index into m_tabGroups, -1 outside any group.
        int tabGroup = -1;
        /// Inside the tab group the caret is in, which shows every fence.
        /// Tracked so the caret crossing a group's edge relays out the whole
        /// group, not only the block it landed in.
        bool inCaretGroup = false;
        /// A fence longer than kCodePieceLines is split into pieces of that
        /// many lines, each a block of its own, so an edit relays out one
        /// piece and not the whole listing. The fence's chrome is drawn across
        /// its pieces: header, label and tabs on the first, caption on the
        /// last. A fence that fits, and every other block, is piece 0 of 1.
        int piece = 0;
        int pieces = 1;
        /// Where highlighting stood at the end of this piece, the state the
        /// next one starts from. Valid when it was worked out from this
        /// piece's current text and the end state of the piece before it.
        CodeHighlighter::State codeEnd;
        bool codeEndValid = false;
        /// The caret's block, in a kind that reveals per element: the syntax
        /// ranges the caret has uncovered. Kept so a caret moving within the
        /// block relays it out only when this actually changes.
        QList<Range> revealed;
        /// The word spelling skipped because the caret was in it. Kept so
        /// the caret moving on to another word relays the block out and the
        /// finished one is checked.
        Range caretWord;
        /// Set when this block is a fenced code block carrying `#|` options.
        const CodeOptions *options = nullptr;
        /// Filled instead of `layout` when this is the frontmatter and live
        /// preview draws it as Obsidian's properties table.
        std::vector<PropertyRow> propertyRows;
        /// For each of the block's list items (listItemsIn order): where its
        /// text starts, block-local, and the item it is nested in (-1 for
        /// none). Siblings share one marker column, as wide as their widest
        /// number, so "10." pushes its list's text over rather than into it.
        std::vector<qreal> listTextX;
        std::vector<int> listParent;
        /// Space above the text: a callout's title row, a code block's header
        /// strip, or the air Obsidian puts over a heading.
        qreal textTop = 0;
        /// Showing its header only. Callouts, `:::` containers, `#| fold`
        /// code blocks and the properties table can all be in this state.
        bool folded = false;
        /// Set when the whole block is one image embed.
        QPixmap image;
        QSize imageSize;
        bool imageCentred = false;
        QString imagePath;
        /// Filename shown in the header strip of a source-file or PDF embed.
        QString embedLabel;
        /// Draw the code panel and header behind this block.
        bool embedPanel = false;
        /// An audio or video embed's file: the block is a player card.
        QString mediaPath;
        bool mediaVideo = false;

        // --- PDF and .tex embeds, PdfEmbedBlock.cpp -----------------------
        /// Set when this block is a PDF, or a compiled `.tex`, page embed -
        /// laid out by pdfArgs instead of the single `image` field above,
        /// which a plain one-page PDF still uses when that is all pdfArgs
        /// asks for (see layoutPdf).
        bool isPdfEmbed = false;
        PdfEmbedArgs pdfArgs;
        QString pdfPath;   ///< the PDF actually shown - a .tex's own compiled output
        qint64 pdfMtime = 0;
        QString pdfError;  ///< a bad argument, a missing file, or a failed compile
        int pdfPageCount = 0;
        /// One requested page. Column and grid lay out every page pdfArgs
        /// resolved to; slides lays out the one currently shown. `image` is
        /// null while its render is still in flight off the UI thread -
        /// painted as a placeholder box sized from `pointSize` until then.
        struct PdfCell {
            int page = 0;
            QRectF rect; ///< block-local
            QPixmap image;
            QSizeF pointSize; ///< the page's own size, in PDF points
        };
        std::vector<PdfCell> pdfCells;
        int pdfSlideIndex = 0;
        int pdfSlideCount = 0;
        QRectF pdfPrevRect, pdfNextRect; ///< slide nav, block-local; null when not slides
        QString pdfCounterText;          ///< "3 / 12"

        // --- file cards and note embeds, FileEmbedBlock.cpp ---------------
        /// Set when this block is a file noteAppC has no richer renderer
        /// for: a small card (icon, name, size) that opens the file on
        /// click - never a binary file's bytes shown as text.
        bool isFileCard = false;
        QString fileCardPath;
        QString fileCardLabel;
        QString fileCardSize;
        QRectF fileCardRect; ///< block-local; the whole card is the click target

        /// One embedded file inside a block that holds more than one embed,
        /// or mixes an embed with running text - see layoutEmbedRow's
        /// comment for why these are laid out as trailing rows rather than
        /// truly inline. Built by the same per-kind logic a sole whole-block
        /// embed uses, addressed by its own rect instead of the block's
        /// single set of fields above.
        struct EmbedCell {
            Range source; ///< the `![[...]]` span, for a click
            QRectF rect;    ///< block-local
            QPixmap image;  ///< a thumbnail (image or a PDF's first requested page)
            QString label;
            QString sizeText;  ///< set for a file-card cell
            bool isFileCard = false;
            bool panel = false; ///< a small filename caption under a PDF thumbnail
            QString openPath;  ///< what a click opens - the resolved file, or a note target
            bool isNote = false; ///< opens through linkActivated rather than QDesktopServices
        };
        std::vector<EmbedCell> embedCells;

        /// Set when this block is a fenced ```query``` block rendered as
        /// search results: paintBlocks and handleBlockClick both skip the
        /// ordinary code-listing chrome (panel, "Copy", tabs) for it.
        bool isQuery = false;
        /// One clickable result row, in block-local coordinates (add `left`
        /// and `top` to place it in the viewport, the way every other rect
        /// on a BlockView already does).
        struct QueryRow {
            QRectF rect;
            QString path;
            qsizetype offset = 0;
        };
        std::vector<QueryRow> queryRows;
        /// The header's two clickable controls, block-local coordinates -
        /// null-sized when the block has no header yet (still searching, an
        /// error, or an empty search - see layoutQuery). queryHeaderAt and
        /// handleBlockClick both read these rather than recomputing them, so
        /// a click always lands exactly where the paint went.
        QRectF queryCollapseRect;
        QRectF querySortRect;
        /// "N results", as last computed - what queryHeaderAt hands back
        /// without re-running a search from a const accessor.
        QString queryResultCountText;

        /// Set when this block is a fenced ```` ```base ```` block rendered
        /// as a table: paintBlocks and handleBlockClick both skip the
        /// ordinary code-listing chrome for it, the same gate isQuery is for
        /// a ```query``` block.
        bool isBase = false;
        /// The shown view's name, painted above the table - empty shows
        /// nothing extra (an error or a still-loading block shows its
        /// message instead of a table; see layoutBase).
        QString baseViewName;
        QString baseYAML; ///< what the table shows: a fence's text, or a .base file's
        /// One clickable row's first cell, block-local coordinates - only
        /// the name column opens its note, mirroring
        /// BaseTableModel::isNameColumn's choice for the real widget.
        struct BaseRow {
            QRectF rect;
            QString path;
        };
        std::vector<BaseRow> baseRows;

        /// Set when this block is a raw HTML block (top-level `<div>`,
        /// `<table>`, `<details>`... - not inline HTML inside a paragraph)
        /// rendered by litehtml. It paints through `image` exactly like an
        /// embed, so nothing else needs to know it came from HtmlRenderer
        /// rather than an image file; this flag exists only so the "Copy"
        /// button and tab strip a Code-kind block would otherwise carry
        /// never gets a chance to (HTML blocks are BlockSpan::Kind::Html,
        /// which has no such chrome to begin with - kept for symmetry with
        /// isQuery and in case a future BlockSpan kind needs the same gate).
        bool isHtml = false;
        /// Set when a plugin draws this fence (CodeBlockHook.cpp): no
        /// listing chrome, and codeBlockKey finds it again on delivery.
        bool isPluginBlock = false;
        QString codeBlockKey;
        /// Text runs from inline HTML (`<b>`, `<kbd>`, `<span style=...>`...)
        /// resolved to a QTextCharFormat by layoutInlineHtml - added to
        /// formatsIn's output the way spellcheck underlines are. Source
        /// coordinates; formatsIn converts through `view.map` like every
        /// other entry there.
        std::vector<std::pair<Range, QTextCharFormat>> inlineHtmlFormats;

        /// One inline `$...$` formula resolved while laying this block out.
        /// `QTextLayout` has no notion of an inline object, so the lone "$"
        /// the DisplayMap left visible carries letter spacing equal to the
        /// image's width instead, reserving exactly the room the paint step
        /// draws into once it knows which line that character landed on.
        struct InlineMath {
            qsizetype sourcePos = 0;  ///< the visible "$", in source coordinates
            qsizetype displayPos = 0; ///< the same character, in display coordinates
            QPixmap image;
            QSize imageSize;
        };
        std::vector<InlineMath> inlineMaths;
        /// A heading link's "#", which Obsidian shows as " > " between the
        /// note and the heading. The same trick as inline maths: the "#"
        /// stays, in invisible ink and as wide as the separator, and the paint
        /// step draws the separator over it.
        struct AnchorSeparator {
            qsizetype sourcePos = 0;
            qsizetype displayPos = 0;
            QColor colour;
        };
        std::vector<AnchorSeparator> separators;
    };

    /// Parses on the calling thread. Used when the whole note changes.
    void reparse();
    /// Starts a parse on the thread pool; the editor never waits for it.
    void startParse();
    void adoptDecorations(Decorations decorations, QList<Property> properties, QList<Heading> headings);
    void rebuildBlocks();
    void indexDecorations();
    void layoutBlock(BlockView &view);
    /// Lays a table out as a grid. False when this block is not a table or
    /// its structure is missing, so the caller falls back to plain text.
    bool layoutTable(BlockView &view);
    void paintTable(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The table under `viewportPoint` that is wider than the column, or null.
    BlockView *scrollableTableAt(const QPoint &viewportPoint);
    void paintCallout(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// Bullets and checkboxes in the margin of a list block.
    void paintListMarkers(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The list items that start inside `block`.
    std::span<const ListItemSpan> listItemsIn(const Range &block) const;
    /// Checkbox rectangle of `item` in viewport coordinates, for hit testing.
    /// Fills listTextX and listParent; empty for a block with no list lines.
    void layoutListColumns(BlockView &view) const;
    /// The text start of the innermost list item holding `source`, or -1.
    qreal listTextAt(const BlockView &view, qsizetype source) const;
    QRectF checkboxRect(const BlockView &view, const ListItemSpan &item) const;
    void toggleTask(const ListItemSpan &item);
    const CalloutSpan *calloutFor(const BlockView &view) const;

    // --- block renderers, implemented in BlockRenderers.cpp ---------------

    /// Lays the frontmatter out as Obsidian's properties table. False when
    /// there is nothing to show, so the caller falls back to plain text.
    bool layoutProperties(BlockView &view);
    void paintProperties(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The panel, the header strip, the line-number gutter and the highlighted
    /// rows of a fenced code block.
    void paintCodeChrome(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The first piece's share of the chrome: the `#|` header, the tab strip
    /// or just the label, over `panel`, which spans the whole fence.
    void paintCodeHeader(QPainter &painter, const BlockView &view, qreal left, qreal top,
                         const QRectF &panel) const;
    /// `#| highlight` rows and `#| line-numbers` for one piece.
    void paintCodeRows(QPainter &painter, const BlockView &view, qreal left, qreal top,
                       const QRectF &panel) const;
    QRectF bandOf(const BlockView &view, const Range &source) const;

    /// The label on the side of a code block's header: its language's style
    /// name and icon, and the thing to click to copy the listing. Painting and
    /// hit-testing both come from codeLabelOf, so they cannot disagree about
    /// where it is.
    struct CodeLabel {
        QRectF rect;   ///< in viewport coordinates, icon included
        QString text;
        QIcon icon;
        QColor colour;
        QFont font;
        bool visible = false;
    };
    CodeLabel codeLabelOf(const BlockView &view, qreal left, qreal top) const;
    const CodeFenceSpan *codeFenceOf(const BlockView &view) const;
    LanguageStyle codeStyleOf(const BlockView &view) const;
    LanguageStyle styleOfFence(const CodeFenceSpan &fence) const;

    /// A run of adjacent `tab` fences, shown as one block with a tab strip.
    struct TabGroup {
        Range span;    ///< the first fence's start to the last fence's end
        /// Where hiding stops: the end of the last fence's listing. A code
        /// block's listing is its own block, and its fences sit in the gap
        /// blocks around it - so the gap after the last listing, which holds
        /// the closing fence and the blank line before the next paragraph,
        /// has to stay, or the group loses the spacing any code block has.
        qsizetype hiddenEnd = 0;
        int first = 0; ///< index into m_decorations.codeFences
        int count = 0;
    };
    /// One tab in a group's strip. tabsOf computes these for painting and for
    /// clicking both, so the two cannot disagree about where a tab is.
    struct TabRect {
        QRectF rect;
        QString text;
        QIcon icon;
        int member = 0;
        bool selected = false;
    };
    void collectTabGroups();
    const TabGroup *tabGroupAt(qsizetype source) const;
    /// The tab group the caret is in, its closing fence's end included.
    const TabGroup *caretTabGroup() const;
    int selectedTabOf(const TabGroup &group) const;
    std::vector<TabRect> tabsOf(const BlockView &view, qreal left, qreal top) const;
    void copyCodeBlock(const BlockView &view);
    const BlockView *codeLabelAt(const QPoint &point) const;
    void paintCodeLabel(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    int nestingDepthAt(const BlockView &view, qsizetype source) const;
    void hideFoldedNestedCallouts(const BlockView &view, QList<Range> &hidden) const;
    /// True for the kinds that show all their source when the caret is in
    /// them - tables, code, maths, callouts - as Obsidian does with its widgets.
    /// Paragraphs, headings, lists and plain quotes reveal per element instead.
    bool revealsWholeBlock(const BlockView &view) const;
    QList<Range> revealedSyntax(const BlockView &view) const;
    QRectF nestedFoldHandleRect(const BlockView &view, const NestedCallout &nested, qreal left,
                                qreal top) const;
    void paintNestedCallouts(QPainter &painter, const BlockView &view, qreal left,
                             qreal top) const;
    void paintNestedCodePanels(QPainter &painter, const BlockView &view, qreal left,
                               qreal top) const;
    void paintIndentGuides(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// Panel and filename header for a source-file or PDF embed.
    void paintEmbedChrome(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    void paintCheckBox(QPainter &painter, const QRectF &rect, bool done) const;
    /// The disclosure triangle every foldable block carries.
    void paintFoldArrow(QPainter &painter, const QRectF &rect, bool folded,
                        const QColor &colour) const;
    /// `#|` options of this block, or nullptr. Matched on the block's start:
    /// `rebuildBlocks` takes the newline after a block into its span, so the
    /// ends no longer compare equal.
    const CodeOptions *codeOptionsFor(const BlockView &view) const;
    qreal codeHeaderHeight() const;
    /// Space under the listing for a `#| caption:`, 0 when there is none.
    qreal codeCaptionHeight(const BlockView &view) const;
    qreal propertyRowHeight() const;
    /// Width a code block reserves for `#| lines:` numbers, 0 when it has none.
    qreal codeGutterWidth(const BlockView &view) const;
    /// Obsidian's `--p-spacing`: one rem of the text font, which is the unit
    /// every block margin is made of.
    qreal blockSpacing() const;
    /// Where this block's disclosure triangle is, in viewport coordinates.
    QRectF foldHandleRect(const BlockView &view, qreal left, qreal top) const;
    bool isFoldable(const BlockView &view) const;
    /// What the source itself says: `[!note]-` and `::: details` start folded.
    bool defaultFoldOf(const BlockView &view) const;
    void setFolded(const BlockView &view, bool folded);
    /// True when the press landed on a fold handle or a property control, so
    /// the caret must not move.
    bool handleBlockClick(const QPoint &point);
    /// Flips a checkbox property, rewriting only that value's bytes.
    void toggleProperty(int index);
    /// The key's line, and where on it the value is, when its value can be
    /// replaced without touching anything else; both invalid otherwise.
    Range editableValueOf(const Property &property) const;
    bool listIsRewritable(const Property &property) const;
    int propertyAt(qsizetype sourceOffset) const;
    /// The laid-out row of property `index`, and the table's top in viewport
    /// coordinates; null when the table is folded or not laid out.
    const PropertyRow *propertyRowOf(int index, qreal *top) const;
    void openPropertyEditor(int index, const QRectF &cell, const QString &text, int item);
    /// Writes a list property's items back, as one undo step.
    bool replaceListEntry(int index, const QStringList &raws, const QStringList &values);
    /// Closes the property editor, writing what it holds when `commit`.
    void finishPropertyEdit(bool commit);
    bool eventFilter(QObject *watched, QEvent *event) override;
    /// Every whole-block embed kind (image, PDF, .tex, media, a file card, a
    /// hooked renderer) paints from its own fields, not from `view.layout` -
    /// but `view.map` still has to say the block's whole source is hidden,
    /// or displayText() (and anything else that reads the map rather than
    /// the paint output) sees whatever the block's *previous* layout left
    /// behind, stale, instead of nothing. Called once the kind is settled,
    /// before `view.laidOut = true`.
    void hideWholeBlockText(BlockView &view);
    /// The embed that fills this block on its own, or nullptr.
    const EmbedSpan *soleEmbedOf(const BlockView &view) const;
    /// A block that is nothing but an embed: an image, a PDF page, a note, a
    /// file the window has a renderer for, or a file shown as a read-only
    /// code block or a small card.
    bool layoutEmbed(BlockView &view);
    bool layoutImage(BlockView &view, const EmbedSpan &embed, const QString &path);
    bool layoutSourceFile(BlockView &view, const EmbedSpan &embed, const QString &path);

    // --- PDF and .tex embeds, implemented in PdfEmbedBlock.cpp -------------

    bool layoutPdf(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// A `.tex` embed whose source is a full document (has `\documentclass`)
    /// rather than maths alone: compiled to a PDF off the UI thread, then
    /// laid out exactly as layoutPdf lays out a real one. Maths-alone `.tex`
    /// is not an embed at all in this sense - layoutMath's own inline/display
    /// formula path handles it, since it is a formula and not a document.
    bool layoutTexEmbed(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// Shared by layoutPdf and layoutTexEmbed once each has a real PDF path
    /// on disk (the file itself, or a .tex's compiled output): resolves the
    /// page arguments against the document and fills in view's PDF fields.
    bool buildPdfBlock(BlockView &view, const QString &pdfPath, const QString &label,
                       const PdfEmbedArgs &args, int requestedWidth);
    void paintPdfEmbed(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The previous/next controls (slides layout) or nothing; a click on
    /// either steps pdfSlideIndex and relays out just this block.
    bool handlePdfEmbedClick(const BlockView &view, const QPoint &point, qreal left, qreal top);
    /// Cached page count and per-page size, keyed by path and mtime - a
    /// metadata read, not a render, so this runs on the UI thread; PdfPage
    /// itself still opens the file at most once per (path, mtime) per
    /// thread. `count` and `sizes` are left untouched when the file cannot
    /// be opened as a PDF.
    void pdfMetadata(const QString &path, qint64 mtime, int &count, QList<QSizeF> &sizes) const;
    struct PdfMeta {
        int count = 0;
        QList<QSizeF> sizes;
    };
    mutable QHash<QString, PdfMeta> m_pdfMetaCache; ///< "path|mtime" -> page count and sizes
    /// The cached page image for (path, mtime, page, width), or a null
    /// QPixmap while a render is in flight - which this also starts, off the
    /// UI thread, if one is not in flight already.
    QPixmap ensurePdfPageAsync(const QString &path, qint64 mtime, int page, int width);
    struct PdfPageCacheEntry {
        QPixmap image;
        bool pending = true;
        quint64 generation = 0;
    };
    QHash<QString, PdfPageCacheEntry> m_pdfPageCache; ///< "path|mtime|page|width" -> its render
    quint64 m_pdfGeneration = 0;
    /// How every worker - a search, a base, a PDF page, a .tex compile, a
    /// hooked file embed - posts back here; ended first in ~MarkdownEditor.
    std::shared_ptr<Lifeline> m_lifeline = std::make_shared<Lifeline>();
    /// A slides-layout embed's current page, by the block's own source start
    /// - like m_queryCollapsed, session-only and never written to the note.
    QHash<qsizetype, int> m_pdfSlideIndex;
    /// Called back (marshalled onto this thread) when a render started by
    /// ensurePdfPageAsync finishes. Relays out every block currently showing
    /// `path`, since column and grid can have several cells waiting on it.
    void deliverPdfPage(const QString &key, quint64 generation, const QString &path, QImage image);
    /// A `.tex` compile, cached by the file's own path and mtime - mirrors
    /// PdfPageCacheEntry, just for TexCompiler::compile instead of a render.
    struct TexCacheEntry {
        TexCompiler::Result result;
        bool pending = true;
        quint64 generation = 0;
    };
    QHash<QString, TexCacheEntry> m_texCache; ///< "path|mtime" -> its compile
    quint64 m_texGeneration = 0;
    const TexCacheEntry &ensureTexCompiled(const QString &path, qint64 mtime);
    void deliverTexCompiled(const QString &key, quint64 generation, TexCompiler::Result result);

    // --- file cards, note embeds and multi-embed rows, FileEmbedBlock.cpp --

    /// A file none of the above render: a small card (icon, name, size) that
    /// opens it on click. Also where layoutSourceFile redirects when the
    /// bytes it read back are not text - never shown as a wall of garbage.
    bool layoutFileCard(BlockView &view, const EmbedSpan &embed, const QString &path);
    void paintFileCard(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    bool handleFileCardClick(const BlockView &view, const QPoint &point, qreal left,
                             qreal top) const;
    /// A crude, cheap binary sniff over the file's first few KB: a NUL byte,
    /// or a high enough share of bytes that are neither printable ASCII nor
    /// part of a valid UTF-8 sequence, both say "binary" the way `file(1)`
    /// and git's own diff heuristic do. False (text) on a read failure or an
    /// empty file - an embed that cannot be read shows that as an error
    /// elsewhere, not here.
    bool isBinaryFile(const QString &path) const;
    /// `.canvas`, `.base`, or any other suffix the window registered a
    /// renderer for (setFileEmbedRenderer) - false when there is none, which
    /// falls through to layoutSourceFile/layoutFileCard.
    bool layoutHookedFileEmbed(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// A note embed's slice drawn formatted, `width` wide; null when it
    /// cannot be drawn without waiting.
    QImage renderNoteEmbed(const QString &slice, int width) const;
    /// What an embed drawn into an image took from the theme, so a theme
    /// change misses its cache instead of showing the old colours.
    QString embedThemeTag() const;
    QHash<QString, FileEmbedRenderer> m_fileEmbedRenderers; ///< lowercase suffix -> renderer
    struct FileEmbedCacheEntry {
        QImage image;
        bool pending = true;
        quint64 generation = 0;
    };
    QHash<QString, FileEmbedCacheEntry> m_fileEmbedCache; ///< "path|mtime|subpath|width" -> result
    quint64 m_fileEmbedGeneration = 0;
    void deliverFileEmbed(const QString &key, quint64 generation, QImage image);

    /// CodeBlockHook.cpp: a fence in a language setCodeBlockRenderer has.
    bool layoutPluginCode(BlockView &view);
    void deliverCodeBlock(const QString &key, quint64 generation, CodeBlockResult result);
    QHash<QString, CodeBlockRenderer> m_codeBlockRenderers; ///< lowercase language -> renderer
    struct CodeBlockCacheEntry {
        QImage image;
        bool pending = true;
        quint64 generation = 0;
    };
    QHash<QString, CodeBlockCacheEntry> m_codeBlockCache; ///< "language|width|theme|source" -> result
    quint64 m_codeBlockGeneration = 0;

    /// `![[Note]]`, `![[Note#Heading]]` or `![[Note#^block-id]]`: the target
    /// note's own text (or the named section of it), in a panel like a
    /// source file embed's. A plain-text slice, not a recursive re-render of
    /// the target's own markdown - ponytail: no nested live-preview styling,
    /// upgrade to one if a transcluded note's own formatting turns out to
    /// matter enough to pay for a second parser instance per embed.
    bool layoutNoteEmbed(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// The panel chrome a source-file, note or failed-`.tex` embed all share:
    /// a header strip with `label`, then `body` laid out as plain text
    /// (`monospace` picks the code font over the note font). Always
    /// succeeds - callers with a real failure still have text to show here,
    /// even if it is only the reason.
    bool layoutTextPanel(BlockView &view, const QString &label, const QString &body,
                        bool monospace);
    /// The slice of `noteText` that `subpath` (empty, `#Heading` or
    /// `#^block-id`) asks for, and the label to show above it. `ok` is false
    /// when a `#Heading` or `#^block-id` was asked for and not found.
    QString noteEmbedSlice(const QString &noteText, const QString &subpath, const QString &title,
                           QString &label, bool &ok) const;

    /// A block that holds two or more embeds - each on its own line, or
    /// several on one line - with nothing else in it, or an embed mixed in
    /// with running text. Obsidian renders every one of them; today only a
    /// block that is exactly one embed does (soleEmbedOf), so a paragraph of
    /// several, or a sentence with one in the middle, showed literal link
    /// text for every embed instead of the file.
    ///
    /// What this actually builds: `view.embedCells`, one row per source line
    /// that carries an embed (several embeds sharing a line sit side by side
    /// in that row), appended after the block's own text - not spliced in
    /// after that exact line. QTextLayout, used bare here rather than
    /// through QTextDocument, has no inline-object registration to hook a
    /// widget into a specific line the way Obsidian's CodeMirror does; doing
    /// that would mean hand-rolling line-by-line height injection into
    /// layoutBlock's shared layout loop, which every other block kind also
    /// runs through. ponytail: rows trail the paragraph instead of
    /// interleaving with it; upgrade path is in layoutBlock's own comment,
    /// next to where the trailing rows are appended.
    ///
    /// Each cell reuses layoutImage's and layoutPdf's own caches (a thumbnail
    /// is not rendered twice just because the same file is both a sole embed
    /// elsewhere and one of several here); anything that is not an image or
    /// a PDF page becomes a file card, including audio, video and notes -
    /// their own richer treatment is what a *sole* embed still gets.
    void collectTextEmbeds(const BlockView &view, QList<Range> &hidden,
                           QList<const EmbedSpan *> &found) const;
    void appendEmbedRows(BlockView &view, const QList<const EmbedSpan *> &embeds);
    BlockView::EmbedCell buildEmbedCell(const EmbedSpan &embed, qreal maxWidth);
    void paintEmbedCells(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    bool handleEmbedCellClick(const BlockView &view, const QPoint &point, qreal left,
                              qreal top);

    Features m_features;

    /// A block that is one `$$...$$` formula, laid out by MicroTeX.
    bool layoutMath(BlockView &view);
    const MathSpan *displayMathOf(const BlockView &view) const;
    /// Renders every inline `$...$` formula the block's own maths span list
    /// touches, and drops a formula's hide range back out of `hidden` when
    /// MicroTeX cannot draw it - so DisplayMap shows the raw source rather
    /// than an empty gap. Must run before `view.map` is built from `hidden`.
    void layoutInlineMaths(BlockView &view, QList<Range> &hidden);
    /// Draws the images `layoutInlineMaths` recorded, once the block's own
    /// QTextLayout exists and can say which line each one's "$" landed on.
    void paintInlineMaths(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// `[[Note#Heading]]` reads "Note > Heading" and `[[#Heading]]` reads
    /// "Heading", as they do in Obsidian, while the link is rendered. Hides a
    /// same-note link's "#" and records the others' separators, so it must run
    /// before `view.map` is built from `hidden`.
    void layoutAnchorSeparators(BlockView &view, QList<Range> &hidden);
    void paintAnchorSeparators(QPainter &painter, const BlockView &view, qreal left,
                               qreal top) const;

    // --- query blocks, implemented in QueryBlock.cpp -----------------------

    /// A fenced ```` ```query ```` block: renders its search results (a note
    /// list with match counts and highlighted snippets) instead of the query
    /// text, the way live preview renders any other block - false for every
    /// other fence, which falls through to the ordinary code listing.
    bool layoutQuery(BlockView &view);
    /// The cached entry for `queryText`, starting a search through
    /// m_queryRunner when there is none yet. Never blocks: a block shows
    /// "Searching..." until deliverQueryResults lands.
    struct QueryCacheEntry {
        QueryResults results;
        bool pending = true;
        /// Bumped each time a search for this text is (re)started, so a
        /// delivery from a superseded request - refreshQueries restarted one
        /// while it was still in flight - is recognised as stale and dropped.
        quint64 generation = 0;
    };
    const QueryCacheEntry &ensureQuerySearch(const QString &queryText);
    /// Called back (marshalled onto this thread) when a search started by
    /// ensureQuerySearch finishes. Ignored if `generation` is no longer the
    /// one on record for `queryText`.
    void deliverQueryResults(const QString &queryText, quint64 generation, QueryResults results);

    // --- base blocks, implemented in BaseBlock.cpp --------------------------

    /// A fenced ```` ```base ```` block: renders its shown view as a table
    /// instead of the raw YAML, the same shape layoutQuery already gives a
    /// ```query``` block. False for every other fence.
    bool layoutBase(BlockView &view);
    /// A base's table for `yamlText`, for a ```base fence and for an
    /// embedded .base file alike.
    bool layoutBaseTable(BlockView &view, const QString &yamlText);
    /// A .base file's text, read again only when its mtime moves.
    QHash<QString, QPair<qint64, QString>> m_baseFiles;
    /// The view's name (if any) above the grid, then paintTable for the
    /// grid itself - the table painting is not duplicated, only framed.
    void paintBase(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The cached entry for `yamlText`, starting an evaluation through
    /// m_baseRunner when there is none yet. Mirrors QueryCacheEntry.
    struct BaseCacheEntry {
        BaseTableData results;
        bool pending = true;
        quint64 generation = 0;
    };
    const BaseCacheEntry &ensureBaseSearch(const QString &yamlText);
    /// Called back (marshalled onto this thread) when an evaluation started
    /// by ensureBaseSearch finishes. Ignored if `generation` is no longer
    /// the one on record for `yamlText`. Mirrors deliverQueryResults.
    void deliverBaseResults(const QString &yamlText, quint64 generation, BaseTableData results);

    // --- HTML blocks and inline HTML, implemented in HtmlBlock.cpp --------

    /// A block that is nothing but raw HTML (`<div>`, `<table>`,
    /// `<details>`...) at the top level, rendered by litehtml into a cached
    /// image the way a formula or an embed is. False when litehtml is not
    /// compiled in (NOTEAPPC_WITH_HTML off, or it was not found), or the
    /// block has no visible content - either way the source shows instead.
    bool layoutHtml(BlockView &view);
    /// Resolves `<img src="...">` against `m_resolver`, the same one embeds
    /// use, and drops anything with a network scheme - litehtml never
    /// fetches a remote image. Local paths are rewritten to what the
    /// resolver returns; everything else about the markup is untouched.
    QString resolveHtmlImages(const QString &html) const;
    /// Pairs up inline HTML tags (`<b>...</b>`, `<span style="color:...">
    /// ...</span>`) within one block into QTextCharFormat runs, hiding the
    /// tags themselves the way `**bold**`'s asterisks are hidden. Must run
    /// before `view.map` is built from `hidden`, like layoutInlineMaths.
    /// Skipped for a block-level HTML span - that is litehtml's job, not
    /// this pairing pass.
    void layoutInlineHtml(BlockView &view, QList<Range> &hidden);
    /// The QTextCharFormat for a recognised inline tag, or nullopt for one
    /// this pass leaves as raw source (unknown tags, and anything in a
    /// `style=` attribute besides `color:`). `openTag` is the opening tag's
    /// own source text, for `<span style="...">`'s colour.
    std::optional<QTextCharFormat> htmlTagFormat(const QString &name, const QString &openTag) const;
    /// True when the newline at `source` joins two lines of one paragraph
    /// rather than breaking them apart.
    bool softBreakAt(qsizetype source) const;
    /// Extra space a callout reserves for its title row.
    qreal calloutHeaderHeight(const CalloutStyle &style) const;
    QList<QTextLayout::FormatRange> formatsIn(const BlockView &view, const Range &source,
                                              const DisplayMap &map) const;
    const TableSpan *tableFor(const BlockView &view) const;
    /// Heights, positions, and layouts for the blocks near the viewport.
    void updateLayout();
    void invalidateLayouts();
    qreal estimatedHeight(const BlockView &view) const;
    qreal lineHeightFor(const BlockSpan &span) const;
    mutable QHash<int, qreal> m_lineHeights; ///< memoised per block kind and level
    /// Moves every range at or after `at` by `delta`, so one keystroke does
    /// not need a reparse before the caret's block can be redrawn.
    void shiftRanges(qsizetype at, qsizetype delta);
    void applyEdit(qsizetype at, qsizetype removed, const QString &inserted);
    bool m_applyingExternal = false; ///< applyEdit for another view: no caret move, no edited()
    void updateScrollRange();
    int contentLeft() const;
    int contentWidth() const;
    /// Width reserved for the line-number gutter, 0 when it is not shown.
    int gutterWidth() const;
    void paintGutter(QPainter &painter, const BlockView &view, qreal top) const;

    /// True when a wikilink target has no note behind it, so live preview can
    /// dim it the way Obsidian does. Cached per parse: the resolver reaches
    /// the index, and a note full of links is laid out on every keystroke.
    bool isUnresolvedLink(const QString &target) const;
    bool resolvesToNothing(const QString &target) const;

    qsizetype blockAt(qsizetype sourceOffset) const;
    qsizetype offsetAt(const QPoint &viewportPoint) const;
    QPointF cursorPoint() const;
    void ensureCursorVisible();

    void insertText(const QString &text);
    void removeSelectionOrChar(int direction);
    void moveCursor(qsizetype offset, bool keepAnchor);
    void moveVertically(int lines, bool keepAnchor);
    Range selection() const;
    void pushUndo();
    void undo();
    void redo();

    // --- editing commands, implemented in EditingCommands.cpp -------------

    /// `applyEdit` leaves the caret after what it inserted; a command that
    /// wants it elsewhere - or wants the selection kept on the text it just
    /// moved - says so with this.
    void setSelectionRange(qsizetype anchor, qsizetype cursor);
    /// True when the keystroke was an auto-pair and needs no further handling.
    bool handleAutoPair(QChar typed);
    /// Backspace between an empty pair takes both halves.
    bool deleteAutoPair();
    /// Enter inside a list item: continue it, or end the list.
    bool handleListEnter();
    /// Tab / Shift+Tab on a list item. `direction` is +1 in, -1 out.
    bool handleListIndent(int direction);
    /// The run of consecutive list lines around `offset`: the stretch one
    /// structural edit renumbers, rewritten as a single splice.
    Range listRun(qsizetype offset) const;
    /// Wraps or unwraps the selection in `marker`; the whole of Ctrl+B/I and
    /// Ctrl+Shift+H.
    void toggleWrap(const QString &marker);

    void updateFindMatches();
    /// Puts the current match on screen with the viewport-only layout path.
    void scrollToOffset(qsizetype offset);
    void syncFindBar();
    void positionFindBar();
    /// `--text-highlight-bg`, or Obsidian's own default when the theme has no
    /// such token.
    QColor findHighlightColor() const;
    /// Background ranges for the matches inside `view`, in display offsets.
    void appendFindHighlights(const BlockView &view,
                              QList<QTextLayout::FormatRange> &into) const;

    /// Text typed after the nearest unclosed `[[` before the caret; unset when
    /// the caret is not inside a wikilink.
    std::optional<QString> wikilinkPrefix() const;
    void updateSuggestions();
    void insertCompletion(const QString &completion);

    QList<QTextLayout::FormatRange> formatsFor(BlockView &view);
    /// A wavy underline under each word of `view` the dictionary rejects.
    void appendSpellingFormats(const BlockView &view, QList<QTextLayout::FormatRange> &formats) const;
    /// The words of `view` spelling looks at, in source coordinates: prose
    /// only, never code, links, tags or maths, and not the word being typed.
    QList<Range> checkedWordsOf(const BlockView &view) const;
    /// The word the caret is in or just after, while spelling is on.
    Range wordAtCaret() const;
    /// Kate-definition colours for a fenced code block, in the coordinates of
    /// that block's display string. Not const: a piece records the state it
    /// ended in, for the piece after it.
    QList<QTextLayout::FormatRange> codeFormatsFor(BlockView &view);
    /// Highlights one piece from the end state of the piece before it and
    /// records where it ended; the formats are relative to `*content`.
    QList<QTextLayout::FormatRange> highlightPiece(BlockView &view, Range *content);
    /// Brings the end state of every piece before `view` up to date, so
    /// `view` can start from the one just before it.
    void updateCodeStatesBefore(BlockView &view);
    const BlockView &firstPiece(const BlockView &view) const { return *(&view - view.piece); }
    const BlockView &lastPiece(const BlockView &view) const
    {
        return *(&view - view.piece + view.pieces - 1);
    }
    QList<QTextLayout::FormatRange> nestedCodeFormatsFor(const BlockView &view) const;
    QList<BlockSpan> plainParagraphs() const;
    /// The code between the fences, and the language the fence declared.
    /// For one piece of a long fence: that piece's share of the code, and the
    /// fence's language.
    Range codeContentOf(const BlockView &view, QString *language) const;
    QFont fontFor(const BlockSpan &span) const;

    Theme m_theme = Theme::builtin(Theme::Mode::Dark);
    /// `~/.config/noteAppC/callouts.ini` and `codeblocks.ini`, reread the
    /// moment either changes on disk - they are the way a person restyles
    /// these, so an edit has to show without a restart.
    CalloutStyles m_calloutStyles = CalloutStyles::load();
    LanguageStyles m_languageStyles = LanguageStyles::load();
    QFileSystemWatcher *m_configWatcher = nullptr;
    void watchStyleFiles();
    void reloadStyleFiles();
    Parser m_parser;
    /// Every block between `exposedTop` and `exposedBottom`, which are in
    /// viewport coordinates for a document scrolled to `scroll`. The screen
    /// and a PDF page both paint through this.
    void paintBlocks(QPainter &painter, qreal scroll, qreal exposedTop, qreal exposedBottom) const;
    CodeHighlighter m_code;
    /// Plays audio and video embeds; made on first use, one file at a time.
    class MediaPlayback *m_media = nullptr;
    bool layoutMedia(BlockView &view, const QString &path, bool video);
    void paintMedia(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// A player card's parts, in viewport coordinates: the picture (empty for
    /// audio), the play button and the progress bar.
    QRectF mediaScreenRect(const BlockView &view, qreal left, qreal top) const;
    QRectF mediaButtonRect(const BlockView &view, qreal left, qreal top) const;
    QRectF mediaBarRect(const BlockView &view, qreal left, qreal top) const;
    /// Play, pause or seek, for a click at `point` on a player card.
    bool handleMediaClick(const BlockView &view, const QPoint &point, qreal left, qreal top);
    /// The link a preview was asked for, so the signals fire on changes only.
    QString m_previewLink;
    void updateLinkHover(const QPoint &viewportPoint, bool control, const QPoint &globalPos);
    /// A key combination, as QKeyCombination::toCombined(), to the command
    /// it runs.
    QHash<int, QString> m_keymap;
    Spelling m_spelling;
    /// The inline editor over a property value, while one is open, and the
    /// key it edits: a reparse can renumber the properties under it.
    QPointer<QLineEdit> m_propertyEdit;
    QString m_editedKey;
    /// What the open editor writes: the scalar value (kEditValue), a new list
    /// item (kEditNewItem), or list item n.
    int m_editedItem = -1;
    static constexpr int kEditValue = -1;
    static constexpr int kEditNewItem = -2;
    /// Where the last updateLayout() moved blocks from, in document y; +inf
    /// when nothing moved. A scroll can reuse the pixels on screen only if
    /// nothing on screen moved.
    qreal m_movedFrom = 0;
    MathRenderer m_math;
    std::function<QString(const QString &)> m_resolver;
    mutable QHash<QString, QPixmap> m_images;
    Mode m_mode = Mode::LivePreview;

    std::function<void(const QString &, const QueryDeliver &)> m_queryRunner;
    QHash<QString, QueryCacheEntry> m_queryCache; ///< query text -> its last search
    quint64 m_queryGeneration = 0;
    /// Debounces refreshQueries(): Index::changed can fire once per save, and
    /// a burst of them should cost one re-search per block, not one each.
    QTimer *m_queryRefreshTimer = nullptr;
    /// A query block's collapsed/sort-by-name state, by its own source start
    /// offset - like m_folds, session-only and never written to the note,
    /// and orphaned (harmlessly - it just falls back to the default) if an
    /// edit earlier in the note shifts the block's offset.
    QHash<qsizetype, bool> m_queryCollapsed;
    QHash<qsizetype, bool> m_queryNameSort;

    std::function<void(const QString &, const BaseDeliver &)> m_baseRunner;
    QHash<QString, BaseCacheEntry> m_baseCache; ///< YAML text -> its last evaluation
    quint64 m_baseGeneration = 0;
    /// Debounces refreshBases(), the same reason m_queryRefreshTimer does.
    QTimer *m_baseRefreshTimer = nullptr;

    QString m_text;
    SourceMap m_lines; ///< line starts, for the gutter
    Decorations m_decorations; ///< styles and syntax kept sorted by start
    mutable QHash<QString, bool> m_unresolved; ///< link target -> has no note
    QList<Property> m_properties; ///< frontmatter, for the properties table
    QList<Heading> m_headings; ///< as of the last parse, for the outline pane
    std::vector<BlockView> m_blocks; // move-only: QTextLayout is not copyable
    /// Fold state by block start offset. A block with no entry is in the state
    /// its source asked for, so folding survives a reparse without the
    /// decorations having to carry it.
    QHash<qsizetype, bool> m_folds;
    mutable qreal m_blockSpacing = 0; ///< memoised, cleared with the theme

    qsizetype m_cursor = 0;
    qsizetype m_anchor = 0;
    qsizetype m_activeBlock = -1;
    qreal m_goalX = -1; ///< remembered column for up/down

    /// A freshly opened note renders whole; the block under the caret only
    /// shows its source once the reader actually edits or clicks into it,
    /// which is how Obsidian behaves.
    bool m_interacted = false;
    bool m_modified = false;
    bool m_readable = true;
    bool m_lineNumbers = true; ///< shown in source mode only
    bool m_strictLineBreaks = false; ///< Obsidian's app.json setting
    bool m_indentGuides = false;
    bool m_outlineNumbering = false;
    int m_tabWidth = 4;
    bool m_useWikilinks = true;
    bool m_GPUacceleration = false;
    /// The left button is held: what is revealed stays exactly as it was
    /// until release, so a drag never selects across text that just moved.
    bool m_pressing = false;
    /// Where the last press landed, in source coordinates.
    qsizetype m_pressOffset = 0;
    /// A finger held still on the text selects the word under it, the way
    /// a double-click does with a mouse. Only presses from a touchscreen.
    QTimer m_longPress;
    QPoint m_pressPoint;
    /// The code block whose label just copied, shown as "Copied" briefly; and
    /// the one under the pointer, which is when a block without a language
    /// shows its "Copy" label at all. Both keyed by the block's source start.
    qsizetype m_copiedBlock = -1;
    /// Tab groups, sorted by start, rebuilt with each parse; and which tab
    /// each shows, keyed by the group's start so it survives a reparse.
    std::vector<TabGroup> m_tabGroups;
    QHash<qsizetype, int> m_selectedTabs;
    qsizetype m_hoveredCodeBlock = -1;
    bool m_autoPairs = true; ///< Obsidian ships both auto-pair settings on
    int m_contentWidth = 700;
    int m_minimumSideMargin = 24;
    int m_topPadding = 28;
    qreal m_totalHeight = 0;

    struct Snapshot {
        QString text;
        qsizetype cursor = 0;
    };
    QList<Snapshot> m_undo;
    QList<Snapshot> m_redo;
    QElapsedTimer m_lastEdit;

    /// Obsidian's in-note find bar: the matches live here because the editor
    /// paints them, the bar is only the field and the buttons.
    struct FindState {
        QString pattern;
        bool caseSensitive = false;
        bool regex = false;
        bool valid = true;
        QList<Range> matches; ///< sorted, non-overlapping
        int current = -1;
    };
    FindState m_find;
    FindBar *m_findBar = nullptr;

    QCompleter *m_completer = nullptr;
    QTimer m_parseTimer;
    /// The parse runs off the UI thread; a generation counter says whether the
    /// result still describes the text in the buffer.
    struct ParseJob {
        Decorations decorations;
        QList<Property> properties;
        QList<Heading> headings;
        int words = 0;
        quint64 version = 0;
    };
    int m_wordCount = 0;
    void setWordCount(int words);
    QFutureWatcher<ParseJob> m_parseWatcher;
    quint64 m_textVersion = 0;
    quint64 m_parsedVersion = 0;
    bool m_cursorOn = true;
    QTimer m_blink;
};

} // namespace noteAppC
