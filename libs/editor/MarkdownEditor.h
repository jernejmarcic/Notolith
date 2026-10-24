#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QHash>
#include <QPageLayout>
#include <QPageSize>
#include <QPointer>
#include <QTextLayout>

#include <functional>
#include <memory>
#include <span>
#include <optional>
#include <vector>

class QAbstractItemModel;
class QCompleter;
class QFileSystemWatcher;
class QNetworkAccessManager;
class QMimeData;
class QModelIndex;
class QStringListModel;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;

#include "BulletGlyphs.h"
#include "Decorations.h"
#include "DisplayMap.h"
#include "Parser.h"
#include "TaskStates.h"
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

struct ReferenceExcerpt;

class SyntaxMap;
class VimEngine;

/// Turns each HTML block whose every tag the editor can lay out itself into
/// the paragraphs, headings, lists and tables a Markdown note would give,
/// tags as syntax (HtmlEdit.cpp). Run on a fresh parse, before it is sorted;
/// a no-op without gumbo.
void expandEditableHTML(Decorations &decorations, const QString &text);

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
    Mode mode() const;

    /// Replaces the buffer and clears undo. Returns the caret to the top.
    void setNoteText(const QString &text);
    QString noteText() const;

    bool isModified() const;
    void setModified(bool modified);

    /// Line numbers in a gutter, the way Obsidian shows them in source mode.
    void setLineNumbers(bool on);
    bool lineNumbers() const;

    void setReadableLineLength(bool on);
    /// The clear space at the left and right of the text and above the first line, in pixels (24 and 28 unless set).
    /// A host that draws the editor into a small rect, as an ink text box does, sets both to 0.
    void setViewportPadding(int side, int top);
    void setContentWidth(int pixels);

    /// CommonMark's rule: every newline is a line break. Off, a single newline
    /// inside a paragraph joins, which is what Obsidian does by default.
    void setStrictLineBreaks(bool on);
    bool strictLineBreaks() const;

    /// What the parse looks for beyond Markdown (Parser.h): bare `[brackets]`
    /// drawn in --bracket-color (on by default, like other syntax colouring),
    /// meta notes in --meta-note-color, and what the word and character counts
    /// leave out. Off, the parse does not look at all. A change reparses.
    void setParseOptions(const ParseOptions &options);
    const ParseOptions &parseOptions() const;
    /// Every meta note, outermost, delimiters included, in source order -
    /// empty unless ParseOptions::metaNotes. For a pane or a jump to build on.
    const QList<Range> &metaNoteRanges() const;
    enum class MetaJump { None, Moved, Wrapped };
    /// Alt+] / Alt+[: selects the whole next (`direction` > 0) or previous
    /// meta note after the selection, delimiters included, so Backspace
    /// deletes it and an arrow key lets you edit inside. Past the last one it
    /// wraps to the first (Wrapped). `index` / `count` get the 1-based
    /// position of the run reached and the number of runs. Linear in the runs.
    MetaJump selectMetaNote(int direction, int *index = nullptr, int *count = nullptr);
    /// Selects `range` and scrolls to it: a pane row was clicked.
    void selectRange(const Range &range);

    /// Settings > Editor > Fold heading / Fold indent: a heading hides its
    /// section, a list item with children hides them. On by default.
    void setFolding(bool heading, bool indent);
    /// A faint vertical rule at each level of list indentation.
    void setIndentGuides(bool on);
    bool indentGuides() const;
    /// Nested ordered lists numbered as Word's multilevel lists are - 1.,
    /// then 1.1, 1.2, then 1.2.1 - rather than each list counting on its own.
    void setOutlineNumbering(bool on);
    /// A table's source aligned into columns when the caret leaves a table
    /// it edited, as Advanced Tables does. On by default.
    void setAlignTables(bool on);
    /// The `</>` button over a rendered callout, formula, table, embed or
    /// plugin block (HoverEdit.cpp). On by default; off tracks no hover.
    void setHoverEditButton(bool on);
    /// The button's rect in viewport coordinates, invalid unless the pointer
    /// is over a block that has one (for the tests too).
    QRectF editButtonRect() const;
    /// Row and column grips, add-row and add-column bars and rectangular cell
    /// selection on a rendered pipe table (TableHandles.cpp). On by default;
    /// off tracks no hover for tables and a drag across cells selects text.
    void setTableHandles(bool on);
    /// The parts of a table's hover chrome, for hit tests and the tests.
    enum class TablePart { None, RowGrip, ColumnGrip, AddRow, AddColumn };
    TablePart hoveredTablePart() const;
    /// How many rows or columns the add bar being dragged would add; 0 when none is.
    int tableAddPreview() const;
    /// Whether a table's grips and bars are up (hover, or a finger's tap).
    bool tableChromeShown() const;
    /// Whether the table autoscroll timer is running (for the tests).
    bool tableAutoScrolling() const;
    /// Viewport rect of cell (`row`, `column`) of the rendered table holding
    /// `offset`, or of one of its handles (`index` is the row or the column
    /// for a grip). Empty when that is not drawn.
    QRectF tableCellRect(qsizetype offset, int row, int column) const;
    QRectF tableChromeRect(qsizetype offset, TablePart part, int index = 0) const;
    /// The selected cells as x = first column, y = first row; null when none.
    QRect cellSelectionRect() const;
    /// Selects the whole of the block `blockIndex` (any piece) with the
    /// caret at its start, which is what shows its source.
    void editBlockSource(qsizetype blockIndex);
    /// What the callout menu does to the top-level callout holding `offset`
    /// (HoverEdit.cpp), one undo step each. All false or empty when there is
    /// none. `calloutSourceAt` is its text (no trailing newline);
    /// `changeCalloutType` rewrites `[!type]` or `:::type`; `removeCallout`
    /// takes the `>` prefixes or the `:::` fences and the marker off and keeps
    /// the content, an explicit title as a first paragraph.
    QString calloutSourceAt(qsizetype offset) const;
    bool changeCalloutType(qsizetype offset, const QString &type);
    bool removeCallout(qsizetype offset);
    qsizetype hoveredBlock() const;
    /// The panel a block of the one at `sourceOffset` is drawn in (all of a
    /// long block's pieces), viewport coordinates; the ring is drawn on it.
    QRectF blockPanelAt(qsizetype sourceOffset) const;
    /// Colour knobs on code blocks - `bg`, `fg`, `border`, `syntax.*` from
    /// codeblocks.ini, a fence's own attributes and the theme. On by default;
    /// off draws every block as if none of it were configured.
    void setComplexCodeBlocks(bool on);
    /// The alternative highlights `info:2 warn:4-6` may name, as the
    /// `editor.codeHighlights` setting spells them (`info=blue, warn=#E5A100`).
    /// Only painting reads this: the names the parser accepts are
    /// ParseOptions::highlightKinds.
    void setCodeHighlights(const QString &spec);
    bool complexCodeBlocks() const;
    bool outlineNumbering() const;
    /// What an ordered number ends in as drawn: "." (the default), ":", ")" or
    /// anything short. Empty draws the delimiter the note was written with.
    /// Display only; the file keeps its own.
    void setListNumberSuffix(const QString &suffix);
    QString listNumberSuffix() const;
    /// The glyph each bullet marker is drawn as, "-=dash,*=dot,+=diamond"
    /// (BulletGlyphs.h). Display only; the file keeps `-`, `*` and `+`.
    void setBulletGlyphs(const QString &spec);
    BulletGlyphMap bulletGlyphs() const;
    /// Nested bullets (depth >= 1) drawn as a turn arrow instead of by marker;
    /// "typed" glyphs stay as typed. On by default.
    void setNestedBulletGlyphs(bool on);
    bool nestedBulletGlyphs() const;
    /// What an ordered item's number reads as drawn (for the tests too).
    QString listLabel(const ListItemSpan &item) const;

    /// Spaces a Tab inserts where it is not indenting a list item.
    void setTabWidth(int spaces);
    int tabWidth() const;

    /// Whether Ctrl+K on a selection writes `[[Note]]` or `[Note](Note.md)`.
    void setUseWikilinks(bool on);

    /// Spelling, from the spellcheck module when it is compiled in and
    /// switched on. The editor only asks; it never links a dictionary. An
    /// empty one - no `isCorrect` - turns underlining off.
    struct Spelling {
        std::function<bool(const QString &)> isCorrect;
        std::function<QStringList(const QString &)> suggestions;
        std::function<void(const QString &)> addWord;
        /// Optional: whether `word` is in the personal dictionary, and take it
        /// out again. Without them the menu has no "Remove from dictionary".
        std::function<bool(const QString &)> isPersonal;
        std::function<void(const QString &)> removeWord;
    };
    void setSpelling(Spelling spelling);
    /// Lays every block out again for new spelling answers (a word added to
    /// the dictionary), keeping each block's height so the view does not move.
    void refreshSpelling();

    /// Types `text` at the caret, replacing any selection, as one undo step:
    /// how a template or a snippet goes in.
    void insertAtCaret(const QString &text) { insertText(text); }
    /// The menu a right click on a misspelt word at `sourceOffset` opens:
    /// its suggestions, then "Add to dictionary". Null anywhere else. Built
    /// rather than shown, so it can be tested.
    std::unique_ptr<QMenu> spellingMenuAt(qsizetype sourceOffset);

    /// Which window-level items the context menu offers. Off/empty: item absent.
    struct ContextActions {
        bool vaultSearch = false;  ///< "Search for ..." -> searchRequested()
        bool lookup = false;       ///< "Look up ..."    -> lookupRequested()
        QString webSearchURL;      ///< "%1" = URL-encoded text; empty hides the item
    };
    void setContextActions(ContextActions actions);
    /// The menu a right click at `sourceOffset` opens. Built rather than
    /// shown, so it can be tested. `anchor` is the screen point "Look up"
    /// places its popover at; null: where the mouse is.
    std::unique_ptr<QMenu> contextMenuAt(qsizetype sourceOffset, const QPoint &anchor = {});
    bool useWikilinks() const;

    /// Paints the viewport through OpenGL instead of Qt's raster engine.
    ///
    /// Whether this is faster is a question for the benchmark, not for
    /// intuition: the GL paint engine uploads a glyph cache and re-uploads it
    /// whenever the cache is evicted, which a page of text can do on its own.
    /// Swapping the viewport tears down and rebuilds the backing widget, so
    /// this is meant to be set once at construction, not toggled per frame.
    void setGPUacceleration(bool on);
    bool GPUacceleration() const;
    /// True when this build can do it at all: Qt6::OpenGLWidgets present, and
    /// a platform that can give the window a GL context.
    static bool GPUaccelerationAvailable();
    /// The `editor.gpuAcceleration` setting ("true", "false" or "auto") for
    /// a screen of `devicePixelRatio`. Auto is OpenGL only at a fractional
    /// scale: a scroll step there moves the note by a fraction of a device
    /// pixel, which a raster backing store cannot shift (QBackingStore::scroll
    /// refuses, and so does Qt Wayland's shm store at any fractional scale),
    /// so raster repaints the whole viewport every frame. Measured on PADD at
    /// 1.67 and shuttlepod-1 at 1: docs/PERFORMANCE.md, "GPU acceleration".
    static bool GPUaccelerationWanted(const QString &setting, qreal devicePixelRatio);

    /// Called, on the UI thread, once when the parse of a note over 128 kB first
    /// lands (or is made on the spot by a settings change): the parser's
    /// temporaries are free by then and still resident. The application gives
    /// the memory back; the editor never does. A temporary editor (a canvas
    /// card's render, a PDF export) fires it too, which costs one 5 ms trim on
    /// a worker thread and does no harm.
    static void setBigParseLandedHook(std::function<void()> hook);

    /// Obsidian's Export to PDF options, with its defaults.
    struct PdfOptions {
        bool includeTitle = true;
        QPageSize pageSize = QPageSize(QPageSize::A4);
        QPageLayout::Orientation orientation = QPageLayout::Portrait;
        qreal marginMillimetres = 25.4; ///< "Default", about an inch
        int downscalePercent = 100;
        bool taskStates = false; ///< ParseOptions::taskStates: the note's own checkbox states
        bool codeStyler = false; ///< ParseOptions::codeStyler: Code Styler's syntax, drawn as on screen
        QString codeHighlights;  ///< the `editor.codeHighlights` setting: the names `info:2` may use
        QString vaultRoot; ///< what a ```reference block may read from: empty shows no local file
        bool remoteReferences = false; ///< the `editor.remoteReferences` setting: a ```reference may fetch a URL
        /// Paper is white, so the light theme unless a caller passes its own
        /// (the person's formatting colours on top of it).
        std::optional<Theme> theme;
        /// The markmap plugin: on, a ```markmap fence prints as the map, drawn
        /// from its text in `theme`; off, as the listing.
        bool mindMaps = true;
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
    int wordCount() const;
    /// Waits for a large note's pool parse and adopts it now, without running
    /// the event loop (so no timer, deferred delete or paint fires meanwhile).
    /// For callers that must have the parsed note in hand: a grab, an export.
    void finishParse();
    /// The source offset of the line at the top of the viewport (0 when not
    /// scrolled): what a remembered place is made of. A scroll pixel only
    /// means the same text in the layout it was taken in, and a big note's is
    /// not the one it opens in.
    qsizetype topOffset() const;
    /// Restores a remembered place. While a big note shows its chunks a pixel means other text
    /// than it did in the parsed note, so the line at `top` goes to the top of the viewport;
    /// otherwise (and for `top` <= 0, an entry from before offsets) the scroll pixel is set.
    void restoreScroll(qsizetype top, int pixel);
    /// Characters in the note, frontmatter excluded (as wordCount is). Cheap:
    /// the text's length less the properties block's.
    qsizetype characterCount() const;
    /// Words and characters of the selection (frontmatter not special-cased),
    /// zeros with none. Linear in the selection; the status bar asks after a
    /// short pause, never per keystroke.
    struct TextCount {
        int words = 0;
        qsizetype characters = 0;
    };
    TextCount selectionCount() const;
    /// Source offset of the caret, for tests and for the status bar.
    qsizetype cursorOffset() const;
    /// The selected source text, exactly as it is in the file - markers
    /// included, since that is what a copy has to carry.
    QString selectedText() const;
    void setCursorOffset(qsizetype offset);
    /// A touch selection's handle, viewport coordinates; empty when the
    /// selection has none (it was not made by a finger). For the tests.
    QRectF touchSelectionHandle(bool end) const;

    /// What the widget currently shows, block by block. The quickest way to
    /// assert live-preview behaviour, and useful when debugging a layout.
    QString displayText() const;
    /// The character format laid out at `sourceOffset`, merged from every
    /// range covering it; empty when its block has no layout yet. The way to
    /// assert what colour a token got without comparing pixels.
    QTextCharFormat formatAt(qsizetype sourceOffset) const;

    /// Height of the whole note as it is currently laid out. What the scroll
    /// bar is built from, and the only honest way to assert block spacing.
    qreal documentHeight() const;

    /// Pretends the reader clicked into the block holding `offset`, so it
    /// reveals its source. Used by the tests and by "reveal on open".
    void enterBlockAt(qsizetype offset);
    /// Shows tab `member` of the code tab group containing `sourceOffset`.
    /// What a click on a tab does; public for tests and for a keyboard command.
    void selectCodeTab(qsizetype sourceOffset, int member);
    /// How many blocks have been laid out since the editor was built. For
    /// tests: the difference across an action is how much it relaid.
    quint64 layoutCount() const;
    /// How many block views rebuildBlocks has made afresh since the editor was
    /// built (a parse that left the blocks as they were keeps most). For tests:
    /// the difference across a landing is how much of the note it rebuilt.
    quint64 rebuiltViewCount() const;

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

    /// Where the title of the top-level callout holding `offset` is drawn and
    /// clicked, in viewport coordinates, for the tests - null when there is
    /// no such callout on screen.
    QRectF calloutTitleRectAt(qsizetype offset) const;

    /// Link target under `viewportPoint`, empty when there is none.
    QString linkAt(const QPoint &viewportPoint) const;

    /// Names the `[[` suggester offers. Refreshed from the index.
    void setLinkTargets(const QStringList &targets);
    /// `aliases:` entries as {alias, note name}: the suggester offers each as
    /// "alias -> Note" and inserts `Note|alias`.
    void setLinkAliases(const QList<QPair<QString, QString>> &aliases);
    /// The headings (`blocks` false) or block ids of a note, for `[[Note#` and
    /// `[[Note#^`. Asked once per popup, never for the note being edited:
    /// `[[#` is answered from the editor itself. After `[[Note#H1#` `note` is
    /// `Note#H1` and the answer is the headings nested under H1.
    void setLinkFragmentProvider(std::function<QStringList(const QString &note, bool blocks)> provider);

    /// Turns a link target into an absolute path, or an empty string when it
    /// does not resolve. Supplied by the window, which owns the index.
    void setLinkResolver(std::function<QString(const QString &)> resolver);
    /// Pictures that live in the note's own document rather than in a file:
    /// name -> encoded image bytes. `![x](attachment:name)` shows the one
    /// called `name`, as a notebook's markdown-cell attachments do. Read-only
    /// and never written anywhere; an empty table (the default) costs nothing.
    /// `scopeAt`, when given, says for an offset of the text what is put before
    /// the name to find its picture in the table (a notebook's cells each have
    /// their own `image.png`: "3/"); empty for none, so a name stays a bare one.
    void setInlineImages(QHash<QString, QByteArray> images, std::function<QString(qsizetype)> scopeAt = {});
    /// The index changed, so a link drawn as pointing nowhere may point
    /// somewhere now, or the reverse. Relays out only if one did.
    void refreshLinkStatus();
    /// Files came or went in the vault without the index saying so (an image, a
    /// PDF, a CSV, a whole folder): looks at the cached tables on disk again,
    /// then at every link's status.
    void refreshVaultFiles();

    // --- paste and drop --------------------------------------------------
    // The editor never links libs/vault, so saving an attachment or
    // resolving a dropped path is the window's job; these seams hand it
    // back the link text to insert (or empty to insert nothing). See
    // PasteDrop.cpp.
    /// Saves a pasted or dropped image's bytes as `suggestedName` (a name,
    /// not a path); returns the `![[...]]` text to insert, or empty on
    /// failure.
    using AttachmentSaver = std::function<QString(const QString &suggestedName, const QByteArray &bytes)>;
    void setAttachmentSaver(AttachmentSaver saver);
    /// Imports a local file dropped from outside the editor (a path, not a
    /// URL); returns the link or embed text to insert, or empty on failure.
    using FileImporter = std::function<QString(const QString &sourcePath)>;
    void setFileImporter(FileImporter importer);

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
    /// Runs `yamlText` (a ```` ```base ```` block's own content, or a
    /// `.base` file embed's) and calls `deliver` exactly once with the
    /// result, from any thread. `viewName` is the view to show - a `.base`
    /// embed's `#fragment`, empty for a fenced block (always the first
    /// view) and for an embed with no fragment. Supplied by the window,
    /// which owns the index and vault; the editor never links libs/ext/bases
    /// (docs/PLUGINS.md's module boundary) - see bases::evaluateBaseForEditor,
    /// which is what a real runner calls with `viewName` passed straight
    /// through. Layout never waits on it: a block shows "Loading..." until
    /// `deliver` is called, the same contract setQueryRunner keeps for a
    /// search.
    void setBaseRunner(
        std::function<void(const QString &yamlText, const QString &viewName, const BaseDeliver &deliver)>
            runner);
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
        bool CSVembeds = true;   ///< `![[x.csv]]` / `![[x.tsv]]` as a table
        bool notebooks = true;   ///< running a code block through a Jupyter kernel
        bool operator==(const Features &) const = default;
    };
    void setFeatures(const Features &features);
    const Features &features() const;

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

    /// Mouse/wheel input landing on a hooked embed's own image, forwarded so
    /// its renderer can pan, zoom and open things without the editor knowing
    /// what kind of file it is - `.canvas`'s own read-only view inside a
    /// note. `localPoint` is relative to the embed's own top-left, in the
    /// same (unscaled) pixel space FileEmbedRenderer's own `width` sized the
    /// image to. A null `image` in the result leaves the block painted as it
    /// was; a Wheel event answered that way is also left unaccepted, so the
    /// editor's own handling - scrolling the note - runs instead, which is
    /// how an embed avoids ever leaving the page stuck. A non-empty
    /// `openPath` is a vault-relative path, emitted as fileEmbedActivated()
    /// exactly the shape baseRowActivated's own relativePath already is.
    struct FileEmbedInputResult {
        QImage image;
        QString openPath;
    };
    enum class FileEmbedInputKind { Wheel, Drag, Click, DoubleClick };
    struct FileEmbedInputEvent {
        FileEmbedInputKind kind = FileEmbedInputKind::Click;
        QPoint localPoint;
        /// Wheel: angleDelta. Drag: the viewport-pixel move since the last
        /// Drag event of the same gesture (or since the press that started
        /// it). Unused otherwise.
        QPoint delta;
        Qt::KeyboardModifiers modifiers;
    };
    using FileEmbedInputHandler =
        std::function<FileEmbedInputResult(const QString &path, const QString &subpath,
                                           const FileEmbedInputEvent &event)>;
    void setFileEmbedInputHandler(const QStringList &suffixes, FileEmbedInputHandler handler);

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
        /// Places on `image`, in logical pixels from its top left, that a
        /// click answers instead of putting the caret there (a mind map's
        /// fold circles). `activate` is then asked, with that spot's `id`
        /// and a `deliver` of the same contract as the renderer's own, for
        /// the picture that replaces this one; the editor keeps the old one
        /// up until it comes. Empty for a picture with nothing to click.
        struct Hotspot {
            QRectF rect;
            int id = 0;
        };
        QList<Hotspot> hotspots;
        std::function<void(int id, const std::function<void(CodeBlockResult)> &deliver)> activate;
    };
    using CodeBlockRenderer =
        std::function<void(const QString &language, const QString &source, int width, const QString &theme,
                           const std::function<void(CodeBlockResult)> &deliver)>;
    void setCodeBlockRenderer(const QStringList &languages, CodeBlockRenderer renderer);

    /// One piece of a notebook block's output (NotebookBlock.cpp, stage 13):
    /// a Jupyter kernel's stream/execute_result/display_data/error, already
    /// reduced to what this editor can draw without linking libs/ext -
    /// libs/ext/notebook's own NotebookOutput is the source this is built
    /// from, one field at a time, by whatever the window's runner is.
    struct NotebookOutput {
        /// The same order as libs/ext/notebook's NotebookOutput::Kind: the
        /// window converts by number.
        enum class Kind { Text, Markdown, ImagePng, ImageSvg, Error, Html };
        Kind kind = Kind::Text;
        QString text;          ///< text/plain (ANSI stripped), text/markdown source, HTML, or an error's evalue
        QByteArray imageBytes; ///< Kind::ImagePng only (PNG or JPEG)
        QString ename;         ///< Kind::Error only
        QStringList traceback; ///< Kind::Error only, ANSI already stripped
        bool isStderr = false; ///< Kind::Text only: drawn in the error colour
        QString alt;           ///< Kind::Html only: the plain text shown when litehtml is not built in
    };
    /// A block's last run, or - `hasRun` false - nothing run yet, which is
    /// the normal state for a block nobody has clicked "Run" on.
    struct NotebookRunResult {
        bool hasRun = false;
        bool ok = true;
        qint64 wallMs = -1;
        QList<NotebookOutput> outputs;
        QString kernelError; ///< no kernel for this language, one that would not start...
        int executionCount = 0; ///< the kernel's `In [n]`; 0 for a run with no kernel
        /// Set by the editor, not the runner: this is an earlier run of the
        /// block, kept on screen after its source changed until it runs again.
        bool stale = false;
    };
    /// Delivers a block's result exactly once, from any thread - the editor
    /// marshals back onto its own thread before touching a block, the same
    /// contract setQueryRunner keeps for a search.
    using NotebookDeliver = std::function<void(NotebookRunResult)>;
    /// A running block's input(): what it asks, and how the person's answer
    /// goes back. `ok` false is Esc - the run is interrupted rather than
    /// answered. NotebookBlock's run passes a handler in the options and the
    /// editor shows the prompt under the block (NotebookInput.cpp).
    struct NotebookInputPrompt {
        QString prompt;
        bool password = false; ///< getpass: the line is not echoed
    };
    using NotebookInputReply = std::function<void(bool ok, const QString &line)>;
    using NotebookInputHandler = std::function<void(const NotebookInputPrompt &, const NotebookInputReply &)>;
    /// What a run request carries besides the language and source - stage
    /// 13's rest (per-block sandbox tier, runnable embeds). Kept as one
    /// struct rather than growing NotebookRunner's own parameter list again
    /// for the next thing this needs to carry.
    struct NotebookRunOptions {
        /// `#| sandbox: host` on a fenced block - tier 0, opt-in. Always
        /// false for a runnable embed (no `#|` lines to set it from).
        bool sandboxHost = false;
        /// `#| sandbox: wasm`: a WebAssembly module under wasmtime. Stricter
        /// than the default, so no setting gates it.
        bool sandboxWasm = false;
        /// A runnable embed's own folder, so a script that reads sibling
        /// files still can - bound read-only in the bubblewrap tier (the
        /// window's job; the editor only names the path). Empty for a
        /// fenced block.
        QString workingDir;
        /// `#| cache: false`: nothing is read from or written to the sidecar.
        bool cache = true;
        /// `#| timeout: N`, in seconds; 0 is the runner's own limit.
        int timeoutSeconds = 0;
        /// `#| kernel:` - "" the setting's choice, "off" the system's own
        /// tool, "on" the language's Jupyter kernel, or a kernelspec's name.
        QString kernel;
        /// The note's `implicit-<lang>` and `implicit-<lang>-main` frontmatter
        /// (empty: none, or `#| implicit: false`): code the runner puts above
        /// the block, and the template the block goes into when it has no
        /// entry point of its own.
        QString implicit;
        QString implicitMain;
        /// Which note's kernel: blocks that share it share variables.
        /// Left empty here; the window fills it from the note it is in.
        QString scope;
        /// Which of the note's identical blocks this is: how many earlier ones
        /// read the same (0 for the first). Part of the output cache's key
        /// only - the code that runs is never changed by it.
        int duplicateOrdinal = 0;
        /// Remove this block's kept output instead of reading or running it.
        bool clear = false;
        /// Set on a real run (never on a lookup): the runner may call it, from
        /// any thread, when the code asks for a line. Unset: no stdin.
        NotebookInputHandler onInput;
    };
    /// Asks for `language`/`source`'s result. `forceRun` false is a
    /// cache-only lookup that must never start a kernel - what every
    /// notebook block asks for the moment it is laid out, so a reopened
    /// note shows its last output without running anything; `forceRun` true
    /// is what the block's own "Run" control (paintCodeChrome,
    /// codeLabelOf, handleBlockClick) and the window's "Run all blocks"
    /// command ask for. Supplied by the window, which owns
    /// libs/ext/notebook; the editor never links it. A null runner turns
    /// the feature off exactly like a null setCodeBlockRenderer does: no
    /// cache, no lookup, no cost.
    using NotebookRunner = std::function<void(const QString &language, const QString &source,
                                              bool forceRun, const NotebookRunOptions &options,
                                              const NotebookDeliver &deliver)>;
    void setNotebookRunner(NotebookRunner runner);
    /// Whether anything can run `language` at all (a recipe whose tool is
    /// installed, or a kernel for it). A block whose language says no gets no
    /// run control, no output row and no "does not know how to run" warning:
    /// something that cannot be run has no run object. Null, the default,
    /// says yes to every language, so a fake runner in a test needs no
    /// predicate. Supplied by the window, like the runner itself.
    using NotebookRunnable = std::function<bool(const QString &language)>;
    void setNotebookRunnable(NotebookRunnable runnable);

    /// A running kernel's answer to a completion or documentation request.
    struct NotebookIntrospection {
        QStringList matches; ///< completion: the candidates, best first
        qsizetype from = 0;  ///< completion: what they replace, as offsets into the block's code
        qsizetype to = 0;
        QString text;        ///< inspect: plain-text documentation; empty when the kernel knows nothing
    };
    using NotebookIntrospectionDeliver = std::function<void(NotebookIntrospection)>;
    /// Asks the language's kernel - already running for this note, never
    /// started for this - to complete (`inspect` false) or document `code` with
    /// the caret at `cursor`. Returns false, having sent nothing, when there is
    /// no such kernel; otherwise `deliver` is called later, once, from any
    /// thread (an empty answer when the kernel gave none). Supplied by the
    /// window; null leaves Ctrl+Space and Shift+Tab to their other uses.
    using NotebookIntrospector = std::function<bool(const QString &language, const QString &code,
                                                    qsizetype cursor, bool inspect,
                                                    const NotebookRunOptions &options,
                                                    const NotebookIntrospectionDeliver &deliver)>;
    void setNotebookIntrospector(NotebookIntrospector introspector);
    /// For tests: what the inline input prompt under a running block shows.
    struct NotebookInputInfo {
        bool active = false;
        QString prompt;
        bool password = false;
        bool visible = false; ///< the line edit is on screen
        QRect editRect;       ///< viewport coordinates
    };
    NotebookInputInfo notebookInputInfo() const;
    QLineEdit *notebookInputEdit() const;
    /// For tests: the open completion popup's rows (empty when closed), and
    /// the open documentation popup's text.
    QStringList notebookCompletionRows() const;
    QString notebookInspectText() const;

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
        QRectF cellRect; ///< slides layout: the slide and the drawn Next button, viewport coordinates
        QRectF nextRect;
        qreal zoom = 1;  ///< 1 - 4, shared by every page of the embed (PDF zoom, see EDITOR.md)
        QPointF pan;     ///< the visible window's top-left, in the unzoomed cell's pixels
        int renderedWidth = 0; ///< pixel width of the first page's raster, 0 while it renders
    };
    PdfEmbedInfo pdfEmbedInfoAt(qsizetype offset) const;
    /// +1 / -1: what a click on the slide layout's next/previous control
    /// does. Remembered per block for the editor's lifetime, like a query's
    /// collapsed state - never written to the note.
    void pdfSlideStep(qsizetype offset, int direction);
    /// For tests: the clock (milliseconds) the slide controls' one-second
    /// "last button used" window reads, so a test need not sleep.
    void setPdfNavClock(std::function<qint64()> nowMs);
    /// For tests: the zoomed rasters' memory budget (bytes) and what they hold now.
    void setPdfZoomCacheBudget(qint64 bytes);
    qint64 pdfZoomCacheBytes() const;

    /// For the tests: whether the block containing `offset` rendered as a
    /// file card (icon, name, size) - the fallback for a binary file, or any
    /// file with no richer renderer.
    bool isFileCardAt(qsizetype offset) const;
    /// For the tests: whether that file card shows an EPUB's cover.
    bool fileCardHasCoverAt(qsizetype offset) const;
    /// For the tests: whether that block is an audio or video player card.
    bool isMediaCardAt(qsizetype offset) const;
    /// How many embeds the block containing `offset` rendered inline - see
    /// collectTextEmbeds' comment. 0 outside such a block.
    int embedCellCountAt(qsizetype offset) const;
    /// For the tests: where that block's `cell`-th inline embed sits,
    /// block-local.
    QRectF embedCellRectAt(qsizetype offset, int cell) const;
    /// For the tests: how many GIF movies are playing now (0 for a note with
    /// no animated GIF on screen), and the frame `path`'s is on (-1 if none).
    int GIFmovieCount() const;
    int GIFframeOf(const QString &path) const;
    /// For the tests: how many table cells the last paintTable drew.
    int tableCellsPainted() const;
    /// For the tests: the text of every cell of the table laid out in the
    /// block containing `offset`, row by row, header first - a Markdown
    /// table, a base, a CSV embed. Empty when that block is not a table (or
    /// is one still loading).
    QList<QStringList> tableCellTextsAt(qsizetype offset) const;
    /// For the tests: how many CSV files this editor holds a read of.
    int CSVcacheSize() const;
    /// For the tests: called on the worker thread at the top of every CSV
    /// read, with the path; one that throws is what an out-of-memory looks
    /// like. Null clears it.
    static void setCSVreadHookForTests(void (*hook)(const QString &));
    /// The vault's root, asked each time a ```reference block is laid out: a
    /// reference may show and open only files inside it (symlinks resolved).
    /// Without one, no local file is shown.
    void setVaultRootProvider(std::function<QString()> provider);
    /// Whether a ```reference block may fetch an `http(s)` URL. Off by
    /// default: nothing touches the network unasked (ReferenceBlock.cpp).
    void setRemoteReferences(bool on);
    /// Whether a file or URL a reference block shows is still being read, or its excerpt cut.
    bool referencesPending() const;
    /// Blocks until every reference block shows its excerpt (a file read, then the cut), or `timeoutMs` ran out:
    /// what a printed page waits for. Lays the blocks out as it goes, so each one's work has been started.
    void awaitReferences(int timeoutMs);
    /// For the tests: how many files and URLs this editor holds a read of.
    int referenceCacheSize() const;
    /// The address a hosting site's page for a file (GitHub `/blob/`, GitLab
    /// `/-/blob/`) is fetched as: the raw file's; any other link as it is.
    static QString rawReferenceURL(const QString &link);
    /// For the tests: the number of colour runs the panel at `offset` drew.
    int panelFormatCountAt(qsizetype offset) const;
    /// For the tests: what a reference block's watcher holds, how many URLs'
    /// text the process keeps, how many times a file's text was cut to a
    /// block's lines, and an address the remote-host check lets through (the
    /// local servers the tests run); empty clears it.
    int referenceWatchedForTests() const;
    static int remoteReferenceCopiesForTests();
    static int referenceScansForTests();
    static void allowReferenceAddressForTests(const QString &address);
    /// For the tests: called on the worker thread at the top of every local
    /// reference read, with the path. Null clears it.
    static void setReferenceReadHookForTests(void (*hook)(const QString &));
    /// For the tests: that cell's caption - the file name of a card, the
    /// `alt` of an `<img>` whose file is missing. Empty for an image.
    QString embedCellLabelAt(qsizetype offset, int cell) const;
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
    /// The title strip's text over a note embed's panel, empty for none.
    QString panelLabelAt(qsizetype offset) const;
    /// That image itself (null while text), for the tests.
    QImage embedImageAt(qsizetype offset) const;
    /// The same image's rect, in viewport coordinates - the same rect
    /// hookedEmbedBlockAt hit-tests wheel/drag/click against - so a test can
    /// send one there without reimplementing this layout by hand. Null
    /// outside a hooked embed with an image.
    QRect hookedEmbedImageRectAt(qsizetype offset) const;
    /// The same for the picture a renderer made for a fence (setCodeBlockRenderer),
    /// where its hotspots are; null while the fence is its listing or a placeholder.
    QRect codeBlockImageRectAt(qsizetype offset) const;

    /// A notebook block's state at `offset` (NotebookBlock.cpp) - default
    /// (`eligible` false) outside a fenced code block, when the notebooks
    /// feature or setNotebookRunner is off, or when the block has never run
    /// and nothing is cached for it. `runRect` is in viewport coordinates,
    /// the way queryHeaderAt's rects are, so a test can click it without
    /// reimplementing the header strip's layout by hand.
    struct NotebookBlockInfo {
        bool eligible = false;
        bool hasRun = false;
        bool running = false;
        bool ok = true;
        QRectF runRect;
        QRectF labelRect; ///< the language label beside it, the same coordinates
        bool stale = false;      ///< showing an earlier run of source that has since changed
        int executionCount = 0;  ///< the kernel's `In [n]`, 0 for none
        int outputCellCount = 0; ///< outputs laid out under the block
    };
    NotebookBlockInfo notebookInfoAt(qsizetype offset) const;
    /// The size of the formula or diagram drawn under the source of the block
    /// holding `offset` while the caret is in it, or an empty size when there
    /// is none. For the tests.
    QSize previewSizeAt(qsizetype offset) const;
    /// The dim line shown where that drawing would be when it cannot be made.
    QString previewHintAt(qsizetype offset) const;
    /// The text of each output cell under the block at `offset`, line breaks
    /// as typed (an image or other cell with no text gives an empty string).
    /// For the tests.
    QStringList notebookOutputTextAt(qsizetype offset) const;
    /// Whether the table holding `offset` is drawn as a table rather than
    /// as its source. For the tests.
    bool tableRenderedAt(qsizetype offset) const;
    /// What a click on that block's run control does: asks the window's
    /// runner to actually run it (forceRun). False, and nothing asked for,
    /// when notebookInfoAt(offset).eligible is false.
    bool runNotebookBlockAt(qsizetype offset);
    /// Every runnable block in the note, top to bottom; how many were asked for.
    int runAllNotebookBlocks();
    /// Every runnable block from the top of the note to `offset`, the block
    /// holding it included (Jupyter's "run all above"); how many were asked for.
    int runNotebookBlocksUpTo(qsizetype offset);
    /// Every runnable block from the one holding `offset` to the end of the
    /// note (Jupyter's "run all below"); how many were asked for.
    int runNotebookBlocksFrom(qsizetype offset);
    /// Forgets every block's output here and in the sidecar (Jupyter's
    /// "clear all outputs"); how many blocks had any.
    int clearNotebookOutputs();
    /// The language of the fenced block at `offset`, empty outside one.
    QString codeLanguageAt(qsizetype offset) const;
    /// The name its header strip shows for the fenced block at `offset`: the
    /// `#| title:`, else the style's name (the language, or `name="..."`).
    /// Empty for a block with no header strip and for one in a tab group,
    /// where the tabs are the names.
    QString codeTitleAt(qsizetype offset) const;
    /// Where the text column starts, in viewport coordinates: the x a panel
    /// sits 14 px left of. For the tests.
    int textColumn() const { return contentLeft(); }
    /// The selected tab of the tab group holding `offset`, in viewport
    /// coordinates; null outside a group, or while the caret is in it. For the tests.
    QRectF selectedTabRectAt(qsizetype offset) const;
    /// How far the fence holding `offset` is scrolled sideways, in pixels; 0
    /// when none of its lines is wider than the column. For the tests.
    qreal sideScrollAt(qsizetype offset) const;
    /// The caret's rectangle, viewport coordinates; empty when not drawn. For the tests.
    QRectF caretRectNow() const { return caretRect(); }
    /// How many parses have run on this editor, on the calling thread or the
    /// pool. For the tests.
    int parseCountForTest() const;
    /// sizeof one block view: a 1 MB note has tens of thousands of them. For the tests.
    static std::size_t blockViewSizeForTest();
    /// How many block views hold a BlockExtras (the rarely used fields), and how many
    /// of those were never laid out - always 0, an untouched view is a bare one. For the tests.
    int blockExtrasCountForTest() const;
    int blockCountForTest() const;
    int untouchedBlockExtrasCountForTest() const;
    /// How many diagrams the fence cache holds (drawn, pending or failed).
    int codeBlockCacheSizeForTest() const;
    /// Pretends the last parse took this long, which is what decides whether
    /// the next newline is parsed on the spot (applyEdit). For the tests.
    void setLastParseTimeForTest(qint64 nanoseconds);

    /// The other end of the selection; the caret's offset when nothing is selected.
    qsizetype anchorOffset() const;
    /// Other people's carets in a shared note (docs/SHARE.md): a coloured bar
    /// with their name over it. They move with every edit made here, until
    /// the next set replaces them; nothing else reads them.
    struct RemoteCaret {
        QString name;
        QColor color;
        qsizetype anchor = 0, head = 0;
    };
    void setRemoteCarets(const QList<RemoteCaret> &carets);
    QList<RemoteCaret> remoteCarets() const;

    /// Closes brackets and quotes as you type and wraps a selection instead of
    /// replacing it. Obsidian's `autoPairBrackets` and `autoPairMarkdown` as
    /// one switch, on by default the way Obsidian ships them.
    void setAutoPairs(bool on);
    bool autoPairs() const;

    // --- vim mode, VimMode.cpp --------------------------------------------
    // Optional modal editing: off by default, and off it is one bool test in
    // keyPressEvent. Insert mode passes every key through to the ordinary
    // handling below, so IME, auto-pairs and the rebindable shortcuts are
    // untouched there.

    /// Switches modal editing on or off. A fresh editor starts in normal mode.
    void setVimMode(bool on);
    bool vimMode() const;
    /// "normal", "insert", "visual" or "visual line"; empty with vim mode off.
    QString vimModeName() const;
    /// What the status bar shows: "-- INSERT --", the keys of a half-typed
    /// command, a `:` or `/` line being typed, or an error. Empty when off.
    QString vimStatus() const;

    /// Every block's painted extent in document coordinates, in source order,
    /// a callout's panel margin included; empty for a blank line, which draws
    /// nothing. Blocks laid out as the view stands, so a test scrolls through
    /// the note first. For tests that check neighbours never overlap.
    struct BlockBox {
        QRectF rect;
        BlockSpan::Kind kind;
        Range source;
    };
    QList<BlockBox> blockBoxes();

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
    /// An edit another view of the same note (or another person) made: the
    /// text follows and this view's caret keeps its place in it. Undo and
    /// redo keep working across it: each step the edit does not overlap is
    /// carried over it, so undoing here undoes only what was typed here. The
    /// steps it does overlap, and everything before them, are dropped, as
    /// are all of them when the edit replaces the whole text.
    void applyExternalEdit(qsizetype at, qsizetype removed, const QString &inserted);
    /// The one region that differs between two texts, found from both ends:
    /// replace `removed` units of `from` at `at` with `inserted` to get `to`.
    /// Never splits a surrogate pair. Empty (nothing removed or inserted) when
    /// the texts are equal.
    struct Hunk {
        qsizetype at = 0;
        qsizetype removed = 0;
        QString inserted;
    };
    static Hunk hunkBetween(const QString &from, const QString &to);
    bool runCommand(const QString &id);
    /// Keys by command id, over the built-in ones; an empty list unbinds.
    /// Undo, redo, copy, cut, paste and select all are not commands: they
    /// keep their standard keys, as they do in Obsidian.
    void setKeyBindings(const QHash<QString, QList<QKeySequence>> &bindings);

    // --- slash commands, implemented in SlashCommands.cpp ------------------
    // Obsidian's `slash-command`: "/" at the start of a line or after
    // whitespace lists commands, fuzzy filtered by what follows it. The
    // editor does not know what a command is - MainWindow hands it a source
    // (window actions and editor commands) and a way to run one by id.

    /// One row of the popup: `keys` is already formatted for display (as
    /// QuickSwitcher's command palette shows its own hotkeys), empty for an
    /// unbound command.
    struct SlashCommand {
        QString id;
        QString name;
        QString keys;
    };
    /// Called once, when the popup opens - not per keystroke.
    using SlashCommandSource = std::function<QList<SlashCommand>()>;
    /// A null `source` turns the feature off: "/" is then just a character,
    /// at no cost to a note that never opens the popup.
    void setSlashCommands(SlashCommandSource source, std::function<void(const QString &id)> run);
    /// Whether the popup is currently up - for tests; the window never asks.
    bool slashPopupVisible() const;
    /// The popup's current rows, filtered and ranked, top first - for tests.
    QStringList slashSuggestions() const;

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
    int findMatchCount() const;
    /// Zero-based index of the current match, -1 when there is none.
    int findCurrentIndex() const;
    /// False when a regex query does not compile; the field turns red.
    bool findPatternValid() const;
    void replaceCurrent(const QString &with);
    /// Returns how many matches were replaced.
    int replaceAll(const QString &with);

    // --- folding ----------------------------------------------------------
    // A `> [!type]-` callout, a `:::` container and a `#| fold: true` code
    // block all fold the same way; so does the properties table. `offset` is
    // anywhere inside the block.

    // A heading folds its section (up to the next heading of its level or
    // higher) and a list item with children folds them; `offset` is anywhere
    // on the heading's or the item's own first line.

    void toggleFoldAt(qsizetype sourceOffset);
    bool isFoldedAt(qsizetype sourceOffset) const;
    /// Opens every heading and list fold hiding `offset`, as the caret
    /// arriving there would. For a jump (find, the outline) that is not a click.
    void revealOffset(qsizetype offset);

    /// What has been folded or opened by hand, in a form that outlives the
    /// note's text: one entry per foldable block, nested callout, folded
    /// heading or folded list parent, keyed by
    /// the text of the line it starts on and how many earlier foldable lines
    /// read the same ("<0|1><occurrence> <line>", 1 = folded). setFoldState()
    /// puts it back on whatever blocks the current text still has; entries
    /// with no match are dropped. Call after setNoteText(), which clears folds.
    QStringList foldState() const;
    void setFoldState(const QStringList &state);

    /// The frontmatter properties, as the last parse read them. The properties
    /// table renders from these, and the window's future property pane will.
    const QList<Property> &properties() const;

    /// The note's headings, as the last parse read them, in source order. The
    /// outline pane builds its tree from these instead of re-parsing.
    const QList<Heading> &headings() const;

    /// One footnote, deduplicated to its first reference and numbered - a
    /// repeated `[^a]` collapses to one entry, as the Footnotes pane and
    /// reading view's list both want. Sorted by number.
    struct FootnoteEntry {
        int number = 0;
        QString label;     ///< empty for an inline `^[...]` footnote
        QString preview;   ///< the definition's (or inline text's) first line
        qsizetype jumpOffset = 0; ///< setCursorOffset() target for a click
    };
    /// The note's footnotes, as the last parse read them. Cheap enough to
    /// call from footnotesChanged() every time: a note with none costs one
    /// early-out, and even a heavily footnoted note has few of them.
    QList<FootnoteEntry> footnotes() const;
    /// For the tests: the numbers drawn for footnotes in laid-out blocks, in
    /// order, and how many rows reading view's footnote list has.
    QStringList footnoteMarkTexts() const;
    int footnotesListRowCount() const;

