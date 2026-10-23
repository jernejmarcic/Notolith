#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "FormatColours.h"
#include "Frontmatter.h"

#include "Icons.h"
#include "MediaPlayback.h"
#include "ObsidianSyntax.h"
#include "PdfPage.h"
#include "PopupStyle.h"
#include "Vault.h"

#include <QAbstractItemView>
#include <QAbstractListModel>
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QCompleter>
#include <QListView>
#include <QCursor>
#include <QUrl>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QWheelEvent>
#include <QPainter>
#include <QRegion>
#include <QPainterPath>
#include <QGuiApplication>
#include <QScrollBar>
#include <QScroller>
#include <QInputDevice>
#include <QElapsedTimer>
#include <QFile>
#include <QScopeGuard>
#ifdef NOTEAPPC_HAVE_OPENGLWIDGETS
#include <QOpenGLWidget>
#endif
#include <QFileInfo>
#include <QImageReader>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QRegularExpression>
#include <QSet>
#include <QStringListModel>
#include <QtConcurrent>
#include <QTextLine>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>

namespace noteAppC {

namespace {

/// What the `[[` popup lists: every name, then every alias. An alias row is
/// matched on the alias, shown as "alias -> Note" and inserted as `Note|alias`.
/// Shares the names' list rather than copying it: a vault can have 20 000.
class LinkModel : public QAbstractListModel {
public:
    static constexpr int InsertRole = Qt::UserRole;

    using QAbstractListModel::QAbstractListModel;

    void setNames(const QStringList &names)
    {
        beginResetModel();
        m_names = names;
        endResetModel();
    }
    void setAliases(const QList<QPair<QString, QString>> &aliases)
    {
        beginResetModel();
        m_aliases = aliases;
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(m_names.size() + m_aliases.size());
    }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return {};
        if (index.row() < m_names.size())
            return role == Qt::DisplayRole || role == Qt::EditRole || role == InsertRole
                       ? QVariant(m_names.at(index.row())) : QVariant();
        const auto &[alias, note] = m_aliases.at(index.row() - m_names.size());
        switch (role) {
        case Qt::EditRole: return alias;
        case Qt::DisplayRole: return alias + QStringLiteral(" -> ") + note;
        case InsertRole: return note + QLatin1Char('|') + alias;
        default: return {};
        }
    }

private:
    QStringList m_names;
    QList<QPair<QString, QString>> m_aliases;
};

/// The caret's `[[` text split at its first `#`: `note` before it (empty for
/// the note being edited), then `text` after it, or after `#^` for block ids.
/// Further `#`s nest: `Note#H1#te` has `parents` {H1} and `text` te.
struct LinkFragment {
    bool active = false; ///< the text has a `#`
    QString note;
    QStringList parents;
    bool blocks = false;
    QString text;
};

LinkFragment splitLinkFragment(const QString &typed)
{
    LinkFragment fragment;
    const qsizetype hash = typed.indexOf(QLatin1Char('#'));
    if (hash < 0)
        return fragment;
    fragment.active = true;
    fragment.note = typed.left(hash);
    fragment.blocks = typed.mid(hash + 1).startsWith(QLatin1Char('^'));
    fragment.text = typed.mid(hash + 1 + (fragment.blocks ? 1 : 0));
    if (!fragment.blocks) {
        if (const qsizetype nested = fragment.text.lastIndexOf(QLatin1Char('#')); nested >= 0) {
            fragment.parents = fragment.text.left(nested).split(QLatin1Char('#'));
            fragment.text = fragment.text.mid(nested + 1);
        }
    }
    return fragment;
}

// The source's own blank lines make the vertical rhythm, exactly as the file
// reads; only headings get extra air, the way Obsidian styles them.
constexpr int kBlockSpacing = 0;

// These take the private BlockView, so they are templates: no friend or header
// line needed to name it.

/// A blank line draws nothing.
template<typename View>
bool isBlankBlock(const QString &text, const View &view)
{
    const Range &range = view.span.range;
    return view.span.kind == BlockSpan::Kind::Paragraph
           && QStringView(text).mid(range.start, range.length()).trimmed().isEmpty();
}

/// How far a callout's panel reaches past the block's own box, above its first
/// piece or below its last (see paintCallout). A fence's panel reaches as far,
/// but into its own fence lines, which are blocks of their own.
template<typename View>
qreal panelBleed(const View &view, bool below)
{
    if (view.span.kind != BlockSpan::Kind::Callout)
        return 0;
    return (below ? view.piece == view.pieces - 1 || view.pieces <= 1 : view.piece == 0) ? 8 : 0;
}

/// The space between two neighbouring blocks. None, except that a callout's
/// panel runs 8 px into it, so a block right against the callout, with no
/// blank line between, needs that much to stay clear of the panel.
template<typename View>
qreal blockGap(const QString &text, const View &above, const View &below)
{
    // The bleeds are fields beside `y` (set in rebuildBlocks) and checked first:
    // reading the kind, the pieces or the note's text for every block cost a
    // cache miss each on a layout pass that walks them all (0.2 ms at 1 MB).
    const qreal bleed = above.bleedBelow + below.bleedAbove;
    if (bleed == 0 || isBlankBlock(text, above) || isBlankBlock(text, below))
        return kBlockSpacing;
    return kBlockSpacing + bleed;
}

/// Whether Qt reads this file suffix as an image. The usual ones are answered
/// from a list: asking QImageReader first loads every image plugin, 28 ms and
/// 13 MB for a note whose only content is a table.
bool isImageSuffix(const QString &suffix)
{
    static const QStringList common = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                       QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("svg"),
                                       QStringLiteral("webp"), QStringLiteral("avif")};
    if (common.contains(suffix))
        return true;
    static const QByteArrayList supported = QImageReader::supportedImageFormats();
    return supported.contains(suffix.toUtf8());
}

/// Width of the margin a list keeps for its bullets and checkboxes.
constexpr qreal kListIndent = 26;
/// Above this many characters a note is shown first and parsed on the thread
/// pool. Below it the parse is quicker than a frame, so the intermediate
/// plain-text state would be a flicker and nothing else. 200 kB parses in
/// about 26 ms, 50 kB in 7 ms; this sits between them.
constexpr qsizetype kAsyncParseThreshold = 128 * 1024;
/// A stand-in block of a big note whose parse is out ends after this many lines or this many
/// characters, whichever comes first (plainParagraphs). A longer line is a chunk of its own.
constexpr int kPlaceholderLines = 64;
constexpr qsizetype kPlaceholderChars = 4096;
/// A newline the reader types is parsed on the spot (see applyEdit) while this
/// document's last parse took less than this: well inside what one key may cost,
/// whatever the machine. Past it the debounced pool parse does it.
constexpr qint64 kSyncParseBudgetNs = 4'000'000;
constexpr int kTypingBurstMs = 500;
/// How many undo (and redo) steps an edit from elsewhere is carried over. Each
/// one costs a scan of the whole note, so the oldest go rather than a remote
/// keystroke costing milliseconds on a long note with a deep history.
constexpr qsizetype kCarriedSteps = 100;
/// A fence longer than this is split into blocks of this many lines. Laying
/// out and highlighting 32 lines costs about a millisecond; the whole of a
/// 5 000-line listing cost 158 ms, and it was redone on every keystroke.
constexpr qsizetype kCodePieceLines = 32;

qreal headingScale(int level)
{
    // Obsidian's h1..h6 multipliers.
    switch (level) {
    case 1:
        return 1.802;
    case 2:
        return 1.602;
    case 3:
        return 1.424;
    case 4:
        return 1.266;
    case 5:
        return 1.125;
    default:
        return 1.0;
    }
}

} // namespace

MarkdownEditor::MarkdownEditor(QWidget *parent)
    : QAbstractScrollArea(parent)
    , d(std::make_unique<Private>())
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(true);
    viewport()->setAcceptDrops(true); // an image or a file dropped in; see PasteDrop.cpp
    horizontalScrollBar()->setRange(0, 0);
    // An icon given as an https URL arrives after the first paint. Without
    // this it stayed blank until something unrelated happened to repaint.
    if (auto *icons = qobject_cast<IconsNotifier *>(Icons::notifier()))
        connect(icons, &IconsNotifier::iconReady, this, [this] { viewport()->update(); });
    watchStyleFiles();
    setKeyBindings({}); // the built-in keys, until the window hands over its own

    grabTouchScroll(viewport()); // a drag scrolls; a held finger selects a word
    d->m_longPress.setSingleShot(true);
    d->m_longPress.setInterval(500);
    d->m_pdfZoomSettle.setSingleShot(true);
    d->m_pdfZoomSettle.setInterval(150); // after the last zoom step
    connect(&d->m_pdfZoomSettle, &QTimer::timeout, this, &MarkdownEditor::requestPdfZoomRenders);
    d->m_tableScrollTimer.setInterval(16);
    connect(&d->m_tableScrollTimer, &QTimer::timeout, this, &MarkdownEditor::tableAutoScrollTick);
    connect(&d->m_longPress, &QTimer::timeout, this, [this] {
        d->m_pressing = false;
        // In a table cell the finger may now drag a rectangle of cells; the
        // word is selected as ever, until it does.
        if (d->m_cellPress >= 0 && d->m_cellPressTouch) {
            d->m_cellArmed = true;
            if (QScroller::hasScroller(viewport()))
                QScroller::scroller(viewport())->stop();
        }
        selectWordAt(d->m_pressOffset);
        d->m_touchHandles = true; // a finger's selection is adjusted by its handles
        viewport()->update();
    });

    // ponytail: the parse is synchronous behind a short debounce. It measures
    // well under the keystroke budget on notes this size; move it to the
    // thread pool when a 1 MB note says otherwise.
    d->m_parseTimer.setSingleShot(true);
    d->m_parseTimer.setInterval(20);
    connect(&d->m_parseTimer, &QTimer::timeout, this, &MarkdownEditor::startParse);
    connect(&d->m_parseWatcher, &QFutureWatcher<ParseJob>::finished, this, &MarkdownEditor::adoptParse);

    d->m_blink.setInterval(QApplication::cursorFlashTime() / 2);
    connect(&d->m_blink, &QTimer::timeout, this, [this] {
        d->m_cursorOn = !d->m_cursorOn;
        // Only where the caret is, not the whole page twice a second.
        const QRect caret = d->m_cursorOn ? caretRect().toAlignedRect().adjusted(-2, -2, 2, 2) : d->m_caretPainted;
        if (caret.isEmpty())
            viewport()->update();
        else
            viewport()->update(caret);
    });

    auto *linkModel = new LinkModel(this);
    d->m_linkModel = linkModel;
    d->m_fragmentModel = new QStringListModel(this);
    d->m_completer = new QCompleter(linkModel, this);
    d->m_completer->setWidget(this);
    d->m_completer->popup()->setObjectName(QStringLiteral("completerPopup"));
    // One line per name. Without this, every application-wide style change
    // (each theme pass) sized all of a vault's link targets row by row, hidden
    // popup or not: 157 ms of a 20 000-note vault's cold start.
    if (auto *list = qobject_cast<QListView *>(d->m_completer->popup()))
        list->setUniformItemSizes(true);
    d->m_completer->setCompletionMode(QCompleter::PopupCompletion);
    d->m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    d->m_completer->setFilterMode(Qt::MatchContains);
    connect(d->m_completer, qOverload<const QModelIndex &>(&QCompleter::activated),
            this, &MarkdownEditor::insertCompletion);

    d->m_lastEdit.start();
}

void MarkdownEditor::setLinkResolver(std::function<QString(const QString &)> resolver)
{
    m_resolver = std::move(resolver);
    d->m_images.clear();
    clearGIFs();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setFeatures(const Features &features)
{
    if (features == d->m_features)
        return; // every settings pass asks, and this relays out every block
    const bool HTMLchanged = features.HTMLblocks != d->m_features.HTMLblocks;
    d->m_features = features;
    if (HTMLchanged && !d->m_text.isEmpty())
        reparse(); // an HTML block's own blocks come from the parse (HtmlEdit.cpp)
    // A rare, administrative call - not the keystroke path - so relaying out
    // every block rather than picking out just the ones a flag touches is
    // the simple and correct choice, not a slow one that happens to work.
    d->clearPdfPages();
    d->m_texCache.clear();
    d->m_fileEmbedCache.clear();
    if (!features.CSVembeds)
        dropCSVcache();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setFileEmbedRenderer(const QStringList &suffixes, FileEmbedRenderer renderer)
{
    // A null renderer takes the suffix back. Only a change in which suffixes
    // have one re-renders anything: every settings pass hands the same one.
    bool changed = false;
    for (const QString &suffix : suffixes) {
        changed = changed || bool(renderer) != d->m_fileEmbedRenderers.contains(suffix.toLower());
        if (renderer)
            d->m_fileEmbedRenderers.insert(suffix.toLower(), renderer);
        else
            d->m_fileEmbedRenderers.remove(suffix.toLower());
    }
    if (!changed)
        return;
    d->m_fileEmbedCache.clear();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setFileEmbedInputHandler(const QStringList &suffixes, FileEmbedInputHandler handler)
{
    // Unlike setFileEmbedRenderer, registering or dropping a handler never
    // changes what is drawn - only whether it answers to a mouse - so
    // nothing here needs a relayout.
    for (const QString &suffix : suffixes) {
        if (handler)
            d->m_fileEmbedInputHandlers.insert(suffix.toLower(), handler);
        else
            d->m_fileEmbedInputHandlers.remove(suffix.toLower());
    }
}

void MarkdownEditor::setLinkTargets(const QStringList &targets)
{
    static_cast<LinkModel *>(d->m_linkModel)->setNames(targets);
}

void MarkdownEditor::setLinkAliases(const QList<QPair<QString, QString>> &aliases)
{
    static_cast<LinkModel *>(d->m_linkModel)->setAliases(aliases);
}

void MarkdownEditor::setLinkFragmentProvider(std::function<QStringList(const QString &, bool)> provider)
{
    m_fragmentProvider = std::move(provider);
    d->m_fragmentKey.clear();
}

std::optional<QString> MarkdownEditor::wikilinkPrefix() const
{
    const qsizetype lineStart = d->m_text.lastIndexOf(QLatin1Char('\n'),
                                                   qMax<qsizetype>(0, d->m_cursor - 1)) + 1;
    const QStringView before = QStringView(d->m_text).mid(lineStart, d->m_cursor - lineStart);

    const qsizetype open = before.lastIndexOf(QLatin1String("[["));
    if (open < 0)
        return std::nullopt;
    const QStringView typed = before.mid(open + 2);
    if (typed.contains(QLatin1String("]]")) || typed.contains(QLatin1Char('|')))
        return std::nullopt;
    return typed.toString();
}

void MarkdownEditor::updateSuggestions()
{
    const auto prefix = wikilinkPrefix();
    if (!prefix) {
        d->m_completer->popup()->hide();
        d->m_fragmentKey.clear();
        return;
    }

    // After a `#` the popup lists that note's headings or block ids instead of
    // names; they are fetched once per note and kind, not per keystroke.
    const LinkFragment fragment = splitLinkFragment(*prefix);
    if (fragment.active) {
        const QString key = (fragment.blocks ? QStringLiteral("^") : QStringLiteral("#")) + fragment.note
                            + (fragment.parents.isEmpty() ? QString() : QLatin1Char('#') + fragment.parents.join(QLatin1Char('#')));
        if (key != d->m_fragmentKey) {
            d->m_fragmentKey = key;
            QStringList candidates;
            if (!fragment.note.isEmpty()) {
                // `Note#H1` asks for what lies under H1 (see setLinkFragmentProvider).
                if (m_fragmentProvider)
                    candidates = m_fragmentProvider(key.mid(1), fragment.blocks);
            } else if (fragment.blocks) {
                for (const BlockId &block : ObsidianSyntax::scan(d->m_text, {}).blockIds)
                    candidates << block.id;
            } else {
                for (const qsizetype i : ObsidianSyntax::headingsUnder(d->m_headings, fragment.parents))
                    candidates << d->m_headings.at(i).text;
            }
            // Written as the link spells them, so what is inserted resolves.
            if (!fragment.blocks) {
                for (QString &candidate : candidates)
                    candidate = ObsidianSyntax::headingLinkText(candidate);
            }
            d->m_fragmentModel->setStringList(candidates);
        }
    } else {
        d->m_fragmentKey.clear();
    }
    QAbstractItemModel *wanted = fragment.active ? static_cast<QAbstractItemModel *>(d->m_fragmentModel) : d->m_linkModel;
    if (d->m_completer->model() != wanted)
        d->m_completer->setModel(wanted);

    d->m_completer->setCompletionPrefix(fragment.active ? fragment.text : *prefix);
    if (d->m_completer->completionCount() == 0) {
        d->m_completer->popup()->hide();
        return;
    }
    d->m_completer->popup()->setCurrentIndex(d->m_completer->completionModel()->index(0, 0));

    const QPointF point = cursorPoint();
    QRect anchor(point.toPoint(), QSize(1, int(QFontMetricsF(font()).height())));
    anchor.moveTopLeft(viewport()->mapTo(this, anchor.topLeft()));
    anchor.setWidth(d->m_completer->popup()->sizeHintForColumn(0) + 32);
    d->m_completer->complete(anchor);
}

void MarkdownEditor::insertCompletion(const QModelIndex &chosen)
{
    const auto prefix = wikilinkPrefix();
    if (!prefix)
        return;
    const QVariant insert = chosen.data(LinkModel::InsertRole);
    const QString completion = insert.isValid() ? insert.toString() : chosen.data().toString();

    // Replace what was typed (after the `#` for a heading or block id), then
    // step over the closing brackets if the auto-pair already put them there.
    const LinkFragment fragment = splitLinkFragment(*prefix);
    const qsizetype typed = fragment.active ? fragment.text.size() : prefix->size();
    const qsizetype at = d->m_cursor - typed;
    const bool closed =
        QStringView(d->m_text).mid(d->m_cursor).startsWith(QLatin1String("]]"));
    applyEdit(at, typed, closed ? completion : completion + QLatin1String("]]"));
    if (closed)
        d->m_cursor = d->m_anchor = d->m_cursor + 2;
}

MarkdownEditor::~MarkdownEditor()
{
    // Workers post their results back through m_lifeline; ending it first
    // means none is part-way through a post to this object (see Lifeline).
    d->m_lifeline->end();

    // An open property editor is a child widget, and Qt deletes children
    // after this object's members are gone: losing focus as it went, it
    // would call back into an editor that no longer exists. Cut it loose now.
    if (d->m_propertyEdit) {
        d->m_propertyEdit->disconnect(this);
        d->m_propertyEdit->removeEventFilter(this);
        delete d->m_propertyEdit.data();
    }
    if (d->m_calloutTitleEdit) {
        d->m_calloutTitleEdit->disconnect(this);
        d->m_calloutTitleEdit->removeEventFilter(this);
        delete d->m_calloutTitleEdit.data();
    }
    if (d->m_parseWatcher.isRunning())
        d->m_parseWatcher.waitForFinished();
}

// Public accessors for state that lives in Private, so the header need not
// know it.
MarkdownEditor::Mode MarkdownEditor::mode() const
{
    return d->m_mode;
}

QString MarkdownEditor::noteText() const
{
    return d->m_text;
}

bool MarkdownEditor::isModified() const
{
    return d->m_modified;
}

bool MarkdownEditor::lineNumbers() const
{
    return d->m_lineNumbers;
}

bool MarkdownEditor::strictLineBreaks() const
{
    return d->m_strictLineBreaks;
}

const ParseOptions &MarkdownEditor::parseOptions() const
{
    return d->m_parseOptions;
}

const QList<Range> &MarkdownEditor::metaNoteRanges() const
{
    return d->m_decorations.metaNotes;
}

bool MarkdownEditor::indentGuides() const
{
    return d->m_indentGuides;
}

void MarkdownEditor::setAlignTables(bool on)
{
    d->m_alignTables = on;
}

MarkdownEditor::TablePart MarkdownEditor::hoveredTablePart() const
{
    return d->m_tableHover.part;
}

int MarkdownEditor::tableAddPreview() const
{
    const bool bar = d->m_tableDrag.part == TablePart::AddRow || d->m_tableDrag.part == TablePart::AddColumn;
    return bar && d->m_tableDrag.moved ? d->m_tableDrag.to : 0;
}

bool MarkdownEditor::tableChromeShown() const
{
    return d->m_tableHover.block >= 0;
}

bool MarkdownEditor::tableAutoScrolling() const
{
    return d->m_tableScrollTimer.isActive();
}

qsizetype MarkdownEditor::hoveredBlock() const
{
    return d->m_hoveredBlock;
}

bool MarkdownEditor::complexCodeBlocks() const
{
    return d->m_complexCodeBlocks;
}

bool MarkdownEditor::outlineNumbering() const
{
    return d->m_outlineNumbering;
}

QString MarkdownEditor::listNumberSuffix() const
{
    return d->m_listNumberSuffix;
}

BulletGlyphMap MarkdownEditor::bulletGlyphs() const
{
    return d->m_bulletGlyphs;
}

bool MarkdownEditor::nestedBulletGlyphs() const
{
    return d->m_nestedBulletGlyphs;
}

int MarkdownEditor::tabWidth() const
{
    return d->m_tabWidth;
}

bool MarkdownEditor::useWikilinks() const
{
    return d->m_useWikilinks;
}

bool MarkdownEditor::GPUacceleration() const
{
    return d->m_GPUacceleration;
}

int MarkdownEditor::wordCount() const
{
    return d->m_wordCount;
}

qsizetype MarkdownEditor::cursorOffset() const
{
    return d->m_cursor;
}

QRectF MarkdownEditor::touchSelectionHandle(bool end) const
{
    return d->m_touchHandles && selection().length() > 0 ? selectionHandleRect(end) : QRectF();
}

qreal MarkdownEditor::documentHeight() const
{
    return d->m_totalHeight;
}

quint64 MarkdownEditor::layoutCount() const
{
    return d->m_layoutCount;
}

quint64 MarkdownEditor::rebuiltViewCount() const
{
    return d->m_rebuiltViews;
}

const MarkdownEditor::Features &MarkdownEditor::features() const
{
    return d->m_features;
}

void MarkdownEditor::setAutoPairs(bool on)
{
    d->m_autoPairs = on;
}

bool MarkdownEditor::autoPairs() const
{
    return d->m_autoPairs;
}

bool MarkdownEditor::vimMode() const
{
    return d->m_vim.enabled;
}

int MarkdownEditor::findMatchCount() const
{
    return int(d->m_find.matches.size());
}

int MarkdownEditor::findCurrentIndex() const
{
    return d->m_find.current;
}

bool MarkdownEditor::findPatternValid() const
{
    return d->m_find.valid;
}

const QList<Property> &MarkdownEditor::properties() const
{
    return d->m_properties;
}

const QList<Heading> &MarkdownEditor::headings() const
{
    return d->m_headings;
}

void MarkdownEditor::setTheme(const Theme &theme)
{
    const bool recolour = d->m_theme.sameExcept(theme, formatColourTokens());
    d->m_theme = theme;
    d->m_themeSyntax = theme.syntaxStyles();
    d->m_code.setTheme(theme);
    if (d->m_findBar)
        d->m_findBar->setTheme(theme);
    if (recolour) {
        // Only how kinds of text are coloured: keep every height and the
        // scroll position, and lay out again just what is on screen.
        refreshSpelling();
        return;
    }
    d->m_lineHeights.clear();
    d->m_blockSpacing = 0;
    invalidateLayouts();
    setFont(theme.textFont());
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setMode(Mode mode)
{
    if (d->m_mode == mode)
        return;
    finishPropertyEdit(true); // source and reading modes draw no table to edit in
    d->m_mode = mode;
    // Reading view appends a trailing footnotes-list block that no other
    // mode shows; rebuildBlocks() is what adds or drops it. Skipped when
    // there is nothing to list - a note without footnotes costs nothing.
    if (!d->m_decorations.footnoteDefinitions.isEmpty()) {
        rebuildBlocks();
        indexDecorations(); // new BlockViews hold no styles or syntax until told which
    }
    invalidateLayouts();
    updateLayout();
    viewport()->update();
    emit modeChanged();
}

namespace {
std::function<void()> &bigParseLandedHook()
{
    static std::function<void()> hook;
    return hook;
}
} // namespace

void MarkdownEditor::setBigParseLandedHook(std::function<void()> hook)
{
    bigParseLandedHook() = std::move(hook);
}

void MarkdownEditor::setNoteText(const QString &text)
{
    finishPropertyEdit(false); // a different note: what was typed belongs to the old one
    clearGIFs();
    if (d->m_media)
        d->m_media->stop();
    d->m_text = text;
    ++d->m_textVersion;
    d->m_cursor = d->m_anchor = 0;
    d->m_activeBlock = -1;
    d->m_tableEdited = false;
    d->m_caretCell = {-1, -1};
    d->m_undo.clear();
    d->m_redo.clear();
    d->m_interacted = false;
    // Per-block state keyed by source offset belongs to the note it was made
    // in: the next note's block at that offset would inherit it.
    d->m_pdfZoom.clear();
    d->m_pdfSlideIndex.clear();
    d->m_pdfNavLast.clear();
    d->m_folds.clear();
    d->m_pendingFolds.clear();
    d->m_queryCollapsed.clear();
    d->m_queryNameSort.clear();
    // A big note is shown before it is understood: its text goes up as plain
    // paragraphs at once and the parse runs on the thread pool, because the
    // parse is the whole cost of opening one (142 ms of a 1 MB note's 146 ms)
    // and there is no reason to stare at nothing while it happens. A small
    // note parses faster than a frame, so it skips the intermediate state
    // rather than flickering through it.
    d->m_placeholderVersion = text.size() > kAsyncParseThreshold ? d->m_textVersion : 0;
    if (d->m_placeholderVersion) {
        setWordCount(-1); // not counted until the parse comes back
        adoptDecorations({}, {}, {});
        startParse();
    } else {
        reparse();
    }
    if (!d->m_find.pattern.isEmpty())
        updateFindMatches();
    setModified(false);
    verticalScrollBar()->setValue(0);
    if (d->m_vim.enabled) { // a new buffer: back to normal mode, the register kept
        VimState fresh;
        fresh.enabled = true;
        fresh.reg = d->m_vim.reg;
        fresh.regLinewise = d->m_vim.regLinewise;
        fresh.lastSearch = d->m_vim.lastSearch;
        fresh.searchForward = d->m_vim.searchForward;
        fresh.lastStatus = d->m_vim.lastStatus;
        d->m_vim = fresh;
        emitVimStatus();
    }
    viewport()->update();
}

// --- the find bar ---------------------------------------------------------

void MarkdownEditor::showFind(bool withReplace)
{
    if (!d->m_findBar) {
        d->m_findBar = new FindBar(viewport());
        d->m_findBar->setTheme(d->m_theme);
        connect(d->m_findBar, &FindBar::queryChanged, this, [this] {
            setFindQuery(d->m_findBar->pattern(), d->m_findBar->caseSensitive(),
                         d->m_findBar->useRegex());
        });
        connect(d->m_findBar, &FindBar::stepped, this, &MarkdownEditor::findStep);
        connect(d->m_findBar, &FindBar::replaceRequested, this, [this] {
            replaceCurrent(d->m_findBar->replacement());
        });
        connect(d->m_findBar, &FindBar::replaceAllRequested, this, [this] {
            replaceAll(d->m_findBar->replacement());
        });
        connect(d->m_findBar, &FindBar::dismissed, this, &MarkdownEditor::hideFind);
    }

    // Ctrl+H on an open find bar adds the replace row rather than closing it.
    if (withReplace)
        d->m_findBar->setReplaceShown(true);
    d->m_findBar->show();
    positionFindBar();
    d->m_findBar->focusSearch();
    // Reopening picks the last query back up: the field kept its text while
    // the bar was away, so the matches have to catch up with it.
    setFindQuery(d->m_findBar->pattern(), d->m_findBar->caseSensitive(), d->m_findBar->useRegex());
}

void MarkdownEditor::hideFind()
{
    if (!d->m_findBar)
        return;
    d->m_findBar->hide();
    d->m_find.pattern.clear();
    d->m_find.matches.clear();
    d->m_find.current = -1;
    d->m_find.valid = true;
    setFocus(Qt::OtherFocusReason);
    viewport()->update();
}

bool MarkdownEditor::findVisible() const
{
    // isVisible() is false while the whole editor is still unshown, which is
    // not what "the bar is open" means.
    return d->m_findBar && !d->m_findBar->isHidden();
}

void MarkdownEditor::positionFindBar()
{
    if (!d->m_findBar || d->m_findBar->isHidden())
        return;
    const QSize hint = d->m_findBar->sizeHint();
    const int width = qMin(hint.width(), viewport()->width() - 16);
    d->m_findBar->setGeometry(viewport()->width() - width - 8, 8, width, hint.height());
}

void MarkdownEditor::setModified(bool modified)
{
    if (d->m_modified == modified)
        return;
    d->m_modified = modified;
    emit modificationChanged(d->m_modified);
}

void MarkdownEditor::setStrictLineBreaks(bool on)
{
    if (d->m_strictLineBreaks == on)
        return;
    d->m_strictLineBreaks = on;
    // It decides whether a newline inside a paragraph joins or breaks, so
    // every block has to be laid out again, not merely repainted.
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setParseOptions(const ParseOptions &options)
{
    const auto same = [](const ParseOptions &l, const ParseOptions &r) {
        // A delimiter pair matters only while its feature is on.
        return l.brackets == r.brackets && l.metaNotes == r.metaNotes
               && (!l.metaNotes || (l.open == r.open && l.close == r.close))
               && l.excludeBetween == r.excludeBetween
               && (!l.excludeBetween || (l.excludeOpen == r.excludeOpen && l.excludeClose == r.excludeClose))
               && l.ignoreFormatting == r.ignoreFormatting && l.taskStates == r.taskStates;
    };
    if (same(options, d->m_parseOptions))
        return; // every settings pass asks
    d->m_parseOptions = options;
    if (options.taskStates)
        TaskStates::reload();
    // The runs and counts come from the parse, so a change is a new parse.
    if (!d->m_text.isEmpty()) {
        // A parse in flight has the old options: let it end unadopted.
        if (d->m_parseWatcher.isRunning())
            d->m_parseWatcher.waitForFinished();
        d->m_parsePending = false;
        reparse();
        invalidateLayouts();
        updateLayout();
        viewport()->update();
    }
}

void MarkdownEditor::setIndentGuides(bool on)
{
    if (d->m_indentGuides == on)
        return;
    d->m_indentGuides = on;
    viewport()->update(); // drawn behind the text; nothing moves
}

void MarkdownEditor::setFolding(bool heading, bool indent)
{
    if (d->m_foldHeading == heading && d->m_foldIndent == indent)
        return;
    d->m_foldHeading = heading;
    d->m_foldIndent = indent;
    invalidateLayouts(); // a list's hidden lines are part of its layout
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setOutlineNumbering(bool on)
{
    if (d->m_outlineNumbering == on)
        return;
    d->m_outlineNumbering = on;
    // The labels' widths set the lists' columns, so the text moves.
    for (BlockView &block : d->m_blocks) {
        if (block.listTextX.empty())
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
    }
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setListNumberSuffix(const QString &suffix)
{
    if (d->m_listNumberSuffix == suffix)
        return;
    d->m_listNumberSuffix = suffix;
    // The labels' widths set the lists' columns, as outline numbering's do.
    for (BlockView &block : d->m_blocks) {
        if (block.listTextX.empty())
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
    }
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setBulletGlyphs(const QString &spec)
{
    const BulletGlyphMap map = BulletGlyphMap::parse(spec);
    if (map == d->m_bulletGlyphs)
        return;
    d->m_bulletGlyphs = map;
    viewport()->update(); // drawn in the margin; nothing moves
}

void MarkdownEditor::setNestedBulletGlyphs(bool on)
{
    if (on == d->m_nestedBulletGlyphs)
        return;
    d->m_nestedBulletGlyphs = on;
    viewport()->update();
}

void MarkdownEditor::setTabWidth(int spaces)
{
    d->m_tabWidth = qBound(1, spaces, 8);
}

void MarkdownEditor::setUseWikilinks(bool on)
{
    d->m_useWikilinks = on;
}

bool MarkdownEditor::GPUaccelerationWanted(const QString &setting, qreal devicePixelRatio)
{
    if (setting != QLatin1String("auto"))
        return setting == QLatin1String("true");
    return !qFuzzyCompare(devicePixelRatio, std::round(devicePixelRatio));
}

bool MarkdownEditor::GPUaccelerationAvailable()
{
#ifdef NOTEAPPC_HAVE_OPENGLWIDGETS
    // The offscreen and minimal platforms have no GL context to give, and
    // asking anyway aborts rather than degrading.
    const QString platform = QGuiApplication::platformName();
    return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
#else
    return false;
#endif
}

void MarkdownEditor::setGPUacceleration(bool on)
{
    if (d->m_GPUacceleration == on)
        return;
#ifdef NOTEAPPC_HAVE_OPENGLWIDGETS
    if (on && !GPUaccelerationAvailable())
        return;
    d->m_GPUacceleration = on;
    // setViewport takes ownership and destroys the old one, so everything the
    // viewport carries has to be set again.
    setViewport(on ? new QOpenGLWidget(this) : nullptr);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(true);
    viewport()->setAcceptDrops(true);
    grabTouchScroll(viewport());
    viewport()->update();
#else
    Q_UNUSED(on);
#endif
}

void MarkdownEditor::setLineNumbers(bool on)
{
    if (d->m_lineNumbers == on)
        return;
    d->m_lineNumbers = on;
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setReadableLineLength(bool on)
{
    if (d->m_readable == on)
        return;
    d->m_readable = on;
    relayoutIfWidthChanged();
    viewport()->update();
}

void MarkdownEditor::setViewportPadding(int side, int top)
{
    side = qMax(0, side);
    top = qMax(0, top);
    if (d->m_minimumSideMargin == side && d->m_topPadding == top)
        return;
    d->m_minimumSideMargin = side;
    d->m_topPadding = top;
    relayoutIfWidthChanged(); // the side margin changes the width; the layout pass places the blocks under the top one
    viewport()->update();
}

void MarkdownEditor::setContentWidth(int pixels)
{
    if (qMax(200, pixels) == d->m_contentWidth)
        return; // every settings pass sets it, and this relaid out every block
    d->m_contentWidth = qMax(200, pixels);
    relayoutIfWidthChanged();
    viewport()->update();
}

void MarkdownEditor::relayoutIfWidthChanged()
{
    // Line breaking depends on the text width alone (layouts are in block
    // coordinates; contentLeft is added at paint), so a window still wider
    // than the readable line length moves nothing.
    if (contentWidth() != d->m_layoutWidth)
        invalidateLayouts();
    updateLayout();
}

bool MarkdownEditor::revealsWholeBlock(const BlockView &view) const
{
    switch (view.span.kind) {
    case BlockSpan::Kind::Paragraph:
    case BlockSpan::Kind::Heading:
    case BlockSpan::Kind::List:
    case BlockSpan::Kind::Quote:
        return false;
    default:
        return true;
    }
}

/// The syntax the caret uncovers in its own block, the way Obsidian reveals it:
/// the markers of the inline element the caret is inside or touching - bold,
/// a link, inline code, a formula - and the line-level markers of the caret's
/// line, a heading's `#` or a quote's `>`. Everything else stays rendered, so
/// moving the caret through a paragraph no longer turns the whole of it raw.
///
/// A list bullet is the exception: it stays drawn unless the caret is inside
/// the marker itself. Obsidian's exact rule for bullets was not checked, and
/// this is the choice that changes least under the caret.
///
/// Worked from the caret outward - the styles that contain it, the syntax on
/// its line - never a scan of the whole block, because a long list is one
/// block.
const CodeFenceSpan *MarkdownEditor::fenceAroundCaret() const
{
    if (d->m_mode != Mode::LivePreview || d->m_decorations.codeFences.isEmpty())
        return nullptr;
    const QList<CodeFenceSpan> &fences = d->m_decorations.codeFences;
    auto it = std::lower_bound(fences.cbegin(), fences.cend(), d->m_cursor,
                               [](const CodeFenceSpan &fence, qsizetype at) { return fence.block.end < at; });
    return it != fences.cend() && it->depth == 0 && it->block.start <= d->m_cursor ? &*it : nullptr;
}

QList<Range> MarkdownEditor::revealedSyntax(const BlockView &view) const
{
    QList<Range> revealed;
    const qsizetype caret = d->m_cursor;
    // The caret anywhere in a fenced listing opens both fence lines, which
    // md4qt leaves in the blocks either side of the listing: Obsidian shows
    // the opening and closing fence (and the language) as one unit.
    if (view.span.kind == BlockSpan::Kind::Paragraph) {
        if (const CodeFenceSpan *fence = fenceAroundCaret(); fence && view.span.range.overlaps(fence->block)) {
            for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
                if (d->m_decorations.syntax.at(i).overlaps(fence->block))
                    revealed.append(d->m_decorations.syntax.at(i));
            }
            return revealed;
        }
    }
    if (!view.span.range.contains(caret) && caret != view.span.range.end)
        return revealed;

    // An entity the caret touches shows as written (it is not syntax the
    // parser lists, so the markers below never find it).
    revealed = revealedEntities();
    // So do the tags of an inline HTML element the caret touches,
    // `H<sub>2</sub>O`'s pair as a whole, as Obsidian shows them.
    for (const InlineHTMLpair &pair : inlineHTMLpairs(view)) {
        if (caret >= pair.open.start && caret <= pair.close.end)
            revealed << pair.open << pair.close;
    }

    const qsizetype line = d->m_lines.lineAt(caret);
    const qsizetype lineStart = d->m_lines.lineStart(line);
    const qsizetype lineEnd = d->m_lines.lineEnd(line);

    // The markers on the caret's line, found by binary search rather than by
    // walking the block from its start - a long list is a single block.
    const auto syntaxBegin = d->m_decorations.syntax.cbegin() + view.firstSyntax;
    const auto syntaxEnd = d->m_decorations.syntax.cbegin() + view.lastSyntax;
    QList<Range> markers;
    for (auto it = std::lower_bound(syntaxBegin, syntaxEnd, lineStart,
                                    [](const Range &marker, qsizetype at) {
                                        return marker.start < at;
                                    });
         it != syntaxEnd && it->start <= lineEnd; ++it)
        markers.append(*it);
    if (markers.isEmpty())
        return revealed;

    const auto isInline = [](StyleSpan::Kind kind) {
        return kind != StyleSpan::Kind::Heading && kind != StyleSpan::Kind::Quote
               && kind != StyleSpan::Kind::CodeBlock;
    };
    const auto touches = [](const Range &a, const Range &b) {
        return a.end >= b.start && b.end >= a.start;
    };

    // Each inline element on the line: its span, grown by the markers that
    // touch it. The parser's range for `**bold**` is the word alone, with the
    // asterisks beside it, so ownership is adjacency - and transitive, so the
    // outer `**` of `***both***` joins through the inner `*`.
    std::vector<bool> owned(size_t(markers.size()), false);
    const auto stylesBegin = d->m_decorations.styles.cbegin() + view.firstStyle;
    const auto stylesEnd = d->m_decorations.styles.cbegin() + view.lastStyle;
    for (auto it = std::lower_bound(stylesBegin, stylesEnd, lineStart,
                                    [](const StyleSpan &span, qsizetype at) {
                                        return span.range.start < at;
                                    });
         it != stylesEnd && it->range.start <= lineEnd; ++it) {
        if (!isInline(it->kind))
            continue;
        Range extent = it->range;
        std::vector<int> members;
        for (bool grew = true; grew;) {
            grew = false;
            for (int i = 0; i < markers.size(); ++i) {
                if (std::find(members.cbegin(), members.cend(), i) != members.cend()
                    || !touches(markers.at(i), extent))
                    continue;
                members.push_back(i);
                extent = {qMin(extent.start, markers.at(i).start),
                          qMax(extent.end, markers.at(i).end)};
                grew = true;
            }
        }
        const bool caretInside = caret >= extent.start && caret <= extent.end;
        for (int i : members) {
            owned[size_t(i)] = true;
            if (caretInside && !revealed.contains(markers.at(i)))
                revealed.append(markers.at(i));
        }
    }

    // What no inline element owns is a line-level marker: a heading's `#`, a
    // quote's `>`. Those show for the caret's line - except a list bullet,
    // which stays drawn unless the caret is inside the marker itself.
    for (int i = 0; i < markers.size(); ++i) {
        if (owned[size_t(i)])
            continue;
        const Range &marker = markers.at(i);
        if (view.span.kind == BlockSpan::Kind::List) {
            const std::span<const ListItemSpan> items = listItemsIn(view.span.range);
            const auto item = std::find_if(items.begin(), items.end(),
                                           [&marker](const ListItemSpan &list) {
                                               return list.marker.overlaps(marker);
                                           });
            if (item != items.end()) {
                if (caret >= item->marker.start && caret < item->marker.end)
                    revealed.append(marker);
                continue;
            }
        }
        revealed.append(marker);
    }
    return revealed;
}

/// The body of every folded callout nested in this block, as ranges to hide.
/// A nested callout lives inside its parent's layout, so folding one is just
/// hiding its lines like syntax - its title stays, to be clicked open again.
void MarkdownEditor::hideFoldedNestedCallouts(const BlockView &view, QList<Range> &hidden) const
{
    for (const CalloutSpan &callout : d->m_decorations.callouts) {
        if (callout.depth == 0 || !callout.foldable
            || !view.span.range.contains(callout.range.start)
            || !d->m_folds.value(callout.range.start, callout.collapsed))
            continue;
        const qsizetype from = callout.titleRange.isValid() ? callout.titleRange.end
                                                            : callout.range.start;
        const qsizetype titleEnd = d->m_text.indexOf(QLatin1Char('\n'), from);
        if (titleEnd < 0 || titleEnd + 1 >= callout.range.end)
            continue;
        qsizetype end = callout.range.end;
        // Take the body's own last newline too, or it is left behind as an
        // empty line under the folded title.
        if (d->m_text.at(end - 1) != QLatin1Char('\n') && end < d->m_text.size()
            && d->m_text.at(end) == QLatin1Char('\n'))
            ++end;
        hidden.append({titleEnd + 1, end});
    }
}

QString MarkdownEditor::displayText() const
{
    // Blocks off screen carry no layout and no map, so this builds what they
    // would show. Test and debug helper only; painting never calls it.
    QStringList parts;
    parts.reserve(int(d->m_blocks.size()));
    for (const BlockView &view : d->m_blocks) {
        if (view.foldHidden)
            continue;
        if (view.laidOut) {
            parts << view.map.text();
            continue;
        }
        const bool reveal = d->m_mode == Mode::Source
                            || (d->m_mode == Mode::LivePreview && view.active);
        if (reveal) {
            QList<Range> folded;
            hideFoldedListItems(view, folded);
            parts << DisplayMap::build(d->m_text, view.span.range, folded, {}).text();
            continue;
        }
        QList<Range> hidden;
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i)
            hidden.append(d->m_decorations.syntax.at(i));
        hideFoldedNestedCallouts(view, hidden);
        hideFoldedListItems(view, hidden);
        parts << DisplayMap::build(d->m_text, view.span.range, hidden, d->m_decorations.substitutions).text();
    }
    return parts.join(QString());
}

QTextCharFormat MarkdownEditor::formatAt(qsizetype sourceOffset) const
{
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0 || !d->m_blocks[size_t(index)].layout)
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    const qsizetype shown = view.map.toDisplay(sourceOffset);
    QTextCharFormat merged;
    for (const QTextLayout::FormatRange &range : view.layout->formats()) {
        if (range.start <= shown && shown < range.start + range.length)
            merged.merge(range.format);
    }
    return merged;
}

void MarkdownEditor::enterBlockAt(qsizetype offset)
{
    d->m_interacted = true;
    moveCursor(offset, false);
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setWordCount(int words)
{
    if (words == d->m_wordCount)
        return;
    d->m_wordCount = words;
    emit wordCountChanged(words);
}

// --- parsing and block construction --------------------------------------

namespace {

/// One parser per worker thread: md4qt parsers are cheap but not shareable.
Parser &threadParser()
{
    static thread_local Parser parser;
    return parser;
}

/// Sorted once, so every later lookup is a binary search instead of a walk
/// over the whole document.
void sortDecorations(Decorations &decorations)
{
    // Sorted so a list can find its own items by binary search: rebuildBlocks
    // splits every list at its items, and scanning all of a note's items for
    // each list would make that quadratic in a note full of lists.
    std::sort(decorations.listItems.begin(), decorations.listItems.end(),
              [](const ListItemSpan &l, const ListItemSpan &r) {
                  return l.range.start < r.range.start;
              });
    std::sort(decorations.syntax.begin(), decorations.syntax.end(),
              [](const Range &l, const Range &r) { return l.start < r.start; });
    // Stable: `***both***` is a bold and an italic range from one offset, and
    // which colour wins depends on their order (docs/STYLING.md).
    std::stable_sort(decorations.styles.begin(), decorations.styles.end(),
                     [](const StyleSpan &l, const StyleSpan &r) { return l.range.start < r.range.start; });
    // The parser's (Markdown entities) and the HTML pass's, appended after.
    std::sort(decorations.substitutions.begin(), decorations.substitutions.end(),
              [](const Substitution &l, const Substitution &r) { return l.range.start < r.range.start; });
}

} // namespace

void MarkdownEditor::reparse()
{
    d->m_parseTimer.stop();
    // A pool parse still out has the text as it was: let it end unadopted, so
    // its older decorations are not laid over this text for a frame.
    const bool dropped = d->m_parsePending;
    if (d->m_parseWatcher.isRunning())
        d->m_parseWatcher.waitForFinished();
    d->m_parsePending = false;
    ++d->m_parses;
    QElapsedTimer clock;
    clock.start();
    Parser::Result result = d->m_parser.parseAll(d->m_text, {}, d->m_parseOptions);
    if (d->m_features.HTMLblocks)
        expandEditableHTML(result.decorations, d->m_text);
    sortDecorations(result.decorations);
    // The parse alone, as the pool job times it: the layout that follows is not
    // what the next newline's budget is about.
    d->m_lastParseNs = clock.nsecsElapsed();
    d->m_parsedVersion = d->m_textVersion;
    setWordCount(result.metadata.wordCount);
    landDecorations(d->m_textVersion, std::move(result.decorations), std::move(result.metadata.properties),
                    std::move(result.metadata.headings));
    if (dropped && !d->m_pendingFolds.isEmpty()) // parked for the parse that was dropped
        applyFoldState(std::exchange(d->m_pendingFolds, {}));
}

int MarkdownEditor::parseCountForTest() const
{
    return d->m_parses;
}

void MarkdownEditor::setLastParseTimeForTest(qint64 nanoseconds)
{
    d->m_lastParseNs = nanoseconds;
}

void MarkdownEditor::finishParse()
{
    if (d->m_parseWatcher.isRunning())
        d->m_parseWatcher.waitForFinished(); // blocks the thread, dispatches nothing
    adoptParse();
}

void MarkdownEditor::adoptParse()
{
    // The finished signal still arrives, queued, after finishParse() took the
    // result; a parse adopted once is not adopted twice.
    if (!d->m_parsePending || d->m_parseWatcher.future().resultCount() == 0)
        return;
    d->m_parsePending = false;
    ++d->m_parses;
    ParseJob job = d->m_parseWatcher.result();
    d->m_lastParseNs = job.nanoseconds;
    setWordCount(job.words);
    landDecorations(job.version, std::move(job.decorations), std::move(job.properties), std::move(job.headings));
    d->m_parsedVersion = job.version;
    // The buffer moved on while we were parsing: go round again.
    if (d->m_parsedVersion != d->m_textVersion)
        startParse();
    else if (!d->m_pendingFolds.isEmpty())
        applyFoldState(std::exchange(d->m_pendingFolds, {}));
}

void MarkdownEditor::startParse()
{
    if (d->m_parseWatcher.isRunning())
        return; // the finished handler starts the next one
    d->m_parsePending = true;

    const QString snapshot = d->m_text;
    const quint64 version = d->m_textVersion;
    const bool HTML = d->m_features.HTMLblocks;
    const ParseOptions options = d->m_parseOptions;
    d->m_parseWatcher.setFuture(QtConcurrent::run([snapshot, version, HTML, options] {
        QElapsedTimer clock;
        clock.start();
        Parser::Result result = threadParser().parseAll(snapshot, {}, options);
        if (HTML)
            expandEditableHTML(result.decorations, snapshot);
        ParseJob job;
        job.decorations = std::move(result.decorations);
        job.properties = std::move(result.metadata.properties);
        job.headings = std::move(result.metadata.headings);
        job.words = result.metadata.wordCount;
        sortDecorations(job.decorations);
        job.version = version;
        job.nanoseconds = clock.nsecsElapsed();
        return job;
    }));
}

void MarkdownEditor::placeLineAt(qsizetype offset, qreal y)
{
    if (d->m_blocks.empty())
        return;
    QScrollBar *bar = verticalScrollBar();
    // The block's top first: moving there lays out what is around it, and the layouts
    // settle heights that were estimates, so the line's own place is known only then, and
    // moves again as the blocks above it come into range. A few passes, each of them a step.
    bar->setValue(int(d->m_blocks.at(size_t(blockAt(offset))).y));
    for (int pass = 0; pass < 6; ++pass) {
        const int step = int(std::lround(pointFor(offset).y() - y));
        const int before = bar->value();
        if (step == 0)
            break;
        bar->setValue(before + step);
        if (bar->value() == before)
            break;
    }
}

qsizetype MarkdownEditor::topOffset() const
{
    if (verticalScrollBar()->value() <= 0)
        return 0;
    // Two pixels in: a line placed at the very top lands within a pixel of it, rounding
    // either way, and row 0 would then belong to the line above. A line with only those
    // few pixels left in view is as good as gone.
    const qsizetype offset = offsetAt(QPoint(contentLeft(), 2));
    // That line is as good as gone: the one under it shows. Inside a block it is the block's
    // next line; the gap between two blocks answers with the end of the block above again, and
    // then the text at the top is the next block's.
    qreal height = 0;
    const QPointF point = pointFor(offset, &height);
    const qsizetype index = blockAt(offset);
    if (point.y() + height > 3 || index < 0)
        return offset;
    const qsizetype below = offsetAt(QPoint(contentLeft(), int(point.y() + height) + 1));
    if (below > offset)
        return below;
    for (size_t next = size_t(index) + 1; next < d->m_blocks.size(); ++next) {
        if (!d->m_blocks.at(next).foldHidden)
            return d->m_blocks.at(next).span.range.start;
    }
    return offset;
}

void MarkdownEditor::restoreScroll(qsizetype top, int pixel)
{
    if (top > 0 && d->m_placeholderVersion != 0)
        placeLineAt(qMin(top, d->m_text.size()), 0);
    else
        verticalScrollBar()->setValue(pixel);
}

void MarkdownEditor::landDecorations(quint64 version, Decorations decorations, QList<Property> properties,
                                     QList<Heading> headings)
{
    // A big note's chunks are estimated a line over each and the parse's blocks a line over
    // each of theirs, which is more, so the pixel the reader was at would show other text
    // once they are replaced: the text stays, whichever note's parse this is.
    const bool placeholder = d->m_placeholderVersion != 0;
    const qsizetype keep = placeholder && verticalScrollBar()->value() > 0 ? topOffset() : -1;
    const qreal keepY = keep >= 0 ? pointFor(keep).y() : 0;
    adoptDecorations(std::move(decorations), std::move(properties), std::move(headings));
    if (keep >= 0)
        placeLineAt(keep, keepY);
    // The trim belongs to the note's own parse: one that left before it (the text was
    // replaced while it ran) is adopted, and the next landing is the one.
    if (placeholder && version >= d->m_placeholderVersion) {
        d->m_placeholderVersion = 0;
        if (bigParseLandedHook())
            bigParseLandedHook()();
    }
}

void MarkdownEditor::adoptDecorations(Decorations decorations, QList<Property> properties,
                                      QList<Heading> headings)
{
    // Every block comes back as an estimate, and only the ones near the view are
    // then laid out for real, so everything below a block far above the view
    // can move by the difference between the two (an estimate counts a
    // paragraph's own newline as a second line): the text on screen would jump
    // by hundreds of pixels each time a parse lands. The block at the top of
    // the view, and how far into it the view starts, is what stays put.
    QScrollBar *bar = verticalScrollBar();
    qsizetype anchorStart = -1;
    qreal anchorInset = 0;
    if (const auto top = std::partition_point(d->m_blocks.cbegin(), d->m_blocks.cend(),
                                              [scroll = qreal(bar->value())](const BlockView &view) {
                                                  return view.bottomY() <= scroll;
                                              });
        top != d->m_blocks.cend() && !top->foldHidden) {
        anchorStart = top->span.range.start;
        anchorInset = bar->value() - top->y;
    }
    d->m_decorations = std::move(decorations);
    d->m_properties = std::move(properties);
    d->m_headings = std::move(headings);
    d->m_lines = SourceMap(d->m_text);
    collectTabGroups();
    rebuildBlocks();
    indexDecorations();
    invalidateLayouts();
    updateLayout();
    // Laying out around the new position gives the blocks above it their real
    // heights, which moves the anchor again: a few passes settle it.
    for (int pass = 0; pass < 4 && anchorStart >= 0; ++pass) {
        const BlockView &anchor = d->m_blocks.at(size_t(blockAt(anchorStart)));
        const int wanted = int(anchor.y + anchorInset);
        if (anchor.foldHidden || wanted == bar->value())
            break;
        bar->setValue(wanted);
        updateLayout();
    }
    viewport()->update();
    emit headingsChanged();
    emit footnotesChanged();
}

void MarkdownEditor::indexDecorations()
{
    d->m_unresolved.clear();
    // One merge pass: each block records the slice of the sorted decoration
    // lists that can possibly touch it.
    const auto assign = [](auto &items, auto start, int &first, int &last,
                           const Range &block, int &walker) {
        while (walker > 0 && start(items.at(walker - 1)) >= block.start)
            --walker;
        while (walker < items.size() && start(items.at(walker)) < block.start)
            ++walker;
        first = walker;
        int end = walker;
        while (end < items.size() && start(items.at(end)) < block.end)
            ++end;
        last = end;
    };

    int syntaxWalker = 0;
    int styleWalker = 0;
    for (BlockView &view : d->m_blocks) {
        assign(d->m_decorations.syntax, [](const Range &r) { return r.start; },
               view.firstSyntax, view.lastSyntax, view.span.range, syntaxWalker);
        assign(d->m_decorations.styles, [](const StyleSpan &s) { return s.range.start; },
               view.firstStyle, view.lastStyle, view.span.range, styleWalker);
    }
}

void MarkdownEditor::invalidateLayouts()
{
    d->m_layoutWidth = contentWidth();
    for (BlockView &view : d->m_blocks) {
        if (view.touched) // an untouched view has no layout, and its cache line is not worth a visit
            view.layout.reset();
        view.laidOut = false;
        view.heightEstimated = false;
        view.height = 0;
    }
}

void MarkdownEditor::invalidateBlock(qsizetype index)
{
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return;
    // Every piece of a long fence or callout: they are laid out as one.
    const qsizetype first = index - d->m_blocks[size_t(index)].piece;
    const qsizetype end = qMin<qsizetype>(qsizetype(d->m_blocks.size()), first + d->m_blocks[size_t(first)].pieces);
    for (qsizetype i = first; i < end; ++i) {
        BlockView &view = d->m_blocks[size_t(i)];
        view.layout.reset();
        view.laidOut = false;
        view.heightEstimated = false;
    }
}

qreal MarkdownEditor::lineHeightFor(const BlockSpan &span) const
{
    // Building a QFont and its metrics per block per layout pass is what made
    // a keystroke cost tens of milliseconds on a long note.
    const int key = int(span.kind) * 16 + span.level;
    const auto cached = d->m_lineHeights.constFind(key);
    if (cached != d->m_lineHeights.constEnd())
        return *cached;

    // A CSS line-height is a multiple of the font *size*, not of the font's
    // own metric height, which is what made every line here about a quarter
    // taller than the same line in Obsidian. Obsidian's numbers:
    // --line-height-normal 1.5, and headings tighter the larger they are.
    static constexpr qreal headingRatios[] = {1.5, 1.2, 1.2, 1.3, 1.4, 1.5, 1.5};
    const qreal ratio = span.kind == BlockSpan::Kind::Heading
                            ? headingRatios[qBound(0, span.level, 6)]
                            : 1.5;
    const QFont font = fontFor(span);
    const qreal height = font.pixelSize() > 0 ? font.pixelSize() * ratio
                                              : QFontMetricsF(font).height() * ratio;
    d->m_lineHeights.insert(key, height);
    return height;
}

qreal MarkdownEditor::blockSpacing() const
{
    if (d->m_blockSpacing <= 0) {
        const int size = d->m_theme.textFont().pixelSize();
        d->m_blockSpacing = size > 0 ? size : 16;
    }
    return d->m_blockSpacing;
}

qreal MarkdownEditor::estimatedHeight(const BlockView &view) const
{
    // Lines in the source, plus an allowance for wrapping, is close enough
    // for a scrollbar; the real height arrives when the block scrolls in.
    const qsizetype lines =
        QStringView(d->m_text).mid(view.span.range.start, view.span.range.length())
            .count(QLatin1Char('\n'))
        + 1;
    return lines * lineHeightFor(view.span);
}

/// Paragraph blocks split on blank lines, for a note whose parse has not come
/// back yet. Without this an unparsed note is one block a megabyte long, and
/// laying that out as a single QTextLayout costs more than the parse did.
/// A note over kAsyncParseThreshold is cut into chunks instead: its 1 MB is 40 000
/// paragraphs and gaps, 1.8 kB of BlockView each, 75 MB for the half second until the
/// parse lands. A chunk is laid out as one multi-line paragraph, the same lines at the
/// same heights, and ends after kPlaceholderLines lines or kPlaceholderChars characters
/// (a keystroke relays its whole chunk). It owns the newline that ends it, which
/// estimatedHeight() counts as a line more.
QList<BlockSpan> MarkdownEditor::plainParagraphs() const
{
    QList<BlockSpan> spans;
    qsizetype start = 0;
    const qsizetype size = d->m_text.size();
    if (size > kAsyncParseThreshold) {
        while (start < size) {
            qsizetype end = start;
            for (int line = 0; line < kPlaceholderLines && end < size; ++line) {
                const qsizetype newline = d->m_text.indexOf(QLatin1Char('\n'), end);
                const qsizetype next = newline < 0 ? size : newline + 1;
                if (end > start && next - start > kPlaceholderChars)
                    break; // this line starts the next chunk; one over the cap alone is a chunk
                end = next;
            }
            spans.append({BlockSpan::Kind::Paragraph, {start, end}, 0});
            start = end;
        }
        return spans;
    }
    for (qsizetype i = 0; i < size; ++i) {
        if (d->m_text.at(i) != QLatin1Char('\n'))
            continue;
        // A blank line ends a paragraph, exactly as Markdown says.
        if (i + 1 < size && d->m_text.at(i + 1) == QLatin1Char('\n')) {
            if (i > start)
                spans.append({BlockSpan::Kind::Paragraph, {start, i}, 0});
            start = i + 2;
            ++i;
        }
    }
    if (start < size)
        spans.append({BlockSpan::Kind::Paragraph, {start, size}, 0});
    return spans;
}

void MarkdownEditor::rebuildBlocks()
{
    QList<BlockSpan> spans =
        d->m_decorations.blocks.isEmpty() && !d->m_text.isEmpty() ? plainParagraphs()
                                                            : d->m_decorations.blocks;
    const auto byStart = [](const BlockSpan &l, const BlockSpan &r) { return l.range.start < r.range.start; };
    if (!std::is_sorted(spans.cbegin(), spans.cend(), byStart)) // the parser's own order, nearly always
        std::sort(spans.begin(), spans.end(), byStart);

    // The spans are cut up first and the views built once, in a vector sized
    // for them: a BlockView is 1.6 kB, and growing a vector of them moved
    // every one built so far, a fifth of opening a 1 MB note.
    struct Part {
        BlockSpan span;
        int piece = 0;
        int pieces = 1;
    };
    std::vector<Part> parts;
    parts.reserve(spans.size() * 2 + 1);
    qsizetype cursor = 0;
    const auto addGap = [&](qsizetype from, qsizetype to) {
        if (to > from)
            parts.push_back({{BlockSpan::Kind::Paragraph, {from, to}, 0}});
    };

    for (const BlockSpan &span : std::as_const(spans)) {
        if (!span.range.isValid() || span.range.start < cursor)
            continue; // nested or overlapping: the outer block owns it
        addGap(cursor, span.range.start);

        Part view{span};
        // A block owns the newline that ends it, so the blank lines left over
        // in the gap are exactly the blank lines the file has. A range that
        // already ends in one (a `:::` container's does) has its own; taking
        // the next as well swallowed the blank line between two containers,
        // and their panels, which reach past the block, painted over each other.
        if (view.span.range.end < d->m_text.size()
            && d->m_text.at(view.span.range.end) == QLatin1Char('\n')
            && !(view.span.range.end > view.span.range.start
                 && d->m_text.at(view.span.range.end - 1) == QLatin1Char('\n')))
            ++view.span.range.end;

        // A list becomes one block per top-level item, its nested items going
        // with their parent. As one block, a list was laid out whole on every
        // keystroke and every caret move into an element: 11.6 ms per keystroke
        // at 1 000 items, over the 8 ms budget, and 154 ms at 5 000. Blocks sit
        // with no spacing between them, so nothing on screen changes.
        if (span.kind == BlockSpan::Kind::List) {
            const auto items = d->m_decorations.listItems;
            auto item = std::lower_bound(items.cbegin(), items.cend(), span.range.start,
                                         [](const ListItemSpan &list, qsizetype at) {
                                             return list.range.start < at;
                                         });
            qsizetype from = view.span.range.start;
            for (; item != items.cend() && item->range.start < view.span.range.end; ++item) {
                if (item->indent != 0)
                    continue;
                // md4qt starts an item at its bullet; the block starts at its line.
                const qsizetype lineStart = d->m_lines.lineStart(d->m_lines.lineAt(item->range.start));
                if (lineStart <= from)
                    continue;
                parts.push_back({{BlockSpan::Kind::List, {from, lineStart}, span.level}});
                from = lineStart;
            }
            view.span.range.start = from;
        }
        // A long fence becomes pieces of kCodePieceLines lines, for the same
        // reason. Its chrome is drawn across them, and each piece carries on
        // highlighting from the state the one before it ended in.
        // A long callout too: a listing inside one was part of its single
        // layout, 89 ms a keystroke at 2 000 lines. Its panel is drawn across
        // the pieces, its title on the first.
        if (span.kind == BlockSpan::Kind::Code || span.kind == BlockSpan::Kind::Callout) {
            const qsizetype firstLine = d->m_lines.lineAt(view.span.range.start);
            const qsizetype lastLine =
                d->m_lines.lineAt(qMax(view.span.range.start, view.span.range.end - 1));
            const int pieces = int((lastLine - firstLine) / kCodePieceLines) + 1;
            qsizetype from = view.span.range.start;
            for (int piece = 0; piece + 1 < pieces; ++piece) {
                const qsizetype to = d->m_lines.lineStart(firstLine + (piece + 1) * kCodePieceLines);
                parts.push_back({{span.kind, {from, to}, span.level}, piece, pieces});
                from = to;
            }
            view.span.range.start = from;
            view.piece = pieces - 1;
            view.pieces = pieces;
        }
        cursor = view.span.range.end;
        parts.push_back(view);
    }
    addGap(cursor, d->m_text.size());

    // Every parse rebuilds the blocks. A piece keeps the highlighter state the
    // old piece ending where it ends had, so a listing is not re-highlighted
    // from its top to show the piece on screen - and that state is still
    // valid when the old piece covered exactly the same text.
    // A fence, table or CSV embed scrolled sideways keeps its offset (the first piece
    // holds it), or typing in a wide listing would snap it back every parse.
    const auto oldBlockAt = [this](qsizetype at) {
        auto it = std::upper_bound(d->m_blocks.cbegin(), d->m_blocks.cend(), at,
                                   [](qsizetype offset, const BlockView &old) {
                                       return offset < old.span.range.start;
                                   });
        return it == d->m_blocks.cbegin() ? d->m_blocks.cend() : it - 1;
    };
    const auto fillView = [&](BlockView &view, const Part &part) {
        view.span = part.span;
        view.piece = part.piece;
        view.pieces = part.pieces;
        view.bleedAbove = panelBleed(view, false);
        view.bleedBelow = panelBleed(view, true);
        if (view.piece == 0) {
            const auto old = oldBlockAt(view.span.range.start);
            // a CSV embed is a paragraph by kind: what it was laid out as (isBase) says it is a table
            const bool scrolls = view.span.kind == BlockSpan::Kind::Code || view.span.kind == BlockSpan::Kind::Table
                                 || (old != d->m_blocks.cend() && old->isBase && !old->CSVpath.isEmpty());
            if (scrolls && old != d->m_blocks.cend() && old->span.range.start == view.span.range.start)
                view.sideScroll = old->sideScroll;
        }
        if (view.pieces > 1) {
            auto it = oldBlockAt(view.span.range.end - 1);
            if (it != d->m_blocks.cend() && it->pieces > 1 && it->span.range.end == view.span.range.end) {
                view.codeEnd = it->codeEnd;
                view.codeEndValid = it->codeEndValid && it->piece == view.piece
                                    && it->span.range.start == view.span.range.start;
            }
        }
    };

    const bool footnotesList = d->m_mode == Mode::Reading && !d->m_decorations.footnoteDefinitions.isEmpty();
    if (!parts.empty() && !footnotesList && parts.size() == d->m_blocks.size()) {
        // The same blocks as before, as after most pauses in typing: a view
        // nothing has laid out or highlighted yet is a default one with its
        // span (the invariant on BlockView::touched), so it stays where it is,
        // reset, and only the blocks that changed - or that were ever drawn -
        // are made again. Rebuilding all of them cost 77 % of a parse landing,
        // 10 ms of a megabyte's 13.
        std::vector<std::size_t> changed;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const BlockView &old = d->m_blocks[i];
            const Part &part = parts[i];
            if (old.touched || old.isFootnotesList || old.piece != part.piece || old.pieces != part.pieces
                || old.span.kind != part.span.kind || old.span.level != part.span.level
                || !(old.span.range == part.span.range))
                changed.push_back(i);
        }
        // More than half of them made again (a note scrolled through end to end
        // has every view laid out) is the full path's work: it moves each view
        // once, this would move it twice.
        if (changed.size() * 2 <= parts.size()) {
            // Made before any old view is touched: they look the old ones up.
            std::vector<BlockView> fresh(changed.size());
            for (std::size_t k = 0; k < changed.size(); ++k)
                fillView(fresh[k], parts[changed[k]]);
            std::size_t next = 0;
            for (std::size_t i = 0; i < d->m_blocks.size(); ++i) {
                BlockView &view = d->m_blocks[i];
                if (next < changed.size() && changed[next] == i) {
                    view = std::move(fresh[next++]);
                    continue;
                }
                view.y = 0;
                view.height = 0;
                view.active = false;
                view.laidOut = false;
                view.heightEstimated = false;
                view.foldHidden = false;
                view.editedAsSource = false;
                view.editedListing = false;
            }
            d->m_rebuiltViews += changed.size();
            d->m_activeBlock = blockAt(d->m_cursor);
            return;
        }
    }
    d->m_rebuiltViews += parts.size();

    // Sized once, here, for the parts and a footnotes list: a BlockView is 1.6 kB,
    // and moving each into place cost a seventh of opening a 1 MB note.
    std::vector<BlockView> blocks;
    blocks.reserve(parts.size() + 1);
    blocks.resize(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i)
        fillView(blocks[i], parts[i]);

    if (blocks.empty()) {
        BlockView view;
        view.span = {BlockSpan::Kind::Paragraph, {0, d->m_text.size()}, 0};
        blocks.push_back(std::move(view));
    }

    // Reading view lists footnote definitions at the end instead of where
    // they sit in the source (see layoutBlock's Mode::Reading branch above)
    // - a block with no real source range, appended after everything else.
    if (footnotesList) {
        BlockView view;
        view.span = {BlockSpan::Kind::Paragraph, {d->m_text.size(), d->m_text.size()}, 0};
        view.isFootnotesList = true;
        blocks.push_back(std::move(view));
    }

    d->m_blocks = std::move(blocks);
    d->m_activeBlock = blockAt(d->m_cursor);
}

QColor MarkdownEditor::linkColourAt(qsizetype sourceOffset) const
{
    for (const StyleSpan &span : d->m_decorations.styles) {
        if (span.kind != StyleSpan::Kind::Link || !span.range.contains(sourceOffset))
            continue;
        return linkColourFor(span.payload);
    }
    return {};
}

QColor MarkdownEditor::linkColourFor(const QString &target) const
{
    if (isUnresolvedLink(target))
        return d->m_theme.color(QStringLiteral("--link-unresolved-color"));
    const bool external = target.contains(QLatin1String("://")) || target.startsWith(QLatin1String("mailto:"));
    return d->m_theme.color(external ? QStringLiteral("--link-external-color")
                                     : QStringLiteral("--link-color"));
}

bool MarkdownEditor::isUnresolvedLink(const QString &target) const
{
    if (target.isEmpty() || !m_resolver)
        return false;
    // External links resolve to nothing and are not meant to.
    if (target.contains(QLatin1String("://")) || target.startsWith(QLatin1String("mailto:")))
        return false;

    const auto cached = d->m_unresolved.constFind(target);
    if (cached != d->m_unresolved.constEnd())
        return *cached;

    const bool missing = resolvesToNothing(target);
    d->m_unresolved.insert(target, missing);
    return missing;
}

bool MarkdownEditor::resolvesToNothing(const QString &target) const
{
    // The subpath is not part of what resolves to a file.
    QString name = target;
    const qsizetype hash = name.indexOf(QLatin1Char('#'));
    if (hash >= 0)
        name.truncate(hash);
    return !name.isEmpty() && m_resolver(name).isEmpty();
}

void MarkdownEditor::refreshLinkStatus()
{
    // Only the targets already drawn are asked again, and only a link that
    // changed sides costs a relayout - the index changes on every save.
    if (!m_resolver)
        return;
    QSet<QString> flipped;
    for (auto it = d->m_unresolved.begin(); it != d->m_unresolved.end(); ++it) {
        const bool missing = resolvesToNothing(it.key());
        if (missing != it.value())
            flipped.insert(it.key());
        it.value() = missing;
    }
    if (flipped.isEmpty())
        return;
    // Only the blocks with one of those links in them change colour.
    for (const StyleSpan &span : std::as_const(d->m_decorations.styles)) {
        if (span.kind == StyleSpan::Kind::Link && flipped.contains(span.payload))
            invalidateBlock(blockAt(span.range.start));
    }
    updateLayout();
    viewport()->update();
}

qsizetype MarkdownEditor::blockAt(qsizetype sourceOffset) const
{
    if (d->m_blocks.empty())
        return -1;

    // Blocks are sorted and cover the document without gaps.
    auto it = std::upper_bound(d->m_blocks.cbegin(), d->m_blocks.cend(), sourceOffset,
                               [](qsizetype offset, const BlockView &view) {
                                   return offset < view.span.range.start;
                               });
    const qsizetype index = qsizetype(std::distance(d->m_blocks.cbegin(), it)) - 1;
    return qBound<qsizetype>(0, index, qsizetype(d->m_blocks.size()) - 1);
}

QFont MarkdownEditor::fontFor(const BlockSpan &span) const
{
    // A table only ever reaches here as its source, and its pipes line up
    // only in a font where every character is as wide as the next.
    if (span.kind == BlockSpan::Kind::Code || span.kind == BlockSpan::Kind::Frontmatter
        || span.kind == BlockSpan::Kind::Table) {
        QFont font = d->m_theme.monospaceFont();
        font.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-small")).toInt() + 1);
        return font;
    }

    QFont font = d->m_theme.textFont();
    if (span.kind == BlockSpan::Kind::Heading) {
        font.setPixelSize(int(font.pixelSize() * headingScale(span.level)));
        font.setWeight(QFont::Bold);
    }
    return font;
}

QList<QTextLayout::FormatRange> MarkdownEditor::formatsFor(BlockView &view)
{
    QList<QTextLayout::FormatRange> formats = formatsIn(view, view.span.range, view.map);
    formats += codeFormatsFor(view);
    appendSpellingFormats(view, formats);
    return formats;
}

void MarkdownEditor::setSpelling(Spelling spelling)
{
    d->m_spelling = std::move(spelling);
    refreshSpelling();
}

void MarkdownEditor::refreshSpelling()
{
    // Only the underlines change, never a height: zeroing the heights, as
    // invalidateLayouts() does, re-estimated every block above the view and
    // threw the scroll position back to the top of the block.
    for (BlockView &view : d->m_blocks) {
        view.layout.reset();
        view.laidOut = false;
    }
    updateLayout();
    viewport()->update();
}

QList<Range> MarkdownEditor::checkedWordsOf(const BlockView &view) const
{
    QList<Range> words;
    switch (view.span.kind) {
    case BlockSpan::Kind::Paragraph:
    case BlockSpan::Kind::Heading:
    case BlockSpan::Kind::List:
    case BlockSpan::Kind::Quote:
    case BlockSpan::Kind::Callout:
        break;
    default:
        return words; // code, maths, tables, properties: not prose
    }
    if (d->m_mode == Mode::Source)
        return words;

    // A run of letters, with apostrophes inside it ("don't"), standing on its
    // own: part of "abc123" or "snake_case" is not a word to check.
    static const QRegularExpression word(QStringLiteral(
        "(?<![\\p{L}\\p{M}\\p{N}_])[\\p{L}\\p{M}]+(?:['\u2019][\\p{L}\\p{M}]+)*"
        "(?![\\p{L}\\p{M}\\p{N}_])"));
    const QString &shown = view.map.text();
    for (auto match = word.globalMatch(shown); match.hasNext();) {
        const QRegularExpressionMatch found = match.next();
        const Range source{view.map.toSource(found.capturedStart()),
                           view.map.toSource(found.capturedEnd())};
        // A hidden marker inside it ("wo**rd**"): replacing it would lose the
        // marker, so it is left alone.
        if (source.length() != found.capturedLength())
            continue;
        // Not the word being typed: it is not finished yet.
        if (view.active && view.caretWord.isValid() && source.overlaps(view.caretWord))
            continue;
        bool skip = false;
        for (int i = view.firstStyle; i < view.lastStyle && !skip; ++i) {
            const StyleSpan &span = d->m_decorations.styles.at(i);
            if (!span.range.overlaps(source))
                continue;
            switch (span.kind) {
            case StyleSpan::Kind::InlineCode:
            case StyleSpan::Kind::CodeBlock:
            case StyleSpan::Kind::Link:
            case StyleSpan::Kind::Tag:
            case StyleSpan::Kind::Math:
            case StyleSpan::Kind::Html:
            case StyleSpan::Kind::Footnote:
                skip = true;
                break;
            default:
                break;
            }
        }
        if (!skip)
            words.append(source);
    }
    return words;
}

Range MarkdownEditor::wordAtCaret() const
{
    if (!d->m_spelling.isCorrect)
        return {};
    return wordAt(d->m_cursor);
}

Range MarkdownEditor::wordAt(qsizetype offset) const
{
    const auto inWord = [](QChar c) {
        return c.isLetterOrNumber() || c.isMark() || c == QLatin1Char('\'')
               || c == QChar(0x2019) || c == QLatin1Char('_');
    };
    qsizetype start = qBound<qsizetype>(0, offset, d->m_text.size());
    qsizetype end = start;
    while (start > 0 && inWord(d->m_text.at(start - 1)))
        --start;
    while (end < d->m_text.size() && inWord(d->m_text.at(end)))
        ++end;
    return start < end ? Range{start, end} : Range{};
}

void MarkdownEditor::appendSpellingFormats(const BlockView &view,
                                           QList<QTextLayout::FormatRange> &formats) const
{
    if (!d->m_spelling.isCorrect)
        return;
    QTextCharFormat underline;
    underline.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    underline.setUnderlineColor(d->m_theme.color(QStringLiteral("--text-error")));
    for (const Range &source : checkedWordsOf(view)) {
        if (d->m_spelling.isCorrect(d->m_text.mid(source.start, source.length())))
            continue;
        const Range shown = view.map.toDisplay(source);
        formats.append({int(shown.start), int(shown.length()), underline});
    }
}

bool MarkdownEditor::addSpellingActions(QMenu &menu, qsizetype sourceOffset, bool personal)
{
    if (!d->m_spelling.isCorrect)
        return false;
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0)
        return false;
    const BlockView &view = d->m_blocks[size_t(index)];
    if (!view.laidOut)
        return false;
    for (const Range &source : checkedWordsOf(view)) {
        if (sourceOffset < source.start || sourceOffset > source.end)
            continue;
        const QString word = d->m_text.mid(source.start, source.length());
        if (d->m_spelling.isCorrect(word)) {
            // A word the reader added: the way back, for a slip of the mouse.
            if (!personal || !d->m_spelling.isPersonal || !d->m_spelling.removeWord || !d->m_spelling.isPersonal(word))
                return false;
            connect(menu.addAction(tr("Remove \"%1\" from dictionary").arg(word)),
                    &QAction::triggered, this, [this, word] {
                        d->m_spelling.removeWord(word);
                        refreshSpelling();
                    });
            return true;
        }

        const QStringList suggestions =
            d->m_spelling.suggestions ? d->m_spelling.suggestions(word) : QStringList();
        for (const QString &suggestion : suggestions) {
            connect(menu.addAction(suggestion), &QAction::triggered, this,
                    [this, source, word, suggestion] {
                        // Only if the word is still there: the menu may have
                        // stayed open over a reparse.
                        if (d->m_text.mid(source.start, source.length()) != word)
                            return;
                        pushUndo();
                        d->m_lastEdit.invalidate();
                        applyEdit(source.start, source.length(), suggestion);
                    });
        }
        if (suggestions.isEmpty())
            menu.addAction(tr("No suggestions"))->setEnabled(false);
        if (d->m_spelling.addWord) {
            menu.addSeparator();
            connect(menu.addAction(tr("Add \"%1\" to dictionary").arg(word)),
                    &QAction::triggered, this, [this, word] {
                        d->m_spelling.addWord(word);
                        refreshSpelling();
                    });
        }
        return true;
    }
    return false;
}

std::unique_ptr<QMenu> MarkdownEditor::spellingMenuAt(qsizetype sourceOffset)
{
    auto menu = std::make_unique<QMenu>(this);
    // Only a misspelt word has a menu of its own; "Remove" is part of contextMenuAt().
    return addSpellingActions(*menu, sourceOffset, false) ? std::move(menu) : nullptr;
}

void MarkdownEditor::setContextActions(ContextActions actions)
{
    d->m_contextActions = std::move(actions);
}

void MarkdownEditor::copySelection()
{
    if (d->m_cellSel.tableStart >= 0 && copyCellSelection(false))
        return;
    if (selection().length() > 0)
        QApplication::clipboard()->setText(d->m_text.mid(selection().start, selection().length()));
}

void MarkdownEditor::cutSelection()
{
    if (d->m_cellSel.tableStart >= 0 && copyCellSelection(true))
        return;
    if (selection().length() > 0) {
        QApplication::clipboard()->setText(d->m_text.mid(selection().start, selection().length()));
        removeSelectionOrChar(0);
    }
}

void MarkdownEditor::pasteClipboard(bool plain)
{
    const TypingScope typed(d->m_typing);
    // Plain text only with Shift, as Obsidian does; otherwise an
    // image on the clipboard is saved as an attachment (PasteDrop.cpp).
    if (plain)
        insertText(cellSafe(QApplication::clipboard()->text()));
    else if (!pasteIntoCells()) // a block of cells into the table's cells, else the ordinary paste
        insertMimeData(QApplication::clipboard()->mimeData());
}

std::unique_ptr<QMenu> MarkdownEditor::contextMenuAt(qsizetype sourceOffset, const QPoint &anchor)
{
    auto menu = std::make_unique<QMenu>(this);
    menu->setObjectName(QStringLiteral("editorContextMenu"));
    const bool editable = d->m_mode != Mode::Reading;
    // One separator between groups, none at the ends.
    const auto gap = [&menu] {
        if (!menu->isEmpty() && !menu->actions().constLast()->isSeparator())
            menu->addSeparator();
    };
    // "Name<tab>Ctrl+B": the key is shown, never registered, so it cannot fire twice.
    const auto keyText = [](const QString &id) {
        for (const Command &command : commands()) {
            if (command.id == id && !command.keys.isEmpty())
                return QLatin1Char('\t') + command.keys.first().toString(QKeySequence::NativeText);
        }
        return QString();
    };
    const auto elide = [](QString text) {
        if (text.size() > 30)
            text = text.left(30) + QStringLiteral("...");
        return text.replace(QLatin1Char('&'), QStringLiteral("&&"));
    };

    if (addSpellingActions(*menu, sourceOffset))
        gap();

    // Cut, copy and paste.
    const bool hasSelection = selection().length() > 0 || d->m_cellSel.tableStart >= 0;
    if (editable) {
        connect(menu->addAction(tr("Cut") + QStringLiteral("\tCtrl+X")), &QAction::triggered, this,
                [this] { cutSelection(); });
        menu->actions().constLast()->setEnabled(hasSelection);
    }
    connect(menu->addAction(tr("Copy") + QStringLiteral("\tCtrl+C")), &QAction::triggered, this,
            [this] { copySelection(); });
    menu->actions().constLast()->setEnabled(hasSelection);
    if (editable) {
        connect(menu->addAction(tr("Paste") + QStringLiteral("\tCtrl+V")), &QAction::triggered, this,
                [this] { pasteClipboard(); });
    }

    if (editable) {
        gap();
        // With nothing selected a format applies to the word under the pointer.
        const auto format = [this, sourceOffset](const QString &id) {
            if (selection().length() == 0) {
                if (const Range word = wordAt(sourceOffset); word.length() > 0)
                    setSelectionRange(word.start, word.end);
            }
            runCommand(id);
        };
        QMenu *submenu = menu->addMenu(tr("Format"));
        static const std::pair<const char *, const char *> formats[] = {
            {"Bold", "editor:toggle-bold"},
            {"Italic", "editor:toggle-italics"},
            {"Strikethrough", "editor:toggle-strikethrough"},
            {"Highlight", "editor:toggle-highlight"},
            {"Code", "editor:toggle-code"},
            {"Math", "editor:toggle-inline-math"},
            {"Comment", "editor:toggle-comments"},
        };
        for (const auto &[name, id] : formats) {
            const QString commandID = QString::fromLatin1(id);
            connect(submenu->addAction(tr(name) + keyText(commandID)), &QAction::triggered, this,
                    [format, commandID] { format(commandID); });
        }
        connect(menu->addAction(tr("Add link") + keyText(QStringLiteral("editor:insert-link"))),
                &QAction::triggered, this, [this] { insertLink(); });
        if (d->m_parseOptions.taskStates) {
            auto *states = new QMenu(tr("Checkbox state"), menu.get());
            if (addTaskStateActions(*states, sourceOffset))
                menu->addMenu(states);
            else
                delete states; // not on a list item
        }

        // The only way a meta note leaves the text: removed, never hidden.
        for (const Range &run : d->m_decorations.metaNotes) {
            if (sourceOffset < run.start || sourceOffset >= run.end)
                continue;
            gap();
            connect(menu->addAction(tr("Delete meta note")), &QAction::triggered, this,
                    [this, run] { deleteMetaNote(run); });
            break;
        }
    }

    // What the window can do with a word: look it up, search for it. A
    // selection of one short line counts; anything longer is not "a word".
    QString target;
    if (const Range chosen = selection(); chosen.length() > 0) {
        const QString text = d->m_text.mid(chosen.start, chosen.length()).trimmed();
        if (chosen.length() <= 60 && !text.contains(QLatin1Char('\n')))
            target = text;
    } else if (const Range word = wordAt(sourceOffset); word.length() > 0) {
        target = d->m_text.mid(word.start, word.length());
    }
    if (!target.isEmpty()) {
        const QString label = elide(target);
        const bool web = !d->m_contextActions.webSearchURL.isEmpty();
        if (d->m_contextActions.lookup || d->m_contextActions.vaultSearch || web)
            gap();
        if (d->m_contextActions.lookup) {
            connect(menu->addAction(tr("Look up \"%1\"").arg(label)), &QAction::triggered, this,
                    [this, target, anchor] {
                        emit lookupRequested(target, anchor.isNull() ? QCursor::pos() : anchor);
                    });
        }
        if (d->m_contextActions.vaultSearch) {
            connect(menu->addAction(tr("Search for \"%1\"").arg(label)), &QAction::triggered, this,
                    [this, target] { emit searchRequested(target); });
        }
        if (web) {
            const QString url = d->m_contextActions.webSearchURL;
            connect(menu->addAction(tr("Search the web for \"%1\"").arg(label)),
                    &QAction::triggered, this, [url, target] {
                        QDesktopServices::openUrl(QUrl(url.arg(
                            QString::fromUtf8(QUrl::toPercentEncoding(target)))));
                    });
        }
    }

    if (tableCellAt(sourceOffset).row >= 0) {
        gap();
        addTableActions(*menu, sourceOffset);
    }
    // A callout is edited as source from its menu as from its button, and
    // copied, retyped or unwrapped there.
    const qsizetype block = blockAt(sourceOffset);
    if (block >= 0 && firstPiece(d->m_blocks[size_t(block)]).callout
        && hasEditButton(firstPiece(d->m_blocks[size_t(block)]))) {
        gap();
        addCalloutActions(*menu, block);
    }
    return menu;
}

void MarkdownEditor::deleteMetaNote(Range run)
{
    // One neighbouring space goes with the run so no double or stray space is
    // left: `early [x] and` -> `early and`, `[y] She` -> `She`, `back [z].`
    // -> `back.`. A run glued to a word keeps the space after it.
    const bool spaceBefore = run.start > 0 && d->m_text.at(run.start - 1) == u' ';
    const bool spaceAfter = run.end < d->m_text.size() && d->m_text.at(run.end) == u' ';
    const bool atLineStart = run.start == 0 || d->m_text.at(run.start - 1) == u'\n';
    if (spaceAfter && (spaceBefore || atLineStart))
        ++run.end;
    else if (spaceBefore && !spaceAfter)
        --run.start;

    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(run.start, run.length(), QString());
}

void MarkdownEditor::contextMenuEvent(QContextMenuEvent *event)
{
    const QPoint point = viewport()->mapFrom(this, event->pos());
    // The menu key acts on the caret itself, not on the point Qt guessed for it.
    const qsizetype offset = event->reason() == QContextMenuEvent::Keyboard ? d->m_cursor : offsetAt(point);
    // A click outside the selection moves the caret there, inside keeps it.
    if (const Range chosen = selection(); offset < chosen.start || offset > chosen.end)
        setCursorOffset(offset);
    // The menu key opens it at the caret: the popover goes there, not to the mouse.
    if (const auto menu = contextMenuAt(offset, event->globalPos()))
        menu->exec(event->globalPos());
}

void strikeThrough(QTextCharFormat &format, const Theme &theme)
{
    const QColor text = theme.color(QStringLiteral("--strikethrough-color"));
    const QColor line = theme.color(QStringLiteral("--strikethrough-decoration-color"));
    if (text.isValid())
        format.setForeground(text);
    if (line.isValid())
        format.setProperty(kStrikeLineProperty, line);
    else
        format.setFontStrikeOut(true);
}

void paintStrikeLines(QPainter &painter, const QTextLayout &layout, const QPointF &origin)
{
    const QFontMetricsF metrics(layout.font());
    painter.save();
    for (const QTextLayout::FormatRange &range : layout.formats()) {
        const QVariant line = range.format.property(kStrikeLineProperty);
        if (!line.isValid())
            continue;
        // The stroke's edges on whole pixels, like Qt's own strikeout, or the
        // line is a blur of two rows.
        const qreal width = qMax<qreal>(1, std::round(metrics.lineWidth()));
        painter.setPen(QPen(line.value<QColor>(), width));
        const int end = range.start + range.length;
        for (QTextLine row = layout.lineForTextPosition(range.start); row.isValid() && row.textStart() < end;
             row = layout.lineAt(row.lineNumber() + 1)) {
            const qreal from = row.cursorToX(qMax(range.start, row.textStart()));
            const qreal to = row.cursorToX(qMin(end, row.textStart() + row.textLength()));
            const qreal y = std::round(origin.y() + row.y() + row.ascent() - metrics.strikeOutPos() - width / 2) + width / 2;
            painter.drawLine(QPointF(origin.x() + from, y), QPointF(origin.x() + to, y));
        }
    }
    painter.restore();
}

QList<QTextLayout::FormatRange> MarkdownEditor::formatsIn(const BlockView &view,
                                                          const Range &block,
                                                          const DisplayMap &map) const
{
    QList<QTextLayout::FormatRange> formats;

    // Set only by a scheme or the person: unset, bold, italic, headings and
    // quotes keep the colour of the block they sit in, so no default stands
    // in for it (Theme.cpp). Same for strikethrough and underline.
    const QColor boldColour = d->m_theme.color(QStringLiteral("--bold-color"));
    const QColor italicColour = d->m_theme.color(QStringLiteral("--italic-color"));
    const QColor faint = d->m_theme.color(QStringLiteral("--text-faint"));
    const QColor codeColour = d->m_theme.color(QStringLiteral("--code-normal"));
    const QColor codeBackground = d->m_theme.color(QStringLiteral("--code-background"));
    const QColor highlight = d->m_theme.color(QStringLiteral("--text-highlight-bg"));
    const QColor tagColour = d->m_theme.color(QStringLiteral("--tag-color"));
    const QColor footnoteColour = d->m_theme.color(QStringLiteral("--footnote-color"));
    const QColor metaNote = d->m_theme.color(QStringLiteral("--meta-note-color"));
    const QColor bracket = d->m_theme.color(QStringLiteral("--bracket-color"));

    const auto add = [&](const Range &source, const QTextCharFormat &format) {
        const Range shown = map.toDisplay(source);
        if (shown.length() <= 0)
            return;
        QTextLayout::FormatRange range;
        range.start = int(shown.start);
        range.length = int(shown.length());
        range.format = format;
        formats.append(range);
    };

    // Under everything else: a heading's or a quote's colour is the block's,
    // which a bold word, a link or a revealed marker inside it overrides.
    QList<QTextLayout::FormatRange> underlay;
    if (view.span.kind == BlockSpan::Kind::Quote) {
        const QColor quote = d->m_theme.color(QStringLiteral("--blockquote-color"));
        const Range shown = map.toDisplay(block);
        if (quote.isValid() && shown.length() > 0) {
            QTextCharFormat format;
            format.setForeground(quote);
            underlay.append({int(shown.start), int(shown.length()), format});
        }
    }

    // In source mode the markers stay visible, so colour them like Obsidian
    // rather than hiding them.
    if (plainSource(view) || view.active) {
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
            const Range &marker = d->m_decorations.syntax.at(i);
            if (!marker.overlaps(block))
                continue;
            QTextCharFormat format;
            format.setForeground(faint);
            add(marker, format);
        }
    }

    for (const ListItemSpan &item : listItemsIn(block)) {
        if (!item.task)
            continue;
        const TaskState state = taskStateOf(item.state);
        if (!state.strike && !state.mute)
            continue;
        QTextCharFormat struck;
        if (state.strike)
            strikeThrough(struck, d->m_theme);
        if (state.mute)
            struck.setForeground(d->m_theme.color(QStringLiteral("--checklist-done-color")));
        add({item.marker.end, item.range.end}, struck);
    }

    for (const NestedCallout &nested : view.nested) {
        if (!nested.span->titleRange.isValid() || !nested.span->titleRange.overlaps(block))
            continue;
        QTextCharFormat title;
        title.setFont(nested.style.titleFont(d->m_theme));
        title.setForeground(nested.style.headingColor);
        add(nested.span->titleRange, title);
    }

    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = d->m_decorations.styles.at(i);
        if (!span.range.overlaps(block))
            continue;

        QTextCharFormat format;
        switch (span.kind) {
        case StyleSpan::Kind::Bold:
            format.setFontWeight(QFont::Bold);
            if (boldColour.isValid())
                format.setForeground(boldColour);
            break;
        case StyleSpan::Kind::Italic:
            format.setFontItalic(true);
            if (italicColour.isValid())
                format.setForeground(italicColour);
            break;
        case StyleSpan::Kind::Strikethrough:
            strikeThrough(format, d->m_theme);
            break;
        case StyleSpan::Kind::InlineCode:
            format.setFont(d->m_theme.monospaceFont());
            format.setForeground(codeColour);
            format.setBackground(codeBackground);
            break;
        case StyleSpan::Kind::CodeBlock:
            if (view.span.kind == BlockSpan::Kind::Code)
                continue; // the block paints its own panel, font and tokens
            // A fence inside a callout or a blockquote is not a block of its
            // own, so it has nothing to paint its panel or set its font. It
            // takes both from here, and its tokens from codeFormatsFor.
            format.setFont(d->m_theme.monospaceFont());
            break;
        case StyleSpan::Kind::Link:
            // A link to a note that does not exist is dimmed, which is how a
            // typo in a link is spotted at all.
            format.setForeground(linkColourFor(span.payload));
            break;
        case StyleSpan::Kind::Tag:
            format.setForeground(tagColour);
            break;
        case StyleSpan::Kind::Footnote:
            // Only tints the raw source, once the caret reveals it -
            // layoutFootnotes paints the hidden case's number itself.
            format.setForeground(footnoteColour);
            break;
        case StyleSpan::Kind::MetaNote:
            format.setForeground(metaNote);
            break;
        case StyleSpan::Kind::BareBracket:
            format.setForeground(bracket);
            break;
        case StyleSpan::Kind::Highlight:
            format.setBackground(highlight);
            format.setForeground(d->m_theme.color(QStringLiteral("--text-highlight-fg")));
            break;
        case StyleSpan::Kind::Math:
        case StyleSpan::Kind::Html:
            format.setForeground(d->m_theme.color(QStringLiteral("--color-purple")));
            break;
        case StyleSpan::Kind::HtmlText: {
            // `<u>`, `<kbd>`, `<span style="color:...">` in an HTML block the
            // editor lays out itself (HtmlEdit.cpp), styled as inline HTML is.
            qsizetype end = 1;
            while (end < span.payload.size() && span.payload.at(end).isLetterOrNumber())
                ++end;
            const std::optional<QTextCharFormat> html = htmlTagFormat(span.payload.mid(1, end - 1).toLower(), span.payload);
            if (!html)
                continue;
            format = *html;
            break;
        }
        case StyleSpan::Kind::Heading: {
            // The weight and size come from the block font; only a colour set
            // for this level is added.
            const QColor heading = d->m_theme.color(QStringLiteral("--h%1-color").arg(span.level));
            if (!heading.isValid())
                continue;
            format.setForeground(heading);
            const Range shown = map.toDisplay(span.range);
            if (shown.length() > 0)
                underlay.append({int(shown.start), int(shown.length()), format});
            continue;
        }
        case StyleSpan::Kind::HtmlTag:
        case StyleSpan::Kind::Quote:
            continue; // carried by the block font
        }
        add(span.range, format);
    }
    // Inline HTML tags layoutInlineHtml recognised and paired: it already
    // hid the tags themselves, so this is the format for the text between
    // them (`<b>text</b>` -> "text", bold).
    for (const auto &[source, htmlFormat] : view.inlineHtmlFormats)
        add(source, htmlFormat);
    return underlay + formats;
}

Range MarkdownEditor::codeContentOf(const BlockView &view, QString *language) const
{
    Range content = view.span.range;

    // The fences, and the language tag on the opening one, are syntax; what
    // is left between them is the code.
    for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
        const Range &marker = d->m_decorations.syntax.at(i);
        if (marker.start <= content.start)
            content.start = qMax(content.start, marker.end);
        if (marker.end >= content.end)
            content.end = qMin(content.end, marker.start);
    }
    while (content.start < content.end && d->m_text.at(content.start) == QLatin1Char('\n'))
        ++content.start;

    if (language) {
        // The listing's style span starts in the fence's first piece, so only
        // that piece's slice of the decorations holds it.
        const BlockView &first = firstPiece(view);
        for (int i = first.firstStyle; i < first.lastStyle; ++i) {
            const StyleSpan &span = d->m_decorations.styles.at(i);
            if (span.kind == StyleSpan::Kind::CodeBlock && span.range.overlaps(first.span.range)) {
                *language = span.payload;
                break;
            }
        }
    }
    return content;
}

QList<QTextLayout::FormatRange> MarkdownEditor::codeFormatsFor(BlockView &view)
{
    if (view.span.kind != BlockSpan::Kind::Code)
        return nestedCodeFormatsFor(view);

    updateCodeStatesBefore(view);
    Range content;
    QColor plainText;
    QList<QTextLayout::FormatRange> formats = highlightPiece(view, &content, &plainText);

    // Highlighter offsets are relative to the code; the layout wants display
    // offsets. Nothing inside a fence is ever hidden, so the shift is uniform.
    const qsizetype shift = view.map.toDisplay(content.start);
    for (QTextLayout::FormatRange &range : formats)
        range.start += int(shift);
    if (plainText.isValid() && content.isValid() && content.length() > 0) {
        // The block's `fg`: one range under every token, which paint over it.
        QTextLayout::FormatRange plain;
        plain.start = int(shift);
        plain.length = int(content.length());
        plain.format.setForeground(plainText);
        formats.prepend(plain);
    }
    return formats;
}

QList<QTextLayout::FormatRange> MarkdownEditor::highlightPiece(BlockView &view, Range *content,
                                                               QColor *plainText)
{
    view.touched = true;
    QString language;
    *content = codeContentOf(view, &language);
    CodeHighlighter::State state =
        view.piece > 0 ? (&view - 1)->codeEnd : CodeHighlighter::State();
    QList<QTextLayout::FormatRange> formats;
    // Nothing configured and the setting off both come out as a null map and
    // no plain-text colour: the highlighter's own output, untouched.
    SyntaxMap map;
    bool mapped = false;
    if (d->m_complexCodeBlocks) {
        const LanguageStyle style = codeStyleOf(view);
        mapped = syntaxMapFor(style, &map);
        if (plainText)
            *plainText = style.fg;
    }
    if (content->isValid() && content->length() > 0 && !language.isEmpty())
        formats = d->m_code.highlight(d->m_text.mid(content->start, content->length()), language, &state,
                                   mapped ? &map : nullptr);
    if (view.pieces <= 1)
        return formats;

    const bool moved = !(state == view.codeEnd);
    view.codeEnd = state;
    view.codeEndValid = true;
    if (moved && view.piece + 1 < view.pieces) {
        // The next piece starts from somewhere else now - a comment opened
        // here, say - so its own end state is stale, and every later piece
        // already laid out may be showing colours that no longer hold.
        // Clearing their layouts is cheap; they are redone as they come into
        // view, and a state that converges again stops the redoing there.
        (&view + 1)->codeEndValid = false;
        for (BlockView *later = &view + 1; later <= &lastPiece(view); ++later) {
            if (!later->laidOut)
                continue;
            later->layout.reset();
            later->laidOut = false;
            later->heightEstimated = true; // keep the height it had meanwhile
        }
    }
    return formats;
}

void MarkdownEditor::updateCodeStatesBefore(BlockView &view)
{
    for (BlockView *piece = &view - view.piece; piece != &view; ++piece) {
        if (piece->codeEndValid)
            continue;
        Range content;
        highlightPiece(*piece, &content);
    }
}

/// Syntax highlighting for a fence that lives inside a callout or a
/// blockquote, where it is part of the surrounding layout rather than a block
/// of its own.
///
/// The block form can shift the highlighter's offsets by one constant, because
/// nothing inside a fence is hidden. Here the `>` that starts every line is,
/// and the shift is not uniform - so the highlighter is given the *displayed*
/// text, which is already the listing as the reader sees it.
QList<QTextLayout::FormatRange> MarkdownEditor::nestedCodeFormatsFor(const BlockView &view) const
{
    QList<QTextLayout::FormatRange> formats;
    const QString shown = view.map.text();
    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = d->m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::CodeBlock || span.payload.isEmpty()
            || !span.range.overlaps(view.span.range))
            continue;

        // ponytail: a listing that began in an earlier piece of a long callout
        // is highlighted from this piece's first line, so a comment opened
        // before it is not carried over; carry CodeHighlighter::State across
        // callout pieces as fences do if that shows up.
        const Range clipped{qMax(span.range.start, view.span.range.start), qMin(span.range.end, view.span.range.end)};
        const Range range = view.map.toDisplay(clipped);
        if (range.length() <= 0 || range.end > shown.size())
            continue;
        SyntaxMap map;
        const LanguageStyle style = styleOfLanguage(span.payload);
        const bool mapped = syntaxMapFor(style, &map);
        QList<QTextLayout::FormatRange> tokens = d->m_code.highlight(
            shown.mid(range.start, range.length()), span.payload, mapped ? &map : nullptr);
        for (QTextLayout::FormatRange &token : tokens)
            token.start += int(range.start);
        if (style.fg.isValid()) {
            QTextLayout::FormatRange plain;
            plain.start = int(range.start);
            plain.length = int(range.length());
            plain.format.setForeground(style.fg);
            formats.append(plain);
        }
        formats += tokens;
    }
    return formats;
}