signals:
    /// A click on a running block's stop control: the window interrupts the
    /// kernel (or the process) running `language` in this note.
    void notebookInterruptRequested(const QString &language);
    /// vimStatus() changed: a mode switch, a pending key, a `:` line.
    void vimStatusChanged(const QString &status);
    /// `:w` (and `:wq`, `:x`): the window saves this note.
    void vimSaveRequested();
    /// `:q` (and `:wq`, `:x`): the window closes this note's tab.
    void vimCloseRequested();
    /// The word count moved. It is published with each parse, so it lags a
    /// keystroke by the parse debounce instead of costing the keystroke.
    void wordCountChanged(int words);
    /// setMode() switched between live preview, source and reading.
    void modeChanged();
    /// A parse landed, so headings() reflects the current text. Fired even
    /// when the heading list did not actually change - cheap to ignore, and
    /// the outline pane only rebuilds a QTreeWidget from it.
    void headingsChanged();
    /// A parse landed, so footnotes() reflects the current text - the same
    /// contract as headingsChanged(), for the Footnotes pane.
    void footnotesChanged();
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
    /// The context menu's "Search for ..." and "Look up ...": the window
    /// owns the vault search and the dictionary, the editor only asks.
    void searchRequested(const QString &text);
    void lookupRequested(const QString &text, const QPoint &globalPos);
    /// It left that link, or let go of it by leaving the editor.
    void linkHoverEnded();
    /// Something the user should be told that is no error: the window shows it in the status bar.
    void noticeRaised(const QString &message);
    /// A result in a rendered ```query``` block was clicked: `relativePath`
    /// is the note it matched, `offset` the first match inside it.
    void queryResultActivated(const QString &relativePath, qsizetype offset);
    /// A row in a rendered ```` ```base ```` block's table was clicked:
    /// `relativePath` is the note behind it. No offset - a base row opens a
    /// note, it does not point at a place inside it the way a search match
    /// does.
    void baseRowActivated(const QString &relativePath);
    /// A card in a hooked embed's own image was clicked, or empty space in
    /// one was double-clicked - `.canvas`'s own cards and its own file,
    /// opened the same way every other pane's noteActivated already is:
    /// MainWindow connects this straight to openRelative().
    void fileEmbedActivated(const QString &relativePath);
    /// The header of a ```reference block was clicked: `target` is the file's
    /// absolute path or the URL, for the window to open - a note in a tab,
    /// anything else with the system's own handler.
    void referenceActivated(const QString &target);