int MarkdownEditor::gutterWidth() const
{
    if (!d->m_lineNumbers || d->m_mode != Mode::Source)
        return 0;
    // Wide enough for the largest number this note can reach.
    const int digits = qMax(2, QString::number(qMax<qsizetype>(1, d->m_lines.lineCount())).size());
    return int(QFontMetricsF(d->m_theme.monospaceFont()).horizontalAdvance(QLatin1Char('0')))
               * digits
           + 18;
}

int MarkdownEditor::contentWidth() const
{
    const int available = viewport()->width() - 2 * d->m_minimumSideMargin - gutterWidth();
    if (!d->m_readable)
        return qMax(120, available);
    return qMax(120, qMin(available, d->m_contentWidth));
}

int MarkdownEditor::contentLeft() const
{
    return qMax(d->m_minimumSideMargin + gutterWidth(),
                (viewport()->width() - contentWidth()) / 2);
}

void MarkdownEditor::paintGutter(QPainter &painter, const BlockView &view, qreal top) const
{
    if (gutterWidth() == 0 || !view.layout)
        return;

    painter.save();
    QFont font = d->m_theme.monospaceFont();
    font.setPixelSize(int(font.pixelSize() * 0.9));
    painter.setFont(font);
    painter.setPen(d->m_theme.color(QStringLiteral("--text-faint")));

    const qreal right = contentLeft() - 12;
    for (int i = 0; i < view.layout->lineCount(); ++i) {
        const QTextLine line = view.layout->lineAt(i);
        const qsizetype source = view.map.toSource(line.textStart());
        // Only the first visual line of a wrapped source line is numbered.
        if (i > 0 && d->m_lines.lineAt(source) == d->m_lines.lineAt(view.map.toSource(
                         view.layout->lineAt(i - 1).textStart())))
            continue;
        painter.drawText(QRectF(right - gutterWidth(), top + line.y(), gutterWidth(),
                                line.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(d->m_lines.lineAt(source) + 1));
    }
    painter.restore();
}

bool MarkdownEditor::softBreakAt(qsizetype source) const
{
    // Source mode shows the file as it is written: one line of the file is one
    // line on screen, the way Obsidian's own source mode lays out.
    // Obsidian's default (strict off) honours every newline; only the strict
    // setting follows CommonMark and joins the lines of a paragraph.
    if (!d->m_strictLineBreaks || d->m_mode == Mode::Source)
        return false;
    // A hard break: two spaces or a backslash before the newline.
    if (source >= 2 && d->m_text.at(source - 1) == QLatin1Char(' ')
        && d->m_text.at(source - 2) == QLatin1Char(' '))
        return false;
    if (source >= 1 && d->m_text.at(source - 1) == QLatin1Char('\\'))
        return false;

    // A nested callout's title stays in the layout, so the newline that ends
    // its header line has to break, or the title and the first line of the
    // body run together as one paragraph - which, in Markdown, they are.
    for (const CalloutSpan &callout : d->m_decorations.callouts) {
        if (callout.depth > 0 && callout.titleRange.isValid() && source >= callout.titleRange.end
            && source <= callout.titleRange.end + 1)
            return false;
    }

    for (const Range &paragraph : d->m_decorations.paragraphs) {
        if (paragraph.start > source)
            break; // sorted by the walk order, which follows the document
        if (source > paragraph.start && source < paragraph.end)
            return true;
    }
    return false;
}

std::span<const ListItemSpan> MarkdownEditor::listItemsIn(const Range &block) const
{
    // Sorted by start, so this is two binary searches rather than a walk over
    // every item in the note - which layout used to do once per line.
    const QList<ListItemSpan> &items = d->m_decorations.listItems;
    const auto startsBefore = [](const ListItemSpan &item, qsizetype at) {
        return item.range.start < at;
    };
    const auto first = std::lower_bound(items.cbegin(), items.cend(), block.start, startsBefore);
    const auto last = std::lower_bound(first, items.cend(), block.end, startsBefore);
    return {items.constData() + (first - items.cbegin()), size_t(last - first)};
}

/// Equal-width digits, so a column of numbers lines up as Obsidian's do.
static QFont tabularFigures(QFont font)
{
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
}

QString MarkdownEditor::listLabel(const ListItemSpan &item) const
{
    if (!item.ordered)
        return {};
    // Word's multilevel style: "1." at the top, then "1.1.", "1.2.1.".
    QString body;
    if (d->m_outlineNumbering && item.outline.contains(QLatin1Char('.')))
        body = item.outline;
    else
        body = QString::number(item.shownNumber > 0 ? item.shownNumber : item.number);
    if (d->m_listNumberSuffix.isEmpty()) // as the note wrote it
        return body + (item.bullet.isNull() ? QLatin1Char('.') : item.bullet);
    return body + d->m_listNumberSuffix;
}

void MarkdownEditor::layoutListColumns(BlockView &view) const
{
    view.listTextX.clear();
    view.listParent.clear();
    const std::span<const ListItemSpan> items = listItemsIn(view.span.range);
    if (items.empty())
        return;

    // Each item's parent: the nearest item before it, one level out, whose
    // range holds it.
    view.listParent.assign(items.size(), -1);
    std::vector<int> open;
    for (size_t i = 0; i < items.size(); ++i) {
        while (!open.empty()) {
            const ListItemSpan &candidate = items[size_t(open.back())];
            if (candidate.indent < items[i].indent && candidate.range.start <= items[i].range.start
                && items[i].range.start < candidate.range.end)
                break;
            open.pop_back();
        }
        view.listParent[i] = open.empty() ? -1 : open.back();
        open.push_back(int(i));
    }

    // Siblings share a column as wide as their widest label.
    const QFontMetricsF metrics(tabularFigures(fontFor(view.span)));
    QHash<qint64, qreal> columns; // (parent, level) -> width
    const auto keyOf = [&](size_t i) { return (qint64(view.listParent[i]) << 16) ^ items[i].indent; };
    for (size_t i = 0; i < items.size(); ++i) {
        const QString label = listLabel(items[i]);
        const qreal wanted = label.isEmpty() ? kListIndent : metrics.horizontalAdvance(label) + 10;
        qreal &column = columns[keyOf(i)];
        column = qMax(column, qMax(kListIndent, wanted));
    }
    view.listTextX.resize(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const int parent = view.listParent[i];
        const qreal base = parent < 0 ? kListIndent * items[i].indent : view.listTextX[size_t(parent)];
        view.listTextX[i] = base + columns.value(keyOf(i));
    }
}

qreal MarkdownEditor::listTextAt(const BlockView &view, qsizetype source) const
{
    // Items come in order and a nested one lies inside its parent, so the
    // last one begun that still holds the offset is the innermost.
    const std::span<const ListItemSpan> items = listItemsIn(view.span.range);
    qreal x = -1;
    for (size_t i = 0; i < items.size() && i < view.listTextX.size(); ++i) {
        if (items[i].range.start > source)
            break;
        if (source < items[i].range.end || source == items[i].range.start)
            x = view.listTextX[i];
    }
    return x;
}

QRectF MarkdownEditor::checkboxRect(const BlockView &view, const ListItemSpan &item) const
{
    if (!view.layout)
        return {};
    const qsizetype display = view.map.toDisplay(item.marker.end);
    const QTextLine line = view.layout->lineForTextPosition(int(display));
    if (!line.isValid())
        return {};

    const qreal box = QFontMetricsF(view.layout->font()).height() * 0.8;
    const qreal x = contentLeft() + line.x() - box - 8;
    const qreal y = view.y + view.textTop - verticalScrollBar()->value() + line.y()
                    + (line.height() - box) / 2;
    return QRectF(x, y, box, box);
}

/// A faint vertical rule at each level of list indentation, the way an editor
/// shows nesting. Drawn behind the text, one run per item so a wrapped line
/// keeps the guides of the item it belongs to.
void MarkdownEditor::paintIndentGuides(QPainter &painter, const BlockView &view, qreal left,
                                       qreal top) const
{
    if (!view.layout || view.folded || view.listTextX.empty())
        return;

    QColor guide = d->m_theme.color(QStringLiteral("--text-faint"));
    guide.setAlphaF(0.4f);

    painter.save();
    painter.setPen(QPen(guide, 1));
    const std::span<const ListItemSpan> items = listItemsIn(view.span.range);
    qsizetype hiddenUntil = 0; // under a folded item
    for (size_t i = 0; i < items.size() && i < view.listParent.size(); ++i) {
        if (items[i].range.start < hiddenUntil)
            continue;
        if (foldedItem(items, i))
            hiddenUntil = items[i].range.end;
        if (view.listParent[i] < 0)
            continue;
        const QRectF band = bandOf(view, items[i].range);
        if (band.height() <= 0)
            continue;
        // One rule under each item it sits in, just left of that item's text.
        for (int parent = view.listParent[i]; parent >= 0; parent = view.listParent[size_t(parent)]) {
            const qreal x = left + view.listTextX[size_t(parent)] - 8;
            painter.drawLine(QPointF(x, top + band.y()), QPointF(x, top + band.y() + band.height()));
        }
    }
    painter.restore();
}

void MarkdownEditor::paintListMarkers(QPainter &painter, const BlockView &view, qreal left,
                                      qreal top) const
{
    // Source mode and the caret's own block show the real markers, so drawing
    // bullets as well would double them up.
    if (!view.layout || view.listTextX.empty() || d->m_mode == Mode::Source)
        return;

    const QColor muted = d->m_theme.color(QStringLiteral("--list-marker-color"));

    const auto markerShown = [&view](const ListItemSpan &item) {
        return std::any_of(view.revealed.cbegin(), view.revealed.cend(),
                           [&item](const Range &range) { return range.overlaps(item.marker); });
    };
    const std::span<const ListItemSpan> items = listItemsIn(view.span.range);
    qsizetype hiddenUntil = 0; // under a folded item
    for (size_t at = 0; at < items.size(); ++at) {
        const ListItemSpan &item = items[at];
        if (item.range.start < hiddenUntil)
            continue;
        if (foldedItem(items, at))
            hiddenUntil = item.range.end;
        // The caret has uncovered this item's marker as text, so a drawn
        // bullet would double it.
        if (markerShown(item))
            continue;

        const qsizetype display = view.map.toDisplay(item.marker.end);
        const QTextLine line = view.layout->lineForTextPosition(int(display));
        if (!line.isValid())
            continue;

        // Where the item's text starts: its column's right edge.
        const qreal textX = left + line.x();
        const qreal baseline = top + line.y();

        if (item.task) {
            const qreal box = QFontMetricsF(view.layout->font()).height() * 0.8;
            paintTaskBox(painter, QRectF(textX - box - 8, baseline + (line.height() - box) / 2, box, box),
                         taskStateOf(item.state), view.layout->font());
            continue;
        }

        painter.save();
        painter.setPen(muted);
        if (item.ordered) {
            const QFont numbers = tabularFigures(view.layout->font());
            painter.setFont(numbers);
            const QString label = listLabel(item);
            const qreal width = QFontMetricsF(numbers).horizontalAdvance(label);
            painter.drawText(QRectF(textX - 8 - width - 2, baseline, width + 2, line.height()),
                             Qt::AlignRight | Qt::AlignVCenter, label);
        } else {
            // One look per way of writing a bullet, set in Settings.
            BulletGlyph glyph = d->m_bulletGlyphs.glyphFor(item.bullet);
            if (d->m_nestedBulletGlyphs && item.indent > 0 && glyph != BulletGlyph::AsTyped)
                glyph = BulletGlyph::Turn; // a nested item hangs off the one above it
            paintBulletGlyph(painter, glyph, item.bullet, QPointF(textX - 12, baseline + line.height() / 2), muted,
                             view.layout->font());
        }
        painter.restore();
    }
}

void MarkdownEditor::toggleTask(const ListItemSpan &item)
{
    if (!item.task || !item.checkbox.isValid())
        return;
    // Ticking a box must not steal the caret; every box is three characters
    // wide, so the offsets it was on are still the right ones.
    const qsizetype anchor = d->m_anchor;
    const qsizetype caret = d->m_cursor;
    pushUndo();
    d->m_lastEdit.invalidate();
    applyEdit(item.checkbox.start, item.checkbox.length(), taskBox(nextTaskState(item.state)));
    setSelectionRange(anchor, caret);
}

/// How many nested callouts cover this source offset. Zero is the block's own
/// callout, which is not nested and carries the block's normal indent.
int MarkdownEditor::nestingDepthAt(const BlockView &view, qsizetype source) const
{
    int depth = 0;
    for (const NestedCallout &nested : view.nested) {
        if (nested.span->range.contains(source))
            depth = qMax(depth, nested.span->depth);
    }
    return depth;
}

const CalloutSpan *MarkdownEditor::calloutFor(const BlockView &view) const
{
    const qsizetype start = firstPiece(view).span.range.start; // a long callout is in pieces
    for (const CalloutSpan &callout : d->m_decorations.callouts) {
        if (callout.range.start == start)
            return &callout;
    }
    return nullptr;
}

qreal MarkdownEditor::calloutHeaderHeight(const CalloutStyle &style) const
{
    return QFontMetricsF(style.titleFont(d->m_theme)).height() * 1.9;
}

void MarkdownEditor::paintCallout(QPainter &painter, const BlockView &view, qreal left,
                                  qreal top) const
{
    if (!view.callout)
        return;

    const CalloutStyle &style = view.calloutStyle;
    // A callout in pieces is one panel: each piece draws its slice of it, the
    // rounded corners only where the panel really begins and ends.
    const bool first = view.piece == 0;
    const bool last = view.piece == view.pieces - 1 || view.pieces <= 1;
    const QRectF slice(left - 14, first ? top - 8 : top, contentWidth() + 28,
                       view.height + (first ? 8 : 0) + (last ? 8 : 0));
    const QRectF panel = slice.adjusted(0, first ? 0 : -16, 0, last ? 0 : 16);
    QPainterPath rounded;
    rounded.addRoundedRect(panel, 6, 6);
    painter.save();
    painter.setClipRect(slice, Qt::IntersectClip);
    painter.fillPath(rounded, style.background);
    painter.fillRect(QRectF(panel.left(), panel.top(), 3, panel.height()), style.bar);
    painter.restore();
    if (!first)
        return; // the title, icon and fold arrow are the first piece's

    // The icon keeps one size whatever it is, so a photograph and a glyph line
    // up in the same column. Only a Lucide glyph is recoloured; a user's own
    // image is drawn as it is.
    const QFont titleFont = style.titleFont(d->m_theme);
    const qreal iconSize = QFontMetricsF(titleFont).height();
    const QIcon icon = Icons::icon(style.icon, style.bar, int(iconSize));
    const QRectF iconRect(panel.left() + 14, panel.top() + 10, iconSize, iconSize);
    if (!icon.isNull())
        icon.paint(&painter, iconRect.toRect());

    painter.save();
    painter.setFont(titleFont);
    painter.setPen(style.headingColor);
    // The title runs from the icon to the fold arrow, whatever size the style
    // gives it; a fixed width clipped any heading larger than the theme's.
    const qreal titleLeft = iconRect.right() + 8;
    if (!d->m_calloutTitleEdit || d->m_calloutTitleAt != view.callout->range.start) // else the editor draws it
        painter.drawText(QRectF(titleLeft, panel.top() + 8,
                                qMax(qreal(0), panel.right() - 36 - titleLeft), iconSize * 1.4),
                         Qt::AlignVCenter | Qt::AlignLeft, view.callout->title);
    painter.restore();

    // `[!type]+`, `[!type]-` and a `:::` details container all fold; the
    // triangle says so, and the whole title row toggles it.
    if (view.callout->foldable)
        paintFoldArrow(painter, foldHandleRect(view, left, top), view.folded, style.bar);
}

const TableSpan *MarkdownEditor::tableFor(const BlockView &view) const
{
    for (const TableSpan &table : d->m_decorations.tables) {
        if (table.range.start == view.span.range.start)
            return &table;
    }
    return nullptr;
}

const EmbedSpan *MarkdownEditor::soleEmbedOf(const BlockView &view) const
{
    for (const EmbedSpan &embed : d->m_decorations.embeds) {
        // An <img> is always an inline cell (buildEmbedCell): it never
        // transcludes a note or a PDF the way a whole-block `![[x]]` does.
        if (embed.html || !embed.range.overlaps(view.span.range))
            continue;
        // Only when the embed is the whole block; an image in the middle of a
        // sentence stays inline until Phase 3 finishes inline objects.
        const QStringView rest =
            QStringView(d->m_text).mid(view.span.range.start, view.span.range.length());
        const QString trimmed = rest.trimmed().toString();
        if (trimmed == d->m_text.mid(embed.range.start, embed.range.length()))
            return &embed;
    }
    return nullptr;
}

void MarkdownEditor::hideWholeBlockText(BlockView &view)
{
    view.map = DisplayMap::build(d->m_text, view.span.range, {view.span.range});
}

bool MarkdownEditor::layoutEmbed(BlockView &view)
{
    view.image = QPixmap();
    view.imagePath.clear();
    view.embedLabel.clear();
    view.embedPanel = false;
    view.mediaPath.clear();
    view.mediaVideo = false;
    view.isPdfEmbed = false;
    view.pdfCells.clear();
    view.pdfError.clear();
    view.isFileCard = false;
    // A runnable embed (layoutSourceFile, NotebookEmbed part) sets these;
    // reset here so a file that stops being runnable - a non-code extension
    // now, or the notebooks feature toggled off - does not keep showing a
    // stale run control or a previous run's output.
    view.notebookEligible = false;
    view.notebookOutputs.clear();

    const EmbedSpan *embed = soleEmbedOf(view);
    if (!embed || !m_resolver)
        return false;

    const QString path = m_resolver(embed->target);
    if (path.isEmpty())
        return false;

    const QString suffix = QFileInfo(path).suffix().toLower();
    // Before the image formats below are asked for: listing them loads every
    // image plugin, which a note of tables has no use for.
    if (d->m_features.fileEmbeds && d->m_features.CSVembeds
        && (suffix == QLatin1String("csv") || suffix == QLatin1String("tsv")))
        return layoutCSVembed(view, path);

    // Some installs register a PDF-reading QImageIOHandler (a thumbnailer
    // plugin, commonly), which would otherwise steal every PDF into a flat
    // one-page raster here before the richer, page-aware PdfPage path below
    // ever gets a look at it.
    if (suffix != QLatin1String("pdf") && isImageSuffix(suffix))
        return layoutImage(view, *embed, path);
    // Obsidian's audio and video formats, and the two Ogg audio spellings
    // its list leaves out (the freedesktop sounds are .oga).
    static const QStringList audio = {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("m4a"),
                                      QStringLiteral("ogg"), QStringLiteral("3gp"), QStringLiteral("flac"),
                                      QStringLiteral("opus"), QStringLiteral("oga")};
    static const QStringList video = {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("ogv"),
                                      QStringLiteral("mov"), QStringLiteral("mkv")};
    if (d->m_features.mediaEmbeds && (audio.contains(suffix) || video.contains(suffix)))
        return layoutMedia(view, path, video.contains(suffix));
    if (d->m_features.PDFembeds && suffix == QLatin1String("pdf"))
        return layoutPdf(view, *embed, path);
    if (d->m_features.texEmbeds && suffix == QLatin1String("tex"))
        return layoutTexEmbed(view, *embed, path);
    if (!d->m_features.fileEmbeds)
        return false; // falls through to the paragraph's plain link text

    if (Vault::isNote(path))
        return layoutNoteEmbed(view, *embed, path);
    // A .base is the ```base block's own table, fed the file: its rows open
    // their notes, as they do in Obsidian's embedded base.
    if (d->m_features.baseBlocks && suffix == QLatin1String("base")) {
        const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        auto cached = d->m_baseFiles.find(path);
        if (cached == d->m_baseFiles.end() || cached->first != mtime) {
            QFile file(path);
            const QString text = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
            cached = d->m_baseFiles.insert(path, {mtime, text});
        }
        hideWholeBlockText(view);
        // Obsidian's own embed syntax for a view: ![[Books.base#Reading]] -
        // the fragment after "#" names it, the same slot a note embed's
        // heading/block reference uses (see noteEmbedSlice). An unmatched
        // name falls back to the file's first view, in evaluateBaseForEditor.
        QString viewName = embed->subpath;
        if (viewName.startsWith(QLatin1Char('#')))
            viewName.remove(0, 1);
        return layoutBaseTable(view, cached->second.trimmed(), viewName);
    }
    if (d->m_fileEmbedRenderers.contains(suffix))
        return layoutHookedFileEmbed(view, *embed, path);
    return layoutSourceFile(view, *embed, path);
}

bool MarkdownEditor::layoutImage(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    QPixmap pixmap = d->m_images.value(path);
    if (pixmap.isNull()) {
        pixmap = QPixmap(path);
        if (pixmap.isNull())
            return false;
        d->m_images.insert(path, pixmap);
    }

    // Obsidian caps an image at the readable column and honours |WxH.
    const QSizeF size = embedImageSize(embed, pixmap.size(), contentWidth());
    const int width = qMax(1, qRound(size.width()));
    const int height = qMax(1, qRound(size.height()));

    view.image = pixmap;
    view.imageSize = QSize(width, height);
    view.imagePath = path;
    view.layout.reset();
    view.cells.clear();
    view.height = height;
    hideWholeBlockText(view);
    view.laidOut = true;
    return true;
}

bool MarkdownEditor::layoutSourceFile(BlockView &view, const EmbedSpan &embed,
                                      const QString &path)
{
    constexpr qint64 kMaxBytes = 512 * 1024;
    QFileInfo info(path);
    if (!info.isFile())
        return false;
    // Never a binary file's bytes as text: a card (icon, name, size) opens
    // it instead - the same fallback Obsidian's own "unsupported file" embed
    // shows.
    if (isBinaryFile(path))
        return layoutFileCard(view, embed, path);

    view.embedLabel = info.fileName();
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = codeHeaderHeight();

    QString body;
    if (info.size() > kMaxBytes) {
        // Plan 16.4: a huge file shows its header and nothing else. There is
        // no run button here, and never will be.
        body = tr("%1 is %2 - too large to show inline.")
                   .arg(info.fileName(), QLocale().formattedDataSize(info.size()));
    } else {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        body = QString::fromUtf8(file.readAll());
        body.replace(QLatin1String("\r\n"), QLatin1String("\n"));

        // `#L10-L20`, or `#L10` for one line, as in Obsidian's line links.
        static const QRegularExpression lineRef(
            QStringLiteral("#L(\\d+)(?:-L?(\\d+))?"), QRegularExpression::CaseInsensitiveOption);
        const auto match = lineRef.match(embed.subpath);
        if (match.hasMatch()) {
            const QStringList lines = body.split(QLatin1Char('\n'));
            const int first = qBound(1, match.captured(1).toInt(), int(lines.size()));
            const int last = match.captured(2).isEmpty()
                                 ? first
                                 : qBound(first, match.captured(2).toInt(), int(lines.size()));
            body = lines.mid(first - 1, last - first + 1).join(QLatin1Char('\n'));
            view.embedLabel =
                first == last ? tr("%1:%2").arg(info.fileName()).arg(first)
                              : tr("%1:%2-%3").arg(info.fileName()).arg(first).arg(last);
        }
    }
    while (body.endsWith(QLatin1Char('\n')))
        body.chop(1);

    const BlockSpan codeSpan{BlockSpan::Kind::Code, view.span.range, 0};
    QString shown = body;
    shown.replace(QLatin1Char('\n'), QChar(QChar::LineSeparator));

    view.map = DisplayMap::build(d->m_text, view.span.range, {view.span.range});
    view.layout = std::make_unique<QTextLayout>(shown);
    view.layout->setCacheEnabled(true);
    view.layout->setFont(fontFor(codeSpan));

    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    view.layout->setTextOption(option);
    // The file's own extension picks the Kate definition.
    view.layout->setFormats(d->m_code.highlight(body, info.suffix()));

    const qreal lineHeight = lineHeightFor(codeSpan);
    view.layout->beginLayout();
    qreal height = 0;
    qreal widest = 0;
    while (true) {
        QTextLine line = view.layout->createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(contentWidth() - 28);
        line.setPosition(QPointF(0, height));
        height += lineHeight;
    }
    view.layout->endLayout();

    view.height = qMax(height, lineHeight) + view.textTop;
    // Never for the "too large to show inline" placeholder above - `body`
    // would be that message, not the file's own code.
    if (info.size() <= kMaxBytes)
        layoutRunnableEmbed(view, path, body);
    view.laidOut = true;
    return true;
}

const MathSpan *MarkdownEditor::displayMathOf(const BlockView &view) const
{
    // md4qt reports a formula's range as the LaTeX between the delimiters, so
    // a block is display maths when it holds one such span and nothing else
    // but the `$$` around it.
    for (const MathSpan &math : d->m_decorations.maths) {
        if (!math.display || !math.range.overlaps(view.span.range))
            continue;

        QString rest = d->m_text.mid(view.span.range.start, view.span.range.length());
        const qsizetype offset = math.range.start - view.span.range.start;
        if (offset >= 0 && offset + math.range.length() <= rest.size())
            rest.remove(offset, math.range.length());
        rest.remove(QLatin1Char('$'));
        if (rest.trimmed().isEmpty())
            return &math;
    }
    return nullptr;
}

bool MarkdownEditor::layoutMath(BlockView &view)
{
    if (!d->m_features.maths)
        return false; // shows as source - no MicroTeX call, no image cached

    const MathSpan *math = displayMathOf(view);
    if (!math)
        return false;

    const Range source = math->formula.isValid() && math->formula.length() > 0 ? math->formula
                                                                               : math->range;
    const QString latex = d->m_text.mid(source.start, source.length()).trimmed();
    const qreal ratio = devicePixelRatioF();
    const QString key = QStringLiteral("math|%1|%2|%3")
                            .arg(latex)
                            .arg(d->m_theme.value(QStringLiteral("--font-text-size")))
                            .arg(contentWidth());

    QPixmap pixmap = d->m_images.value(key);
    if (pixmap.isNull()) {
        const QImage image = d->m_math.render(latex, d->m_theme.textFont().pixelSize() * 1.2,
                                           d->m_theme.color(QStringLiteral("--text-normal")),
                                           contentWidth(), ratio);
        if (image.isNull())
            return false; // falls back to showing the source, not to an error
        pixmap = QPixmap::fromImage(image);
        d->m_images.insert(key, pixmap);
    }

    // Display maths is centred in the column, as TeX sets it.
    const int width = int(pixmap.width() / pixmap.devicePixelRatio());
    const int height = int(pixmap.height() / pixmap.devicePixelRatio());
    view.image = pixmap;
    view.imageSize = QSize(width, height);
    view.imageCentred = true;
    view.layout.reset();
    view.cells.clear();
    view.height = height + 12;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::layoutInlineMaths(BlockView &view, QList<Range> &hidden)
{
    if (d->m_decorations.maths.isEmpty())
        return; // the common case: nothing to check, so nothing to render

    const QFont font = fontFor(view.span);
    const QColor colour = d->m_theme.color(QStringLiteral("--text-normal"));
    const qreal ratio = devicePixelRatioF();

    for (const MathSpan &math : d->m_decorations.maths) {
        if (math.display || !math.range.overlaps(view.span.range))
            continue;

        // The parser hid this exact range unconditionally; find it so it can
        // be dropped again if the formula turns out not to render - which,
        // with the feature off, it never even tries to.
        const Range hideRange{math.formula.start, math.range.end};
        auto it = std::find(hidden.begin(), hidden.end(), hideRange);
        if (it == hidden.end())
            continue;
        if (!d->m_features.maths) {
            hidden.erase(it);
            continue;
        }

        const QString latex = d->m_text.mid(math.formula.start, math.formula.length()).trimmed();
        const QString key = QStringLiteral("inlineMath|%1|%2|%3")
                                .arg(latex)
                                .arg(font.pixelSize())
                                .arg(colour.name(QColor::HexArgb));

        QPixmap pixmap = d->m_images.value(key);
        if (pixmap.isNull()) {
            // Inline maths is set in text style, as TeX sets `$...$`: limits
            // beside a sum rather than above and below it, and a fraction at
            // the size of the text around it. Rendered in display style, a
            // \sum was taller than the line and ran into the one below.
            const QImage image = d->m_math.render(QStringLiteral("\\textstyle ") + latex,
                                               font.pixelSize(), colour, contentWidth(), ratio);
            if (image.isNull()) {
                hidden.erase(it); // not renderable: show the raw source instead of a gap
                continue;
            }
            pixmap = QPixmap::fromImage(image);
            d->m_images.insert(key, pixmap);
        }

        BlockView::InlineMath formula;
        formula.sourcePos = math.range.start; // the "$" DisplayMap left visible
        formula.image = pixmap;
        formula.imageSize = QSize(int(pixmap.width() / pixmap.devicePixelRatio()),
                                  int(pixmap.height() / pixmap.devicePixelRatio()));
        view.inlineMaths.push_back(formula);
    }
}

void MarkdownEditor::layoutAnchorSeparators(BlockView &view, QList<Range> &hidden)
{
    const auto hides = [&hidden](qsizetype at) {
        return std::any_of(hidden.cbegin(), hidden.cend(),
                           [at](const Range &range) { return range.contains(at); });
    };
    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = d->m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::Link || !span.anchorHash.isValid()
            || !span.anchorText.isValid() || span.anchorText.length() <= 0)
            continue;
        // A block reference to another note stays as written, "Note#^id" -
        // Obsidian's live preview rewrites no "#" but a leading one, so
        // `[[#^id]]` shows "^id" (its reading view writes "Note > ^id").
        if (d->m_text.at(span.anchorText.start) == QLatin1Char('^') && !span.payload.isEmpty())
            continue;
        // Only a rendered link: with the caret inside it its brackets show,
        // and so does its "#". An alias has hidden the "#" already.
        if (!hides(span.range.start) || hides(span.anchorHash.start))
            continue;
        if (span.payload.isEmpty()) {
            hidden.append(span.anchorHash); // [[#Heading]]: this note's own heading
            continue;
        }
        view.separators.push_back(
            {span.anchorHash.start, 0,
             linkColourFor(span.payload)});
    }
}