protected:
    void paintEvent(QPaintEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    // Paste and drop: a dropped file or image lands where the pointer is,
    // with the caret shown there while dragging. See PasteDrop.cpp.
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    /// Selects the word around `offset`, for a double-click or a long press.
    void selectWordAt(qsizetype offset);
    void wheelEvent(QWheelEvent *event) override;
    bool viewportEvent(QEvent *event) override; ///< a touchpad's pinch zooms a PDF page
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    friend class VimEngine;

    // Defined in MarkdownEditor_p.h: the state and the types it is made of, so
    // that adding a member does not rebuild everything that includes this header.
    struct VimState;
    struct PropertyRow;
    struct Cell;
    struct NestedCallout;
    struct EmbedCell;
    struct BlockView;
    struct BlockExtras;
    struct FoldMark;
    struct CodeLabel;
    struct TabGroup;
    struct TabRect;
    struct TableChrome;
    struct TableDrag;
    struct CellSelection;
    struct PdfMeta;
    struct PdfPageCacheEntry;
    struct TexCacheEntry;
    struct EpubCoverEntry;
    struct FileEmbedCacheEntry;
    struct CodeBlockCacheEntry;
    struct NotebookInputState;
    struct NotebookCacheEntry;
    struct QueryCacheEntry;
    struct BaseCacheEntry;
    struct CSVcacheEntry;
    struct ReferenceCacheEntry;
    struct InlineHTMLpair;
    struct HTMLtableEdit;
    struct CellRef;
    struct TableText;
    struct EmbedDrag;
    struct Snapshot;
    struct FindState;
    struct ParseJob;
    struct Private;
    std::unique_ptr<Private> d;

    bool handleVimKey(QKeyEvent *event);
    void emitVimStatus();
    /// Parses on the calling thread. Used when the whole note changes.
    void reparse();
    /// Starts a parse on the thread pool; the editor never waits for it.
    void startParse();
    /// Every block or nested callout that folds, in source order, with the key
    /// foldState() writes for it ("<occurrence> <line>").
    QList<std::pair<qsizetype, QString>> foldSites() const;
    void applyFoldState(const QStringList &state);
    void adoptParse(); ///< takes the finished pool parse, once; see finishParse()
    /// adoptDecorations() for a parse that replaces a big note's chunks: the text at the top
    /// of the viewport stays there, and the first such landing fires the hook.
    void landDecorations(quint64 version, Decorations decorations, QList<Property> properties,
                         QList<Heading> headings);
    /// Scrolls so that `offset`'s line is `y` pixels down the viewport, laying out as it goes.
    void placeLineAt(qsizetype offset, qreal y);
    void adoptDecorations(Decorations decorations, QList<Property> properties, QList<Heading> headings);
    void rebuildBlocks();
    void indexDecorations();
    void layoutBlock(BlockView &view);
    /// Lays a table out as a grid. False when this block is not a table or
    /// its structure is missing, so the caller falls back to plain text.
    bool layoutTable(BlockView &view);
    void paintTable(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The edge fades, and the bar under `window` when `withBar`, of a table
    /// or listing `extent` wide scrolled to `scroll`; nothing if it fits.
    /// `background` is what the fades dissolve into.
    void paintSidewaysOverflow(QPainter &painter, const QRectF &window, qreal extent, qreal scroll,
                               bool withBar, qreal barY, const QColor &background) const;
    /// The table, or the fence's first piece, under `viewportPoint` that is
    /// wider than the column, or null.
    BlockView *sidewaysScrollableAt(const QPoint &viewportPoint);
    /// How wide a table or a whole fence is at its natural width (0 when it
    /// fits), and how far it is scrolled, clamped to that: a fence's pieces
    /// share the offset its first piece holds.
    qreal sideExtent(const BlockView &view) const;
    qreal sideOffset(const BlockView &view) const;
    void paintCallout(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The clickable, editable title of a top-level `> [!type]` callout, laid
    /// out from the block's `left`/`top` the way paintCallout draws it; null
    /// for every other callout (nested, `:::`, a later piece) and when the
    /// title is open for editing. CalloutTitle.cpp.
    QRectF calloutTitleRect(const BlockView &view, qreal left, qreal top) const;
    bool pressCalloutTitle(const QPoint &point);
    void openCalloutTitleEditor(const CalloutSpan &callout, const QRectF &rect, const BlockView &view);
    void finishCalloutTitleEdit(bool commit);
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
    /// Writes `c` into the box of the list item on the line at `lineStart`, or
    /// gives a list item that has none a box holding it.
    void setTaskStateAtLine(qsizetype lineStart, QChar c);
    /// One checkable action per state in the table for the list item at
    /// `offset`, the one it holds now checked. False off a list item.
    bool addTaskStateActions(QMenu &menu, qsizetype offset);
    const CalloutSpan *calloutFor(const BlockView &view) const;
    // --- block renderers, implemented in BlockRenderers.cpp ---------------

    /// Lays the frontmatter out as Obsidian's properties table. False when
    /// there is nothing to show, so the caller falls back to plain text.
    bool layoutProperties(BlockView &view);
    void paintProperties(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The panel, the header strip, the line-number gutter and the highlighted
    /// rows of a fenced code block.
    void paintCodeChrome(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// editedListingPanel's fill and border, nothing else: no header strip.
    void paintEditedListingPanel(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// The first piece's share of the chrome: the `#|` header, the tab strip
    /// or just the label, over `panel`, which spans the whole fence.
    void paintCodeHeader(QPainter &painter, const BlockView &view, qreal left, qreal top,
                         const QRectF &panel) const;
    /// `#| highlight` rows and `#| line-numbers` for one piece.
    void paintCodeRows(QPainter &painter, const BlockView &view, qreal left, qreal top,
                       const QRectF &panel) const;
    QRectF bandOf(const BlockView &view, const Range &source) const;
    CodeLabel codeLabelOf(const BlockView &view, qreal left, qreal top) const;
    const CodeFenceSpan *codeFenceOf(const BlockView &view) const;
    LanguageStyle codeStyleOf(const BlockView &view) const;
    LanguageStyle styleOfFence(const CodeFenceSpan &fence) const;
    /// The same for a listing nested in a callout or quote, which has the
    /// language and nothing else to go on.
    LanguageStyle styleOfLanguage(const QString &language) const;
    /// A named highlight's row colour, with the alpha a highlighted row gets;
    /// invalid for a name the setting does not have.
    QColor codeHighlightColour(const QString &kind) const;
    /// The highlighter's tokens for an inline code span written `{lang} code`,
    /// in `map`'s display coordinates.
    QList<QTextLayout::FormatRange> inlineCodeFormats(const StyleSpan &span, const DisplayMap &map) const;
    /// Token colours for a block styled `style`: its own `syntax.*` over the
    /// theme's. False (and `*out` untouched) when nothing is configured or the
    /// setting is off, which is when the highlighter must get no map at all.
    bool syntaxMapFor(const LanguageStyle &style, SyntaxMap *out) const;
    /// NotebookBlock.cpp: a notebook-eligible block's run control, in the
    /// header strip alongside codeLabelOf's own label - recomputed from
    /// `left`/`top` on every call, the same as codeLabelOf, rather than
    /// cached in the view, so it never goes stale across a scroll. Null-sized
    /// when the block is not notebook-eligible or has no header row.
    QRectF notebookRunRectFor(const BlockView &view, qreal left, qreal top) const;
    void paintNotebookRunControl(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    void paintNotebookOutput(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// A small "host" marker left of the run control, on a fenced block that
    /// opted into `#| sandbox: host` - the reader's only sign that this
    /// block runs unsandboxed - or "wasm" for `#| sandbox: wasm`. Nothing on
    /// any other block.
    void paintNotebookHostBadge(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
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
    void hideFoldedListItems(const BlockView &view, QList<Range> &hidden) const;
    /// True for the kinds that show all their source when the caret is in
    /// them - tables, code, maths, callouts - as Obsidian does with its widgets.
    /// Paragraphs, headings, lists and plain quotes reveal per element instead.
    bool revealsWholeBlock(const BlockView &view) const;
    QList<Range> revealedSyntax(const BlockView &view) const;
    /// The top-level fenced block the caret is in or on, in live preview.
    const CodeFenceSpan *fenceAroundCaret() const;
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
    /// A task's box: the state's glyph in its colour. `[ ]` and `[x]` are
    /// painted by paintCheckBox, so they look as they always did.
    void paintTaskBox(QPainter &painter, const QRectF &rect, const TaskState &state,
                      const QFont &font) const;
    /// The disclosure triangle every foldable block carries.
    void paintFoldArrow(QPainter &painter, const QRectF &rect, bool folded,
                        const QColor &colour) const;
    /// `#|` options of this block, or nullptr. Matched on the block's start:
    /// `rebuildBlocks` takes the newline after a block into its span, so the
    /// ends no longer compare equal.
    const CodeOptions *codeOptionsFor(const BlockView &view) const;
    qreal codeHeaderHeight() const;
    /// Source mode draws pure text, and so does a tab group while the caret is
    /// in it: markdown syntax coloured, listings highlighted, nothing else -
    /// no panel, header, run control, callout or fold.
    bool pureSource(const BlockView &view) const;
    /// pureSource, plus a single listing the caret has opened: its text is
    /// plain and it has no header strip or copy label, but unlike pureSource
    /// it keeps a panel, a run control and its output.
    bool plainSource(const BlockView &view) const;
    /// The panel behind a listing being edited: the listing plus the opening
    /// and closing fence lines the neighbouring paragraphs show, so no text is
    /// drawn over the panel's edge. `top` is this block's, in the viewport.
    /// `fenceLine` receives the opening fence line's height, 0 when it shows none.
    QRectF editedListingPanel(const BlockView &view, qreal left, qreal top, qreal *fenceLine = nullptr) const;
    /// Whether this fenced block is laid out under a header strip: `#|`
    /// options, a tab group, a run control, or just a language to name.
    bool hasCodeHeader(const BlockView &view) const;
    /// What the strip says on its left, see codeTitleAt.
    QString codeTitleOf(const BlockView &view) const;
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
    /// Heading and list folds, which hide other blocks or lines rather than
    /// collapsing their own: the item whose first line holds `offset`, null
    /// when none or when it has no children; and whether it is folded.
    const ListItemSpan *foldableItemAt(qsizetype offset) const;
    bool headingFolds(const BlockView &view) const;
    /// foldState(), with the offset-keyed folds dropped; for applyFoldState()
    /// after the text was replaced wholesale (undo, redo, an external rewrite).
    QStringList takeFolds();
    /// Item `i` of a block's items has children and is folded.
    bool foldedItem(std::span<const ListItemSpan> items, size_t i) const;
    /// What a folded item hides: from its second line to the end of its children.
    Range listFoldRange(const ListItemSpan &item) const;
    /// Clears the folds that hide `offset`; false when none did. No layout.
    bool openFoldsAt(qsizetype offset);
    /// The heading and list-item folds around `offset`, innermost first, each
    /// as the offset toggleFoldAt() takes: the list items with children that
    /// hold it, then the heading it is on or sits under, then the ones above.
    QList<qsizetype> enclosingFolds(qsizetype offset) const;
    /// Runs an `editor:*fold*` command; false when `id` is not one.
    bool foldCommand(const QString &id);
    /// A fold arrow (and, folded, its ellipsis) for each site in the block.
    QList<FoldMark> foldMarksOf(const BlockView &view, qreal left, qreal top) const;
    bool handleFoldClick(const BlockView &view, const QPoint &point, qreal left, qreal top);
    void paintFoldMarks(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// What the source itself says: `[!note]-` and `::: details` start folded.
    bool defaultFoldOf(const BlockView &view) const;
    void setFolded(const BlockView &view, bool folded);
    /// True when the press landed on a fold handle or a property control, so
    /// the caret must not move.
    bool handleBlockClick(const QPoint &point);
    // --- the `</>` edit button, HoverEdit.cpp ---------------------------
    /// The first piece of the block whose panel holds the viewport point, or
    /// -1; one lower_bound on the sorted block list, nothing scanned.
    qsizetype blockAtPoint(const QPoint &point) const;
    /// True when the block is drawn as a widget the source of which is not
    /// on screen: a callout, formula, HTML, plugin, query/base or embed
    /// block, or a table's grid.
    bool hasEditButton(const BlockView &view) const;
    /// The top-level callout whose source holds `offset`, or null.
    const CalloutSpan *topCalloutAt(qsizetype offset) const;
    /// Edit / Copy / Change type / Remove, for a right-click on a callout.
    void addCalloutActions(QMenu &menu, qsizetype blockIndex);
    QRectF panelRectOf(qsizetype firstPiece) const;
    /// The block's whole source, every piece, without the newline that ends it.
    Range wholeBlockRange(const BlockView &firstPiece) const;
    QRectF editButtonRectOf(qsizetype firstPiece) const;
    /// The block under a press on its button, else -1.
    qsizetype editButtonAt(const QPoint &point) const;
    /// Tracks the pointer for the code label and the edit button; repaints
    /// only the panels that changed. `tables` false: leave a table's grips
    /// alone (a finger's).
    void updateHover(const QPoint &point, bool tables = true);
    void clearHover();
    void updateFoldHover(const QPoint &point, qsizetype blockFound);
    void paintEditHover(QPainter &painter) const;
    const TableSpan *chromeTable(const BlockView &view) const;
    TableChrome tableChromeAt(const QPoint &point) const;
    QRectF rowGripRect(const BlockView &view, const TableSpan &table, int row, bool hit) const;
    QRectF columnGripRect(const BlockView &view, const TableSpan &table, int column, bool hit) const;
    /// A cell's rect in viewport coordinates.
    QRectF cellViewRect(const BlockView &view, const Cell &cell) const;
    int cellRowAt(const BlockView &view, const TableSpan &table, qreal y) const;
    int cellColumnAt(const BlockView &view, const TableSpan &table, qreal x) const;
    QRectF addRowRect(const BlockView &view) const;
    QRectF addColumnRect(const BlockView &view) const;
    QRectF tableViewRect(const BlockView &view) const;
    /// What a repaint of the chrome's parts in `mask` (kRowGrip, kColumnGrip,
    /// kBars) covers: a few small rects, never the table.
    QRegion chromeDirty(const TableChrome &chrome, int mask) const;
    void updateTableHover(const QPoint &point);
    void clearTableHover();
    void paintTableChrome(QPainter &painter) const;
    /// `touch`: a finger. It has no hover, so a first tap in a table only brings
    /// the grips and bars up; the next press on one of them is theirs.
    bool pressTableChrome(const QPoint &point, bool touch = false);
    bool dragTableChrome(const QPoint &point);
    bool releaseTableChrome(const QPoint &point);
    /// Moves body row or column `from` so it sits before `to` (a boundary,
    /// counted before the move), one undo step. False when it would not move.
    bool moveTableLine(qsizetype table, bool column, int from, int to);
    /// Appends `count` empty rows or columns, caret in the first of them, one
    /// undo step.
    bool appendTableLine(qsizetype table, bool column, int count = 1);
    /// An HTML table's counterparts: `from` moves to `final` (the position it
    /// ends at) by permuting the elements' text between their own slots, so
    /// attributes and the author's spacing between rows stay.
    bool moveHTMLline(qsizetype table, bool column, int from, int final);
    bool appendHTMLline(qsizetype table, bool column, int count);
    /// How many rows or columns an add bar dragged to `point` would add, and
    /// the strip they would take up (painted while dragging).
    int addBarCount(const BlockView &view, const TableSpan &table, const QPoint &point) const;
    QRectF addBarGhost(const BlockView &view, const TableSpan &table, int count) const;
    /// While a grip, an add bar or a cell selection is dragged past the edge
    /// of the viewport (or of a wide table's window), scrolls by a speed that
    /// grows with the distance past it, on a timer; `point` is the pointer.
    void updateTableAutoScroll(const QPoint &point);
    /// How far a tick scrolls (x: the wide table's own window, y: the page)
    /// for a pointer at `point`, and in which block; null when it is inside.
    QPoint tableScrollDelta(const QPoint &point, qsizetype &block) const;
    void stopTableAutoScroll();
    void tableAutoScrollTick();
    const CellSelection *cellSelection() const;
    void setCellSelection(qsizetype table, int anchorRow, int anchorColumn, int headRow, int headColumn);
    /// The cell a drag at `point` reaches in the table the press was in.
    bool dragCellSelection(const QPoint &point);
    bool handleCellSelectionKey(QKeyEvent *event);
    bool copyCellSelection(bool cut);
    bool clearCellSelection();
    bool pasteIntoCells();
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
    /// Tab and Shift+Tab are the editor's (list indent, Jupyter documentation),
    /// not a move of focus to the next widget: QWidget::event hands them to
    /// keyPressEvent only when nothing else can take the focus.
    bool focusNextPrevChild(bool next) override;
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
    /// `attachment:<scope><name>` when setInlineImages holds that picture for
    /// the embed (the "path" such an embed has), else empty.
    QString attachmentPathOf(const EmbedSpan &embed) const;
    /// The picture at `path`: a file, or an `attachment:` one from the table. Null when neither decodes.
    QPixmap loadEmbedPixmap(const QString &path) const;
    bool layoutSourceFile(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// NotebookEmbed.cpp: a runnable `![[script.py]]` embed (stage 13's
    /// rest) - called once layoutSourceFile has already built the panel's
    /// listing, `body` is its unsliced-or-not source exactly as shown. A
    /// no-op, and no run control, for an extension no installed kernel
    /// claims, or when the notebooks feature or its runner is off.
    void layoutRunnableEmbed(BlockView &view, const QString &path, const QString &body);
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
    /// What a point (block-local, below the header) means to a slides embed:
    /// -1 / +1 the button it steps, 0 swallowed with no step (the dead band
    /// along the slide's bottom edge), nothing when the embed does not claim it.
    std::optional<int> pdfNavHit(const BlockView &view, const QPointF &local) const;
    bool pdfNavCanStep(const BlockView &view, int direction) const;
    /// A hand where a click would step, an arrow where it is swallowed,
    /// nothing where the embed leaves the pointer to the usual rules.
    std::optional<Qt::CursorShape> pdfNavCursorAt(const QPoint &point) const;
    /// PDF zoom (PdfEmbedBlock.cpp). The page cell under `point` (its index,
    /// else -1), with the block's index and `point` in block-local space.
    int pdfCellAt(const QPoint &point, qsizetype *block, QPointF *blockLocal) const;
    /// Multiplies the zoom of the page under `point` by `factor`, keeping
    /// the content under the pointer still; false when no page is there.
    bool zoomPdfAt(const QPoint &point, qreal factor);
    /// Zoom and pan inputs: each returns whether it took the event.
    bool wheelPdfCell(QWheelEvent *event);
    bool pressPdfCell(const QPoint &point, bool touch);
    bool doubleClickPdfCell(const QPoint &point);
    void panPdfEmbed(qsizetype block, const QPointF &delta);
    bool resetPdfZoom();
    /// The settle timer's work: the zoomed rasters the visible pages want.
    void requestPdfZoomRenders();
    /// Ends a block's layout so the next updateLayout() builds it again.
    void invalidatePdfBlock(qsizetype block);
    /// Cached page count and per-page size, keyed by path and mtime - a
    /// metadata read, not a render, so this runs on the UI thread; PdfPage
    /// itself still opens the file at most once per (path, mtime) per
    /// thread. `count` and `sizes` are left untouched when the file cannot
    /// be opened as a PDF.
    void pdfMetadata(const QString &path, qint64 mtime, int &count, QList<QSizeF> &sizes) const;
    /// The cached page image for (path, mtime, page, width), or a null
    /// QPixmap while a render is in flight - which this also starts, off the
    /// UI thread, if one is not in flight already.
    QPixmap ensurePdfPageAsync(const QString &path, qint64 mtime, int page, int width);
    /// Called back (marshalled onto this thread) when a render started by
    /// ensurePdfPageAsync finishes. Relays out every block currently showing
    /// `path`, since column and grid can have several cells waiting on it.
    void deliverPdfPage(const QString &key, quint64 generation, const QString &path, QImage image);
    const TexCacheEntry &ensureTexCompiled(const QString &path, qint64 mtime);
    void deliverTexCompiled(const QString &key, quint64 generation, TexCompiler::Result result);
    // --- file cards, note embeds and multi-embed rows, FileEmbedBlock.cpp --

    /// A file none of the above render: a small card (icon, name, size) that
    /// opens it on click. Also where layoutSourceFile redirects when the
    /// bytes it read back are not text - never shown as a wall of garbage.
    bool layoutFileCard(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// An EPUB card's cover, read off the UI thread (a zip walk and an image
    /// decode) and kept per "path|mtime|size"; a null pixmap until it lands,
    /// when the cards showing that file are laid out again.
    QPixmap ensureEpubCoverAsync(const QString &path, qint64 mtime, qint64 size);
    void deliverEpubCover(const QString &key, const QString &path, QImage cover);
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
    /// cannot be drawn without waiting. `fromNote` is the file the slice
    /// came from, joined to this editor's own ancestors for the nested one.
    QImage renderNoteEmbed(const QString &slice, int width, const QString &fromNote = QString()) const;
    static constexpr int kMaxEmbedDepth = 5;
    /// What an embed drawn into an image took from the theme, so a theme
    /// change misses its cache instead of showing the old colours.
    QString embedThemeTag() const;
    void deliverFileEmbed(const QString &key, quint64 generation, QImage image);
    /// The same "path|mtime|subpath|width|theme" key layoutHookedFileEmbed's
    /// cache uses, factored out so an interactive update
    /// (applyFileEmbedInput) can find and overwrite the very same entry -
    /// without that, an edit elsewhere in the note would relay the block out
    /// from the stale, pre-interaction image the cache still had.
    QString fileEmbedCacheKey(const QString &path, const QString &subpath, int width) const;
    /// A hooked embed's own image, in viewport coordinates - hookedEmbedImageRectAt's
    /// own work, shared with hookedEmbedBlockAt and embedLocalPoint so the
    /// hit-test, the local-point conversion and the public accessor all
    /// agree on the very same rect.
    QRectF hookedEmbedImageRect(const BlockView &view) const;
    /// The block under `viewportPoint` if it is a hooked embed with an input
    /// handler registered for its suffix, else -1 - including a hooked embed
    /// with no handler (a `.base` block, say), which is untouched by any of
    /// this and behaves exactly as it does today.
    qsizetype hookedEmbedBlockAt(const QPoint &viewportPoint) const;
    /// `viewportPoint` translated into that block's own local space - the
    /// same origin FileEmbedInputEvent::localPoint is measured from.
    QPoint embedLocalPoint(qsizetype blockIndex, const QPoint &viewportPoint) const;
    /// Runs `event` through the block's input handler; updates its image and
    /// cache entry in place (no relayout - the size never changes) and/or
    /// emits fileEmbedActivated() for whatever it answered. The return value
    /// is only wheelEvent's business: declined, a plain wheel keeps
    /// scrolling the note instead.
    bool applyFileEmbedInput(qsizetype blockIndex, const FileEmbedInputEvent &event);
    /// CodeBlockHook.cpp: a fence in a language setCodeBlockRenderer has.
    bool layoutPluginCode(BlockView &view);
    /// The cache entry for a fence's drawing, asked of its renderer on the
    /// first call (and never twice); null when no renderer has the language.
    /// `key` is what deliverCodeBlock finds its blocks by.
    /// What a fence's drawing is cached under, and found again by.
    QString codeBlockKeyFor(const QString &language, const QString &source, int width) const;
    /// Drops what the editor no longer shows: the preview's earlier drawing
    /// once a newer one is asked for, and, past a cap, anything no block points at.
    void pruneCodeBlockCache(const QString &keep);
    const CodeBlockCacheEntry *requestCodeBlock(const QString &language, const QString &source,
                                                int width, QString *key);
    /// PreviewBelow.cpp: the formula or diagram of the block the caret is in,
    /// kept visible under its source, as Obsidian does.
    void appendPreview(BlockView &view);
    void paintPreview(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    void deliverCodeBlock(const QString &key, quint64 generation, CodeBlockResult result);
    /// A press on a hotspot of a plugin block's picture (CodeBlockResult::
    /// hotspots): asks the renderer for the picture it leads to. False when
    /// the press is not on one.
    bool pressCodeBlockHotspot(const QPoint &viewportPoint);
    /// NotebookBlock.cpp: a fenced code block's output, shown below its own
    /// listing rather than in place of it - the one difference from
    /// layoutPluginCode's shape just above, which this otherwise mirrors.
    /// `workingDir`/`sandboxHost` are a runnable embed's folder and a
    /// fenced block's own `#| sandbox:` - see NotebookRunOptions.
    void appendNotebookOutput(BlockView &view, const QString &language, const QString &source,
                              const QString &blockID, const QString &workingDir = QString(),
                              const CodeOptions *options = nullptr, int duplicateOrdinal = 0);
    /// How many fences before `fence` read the same (same language, same code):
    /// NotebookRunOptions::duplicateOrdinal. Counted over the fences that are cells of a notebook (a fence
    /// that opens at the start of a line: not one quoted or listed inside a markdown cell), in order.
    int notebookDuplicateOrdinal(const CodeFenceSpan &fence) const;
    /// Lays out every code block (and runnable embed) that is not yet laid out, so what a command for all of them
    /// finds is decided for each (notebookEligible, the key, the options) and not just for the ones near the
    /// view. The view stays where it is.
    void layOutNotebookBlocks();
    /// The options a run of what `view` last laid out is made with.
    NotebookRunOptions notebookOptionsOf(const BlockView &view) const;
    void deliverNotebookResult(const QString &key, quint64 generation, NotebookRunResult result);
    /// A Text or Error output, as its own small monospace QTextLayout -
    /// null if `text` ends up empty (an error with no message at all).
    std::unique_ptr<QTextLayout> buildNotebookTextLayout(const QString &text, qreal width,
                                                         const QColor &colour) const;
    /// An Image/Markdown/Svg output rendered once to a QPixmap at `width` -
    /// null on a decode failure, which appendNotebookOutput then just skips.
    QPixmap renderNotebookImage(const NotebookOutput &output, int width) const;
    /// The output rows proper, below which appendNotebookOutput adds the
    /// prompt row while the block's code is waiting in input().
    void appendNotebookOutputRows(BlockView &view, const QString &language, const QString &source,
                                  const QString &blockID, const QString &workingDir,
                                  const CodeOptions *options, int duplicateOrdinal);
    /// The "Running...", or a kernel-level error, single-line message a
    /// block shows below its listing when there is nothing richer yet.
    void appendNotebookMessage(BlockView &view, const QString &text, const QString &colourToken);
    void showNotebookInput(const QString &key, quint64 generation, const NotebookInputPrompt &prompt,
                           const NotebookInputReply &reply);
    /// Answers (ok) or cancels the open prompt and takes the line edit away.
    void finishNotebookInput(bool ok);
    /// Drops the prompt without answering: the run it belonged to is over.
    void dropNotebookInput(const QString &key);
    void appendNotebookInput(BlockView &view);
    void positionNotebookInput();
    /// Ctrl+Space (completion) or Shift+Tab (documentation) with the caret in a
    /// code block that has a running kernel. False when it is not one of those,
    /// so the key goes on to do what it does everywhere else.
    bool handleNotebookIntrospectKey(QKeyEvent *event);
    bool requestNotebookIntrospection(bool inspect, bool tellWhenNone);
    void showNotebookCompletion(const QStringList &matches, qsizetype from, qsizetype to);
    void showNotebookInspect(const QString &text);
    void acceptNotebookCompletion(const QString &match);
    void hideNotebookIntrospection();
    /// Inserts a pending entry and asks m_notebookRunner for it
    /// (forceRun=false: cache-only, never starts a kernel) the first time
    /// `key` is seen; every later call just returns what is already there,
    /// the same one-search-per-text idea ensureQuerySearch keeps for a
    /// ```query``` block.
    const NotebookCacheEntry &ensureNotebookResult(const QString &key, const QString &language,
                                                   const QString &source, const NotebookRunOptions &options);
    /// `![[Note]]`, `![[Note#Heading]]` or `![[Note#^block-id]]`: the target
    /// note's own text (or the named section of it), in a panel like a
    /// source file embed's. A plain-text slice, not a recursive re-render of
    /// the target's own markdown - ponytail: no nested live-preview styling,
    /// upgrade to one if a transcluded note's own formatting turns out to
    /// matter enough to pay for a second parser instance per embed.
    bool layoutNoteEmbed(BlockView &view, const EmbedSpan &embed, const QString &path);
    /// What a note embed shows, for a sole embed and an inline one alike:
    /// false when the file or the `#Heading` / `#^block` is not there;
    /// otherwise `slice` is the text, `label` its title and `image` the
    /// formatted drawing - null while it is still being drawn (`key`, its
    /// cache entry, then names what deliverFileEmbed will fill) or when it
    /// stays text (`key` empty).
    bool noteEmbedContent(const QString &path, const QString &subpath, QString &label,
                          QString &slice, QString &key, QImage &image);
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
    /// What this actually builds: `view.embedCells`, each one inline where
    /// its source sits. QTextLayout, used bare, has no inline objects, so the
    /// embed's "!" stays in the text as a carrier (the rest is hidden): no
    /// ink, and letter spacing that reserves exactly the cell's width - the
    /// trick inline maths uses. A line holding a cell taller than the text
    /// grows by the difference (textEmbedHeightOn, in layoutBlock's line
    /// loop), and placeTextEmbeds puts each cell on its carrier once the
    /// layout is done. Several embeds on one line sit side by side at their
    /// own size and wrap like words, as in Obsidian.
    ///
    /// Each cell reuses layoutImage's and layoutPdf's own caches (a thumbnail
    /// is not rendered twice just because the same file is both a sole embed
    /// elsewhere and one of several here); anything that is not an image or
    /// a PDF page becomes a file card, including audio, video and notes -
    /// their own richer treatment is what a *sole* embed still gets.
    void collectTextEmbeds(const BlockView &view, QList<Range> &hidden,
                           QList<const EmbedSpan *> &found) const;
    void prepareTextEmbeds(BlockView &view, const QList<const EmbedSpan *> &embeds,
                           QList<QTextLayout::FormatRange> &formats);
    qreal textEmbedHeightOn(const BlockView &view, const QTextLine &line) const;
    void placeTextEmbeds(BlockView &view, qreal lineHeight);
    EmbedCell buildEmbedCell(const EmbedSpan &embed, qreal maxWidth);
    /// The `cell`-th inline embed of `view` - its own, then every table
    /// cell's - and where its rect is measured from. Null past the end.
    const EmbedCell *embedCellOf(const BlockView &view, int cell, QPointF &origin) const;
    void paintEmbedCellList(QPainter &painter, const std::vector<EmbedCell> &cells, qreal left,
                            qreal top) const;
    /// Animated GIFs (GIFplayer.cpp). A GIF embed that is drawn, in the
    /// viewport, with a movie slot free, is played by a QMovie; anything else
    /// - off screen, past the cap, one frame, the editor hidden - is its first
    /// frame, exactly as before. `GIFframe` is what the painters draw in place
    /// of `still`; the rest ends movies whose embed left the viewport.
    QPixmap GIFframe(const QPainter &painter, const QString &path, const QRectF &target,
                     const QPixmap &still) const;
    QList<QRect> GIFrectsInView(const QString &path) const;
    void GIFframeChanged(const QString &path);
    void GIFstopped(const QString &path, bool finished);
    void pruneGIFs();
    void clearGIFs();
    void paintEmbedCells(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    bool handleEmbedCellClick(const BlockView &view, const QPoint &point, qreal left,
                              qreal top);
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
    // --- footnotes, implemented in FootnoteMarks.cpp ------------------------
    // References and definition labels: hidden and replaced by a painted
    // number the same way inline maths and anchor separators are, above.
    // Reading view's trailing list is a synthetic block instead (see
    // BlockView::isFootnotesList) since it is not source at any real offset.

    /// The font a footnote number is measured and drawn in: smaller than the
    /// block's own, superscript for a reference raised further still. Shared
    /// by the format that reserves the carrier's width and the paint step
    /// that draws into it, so the two never disagree.
    QFont footnoteMarkFont(const QFont &blockFont, bool superscript) const;
    /// Records this block's footnote marks and, for a reference or a
    /// numbered definition the caret has not revealed, hides everything
    /// after the carrier character. Must run before `view.map` is built.
    void layoutFootnotes(BlockView &view, QList<Range> &hidden);
    /// Draws the numbers `layoutFootnotes` recorded, over their carriers.
    void paintFootnoteMarks(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// True while `view.span.range` sits inside a numbered footnote
    /// definition: hidden outright in reading view, which lists definitions
    /// at the end instead of where they are in the source.
    bool footnoteDefinitionBlock(const BlockView &view) const;
    /// Builds the reading view's trailing footnotes-list block: not real
    /// source, so - like the properties table - it keeps its own rows
    /// instead of a QTextLayout built from view.map.
    void layoutFootnotesList(BlockView &view);
    /// Draws layoutFootnotesList's rows: the rule above them, each numbered
    /// definition and its back-reference glyph.
    void paintFootnotesList(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// Ctrl+click handling for a footnote: `viewportPoint` on a reference
    /// jumps to its definition, on a definition's label (or the trailing
    /// list's back-reference) jumps to the first reference. False, and no
    /// jump, when the click was on neither.
    bool jumpToFootnote(const QPoint &viewportPoint);
    // --- query blocks, implemented in QueryBlock.cpp -----------------------

    /// A fenced ```` ```query ```` block: renders its search results (a note
    /// list with match counts and highlighted snippets) instead of the query
    /// text, the way live preview renders any other block - false for every
    /// other fence, which falls through to the ordinary code listing.
    bool layoutQuery(BlockView &view);
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
    /// embedded .base file alike. `viewName` is the view an embed's
    /// `#fragment` names (Decorations' EmbedSpan::subpath, "#" stripped);
    /// empty for a fence, which has no fragment to name one.
    bool layoutBaseTable(BlockView &view, const QString &yamlText, const QString &viewName = QString());
    /// A one-line message in place of a table: loading, empty, an error.
    bool layoutPlainMessage(BlockView &view, const QString &text, const QColor &colour);
    /// `data` as a grid laid out like a Markdown table: at most `maxRows` data
    /// rows after the header; a non-empty `moreText` is the footer line under
    /// a table cut short. A base's table and a CSV embed's both come here.
    bool layoutDataTable(BlockView &view, const BaseTableData &data, int maxRows, const QString &moreText);
    // --- CSV and TSV embeds, implemented in CSVembed.cpp ---------------------

    /// `![[data.csv]]` / `![[data.tsv]]`: the file as a table, through
    /// layoutDataTable. Read and parsed on a worker thread, "Loading..." until
    /// deliverCSV lands; false for a file that is not there, which falls back
    /// to what layoutSourceFile does for one (the link as text).
    bool layoutCSVembed(BlockView &view, const QString &path);
    /// Called back (marshalled onto this thread) when a read started by
    /// layoutCSVembed finishes; ignored if `generation` is no longer the one
    /// on record for `path` (the file changed again meanwhile).
    void deliverCSV(const QString &path, quint64 generation, CSVcacheEntry result);
    /// The worker's half: read `path` and parse it. Static - it runs off the
    /// UI thread and must not touch the editor.
    static CSVcacheEntry readCSVfile(const QString &path, bool tsv, const QString &unreadable,
                                     const QString &moreColumns);
    /// A change event from the watcher: starts the debounce (200 ms from the
    /// last event, but never more than a second from the first).
    void CSVfileEvent();
    /// Relays out the blocks showing `path`, so layoutCSVembed looks at it again.
    void relayCSVblocks(const QString &path);
    /// Watches `path` and its folder (made on the first call), so a change on
    /// disk reaches refreshCSVfiles after a short debounce.
    void watchCSV(const QString &path);
    /// Stats every cached file, relaying out the blocks of those that changed.
    void refreshCSVfiles();
    /// Forgets every cached file and its watch: the feature went off.
    void dropCSVcache();
    // --- ```reference blocks, implemented in ReferenceBlock.cpp --------------

    /// A fenced block Code Styler's `reference` names a file or URL for: that
    /// file's lines (`start`/`end`) as a listing under the file's name. The
    /// read is on a worker thread - or the network - never in layout: "Loading..."
    /// until deliverReference lands. False for any other block.
    bool layoutReference(BlockView &view);
    /// A read or fetch finished (on this thread); ignored if `generation` is
    /// no longer the one on record for `key`.
    void deliverReference(const QString &key, quint64 generation, ReferenceCacheEntry result);
    /// Asks for `url` through the network manager, never blocking.
    void fetchReference(const QString &key, const QString &url, quint64 generation, int hops);
    /// Relays out the blocks showing `key`.
    void relayReferenceBlocks(const QString &key);
    void deliverExcerpt(const QString &key, quint64 generation, const QString &memoKey, const QString &language,
                        ReferenceExcerpt made);
    void watchReference(const QString &path);
    void referenceFileEvent();
    void refreshReferences();
    /// Forgets every URL's text and the network manager: remote fetching went off.
    void dropRemoteReferences();
    /// Forgets everything a reference block kept - reads, watches, fetches: the plugin went off.
    void dropReferenceCache();
    /// The view's name (if any) above the grid, then paintTable for the
    /// grid itself - the table painting is not duplicated, only framed.
    void paintBase(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    const BaseCacheEntry &ensureBaseSearch(const QString &yamlText, const QString &viewName = QString());
    /// Called back (marshalled onto this thread) when an evaluation started
    /// by ensureBaseSearch finishes. Ignored if `generation` is no longer
    /// the one on record for `yamlText`/`viewName`. Mirrors
    /// deliverQueryResults.
    void deliverBaseResults(const QString &yamlText, const QString &viewName, quint64 generation,
                            BaseTableData results);
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
    QList<InlineHTMLpair> inlineHTMLpairs(const BlockView &view) const;
    /// The QTextCharFormat for a recognised inline tag, or nullopt for one
    /// this pass leaves as raw source (unknown tags, and anything in a
    /// `style=` attribute besides `color:`). `openTag` is the opening tag's
    /// own source text, for `<span style="...">`'s colour.
    std::optional<QTextCharFormat> htmlTagFormat(const QString &name, const QString &openTag) const;
    // --- HTML edited where it is drawn, HtmlEdit.cpp -------------------------
    /// The substitution (an entity, `<br>`) live preview draws over
    /// `offset`'s character, or null where the source shows as it is.
    const Substitution *shownSubstitution(qsizetype offset) const;
    /// An entity (`&amp;`) the caret touches opens into its source, as
    /// the markers of other inline syntax do; with a selection, none does.
    bool entityRevealed(const Substitution &substitution) const;
    /// The ranges of the entities entityRevealed says are open.
    QList<Range> revealedEntities() const;
    /// The substitutions to draw while `revealed` entities show as written.
    QList<Substitution> substitutionsWithout(const QList<Range> &revealed) const;
    /// `target` moved out of a drawn substitution, the way it was going: the
    /// caret steps over `&amp;` as one character.
    qsizetype stepOverSubstitution(qsizetype target, int direction) const;
    /// The innermost HTML element whose text holds `offset`, in live preview
    /// and outside a tag; null anywhere else.
    const HtmlTextSpan *HTMLtextAt(qsizetype offset) const;
    /// `text` as source for that element: `& < >` escaped, a line break a
    /// `<br>` (a space in a cell, kept in a `<pre>`).
    QString HTMLsafe(const QString &text, const HtmlTextSpan &span) const;
    /// Enter in HTML text: a new `<p>` or `<li>`, a `<br>` elsewhere.
    bool handleHTMLenter();
    /// An empty row for an HTML table, written like its row `like`.
    QString HTMLrowText(const TableSpan &table, int like, bool header) const;
    /// The whitespace before `offset` on its line, or nullopt if there is text.
    std::optional<QString> indentBefore(qsizetype offset) const;
    /// Applies `edits` as one undo step and puts the caret in a cell.
    bool applyHTMLtableEdits(QList<HTMLtableEdit> edits, qsizetype table, int row, int column);
    /// pasteIntoCells() for a simple HTML table: `cells` from `rect`'s top-left,
    /// the table grown to fit, as one undo step.
    bool pasteIntoHTMLcells(qsizetype index, const QRect &rect, const QList<QStringList> &cells, bool fill, int rowsWide,
                            int across);
    /// The HTML counterparts of TableCells.cpp's row and column edits.
    bool insertHTMLrow(qsizetype table, int row, bool after, int column);
    bool fillShortHTMLrow(qsizetype table, int row);
    bool runHTMLtableCommand(const QString &id, qsizetype table, int row, int column);
    /// True when the newline at `source` joins two lines of one paragraph
    /// rather than breaking them apart.
    bool softBreakAt(qsizetype source) const;
    /// Extra space a callout reserves for its title row.
    qreal calloutHeaderHeight(const CalloutStyle &style) const;
    QList<QTextLayout::FormatRange> formatsIn(const BlockView &view, const Range &source,
                                              const DisplayMap &map) const;
    const TableSpan *tableFor(const BlockView &view) const;
    CellRef tableCellAt(qsizetype source) const;
    /// Where a caret bound for `offset` goes when that is inside a rendered
    /// table but in no cell - a pipe, a padding space, the delimiter row: the
    /// nearest cell the way it was moving from `from`, or out of the table.
    qsizetype snapToTableCell(qsizetype offset, qsizetype from) const;
    /// Tab, Shift+Tab, Enter, Home, End, and Backspace or Delete at a cell's
    /// edge, inside a rendered table. False when the caret is in none.
    bool handleTableKey(QKeyEvent *event);
    /// Tab, Shift+Tab and Enter inside a table whose pipes are showing (source
    /// mode): the table is aligned and the caret goes to the next cell, the
    /// previous one, or the cell below, a row added past the last, all as one
    /// undo step. `key` is Qt::Key_Tab, Key_Backtab or Key_Return. False off a
    /// table, with `editor.alignTables` off, or with a selection.
    bool handleSourceTableKey(int key);
    /// Enter at the end of a header row typed in plain text (`| a | b |`, the
    /// line above blank and the one below too): writes the delimiter row and
    /// an empty body row, the table aligned, caret in the first body cell, as
    /// one undo step. False for anything else, and with `editor.alignTables`
    /// off. Cells are counted by tableRowCells, so a `\|` or a pipe in code
    /// is not a cell border.
    bool handleTableEnter();
    /// `noteAppC:insert-table`: Obsidian's 2x2 template with the caret in its
    /// first cell, on a paragraph of its own when the caret's line has text.
    void insertTable();
    /// Replaces lines `start`..`end` with the table `text` as one undo step,
    /// reparses (unless `reparseNow` is false: then the debounced parse does),
    /// and lets the caret-leave formatter know there is nothing left for it to do.
    void rewriteTable(qsizetype start, qsizetype end, const QString &text, bool reparseNow = true);
    /// What typing or pasting `text` puts into a drawn table's cell: a pipe
    /// escaped, a line break a space. `text` itself anywhere else.
    QString cellSafe(const QString &text) const;
    /// Aligns the source of the table holding `offset`, or of every table in
    /// the note when `offset` is -1, as one undo step; the caret stays with
    /// the text it was in. False when nothing needed it.
    bool formatTables(qsizetype offset);
    /// The index in m_decorations.tables of the table whose lines hold
    /// `offset`, in any mode; -1 outside every table.
    qsizetype tableAround(qsizetype offset) const;
    /// Advanced Tables' row and column commands on the table at the caret -
    /// ids "noteAppC:table-..." - rewriting the table aligned, as one undo
    /// step, the caret following its cell. False when the caret is in no
    /// table cell or the command does not apply (the header row cannot move).
    bool runTableCommand(const QString &id);
    /// False for an HTML table or a base. `rows` index r is parser row r on
    /// line r, or r + 1 past the header (tableLine).
    bool loadTableText(qsizetype index, TableText &table) const;
    static int tableLine(int row) { return row == 0 ? 0 : row + 1; }
    /// Rewrites the table aligned as one undo step; false when the source
    /// is already that.
    bool storeTableText(const TableText &table);
    /// The same commands for the right-click menu of a table cell.
    void addTableActions(QMenu &menu, qsizetype offset);
    /// Gives row `row` of table `table` (m_decorations.tables) every column
    /// the header has, appending empty cells to its line, as one undo step.
    /// False when it has them already.
    bool fillShortRow(qsizetype table, int row);
    /// fillShortRow for the source-less cell under `viewportPoint`, if any.
    bool fillShortRowAt(const QPoint &viewportPoint);
    /// Adds an empty row below `cell`'s and puts the caret in `column` of it.
    void insertTableRowAfter(const CellRef &cell, int column);
    /// Where a cell's text starts, in block coordinates.
    QPointF cellTextOrigin(const BlockView &view, const Cell &cell) const;
    /// The caret's cell in a rendered table, when it is in one.
    const Cell *caretCell(const BlockView &view) const;
    /// Scrolls a wide table sideways so the caret's cell is in view.
    void revealCaretCell();
    /// Heights, positions, and layouts for the blocks near the viewport.
    void updateLayout();
    /// Every block laid out again: what a theme, mode or width change costs.
    void invalidateLayouts();
    /// The block at `index` (all its pieces) laid out again; out of range is a no-op.
    void invalidateBlock(qsizetype index);
    /// invalidateLayouts() only if contentWidth() differs from the layouts'; then updateLayout().
    void relayoutIfWidthChanged();
    qreal estimatedHeight(const BlockView &view) const;
    qreal lineHeightFor(const BlockSpan &span) const;
    /// Moves every range at or after `at` by `delta`, so one keystroke does
    /// not need a reparse before the caret's block can be redrawn.
    void shiftRanges(qsizetype at, qsizetype delta);
    void applyEdit(qsizetype at, qsizetype removed, const QString &inserted);
    /// "Delete meta note": the run and one neighbouring space, one undo step.
    void deleteMetaNote(Range run);
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
    /// The colour a link to `target` is drawn in: unresolved, external or internal.
    QColor linkColourFor(const QString &target) const;
    bool resolvesToNothing(const QString &target) const;
    qsizetype blockAt(qsizetype sourceOffset) const;
    qsizetype offsetAt(const QPoint &viewportPoint) const;
    QPointF cursorPoint() const;
    /// Where `offset` is drawn, viewport coordinates, top of its line;
    /// `height`, when given, gets that line's own height (the caret's).
    QPointF pointFor(qsizetype offset, qreal *height = nullptr) const;
    /// Whether `offset` is on the empty line a note ending in a newline has
    /// under its last block, which belongs to no block: the caret there is
    /// drawn under the last block, not on top of it.
    bool belowLastBlock(qsizetype offset) const;
    /// The caret's rectangle in viewport coordinates, empty when not drawn.
    QRectF caretRect() const;
    void paintRemoteCarets(QPainter &painter) const;
    /// What a remote caret and its name tag cover, viewport coordinates.
    QRect remoteCaretRect(const RemoteCaret &caret) const;
    // --- touch, TouchInput.cpp -------------------------------------------
    /// `rect` grown by `mouseSlop`, or by a finger's worth when the press
    /// being handled came from a touchscreen: task boxes, fold arrows,
    /// chips and the rest are sized for a mouse.
    QRectF touchable(const QRectF &rect, qreal mouseSlop = 0) const;
    /// The two selection handles a touch selection shows: `end` false for
    /// the one at the selection's start. Viewport coordinates.
    QRectF selectionHandleRect(bool end) const;
    void paintSelectionHandles(QPainter &painter) const;
    /// A touch press on a handle starts dragging that end of the selection.
    bool pressSelectionHandle(const QPoint &point);
    bool dragSelectionHandle(const QPoint &point);
    bool releaseSelectionHandle();
    void ensureCursorVisible();
    void insertText(const QString &text);
    /// The insertFromMimeData logic shared by Ctrl+V and a drop: an image
    /// goes through m_attachmentSaver, local file URLs through
    /// m_fileImporter (or the path as text with neither set), else plain
    /// text - cellSafe()'d either way. Inserts at the caret. A remote URL
    /// is [title](url) when `dropped`, the bare URL (nothing, over a
    /// selection) when pasted, as in Obsidian.
    void insertMimeData(const QMimeData *mime, bool dropped = false);
    void removeSelectionOrChar(int direction);
    /// Where a word key takes a caret at `from`, grouped as CodeMirror 6 does
    /// (Obsidian's editor): grapheme clusters sorted into space, word
    /// characters (letters, digits, `_`, CJK) and the rest. `deleting` is
    /// Ctrl+Backspace/Delete: at a line's edge the line break itself, a single
    /// space skipped, never past the line. Otherwise Ctrl+Left/Right: every
    /// space and line break skipped, then one group. In a drawn table cell never
    /// past the cell; never inside an entity drawn as one character.
    qsizetype wordTarget(qsizetype from, int direction, bool deleting) const;
    /// Ctrl+Backspace (-1) and Ctrl+Delete (1): the selection if there is one,
    /// else up to wordTarget().
    void removeWord(int direction);
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
    /// Enter inside a list item: continue it, or end the list. `lineBreak`
    /// (Shift+Enter) breaks the line inside the item instead.
    bool handleListEnter(bool lineBreak = false);
    /// Backspace at the start of an item's text takes the bullet off.
    bool handleListBackspace();
    /// Swaps a rewritten list run in as one undo step, splicing only what changed.
    void commitRun(const Range &run, const QString &before, const QString &after, qsizetype caret);
    /// A fenced code block holds `offset`: its "- x" lines are code, not items.
    bool inCodeFenceAt(qsizetype offset) const;
    /// The parse's answer alone (no look at fences typed since).
    bool decoratedFenceAt(qsizetype offset) const;
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
    void insertCompletion(const QModelIndex &completion);
    // --- slash commands, implemented in SlashCommands.cpp ------------------
    /// Text typed after the nearest "/" that opens the popup (line start or
    /// after whitespace, not inside code, inline code or maths); unset when
    /// the caret is not in one, which also closes it.
    std::optional<QString> slashPrefix() const;
    void updateSlashSuggestions();
    void insertSlashCompletion(const QString &name);
    void ensureSlashCompleter();
    bool cursorInCodeOrMath() const;
    QList<QTextLayout::FormatRange> formatsFor(BlockView &view);
    /// A wavy underline under each word of `view` the dictionary rejects.
    void appendSpellingFormats(const BlockView &view, QList<QTextLayout::FormatRange> &formats) const;
    /// The words of `view` spelling looks at, in source coordinates: prose
    /// only, never code, links, tags or maths, and not the word being typed.
    QList<Range> checkedWordsOf(const BlockView &view) const;
    /// The word the caret is in or just after, while spelling is on.
    Range wordAtCaret() const;
    /// The run of word characters touching `offset`, whatever spelling does.
    Range wordAt(qsizetype offset) const;
    /// The suggestions and "Add to dictionary" for a misspelt word at
    /// `offset`, or "Remove from dictionary" for a personal one when `personal`.
    /// False, and nothing added, anywhere else.
    bool addSpellingActions(QMenu &menu, qsizetype offset, bool personal = true);
    void copySelection();
    void cutSelection();
    /// `plain`: Ctrl+Shift+V, text only, never an image saved as an attachment.
    void pasteClipboard(bool plain = false);
    /// Kate-definition colours for a fenced code block, in the coordinates of
    /// that block's display string. Not const: a piece records the state it
    /// ended in, for the piece after it.
    QList<QTextLayout::FormatRange> codeFormatsFor(BlockView &view);
    /// Highlights one piece from the end state of the piece before it and
    /// records where it ended; the formats are relative to `*content`.
    QList<QTextLayout::FormatRange> highlightPiece(BlockView &view, Range *content,
                                                   QColor *plainText = nullptr);
    /// Brings the end state of every piece before `view` up to date, so
    /// `view` can start from the one just before it.
    void updateCodeStatesBefore(BlockView &view);
    const BlockView &firstPiece(const BlockView &view) const;
    const BlockView &lastPiece(const BlockView &view) const;
    QList<QTextLayout::FormatRange> nestedCodeFormatsFor(const BlockView &view) const;
    QList<BlockSpan> plainParagraphs() const;
    /// The code between the fences, and the language the fence declared.
    /// For one piece of a long fence: that piece's share of the code, and the
    /// fence's language.
    Range codeContentOf(const BlockView &view, QString *language) const;
    QFont fontFor(const BlockSpan &span) const;
    void watchStyleFiles();
    void reloadStyleFiles();
    /// Every block between `exposedTop` and `exposedBottom`, which are in
    /// viewport coordinates for a document scrolled to `scroll`. The screen
    /// and a PDF page both paint through this.
    void paintBlocks(QPainter &painter, qreal scroll, qreal exposedTop, qreal exposedBottom) const;
    bool layoutMedia(BlockView &view, const QString &path, bool video);
    void paintMedia(QPainter &painter, const BlockView &view, qreal left, qreal top) const;
    /// A player card's parts, in viewport coordinates: the picture (empty for
    /// audio), the play button and the progress bar.
    QRectF mediaScreenRect(const BlockView &view, qreal left, qreal top) const;
    QRectF mediaButtonRect(const BlockView &view, qreal left, qreal top) const;
    QRectF mediaBarRect(const BlockView &view, qreal left, qreal top) const;
    /// Play, pause or seek, for a click at `point` on a player card.
    bool handleMediaClick(const BlockView &view, const QPoint &point, qreal left, qreal top);
    void updateLinkHover(const QPoint &viewportPoint, bool control, const QPoint &globalPos);
    static constexpr int kEditValue = -1;
    static constexpr int kEditNewItem = -2;
    std::function<QString(const QString &)> m_resolver;
    std::function<void(const QString &, const QueryDeliver &)> m_queryRunner;
    std::function<void(const QString &, const QString &, const BaseDeliver &)> m_baseRunner;
    std::function<QStringList(const QString &, bool)> m_fragmentProvider;
    std::function<void(const QString &id)> m_runSlashCommand;
    void setWordCount(int words);
};

} // namespace noteAppC