void MarkdownEditor::paintAnchorSeparators(QPainter &painter, const BlockView &view, qreal left,
                                           qreal top) const
{
    if (view.separators.empty() || !view.layout)
        return;
    painter.save();
    painter.setFont(view.layout->font());
    for (const BlockView::AnchorSeparator &separator : view.separators) {
        const QTextLine line = view.layout->lineForTextPosition(int(separator.displayPos));
        if (!line.isValid())
            continue;
        painter.setPen(separator.colour);
        painter.drawText(QPointF(left + line.cursorToX(int(separator.displayPos)),
                                 top + line.y() + line.ascent()),
                         QStringLiteral(" > "));
    }
    painter.restore();
}

void MarkdownEditor::paintInlineMaths(QPainter &painter, const BlockView &view, qreal left,
                                      qreal top) const
{
    if (view.inlineMaths.empty() || !view.layout)
        return;

    for (const BlockView::InlineMath &formula : view.inlineMaths) {
        const QTextLine line = view.layout->lineForTextPosition(int(formula.displayPos));
        if (!line.isValid())
            continue;
        const qreal x = line.cursorToX(int(formula.displayPos));
        const qreal y = top + line.y() + (line.height() - formula.imageSize.height()) / 2.0;
        painter.drawPixmap(QRectF(left + x, y, formula.imageSize.width(), formula.imageSize.height())
                               .toRect(),
                           formula.image);
    }
}

namespace {

/// A CSS colour as an HTML cell wrote it: a name, #rgb, #rrggbb, or rgb()/rgba()
/// with plain numbers. Invalid when it is none of those.
QColor cssColor(const QString &text)
{
    if (text.isEmpty())
        return {};
    static const QRegularExpression functional(QStringLiteral(
        R"(^rgba?\(\s*(\d+)\s*[, ]\s*(\d+)\s*[, ]\s*(\d+)\s*(?:[,/]\s*([\d.]+)\s*)?\)$)"));
    const QRegularExpressionMatch match = functional.match(text);
    if (match.hasMatch()) {
        const QColor color(qMin(255, match.captured(1).toInt()), qMin(255, match.captured(2).toInt()),
                           qMin(255, match.captured(3).toInt()));
        QColor out = color;
        if (!match.captured(4).isEmpty())
            out.setAlphaF(qBound(0.0, match.captured(4).toDouble(), 1.0));
        return out;
    }
    return QColor::isValidColorName(text) ? QColor::fromString(text) : QColor();
}

} // namespace

bool MarkdownEditor::layoutTable(BlockView &view)
{
    view.cells.clear();
    const TableSpan *table = tableFor(view);
    if (!table || table->columns <= 0 || table->rows.isEmpty())
        return false;

    const QFont font = d->m_theme.textFont();
    const qreal lineHeight = QFontMetricsF(font).height() * 1.4;
    const qreal padX = 10;
    const qreal padY = 6;
    const qreal minimum = QFontMetricsF(font).horizontalAdvance(QLatin1Char('m')) * 3;
    // Obsidian does not squeeze a table into the readable column: every column
    // keeps the width its content wants, and a table too wide to fit scrolls
    // sideways inside its own block. Only a single runaway column is capped,
    // so one long cell wraps instead of pushing everything else off screen.
    const qreal available = contentWidth();
    const qreal columnCap = qMax(minimum * 4, available * 0.66);
    // Every embed by where it starts, so each cell finds its own with a binary
    // search instead of walking the note's every embed. Which of them a cell
    // draws (an <img> or an image file; a PDF, a note or a file card stays
    // text) is decided per embed that lands in a cell, not per embed of the note.
    std::vector<const EmbedSpan *> tableEmbeds;
    if (d->m_features.fileEmbeds) {
        tableEmbeds.reserve(size_t(d->m_decorations.embeds.size()));
        for (const EmbedSpan &embed : d->m_decorations.embeds)
            tableEmbeds.push_back(&embed);
        std::sort(tableEmbeds.begin(), tableEmbeds.end(),
                  [](const EmbedSpan *a, const EmbedSpan *b) { return a->range.start < b->range.start; });
    }

    // Natural widths first, so the columns can be sized from their content
    // the way Obsidian does, then scaled down together if they do not fit.
    QList<qreal> natural(table->columns, minimum);
    std::vector<Cell> cells;

    for (int r = 0; r < table->rows.size(); ++r) {
        const QList<Range> &row = table->rows.at(r);
        for (int c = 0; c < row.size() && c < table->columns; ++c) {
            Cell cell;
            cell.source = row.at(c);
            cell.row = r;
            cell.column = c;
            cell.alignment = table->alignment.value(c, Qt::AlignLeft);
            if (table->html) { // the cell's own style=
                const TableSpan::CellLook look = table->cellLooks.value(r).value(c);
                cell.fill = cssColor(look.background);
                cell.ink = cssColor(look.color);
                cell.bold = look.bold;
            }

            // The cell the caret is in shows its source, as any element
            // under the caret does.
            const bool editing = view.active && d->m_anchor == d->m_cursor && cell.source.start <= d->m_cursor
                                 && d->m_cursor <= cell.source.end;
            QList<Range> hidden;
            for (int i = view.firstSyntax; i < view.lastSyntax && !editing; ++i) {
                const Range &marker = d->m_decorations.syntax.at(i);
                if (marker.overlaps(cell.source))
                    hidden.append(marker);
            }
            // An image in the cell - `![[p.png|40]]`, `![](p.png)`, `<img
            // src="p.png" width="40">` - is drawn inline, the way collectTextEmbeds
            // does in a paragraph: its first character stays as a transparent
            // carrier the image's width is reserved on, the rest is hidden.
            // The cell holding the caret shows the source instead.
            for (auto it = editing ? tableEmbeds.end()
                                   : std::lower_bound(tableEmbeds.begin(), tableEmbeds.end(), cell.source.start,
                                                      [](const EmbedSpan *embed, qsizetype start) {
                                                          return embed->range.start < start;
                                                      });
                 it != tableEmbeds.end() && (*it)->range.start < cell.source.end; ++it) {
                const EmbedSpan &embed = **it;
                if (embed.range.end > cell.source.end)
                    continue;
                if (!embed.html && !isImageSuffix(QFileInfo(embed.target).suffix().toLower()))
                    continue;
                EmbedCell built = buildEmbedCell(embed, columnCap - 2 * padX);
                if (built.image.isNull() && !(built.isFileCard && embed.html))
                    continue; // an unresolved Markdown image stays its source
                hidden.removeIf([&](const Range &marker) {
                    return marker.start >= embed.range.start && marker.end <= embed.range.end;
                });
                hidden.append({embed.range.start + 1, embed.range.end});
                cell.embeds.push_back(std::move(built));
            }
            // An entity stays one character even in the cell being edited.
            cell.map = DisplayMap::build(d->m_text, cell.source, hidden, d->m_decorations.substitutions);

            const QString full = cell.map.text();
            while (cell.lead < full.size() && full.at(cell.lead).isSpace())
                ++cell.lead;
            QString shown = full.trimmed();
            cell.layout = std::make_unique<QTextLayout>(shown);
            cell.layout->setCacheEnabled(true);
            QTextOption wrap;
            wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            cell.layout->setTextOption(wrap);
            QFont cellFont = font;
            if (cell.bold)
                cellFont.setWeight(QFont::Bold);
            else if (r < table->headerRows)
                cellFont.setWeight(QFont::DemiBold);
            cell.layout->setFont(cellFont);
            QList<QTextLayout::FormatRange> formats = formatsIn(view, cell.source, cell.map);
            const QFontMetricsF cellMetrics(cellFont);
            qreal imageExtra = 0;
            for (const EmbedCell &embed : cell.embeds) {
                const qreal carrier = cellMetrics.horizontalAdvance(d->m_text.at(embed.source.start));
                QTextCharFormat format;
                format.setForeground(Qt::transparent);
                format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
                format.setFontLetterSpacing(embed.rect.width() - carrier);
                formats.append({qMax(0, int(cell.map.toDisplay(embed.source.start)) - cell.lead), 1, format});
                imageExtra += embed.rect.width() - carrier;
            }
            cell.layout->setFormats(formats);

            natural[c] = qMax(natural.at(c),
                              cellMetrics.horizontalAdvance(shown) + imageExtra + 2 * padX);
            cells.push_back(std::move(cell));
        }
        // A row shorter than the header still draws its missing cells, as
        // Obsidian does; they have no source until a click fills the row in
        // (fillShortRow).
        for (int c = int(row.size()); c < table->columns; ++c) {
            Cell cell;
            cell.row = r;
            cell.column = c;
            cell.alignment = table->alignment.value(c, Qt::AlignLeft);
            cell.layout = std::make_unique<QTextLayout>(QString());
            cell.layout->setFont(font);
            cells.push_back(std::move(cell));
        }
    }

    QList<qreal> widths;
    QList<qreal> offsets;
    qreal x = 0;
    for (const qreal width : std::as_const(natural)) {
        const qreal used = qBound(minimum, width, columnCap);
        widths << used;
        offsets << x;
        x += used;
    }
    view.sideWidth = x;

    // Lay each cell out at its column width, then rows take the tallest cell.
    QList<qreal> rowHeights(table->rows.size(), lineHeight + 2 * padY);
    for (Cell &cell : cells) {
        const qreal width = widths.value(cell.column, minimum) - 2 * padX;
        cell.layout->beginLayout();
        qreal height = 0;
        while (true) {
            QTextLine line = cell.layout->createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(qMax<qreal>(10, width));
            // A line carrying an image taller than the text grows above the
            // text to hold it, as in a paragraph.
            qreal grow = 0;
            for (const EmbedCell &embed : cell.embeds) {
                const qsizetype carrier = qMax<qsizetype>(0, cell.map.toDisplay(embed.source.start) - cell.lead);
                if (carrier >= line.textStart() && carrier < line.textStart() + line.textLength())
                    grow = qMax(grow, embed.rect.height() - lineHeight);
            }
            line.setPosition(QPointF(0, height + grow));
            height += lineHeight + grow;
        }
        cell.layout->endLayout();
        for (EmbedCell &embed : cell.embeds) {
            const int carrier = qMax(0, int(cell.map.toDisplay(embed.source.start)) - cell.lead);
            const QTextLine line = cell.layout->lineForTextPosition(carrier);
            if (!line.isValid())
                continue;
            const qreal h = embed.rect.height();
            embed.rect.moveTo(line.cursorToX(carrier),
                              h > lineHeight ? line.y() + lineHeight - h : line.y() + (lineHeight - h) / 2);
        }
        rowHeights[cell.row] = qMax(rowHeights.at(cell.row), height + 2 * padY);
    }

    QList<qreal> rowTops;
    qreal y = 0;
    for (const qreal height : std::as_const(rowHeights)) {
        rowTops << y;
        y += height;
    }
    for (Cell &cell : cells) {
        cell.rect = QRectF(offsets.value(cell.column), rowTops.value(cell.row),
                           widths.value(cell.column), rowHeights.value(cell.row));
    }

    view.cells = std::move(cells);
    view.headerRows = table->headerRows;
    view.sideScroll = qBound<qreal>(0, view.sideScroll,
                                     qMax<qreal>(0, view.sideWidth - contentWidth()));
    view.layout.reset();
    view.height = y;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::paintTable(QPainter &painter, const BlockView &view, qreal left,
                                qreal top) const
{
    const QColor border = d->m_theme.color(QStringLiteral("--background-modifier-border"));
    const QColor header = d->m_theme.color(QStringLiteral("--background-secondary"));
    const QColor text = d->m_theme.color(QStringLiteral("--text-normal"));
    const bool strikeLines = d->m_theme.color(QStringLiteral("--strikethrough-decoration-color")).isValid();

    // A wide table is clipped to the readable column and scrolls inside it;
    // nothing spills over the note's text.
    const qreal overflow = qMax<qreal>(0, view.sideWidth - contentWidth());
    const QRectF window(left, top, contentWidth(), view.height);
    painter.save();
    if (overflow > 0)
        painter.setClipRect(window.adjusted(-1, -1, 1, 1));
    const qreal shift = -view.sideScroll;

    // A rectangle of selected cells (TableHandles.cpp), when it is this table's.
    QRect picked;
    if (d->m_cellSel.tableStart == view.span.range.start)
        picked = cellSelectionRect();
    QColor pickedFill = d->m_theme.color(QStringLiteral("--interactive-accent"));
    pickedFill.setAlphaF(0.22f);

    // Only the cells inside the rectangle being repainted: a GIF frame in one
    // cell repaints that cell's rect, not every cell of a long table. Null
    // outside a paint event (an export paints the whole table).
    const QRectF exposed = d->m_exposed;
    int painted = 0;
    for (const Cell &cell : view.cells) {
        const QRectF rect = cell.rect.translated(left + shift, top);
        if (rect.right() < window.left() - 2 || rect.left() > window.right() + 2)
            continue;
        if (!exposed.isNull() && (rect.bottom() < exposed.top() - 2 || rect.top() > exposed.bottom() + 2))
            continue;
        ++painted;
        if (cell.row < view.headerRows)
            painter.fillRect(rect, header);
        if (cell.fill.isValid())
            painter.fillRect(rect, cell.fill);
        painter.setPen(QPen(border, 1));
        painter.drawRect(rect);
        if (!picked.isNull() && picked.contains(cell.column, cell.row))
            painter.fillRect(rect.adjusted(1, 1, -1, -1), pickedFill);

        // A selection is drawn in each cell it reaches; one that runs across
        // cells tints the ones it covers whole, so it reads as a block.
        QList<QTextLayout::FormatRange> selected;
        const Range selection = this->selection();
        if (selection.length() > 0 && cell.source.isValid()
            && selection.start <= cell.source.end && cell.source.start <= selection.end) {
            QColor background = d->m_theme.color(QStringLiteral("--interactive-accent"));
            const bool whole = selection.start <= cell.source.start && cell.source.end <= selection.end;
            const bool across = selection.start < cell.source.start || cell.source.end < selection.end;
            if (whole && across) {
                background.setAlphaF(0.18f);
                painter.fillRect(rect.adjusted(1, 1, -1, -1), background);
            }
            const int length = int(cell.layout->text().size());
            const int from = qBound(0, int(cell.map.toDisplay(qMax(selection.start, cell.source.start))) - cell.lead, length);
            const int to = qBound(0, int(cell.map.toDisplay(qMin(selection.end, cell.source.end))) - cell.lead, length);
            if (to > from) {
                QTextLayout::FormatRange range;
                range.start = from;
                range.length = to - from;
                background.setAlphaF(0.33f);
                range.format.setBackground(background);
                selected.append(range);
            }
        }

        painter.setPen(cell.ink.isValid() ? cell.ink : text);
        // Each cell clips its own text, so a long word cannot bleed into the
        // next column.
        painter.save();
        painter.setClipRect(rect.adjusted(1, 1, -1, -1), Qt::IntersectClip);
        cell.layout->draw(&painter, cellTextOrigin(view, cell) + QPointF(left, top), selected);
        if (strikeLines)
            paintStrikeLines(painter, *cell.layout, cellTextOrigin(view, cell) + QPointF(left, top));
        if (!cell.embeds.empty()) {
            const QPointF origin = cellTextOrigin(view, cell);
            paintEmbedCellList(painter, cell.embeds, origin.x() + left, origin.y() + top);
        }
        painter.restore();
    }
    d->m_tableCellsPainted = painted;
    if (!picked.isNull()) {
        const auto cellAt = [&](int row, int column) {
            return view.cells[size_t(row) * size_t(tableFor(view)->columns) + size_t(column)].rect;
        };
        const QRectF box = cellAt(picked.top(), picked.left()).united(cellAt(picked.bottom(), picked.right()));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(d->m_theme.color(QStringLiteral("--interactive-accent")), 1.5));
        painter.drawRect(box.translated(left + shift, top));
    }
    painter.restore();

    paintSidewaysOverflow(painter, window, view.sideWidth, view.sideScroll, true, top + view.height + 2,
                          d->m_theme.color(QStringLiteral("--background-primary")));
}

void MarkdownEditor::paintSidewaysOverflow(QPainter &painter, const QRectF &window, qreal extent,
                                           qreal scroll, bool withBar, qreal barY,
                                           const QColor &background) const
{
    const qreal overflow = extent - contentWidth();
    if (overflow <= 0)
        return;

    // Edge fades say there is more, and a thin bar says where you are.
    const auto fade = [&](const QRectF &rect, bool fromLeft) {
        QLinearGradient gradient(fromLeft ? rect.topLeft() : rect.topRight(),
                                 fromLeft ? rect.topRight() : rect.topLeft());
        QColor opaque = background;
        QColor clear = background;
        clear.setAlpha(0);
        gradient.setColorAt(0, opaque);
        gradient.setColorAt(1, clear);
        painter.fillRect(rect, gradient);
    };
    if (scroll > 1)
        fade(QRectF(window.left(), window.top(), 18, window.height()), true);
    if (scroll < overflow - 1)
        fade(QRectF(window.right() - 18, window.top(), 18, window.height()), false);
    if (!withBar)
        return;

    const qreal fraction = contentWidth() / extent;
    const qreal barWidth = qMax<qreal>(28, contentWidth() * fraction);
    const qreal barX = window.right() - contentWidth() + (contentWidth() - barWidth) * (scroll / overflow);
    QColor bar = d->m_theme.color(QStringLiteral("--text-faint"));
    bar.setAlphaF(0.5f);
    painter.fillRect(QRectF(barX, barY, barWidth, 3), bar);
}

namespace {

constexpr qreal kMediaControls = 36;

QString clockOf(qint64 milliseconds)
{
    const qint64 seconds = qMax<qint64>(0, milliseconds / 1000);
    return seconds >= 3600 ? QStringLiteral("%1:%2:%3")
                                 .arg(seconds / 3600)
                                 .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
                                 .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                           : QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10,
                                                                          QLatin1Char('0'));
}

} // namespace

bool MarkdownEditor::layoutMedia(BlockView &view, const QString &path, bool video)
{
    view.mediaPath = path;
    view.mediaVideo = video;
    view.embedLabel = QFileInfo(path).fileName();
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = codeHeaderHeight();
    view.height = view.textTop + mediaScreenRect(view, 0, 0).height() + kMediaControls;
    hideWholeBlockText(view);
    view.laidOut = true;
    return true;
}

QRectF MarkdownEditor::mediaScreenRect(const BlockView &view, qreal left, qreal top) const
{
    if (!view.mediaVideo)
        return QRectF(left, top + view.textTop, contentWidth(), 0);
    const qreal height = qMin<qreal>(contentWidth() * 9.0 / 16.0, 420);
    return QRectF(left, top + view.textTop, contentWidth(), height);
}

QRectF MarkdownEditor::mediaButtonRect(const BlockView &view, qreal left, qreal top) const
{
    const qreal y = mediaScreenRect(view, left, top).bottom() + (kMediaControls - 24) / 2;
    return QRectF(left, y, 24, 24);
}

QRectF MarkdownEditor::mediaBarRect(const BlockView &view, qreal left, qreal top) const
{
    const qreal y = mediaScreenRect(view, left, top).bottom() + kMediaControls / 2 - 2;
    return QRectF(left + 36, y, qMax<qreal>(40, contentWidth() - 36 - 110), 4);
}

void MarkdownEditor::paintMedia(QPainter &painter, const BlockView &view, qreal left,
                                qreal top) const
{
    const bool current = d->m_media && d->m_media->path() == view.mediaPath;
    const bool playing = current && d->m_media->isPlaying();
    const QColor muted = d->m_theme.color(QStringLiteral("--text-muted"));
    const QColor accent = d->m_theme.color(QStringLiteral("--interactive-accent"));

    painter.save();
    const QRectF screen = mediaScreenRect(view, left, top);
    if (view.mediaVideo) {
        painter.fillRect(screen, Qt::black);
        const QImage frame = current ? d->m_media->frame() : QImage();
        if (!frame.isNull()) {
            // Letterboxed into the card, keeping the picture's own shape.
            const QSizeF fitted = QSizeF(frame.size()).scaled(screen.size(), Qt::KeepAspectRatio);
            const QRectF target(screen.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
            painter.drawImage(target, frame);
        } else {
            QPainterPath triangle;
            const QPointF c = screen.center();
            triangle.addPolygon(QPolygonF({c + QPointF(-14, -18), c + QPointF(20, 0), c + QPointF(-14, 18)}));
            painter.fillPath(triangle, QColor(255, 255, 255, 170));
        }
    }

    // Drawn, not an icon, so it matches the text at any size.
    const QRectF button = mediaButtonRect(view, left, top);
    QPainterPath glyph;
    if (!MediaPlayback::isAvailable()) {
        glyph.addRect(QRectF()); // nothing: the label below says what a click does
    } else if (playing) {
        glyph.addRect(QRectF(button.left() + 6, button.top() + 5, 4, 14));
        glyph.addRect(QRectF(button.left() + 14, button.top() + 5, 4, 14));
    } else {
        glyph.addPolygon(QPolygonF({button.topLeft() + QPointF(7, 5), button.topLeft() + QPointF(19, 12),
                                    button.topLeft() + QPointF(7, 19)}));
    }
    painter.fillPath(glyph, muted);

    QFont small = d->m_theme.uiFont();
    small.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    painter.setFont(small);
    painter.setPen(muted);
    const QRectF bar = mediaBarRect(view, left, top);
    if (!MediaPlayback::isAvailable()) {
        painter.drawText(QRectF(left, bar.center().y() - 10, contentWidth(), 20),
                         Qt::AlignVCenter | Qt::AlignLeft, tr("Click to open it with its own player"));
        painter.restore();
        return;
    }
    QColor track = muted;
    track.setAlphaF(0.3f);
    painter.fillRect(bar, track);
    const qint64 duration = current ? d->m_media->duration() : 0;
    const qint64 position = current ? d->m_media->position() : 0;
    if (duration > 0)
        painter.fillRect(QRectF(bar.left(), bar.top(), bar.width() * position / duration, bar.height()), accent);
    painter.drawText(QRectF(bar.right() + 10, bar.center().y() - 10, 100, 20),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     duration > 0 ? clockOf(position) + QStringLiteral(" / ") + clockOf(duration)
                                  : QStringLiteral("0:00"));
    painter.restore();
}

bool MarkdownEditor::handleMediaClick(const BlockView &view, const QPoint &point, qreal left,
                                      qreal top)
{
    if (view.mediaPath.isEmpty())
        return false;
    const QRectF bar = mediaBarRect(view, left, top).adjusted(0, -8, 0, 8);
    const bool onButton = mediaButtonRect(view, left, top).adjusted(-4, -4, 4, 4).contains(point)
                          || mediaScreenRect(view, left, top).contains(point);
    if (!onButton && !bar.contains(point))
        return false;
    if (!MediaPlayback::isAvailable()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(view.mediaPath));
        return true;
    }
    if (!d->m_media) {
        d->m_media = new MediaPlayback(this);
        // Only the playing card repaints as it plays, not the whole view.
        connect(d->m_media, &MediaPlayback::changed, this, [this] {
            const qreal scroll = verticalScrollBar()->value();
            for (const BlockView &block : d->m_blocks) {
                if (block.mediaPath == d->m_media->path()) {
                    viewport()->update(QRect(0, int(block.y - scroll) - 8, viewport()->width(),
                                             int(block.height) + 16));
                }
            }
        });
    }
    const bool current = d->m_media->path() == view.mediaPath;
    if (bar.contains(point) && current && d->m_media->duration() > 0) {
        const qreal fraction = qBound<qreal>(0, (point.x() - bar.left()) / bar.width(), 1);
        d->m_media->seek(qint64(fraction * d->m_media->duration()));
    } else if (current && d->m_media->isPlaying()) {
        d->m_media->pause();
    } else {
        d->m_media->play(view.mediaPath);
    }
    viewport()->update();
    return true;
}

void MarkdownEditor::paintEmbedChrome(QPainter &painter, const BlockView &view, qreal left,
                                      qreal top) const
{
    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    QPainterPath rounded;
    rounded.addRoundedRect(panel, 6, 6);
    painter.fillPath(rounded, d->m_theme.color(QStringLiteral("--background-primary-alt")));

    if (view.embedLabel.isEmpty())
        return;

    painter.save();
    QFont label = d->m_theme.uiFont();
    label.setPixelSize(d->m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    painter.setFont(label);
    painter.setPen(d->m_theme.color(QStringLiteral("--text-faint")));
    painter.drawText(QRectF(panel.left() + 12, panel.top() + 6, panel.width() - 24,
                            view.textTop),
                     Qt::AlignVCenter | Qt::AlignLeft, view.embedLabel);
    painter.restore();
}

qreal MarkdownEditor::sideExtent(const BlockView &view) const
{
    // A fence's pieces share one offset, so the widest of them bounds it.
    // ponytail: a piece not laid out yet has no width, so the extent can grow
    // as the fence scrolls into view; the offset is clamped on every read.
    const BlockView *piece = &firstPiece(view);
    qreal widest = 0;
    for (int i = 0; i < view.pieces; ++i)
        widest = qMax(widest, piece[i].sideWidth);
    return widest;
}

qreal MarkdownEditor::sideOffset(const BlockView &view) const
{
    return qBound<qreal>(0, firstPiece(view).sideScroll,
                         qMax<qreal>(0, sideExtent(view) - contentWidth()));
}

MarkdownEditor::BlockView *MarkdownEditor::sidewaysScrollableAt(const QPoint &viewportPoint)
{
    const qreal y = viewportPoint.y() + verticalScrollBar()->value();
    for (BlockView &view : d->m_blocks) {
        if (y < view.y || y > view.y + view.height + 6)
            continue;
        if (view.span.kind == BlockSpan::Kind::Code) {
            if (sideExtent(view) > contentWidth())
                return &view - view.piece;
        } else if (!view.cells.empty() && view.sideWidth > contentWidth()) {
            return &view;
        }
    }
    return nullptr;
}

void MarkdownEditor::layoutBlock(BlockView &view)
{
    ++d->m_layoutCount;
    view.touched = true;
    // Whatever this block drew rendered last time goes: a block being edited
    // shows its source, and a PDF, a video or a card left over from before
    // would be painted over that source and the blocks below it.
    view.isPdfEmbed = false;
    view.pdfCells.clear();
    view.pdfError.clear();
    view.mediaPath.clear();
    view.mediaVideo = false;
    view.imagePath.clear();
    view.cells.clear();
    view.sideWidth = 0;
    view.callout = nullptr;
    view.options = nullptr;
    view.propertyRows.clear();
    view.image = QPixmap();
    view.imageCentred = false;
    view.textTop = 0;
    view.folded = false;
    view.inlineMaths.clear();
    view.separators.clear();
    view.footnoteMarks.clear();
    view.tabHidden = false;
    view.tabGroup = -1;
    view.isQuery = false;
    view.queryRows.clear();
    view.queryCollapseRect = QRectF();
    view.querySortRect = QRectF();
    view.queryResultCountText.clear();
    view.isBase = false;
    view.baseViewName.clear();
    view.baseYAML.clear();
    view.baseRows.clear();
    view.CSVpath.clear();
    view.embedCells.clear();
    view.isFileCard = false;
    view.isHtml = false;
    view.isPluginBlock = false;
    view.codeBlockKey.clear();
    view.embedPanel = false;
    view.embedLabel.clear();
    view.inlineHtmlFormats.clear();

    // A tab group shows one of its fences under a strip of tabs, and hides the
    // rest along with the blank lines between them. With the caret anywhere
    // inside the group it shows every fence as it is instead, so the caret can
    // never be inside a tab nobody can see.
    if (d->m_mode != Mode::Source && !d->m_tabGroups.empty()) {
        if (const TabGroup *group = tabGroupAt(view.span.range.start)) {
            if (caretTabGroup() != group && view.span.range.start < group->hiddenEnd) {
                const CodeFenceSpan &shown =
                    d->m_decorations.codeFences.at(group->first + selectedTabOf(*group));
                if (view.span.kind == BlockSpan::Kind::Code && codeFenceOf(view) == &shown) {
                    view.tabGroup = int(group - d->m_tabGroups.data());
                } else {
                    view.tabHidden = true;
                    view.layout.reset();
                    view.map = DisplayMap();
                    view.height = 0;
                    view.laidOut = true;
                    view.heightEstimated = false;
                    return;
                }
            }
        }
    }

    // Reading view's own trailing block: not real source, laid out entirely
    // by layoutFootnotesList.
    if (view.isFootnotesList) {
        layoutFootnotesList(view);
        return;
    }
    // Reading view lists footnote definitions at the end instead of where
    // they sit in the source - hidden outright here, like a tab group's
    // fence nobody is looking at above.
    if (d->m_mode == Mode::Reading && footnoteDefinitionBlock(view)) {
        view.layout.reset();
        view.map = DisplayMap();
        view.height = 0;
        view.laidOut = true;
        view.heightEstimated = false;
        return;
    }

    // Obsidian never shows raw YAML in live preview, only in source mode, so
    // the properties table does not step aside for the caret the way a
    // paragraph does.
    if (view.span.kind == BlockSpan::Kind::Frontmatter && d->m_mode != Mode::Source
        && layoutProperties(view))
        return;

    const bool renderMedia = !plainSource(view) && !view.active;
    if (renderMedia && layoutQuery(view))
        return;
    if (renderMedia && layoutBase(view))
        return;
    if (renderMedia && layoutPluginCode(view))
        return;
    if (renderMedia && layoutHtml(view))
        return;
    if (renderMedia && layoutEmbed(view))
        return;
    if (renderMedia && layoutMath(view))
        return;

    // A table stays a table while the caret is in one of its cells: it is
    // edited where it is drawn (TableCells.cpp), not dropped to its source.
    const bool renderTable = view.span.kind == BlockSpan::Kind::Table && d->m_mode != Mode::Source
                             && (!view.active || (d->m_anchor == d->m_cursor && tableCellAt(d->m_cursor).row >= 0));
    if (renderTable && layoutTable(view))
        return;

    // `#| echo: false` hides the listing in live preview while the caret is
    // elsewhere - Quarto's own meaning, independent of eval. Read further
    // down, in the same `!reveal` pass every other per-block hide range goes
    // through; false here whenever that pass cannot even run (source mode,
    // or the caret already in the block, which reveals everything anyway).
    bool notebookEchoHidden = false;
    // Whether this block gets a run control and an output, given its `#|` options.
    const auto decideNotebookEligible = [this, &view](const CodeOptions *options) {
        if (!d->m_features.notebooks || !d->m_notebookRunner || view.piece != 0)
            return;
        const CodeFenceSpan *fence = codeFenceOf(view);
        // `#| eval: false` - never runs, so no run control and no cache
        // lookup either: the same "off" a null runner already gives.
        view.notebookEligible = fence && !fence->language.trimmed().isEmpty()
                                && !(options && !options->eval)
                                && (!d->m_notebookRunnable || d->m_notebookRunnable(fence->language));
    };
    if (renderMedia && view.span.kind == BlockSpan::Kind::Code) {
        view.options = codeOptionsFor(view);
        // Decided early, like view.options just above, because it feeds the
        // same "does this block get a header strip" decision below - the
        // output itself (appendNotebookOutput) is only built once the rest
        // of this block's own layout is settled, the same distance
        // appendEmbedRows keeps from layoutEmbed.
        decideNotebookEligible(view.options);
        notebookEchoHidden = view.options && !view.options->echo && !view.active;
    } else if (view.editedListing && view.span.kind == BlockSpan::Kind::Code) {
        // The caret opened this listing's fences: its text is plain (no
        // options, no media) but it stays runnable and keeps its output, so
        // clicking into it does not make the block lose its run control and
        // shift everything below. `#| eval: false` is read from the source,
        // view.options being null while the caret is in the block.
        decideNotebookEligible(codeOptionsFor(view));
    } else if (pureSource(view)) {
        // Decided under an earlier state (the tab shown with its run control,
        // before the caret came into the group): plain source has none.
        view.notebookEligible = false;
        view.notebookOutputs.clear();
    }

    const bool perElement = d->m_mode == Mode::LivePreview && view.active && !revealsWholeBlock(view);
    const bool reveal = plainSource(view)
                        || (d->m_mode == Mode::LivePreview && view.active && !perElement);
    view.revealed = perElement ? revealedSyntax(view) : QList<Range>();
    view.caretWord = view.active ? wordAtCaret() : Range{};
    QList<Range> hidden;
    // A folded list item hides its children whatever else the block shows.
    QList<Range> listFolds;
    hideFoldedListItems(view, listFolds);
    // Embeds inside this block that layoutEmbed did not already render whole
    // - several sharing a paragraph, or one mixed in with running text,
    // drawn inline; see collectTextEmbeds' comment in the header.
    QList<const EmbedSpan *> textEmbeds;
    if (!reveal) {
        hidden.reserve(view.lastSyntax - view.firstSyntax);
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
            const Range &marker = d->m_decorations.syntax.at(i);
            if (!view.revealed.contains(marker))
                hidden.append(marker);
        }
        // The parser always adds an inline formula's hide range; whether it
        // actually stays hidden depends on MicroTeX being able to draw it,
        // which only the editor knows.
        layoutInlineMaths(view, hidden);
        layoutInlineHtml(view, hidden);
        layoutAnchorSeparators(view, hidden);
        layoutFootnotes(view, hidden);
        hideFoldedNestedCallouts(view, hidden);
        hidden += listFolds;
        if (renderMedia && d->m_features.fileEmbeds)
            collectTextEmbeds(view, hidden, textEmbeds);
        if (notebookEchoHidden)
            hidden.append(view.span.range);
    }
    // An entity the caret touches is among `revealed`: its source is drawn.
    const QList<Range> openEntities = perElement ? revealedEntities() : QList<Range>();
    const QList<Substitution> withoutOpen = openEntities.isEmpty() ? QList<Substitution>()
                                                                   : substitutionsWithout(openEntities);
    view.map = reveal ? (listFolds.isEmpty() ? DisplayMap::identity(d->m_text, view.span.range)
                                             : DisplayMap::build(d->m_text, view.span.range, listFolds, {}))
                      : DisplayMap::build(d->m_text, view.span.range, hidden,
                                          openEntities.isEmpty() ? d->m_decorations.substitutions : withoutOpen);
    for (BlockView::InlineMath &formula : view.inlineMaths)
        formula.displayPos = view.map.toDisplay(formula.sourcePos);
    for (BlockView::AnchorSeparator &separator : view.separators)
        separator.displayPos = view.map.toDisplay(separator.sourcePos);
    for (BlockView::FootnoteMark &mark : view.footnoteMarks)
        mark.displayPos = view.map.toDisplay(mark.sourcePos);

    // QTextLayout lays out one paragraph and ignores "\n"; U+2028 is the
    // character it does break on. Same length, so every display offset the
    // DisplayMap computed still points at the same glyph.
    //
    // With "Strict line breaks" on, a newline inside a paragraph joins the
    // lines (CommonMark's rule), one between blocks does not, and two trailing
    // spaces still force a break. Off, the default, every newline breaks.
    QString shown = view.map.text();
    for (qsizetype i = 0; i < shown.size(); ++i) {
        if (shown.at(i) != QLatin1Char('\n'))
            continue;
        const qsizetype source = view.map.toSource(i);
        shown[i] = softBreakAt(source) ? QChar(QLatin1Char(' ')) : QChar(QChar::LineSeparator);
    }
    // The break that ends the block's last line is the block's own boundary,
    // not an empty line inside it.
    if (shown.endsWith(QChar::LineSeparator))
        shown.chop(1);

    view.callout = view.span.kind == BlockSpan::Kind::Callout && !plainSource(view) ? calloutFor(view) : nullptr;
    if (view.callout)
        view.calloutStyle = d->m_calloutStyles.resolve(view.callout->type, view.callout->attributes,
                                                    d->m_theme);
    // Callouts nest, and only the outermost one is a block. The rest are
    // painted inside this layout, so they have to be found before it is built:
    // each nested one indents the lines it covers.
    view.nested.clear();
    if (view.callout) {
        for (const CalloutSpan &callout : d->m_decorations.callouts) {
            // One that began in an earlier piece still covers lines of this.
            if (callout.depth > 0 && callout.range.overlaps(view.span.range))
                view.nested.push_back(
                    {&callout,
                     d->m_calloutStyles.resolve(callout.type, callout.attributes, d->m_theme)});
        }
    }

    // A table's source is monospace and its spaces unbreakable, so a wrapped
    // row breaks at the same column as the rows around it: Qt otherwise hangs
    // a run of spaces off the end of a line, and a padded row would start
    // its next line a few columns early. The same length, so offsets hold.
    if (view.span.kind == BlockSpan::Kind::Table)
        shown.replace(QLatin1Char(' '), QChar(QChar::Nbsp));
    // A heading is one line, or two with a setext underline, so a newline past
    // them inside its block is an Enter typed since the last parse: what follows
    // is the paragraph the parse is about to make of it. Drawn that way now -
    // the layout's own font, and so an empty last line's height and the caret's,
    // is the body's, and the heading font goes on the heading's lines alone - the
    // text below and the caret do not change by the difference between the two
    // when the parse lands.
    qsizetype strayAt = -1;
    if (view.span.kind == BlockSpan::Kind::Heading && !view.callout) {
        const QStringView block = QStringView(d->m_text).mid(view.span.range.start, view.span.range.length());
        const qsizetype firstEnd = block.indexOf(QLatin1Char('\n'));
        int lines = block.trimmed().startsWith(QLatin1Char('#')) ? 1 : 0;
        if (lines == 0 && firstEnd >= 0) {
            const QStringView rest = block.mid(firstEnd + 1);
            const QStringView underline = rest.left(rest.indexOf(QLatin1Char('\n')) < 0 ? rest.size()
                                                                                       : rest.indexOf(QLatin1Char('\n'))).trimmed();
            if (!underline.isEmpty() && (underline.front() == QLatin1Char('=') || underline.front() == QLatin1Char('-'))
                && std::all_of(underline.cbegin(), underline.cend(), [&](QChar c) { return c == underline.front(); }))
                lines = 2;
        }
        // Where the text after those lines starts, in the source and then as
        // drawn (a setext underline is not drawn, so the lines cannot be counted
        // in what is shown).
        qsizetype after = -1;
        int newlines = 0;
        for (qsizetype i = 0; i < block.size() && lines > 0; ++i) {
            if (block.at(i) == QLatin1Char('\n') && ++newlines == lines) {
                after = view.span.range.start + i + 1;
                break;
            }
        }
        if (after >= 0 && after < view.span.range.end) {
            const qsizetype display = view.map.toDisplay(after);
            if (display > 0 && display <= shown.size())
                strayAt = display;
        }
    }
    view.layout = std::make_unique<QTextLayout>(shown);
    view.layout->setCacheEnabled(true);
    // A callout may name its own body font, size and effects; everything else
    // takes the font its kind implies.
    view.layout->setFont(view.callout ? view.calloutStyle.bodyFont(d->m_theme)
                                      : fontFor(strayAt >= 0 ? BlockSpan{BlockSpan::Kind::Paragraph, {0, 0}, 0}
                                                             : view.span));

    QTextOption option;
    // A fence never reflows: its longest line scrolls sideways inside the panel
    // (Obsidian's reading view; its live preview wraps, which hides where a
    // diff's lines end). `#| wrap: true` is Quarto's `code-overflow: wrap`,
    // asking for the column back. Source mode keeps wrapping, there is no
    // panel to scroll in. A listing inside a callout is not Kind::Code and
    // wraps. A table's source wraps by the character, not after a pipe (see
    // above); everything else wraps inside the column.
    const CodeOptions *listing = view.span.kind == BlockSpan::Kind::Code ? codeOptionsFor(view) : nullptr;
    const bool sideways = view.span.kind == BlockSpan::Kind::Code && d->m_mode != Mode::Source
                          && !(listing && listing->wrap);
    option.setWrapMode(sideways                                    ? QTextOption::NoWrap
                       : view.span.kind == BlockSpan::Kind::Table ? QTextOption::WrapAnywhere
                                                                   : QTextOption::WrapAtWordBoundaryOrAnywhere);
    view.layout->setTextOption(option);
    QList<QTextLayout::FormatRange> formats = formatsFor(view);
    // A background on nothing but line breaks is a bar drawn at the end of a line:
    // a stale span (a setext underline's "=====" read as a highlight) can land
    // there once the hidden text around it has moved.
    formats.removeIf([&](const QTextLayout::FormatRange &range) {
        return range.format.background().style() != Qt::NoBrush && range.length > 0 && range.start >= 0
               && range.start + range.length <= shown.size()
               && std::all_of(shown.cbegin() + range.start, shown.cbegin() + range.start + range.length,
                              [](QChar c) { return c == QChar::LineSeparator; });
    });
    if (strayAt >= 0) {
        QTextCharFormat heading;
        heading.setFont(fontFor(view.span));
        formats.prepend({0, int(strayAt - 1), heading}); // under every style span, which refine it
    }
    for (const BlockView::InlineMath &formula : view.inlineMaths) {
        // The "$" DisplayMap left visible carries the image: invisible ink,
        // and letter spacing wide enough that the line reserves exactly the
        // room the paint step draws into.
        QTextCharFormat format;
        format.setForeground(Qt::transparent);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(formula.imageSize.width()
                                    - QFontMetricsF(view.layout->font())
                                          .horizontalAdvance(QLatin1Char('$')));
        formats.append({int(formula.displayPos), 1, format});
    }
    if (!view.separators.empty()) {
        // ponytail: measured in the block's font; a "#" inside bold or italic
        // text is a little off from the separator painted over it.
        const QFontMetricsF metrics(view.layout->font());
        QTextCharFormat format;
        format.setForeground(Qt::transparent);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(metrics.horizontalAdvance(QStringLiteral(" > "))
                                    - metrics.horizontalAdvance(QLatin1Char('#')));
        for (const BlockView::AnchorSeparator &separator : view.separators)
            formats.append({int(separator.displayPos), 1, format});
    }
    for (const BlockView::FootnoteMark &mark : view.footnoteMarks) {
        // Same trick: the carrier character reserves exactly the width the
        // number needs, in whichever of the two footnote fonts it paints in.
        const QFontMetricsF metrics(footnoteMarkFont(view.layout->font(), mark.superscript));
        QTextCharFormat format;
        format.setForeground(Qt::transparent);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(metrics.horizontalAdvance(mark.text)
                                    - QFontMetricsF(view.layout->font())
                                          .horizontalAdvance(d->m_text.at(mark.sourcePos)));
        formats.append({int(mark.displayPos), 1, format});
    }
    prepareTextEmbeds(view, textEmbeds, formats);
    view.layout->setFormats(formats);

    // A list's lines, and a list's inside a quote or callout outside source
    // mode, sit at their own item's text column - a wrapped line too.
    const bool listLines = view.span.kind == BlockSpan::Kind::List
                           || (d->m_mode != Mode::Source
                               && (view.span.kind == BlockSpan::Kind::Callout
                                   || view.span.kind == BlockSpan::Kind::Quote));
    if (listLines)
        layoutListColumns(view);
    else
        view.listTextX.clear();
    const qreal codeGutter = codeGutterWidth(view);
    const qreal lineHeight = view.callout && view.calloutStyle.size > 0
                                 ? view.calloutStyle.size * 1.5
                                 : lineHeightFor(view.span);
    const qreal bodyLineHeight = lineHeightFor({BlockSpan::Kind::Paragraph, {0, 0}, 0});
    view.layout->beginLayout();
    qreal height = 0;
    qreal widest = 0;
    while (true) {
        QTextLine line = view.layout->createLine();
        if (!line.isValid())
            break;
        const qsizetype source = view.map.toSource(line.textStart());
        const qreal listX = view.listTextX.empty() ? -1 : listTextAt(view, source);
        const qreal nesting = kCalloutIndent * nestingDepthAt(view, source);
        const qreal indent =
            nesting + (listX >= 0 ? listX
                                  : (view.span.kind == BlockSpan::Kind::List ? kListIndent : codeGutter));
        line.setLineWidth(contentWidth() - indent
                          - (view.callout ? 28 : 0));
        // A line carrying an inline embed taller than its text grows above
        // the text to hold it.
        const qreal grow = view.embedCells.empty()
                               ? 0
                               : qMax<qreal>(0, textEmbedHeightOn(view, line) - lineHeight);
        line.setPosition(QPointF(indent, height + grow));
        height += (strayAt >= 0 && line.textStart() >= strayAt ? bodyLineHeight : lineHeight) + grow;
        if (sideways)
            widest = qMax(widest, indent + line.naturalTextWidth());
    }
    view.layout->endLayout();
    view.height = qMax(height, lineHeight);
    // The extent the offset is clamped to (sideExtent) - 0 when every line fits.
    view.sideWidth = widest > contentWidth() ? widest + 8 : 0;

    // Obsidian's live preview puts one --p-spacing of air over a heading and
    // none under it; the blank line in the source is the rest of the rhythm.
    // Source mode has no block spacing at all: one line is one line.
    if (view.span.kind == BlockSpan::Kind::Heading && d->m_mode != Mode::Source)
        view.textTop = blockSpacing();

    if (view.callout) {
        // The title line is drawn by the callout header, so the body starts
        // below it; an empty callout still shows its title. A later piece
        // carries on the body.
        view.textTop = view.piece == 0 ? calloutHeaderHeight(view.calloutStyle) : 0;
        view.height = height;
    } else if (hasCodeHeader(view) && view.piece == 0) {
        view.textTop = codeHeaderHeight(); // the `#|` header, the tab strip, or the run control
    }

    // A folded fence folds all of its pieces; only the first keeps its header.
    view.folded = isFoldable(view)
                  && d->m_folds.value(firstPiece(view).span.range.start, defaultFoldOf(view));
    // `echo: false` collapses the listing the same way folding does (header
    // kept, body gone) but, unlike folding, still shows the output below -
    // see the `view.notebookEligible` block just under this one.
    view.height = (view.folded || notebookEchoHidden) ? 0 : view.height + codeCaptionHeight(view);
    view.height += view.textTop;
    if (view.folded)
        view.embedCells.clear();
    else
        placeTextEmbeds(view, lineHeight);
    // A notebook block's output, below its own listing - never in place of
    // it, unlike layoutPluginCode's fences. Only the first piece carries it
    // (view.notebookEligible is only ever set there), and a folded fence
    // shows nothing below its header, the same rule embeds follow.
    if (view.notebookEligible && !view.folded) {
        if (const CodeFenceSpan *fence = codeFenceOf(view)) {
            const Range content{codeContentOf(firstPiece(view), nullptr).start,
                                codeContentOf(lastPiece(view), nullptr).end};
            QString source = d->m_text.mid(content.start, content.length());
            if (source.endsWith(QLatin1Char('\n')))
                source.chop(1);
            // Which fence this is across edits: its place among the note's
            // fences (a block inserted above shifts it, which at worst shows
            // a neighbour's old output marked outdated).
            const QString blockID = QStringLiteral("f%1").arg(fence - d->m_decorations.codeFences.constData());
            appendNotebookOutput(view, fence->language, source, blockID, QString(), view.options,
                                 notebookDuplicateOrdinal(*fence));
        }
    }
    view.laidOut = true;
}

void MarkdownEditor::updateLayout()
{
    if (d->m_blocks.empty())
        return;

    // Obsidian keeps the caret's own block raw even when the pane loses
    // focus, so interaction - not focus - is what reveals it.
    const TabGroup *caretGroup = caretTabGroup();
    // The caret's block, or for a long fence its first piece: the caret
    // anywhere in a fence shows the whole of it as written.
    const qsizetype caretFirst =
        d->m_activeBlock >= 0 && d->m_activeBlock < qsizetype(d->m_blocks.size())
            ? d->m_activeBlock - d->m_blocks[size_t(d->m_activeBlock)].piece
            : -1;
    // A block shows its raw source only for a bare caret. With a selection it
    // stays rendered, as in Obsidian, and while the mouse button is held
    // nothing changes at all: revealing the syntax mid-drag moved the text out
    // from under the pointer, so the selection chased it.
    const bool bareCaret =
        d->m_mode == Mode::LivePreview && d->m_interacted && (d->m_vim.enabled || selection().length() == 0);

    // The block the edit button selected shows its source for as long as the
    // selection is exactly that block (HoverEdit.cpp).
    qsizetype sourceShown = -1;
    if (d->m_sourceReveal >= 0) {
        const qsizetype at = blockAt(d->m_sourceReveal);
        const Range selected = selection();
        if (d->m_mode == Mode::LivePreview && at >= 0 && d->m_blocks[size_t(at)].piece == 0
            && d->m_cursor == selected.start && d->m_sourceRange == selected)
            sourceShown = at;
        else
            d->m_sourceReveal = -1;
    }

    const CodeFenceSpan *fenceShown = bareCaret ? fenceAroundCaret() : nullptr;

    // The caret arriving in hidden text opens the folds hiding it, but only
    // once the user has touched the note: a restored caret and folds would
    // otherwise undo each other. A selection runs over folds without opening them.
    if (d->m_interacted && d->m_mode != Mode::Reading && d->m_cursor == d->m_anchor)
        openFoldsAt(d->m_cursor);

    // One pass over the note, which is the only part of this proportional to
    // its length: settle what each block reveals, and place it.
    d->m_movedFrom = qInf();
    // The empty line a note ending in a newline has under its last block.
    const qreal tail = d->m_mode != Mode::Reading && d->m_text.endsWith(QLatin1Char('\n'))
                           ? lineHeightFor({BlockSpan::Kind::Paragraph, {0, 0}, 0})
                           : 0;
    qreal y = d->m_topPadding;
    int foldLevel = 0; // the level of the folded heading whose section this is, 0 outside one
    const BlockView *above = nullptr; // the block before this one that takes room
    for (qsizetype i = 0; i < qsizetype(d->m_blocks.size()); ++i) {
        BlockView &view = d->m_blocks[size_t(i)];
        // A tab group with the caret in it is edited as plain source: its
        // strip and panels drawn over the fence lines would hide them. A
        // single listing whose fences the caret has opened is plain text too,
        // but keeps a panel drawn out over those lines (editedListingPanel).
        const bool editedAsSource = caretGroup && view.span.range.start >= caretGroup->span.start
                                    && view.span.range.start < caretGroup->span.end;
        const bool editedListing = !editedAsSource && fenceShown && view.span.kind == BlockSpan::Kind::Code
                                   && view.span.range.overlaps(fenceShown->block);
        if (editedAsSource != view.editedAsSource || editedListing != view.editedListing) {
            view.editedAsSource = editedAsSource;
            view.editedListing = editedListing;
            view.layout.reset();
            view.laidOut = false;
            view.heightEstimated = false;
        }
        // A fence's two lines, in the blocks around its listing, open with it.
        const bool inFence = bareCaret && fenceShown
                             && view.span.kind == BlockSpan::Kind::Paragraph
                             && view.span.range.overlaps(fenceShown->block);
        const bool active = d->m_pressing ? view.active
                                       : (bareCaret && i - view.piece == caretFirst) || i - view.piece == sourceShown
                                             || inFence;
        if (active != view.active) {
            view.active = active;
            view.layout.reset();
            view.laidOut = false;
            view.heightEstimated = false;
        } else if (active && view.laidOut
                   && ((!revealsWholeBlock(view) && revealedSyntax(view) != view.revealed)
                       || !(wordAtCaret() == view.caretWord))) {
            view.layout.reset();
            view.laidOut = false;
            view.heightEstimated = false;
        }

        // A folded heading hides every block up to the next heading of its
        // level or higher; the blocks keep their layouts and heights.
        if (view.span.kind == BlockSpan::Kind::Heading && view.span.level <= foldLevel)
            foldLevel = 0;
        view.foldHidden = foldLevel > 0;
        if (view.foldHidden) {
            view.y = y;
            continue;
        }
        if (above)
            y += blockGap(d->m_text, *above, view);
        if (view.y != y && d->m_movedFrom > y)
            d->m_movedFrom = y;
        view.y = y;
        if (!view.laidOut && !view.heightEstimated) {
            const qreal before = view.height;
            view.height = estimatedHeight(view);
            view.heightEstimated = true;
            if (view.height != before && d->m_movedFrom > y)
                d->m_movedFrom = y;
        }
        y += view.height + kBlockSpacing;
        above = &view;
        if (headingFolds(view))
            foldLevel = view.span.level;
    }
    d->m_totalHeight = y + d->m_topPadding + tail;

    // Lay out what is on screen, plus a screen either side so scrolling does
    // not stutter, plus the caret's block wherever it is. Blocks are placed in
    // order, so the first one near the screen is a binary search away.
    const qreal scroll = verticalScrollBar()->value();
    const qreal top = scroll - viewport()->height();
    const qreal bottom = scroll + 2 * viewport()->height();
    // Real heights move what follows them, which can bring blocks that were
    // out of range (by their estimates) on screen: the next pass lays out
    // what is now on screen itself - not the margins, which can wait for the
    // next call - until nothing moves. Once, those stayed unlaid, not drawn,
    // until something else called this again, which a resize always did.
    // ponytail: capped at eight passes; a note whose estimates are that far
    // off fills in on the next call.
    for (int pass = 0; pass < 8; ++pass) {
        const qreal passTop = pass == 0 ? top : scroll;
        const qreal passBottom = pass == 0 ? bottom : scroll + viewport()->height();
        const auto nearTop = std::partition_point(d->m_blocks.begin(), d->m_blocks.end(),
                                                  [passTop](const BlockView &view) {
                                                      return view.bottomY() < passTop;
                                                  });
        const qsizetype from = std::distance(d->m_blocks.begin(), nearTop);

        qsizetype firstChanged = -1;
        const auto layOut = [this, &firstChanged](qsizetype i) {
            BlockView &view = d->m_blocks[size_t(i)];
            if (view.laidOut || view.foldHidden)
                return;
            const qreal before = view.height;
            layoutBlock(view);
            if (!qFuzzyCompare(before + 1, view.height + 1) && (firstChanged < 0 || i < firstChanged))
                firstChanged = i;
        };
        // In index order: laying out a piece of a long fence can clear the
        // layouts of the pieces after it, and those have to be redone here.
        const bool caretAbove = d->m_activeBlock >= 0 && d->m_activeBlock < from;
        if (caretAbove)
            layOut(d->m_activeBlock);
        qsizetype i = from;
        for (; i < qsizetype(d->m_blocks.size()) && d->m_blocks[size_t(i)].y <= passBottom; ++i)
            layOut(i);
        if (d->m_activeBlock >= i && d->m_activeBlock < qsizetype(d->m_blocks.size()))
            layOut(d->m_activeBlock);

        if (firstChanged < 0)
            break;
        // Only what follows a block whose height changed moves.
        d->m_movedFrom = qMin(d->m_movedFrom, d->m_blocks[size_t(firstChanged)].y);
        // Nothing before it moved, so it stays where it is (never a hidden
        // block: those are not laid out).
        qreal y = d->m_blocks[size_t(firstChanged)].y;
        const BlockView *shown = nullptr;
        for (qsizetype j = firstChanged; j < qsizetype(d->m_blocks.size()); ++j) {
            BlockView &view = d->m_blocks[size_t(j)];
            if (view.foldHidden) {
                view.y = y;
                continue;
            }
            if (shown)
                y += blockGap(d->m_text, *shown, view);
            view.y = y;
            shown = &view;
            if (!view.laidOut && !view.heightEstimated) {
                view.height = estimatedHeight(view);
                view.heightEstimated = true;
            }
            y += view.height + kBlockSpacing;
        }
        d->m_totalHeight = y + d->m_topPadding + tail;
    }
    updateScrollRange();
    if (d->m_notebookInput.edit)
        positionNotebookInput(); // the prompt under a block follows it as it moves
}

QList<MarkdownEditor::BlockBox> MarkdownEditor::blockBoxes()
{
    updateLayout();
    QList<BlockBox> boxes;
    boxes.reserve(qsizetype(d->m_blocks.size()));
    for (const BlockView &view : d->m_blocks) {
        const Range &range = view.span.range;
        // A blank line draws nothing, so a panel reaching into it is not an
        // overlap; it is what keeps two panels apart.
        const qreal above = panelBleed(view, false);
        const qreal below = panelBleed(view, true);
        boxes.append({view.foldHidden || isBlankBlock(d->m_text, view)
                          ? QRectF()
                          : QRectF(0, view.y - above, contentWidth(), view.height + above + below),
                      view.span.kind, range});
    }
    return boxes;
}

namespace {

/// NOTOLITH_FRAME_LOG=<file>: one line per editor paint - the gap since the
/// last one, how long this one took, and what it covered - to see what a
/// device really scrolls at, in its own session, without a bench window.
struct FrameLog {
    QFile file;
    QElapsedTimer clock;
    qint64 last = -1;
    // Scroll steps since the last paint: how many, the time spent handling
    // them, and when the first one asked for this frame.
    int steps = 0;
    qint64 stepNs = 0;
    qint64 firstStep = -1;
    FrameLog()
    {
        const QString path = qEnvironmentVariable("NOTOLITH_FRAME_LOG");
        if (path.isEmpty())
            return;
        file.setFileName(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            return;
        file.write("# gap_ms paint_ms exposed_wxh viewport_wxh dpr steps step_ms wait_ms\n");
        clock.start();
    }
    void record(qint64 start, qint64 end, const QRect &exposed, const QSize &viewport, qreal dpr)
    {
        const double gap = last < 0 ? 0.0 : double(start - last) / 1e6;
        last = start;
        const double wait = firstStep < 0 ? 0.0 : double(start - firstStep) / 1e6;
        file.write(QStringLiteral("%1 %2 %3x%4 %5x%6 %7 %8 %9 %10\n")
                       .arg(gap, 0, 'f', 2)
                       .arg(double(end - start) / 1e6, 0, 'f', 2)
                       .arg(exposed.width()).arg(exposed.height())
                       .arg(viewport.width()).arg(viewport.height())
                       .arg(dpr)
                       .arg(steps)
                       .arg(double(stepNs) / 1e6, 0, 'f', 2)
                       .arg(wait, 0, 'f', 2)
                       .toLatin1());
        file.flush();
        steps = 0;
        stepNs = 0;
        firstStep = -1;
    }
};

FrameLog &frameLog()
{
    static FrameLog log;
    return log;
}

} // namespace

void MarkdownEditor::scrollContentsBy(int dx, int dy)
{
    FrameLog &log = frameLog();
    const qint64 stepStart = log.file.isOpen() ? log.clock.nsecsElapsed() : 0;
    const auto logged = qScopeGuard([&] {
        if (!log.file.isOpen())
            return;
        ++log.steps;
        log.stepNs += log.clock.nsecsElapsed() - stepStart;
        if (log.firstStep < 0)
            log.firstStep = stepStart;
    });

    // The property editor is a widget over the table, and would stay put
    // while the table moved away under it.
    finishPropertyEdit(true);
    pruneGIFs(); // a GIF that scrolled out of view stops here, not a frame later
    // Blocks scrolling into reach need their real layout; the rest of what
    // updateLayout() settles does not change with the scroll position, so a
    // note already laid out around the new position is left alone. That pass
    // runs over the whole note, and a finger sends a step per touch event.
    const qreal scroll = verticalScrollBar()->value();
    const auto reach = std::partition_point(d->m_blocks.cbegin(), d->m_blocks.cend(),
                                            [top = scroll - viewport()->height()](const BlockView &view) {
                                                return view.bottomY() < top;
                                            });
    const auto beyond = std::partition_point(reach, d->m_blocks.cend(),
                                             [bottom = scroll + 2 * viewport()->height()](const BlockView &view) {
                                                 return view.y <= bottom;
                                             });
    const bool laidOut = std::all_of(reach, beyond, [](const BlockView &view) {
        return view.laidOut || view.foldHidden;
    });
    if (!laidOut)
        updateLayout();
    else if (d->m_notebookInput.edit)
        positionNotebookInput(); // the prompt under a block follows it
    // The block under a stationary pointer changes as the content moves: the
    // ring and button follow their block, so re-aim them (HoverEdit.cpp). A
    // finger flicking the page has no pointer resting anywhere.
    d->m_hoverRect.translate(dx, dy);
    const bool flicking = QScroller::hasScroller(viewport())
                          && QScroller::scroller(viewport())->state() != QScroller::Inactive;
    if (!flicking && (d->m_hoverEditButton || d->m_tableHandles || !d->m_hoverRect.isNull())) {
        if (viewport()->underMouse())
            updateHover(viewport()->mapFromGlobal(QCursor::pos()));
        else
            clearHover();
    }

    // A wheel notch moves most of the screen by a few lines, and unless the
    // layout just moved something that is on screen those pixels are still
    // right: move them, and paint only the strip that came in. An OpenGL
    // viewport has no backing store to move pixels in, so it repaints.
    const bool settled = laidOut || d->m_movedFrom > scroll + viewport()->height();
    const bool raster = !qobject_cast<QOpenGLWidget *>(viewport());
    if (dx == 0 && qAbs(dy) < viewport()->height() && settled && raster)
        viewport()->scroll(0, dy, viewport()->rect());
    else
        viewport()->update();
}

void MarkdownEditor::updateScrollRange()
{
    const int page = viewport()->height();
    verticalScrollBar()->setPageStep(page);
    verticalScrollBar()->setSingleStep(qMax(1, int(QFontMetricsF(font()).height())));
    verticalScrollBar()->setRange(0, qMax(0, int(d->m_totalHeight) - page));
}

// --- painting -------------------------------------------------------------

void MarkdownEditor::paintBlocks(QPainter &painter, qreal scroll, qreal exposedTop,
                                 qreal exposedBottom) const
{
    const qreal left = contentLeft();
    const QColor text = d->m_theme.color(QStringLiteral("--text-normal"));
    const QColor selectionColour = d->m_theme.color(QStringLiteral("--interactive-accent"));
    const bool strikeLines = d->m_theme.color(QStringLiteral("--strikethrough-decoration-color")).isValid();

    const Range selected = selection();
    painter.setPen(text);

    const auto firstExposed = std::partition_point(
        d->m_blocks.cbegin(), d->m_blocks.cend(), [scroll, exposedTop](const BlockView &view) {
            return view.bottomY() - scroll < exposedTop;
        });
    // A listing being edited has its panel drawn first, behind everything: it
    // reaches out over the fence lines of the paragraphs either side, which
    // paint after it. One block back catches a listing that ends just above
    // the exposed strip, one fence line of slack one that starts just below it.
    for (auto it = firstExposed == d->m_blocks.cbegin() ? firstExposed : std::prev(firstExposed);
         it != d->m_blocks.cend(); ++it) {
        const qreal top = it->y - scroll;
        if (top > exposedBottom + 2 * lineHeightFor({BlockSpan::Kind::Paragraph, {0, 0}, 0}))
            break;
        if (it->editedListing && !it->tabHidden && !it->foldHidden)
            paintEditedListingPanel(painter, *it, left, top);
    }

    for (auto it = firstExposed; it != d->m_blocks.cend(); ++it) {
        const BlockView &view = *it;
        const qreal top = view.y - scroll;
        if (top > exposedBottom)
            break;
        if (view.tabHidden || view.foldHidden)
            continue;

        if (view.span.kind == BlockSpan::Kind::Frontmatter && d->m_mode != Mode::Source
            && !d->m_properties.isEmpty()) {
            paintProperties(painter, view, left, top);
            painter.setPen(text);
            continue;
        }

        if (view.isFootnotesList) {
            paintFootnotesList(painter, view, left, top);
            continue;
        }

        if (view.callout) {
            paintCallout(painter, view, left, top);
            painter.setPen(view.calloutStyle.color.isValid() ? view.calloutStyle.color : text);
        } else {
            painter.setPen(text);
        }

        // A `---` rule, its characters hidden, is a line across the text
        // column - until the caret is on it and it shows as typed.
        if (view.span.kind == BlockSpan::Kind::Rule && d->m_mode != Mode::Source && !view.active) {
            const qreal y = std::round(top + view.height / 2.0) + 0.5;
            painter.save();
            painter.setPen(QPen(d->m_theme.color(QStringLiteral("--background-modifier-border")), 2));
            painter.drawLine(QPointF(left, y), QPointF(left + contentWidth(), y));
            painter.restore();
            continue;
        }

        if (view.span.kind == BlockSpan::Kind::Code && !view.isQuery && !view.isBase && !view.isPluginBlock
            && !plainSource(view)) {
            paintCodeChrome(painter, view, left, top);
            if (view.notebookEligible && !view.folded)
                paintNotebookOutput(painter, view, left, top);
            painter.setPen(text);
        } else if (view.editedListing && view.span.kind == BlockSpan::Kind::Code && !view.isQuery
                   && !view.isBase && !view.isPluginBlock) {
            // The panel is already down (above); the run control and the last
            // output stay, as they were before the caret came in.
            paintNotebookRunControl(painter, view, left, top);
            if (view.notebookEligible && !view.folded)
                paintNotebookOutput(painter, view, left, top);
            painter.setPen(text);
        }

        if (view.folded)
            continue; // the header is all a folded block shows

        QList<QTextLayout::FormatRange> selectionRanges;
        // Vim's block selection is several column strips, not one span.
        const bool strips = d->m_vim.enabled && !d->m_vim.overrideBlocks.isEmpty() && selected.length() > 0
                            && d->m_cursor == d->m_vim.overrideCursor && d->m_anchor == d->m_vim.overrideAnchor;
        const QList<Range> parts = strips ? d->m_vim.overrideBlocks : QList<Range>{selected};
        for (const Range &part : parts) {
            if (part.length() <= 0 || !part.overlaps(view.span.range))
                continue;
            const Range shown = view.map.toDisplay(
                {qMax(part.start, view.span.range.start),
                 qMin(part.end, view.span.range.end)});
            if (shown.length() > 0) {
                QTextLayout::FormatRange range;
                range.start = int(shown.start);
                range.length = int(shown.length());
                QColor background = selectionColour;
                background.setAlphaF(0.33f);
                range.format.setBackground(background);
                selectionRanges.append(range);
            }
        }
        appendFindHighlights(view, selectionRanges);
        // Several embeds sharing this block, or one mixed in with running
        // text: drawn inline, under the text's invisible carriers.
        paintEmbedCells(painter, view, left, top);

        if (view.embedPanel) {
            paintEmbedChrome(painter, view, left, top);
            // A runnable code-file embed (NotebookEmbed part of
            // FileEmbedBlock.cpp): the same run control and output cells a
            // fenced block gets, just reached from the embed-panel branch
            // rather than the Kind::Code one, since an embed's span.kind is
            // Paragraph.
            if (view.notebookEligible) {
                paintNotebookRunControl(painter, view, left, top);
                if (!view.folded)
                    paintNotebookOutput(painter, view, left, top);
            }
        }
        if (view.isPdfEmbed) {
            paintPdfEmbed(painter, view, left, top);
            continue;
        }
        if (view.isFileCard) {
            paintFileCard(painter, view, left, top);
            continue;
        }
        if (!view.mediaPath.isEmpty()) {
            paintMedia(painter, view, left, top);
            continue;
        }

        if (!view.image.isNull()) {
            const qreal imageX =
                view.imageCentred ? left + (contentWidth() - view.imageSize.width()) / 2.0 : left;
            const QRectF target(imageX, top + view.textTop, view.imageSize.width(),
                                view.imageSize.height());
            painter.drawPixmap(target.toRect(), GIFframe(painter, view.imagePath, target, view.image));
            continue;
        }
        if (view.isBase && !view.cells.empty()) {
            paintBase(painter, view, left, top);
            continue;
        }
        if (!view.cells.empty()) {
            paintTable(painter, view, left, top);
            continue;
        }
        if (view.layout) {
            const qreal indent = view.callout ? 14 : (view.embedPanel ? 2 : 0);
            const qreal drop = view.textTop;
            if (d->m_mode != Mode::Source) {
                // Behind the text, outermost first: a nested callout's panel,
                // then any listing's panel on top of it.
                paintNestedCallouts(painter, view, left, top);
                if (view.span.kind != BlockSpan::Kind::Code)
                    paintNestedCodePanels(painter, view, left, top);
            }
            if (d->m_indentGuides && !view.listTextX.empty())
                paintIndentGuides(painter, view, left, top + drop);
            // A listing with a line wider than the column scrolls inside its
            // panel: the text moves and is clipped to the window, the
            // gutter's numbers (and the margin's) stay where they are.
            const bool clip = view.span.kind == BlockSpan::Kind::Code && sideExtent(view) > contentWidth();
            const qreal gutter = clip ? codeGutterWidth(view) : 0;
            const qreal textLeft = left + indent - (clip ? sideOffset(view) : 0);
            if (clip) {
                painter.save();
                painter.setClipRect(QRectF(left + gutter, top, contentWidth() - gutter, view.height));
            }
            view.layout->draw(&painter, QPointF(textLeft, top + drop), selectionRanges);
            if (strikeLines)
                paintStrikeLines(painter, *view.layout, QPointF(textLeft, top + drop));
            paintInlineMaths(painter, view, textLeft, top + drop);
            paintAnchorSeparators(painter, view, textLeft, top + drop);
            paintFootnoteMarks(painter, view, textLeft, top + drop);
            paintListMarkers(painter, view, textLeft, top + drop);
            paintFoldMarks(painter, view, left, top);
            if (clip)
                painter.restore();
            paintGutter(painter, view, top + drop);
            if (clip && !view.folded) {
                const LanguageStyle style = codeStyleOf(view);
                const qreal bottom = top + view.height - codeCaptionHeight(view);
                paintSidewaysOverflow(painter, QRectF(left + gutter, top + view.textTop, contentWidth() - gutter,
                                                      bottom - top - view.textTop),
                                      sideExtent(view), sideOffset(view), view.piece == view.pieces - 1,
                                      bottom + 3,
                                      style.bg.isValid()
                                          ? style.bg
                                          : d->m_theme.color(QStringLiteral("--background-primary-alt")));
            }
        }
    }
}

void MarkdownEditor::paintEvent(QPaintEvent *event)
{
    FrameLog &log = frameLog();
    const qint64 paintStart = log.file.isOpen() ? log.clock.nsecsElapsed() : 0;
    const auto logged = qScopeGuard([&] {
        if (log.file.isOpen())
            log.record(paintStart, log.clock.nsecsElapsed(), event->rect(), viewport()->size(),
                       viewport()->devicePixelRatioF());
    });
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    d->m_exposed = QRectF(event->rect());
    const auto leftPaint = qScopeGuard([this] { d->m_exposed = QRectF(); });
    // After a scroll only the strip that came into view is exposed; blocks
    // outside it are skipped rather than drawn and clipped away. The margin
    // covers what a block paints past its own edges, like a code panel's.
    const QRect exposed = event->rect();
    const qreal exposedTop = exposed.top() - 16;
    const qreal exposedBottom = exposed.bottom() + 16;
    painter.fillRect(exposed, d->m_theme.color(QStringLiteral("--background-primary")));

    paintBlocks(painter, verticalScrollBar()->value(), exposedTop, exposedBottom);
    paintEditHover(painter);
    paintTableChrome(painter);
    paintRemoteCarets(painter);

    d->m_caretPainted = QRect();
    if (hasFocus() && d->m_cursorOn && d->m_mode != Mode::Reading) {
        const QRectF caret = caretRect();
        if (!caret.isEmpty()) {
            if (d->m_vim.enabled && d->m_vim.mode != VimState::Mode::Insert) {
                QColor block = d->m_theme.color(QStringLiteral("--text-normal"));
                block.setAlphaF(0.55f);
                painter.fillRect(caret, block);
            } else {
                painter.setPen(QPen(d->m_theme.color(QStringLiteral("--text-normal")), 2));
                painter.drawLine(caret.topLeft() + QPointF(1, 0), caret.bottomLeft() + QPointF(1, 0));
            }
            d->m_caretPainted = caret.toAlignedRect().adjusted(-2, -2, 2, 2);
        }
    }
    paintSelectionHandles(painter);
}

qsizetype MarkdownEditor::anchorOffset() const
{
    return d->m_anchor;
}

void MarkdownEditor::setRemoteCarets(const QList<RemoteCaret> &carets)
{
    if (carets.isEmpty() && d->m_remoteCarets.isEmpty())
        return;
    // Only where a caret was and is: a peer moving its caret ten times a
    // second repaints two small boxes, not the page.
    QRegion dirty;
    const auto add = [&](const QList<RemoteCaret> &list) {
        for (const RemoteCaret &caret : list)
            dirty += remoteCaretRect(caret);
    };
    add(d->m_remoteCarets);
    d->m_remoteCarets = carets;
    add(d->m_remoteCarets);
    viewport()->update(dirty);
}

QRect MarkdownEditor::remoteCaretRect(const RemoteCaret &caret) const
{
    qreal height = 0;
    const QPointF point = pointFor(qBound<qsizetype>(0, caret.head, d->m_text.size()), &height);
    if (height <= 0)
        height = QFontMetricsF(font()).height();
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.75);
    const QFontMetricsF metrics(small);
    return QRectF(point.x() - 2, point.y() - metrics.height() - 1, metrics.horizontalAdvance(caret.name) + 10,
                  height + metrics.height() + 3)
        .toAlignedRect();
}

QList<MarkdownEditor::RemoteCaret> MarkdownEditor::remoteCarets() const
{
    return d->m_remoteCarets;
}

// ponytail: the caret and a name tag; another person's selection is not tinted.
void MarkdownEditor::paintRemoteCarets(QPainter &painter) const
{
    if (d->m_remoteCarets.isEmpty() || d->m_mode == Mode::Reading)
        return;
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.75);
    const QFontMetricsF metrics(small);
    painter.save();
    painter.setFont(small);
    for (const RemoteCaret &caret : d->m_remoteCarets) {
        const qsizetype head = qBound<qsizetype>(0, caret.head, d->m_text.size());
        qreal height = 0;
        const QPointF point = pointFor(head, &height);
        if (point.isNull() && head != 0)
            continue; // folded away, or not laid out
        if (height <= 0)
            height = QFontMetricsF(font()).height();
        painter.setPen(QPen(caret.color, 2));
        painter.drawLine(point, point + QPointF(0, height));
        const QRectF tag(point.x(), point.y() - metrics.height(), metrics.horizontalAdvance(caret.name) + 6,
                         metrics.height());
        painter.fillRect(tag, caret.color);
        painter.setPen(caret.color.lightnessF() > 0.6 ? Qt::black : Qt::white);
        painter.drawText(tag, Qt::AlignCenter, caret.name);
    }
    painter.restore();
}

QPointF MarkdownEditor::cursorPoint() const
{
    return pointFor(d->m_cursor);
}

QRectF MarkdownEditor::caretRect() const
{
    qreal height = 0;
    const QPointF point = pointFor(d->m_cursor, &height);
    if (point.isNull() && d->m_cursor != 0)
        return {};
    if (height <= 0) // no laid-out line under it: one line of the body text
        height = QFontMetricsF(font()).height();
    if (d->m_vim.enabled && d->m_vim.mode != VimState::Mode::Insert) {
        // Normal and visual mode: a block over the character the next
        // command acts on, as wide as that character is drawn.
        qreal width = QFontMetricsF(font()).averageCharWidth();
        if (d->m_cursor < d->m_text.size() && d->m_text.at(d->m_cursor) != QLatin1Char('\n')) {
            const QPointF after = pointFor(d->m_cursor + 1);
            if (!after.isNull() && qAbs(after.y() - point.y()) < 1 && after.x() > point.x())
                width = after.x() - point.x();
        }
        return QRectF(point.x(), point.y(), width, height);
    }
    return QRectF(point.x() - 1, point.y(), 2, height);
}

bool MarkdownEditor::belowLastBlock(qsizetype offset) const
{
    // The newline that ends a block is the block's own and is not drawn as a
    // line, so the empty line after a note's last one has no block to be in.
    return offset >= d->m_text.size() && d->m_text.endsWith(QLatin1Char('\n'))
           && d->m_mode != Mode::Reading && !d->m_blocks.empty() && !d->m_blocks.back().foldHidden;
}

QPointF MarkdownEditor::pointFor(qsizetype offset, qreal *height) const
{
    if (belowLastBlock(offset)) {
        const BlockView &last = d->m_blocks.back();
        if (height)
            *height = QFontMetricsF(fontFor({BlockSpan::Kind::Paragraph, {0, 0}, 0})).height();
        return QPointF(contentLeft(), last.y + last.height - verticalScrollBar()->value());
    }
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return {};
    const BlockView &view = d->m_blocks.at(size_t(index));
    if (view.foldHidden)
        return {}; // a caret restored inside a fold has nowhere to be drawn
    const Cell *cell = nullptr;
    for (const Cell &candidate : view.cells) {
        if (candidate.source.isValid() && candidate.source.start <= offset && offset <= candidate.source.end)
            cell = &candidate;
    }
    if (cell && cell->layout) {
        const int length = int(cell->layout->text().size());
        const int display = qBound(0, int(cell->map.toDisplay(offset)) - cell->lead, length);
        const QTextLine line = cell->layout->lineForTextPosition(display);
        const QPointF origin = cellTextOrigin(view, *cell);
        if (height && line.isValid())
            *height = line.height();
        return QPointF(contentLeft() + origin.x() + (line.isValid() ? line.cursorToX(display) : 0),
                       view.y - verticalScrollBar()->value() + origin.y() + (line.isValid() ? line.y() : 0));
    }
    if (!view.layout)
        return QPointF(contentLeft(), view.y - verticalScrollBar()->value());

    const qsizetype display = view.map.toDisplay(offset);
    const QTextLine line = view.layout->lineForTextPosition(int(display));
    if (!line.isValid())
        return QPointF(contentLeft(), view.y - verticalScrollBar()->value());

    const qreal x = line.cursorToX(int(display))
                    - (view.span.kind == BlockSpan::Kind::Code ? sideOffset(view) : 0);
    if (height)
        *height = line.height();
    return QPointF(contentLeft() + x,
                   view.y + view.textTop - verticalScrollBar()->value() + line.y());
}

// --- geometry and hit testing --------------------------------------------

qsizetype MarkdownEditor::offsetAt(const QPoint &viewportPoint) const
{
    const qreal scroll = verticalScrollBar()->value();
    const qreal y = viewportPoint.y() + scroll;
    const qreal x = viewportPoint.x() - contentLeft();
    if (d->m_blocks.empty())
        return d->m_text.size();
    // The empty line after the note's final newline: no block holds it.
    if (belowLastBlock(d->m_text.size()) && y >= d->m_blocks.back().y + d->m_blocks.back().height)
        return d->m_text.size();

    // The last block that shows: a click below the end lands in it, not in
    // a folded section's hidden tail.
    const BlockView *lastShown = &d->m_blocks.back();
    while (lastShown != d->m_blocks.data() && lastShown->foldHidden)
        --lastShown;
    for (const BlockView &view : d->m_blocks) {
        if (view.foldHidden)
            continue;
        if (!view.cells.empty()) {
            if (y < view.y || y > view.y + view.height)
                continue;
            // Clicking a cell puts the caret on the character under the
            // pointer, and the table is edited where it is drawn.
            for (const Cell &cell : view.cells) {
                // A cell with no source - a base's, or one a short row lacks
                // - has no character to land on; the block's start does.
                if (!cell.source.isValid() || !cell.rect.translated(-view.sideScroll, view.y).contains(QPointF(x, y)))
                    continue;
                const QPointF origin = cellTextOrigin(view, cell) + QPointF(0, view.y);
                for (int i = 0; cell.layout && i < cell.layout->lineCount(); ++i) {
                    const QTextLine line = cell.layout->lineAt(i);
                    if (y <= origin.y() + line.y() + line.height() || i == cell.layout->lineCount() - 1) {
                        const qsizetype source = cell.map.toSource(line.xToCursor(x - origin.x()) + cell.lead);
                        return qBound(cell.source.start, source, cell.source.end);
                    }
                }
                return cell.source.start;
            }
            return view.span.range.start;
        }
        if (!view.layout) {
            // Drawn as an image - display maths, HTML: a click on it opens
            // its source, as it does in Obsidian, instead of falling through
            // to the block below.
            if (y >= view.y && y <= view.y + view.height)
                return view.span.range.start;
            continue;
        }
        const bool last = &view == lastShown;
        if (y > view.y + view.height + kBlockSpacing && !last)
            continue;
        if (view.folded)
            return view.span.range.start;

        const qreal textTop = view.y + view.textTop;
        for (int i = 0; i < view.layout->lineCount(); ++i) {
            const QTextLine line = view.layout->lineAt(i);
            const bool lastLine = i == view.layout->lineCount() - 1;
            if (y <= textTop + line.y() + line.height() || (lastLine && last)) {
                const qreal shift = view.span.kind == BlockSpan::Kind::Code ? sideOffset(view) : 0;
                return view.map.toSource(line.xToCursor(x + shift));
            }
        }
        return view.span.range.end;
    }
    return d->m_text.size();
}

QString MarkdownEditor::linkAt(const QPoint &viewportPoint) const
{
    // Asked on every mouse move, so only the block under the pointer is
    // searched, not every span in the note.
    const qsizetype offset = offsetAt(viewportPoint);
    const qsizetype index = blockAt(offset);
    if (index < 0)
        return {};
    const BlockView &view = d->m_blocks[size_t(index)];
    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = d->m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::Link || !span.range.contains(offset))
            continue;
        // The whole target, "Note#Heading" when the link names one.
        if (span.anchorText.isValid() && span.anchorText.length() > 0)
            return span.payload + QLatin1Char('#') + d->m_text.mid(span.anchorText.start, span.anchorText.length());
        return span.payload;
    }
    return {};
}

void MarkdownEditor::updateLinkHover(const QPoint &viewportPoint, bool control,
                                     const QPoint &globalPos)
{
    // "Note#Heading" when the link names one, so the preview opens there.
    QString target = linkAt(viewportPoint);
    // Only a link to a note has a page to preview.
    if (target.contains(QLatin1String("://")) || target.startsWith(QLatin1String("mailto:")))
        target.clear();
    if (!target.isEmpty() && !(d->m_mode == Mode::Reading || control))
        target.clear();
    if (target == d->m_previewLink)
        return;
    d->m_previewLink = target;
    if (target.isEmpty())
        emit linkHoverEnded();
    else
        emit linkHovered(target, globalPos);
}

void MarkdownEditor::leaveEvent(QEvent *event)
{
    if (!d->m_previewLink.isEmpty()) {
        d->m_previewLink.clear();
        emit linkHoverEnded();
    }
    clearHover();
    QAbstractScrollArea::leaveEvent(event);
}

void MarkdownEditor::ensureCursorVisible()
{
    if (belowLastBlock(d->m_cursor)) {
        const BlockView &last = d->m_blocks.back();
        const qreal line = QFontMetricsF(fontFor({BlockSpan::Kind::Paragraph, {0, 0}, 0})).height();
        QScrollBar *bar = verticalScrollBar();
        if (last.y + last.height + line > bar->value() + viewport()->height())
            bar->setValue(int(last.y + last.height + line - viewport()->height()));
        return;
    }
    const qsizetype index = blockAt(d->m_cursor);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()))
        return;
    // Laid out first if a relayout dropped it: without its lines only the
    // block's top is known, and a caret deep in a long list would scroll the
    // view to the top of the list (a word added to the dictionary did that).
    if (!d->m_blocks[size_t(index)].laidOut)
        layoutBlock(d->m_blocks[size_t(index)]);
    const BlockView &view = d->m_blocks.at(size_t(index));
    const qreal lineHeight = QFontMetricsF(view.layout ? view.layout->font() : font()).height();

    const qsizetype display = view.map.toDisplay(d->m_cursor);
    qreal y = view.y;
    if (view.layout) {
        const QTextLine line = view.layout->lineForTextPosition(int(display));
        if (line.isValid()) {
            y += view.textTop + line.y();
            // A fence that scrolls sideways keeps the caret inside its window
            // too, the way revealCaretCell does for a table's cell.
            const qreal extent = view.span.kind == BlockSpan::Kind::Code ? sideExtent(view) : 0;
            if (extent > contentWidth()) {
                const qreal gutter = codeGutterWidth(view);
                const qreal x = line.cursorToX(int(display));
                qreal scroll = sideOffset(view);
                if (x - gutter < scroll + 24)
                    scroll = x - gutter - 24;
                else if (x > scroll + contentWidth() - 24)
                    scroll = x - contentWidth() + 24;
                BlockView &first = d->m_blocks[size_t(index - view.piece)];
                scroll = qBound<qreal>(0, scroll, extent - contentWidth());
                if (!qFuzzyCompare(scroll + 1, first.sideScroll + 1)) {
                    first.sideScroll = scroll;
                    viewport()->update();
                }
            }
        }
    }

    QScrollBar *bar = verticalScrollBar();
    if (y < bar->value())
        bar->setValue(int(y));
    else if (y + lineHeight > bar->value() + viewport()->height())
        bar->setValue(int(y + lineHeight - viewport()->height()));
}

void MarkdownEditor::resizeEvent(QResizeEvent *event)
{
    finishPropertyEdit(true);
    QAbstractScrollArea::resizeEvent(event);
    // Also called for the viewport's own resize (a scrollbar showing).
    relayoutIfWidthChanged();
    positionFindBar();
    pruneGIFs();
}

void MarkdownEditor::hideEvent(QHideEvent *event)
{
    clearGIFs(); // nothing plays in a hidden editor; the next paint starts what is on screen again
    QAbstractScrollArea::hideEvent(event);
}

// --- cursor ---------------------------------------------------------------

void MarkdownEditor::setCursorOffset(qsizetype offset)
{
    // A jump (the outline, a link, a search) into a fold opens it. Reading
    // view draws no caret to hide, and a restored note sets its folds after.
    if (d->m_mode != Mode::Reading && openFoldsAt(qBound<qsizetype>(0, offset, d->m_text.size())))
        updateLayout();
    moveCursor(offset, false);
}

void MarkdownEditor::moveCursor(qsizetype offset, bool keepAnchor)
{
    const qsizetype clamped = qBound<qsizetype>(0, offset, d->m_text.size());
    const qsizetype previousCursor = d->m_cursor;
    const qsizetype previousBlock = d->m_activeBlock;
    const bool hadSelection = d->m_anchor != d->m_cursor;

    d->m_cursor = clamped;
    if (!keepAnchor)
        d->m_anchor = clamped;

    d->m_activeBlock = blockAt(d->m_cursor);
    // Out of a table it edited: that table's source is aligned, the way
    // Advanced Tables does it, so the file reads as a table too.
    // Not while a selection is being dragged out of it: that would drop it.
    if (d->m_tableEdited && d->m_alignTables && !keepAnchor && tableAround(previousCursor) != tableAround(d->m_cursor)) {
        d->m_tableEdited = false;
        formatTables(previousCursor);
    }
    // Into another cell of the same table: the cell it left draws rendered
    // again and the one it entered shows its source.
    const CellRef cell = tableCellAt(d->m_cursor);
    const Range caretCell = cell.row >= 0 ? cell.range : Range{-1, -1};
    if (!(caretCell == d->m_caretCell)) {
        for (const qsizetype block : {previousBlock, d->m_activeBlock}) {
            // A table laid out as source (the caret was outside its cells
            // for a moment, as an edit that rewrites it leaves it) has no
            // cells either, and is the one that must be drawn as a grid now.
            if (block < 0 || block >= qsizetype(d->m_blocks.size())
                || (d->m_blocks[size_t(block)].cells.empty() && d->m_blocks[size_t(block)].span.kind != BlockSpan::Kind::Table))
                continue;
            BlockView &view = d->m_blocks[size_t(block)];
            view.laidOut = false;
            view.heightEstimated = false;
        }
        d->m_caretCell = caretCell;
        updateLayout();
        revealCaretCell();
    }
    // The two blocks swap between raw and rendered - and so does one block
    // when a selection starts or collapses inside it, since only a bare caret
    // reveals source.
    if (d->m_activeBlock != previousBlock || hadSelection != (d->m_anchor != d->m_cursor)) {
        updateLayout();
    } else if (d->m_activeBlock >= 0 && d->m_activeBlock < qsizetype(d->m_blocks.size())) {
        // Within the block it was in: what the caret now touches (an element's
        // markers, an entity) may differ, which relays out this block alone.
        const BlockView &view = d->m_blocks[size_t(d->m_activeBlock)];
        if (view.active && view.laidOut && !revealsWholeBlock(view)
            && (revealedSyntax(view) != view.revealed || !(wordAtCaret() == view.caretWord)))
            updateLayout();
    }

    ensureCursorVisible();
    d->m_cursorOn = true;
    viewport()->update();
    emit cursorMoved(d->m_cursor);
}

void MarkdownEditor::moveVertically(int lines, bool keepAnchor)
{
    const QPointF point = cursorPoint();
    const qreal x = d->m_goalX >= 0 ? d->m_goalX : point.x();
    const qreal lineHeight = QFontMetricsF(font()).height() * 1.5;
    const QPoint target(int(x), int(point.y() + lines * lineHeight + lineHeight / 2));

    const qreal goal = x;
    const qsizetype landed = offsetAt(target);
    moveCursor(keepAnchor ? landed : snapToTableCell(landed, d->m_cursor), keepAnchor);
    d->m_goalX = goal;
}

Range MarkdownEditor::selection() const
{
    if (d->m_vim.overrideRange.isValid() && d->m_cursor == d->m_vim.overrideCursor && d->m_anchor == d->m_vim.overrideAnchor)
        return d->m_vim.overrideRange;
    return {qMin(d->m_cursor, d->m_anchor), qMax(d->m_cursor, d->m_anchor)};
}

// --- editing --------------------------------------------------------------

void MarkdownEditor::pushUndo()
{
    if (d->m_vim.hold) // a vim insert session: one step, taken when it began
        return;
    // Typing bursts collapse into one undo step, as in every other editor.
    // A command that invalidated the timer is saying it is not part of one.
    if (!d->m_undo.isEmpty() && d->m_lastEdit.isValid() && d->m_lastEdit.elapsed() < kTypingBurstMs) {
        d->m_lastEdit.restart();
        return;
    }
    d->m_undo.append({d->m_text, d->m_cursor});
    if (d->m_undo.size() > 500)
        d->m_undo.removeFirst();
    d->m_redo.clear();
    d->m_lastEdit.restart();
}

void MarkdownEditor::shiftRanges(qsizetype at, qsizetype delta)
{
    // Between a keystroke and the reparse that follows it, everything that
    // speaks source offsets has to agree about them: a block below the edit
    // keeps its cached layout, so its map moves with the text instead of being
    // rebuilt, and every decoration a layout or a paint can consult moves too.
    // Once, only some of them did, and a block laid out or clicked in those
    // 20 ms read the others at the wrong offsets.
    const auto shift = [at, delta](Range &range) {
        if (range.start >= at)
            range.start += delta;
        if (range.end >= at)
            range.end += delta;
    };
    for (BlockView &view : d->m_blocks) {
        if (view.span.range.start >= at)
            view.map.shift(delta);
        shift(view.span.range);
    }
    // Text typed where a marker ends is after it, not in it: an empty list
    // item's first letter would be hidden with the "- " until the parse.
    const auto shiftMarker = [at, delta](Range &range) {
        if (range.start >= at)
            range.start += delta;
        if (range.end > at || (range.end == at && delta < 0))
            range.end += delta;
    };
    for (Range &range : d->m_decorations.syntax)
        shiftMarker(range);
    for (Range &range : d->m_decorations.metaNotes) // a jump or a delete in these 20 ms
        shift(range);
    for (Range &range : d->m_decorations.uncounted) // a selection count in these 20 ms
        shift(range);
    for (StyleSpan &span : d->m_decorations.styles)
        shift(span.range);
    // m_decorations.blocks is not shifted: only rebuildBlocks reads it, and
    // only straight after a parse has replaced it.
    for (Property &property : d->m_properties)
        shift(property.range);
    for (ListItemSpan &item : d->m_decorations.listItems) {
        shift(item.range);
        shiftMarker(item.marker);
        shiftMarker(item.checkbox);
    }
    for (CalloutSpan &callout : d->m_decorations.callouts) {
        shift(callout.range);
        shiftMarker(callout.marker);
        shift(callout.titleRange);
    }
    for (TableSpan &table : d->m_decorations.tables) {
        shift(table.range);
        for (Range &row : table.rowElements)
            shift(row);
        for (QList<Range> &row : table.cellElements) {
            for (Range &cell : row)
                shift(cell);
        }
        for (QList<Range> &row : table.rows) {
            // Typed at a cell's very start, or into an empty cell, the text
            // is the cell's: it grows rather than moving off.
            for (Range &cell : row) {
                if (cell.start > at || (cell.start == at && delta < 0))
                    cell.start += delta;
                if (cell.end >= at)
                    cell.end += delta;
            }
        }
    }
    for (Range &paragraph : d->m_decorations.paragraphs)
        shift(paragraph);
    if (!d->m_decorations.substitutions.isEmpty()) {
        // One the edit removed is gone; text typed just after one is not
        // swallowed by it, so its end moves only when past the edit.
        if (delta < 0) {
            d->m_decorations.substitutions.removeIf([at, delta](const Substitution &s) {
                return s.range.overlaps({at + delta, at});
            });
        }
        for (Substitution &substitution : d->m_decorations.substitutions) {
            if (substitution.range.start >= at)
                substitution.range.start += delta;
            if (substitution.range.end > at)
                substitution.range.end += delta;
        }
    }
    for (HtmlTextSpan &span : d->m_decorations.HTMLtext) {
        // Typed at its very start, the text is the element's, as for a cell.
        if (span.content.start > at || (span.content.start == at && delta < 0))
            span.content.start += delta;
        if (span.content.end >= at)
            span.content.end += delta;
        if (span.openTag.start >= at)
            span.openTag.start += delta;
        if (span.openTag.end > at)
            span.openTag.end += delta;
    }
    for (EmbedSpan &embed : d->m_decorations.embeds)
        shift(embed.range);
    for (MathSpan &math : d->m_decorations.maths) {
        shift(math.range);
        shift(math.formula);
    }
    for (CodeOptions &options : d->m_decorations.codeOptions) {
        shift(options.block);
        shift(options.range);
    }

    for (CodeFenceSpan &fence : d->m_decorations.codeFences) {
        shift(fence.block);
        shift(fence.content);
        shift(fence.attributesRange);
    }
    for (TabGroup &group : d->m_tabGroups) {
        shift(group.span);
        if (group.hiddenEnd >= at)
            group.hiddenEnd += delta;
    }
    if (delta != 0 && !d->m_selectedTabs.isEmpty()) {
        QHash<qsizetype, int> moved;
        for (auto it = d->m_selectedTabs.cbegin(); it != d->m_selectedTabs.cend(); ++it)
            moved.insert(it.key() >= at ? it.key() + delta : it.key(), it.value());
        d->m_selectedTabs = std::move(moved);
    }

    // So are a PDF embed's slide and zoom, which an edit above must not reset.
    const auto remap = [at, delta](auto &hash) {
        if (delta == 0 || hash.isEmpty())
            return;
        std::remove_reference_t<decltype(hash)> moved;
        for (auto it = hash.cbegin(); it != hash.cend(); ++it)
            moved.insert(it.key() >= at ? it.key() + delta : it.key(), it.value());
        hash = std::move(moved);
    };
    remap(d->m_pdfZoom);
    remap(d->m_pdfSlideIndex);
    remap(d->m_pdfNavLast);

    // Fold state is keyed by the block's start, so it has to move with it.
    if (delta != 0 && !d->m_folds.isEmpty()) {
        QHash<qsizetype, bool> moved;
        moved.reserve(d->m_folds.size());
        for (auto it = d->m_folds.cbegin(); it != d->m_folds.cend(); ++it)
            moved.insert(it.key() >= at ? it.key() + delta : it.key(), it.value());
        d->m_folds = std::move(moved);
    }
}

void MarkdownEditor::applyEdit(qsizetype at, qsizetype removed, const QString &inserted)
{
    const bool breaksLines = inserted.contains(QLatin1Char('\n'))
                             || QStringView(d->m_text).mid(at, removed).contains(QLatin1Char('\n'));
    // A letter typed on the empty line after a table or a callout that ends the
    // note: that block would take it in as a part of itself until the parse.
    const bool onTheTail = removed == 0 && !inserted.isEmpty() && at == d->m_text.size() && belowLastBlock(at)
                           && (d->m_blocks.back().span.kind == BlockSpan::Kind::Table || d->m_blocks.back().callout);
    d->m_text.remove(at, removed);
    d->m_text.insert(at, inserted);
    for (RemoteCaret &caret : d->m_remoteCarets) { // they follow their text, as vim's marks do
        for (qsizetype *offset : {&caret.anchor, &caret.head}) {
            if (*offset >= at + removed)
                *offset += inserted.size() - removed;
            else if (*offset > at)
                *offset = at + inserted.size();
        }
    }
    if (d->m_vim.enabled && !d->m_vim.marks.isEmpty()) { // vim's marks follow their text
        for (qsizetype &mark : d->m_vim.marks) {
            if (mark >= at + removed)
                mark += inserted.size() - removed;
            else if (mark > at)
                mark = at;
        }
    }

    // The caret's block is shown raw, so a stale decoration inside it changes
    // nothing on screen; every other block is untouched by this edit. Shifting
    // the offsets is therefore enough until the debounced parse lands.
    ++d->m_textVersion;
    d->m_lines.applyEdit(at, removed, inserted); // the gutter needs current line starts
    const qsizetype delta = inserted.size() - removed;
    // shiftRanges already moves the end of the block the edit is in; moving it
    // here as well once made that block run on into the next one, drawn twice
    // for the 20 ms until the reparse. An edit that keeps the length still
    // changes the text, so the block relays out either way.
    shiftRanges(at + removed, delta);
    // The open title's callout moves with the text, by shiftRanges' own rule.
    if (d->m_calloutTitleEdit && d->m_calloutTitleAt >= at + removed)
        d->m_calloutTitleAt += delta;
    const qsizetype index = blockAt(at);
    if (index >= 0 && index < qsizetype(d->m_blocks.size())) {
        BlockView &view = d->m_blocks[size_t(index)];
        view.layout.reset();
        view.laidOut = false;
        view.heightEstimated = false;
        view.codeEndValid = false;
    }

    if (!d->m_applyingExternal) {
        d->m_cursor = d->m_anchor = at + inserted.size();
        d->m_activeBlock = blockAt(d->m_cursor);
        if (tableAround(d->m_cursor) >= 0 && !d->m_formattingTables)
            d->m_tableEdited = true;
    }

    setModified(true);
    // A newline changes what the blocks around it are (a heading ends, a list
    // item begins), which no shifted offset can say: until the parse lands the
    // stale blocks are drawn a little differently, and the text visibly jumps
    // when it does. So the reader's own newline is parsed now, before anything
    // is drawn - while parses are cheap: a key may cost a few milliseconds, and
    // this document's last parse says what one costs on this machine. Edits
    // from elsewhere (another pane, a peer, a command's loop) keep the
    // debounce, so a burst of them is one parse; a parse already running is
    // left to land.
    // ponytail: past the budget the debounced pool parse and the jump stay -
    // softened for headings in layoutBlock; a parse of the edited block alone
    // would remove it.
    const bool parsedNow = (breaksLines || onTheTail) && d->m_typing && !d->m_applyingExternal && !d->m_formattingTables
                           && !d->m_parsePending && d->m_lastParseNs < kSyncParseBudgetNs;
    if (parsedNow)
        reparse();
    else
        updateLayout();
    if (!d->m_applyingExternal) {
        ensureCursorVisible();
        emit edited(at, removed, inserted);
    }
    if (!parsedNow)
        d->m_parseTimer.start(); // the real parse catches up when typing pauses
    // Only while the find bar is actually searching: a keystroke with no
    // query must not pay for a document scan.
    if (!d->m_find.pattern.isEmpty())
        updateFindMatches();
    emit textChanged();
    viewport()->update();
}

void MarkdownEditor::insertText(const QString &text)
{
    if (d->m_mode == Mode::Reading || text.isEmpty())
        return;
    pushUndo();

    const Range selected = selection();
    applyEdit(selected.start, selected.length(), text);
}

void MarkdownEditor::removeSelectionOrChar(int direction)
{
    if (d->m_mode == Mode::Reading)
        return;

    const Range selected = selection();
    Range toRemove = selected;
    if (selected.length() == 0) {
        if (direction < 0 && d->m_cursor > 0)
            toRemove = {d->m_cursor - 1, d->m_cursor};
        else if (direction > 0 && d->m_cursor < d->m_text.size())
            toRemove = {d->m_cursor, d->m_cursor + 1};
        else
            return;
        // An entity drawn as one character goes as one, never half of it.
        if (const Substitution *substitution = shownSubstitution(toRemove.start))
            toRemove = substitution->range;
    }

    pushUndo();
    applyEdit(toRemove.start, toRemove.length(), QString());
}

void MarkdownEditor::undo()
{
    if (d->m_undo.isEmpty())
        return;
    d->m_redo.append({d->m_text, d->m_cursor});
    const Snapshot snapshot = d->m_undo.takeLast();
    const qsizetype before = d->m_text.size();
    const QStringList folds = takeFolds();
    d->m_text = snapshot.text;
    ++d->m_textVersion;
    d->m_cursor = d->m_anchor = qBound<qsizetype>(0, snapshot.cursor, d->m_text.size());
    setModified(true);
    reparse();
    applyFoldState(folds);
    emit textChanged();
    emit edited(0, before, d->m_text);
    viewport()->update();
}

void MarkdownEditor::redo()
{
    if (d->m_redo.isEmpty())
        return;
    d->m_undo.append({d->m_text, d->m_cursor});
    const Snapshot snapshot = d->m_redo.takeLast();
    const qsizetype before = d->m_text.size();
    const QStringList folds = takeFolds();
    d->m_text = snapshot.text;
    ++d->m_textVersion;
    d->m_cursor = d->m_anchor = qBound<qsizetype>(0, snapshot.cursor, d->m_text.size());
    setModified(true);
    reparse();
    applyFoldState(folds);
    emit textChanged();
    emit edited(0, before, d->m_text);
    viewport()->update();
}

MarkdownEditor::Hunk MarkdownEditor::hunkBetween(const QString &from, const QString &to)
{
    const qsizetype most = qMin(from.size(), to.size());
    // Halves of a pair never part: one emoji may share its first half with another.
    const auto splits = [](const QString &text, qsizetype at) {
        return at > 0 && at < text.size() && text.at(at - 1).isHighSurrogate() && text.at(at).isLowSurrogate();
    };
    qsizetype head = 0;
    while (head < most && from.at(head) == to.at(head))
        ++head;
    if (splits(from, head) || splits(to, head))
        --head;
    qsizetype tail = 0;
    while (tail < most - head && from.at(from.size() - 1 - tail) == to.at(to.size() - 1 - tail))
        ++tail;
    if (tail > 0 && (splits(from, from.size() - tail) || splits(to, to.size() - tail)))
        --tail;
    return {head, from.size() - head - tail, to.mid(head, to.size() - head - tail)};
}

void MarkdownEditor::applyExternalEdit(qsizetype at, qsizetype removed, const QString &inserted)
{
    at = qBound<qsizetype>(0, at, d->m_text.size());
    removed = qBound<qsizetype>(0, removed, d->m_text.size() - at);
    const auto follow = [](qsizetype offset, qsizetype at, qsizetype removed, qsizetype inserted) {
        if (offset >= at + removed)
            return offset + inserted - removed;
        return offset > at ? at + inserted : offset;
    };
    // Each step is the text as it was before some local edits; those edits
    // are one region (hunkBetween) of the text as it is now. The edit from
    // elsewhere lands before that region or after it, and is made in the step
    // too. Landing inside it, the step cannot say what it would have been, and
    // nor can any step before it, which holds that region and more.
    const auto carry = [&](QList<Snapshot> &steps) {
        const qsizetype oldest = qMax<qsizetype>(0, steps.size() - kCarriedSteps);
        for (qsizetype i = steps.size() - 1; i >= oldest; --i) {
            Snapshot &step = steps[i];
            const Hunk own = hunkBetween(step.text, d->m_text);
            qsizetype where = at;
            if (at + removed > own.at) {
                if (at < own.at + own.inserted.size()) {
                    steps.remove(0, i + 1);
                    return;
                }
                where = at + own.removed - own.inserted.size();
            }
            step.text.replace(where, removed, inserted);
            step.cursor = follow(step.cursor, where, removed, inserted.size());
        }
        steps.remove(0, oldest);
    };
    const bool whole = at == 0 && removed == d->m_text.size();
    if (whole) {
        d->m_undo.clear();
        d->m_redo.clear();
    } else {
        carry(d->m_undo);
        carry(d->m_redo);
    }
    const qsizetype cursor = follow(d->m_cursor, at, removed, inserted.size());
    const qsizetype anchor = follow(d->m_anchor, at, removed, inserted.size());
    if (whole) {
        // An undo or redo over there: the whole text, as undo() takes it. The
        // callouts may be gone or new, so an open title has nowhere to land.
        finishCalloutTitleEdit(false);
        const QStringList folds = takeFolds();
        d->m_text = inserted;
        ++d->m_textVersion;
        d->m_cursor = d->m_anchor = qMin(d->m_cursor, d->m_text.size());
        setModified(true);
        reparse();
        applyFoldState(folds);
        emit textChanged();
    } else {
        d->m_applyingExternal = true;
        applyEdit(at, removed, inserted);
        d->m_applyingExternal = false;
    }
    d->m_cursor = qMin(cursor, d->m_text.size());
    d->m_anchor = qMin(anchor, d->m_text.size());
    d->m_activeBlock = blockAt(d->m_cursor);
    // An open title keeps what was typed: written back to its (moved) callout
    // now, since the field no longer sits over the text it was opened on.
    finishCalloutTitleEdit(true);
    viewport()->update();
}

// --- events ---------------------------------------------------------------

const QList<MarkdownEditor::Command> &MarkdownEditor::commands()
{
    // Obsidian's ids where it has the command; its own name for the few it
    // does not. Obsidian binds fewer of these by default - the keys are the
    // ones this editor always had.
    static const QList<Command> table = {
        {QStringLiteral("editor:toggle-bold"), tr("Toggle bold"), {QKeySequence(Qt::CTRL | Qt::Key_B)}},
        {QStringLiteral("editor:toggle-italics"), tr("Toggle italics"), {QKeySequence(Qt::CTRL | Qt::Key_I)}},
        {QStringLiteral("editor:toggle-highlight"), tr("Toggle highlight"),
         {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H)}},
        {QStringLiteral("editor:toggle-strikethrough"), tr("Toggle strikethrough"), {}},
        {QStringLiteral("editor:toggle-code"), tr("Toggle code"), {}},
        {QStringLiteral("editor:toggle-inline-math"), tr("Toggle inline math"), {}},
        {QStringLiteral("editor:toggle-comments"), tr("Toggle comment"), {}},
        {QStringLiteral("editor:insert-link"), tr("Insert Markdown link"), {QKeySequence(Qt::CTRL | Qt::Key_K)}},
        {QStringLiteral("editor:toggle-checklist-status"), tr("Toggle checkbox status"),
         {QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)}},
        {QStringLiteral("noteAppC:checkbox-state"), tr("Set checkbox state..."), {}},
        {QStringLiteral("editor:toggle-fold"), tr("Toggle fold on the current line"), {}},
        {QStringLiteral("editor:fold-all"), tr("Fold all headings and lists"), {}},
        {QStringLiteral("editor:unfold-all"), tr("Unfold all headings and lists"), {}},
        {QStringLiteral("editor:fold-more"), tr("Fold more"), {}},
        {QStringLiteral("editor:fold-less"), tr("Fold less"), {}},
        {QStringLiteral("editor:toggle-fold-properties"), tr("Toggle Properties fold"), {}},
        {QStringLiteral("editor:open-search"), tr("Search current file"), {QKeySequence(Qt::CTRL | Qt::Key_F)}},
        {QStringLiteral("editor:open-search-replace"), tr("Search and replace in current file"),
         {QKeySequence(Qt::CTRL | Qt::Key_H)}},
        {QStringLiteral("editor:swap-line-up"), tr("Move line up"), {QKeySequence(Qt::ALT | Qt::Key_Up)}},
        {QStringLiteral("editor:swap-line-down"), tr("Move line down"), {QKeySequence(Qt::ALT | Qt::Key_Down)}},
        {QStringLiteral("noteAppC:duplicate-lines"), tr("Duplicate line"),
         {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)}},
        {QStringLiteral("noteAppC:insert-table"), tr("Insert table"), {}},
        {QStringLiteral("noteAppC:edit-block-source"), tr("Edit block source"), {}},
        {QStringLiteral("noteAppC:format-table"), tr("Format table"), {}},
        {QStringLiteral("noteAppC:format-all-tables"), tr("Format all tables"), {}},
        {QStringLiteral("noteAppC:table-insert-row-before"), tr("Table: Insert row before"), {}},
        {QStringLiteral("noteAppC:table-insert-row-after"), tr("Table: Insert row after"), {}},
        {QStringLiteral("noteAppC:table-delete-row"), tr("Table: Delete row"), {}},
        {QStringLiteral("noteAppC:table-move-row-up"), tr("Table: Move row up"), {}},
        {QStringLiteral("noteAppC:table-move-row-down"), tr("Table: Move row down"), {}},
        {QStringLiteral("noteAppC:table-insert-column-before"), tr("Table: Insert column before"), {}},
        {QStringLiteral("noteAppC:table-insert-column-after"), tr("Table: Insert column after"), {}},
        {QStringLiteral("noteAppC:table-delete-column"), tr("Table: Delete column"), {}},
        {QStringLiteral("noteAppC:table-move-column-left"), tr("Table: Move column left"), {}},
        {QStringLiteral("noteAppC:table-move-column-right"), tr("Table: Move column right"), {}},
        {QStringLiteral("noteAppC:table-align-column-left"), tr("Table: Align column left"), {}},
        {QStringLiteral("noteAppC:table-align-column-center"), tr("Table: Align column center"), {}},
        {QStringLiteral("noteAppC:table-align-column-right"), tr("Table: Align column right"), {}},
        {QStringLiteral("noteAppC:table-duplicate-row"), tr("Table: Duplicate row"), {}},
        {QStringLiteral("noteAppC:table-duplicate-column"), tr("Table: Duplicate column"), {}},
        {QStringLiteral("noteAppC:table-sort-column-ascending"), tr("Table: Sort column ascending"), {}},
        {QStringLiteral("noteAppC:table-sort-column-descending"), tr("Table: Sort column descending"), {}},
    };
    return table;
}

bool MarkdownEditor::runCommand(const QString &id)
{
    if (foldCommand(id))
        return true; // folding is not editing: Reading view folds too
    if (d->m_mode == Mode::Reading && id != QLatin1String("editor:open-search"))
        return false; // nothing to edit; searching still works
    if (id == QLatin1String("editor:toggle-bold"))
        toggleBold();
    else if (id == QLatin1String("editor:toggle-italics"))
        toggleItalic();
    else if (id == QLatin1String("editor:toggle-highlight"))
        toggleHighlight();
    else if (id == QLatin1String("editor:toggle-strikethrough"))
        toggleWrap(QStringLiteral("~~"));
    else if (id == QLatin1String("editor:toggle-code"))
        toggleWrap(QStringLiteral("`"));
    else if (id == QLatin1String("editor:toggle-inline-math"))
        toggleWrap(QStringLiteral("$"));
    else if (id == QLatin1String("editor:toggle-comments"))
        toggleWrap(QStringLiteral("%%"));
    else if (id == QLatin1String("editor:insert-link"))
        insertLink();
    else if (id == QLatin1String("editor:toggle-checklist-status"))
        toggleTaskAtCursor();
    else if (id == QLatin1String("noteAppC:checkbox-state")) {
        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        if (d->m_parseOptions.taskStates && addTaskStateActions(*menu, d->m_cursor))
            menu->popup(viewport()->mapToGlobal(caretRect().bottomLeft().toPoint()));
        else {
            delete menu;
            toggleTaskAtCursor();
        }
    }
    else if (id == QLatin1String("editor:open-search"))
        showFind(false);
    else if (id == QLatin1String("editor:open-search-replace"))
        showFind(true);
    else if (id == QLatin1String("editor:swap-line-up"))
        moveLines(-1);
    else if (id == QLatin1String("editor:swap-line-down"))
        moveLines(1);
    else if (id == QLatin1String("noteAppC:duplicate-lines"))
        duplicateLines();
    else if (id == QLatin1String("noteAppC:edit-block-source"))
        editBlockSource(blockAt(d->m_cursor));
    else if (id == QLatin1String("noteAppC:format-table"))
        formatTables(d->m_cursor);
    else if (id == QLatin1String("noteAppC:format-all-tables"))
        formatTables(-1);
    else if (id == QLatin1String("noteAppC:insert-table"))
        insertTable();
    else if (id.startsWith(QLatin1String("noteAppC:table-"))) {
        // A known command, run - whether or not the caret is in a table for
        // it to change, as format-table outside one is.
        const auto &all = commands();
        if (std::none_of(all.cbegin(), all.cend(), [&id](const Command &command) { return command.id == id; }))
            return false;
        runTableCommand(id);
    } else {
        return false;
    }
    return true;
}

void MarkdownEditor::setKeyBindings(const QHash<QString, QList<QKeySequence>> &bindings)
{
    d->m_keymap.clear();
    for (const Command &command : commands()) {
        const auto own = bindings.constFind(command.id);
        for (const QKeySequence &keys : own != bindings.constEnd() ? *own : command.keys) {
            if (!keys.isEmpty())
                d->m_keymap.insert(keys[0].toCombined(), command.id);
        }
    }
}

void MarkdownEditor::keyPressEvent(QKeyEvent *event)
{
    const TypingScope typed(d->m_typing); // applyEdit: this edit is the reader's own
    if (event->key() == Qt::Key_0 && event->modifiers() == Qt::ControlModifier && resetPdfZoom())
        return;
    // Ctrl pressed with the pointer already resting on a link previews it,
    // as in Obsidian, without the mouse having to move.
    if (event->key() == Qt::Key_Control && viewport()->underMouse()) {
        const QPoint global = QCursor::pos();
        updateLinkHover(viewport()->mapFromGlobal(global), true, global);
    }
#ifndef Q_OS_WIN // Windows turns Shift+F10 into the menu event itself
    if (event->key() == Qt::Key_F10 && event->modifiers() == Qt::ShiftModifier) {
        const QPoint caret = viewport()->mapTo(this, caretRect().center().toPoint());
        QContextMenuEvent menuEvent(QContextMenuEvent::Keyboard, caret, mapToGlobal(caret));
        contextMenuEvent(&menuEvent);
        return;
    }
#endif
    // Ctrl+Space and Shift+Tab in a code block with a running kernel, and the
    // popups they open (NotebookComplete.cpp).
    if (d->m_notebookIntrospector && handleNotebookIntrospectKey(event))
        return;
    // The slash popup's keys are handled here rather than left to QCompleter's
    // own filter on the popup, which only sees them while the popup holds the
    // keyboard - not, for one, when a platform does not give a popup focus.
    if (d->m_slashCompleter && d->m_slashCompleter->popup()->isVisible()) {
        QAbstractItemView *popup = d->m_slashCompleter->popup();
        const QAbstractItemModel *rows = popup->model();
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab: {
            const QString name = popup->currentIndex().data().toString();
            popup->hide();
            if (!name.isEmpty())
                insertSlashCompletion(name);
            return;
        }
        case Qt::Key_Escape:
            popup->hide();
            return;
        case Qt::Key_Up:
        case Qt::Key_Down: {
            const int count = rows ? rows->rowCount() : 0;
            if (count > 0) {
                const int step = event->key() == Qt::Key_Down ? 1 : -1;
                const int row = (qMax(0, popup->currentIndex().row()) + step + count) % count;
                popup->setCurrentIndex(rows->index(row, 0));
            }
            return;
        }
        default:
            break;
        }
    }
    if (d->m_completer->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Escape:
            event->ignore(); // the popup owns these while it is up
            return;
        default:
            break;
        }
    }

    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    if (!d->m_interacted) {
        d->m_interacted = true;
        updateLayout();
    }
    // Vim mode (VimMode.cpp). Off, this is the only cost; in insert mode it
    // claims Escape and nothing else.
    if (d->m_vim.enabled && d->m_mode != Mode::Reading && handleVimKey(event))
        return;

    // Rebindable commands first, then the keys every text field has.
    const QKeyCombination pressed(event->modifiers() & ~Qt::KeypadModifier,
                                  Qt::Key(event->key()));
    const auto bound = d->m_keymap.constFind(pressed.toCombined());
    if (bound != d->m_keymap.constEnd() && runCommand(*bound))
        return;

    if (control) {
        switch (event->key()) {
        case Qt::Key_Z:
            shift ? redo() : undo();
            return;
        case Qt::Key_Y:
            redo();
            return;
        case Qt::Key_A:
            d->m_anchor = 0;
            moveCursor(d->m_text.size(), true);
            return;
        case Qt::Key_C:
            copySelection();
            return;
        case Qt::Key_X:
            cutSelection();
            return;
        case Qt::Key_V:
            pasteClipboard(shift);
            return;
        default:
            break;
        }
    }

    d->m_goalX = -1;
    if (d->m_tableHandles && handleCellSelectionKey(event))
        return;
    if (handleTableKey(event))
        return;
    switch (event->key()) {
    // In a drawn table the arrows cross from one cell into the next.
    // An entity is one character on screen, so one step for the caret.
    case Qt::Key_Left: {
        const qsizetype target = stepOverSubstitution(d->m_cursor > 0 ? d->m_cursor - 1 : 0, -1);
        moveCursor(shift ? target : snapToTableCell(target, d->m_cursor), shift);
        return;
    }
    case Qt::Key_Right: {
        const qsizetype target = stepOverSubstitution(d->m_cursor + 1, 1);
        moveCursor(shift ? target : snapToTableCell(target, d->m_cursor), shift);
        return;
    }
    case Qt::Key_Up:
        moveVertically(-1, shift);
        return;
    case Qt::Key_Down:
        moveVertically(1, shift);
        return;
    case Qt::Key_Home:
        if (event->modifiers() & Qt::ControlModifier) { // the start of the note
            moveCursor(0, shift);
            return;
        }
        moveCursor(d->m_text.lastIndexOf(QLatin1Char('\n'), qMax<qsizetype>(0, d->m_cursor - 1)) + 1,
                   shift);
        return;
    case Qt::Key_End: {
        if (event->modifiers() & Qt::ControlModifier) { // the end of the note: the empty line after a final newline too
            moveCursor(d->m_text.size(), shift);
            return;
        }
        const qsizetype next = d->m_text.indexOf(QLatin1Char('\n'), d->m_cursor);
        moveCursor(next < 0 ? d->m_text.size() : next, shift);
        return;
    }
    case Qt::Key_Backspace:
        if (!deleteAutoPair() && !handleListBackspace())
            removeSelectionOrChar(-1);
        updateSuggestions();
        updateSlashSuggestions();
        return;
    case Qt::Key_Delete:
        removeSelectionOrChar(1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!handleHTMLenter() && !(!shift && (handleSourceTableKey(Qt::Key_Return) || handleTableEnter()))
            && !handleListEnter(shift))
            insertText(QStringLiteral("\n"));
        return;
    case Qt::Key_Tab:
        if (!handleSourceTableKey(Qt::Key_Tab) && !handleListIndent(1))
            insertText(QString(d->m_tabWidth, QLatin1Char(' ')));
        return;
    case Qt::Key_Backtab: // Shift+Tab
        if (handleSourceTableKey(Qt::Key_Backtab) || handleListIndent(-1))
            return;
        break;
    case Qt::Key_Escape:
        if (findVisible()) {
            hideFind();
            return;
        }
        break; // otherwise Escape is the window's, not the editor's
    default:
        break;
    }

    // A shortcut nobody holds types nothing: Ctrl+B unbound does nothing, as
    // in Obsidian, rather than typing "b". Ctrl+Alt stays text, because some
    // layouts report AltGr that way.
    const Qt::KeyboardModifiers held = event->modifiers();
    const bool shortcut = (held & (Qt::ControlModifier | Qt::MetaModifier))
                          && !held.testFlag(Qt::AltModifier);
    if (!shortcut && !event->text().isEmpty() && event->text().at(0).isPrint()) {
        // Auto-pairing owns the brackets and quotes, "[[" included: the first
        // "[" opens "[]", the second sees the "]" ahead of it and opens
        // another, so the suggester still gets its "[[]]".
        if (!handleAutoPair(event->text().at(0)))
            insertText(cellSafe(event->text()));
        updateSuggestions();
        updateSlashSuggestions();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void MarkdownEditor::mousePressEvent(QMouseEvent *event)
{
    if (d->m_notebookInspect && d->m_notebookInspect->isVisible())
        hideNotebookIntrospection();
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    // A finger is bigger than a pointer: every small target grows for it.
    const bool touch = event->pointingDevice()
                       && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen;
    d->m_touchSlop = touch ? 10 : 0;
    if (touch && pressSelectionHandle(event->pos()))
        return;
    d->m_touchHandles = false;

    // The `</>` button comes first: over a PDF's controls, a card, a query
    // block or a hooked embed it is the only thing a press there means.
    if (!touch) {
        if (const qsizetype block = editButtonAt(event->pos()); block >= 0) {
            editBlockSource(block);
            return;
        }
    }
    // A table's grips and add bars (TableHandles.cpp); a press anywhere else
    // ends a rectangle of selected cells.
    d->m_cellPress = -1;
    d->m_cellArmed = false;
    if (d->m_tableHandles) {
        if (d->m_cellSel.tableStart >= 0) {
            d->m_cellSel = CellSelection{};
            viewport()->update();
        }
        if (pressTableChrome(event->pos(), touch))
            return;
    }

    // A hooked embed's own image (a `.canvas`, say) takes the press instead
    // of the caret: a plain click may open a card on release, and a drag
    // pans it - neither is a place the caret can land.
    if (const qsizetype embed = hookedEmbedBlockAt(event->pos()); embed >= 0) {
        d->m_embedDrag.blockIndex = embed;
        d->m_embedDrag.startPoint = event->pos();
        d->m_embedDrag.lastPoint = event->pos();
        d->m_embedDrag.moved = false;
        return;
    }
    if (pressPdfCell(event->pos(), touch))
        return;

    const QString target = linkAt(event->pos());
    if (!target.isEmpty() && event->modifiers().testFlag(Qt::ControlModifier)) {
        emit linkActivated(target);
        return;
    }
    // A callout's title is edited where it is drawn (CalloutTitle.cpp).
    if (!touch && pressCalloutTitle(event->pos()))
        return;
    // A footnote never leaves the note, so it jumps straight to an offset
    // rather than going through linkActivated the way a real link does.
    if (event->modifiers().testFlag(Qt::ControlModifier) && jumpToFootnote(event->pos()))
        return;

    // A click on a cell a short row lacks gives the row that cell first.
    fillShortRowAt(event->pos());
    if (handleBlockClick(event->pos()))
        return;

    for (const BlockView &view : d->m_blocks) {
        if (view.listTextX.empty() || !view.laidOut || view.foldHidden)
            continue;
        const std::span<const ListItemSpan> items = listItemsIn(view.span.range);
        qsizetype hiddenUntil = 0; // under a folded item
        for (size_t at = 0; at < items.size(); ++at) {
            const ListItemSpan &item = items[at];
            if (item.range.start < hiddenUntil)
                continue;
            if (foldedItem(items, at))
                hiddenUntil = item.range.end;
            if (!item.task)
                continue;
            // A marker the caret has uncovered shows as text, with no box to click.
            if (std::any_of(view.revealed.cbegin(), view.revealed.cend(),
                            [&item](const Range &range) { return range.overlaps(item.marker); }))
                continue;
            if (touchable(checkboxRect(view, item), 3).contains(event->pos())) {
                toggleTask(item);
                return;
            }
        }
    }

    d->m_goalX = -1;
    d->m_interacted = true;
    // Measured against the layout the reader is looking at now, before this
    // click can change what is revealed. A double-click selects the word
    // found here, not under a layout the first click has since shifted.
    d->m_pressOffset = offsetAt(event->pos());
    if (!event->modifiers().testFlag(Qt::ShiftModifier))
        d->m_pressOffset = snapToTableCell(d->m_pressOffset, d->m_pressOffset);
    d->m_pressing = true;
    if (event->pointingDevice() && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen) {
        d->m_pressPoint = event->pos();
        d->m_longPress.start();
    }
    moveCursor(d->m_pressOffset, event->modifiers().testFlag(Qt::ShiftModifier));
    // A drag from a cell into another selects cells, not text.
    if (d->m_tableHandles && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        const CellRef cell = tableCellAt(d->m_pressOffset);
        if (cell.row >= 0 && tableRenderedAt(d->m_pressOffset) && (!cell.table->html || cell.table->simple)) {
            d->m_cellPress = cell.table->range.start;
            d->m_cellPressRow = cell.row;
            d->m_cellPressColumn = cell.column;
            d->m_cellPressTouch = touch;
        }
    }
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::watchStyleFiles()
{
    d->m_configWatcher = new QFileSystemWatcher(this);
    // The files, and the directory they live in. An editor that saves by
    // writing a new file and renaming it over the old one - most do - makes
    // inotify drop the watch on the file, and a file that does not exist yet
    // cannot be watched at all; the directory sees both happen.
    const QStringList files = {CalloutStyles::defaultPath(), LanguageStyles::defaultPath(), TaskStates::path()};
    const QString directory = QFileInfo(files.first()).absolutePath();
    if (QFileInfo(directory).isDir())
        d->m_configWatcher->addPath(directory);
    for (const QString &file : files) {
        if (QFileInfo::exists(file))
            d->m_configWatcher->addPath(file);
    }
    connect(d->m_configWatcher, &QFileSystemWatcher::fileChanged, this,
            &MarkdownEditor::reloadStyleFiles);
    connect(d->m_configWatcher, &QFileSystemWatcher::directoryChanged, this,
            &MarkdownEditor::reloadStyleFiles);
}

void MarkdownEditor::reloadStyleFiles()
{
    // Put back any watch a rename-over-save took away.
    for (const QString &file : {CalloutStyles::defaultPath(), LanguageStyles::defaultPath(), TaskStates::path()}) {
        if (QFileInfo::exists(file) && !d->m_configWatcher->files().contains(file))
            d->m_configWatcher->addPath(file);
    }
    d->m_calloutStyles = CalloutStyles::load();
    d->m_languageStyles = LanguageStyles::load();
    if (d->m_parseOptions.taskStates)
        TaskStates::reload();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

qsizetype MarkdownEditor::characterCount() const
{
    const Range front = Frontmatter::find(d->m_text);
    qsizetype count = d->m_text.size() - (front.isValid() ? front.end : 0);
    // What the last parse left out of the count; the note may have moved on
    // since, so it can only overshoot a little, never below nothing.
    for (const Range &range : d->m_decorations.uncounted)
        count -= qMax<qsizetype>(0, qMin<qsizetype>(range.end, d->m_text.size()) - range.start);
    return qMax<qsizetype>(0, count);
}

MarkdownEditor::TextCount MarkdownEditor::selectionCount() const
{
    const Range range = selection();
    if (range.length() <= 0)
        return {};
    if (d->m_decorations.uncounted.isEmpty())
        return {wordsIn(QStringView(d->m_text).mid(range.start, range.length())), range.length()};
    // The same things the note's count leaves out, in the selection too.
    QString shown = d->m_text.mid(range.start, range.length());
    qsizetype left = range.length();
    for (const Range &out : d->m_decorations.uncounted) {
        const qsizetype from = qMax(out.start, range.start);
        const qsizetype to = qMin(out.end, range.end);
        if (from >= to)
            continue;
        std::fill_n(shown.data() + (from - range.start), to - from, QLatin1Char(' '));
        left -= to - from;
    }
    return {wordsIn(shown), left};
}

QString MarkdownEditor::selectedText() const
{
    const Range range = selection();
    return d->m_text.mid(range.start, range.length());
}

void MarkdownEditor::mouseReleaseEvent(QMouseEvent *event)
{
    d->m_longPress.stop();
    d->m_cellPress = -1;
    d->m_cellArmed = false;
    stopTableAutoScroll();
    if (releaseTableChrome(event->pos()))
        return;
    if (d->m_embedDrag.blockIndex >= 0) {
        const qsizetype block = d->m_embedDrag.blockIndex;
        const bool wasClick = !d->m_embedDrag.moved && !d->m_embedDrag.pdf;
        const QPoint pos = event->pos();
        d->m_embedDrag = EmbedDrag{};
        if (wasClick && event->button() == Qt::LeftButton) {
            applyFileEmbedInput(block, {FileEmbedInputKind::Click, embedLocalPoint(block, pos),
                                        {}, event->modifiers()});
        }
        return;
    }
    if (releaseSelectionHandle())
        return;
    if (event->button() != Qt::LeftButton || !d->m_pressing) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    // Only now does the caret's block decide whether to show its source: a
    // plain click reveals it, a drag that selected something does not.
    d->m_pressing = false;
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (d->m_embedDrag.blockIndex >= 0) {
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            d->m_embedDrag = EmbedDrag{}; // the release was missed - it left the window, say
            return;
        }
        if (!d->m_embedDrag.moved
            && (event->pos() - d->m_embedDrag.startPoint).manhattanLength() >= 4)
            d->m_embedDrag.moved = true;
        const QPoint delta = event->pos() - d->m_embedDrag.lastPoint;
        if (d->m_embedDrag.moved && !delta.isNull()) {
            if (d->m_embedDrag.pdf)
                panPdfEmbed(d->m_embedDrag.blockIndex, delta);
            else
                applyFileEmbedInput(d->m_embedDrag.blockIndex,
                                    {FileEmbedInputKind::Drag, embedLocalPoint(d->m_embedDrag.blockIndex, event->pos()),
                                     delta, event->modifiers()});
            d->m_embedDrag.lastPoint = event->pos();
        }
        return;
    }
    if (dragSelectionHandle(event->pos()))
        return;
    if (d->m_tableDrag.part != TablePart::None) {
        if (event->buttons().testFlag(Qt::LeftButton))
            dragTableChrome(event->pos());
        else
            d->m_tableDrag = TableDrag{}; // the release was missed
        return;
    }
    if (event->buttons().testFlag(Qt::LeftButton)) {
        if (d->m_longPress.isActive()) {
            // A finger always wobbles a little; only a real move is a drag.
            if ((event->pos() - d->m_pressPoint).manhattanLength() < 12)
                return;
            d->m_longPress.stop();
        }
        if (d->m_cellPress >= 0 && (!d->m_cellPressTouch || d->m_cellArmed) && dragCellSelection(event->pos()))
            return;
        // A finger's drag is the scroller's: Qt still sends the mouse events
        // it synthesizes from the touch, and they selected the text the
        // note scrolled past. Only after a long press does a finger select.
        const bool finger =
            event->pointingDevice() && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen;
        if (finger && !d->m_touchHandles)
            return;
        moveCursor(offsetAt(event->pos()), true);
        return;
    }
    // The code block the pointer is over (that is when one without a
    // language shows its "Copy" label) and the rendered block whose `</>`
    // button shows (HoverEdit.cpp). Only a change repaints, and only the
    // panels involved.
    // A finger has no hover: the move Qt sends ahead of its press must not
    // bring a table's grips up on its own (its first tap does that).
    const bool finger = event->pointingDevice() && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen;
    updateHover(event->pos(), !finger);

    const bool clickable = !linkAt(event->pos()).isEmpty() || codeLabelAt(event->pos()) || d->m_overEditButton;
    Qt::CursorShape shape = clickable ? Qt::PointingHandCursor : Qt::IBeamCursor;
    if (const auto nav = pdfNavCursorAt(event->pos()))
        shape = *nav;
    switch (d->m_tableHover.part) {
    case TablePart::RowGrip:
    case TablePart::ColumnGrip:
        shape = Qt::OpenHandCursor;
        break;
    case TablePart::AddRow:
        shape = Qt::SizeVerCursor;
        break;
    case TablePart::AddColumn:
        shape = Qt::SizeHorCursor;
        break;
    case TablePart::None:
        break;
    }
    viewport()->setCursor(shape);
    updateLinkHover(event->pos(), event->modifiers().testFlag(Qt::ControlModifier),
                    event->globalPosition().toPoint());
}

void MarkdownEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    // A second tap that comes quickly, on a grip or a bar a first tap just
    // brought up, reaches here as a double click, not as a press.
    if (d->m_tableHandles && event->pointingDevice()
        && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen) {
        d->m_touchSlop = 10;
            if (pressTableChrome(event->pos(), true))
            return;
    }
    if (doubleClickPdfCell(event->pos()))
        return;
    // Empty space in a hooked embed's own image opens the file itself
    // (a card there already opened on the first click's release, above); a
    // card falls through this unused, harmlessly, since it already handled
    // the preceding single click.
    if (const qsizetype embed = hookedEmbedBlockAt(event->pos()); embed >= 0) {
        applyFileEmbedInput(embed, {FileEmbedInputKind::DoubleClick, embedLocalPoint(embed, event->pos()),
                                    {}, event->modifiers()});
        return;
    }

    const QString target = linkAt(event->pos());
    if (!target.isEmpty()) {
        emit linkActivated(target);
        return;
    }

    // Select the word the first click landed on. By now that click's release
    // may have revealed its block's syntax and moved the text, so the pointer
    // is no longer a reliable way to find the word the reader aimed at.
    selectWordAt(d->m_pressOffset);
}

void MarkdownEditor::selectWordAt(qsizetype offset)
{
    qsizetype start = offset;
    qsizetype end = offset;
    while (start > 0 && d->m_text.at(start - 1).isLetterOrNumber())
        --start;
    while (end < d->m_text.size() && d->m_text.at(end).isLetterOrNumber())
        ++end;
    d->m_anchor = start;
    moveCursor(end, true);
}

void MarkdownEditor::wheelEvent(QWheelEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (const qsizetype embed = hookedEmbedBlockAt(pos); embed >= 0) {
        const FileEmbedInputEvent input{FileEmbedInputKind::Wheel, embedLocalPoint(embed, pos),
                                        event->angleDelta(), event->modifiers()};
        if (applyFileEmbedInput(embed, input)) {
            event->accept();
            return;
        }
        // Declined - no Ctrl held, most likely - so falling through below
        // scrolls the note itself; an embed must never leave the page stuck.
    }
    if (wheelPdfCell(event)) {
        event->accept();
        return;
    }

    BlockView *table = sidewaysScrollableAt(pos);
    if (table) {
        // A horizontal wheel, or shift with a vertical one, pans the table or
        // listing - the gesture Obsidian answers to.
        const QPoint pixels = event->pixelDelta();
        const QPoint degrees = event->angleDelta();
        qreal dx = pixels.x() != 0 ? pixels.x() : degrees.x() / 2.0;
        if (qFuzzyIsNull(dx) && event->modifiers().testFlag(Qt::ShiftModifier))
            dx = pixels.y() != 0 ? pixels.y() : degrees.y() / 2.0;

        if (!qFuzzyIsNull(dx)) {
            const qreal overflow = sideExtent(*table) - contentWidth();
            const qreal wanted = qBound<qreal>(0, sideOffset(*table) - dx, overflow);
            if (!qFuzzyCompare(wanted + 1, table->sideScroll + 1)) {
                table->sideScroll = wanted;
                viewport()->update();
            }
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::wheelEvent(event);
}

void MarkdownEditor::focusInEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusInEvent(event);
    d->m_cursorOn = true;
    d->m_blink.start();
    viewport()->update();
}

bool MarkdownEditor::focusNextPrevChild(bool)
{
    return false;
}

void MarkdownEditor::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);
    if (d->m_notebookInspect && d->m_notebookInspect->isVisible())
        hideNotebookIntrospection();
    d->m_blink.stop();
    d->m_cursorOn = false;
    viewport()->update();
}

void MarkdownEditor::inputMethodEvent(QInputMethodEvent *event)
{
    const TypingScope typed(d->m_typing); // applyEdit: this edit is the reader's own
    // Vim's normal and visual modes take keys as commands: composed text
    // would be typed into the note without an insert.
    if (d->m_vim.enabled && d->m_vim.mode != VimState::Mode::Insert) {
        event->accept();
        return;
    }
    if (!event->commitString().isEmpty())
        insertText(event->commitString());
    event->accept();
}

QVariant MarkdownEditor::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImCursorRectangle: {
        const QPointF point = cursorPoint();
        return QRectF(point, QSizeF(1, QFontMetricsF(font()).height()));
    }
    case Qt::ImSurroundingText:
        return d->m_text;
    case Qt::ImCursorPosition:
        return int(d->m_cursor);
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}

} // namespace noteAppC
