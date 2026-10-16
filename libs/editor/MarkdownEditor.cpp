#include "MarkdownEditor.h"

#include "Icons.h"
#include "MediaPlayback.h"
#include "PdfPage.h"
#include "Vault.h"

#include <QAbstractItemView>
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
#include <QPainterPath>
#include <QGuiApplication>
#include <QScrollBar>
#include <QScroller>
#include <QInputDevice>
#ifdef NOTEAPPC_HAVE_OPENGLWIDGETS
#include <QOpenGLWidget>
#endif
#include <QFileInfo>
#include <QImageReader>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QRegularExpression>
#include <QStringListModel>
#include <QtConcurrent>
#include <QTextLine>
#include <algorithm>
#include <cstdio>
#include <span>

namespace noteAppC {

namespace {

// The source's own blank lines make the vertical rhythm, exactly as the file
// reads; only headings get extra air, the way Obsidian styles them.
constexpr int kBlockSpacing = 0;
/// Width of the margin a list keeps for its bullets and checkboxes.
constexpr qreal kListIndent = 26;
/// Above this many characters a note is shown first and parsed on the thread
/// pool. Below it the parse is quicker than a frame, so the intermediate
/// plain-text state would be a flicker and nothing else. 200 kB parses in
/// about 26 ms, 50 kB in 7 ms; this sits between them.
constexpr qsizetype kAsyncParseThreshold = 128 * 1024;
constexpr int kTypingBurstMs = 500;
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

    // Touch: a drag scrolls with momentum instead of selecting, and a held
    // finger selects a word. Only where a touchscreen exists, so a desktop
    // with a mouse keeps its drag-to-select untouched.
    const QList<const QInputDevice *> devices = QInputDevice::devices();
    if (std::any_of(devices.cbegin(), devices.cend(), [](const QInputDevice *device) {
            return device->type() == QInputDevice::DeviceType::TouchScreen;
        }))
        QScroller::grabGesture(viewport(), QScroller::TouchGesture);
    m_longPress.setSingleShot(true);
    m_longPress.setInterval(500);
    connect(&m_longPress, &QTimer::timeout, this, [this] {
        m_pressing = false;
        selectWordAt(m_pressOffset);
        m_touchHandles = true; // a finger's selection is adjusted by its handles
        viewport()->update();
    });

    // ponytail: the parse is synchronous behind a short debounce. It measures
    // well under the keystroke budget on notes this size; move it to the
    // thread pool when a 1 MB note says otherwise.
    m_parseTimer.setSingleShot(true);
    m_parseTimer.setInterval(20);
    connect(&m_parseTimer, &QTimer::timeout, this, &MarkdownEditor::startParse);
    connect(&m_parseWatcher, &QFutureWatcher<ParseJob>::finished, this, [this] {
        if (m_parseWatcher.future().resultCount() == 0)
            return;
        ParseJob job = m_parseWatcher.result();
        setWordCount(job.words);
        adoptDecorations(std::move(job.decorations), std::move(job.properties), std::move(job.headings));
        m_parsedVersion = job.version;
        // The buffer moved on while we were parsing: go round again.
        if (m_parsedVersion != m_textVersion)
            startParse();
    });

    m_blink.setInterval(QApplication::cursorFlashTime() / 2);
    connect(&m_blink, &QTimer::timeout, this, [this] {
        m_cursorOn = !m_cursorOn;
        // Only where the caret is, not the whole page twice a second.
        const QRect caret = m_cursorOn ? caretRect().toAlignedRect().adjusted(-2, -2, 2, 2) : m_caretPainted;
        if (caret.isEmpty())
            viewport()->update();
        else
            viewport()->update(caret);
    });

    m_completer = new QCompleter(new QStringListModel(this), this);
    m_completer->setWidget(this);
    m_completer->popup()->setObjectName(QStringLiteral("completerPopup"));
    // One line per name. Without this, every application-wide style change
    // (each theme pass) sized all of a vault's link targets row by row, hidden
    // popup or not: 157 ms of a 20 000-note vault's cold start.
    if (auto *list = qobject_cast<QListView *>(m_completer->popup()))
        list->setUniformItemSizes(true);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated),
            this, &MarkdownEditor::insertCompletion);

    m_lastEdit.start();
}

void MarkdownEditor::setLinkResolver(std::function<QString(const QString &)> resolver)
{
    m_resolver = std::move(resolver);
    m_images.clear();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setFeatures(const Features &features)
{
    if (features == m_features)
        return; // every settings pass asks, and this relays out every block
    const bool HTMLchanged = features.HTMLblocks != m_features.HTMLblocks;
    m_features = features;
    if (HTMLchanged && !m_text.isEmpty())
        reparse(); // an HTML block's own blocks come from the parse (HtmlEdit.cpp)
    // A rare, administrative call - not the keystroke path - so relaying out
    // every block rather than picking out just the ones a flag touches is
    // the simple and correct choice, not a slow one that happens to work.
    m_pdfPageCache.clear();
    m_texCache.clear();
    m_fileEmbedCache.clear();
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
        changed = changed || bool(renderer) != m_fileEmbedRenderers.contains(suffix.toLower());
        if (renderer)
            m_fileEmbedRenderers.insert(suffix.toLower(), renderer);
        else
            m_fileEmbedRenderers.remove(suffix.toLower());
    }
    if (!changed)
        return;
    m_fileEmbedCache.clear();
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
            m_fileEmbedInputHandlers.insert(suffix.toLower(), handler);
        else
            m_fileEmbedInputHandlers.remove(suffix.toLower());
    }
}

void MarkdownEditor::setLinkTargets(const QStringList &targets)
{
    if (auto *model = qobject_cast<QStringListModel *>(m_completer->model()))
        model->setStringList(targets);
}

std::optional<QString> MarkdownEditor::wikilinkPrefix() const
{
    const qsizetype lineStart = m_text.lastIndexOf(QLatin1Char('\n'),
                                                   qMax<qsizetype>(0, m_cursor - 1)) + 1;
    const QStringView before = QStringView(m_text).mid(lineStart, m_cursor - lineStart);

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
        m_completer->popup()->hide();
        return;
    }

    m_completer->setCompletionPrefix(*prefix);
    if (m_completer->completionCount() == 0) {
        m_completer->popup()->hide();
        return;
    }
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));

    const QPointF point = cursorPoint();
    QRect anchor(point.toPoint(), QSize(1, int(QFontMetricsF(font()).height())));
    anchor.moveTopLeft(viewport()->mapTo(this, anchor.topLeft()));
    anchor.setWidth(m_completer->popup()->sizeHintForColumn(0) + 32);
    m_completer->complete(anchor);
}

void MarkdownEditor::insertCompletion(const QString &completion)
{
    const auto prefix = wikilinkPrefix();
    if (!prefix)
        return;

    // Replace what was typed, then step over the closing brackets if the
    // auto-pair already put them there.
    const qsizetype at = m_cursor - prefix->size();
    const bool closed =
        QStringView(m_text).mid(m_cursor).startsWith(QLatin1String("]]"));
    applyEdit(at, prefix->size(), closed ? completion : completion + QLatin1String("]]"));
    if (closed)
        m_cursor = m_anchor = m_cursor + 2;
}

MarkdownEditor::~MarkdownEditor()
{
    // Workers post their results back through m_lifeline; ending it first
    // means none is part-way through a post to this object (see Lifeline).
    m_lifeline->end();

    // An open property editor is a child widget, and Qt deletes children
    // after this object's members are gone: losing focus as it went, it
    // would call back into an editor that no longer exists. Cut it loose now.
    if (m_propertyEdit) {
        m_propertyEdit->disconnect(this);
        m_propertyEdit->removeEventFilter(this);
        delete m_propertyEdit.data();
    }
    if (m_parseWatcher.isRunning())
        m_parseWatcher.waitForFinished();
}

void MarkdownEditor::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_code.setTheme(theme);
    if (m_findBar)
        m_findBar->setTheme(theme);
    m_lineHeights.clear();
    m_blockSpacing = 0;
    invalidateLayouts();
    setFont(theme.textFont());
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    finishPropertyEdit(true); // source and reading modes draw no table to edit in
    m_mode = mode;
    // Reading view appends a trailing footnotes-list block that no other
    // mode shows; rebuildBlocks() is what adds or drops it. Skipped when
    // there is nothing to list - a note without footnotes costs nothing.
    if (!m_decorations.footnoteDefinitions.isEmpty())
        rebuildBlocks();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
    emit modeChanged();
}

void MarkdownEditor::setNoteText(const QString &text)
{
    finishPropertyEdit(false); // a different note: what was typed belongs to the old one
    if (m_media)
        m_media->stop();
    m_text = text;
    ++m_textVersion;
    m_cursor = m_anchor = 0;
    m_activeBlock = -1;
    m_tableEdited = false;
    m_caretCell = {-1, -1};
    m_undo.clear();
    m_redo.clear();
    m_interacted = false;
    // A big note is shown before it is understood: its text goes up as plain
    // paragraphs at once and the parse runs on the thread pool, because the
    // parse is the whole cost of opening one (142 ms of a 1 MB note's 146 ms)
    // and there is no reason to stare at nothing while it happens. A small
    // note parses faster than a frame, so it skips the intermediate state
    // rather than flickering through it.
    if (text.size() > kAsyncParseThreshold) {
        setWordCount(-1); // not counted until the parse comes back
        adoptDecorations({}, {}, {});
        startParse();
    } else {
        reparse();
    }
    if (!m_find.pattern.isEmpty())
        updateFindMatches();
    setModified(false);
    verticalScrollBar()->setValue(0);
    if (m_vim.enabled) { // a new buffer: back to normal mode, the register kept
        VimState fresh;
        fresh.enabled = true;
        fresh.reg = m_vim.reg;
        fresh.regLinewise = m_vim.regLinewise;
        fresh.lastSearch = m_vim.lastSearch;
        fresh.searchForward = m_vim.searchForward;
        fresh.lastStatus = m_vim.lastStatus;
        m_vim = fresh;
        emitVimStatus();
    }
    viewport()->update();
}

// --- the find bar ---------------------------------------------------------

void MarkdownEditor::showFind(bool withReplace)
{
    if (!m_findBar) {
        m_findBar = new FindBar(viewport());
        m_findBar->setTheme(m_theme);
        connect(m_findBar, &FindBar::queryChanged, this, [this] {
            setFindQuery(m_findBar->pattern(), m_findBar->caseSensitive(),
                         m_findBar->useRegex());
        });
        connect(m_findBar, &FindBar::stepped, this, &MarkdownEditor::findStep);
        connect(m_findBar, &FindBar::replaceRequested, this, [this] {
            replaceCurrent(m_findBar->replacement());
        });
        connect(m_findBar, &FindBar::replaceAllRequested, this, [this] {
            replaceAll(m_findBar->replacement());
        });
        connect(m_findBar, &FindBar::dismissed, this, &MarkdownEditor::hideFind);
    }

    // Ctrl+H on an open find bar adds the replace row rather than closing it.
    if (withReplace)
        m_findBar->setReplaceShown(true);
    m_findBar->show();
    positionFindBar();
    m_findBar->focusSearch();
    // Reopening picks the last query back up: the field kept its text while
    // the bar was away, so the matches have to catch up with it.
    setFindQuery(m_findBar->pattern(), m_findBar->caseSensitive(), m_findBar->useRegex());
}

void MarkdownEditor::hideFind()
{
    if (!m_findBar)
        return;
    m_findBar->hide();
    m_find.pattern.clear();
    m_find.matches.clear();
    m_find.current = -1;
    m_find.valid = true;
    setFocus(Qt::OtherFocusReason);
    viewport()->update();
}

bool MarkdownEditor::findVisible() const
{
    // isVisible() is false while the whole editor is still unshown, which is
    // not what "the bar is open" means.
    return m_findBar && !m_findBar->isHidden();
}

void MarkdownEditor::positionFindBar()
{
    if (!m_findBar || m_findBar->isHidden())
        return;
    const QSize hint = m_findBar->sizeHint();
    const int width = qMin(hint.width(), viewport()->width() - 16);
    m_findBar->setGeometry(viewport()->width() - width - 8, 8, width, hint.height());
}

void MarkdownEditor::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modificationChanged(m_modified);
}

void MarkdownEditor::setStrictLineBreaks(bool on)
{
    if (m_strictLineBreaks == on)
        return;
    m_strictLineBreaks = on;
    // It decides whether a newline inside a paragraph joins or breaks, so
    // every block has to be laid out again, not merely repainted.
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setIndentGuides(bool on)
{
    if (m_indentGuides == on)
        return;
    m_indentGuides = on;
    viewport()->update(); // drawn behind the text; nothing moves
}

void MarkdownEditor::setOutlineNumbering(bool on)
{
    if (m_outlineNumbering == on)
        return;
    m_outlineNumbering = on;
    // The labels' widths set the lists' columns, so the text moves.
    for (BlockView &block : m_blocks) {
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
    if (m_listNumberSuffix == suffix)
        return;
    m_listNumberSuffix = suffix;
    // The labels' widths set the lists' columns, as outline numbering's do.
    for (BlockView &block : m_blocks) {
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
    if (map == m_bulletGlyphs)
        return;
    m_bulletGlyphs = map;
    viewport()->update(); // drawn in the margin; nothing moves
}

void MarkdownEditor::setTabWidth(int spaces)
{
    m_tabWidth = qBound(1, spaces, 8);
}

void MarkdownEditor::setUseWikilinks(bool on)
{
    m_useWikilinks = on;
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
    if (m_GPUacceleration == on)
        return;
#ifdef NOTEAPPC_HAVE_OPENGLWIDGETS
    if (on && !GPUaccelerationAvailable())
        return;
    m_GPUacceleration = on;
    // setViewport takes ownership and destroys the old one, so everything the
    // viewport carries has to be set again.
    setViewport(on ? new QOpenGLWidget(this) : nullptr);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(true);
    viewport()->update();
#else
    Q_UNUSED(on);
#endif
}

void MarkdownEditor::setLineNumbers(bool on)
{
    if (m_lineNumbers == on)
        return;
    m_lineNumbers = on;
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setReadableLineLength(bool on)
{
    if (m_readable == on)
        return;
    m_readable = on;
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setContentWidth(int pixels)
{
    m_contentWidth = qMax(200, pixels);
    invalidateLayouts();
    updateLayout();
    viewport()->update();
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
QList<Range> MarkdownEditor::revealedSyntax(const BlockView &view) const
{
    QList<Range> revealed;
    const qsizetype caret = m_cursor;
    if (!view.span.range.contains(caret) && caret != view.span.range.end)
        return revealed;

    const qsizetype line = m_lines.lineAt(caret);
    const qsizetype lineStart = m_lines.lineStart(line);
    const qsizetype lineEnd = m_lines.lineEnd(line);

    // The markers on the caret's line, found by binary search rather than by
    // walking the block from its start - a long list is a single block.
    const auto syntaxBegin = m_decorations.syntax.cbegin() + view.firstSyntax;
    const auto syntaxEnd = m_decorations.syntax.cbegin() + view.lastSyntax;
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
    const auto stylesBegin = m_decorations.styles.cbegin() + view.firstStyle;
    const auto stylesEnd = m_decorations.styles.cbegin() + view.lastStyle;
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
    for (const CalloutSpan &callout : m_decorations.callouts) {
        if (callout.depth == 0 || !callout.foldable
            || !view.span.range.contains(callout.range.start)
            || !m_folds.value(callout.range.start, callout.collapsed))
            continue;
        const qsizetype from = callout.titleRange.isValid() ? callout.titleRange.end
                                                            : callout.range.start;
        const qsizetype titleEnd = m_text.indexOf(QLatin1Char('\n'), from);
        if (titleEnd < 0 || titleEnd + 1 >= callout.range.end)
            continue;
        qsizetype end = callout.range.end;
        // Take the body's own last newline too, or it is left behind as an
        // empty line under the folded title.
        if (m_text.at(end - 1) != QLatin1Char('\n') && end < m_text.size()
            && m_text.at(end) == QLatin1Char('\n'))
            ++end;
        hidden.append({titleEnd + 1, end});
    }
}

QString MarkdownEditor::displayText() const
{
    // Blocks off screen carry no layout and no map, so this builds what they
    // would show. Test and debug helper only; painting never calls it.
    QStringList parts;
    parts.reserve(int(m_blocks.size()));
    for (const BlockView &view : m_blocks) {
        if (view.laidOut) {
            parts << view.map.text();
            continue;
        }
        const bool reveal = m_mode == Mode::Source
                            || (m_mode == Mode::LivePreview && view.active);
        if (reveal) {
            parts << m_text.mid(view.span.range.start, view.span.range.length());
            continue;
        }
        QList<Range> hidden;
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i)
            hidden.append(m_decorations.syntax.at(i));
        hideFoldedNestedCallouts(view, hidden);
        parts << DisplayMap::build(m_text, view.span.range, hidden, m_decorations.substitutions).text();
    }
    return parts.join(QString());
}

QTextCharFormat MarkdownEditor::formatAt(qsizetype sourceOffset) const
{
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0 || !m_blocks[size_t(index)].layout)
        return {};
    const BlockView &view = m_blocks[size_t(index)];
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
    m_interacted = true;
    moveCursor(offset, false);
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setWordCount(int words)
{
    if (words == m_wordCount)
        return;
    m_wordCount = words;
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
    std::sort(decorations.styles.begin(), decorations.styles.end(),
              [](const StyleSpan &l, const StyleSpan &r) { return l.range.start < r.range.start; });
    // The parser's (Markdown entities) and the HTML pass's, appended after.
    std::sort(decorations.substitutions.begin(), decorations.substitutions.end(),
              [](const Substitution &l, const Substitution &r) { return l.range.start < r.range.start; });
}

} // namespace

void MarkdownEditor::reparse()
{
    m_parseTimer.stop();
    Parser::Result result = m_parser.parseAll(m_text);
    if (m_features.HTMLblocks)
        expandEditableHTML(result.decorations, m_text);
    sortDecorations(result.decorations);
    m_parsedVersion = m_textVersion;
    setWordCount(result.metadata.wordCount);
    adoptDecorations(std::move(result.decorations), std::move(result.metadata.properties),
                     std::move(result.metadata.headings));
}

void MarkdownEditor::startParse()
{
    if (m_parseWatcher.isRunning())
        return; // the finished handler starts the next one

    const QString snapshot = m_text;
    const quint64 version = m_textVersion;
    const bool HTML = m_features.HTMLblocks;
    m_parseWatcher.setFuture(QtConcurrent::run([snapshot, version, HTML] {
        Parser::Result result = threadParser().parseAll(snapshot);
        if (HTML)
            expandEditableHTML(result.decorations, snapshot);
        ParseJob job;
        job.decorations = std::move(result.decorations);
        job.properties = std::move(result.metadata.properties);
        job.headings = std::move(result.metadata.headings);
        job.words = result.metadata.wordCount;
        sortDecorations(job.decorations);
        job.version = version;
        return job;
    }));
}

void MarkdownEditor::adoptDecorations(Decorations decorations, QList<Property> properties,
                                      QList<Heading> headings)
{
    m_decorations = std::move(decorations);
    m_properties = std::move(properties);
    m_headings = std::move(headings);
    m_lines = SourceMap(m_text);
    collectTabGroups();
    rebuildBlocks();
    indexDecorations();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
    emit headingsChanged();
    emit footnotesChanged();
}

void MarkdownEditor::indexDecorations()
{
    m_unresolved.clear();
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
    for (BlockView &view : m_blocks) {
        assign(m_decorations.syntax, [](const Range &r) { return r.start; },
               view.firstSyntax, view.lastSyntax, view.span.range, syntaxWalker);
        assign(m_decorations.styles, [](const StyleSpan &s) { return s.range.start; },
               view.firstStyle, view.lastStyle, view.span.range, styleWalker);
    }
}

void MarkdownEditor::invalidateLayouts()
{
    for (BlockView &view : m_blocks) {
        view.layout.reset();
        view.laidOut = false;
        view.heightEstimated = false;
        view.height = 0;
    }
}

qreal MarkdownEditor::lineHeightFor(const BlockSpan &span) const
{
    // Building a QFont and its metrics per block per layout pass is what made
    // a keystroke cost tens of milliseconds on a long note.
    const int key = int(span.kind) * 16 + span.level;
    const auto cached = m_lineHeights.constFind(key);
    if (cached != m_lineHeights.constEnd())
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
    m_lineHeights.insert(key, height);
    return height;
}

qreal MarkdownEditor::blockSpacing() const
{
    if (m_blockSpacing <= 0) {
        const int size = m_theme.textFont().pixelSize();
        m_blockSpacing = size > 0 ? size : 16;
    }
    return m_blockSpacing;
}

qreal MarkdownEditor::estimatedHeight(const BlockView &view) const
{
    // Lines in the source, plus an allowance for wrapping, is close enough
    // for a scrollbar; the real height arrives when the block scrolls in.
    const qsizetype lines =
        QStringView(m_text).mid(view.span.range.start, view.span.range.length())
            .count(QLatin1Char('\n'))
        + 1;
    return lines * lineHeightFor(view.span);
}

/// Paragraph blocks split on blank lines, for a note whose parse has not come
/// back yet. Without this an unparsed note is one block a megabyte long, and
/// laying that out as a single QTextLayout costs more than the parse did.
QList<BlockSpan> MarkdownEditor::plainParagraphs() const
{
    QList<BlockSpan> spans;
    qsizetype start = 0;
    const qsizetype size = m_text.size();
    for (qsizetype i = 0; i < size; ++i) {
        if (m_text.at(i) != QLatin1Char('\n'))
            continue;
        // A blank line ends a paragraph, exactly as Markdown says.
        if (i + 1 < size && m_text.at(i + 1) == QLatin1Char('\n')) {
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
        m_decorations.blocks.isEmpty() && !m_text.isEmpty() ? plainParagraphs()
                                                            : m_decorations.blocks;
    std::sort(spans.begin(), spans.end(),
              [](const BlockSpan &l, const BlockSpan &r) { return l.range.start < r.range.start; });

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
        if (view.span.range.end < m_text.size()
            && m_text.at(view.span.range.end) == QLatin1Char('\n')
            && !(view.span.range.end > view.span.range.start
                 && m_text.at(view.span.range.end - 1) == QLatin1Char('\n')))
            ++view.span.range.end;

        // A list becomes one block per top-level item, its nested items going
        // with their parent. As one block, a list was laid out whole on every
        // keystroke and every caret move into an element: 11.6 ms per keystroke
        // at 1 000 items, over the 8 ms budget, and 154 ms at 5 000. Blocks sit
        // with no spacing between them, so nothing on screen changes.
        if (span.kind == BlockSpan::Kind::List) {
            const auto items = m_decorations.listItems;
            auto item = std::lower_bound(items.cbegin(), items.cend(), span.range.start,
                                         [](const ListItemSpan &list, qsizetype at) {
                                             return list.range.start < at;
                                         });
            qsizetype from = view.span.range.start;
            for (; item != items.cend() && item->range.start < view.span.range.end; ++item) {
                if (item->indent != 0)
                    continue;
                // md4qt starts an item at its bullet; the block starts at its line.
                const qsizetype lineStart = m_lines.lineStart(m_lines.lineAt(item->range.start));
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
            const qsizetype firstLine = m_lines.lineAt(view.span.range.start);
            const qsizetype lastLine =
                m_lines.lineAt(qMax(view.span.range.start, view.span.range.end - 1));
            const int pieces = int((lastLine - firstLine) / kCodePieceLines) + 1;
            qsizetype from = view.span.range.start;
            for (int piece = 0; piece + 1 < pieces; ++piece) {
                const qsizetype to = m_lines.lineStart(firstLine + (piece + 1) * kCodePieceLines);
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
    addGap(cursor, m_text.size());

    std::vector<BlockView> blocks;
    blocks.reserve(parts.size() + 1); // + a footnotes list
    blocks.resize(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        blocks[i].span = parts[i].span;
        blocks[i].piece = parts[i].piece;
        blocks[i].pieces = parts[i].pieces;
    }

    // Every parse rebuilds the blocks. A piece keeps the highlighter state the
    // old piece ending where it ends had, so a listing is not re-highlighted
    // from its top to show the piece on screen - and that state is still
    // valid when the old piece covered exactly the same text.
    for (BlockView &view : blocks) {
        if (view.pieces <= 1)
            continue;
        const qsizetype at = view.span.range.end - 1;
        auto it = std::upper_bound(m_blocks.cbegin(), m_blocks.cend(), at,
                                   [](qsizetype offset, const BlockView &old) {
                                       return offset < old.span.range.start;
                                   });
        if (it == m_blocks.cbegin())
            continue;
        --it;
        if (it->pieces <= 1 || it->span.range.end != view.span.range.end)
            continue;
        view.codeEnd = it->codeEnd;
        view.codeEndValid = it->codeEndValid && it->piece == view.piece
                            && it->span.range.start == view.span.range.start;
    }

    if (blocks.empty()) {
        BlockView view;
        view.span = {BlockSpan::Kind::Paragraph, {0, m_text.size()}, 0};
        blocks.push_back(std::move(view));
    }

    // Reading view lists footnote definitions at the end instead of where
    // they sit in the source (see layoutBlock's Mode::Reading branch above)
    // - a block with no real source range, appended after everything else.
    if (m_mode == Mode::Reading && !m_decorations.footnoteDefinitions.isEmpty()) {
        BlockView view;
        view.span = {BlockSpan::Kind::Paragraph, {m_text.size(), m_text.size()}, 0};
        view.isFootnotesList = true;
        blocks.push_back(std::move(view));
    }

    m_blocks = std::move(blocks);
    m_activeBlock = blockAt(m_cursor);
}

QColor MarkdownEditor::linkColourAt(qsizetype sourceOffset) const
{
    for (const StyleSpan &span : m_decorations.styles) {
        if (span.kind != StyleSpan::Kind::Link || !span.range.contains(sourceOffset))
            continue;
        return m_theme.color(isUnresolvedLink(span.payload)
                                 ? QStringLiteral("--link-unresolved-color")
                                 : QStringLiteral("--text-accent"));
    }
    return {};
}

bool MarkdownEditor::isUnresolvedLink(const QString &target) const
{
    if (target.isEmpty() || !m_resolver)
        return false;
    // External links resolve to nothing and are not meant to.
    if (target.contains(QLatin1String("://")) || target.startsWith(QLatin1String("mailto:")))
        return false;

    const auto cached = m_unresolved.constFind(target);
    if (cached != m_unresolved.constEnd())
        return *cached;

    const bool missing = resolvesToNothing(target);
    m_unresolved.insert(target, missing);
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
    bool changed = false;
    for (auto it = m_unresolved.begin(); it != m_unresolved.end(); ++it) {
        const bool missing = resolvesToNothing(it.key());
        changed = changed || missing != it.value();
        it.value() = missing;
    }
    if (!changed)
        return;
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

qsizetype MarkdownEditor::blockAt(qsizetype sourceOffset) const
{
    if (m_blocks.empty())
        return -1;

    // Blocks are sorted and cover the document without gaps.
    auto it = std::upper_bound(m_blocks.cbegin(), m_blocks.cend(), sourceOffset,
                               [](qsizetype offset, const BlockView &view) {
                                   return offset < view.span.range.start;
                               });
    const qsizetype index = qsizetype(std::distance(m_blocks.cbegin(), it)) - 1;
    return qBound<qsizetype>(0, index, qsizetype(m_blocks.size()) - 1);
}

QFont MarkdownEditor::fontFor(const BlockSpan &span) const
{
    // A table only ever reaches here as its source, and its pipes line up
    // only in a font where every character is as wide as the next.
    if (span.kind == BlockSpan::Kind::Code || span.kind == BlockSpan::Kind::Frontmatter
        || span.kind == BlockSpan::Kind::Table) {
        QFont font = m_theme.monospaceFont();
        font.setPixelSize(m_theme.value(QStringLiteral("--font-ui-small")).toInt() + 1);
        return font;
    }

    QFont font = m_theme.textFont();
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
    m_spelling = std::move(spelling);
    invalidateLayouts();
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
    if (m_mode == Mode::Source)
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
            const StyleSpan &span = m_decorations.styles.at(i);
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
    if (!m_spelling.isCorrect)
        return {};
    const auto inWord = [](QChar c) {
        return c.isLetterOrNumber() || c.isMark() || c == QLatin1Char('\'')
               || c == QChar(0x2019) || c == QLatin1Char('_');
    };
    qsizetype start = m_cursor;
    while (start > 0 && inWord(m_text.at(start - 1)))
        --start;
    qsizetype end = m_cursor;
    while (end < m_text.size() && inWord(m_text.at(end)))
        ++end;
    return start < end ? Range{start, end} : Range{};
}

void MarkdownEditor::appendSpellingFormats(const BlockView &view,
                                           QList<QTextLayout::FormatRange> &formats) const
{
    if (!m_spelling.isCorrect)
        return;
    QTextCharFormat underline;
    underline.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    underline.setUnderlineColor(m_theme.color(QStringLiteral("--text-error")));
    for (const Range &source : checkedWordsOf(view)) {
        if (m_spelling.isCorrect(m_text.mid(source.start, source.length())))
            continue;
        const Range shown = view.map.toDisplay(source);
        formats.append({int(shown.start), int(shown.length()), underline});
    }
}

std::unique_ptr<QMenu> MarkdownEditor::spellingMenuAt(qsizetype sourceOffset)
{
    if (!m_spelling.isCorrect)
        return nullptr;
    const qsizetype index = blockAt(sourceOffset);
    if (index < 0)
        return nullptr;
    const BlockView &view = m_blocks[size_t(index)];
    if (!view.laidOut)
        return nullptr;
    for (const Range &source : checkedWordsOf(view)) {
        if (sourceOffset < source.start || sourceOffset > source.end)
            continue;
        const QString misspelt = m_text.mid(source.start, source.length());
        if (m_spelling.isCorrect(misspelt))
            return nullptr;

        auto menu = std::make_unique<QMenu>(this);
        const QStringList suggestions =
            m_spelling.suggestions ? m_spelling.suggestions(misspelt) : QStringList();
        for (const QString &suggestion : suggestions) {
            connect(menu->addAction(suggestion), &QAction::triggered, this,
                    [this, source, misspelt, suggestion] {
                        // Only if the word is still there: the menu may have
                        // stayed open over a reparse.
                        if (m_text.mid(source.start, source.length()) != misspelt)
                            return;
                        pushUndo();
                        m_lastEdit.invalidate();
                        applyEdit(source.start, source.length(), suggestion);
                    });
        }
        if (suggestions.isEmpty())
            menu->addAction(tr("No suggestions"))->setEnabled(false);
        if (m_spelling.addWord) {
            menu->addSeparator();
            connect(menu->addAction(tr("Add \"%1\" to dictionary").arg(misspelt)),
                    &QAction::triggered, this, [this, misspelt] {
                        m_spelling.addWord(misspelt);
                        invalidateLayouts();
                        updateLayout();
                        viewport()->update();
                    });
        }
        return menu;
    }
    return nullptr;
}

void MarkdownEditor::contextMenuEvent(QContextMenuEvent *event)
{
    const QPoint point = viewport()->mapFrom(this, event->pos());
    const qsizetype offset = offsetAt(point);
    std::unique_ptr<QMenu> menu = spellingMenuAt(offset);
    if (tableCellAt(offset).row >= 0) {
        if (!menu)
            menu = std::make_unique<QMenu>(this);
        else
            menu->addSeparator();
        addTableActions(*menu, offset);
    }
    if (menu)
        menu->exec(event->globalPos());
}

QList<QTextLayout::FormatRange> MarkdownEditor::formatsIn(const BlockView &view,
                                                          const Range &block,
                                                          const DisplayMap &map) const
{
    QList<QTextLayout::FormatRange> formats;

    const QColor accent = m_theme.color(QStringLiteral("--text-accent"));
    const QColor faint = m_theme.color(QStringLiteral("--text-faint"));
    const QColor codeColour = m_theme.color(QStringLiteral("--text-normal"));
    const QColor codeBackground = m_theme.color(QStringLiteral("--background-primary-alt"));
    const QColor highlight = m_theme.color(QStringLiteral("--color-yellow"));

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

    const QColor unresolved = m_theme.color(QStringLiteral("--link-unresolved-color"));

    // In source mode the markers stay visible, so colour them like Obsidian
    // rather than hiding them.
    if (m_mode == Mode::Source || view.active) {
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
            const Range &marker = m_decorations.syntax.at(i);
            if (!marker.overlaps(block))
                continue;
            QTextCharFormat format;
            format.setForeground(faint);
            add(marker, format);
        }
    }

    for (const ListItemSpan &item : listItemsIn(block)) {
        if (!item.done)
            continue;
        QTextCharFormat strike;
        strike.setFontStrikeOut(true);
        strike.setForeground(m_theme.color(QStringLiteral("--text-muted")));
        add({item.marker.end, item.range.end}, strike);
    }

    for (const NestedCallout &nested : view.nested) {
        if (!nested.span->titleRange.isValid() || !nested.span->titleRange.overlaps(block))
            continue;
        QTextCharFormat title;
        title.setFont(nested.style.titleFont(m_theme));
        title.setForeground(nested.style.headingColor);
        add(nested.span->titleRange, title);
    }

    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = m_decorations.styles.at(i);
        if (!span.range.overlaps(block))
            continue;

        QTextCharFormat format;
        switch (span.kind) {
        case StyleSpan::Kind::Bold:
            format.setFontWeight(QFont::Bold);
            break;
        case StyleSpan::Kind::Italic:
            format.setFontItalic(true);
            break;
        case StyleSpan::Kind::Strikethrough:
            format.setFontStrikeOut(true);
            break;
        case StyleSpan::Kind::InlineCode:
            format.setFont(m_theme.monospaceFont());
            format.setForeground(codeColour);
            format.setBackground(codeBackground);
            break;
        case StyleSpan::Kind::CodeBlock:
            if (view.span.kind == BlockSpan::Kind::Code)
                continue; // the block paints its own panel, font and tokens
            // A fence inside a callout or a blockquote is not a block of its
            // own, so it has nothing to paint its panel or set its font. It
            // takes both from here, and its tokens from codeFormatsFor.
            format.setFont(m_theme.monospaceFont());
            break;
        case StyleSpan::Kind::Link:
            // A link to a note that does not exist is dimmed, which is how a
            // typo in a link is spotted at all.
            format.setForeground(isUnresolvedLink(span.payload) ? unresolved : accent);
            break;
        case StyleSpan::Kind::Tag:
            format.setForeground(accent);
            break;
        case StyleSpan::Kind::Footnote:
            // Only tints the raw source, once the caret reveals it -
            // layoutFootnotes paints the hidden case's number itself.
            format.setForeground(accent);
            break;
        case StyleSpan::Kind::Highlight:
            format.setBackground(highlight);
            format.setForeground(m_theme.color(QStringLiteral("--color-base-00")));
            break;
        case StyleSpan::Kind::Math:
        case StyleSpan::Kind::Html:
            format.setForeground(m_theme.color(QStringLiteral("--color-purple")));
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
        case StyleSpan::Kind::HtmlTag:
        case StyleSpan::Kind::Heading:
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
    return formats;
}

Range MarkdownEditor::codeContentOf(const BlockView &view, QString *language) const
{
    Range content = view.span.range;

    // The fences, and the language tag on the opening one, are syntax; what
    // is left between them is the code.
    for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
        const Range &marker = m_decorations.syntax.at(i);
        if (marker.start <= content.start)
            content.start = qMax(content.start, marker.end);
        if (marker.end >= content.end)
            content.end = qMin(content.end, marker.start);
    }
    while (content.start < content.end && m_text.at(content.start) == QLatin1Char('\n'))
        ++content.start;

    if (language) {
        // The listing's style span starts in the fence's first piece, so only
        // that piece's slice of the decorations holds it.
        const BlockView &first = firstPiece(view);
        for (int i = first.firstStyle; i < first.lastStyle; ++i) {
            const StyleSpan &span = m_decorations.styles.at(i);
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
    QList<QTextLayout::FormatRange> formats = highlightPiece(view, &content);

    // Highlighter offsets are relative to the code; the layout wants display
    // offsets. Nothing inside a fence is ever hidden, so the shift is uniform.
    const qsizetype shift = view.map.toDisplay(content.start);
    for (QTextLayout::FormatRange &range : formats)
        range.start += int(shift);
    return formats;
}

QList<QTextLayout::FormatRange> MarkdownEditor::highlightPiece(BlockView &view, Range *content)
{
    QString language;
    *content = codeContentOf(view, &language);
    CodeHighlighter::State state =
        view.piece > 0 ? (&view - 1)->codeEnd : CodeHighlighter::State();
    QList<QTextLayout::FormatRange> formats;
    if (content->isValid() && content->length() > 0 && !language.isEmpty())
        formats = m_code.highlight(m_text.mid(content->start, content->length()), language, &state);
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
        const StyleSpan &span = m_decorations.styles.at(i);
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
        QList<QTextLayout::FormatRange> tokens =
            m_code.highlight(shown.mid(range.start, range.length()), span.payload);
        for (QTextLayout::FormatRange &token : tokens)
            token.start += int(range.start);
        formats += tokens;
    }
    return formats;
}

int MarkdownEditor::gutterWidth() const
{
    if (!m_lineNumbers || m_mode != Mode::Source)
        return 0;
    // Wide enough for the largest number this note can reach.
    const int digits = qMax(2, QString::number(qMax<qsizetype>(1, m_lines.lineCount())).size());
    return int(QFontMetricsF(m_theme.monospaceFont()).horizontalAdvance(QLatin1Char('0')))
               * digits
           + 18;
}

int MarkdownEditor::contentWidth() const
{
    const int available = viewport()->width() - 2 * m_minimumSideMargin - gutterWidth();
    if (!m_readable)
        return qMax(120, available);
    return qMax(120, qMin(available, m_contentWidth));
}

int MarkdownEditor::contentLeft() const
{
    return qMax(m_minimumSideMargin + gutterWidth(),
                (viewport()->width() - contentWidth()) / 2);
}

void MarkdownEditor::paintGutter(QPainter &painter, const BlockView &view, qreal top) const
{
    if (gutterWidth() == 0 || !view.layout)
        return;

    painter.save();
    QFont font = m_theme.monospaceFont();
    font.setPixelSize(int(font.pixelSize() * 0.9));
    painter.setFont(font);
    painter.setPen(m_theme.color(QStringLiteral("--text-faint")));

    const qreal right = contentLeft() - 12;
    for (int i = 0; i < view.layout->lineCount(); ++i) {
        const QTextLine line = view.layout->lineAt(i);
        const qsizetype source = view.map.toSource(line.textStart());
        // Only the first visual line of a wrapped source line is numbered.
        if (i > 0 && m_lines.lineAt(source) == m_lines.lineAt(view.map.toSource(
                         view.layout->lineAt(i - 1).textStart())))
            continue;
        painter.drawText(QRectF(right - gutterWidth(), top + line.y(), gutterWidth(),
                                line.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(m_lines.lineAt(source) + 1));
    }
    painter.restore();
}

bool MarkdownEditor::softBreakAt(qsizetype source) const
{
    // Source mode shows the file as it is written: one line of the file is one
    // line on screen, the way Obsidian's own source mode lays out.
    if (m_strictLineBreaks || m_mode == Mode::Source)
        return false;
    // A hard break: two spaces or a backslash before the newline.
    if (source >= 2 && m_text.at(source - 1) == QLatin1Char(' ')
        && m_text.at(source - 2) == QLatin1Char(' '))
        return false;
    if (source >= 1 && m_text.at(source - 1) == QLatin1Char('\\'))
        return false;

    // A nested callout's title stays in the layout, so the newline that ends
    // its header line has to break, or the title and the first line of the
    // body run together as one paragraph - which, in Markdown, they are.
    for (const CalloutSpan &callout : m_decorations.callouts) {
        if (callout.depth > 0 && callout.titleRange.isValid() && source >= callout.titleRange.end
            && source <= callout.titleRange.end + 1)
            return false;
    }

    for (const Range &paragraph : m_decorations.paragraphs) {
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
    const QList<ListItemSpan> &items = m_decorations.listItems;
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
    if (m_outlineNumbering && item.outline.contains(QLatin1Char('.')))
        body = item.outline;
    else
        body = QString::number(item.shownNumber > 0 ? item.shownNumber : item.number);
    if (m_listNumberSuffix.isEmpty()) // as the note wrote it
        return body + (item.bullet.isNull() ? QLatin1Char('.') : item.bullet);
    return body + m_listNumberSuffix;
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

    QColor guide = m_theme.color(QStringLiteral("--text-faint"));
    guide.setAlphaF(0.4f);

    painter.save();
    painter.setPen(QPen(guide, 1));
    const std::span<const ListItemSpan> items = listItemsIn(view.span.range);
    for (size_t i = 0; i < items.size() && i < view.listParent.size(); ++i) {
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
    if (!view.layout || view.listTextX.empty() || m_mode == Mode::Source)
        return;

    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));

    const auto markerShown = [&view](const ListItemSpan &item) {
        return std::any_of(view.revealed.cbegin(), view.revealed.cend(),
                           [&item](const Range &range) { return range.overlaps(item.marker); });
    };
    for (const ListItemSpan &item : listItemsIn(view.span.range)) {
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
            paintCheckBox(painter, QRectF(textX - box - 8, baseline + (line.height() - box) / 2, box, box),
                          item.done);
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
            paintBulletGlyph(painter, m_bulletGlyphs.glyphFor(item.bullet), item.bullet,
                             QPointF(textX - 12, baseline + line.height() / 2), muted,
                             view.layout->font());
        }
        painter.restore();
    }
}

void MarkdownEditor::toggleTask(const ListItemSpan &item)
{
    if (!item.task || !item.checkbox.isValid())
        return;
    // Ticking a box must not steal the caret; "[x]" and "[ ]" are the same
    // width, so the offsets it was on are still the right ones.
    const qsizetype anchor = m_anchor;
    const qsizetype caret = m_cursor;
    pushUndo();
    m_lastEdit.invalidate();
    applyEdit(item.checkbox.start, item.checkbox.length(),
              item.done ? QStringLiteral("[ ]") : QStringLiteral("[x]"));
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
    for (const CalloutSpan &callout : m_decorations.callouts) {
        if (callout.range.start == start)
            return &callout;
    }
    return nullptr;
}

qreal MarkdownEditor::calloutHeaderHeight(const CalloutStyle &style) const
{
    return QFontMetricsF(style.titleFont(m_theme)).height() * 1.9;
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
    const QFont titleFont = style.titleFont(m_theme);
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
    for (const TableSpan &table : m_decorations.tables) {
        if (table.range.start == view.span.range.start)
            return &table;
    }
    return nullptr;
}

const EmbedSpan *MarkdownEditor::soleEmbedOf(const BlockView &view) const
{
    for (const EmbedSpan &embed : m_decorations.embeds) {
        if (!embed.range.overlaps(view.span.range))
            continue;
        // Only when the embed is the whole block; an image in the middle of a
        // sentence stays inline until Phase 3 finishes inline objects.
        const QStringView rest =
            QStringView(m_text).mid(view.span.range.start, view.span.range.length());
        const QString trimmed = rest.trimmed().toString();
        if (trimmed == m_text.mid(embed.range.start, embed.range.length()))
            return &embed;
    }
    return nullptr;
}

void MarkdownEditor::hideWholeBlockText(BlockView &view)
{
    view.map = DisplayMap::build(m_text, view.span.range, {view.span.range});
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
    static const QByteArrayList images = QImageReader::supportedImageFormats();

    // Some installs register a PDF-reading QImageIOHandler (a thumbnailer
    // plugin, commonly), which would otherwise steal every PDF into a flat
    // one-page raster here before the richer, page-aware PdfPage path below
    // ever gets a look at it.
    if (suffix != QLatin1String("pdf") && images.contains(suffix.toUtf8()))
        return layoutImage(view, *embed, path);
    // Obsidian's audio and video formats, and the two Ogg audio spellings
    // its list leaves out (the freedesktop sounds are .oga).
    static const QStringList audio = {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("m4a"),
                                      QStringLiteral("ogg"), QStringLiteral("3gp"), QStringLiteral("flac"),
                                      QStringLiteral("opus"), QStringLiteral("oga")};
    static const QStringList video = {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("ogv"),
                                      QStringLiteral("mov"), QStringLiteral("mkv")};
    if (m_features.mediaEmbeds && (audio.contains(suffix) || video.contains(suffix)))
        return layoutMedia(view, path, video.contains(suffix));
    if (m_features.PDFembeds && suffix == QLatin1String("pdf"))
        return layoutPdf(view, *embed, path);
    if (m_features.texEmbeds && suffix == QLatin1String("tex"))
        return layoutTexEmbed(view, *embed, path);
    if (!m_features.fileEmbeds)
        return false; // falls through to the paragraph's plain link text

    if (Vault::isNote(path))
        return layoutNoteEmbed(view, *embed, path);
    // A .base is the ```base block's own table, fed the file: its rows open
    // their notes, as they do in Obsidian's embedded base.
    if (m_features.baseBlocks && suffix == QLatin1String("base")) {
        const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        auto cached = m_baseFiles.find(path);
        if (cached == m_baseFiles.end() || cached->first != mtime) {
            QFile file(path);
            const QString text = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
            cached = m_baseFiles.insert(path, {mtime, text});
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
    if (m_fileEmbedRenderers.contains(suffix))
        return layoutHookedFileEmbed(view, *embed, path);
    return layoutSourceFile(view, *embed, path);
}

bool MarkdownEditor::layoutImage(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    QPixmap pixmap = m_images.value(path);
    if (pixmap.isNull()) {
        pixmap = QPixmap(path);
        if (pixmap.isNull())
            return false;
        m_images.insert(path, pixmap);
    }

    // Obsidian caps an image at the readable column and honours |WxH.
    int width = embed.width > 0 ? embed.width : pixmap.width();
    width = qMin(width, contentWidth());
    int height = embed.height > 0 ? embed.height
                                  : int(qreal(width) * pixmap.height() / pixmap.width());

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

    view.map = DisplayMap::build(m_text, view.span.range, {view.span.range});
    view.layout = std::make_unique<QTextLayout>(shown);
    view.layout->setCacheEnabled(true);
    view.layout->setFont(fontFor(codeSpan));

    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    view.layout->setTextOption(option);
    // The file's own extension picks the Kate definition.
    view.layout->setFormats(m_code.highlight(body, info.suffix()));

    const qreal lineHeight = lineHeightFor(codeSpan);
    view.layout->beginLayout();
    qreal height = 0;
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
    for (const MathSpan &math : m_decorations.maths) {
        if (!math.display || !math.range.overlaps(view.span.range))
            continue;

        QString rest = m_text.mid(view.span.range.start, view.span.range.length());
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
    if (!m_features.maths)
        return false; // shows as source - no MicroTeX call, no image cached

    const MathSpan *math = displayMathOf(view);
    if (!math)
        return false;

    const Range source = math->formula.isValid() && math->formula.length() > 0 ? math->formula
                                                                               : math->range;
    const QString latex = m_text.mid(source.start, source.length()).trimmed();
    const qreal ratio = devicePixelRatioF();
    const QString key = QStringLiteral("math|%1|%2|%3")
                            .arg(latex)
                            .arg(m_theme.value(QStringLiteral("--font-text-size")))
                            .arg(contentWidth());

    QPixmap pixmap = m_images.value(key);
    if (pixmap.isNull()) {
        const QImage image = m_math.render(latex, m_theme.textFont().pixelSize() * 1.2,
                                           m_theme.color(QStringLiteral("--text-normal")),
                                           contentWidth(), ratio);
        if (image.isNull())
            return false; // falls back to showing the source, not to an error
        pixmap = QPixmap::fromImage(image);
        m_images.insert(key, pixmap);
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
    if (m_decorations.maths.isEmpty())
        return; // the common case: nothing to check, so nothing to render

    const QFont font = fontFor(view.span);
    const QColor colour = m_theme.color(QStringLiteral("--text-normal"));
    const qreal ratio = devicePixelRatioF();

    for (const MathSpan &math : m_decorations.maths) {
        if (math.display || !math.range.overlaps(view.span.range))
            continue;

        // The parser hid this exact range unconditionally; find it so it can
        // be dropped again if the formula turns out not to render - which,
        // with the feature off, it never even tries to.
        const Range hideRange{math.formula.start, math.range.end};
        auto it = std::find(hidden.begin(), hidden.end(), hideRange);
        if (it == hidden.end())
            continue;
        if (!m_features.maths) {
            hidden.erase(it);
            continue;
        }

        const QString latex = m_text.mid(math.formula.start, math.formula.length()).trimmed();
        const QString key = QStringLiteral("inlineMath|%1|%2|%3")
                                .arg(latex)
                                .arg(font.pixelSize())
                                .arg(colour.name(QColor::HexArgb));

        QPixmap pixmap = m_images.value(key);
        if (pixmap.isNull()) {
            // Inline maths is set in text style, as TeX sets `$...$`: limits
            // beside a sum rather than above and below it, and a fraction at
            // the size of the text around it. Rendered in display style, a
            // \sum was taller than the line and ran into the one below.
            const QImage image = m_math.render(QStringLiteral("\\textstyle ") + latex,
                                               font.pixelSize(), colour, contentWidth(), ratio);
            if (image.isNull()) {
                hidden.erase(it); // not renderable: show the raw source instead of a gap
                continue;
            }
            pixmap = QPixmap::fromImage(image);
            m_images.insert(key, pixmap);
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
        const StyleSpan &span = m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::Link || !span.anchorHash.isValid()
            || !span.anchorText.isValid() || span.anchorText.length() <= 0)
            continue;
        // A block reference stays as written: what Obsidian shows for one is
        // not recorded anywhere this was checked.
        if (m_text.at(span.anchorText.start) == QLatin1Char('^'))
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
             m_theme.color(isUnresolvedLink(span.payload) ? QStringLiteral("--link-unresolved-color")
                                                           : QStringLiteral("--text-accent"))});
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

    const QFont font = m_theme.textFont();
    const qreal lineHeight = QFontMetricsF(font).height() * 1.4;
    const qreal padX = 10;
    const qreal padY = 6;
    const qreal minimum = QFontMetricsF(font).horizontalAdvance(QLatin1Char('m')) * 3;

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
            const bool editing = view.active && m_anchor == m_cursor && cell.source.start <= m_cursor
                                 && m_cursor <= cell.source.end;
            QList<Range> hidden;
            for (int i = view.firstSyntax; i < view.lastSyntax && !editing; ++i) {
                const Range &marker = m_decorations.syntax.at(i);
                if (marker.overlaps(cell.source))
                    hidden.append(marker);
            }
            // An entity stays one character even in the cell being edited.
            cell.map = DisplayMap::build(m_text, cell.source, hidden, m_decorations.substitutions);

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
            cell.layout->setFormats(formatsIn(view, cell.source, cell.map));

            natural[c] = qMax(natural.at(c),
                              QFontMetricsF(cellFont).horizontalAdvance(shown) + 2 * padX);
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

    // Obsidian does not squeeze a table into the readable column: every column
    // keeps the width its content wants, and a table too wide to fit scrolls
    // sideways inside its own block. Only a single runaway column is capped,
    // so one long cell wraps instead of pushing everything else off screen.
    const qreal available = contentWidth();
    const qreal columnCap = qMax(minimum * 4, available * 0.66);

    QList<qreal> widths;
    QList<qreal> offsets;
    qreal x = 0;
    for (const qreal width : std::as_const(natural)) {
        const qreal used = qBound(minimum, width, columnCap);
        widths << used;
        offsets << x;
        x += used;
    }
    view.tableWidth = x;

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
            line.setPosition(QPointF(0, height));
            height += lineHeight;
        }
        cell.layout->endLayout();
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
    view.tableScroll = qBound<qreal>(0, view.tableScroll,
                                     qMax<qreal>(0, view.tableWidth - contentWidth()));
    view.layout.reset();
    view.height = y;
    view.laidOut = true;
    return true;
}

void MarkdownEditor::paintTable(QPainter &painter, const BlockView &view, qreal left,
                                qreal top) const
{
    const QColor border = m_theme.color(QStringLiteral("--background-modifier-border"));
    const QColor header = m_theme.color(QStringLiteral("--background-secondary"));
    const QColor text = m_theme.color(QStringLiteral("--text-normal"));

    // A wide table is clipped to the readable column and scrolls inside it;
    // nothing spills over the note's text.
    const qreal overflow = qMax<qreal>(0, view.tableWidth - contentWidth());
    const QRectF window(left, top, contentWidth(), view.height);
    painter.save();
    if (overflow > 0)
        painter.setClipRect(window.adjusted(-1, -1, 1, 1));
    const qreal shift = -view.tableScroll;

    for (const Cell &cell : view.cells) {
        const QRectF rect = cell.rect.translated(left + shift, top);
        if (rect.right() < window.left() - 2 || rect.left() > window.right() + 2)
            continue;
        if (cell.row < view.headerRows)
            painter.fillRect(rect, header);
        if (cell.fill.isValid())
            painter.fillRect(rect, cell.fill);
        painter.setPen(QPen(border, 1));
        painter.drawRect(rect);

        // A selection is drawn in each cell it reaches; one that runs across
        // cells tints the ones it covers whole, so it reads as a block.
        QList<QTextLayout::FormatRange> selected;
        const Range selection = this->selection();
        if (selection.length() > 0 && cell.source.isValid()
            && selection.start <= cell.source.end && cell.source.start <= selection.end) {
            QColor background = m_theme.color(QStringLiteral("--interactive-accent"));
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
        painter.restore();
    }
    painter.restore();

    if (overflow <= 0)
        return;

    // Edge fades say there is more table, and a thin bar says where you are.
    const QColor background = m_theme.color(QStringLiteral("--background-primary"));
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
    if (view.tableScroll > 1)
        fade(QRectF(window.left(), window.top(), 18, window.height()), true);
    if (view.tableScroll < overflow - 1)
        fade(QRectF(window.right() - 18, window.top(), 18, window.height()), false);

    const qreal fraction = contentWidth() / view.tableWidth;
    const qreal barWidth = qMax<qreal>(28, contentWidth() * fraction);
    const qreal barX = left + (contentWidth() - barWidth) * (view.tableScroll / overflow);
    QColor bar = m_theme.color(QStringLiteral("--text-faint"));
    bar.setAlphaF(0.5f);
    painter.fillRect(QRectF(barX, top + view.height + 2, barWidth, 3), bar);
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
    const bool current = m_media && m_media->path() == view.mediaPath;
    const bool playing = current && m_media->isPlaying();
    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));
    const QColor accent = m_theme.color(QStringLiteral("--interactive-accent"));

    painter.save();
    const QRectF screen = mediaScreenRect(view, left, top);
    if (view.mediaVideo) {
        painter.fillRect(screen, Qt::black);
        const QImage frame = current ? m_media->frame() : QImage();
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

    QFont small = m_theme.uiFont();
    small.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
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
    const qint64 duration = current ? m_media->duration() : 0;
    const qint64 position = current ? m_media->position() : 0;
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
    if (!m_media) {
        m_media = new MediaPlayback(this);
        // Only the playing card repaints as it plays, not the whole view.
        connect(m_media, &MediaPlayback::changed, this, [this] {
            const qreal scroll = verticalScrollBar()->value();
            for (const BlockView &block : m_blocks) {
                if (block.mediaPath == m_media->path()) {
                    viewport()->update(QRect(0, int(block.y - scroll) - 8, viewport()->width(),
                                             int(block.height) + 16));
                }
            }
        });
    }
    const bool current = m_media->path() == view.mediaPath;
    if (bar.contains(point) && current && m_media->duration() > 0) {
        const qreal fraction = qBound<qreal>(0, (point.x() - bar.left()) / bar.width(), 1);
        m_media->seek(qint64(fraction * m_media->duration()));
    } else if (current && m_media->isPlaying()) {
        m_media->pause();
    } else {
        m_media->play(view.mediaPath);
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
    painter.fillPath(rounded, m_theme.color(QStringLiteral("--background-primary-alt")));

    if (view.embedLabel.isEmpty())
        return;

    painter.save();
    QFont label = m_theme.uiFont();
    label.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
    painter.setFont(label);
    painter.setPen(m_theme.color(QStringLiteral("--text-faint")));
    painter.drawText(QRectF(panel.left() + 12, panel.top() + 6, panel.width() - 24,
                            view.textTop),
                     Qt::AlignVCenter | Qt::AlignLeft, view.embedLabel);
    painter.restore();
}

MarkdownEditor::BlockView *MarkdownEditor::scrollableTableAt(const QPoint &viewportPoint)
{
    const qreal y = viewportPoint.y() + verticalScrollBar()->value();
    for (BlockView &view : m_blocks) {
        if (view.cells.empty() || view.tableWidth <= contentWidth())
            continue;
        if (y >= view.y && y <= view.y + view.height + 6)
            return &view;
    }
    return nullptr;
}

void MarkdownEditor::layoutBlock(BlockView &view)
{
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
    if (m_mode != Mode::Source && !m_tabGroups.empty()) {
        if (const TabGroup *group = tabGroupAt(view.span.range.start)) {
            if (caretTabGroup() != group && view.span.range.start < group->hiddenEnd) {
                const CodeFenceSpan &shown =
                    m_decorations.codeFences.at(group->first + selectedTabOf(*group));
                if (view.span.kind == BlockSpan::Kind::Code && codeFenceOf(view) == &shown) {
                    view.tabGroup = int(group - m_tabGroups.data());
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
    if (m_mode == Mode::Reading && footnoteDefinitionBlock(view)) {
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
    if (view.span.kind == BlockSpan::Kind::Frontmatter && m_mode != Mode::Source
        && layoutProperties(view))
        return;

    const bool renderMedia = m_mode != Mode::Source && !view.active;
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
    const bool renderTable = view.span.kind == BlockSpan::Kind::Table && m_mode != Mode::Source
                             && (!view.active || (m_anchor == m_cursor && tableCellAt(m_cursor).row >= 0));
    if (renderTable && layoutTable(view))
        return;

    // `#| echo: false` hides the listing in live preview while the caret is
    // elsewhere - Quarto's own meaning, independent of eval. Read further
    // down, in the same `!reveal` pass every other per-block hide range goes
    // through; false here whenever that pass cannot even run (source mode,
    // or the caret already in the block, which reveals everything anyway).
    bool notebookEchoHidden = false;
    if (renderMedia && view.span.kind == BlockSpan::Kind::Code) {
        view.options = codeOptionsFor(view);
        // Decided early, like view.options just above, because it feeds the
        // same "does this block get a header strip" decision below - the
        // output itself (appendNotebookOutput) is only built once the rest
        // of this block's own layout is settled, the same distance
        // appendEmbedRows keeps from layoutEmbed.
        if (m_features.notebooks && m_notebookRunner && view.piece == 0) {
            const CodeFenceSpan *fence = codeFenceOf(view);
            // `#| eval: false` - never runs, so no run control and no cache
            // lookup either: the same "off" a null runner already gives.
            view.notebookEligible = fence && !fence->language.trimmed().isEmpty()
                                    && !(view.options && !view.options->eval);
        }
        notebookEchoHidden = view.options && !view.options->echo && !view.active;
    }

    const bool perElement = m_mode == Mode::LivePreview && view.active && !revealsWholeBlock(view);
    const bool reveal = m_mode == Mode::Source
                        || (m_mode == Mode::LivePreview && view.active && !perElement);
    view.revealed = perElement ? revealedSyntax(view) : QList<Range>();
    view.caretWord = view.active ? wordAtCaret() : Range{};
    QList<Range> hidden;
    // Embeds inside this block that layoutEmbed did not already render whole
    // - several sharing a paragraph, or one mixed in with running text,
    // drawn inline; see collectTextEmbeds' comment in the header.
    QList<const EmbedSpan *> textEmbeds;
    if (!reveal) {
        hidden.reserve(view.lastSyntax - view.firstSyntax);
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
            const Range &marker = m_decorations.syntax.at(i);
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
        if (renderMedia && m_features.fileEmbeds)
            collectTextEmbeds(view, hidden, textEmbeds);
        if (notebookEchoHidden)
            hidden.append(view.span.range);
    }
    view.map = reveal ? DisplayMap::identity(m_text, view.span.range)
                      : DisplayMap::build(m_text, view.span.range, hidden, m_decorations.substitutions);
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
    // Which newlines actually break is CommonMark's rule, which is also
    // Obsidian's default: a newline inside a paragraph joins the lines, one
    // between blocks does not. Two trailing spaces still force a break.
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

    view.callout = view.span.kind == BlockSpan::Kind::Callout ? calloutFor(view) : nullptr;
    if (view.callout)
        view.calloutStyle = m_calloutStyles.resolve(view.callout->type, view.callout->attributes,
                                                    m_theme);
    // Callouts nest, and only the outermost one is a block. The rest are
    // painted inside this layout, so they have to be found before it is built:
    // each nested one indents the lines it covers.
    view.nested.clear();
    if (view.callout) {
        for (const CalloutSpan &callout : m_decorations.callouts) {
            // One that began in an earlier piece still covers lines of this.
            if (callout.depth > 0 && callout.range.overlaps(view.span.range))
                view.nested.push_back(
                    {&callout,
                     m_calloutStyles.resolve(callout.type, callout.attributes, m_theme)});
        }
    }

    // A table's source is monospace and its spaces unbreakable, so a wrapped
    // row breaks at the same column as the rows around it: Qt otherwise hangs
    // a run of spaces off the end of a line, and a padded row would start
    // its next line a few columns early. The same length, so offsets hold.
    if (view.span.kind == BlockSpan::Kind::Table)
        shown.replace(QLatin1Char(' '), QChar(QChar::Nbsp));
    view.layout = std::make_unique<QTextLayout>(shown);
    view.layout->setCacheEnabled(true);
    // A callout may name its own body font, size and effects; everything else
    // takes the font its kind implies.
    view.layout->setFont(view.callout ? view.calloutStyle.bodyFont(m_theme) : fontFor(view.span));

    QTextOption option;
    // `#| wrap: false` is Quarto asking for a listing that does not reflow;
    // a table's source wraps by the character, not after a pipe (see above);
    // everything else wraps inside the column.
    option.setWrapMode(view.options && !view.options->wrap              ? QTextOption::NoWrap
                       : view.span.kind == BlockSpan::Kind::Table ? QTextOption::WrapAnywhere
                                                                   : QTextOption::WrapAtWordBoundaryOrAnywhere);
    view.layout->setTextOption(option);
    QList<QTextLayout::FormatRange> formats = formatsFor(view);
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
                                          .horizontalAdvance(m_text.at(mark.sourcePos)));
        formats.append({int(mark.displayPos), 1, format});
    }
    prepareTextEmbeds(view, textEmbeds, formats);
    view.layout->setFormats(formats);

    // A list's lines, and a list's inside a quote or callout outside source
    // mode, sit at their own item's text column - a wrapped line too.
    const bool listLines = view.span.kind == BlockSpan::Kind::List
                           || (m_mode != Mode::Source
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
    view.layout->beginLayout();
    qreal height = 0;
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
                          - (view.span.kind == BlockSpan::Kind::Callout ? 28 : 0));
        // A line carrying an inline embed taller than its text grows above
        // the text to hold it.
        const qreal grow = view.embedCells.empty()
                               ? 0
                               : qMax<qreal>(0, textEmbedHeightOn(view, line) - lineHeight);
        line.setPosition(QPointF(indent, height + grow));
        height += lineHeight + grow;
    }
    view.layout->endLayout();
    view.height = qMax(height, lineHeight);

    // Obsidian's live preview puts one --p-spacing of air over a heading and
    // none under it; the blank line in the source is the rest of the rhythm.
    // Source mode has no block spacing at all: one line is one line.
    if (view.span.kind == BlockSpan::Kind::Heading && m_mode != Mode::Source)
        view.textTop = blockSpacing();

    if (view.callout) {
        // The title line is drawn by the callout header, so the body starts
        // below it; an empty callout still shows its title. A later piece
        // carries on the body.
        view.textTop = view.piece == 0 ? calloutHeaderHeight(view.calloutStyle) : 0;
        view.height = height;
    } else if ((view.options || view.tabGroup >= 0 || view.notebookEligible) && view.piece == 0) {
        view.textTop = codeHeaderHeight(); // the `#|` header, the tab strip, or the run control
    }

    // A folded fence folds all of its pieces; only the first keeps its header.
    view.folded = isFoldable(view)
                  && m_folds.value(firstPiece(view).span.range.start, defaultFoldOf(view));
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
            QString source = m_text.mid(content.start, content.length());
            if (source.endsWith(QLatin1Char('\n')))
                source.chop(1);
            // Which fence this is across edits: its place among the note's
            // fences (a block inserted above shifts it, which at worst shows
            // a neighbour's old output marked outdated).
            const QString blockID = QStringLiteral("f%1").arg(fence - m_decorations.codeFences.constData());
            appendNotebookOutput(view, fence->language, source, blockID, QString(), view.options);
        }
    }
    view.laidOut = true;
}

void MarkdownEditor::updateLayout()
{
    if (m_blocks.empty())
        return;

    // Obsidian keeps the caret's own block raw even when the pane loses
    // focus, so interaction - not focus - is what reveals it.
    const TabGroup *caretGroup = caretTabGroup();
    // The caret's block, or for a long fence its first piece: the caret
    // anywhere in a fence shows the whole of it as written.
    const qsizetype caretFirst =
        m_activeBlock >= 0 && m_activeBlock < qsizetype(m_blocks.size())
            ? m_activeBlock - m_blocks[size_t(m_activeBlock)].piece
            : -1;
    // A block shows its raw source only for a bare caret. With a selection it
    // stays rendered, as in Obsidian, and while the mouse button is held
    // nothing changes at all: revealing the syntax mid-drag moved the text out
    // from under the pointer, so the selection chased it.
    const bool bareCaret =
        m_mode == Mode::LivePreview && m_interacted && (m_vim.enabled || selection().length() == 0);

    // One pass over the note, which is the only part of this proportional to
    // its length: settle what each block reveals, and place it.
    m_movedFrom = qInf();
    qreal y = m_topPadding;
    for (qsizetype i = 0; i < qsizetype(m_blocks.size()); ++i) {
        BlockView &view = m_blocks[size_t(i)];
        const bool inCaretGroup = caretGroup && view.span.range.start >= caretGroup->span.start
                                  && view.span.range.start < caretGroup->span.end;
        if (inCaretGroup != view.inCaretGroup) {
            view.inCaretGroup = inCaretGroup;
            view.layout.reset();
            view.laidOut = false;
            view.heightEstimated = false;
        }
        const bool active = m_pressing ? view.active : bareCaret && i - view.piece == caretFirst;
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

        if (view.y != y && m_movedFrom > y)
            m_movedFrom = y;
        view.y = y;
        if (!view.laidOut && !view.heightEstimated) {
            const qreal before = view.height;
            view.height = estimatedHeight(view);
            view.heightEstimated = true;
            if (view.height != before && m_movedFrom > y)
                m_movedFrom = y;
        }
        y += view.height + kBlockSpacing;
    }
    m_totalHeight = y + m_topPadding;

    // Lay out what is on screen, plus a screen either side so scrolling does
    // not stutter, plus the caret's block wherever it is. Blocks are placed in
    // order, so the first one near the screen is a binary search away.
    const qreal scroll = verticalScrollBar()->value();
    const qreal top = scroll - viewport()->height();
    const qreal bottom = scroll + 2 * viewport()->height();
    const auto nearTop = std::partition_point(m_blocks.begin(), m_blocks.end(),
                                              [top](const BlockView &view) {
                                                  return view.y + view.height < top;
                                              });
    const qsizetype from = std::distance(m_blocks.begin(), nearTop);

    qsizetype firstChanged = -1;
    const auto layOut = [this, &firstChanged](qsizetype i) {
        BlockView &view = m_blocks[size_t(i)];
        if (view.laidOut)
            return;
        const qreal before = view.height;
        layoutBlock(view);
        if (!qFuzzyCompare(before + 1, view.height + 1) && (firstChanged < 0 || i < firstChanged))
            firstChanged = i;
    };
    // In index order: laying out a piece of a long fence can clear the
    // layouts of the pieces after it, and those have to be redone here.
    const bool caretAbove = m_activeBlock >= 0 && m_activeBlock < from;
    if (caretAbove)
        layOut(m_activeBlock);
    qsizetype i = from;
    for (; i < qsizetype(m_blocks.size()) && m_blocks[size_t(i)].y <= bottom; ++i)
        layOut(i);
    if (m_activeBlock >= i && m_activeBlock < qsizetype(m_blocks.size()))
        layOut(m_activeBlock);

    // Only what follows a block whose height changed moves.
    if (firstChanged >= 0) {
        m_movedFrom = qMin(m_movedFrom, m_blocks[size_t(firstChanged)].y);
        qreal y = firstChanged == 0 ? m_topPadding
                                    : m_blocks[size_t(firstChanged - 1)].y
                                          + m_blocks[size_t(firstChanged - 1)].height
                                          + kBlockSpacing;
        for (qsizetype j = firstChanged; j < qsizetype(m_blocks.size()); ++j) {
            BlockView &view = m_blocks[size_t(j)];
            view.y = y;
            if (!view.laidOut && !view.heightEstimated) {
                view.height = estimatedHeight(view);
                view.heightEstimated = true;
            }
            y += view.height + kBlockSpacing;
        }
        m_totalHeight = y + m_topPadding;
    }
    updateScrollRange();
}

QList<MarkdownEditor::BlockBox> MarkdownEditor::blockBoxes()
{
    updateLayout();
    QList<BlockBox> boxes;
    boxes.reserve(qsizetype(m_blocks.size()));
    for (const BlockView &view : m_blocks) {
        const Range &range = view.span.range;
        // A blank line draws nothing, so a callout panel reaching into it is
        // not an overlap; it is what keeps two panels 8 px apart.
        const bool blank = view.span.kind == BlockSpan::Kind::Paragraph
                           && QStringView(m_text).mid(range.start, range.length()).trimmed().isEmpty();
        // A callout's panel reaches 8 px above its first piece and below its
        // last (see paintCallout), outside view.height.
        const bool callout = view.span.kind == BlockSpan::Kind::Callout;
        const qreal above = callout && view.piece == 0 ? 8 : 0;
        const qreal below = callout && (view.piece == view.pieces - 1 || view.pieces <= 1) ? 8 : 0;
        boxes.append({blank ? QRectF() : QRectF(0, view.y - above, contentWidth(), view.height + above + below),
                      view.span.kind, range});
    }
    return boxes;
}

void MarkdownEditor::scrollContentsBy(int dx, int dy)
{
    // The property editor is a widget over the table, and would stay put
    // while the table moved away under it.
    finishPropertyEdit(true);
    updateLayout(); // blocks scrolling into view need their real layout

    // A wheel notch moves most of the screen by a few lines, and unless the
    // layout just moved something that is on screen those pixels are still
    // right: move them, and paint only the strip that came in. An OpenGL
    // viewport has no backing store to move pixels in, so it repaints.
    const bool settled = m_movedFrom > verticalScrollBar()->value() + viewport()->height();
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
    verticalScrollBar()->setRange(0, qMax(0, int(m_totalHeight) - page));
}

// --- painting -------------------------------------------------------------

void MarkdownEditor::paintBlocks(QPainter &painter, qreal scroll, qreal exposedTop,
                                 qreal exposedBottom) const
{
    const qreal left = contentLeft();
    const QColor text = m_theme.color(QStringLiteral("--text-normal"));
    const QColor selectionColour = m_theme.color(QStringLiteral("--interactive-accent"));

    const Range selected = selection();
    painter.setPen(text);

    const auto firstExposed = std::partition_point(
        m_blocks.cbegin(), m_blocks.cend(), [scroll, exposedTop](const BlockView &view) {
            return view.y - scroll + view.height < exposedTop;
        });
    for (auto it = firstExposed; it != m_blocks.cend(); ++it) {
        const BlockView &view = *it;
        const qreal top = view.y - scroll;
        if (top > exposedBottom)
            break;
        if (view.tabHidden)
            continue;

        if (view.span.kind == BlockSpan::Kind::Frontmatter && m_mode != Mode::Source
            && !m_properties.isEmpty()) {
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
        if (view.span.kind == BlockSpan::Kind::Rule && m_mode != Mode::Source && !view.active) {
            const qreal y = std::round(top + view.height / 2.0) + 0.5;
            painter.save();
            painter.setPen(QPen(m_theme.color(QStringLiteral("--background-modifier-border")), 2));
            painter.drawLine(QPointF(left, y), QPointF(left + contentWidth(), y));
            painter.restore();
            continue;
        }

        if (view.span.kind == BlockSpan::Kind::Code && !view.isQuery && !view.isBase && !view.isPluginBlock) {
            paintCodeChrome(painter, view, left, top);
            if (view.notebookEligible && !view.folded)
                paintNotebookOutput(painter, view, left, top);
            painter.setPen(text);
        }

        if (view.folded)
            continue; // the header is all a folded block shows

        QList<QTextLayout::FormatRange> selectionRanges;
        // Vim's block selection is several column strips, not one span.
        const bool strips = m_vim.enabled && !m_vim.overrideBlocks.isEmpty() && selected.length() > 0
                            && m_cursor == m_vim.overrideCursor && m_anchor == m_vim.overrideAnchor;
        const QList<Range> parts = strips ? m_vim.overrideBlocks : QList<Range>{selected};
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
            painter.drawPixmap(target.toRect(), view.image);
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
            if (m_mode != Mode::Source) {
                // Behind the text, outermost first: a nested callout's panel,
                // then any listing's panel on top of it.
                paintNestedCallouts(painter, view, left, top);
                if (view.span.kind != BlockSpan::Kind::Code)
                    paintNestedCodePanels(painter, view, left, top);
            }
            if (m_indentGuides && !view.listTextX.empty())
                paintIndentGuides(painter, view, left, top + drop);
            // A listing that asked not to wrap runs off the end of its line;
            // the panel is where it stops, not the window.
            const bool clip = view.options && !view.options->wrap;
            if (clip) {
                painter.save();
                painter.setClipRect(
                    QRectF(left - 14, top, contentWidth() + 28, view.height));
            }
            view.layout->draw(&painter, QPointF(left + indent, top + drop), selectionRanges);
            paintInlineMaths(painter, view, left + indent, top + drop);
            paintAnchorSeparators(painter, view, left + indent, top + drop);
            paintFootnoteMarks(painter, view, left + indent, top + drop);
            paintListMarkers(painter, view, left + indent, top + drop);
            paintGutter(painter, view, top + drop);
            if (clip)
                painter.restore();
        }
    }
}

void MarkdownEditor::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    // After a scroll only the strip that came into view is exposed; blocks
    // outside it are skipped rather than drawn and clipped away. The margin
    // covers what a block paints past its own edges, like a code panel's.
    const QRect exposed = event->rect();
    const qreal exposedTop = exposed.top() - 16;
    const qreal exposedBottom = exposed.bottom() + 16;
    painter.fillRect(exposed, m_theme.color(QStringLiteral("--background-primary")));

    paintBlocks(painter, verticalScrollBar()->value(), exposedTop, exposedBottom);

    m_caretPainted = QRect();
    if (hasFocus() && m_cursorOn && m_mode != Mode::Reading) {
        const QRectF caret = caretRect();
        if (!caret.isEmpty()) {
            if (m_vim.enabled && m_vim.mode != VimState::Mode::Insert) {
                QColor block = m_theme.color(QStringLiteral("--text-normal"));
                block.setAlphaF(0.55f);
                painter.fillRect(caret, block);
            } else {
                painter.setPen(QPen(m_theme.color(QStringLiteral("--text-normal")), 2));
                painter.drawLine(caret.topLeft() + QPointF(1, 0), caret.bottomLeft() + QPointF(1, 0));
            }
            m_caretPainted = caret.toAlignedRect().adjusted(-2, -2, 2, 2);
        }
    }
    paintSelectionHandles(painter);
}

QPointF MarkdownEditor::cursorPoint() const
{
    return pointFor(m_cursor);
}

QRectF MarkdownEditor::caretRect() const
{
    qreal height = 0;
    const QPointF point = pointFor(m_cursor, &height);
    if (point.isNull() && m_cursor != 0)
        return {};
    if (height <= 0) // no laid-out line under it: one line of the body text
        height = QFontMetricsF(font()).height();
    if (m_vim.enabled && m_vim.mode != VimState::Mode::Insert) {
        // Normal and visual mode: a block over the character the next
        // command acts on, as wide as that character is drawn.
        qreal width = QFontMetricsF(font()).averageCharWidth();
        if (m_cursor < m_text.size() && m_text.at(m_cursor) != QLatin1Char('\n')) {
            const QPointF after = pointFor(m_cursor + 1);
            if (!after.isNull() && qAbs(after.y() - point.y()) < 1 && after.x() > point.x())
                width = after.x() - point.x();
        }
        return QRectF(point.x(), point.y(), width, height);
    }
    return QRectF(point.x() - 1, point.y(), 2, height);
}

QPointF MarkdownEditor::pointFor(qsizetype offset, qreal *height) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks.at(size_t(index));
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

    const qreal x = line.cursorToX(int(display));
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

    for (const BlockView &view : m_blocks) {
        if (!view.cells.empty()) {
            if (y < view.y || y > view.y + view.height)
                continue;
            // Clicking a cell puts the caret on the character under the
            // pointer, and the table is edited where it is drawn.
            for (const Cell &cell : view.cells) {
                // A cell with no source - a base's, or one a short row lacks
                // - has no character to land on; the block's start does.
                if (!cell.source.isValid() || !cell.rect.translated(-view.tableScroll, view.y).contains(QPointF(x, y)))
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
        const bool last = &view == &m_blocks.back();
        if (y > view.y + view.height + kBlockSpacing && !last)
            continue;
        if (view.folded)
            return view.span.range.start;

        const qreal textTop = view.y + view.textTop;
        for (int i = 0; i < view.layout->lineCount(); ++i) {
            const QTextLine line = view.layout->lineAt(i);
            const bool lastLine = i == view.layout->lineCount() - 1;
            if (y <= textTop + line.y() + line.height() || (lastLine && last)) {
                return view.map.toSource(line.xToCursor(x));
            }
        }
        return view.span.range.end;
    }
    return m_text.size();
}

QString MarkdownEditor::linkAt(const QPoint &viewportPoint) const
{
    // Asked on every mouse move, so only the block under the pointer is
    // searched, not every span in the note.
    const qsizetype offset = offsetAt(viewportPoint);
    const qsizetype index = blockAt(offset);
    if (index < 0)
        return {};
    const BlockView &view = m_blocks[size_t(index)];
    for (int i = view.firstStyle; i < view.lastStyle; ++i) {
        const StyleSpan &span = m_decorations.styles.at(i);
        if (span.kind != StyleSpan::Kind::Link || !span.range.contains(offset))
            continue;
        // The whole target, "Note#Heading" when the link names one.
        if (span.anchorText.isValid() && span.anchorText.length() > 0)
            return span.payload + QLatin1Char('#') + m_text.mid(span.anchorText.start, span.anchorText.length());
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
    if (!target.isEmpty() && !(m_mode == Mode::Reading || control))
        target.clear();
    if (target == m_previewLink)
        return;
    m_previewLink = target;
    if (target.isEmpty())
        emit linkHoverEnded();
    else
        emit linkHovered(target, globalPos);
}

void MarkdownEditor::leaveEvent(QEvent *event)
{
    if (!m_previewLink.isEmpty()) {
        m_previewLink.clear();
        emit linkHoverEnded();
    }
    QAbstractScrollArea::leaveEvent(event);
}

void MarkdownEditor::ensureCursorVisible()
{
    const qsizetype index = blockAt(m_cursor);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return;
    const BlockView &view = m_blocks.at(size_t(index));
    const qreal lineHeight = QFontMetricsF(view.layout ? view.layout->font() : font()).height();

    const qsizetype display = view.map.toDisplay(m_cursor);
    qreal y = view.y;
    if (view.layout) {
        const QTextLine line = view.layout->lineForTextPosition(int(display));
        if (line.isValid())
            y += view.textTop + line.y();
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
    // Line breaking depends on the width, so every cached layout is stale.
    invalidateLayouts();
    updateLayout();
    positionFindBar();
}

// --- cursor ---------------------------------------------------------------

void MarkdownEditor::setCursorOffset(qsizetype offset)
{
    moveCursor(offset, false);
}

void MarkdownEditor::moveCursor(qsizetype offset, bool keepAnchor)
{
    const qsizetype clamped = qBound<qsizetype>(0, offset, m_text.size());
    const qsizetype previousCursor = m_cursor;
    const qsizetype previousBlock = m_activeBlock;
    const bool hadSelection = m_anchor != m_cursor;

    m_cursor = clamped;
    if (!keepAnchor)
        m_anchor = clamped;

    m_activeBlock = blockAt(m_cursor);
    // Out of a table it edited: that table's source is aligned, the way
    // Advanced Tables does it, so the file reads as a table too.
    // Not while a selection is being dragged out of it: that would drop it.
    if (m_tableEdited && m_alignTables && !keepAnchor && tableAround(previousCursor) != tableAround(m_cursor)) {
        m_tableEdited = false;
        formatTables(previousCursor);
    }
    // Into another cell of the same table: the cell it left draws rendered
    // again and the one it entered shows its source.
    const CellRef cell = tableCellAt(m_cursor);
    const Range caretCell = cell.row >= 0 ? cell.range : Range{-1, -1};
    if (!(caretCell == m_caretCell)) {
        for (const qsizetype block : {previousBlock, m_activeBlock}) {
            if (block < 0 || block >= qsizetype(m_blocks.size()) || m_blocks[size_t(block)].cells.empty())
                continue;
            BlockView &view = m_blocks[size_t(block)];
            view.laidOut = false;
            view.heightEstimated = false;
        }
        m_caretCell = caretCell;
        updateLayout();
        revealCaretCell();
    }
    // The two blocks swap between raw and rendered - and so does one block
    // when a selection starts or collapses inside it, since only a bare caret
    // reveals source.
    if (m_activeBlock != previousBlock || hadSelection != (m_anchor != m_cursor))
        updateLayout();

    ensureCursorVisible();
    m_cursorOn = true;
    viewport()->update();
    emit cursorMoved(m_cursor);
}

void MarkdownEditor::moveVertically(int lines, bool keepAnchor)
{
    const QPointF point = cursorPoint();
    const qreal x = m_goalX >= 0 ? m_goalX : point.x();
    const qreal lineHeight = QFontMetricsF(font()).height() * 1.5;
    const QPoint target(int(x), int(point.y() + lines * lineHeight + lineHeight / 2));

    const qreal goal = x;
    const qsizetype landed = offsetAt(target);
    moveCursor(keepAnchor ? landed : snapToTableCell(landed, m_cursor), keepAnchor);
    m_goalX = goal;
}

Range MarkdownEditor::selection() const
{
    if (m_vim.overrideRange.isValid() && m_cursor == m_vim.overrideCursor && m_anchor == m_vim.overrideAnchor)
        return m_vim.overrideRange;
    return {qMin(m_cursor, m_anchor), qMax(m_cursor, m_anchor)};
}

// --- editing --------------------------------------------------------------

void MarkdownEditor::pushUndo()
{
    if (m_vim.hold) // a vim insert session: one step, taken when it began
        return;
    // Typing bursts collapse into one undo step, as in every other editor.
    // A command that invalidated the timer is saying it is not part of one.
    if (!m_undo.isEmpty() && m_lastEdit.isValid() && m_lastEdit.elapsed() < kTypingBurstMs) {
        m_lastEdit.restart();
        return;
    }
    m_undo.append({m_text, m_cursor});
    if (m_undo.size() > 500)
        m_undo.removeFirst();
    m_redo.clear();
    m_lastEdit.restart();
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
    for (BlockView &view : m_blocks) {
        if (view.span.range.start >= at)
            view.map.shift(delta);
        shift(view.span.range);
    }
    for (Range &range : m_decorations.syntax)
        shift(range);
    for (StyleSpan &span : m_decorations.styles)
        shift(span.range);
    // m_decorations.blocks is not shifted: only rebuildBlocks reads it, and
    // only straight after a parse has replaced it.
    for (Property &property : m_properties)
        shift(property.range);
    for (ListItemSpan &item : m_decorations.listItems) {
        shift(item.range);
        shift(item.marker);
        shift(item.checkbox);
    }
    for (CalloutSpan &callout : m_decorations.callouts) {
        shift(callout.range);
        shift(callout.marker);
        shift(callout.titleRange);
    }
    for (TableSpan &table : m_decorations.tables) {
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
    for (Range &paragraph : m_decorations.paragraphs)
        shift(paragraph);
    if (!m_decorations.substitutions.isEmpty()) {
        // One the edit removed is gone; text typed just after one is not
        // swallowed by it, so its end moves only when past the edit.
        if (delta < 0) {
            m_decorations.substitutions.removeIf([at, delta](const Substitution &s) {
                return s.range.overlaps({at + delta, at});
            });
        }
        for (Substitution &substitution : m_decorations.substitutions) {
            if (substitution.range.start >= at)
                substitution.range.start += delta;
            if (substitution.range.end > at)
                substitution.range.end += delta;
        }
    }
    for (HtmlTextSpan &span : m_decorations.HTMLtext) {
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
    for (EmbedSpan &embed : m_decorations.embeds)
        shift(embed.range);
    for (MathSpan &math : m_decorations.maths) {
        shift(math.range);
        shift(math.formula);
    }
    for (CodeOptions &options : m_decorations.codeOptions) {
        shift(options.block);
        shift(options.range);
    }

    for (CodeFenceSpan &fence : m_decorations.codeFences) {
        shift(fence.block);
        shift(fence.content);
        shift(fence.attributesRange);
    }
    for (TabGroup &group : m_tabGroups) {
        shift(group.span);
        if (group.hiddenEnd >= at)
            group.hiddenEnd += delta;
    }
    if (delta != 0 && !m_selectedTabs.isEmpty()) {
        QHash<qsizetype, int> moved;
        for (auto it = m_selectedTabs.cbegin(); it != m_selectedTabs.cend(); ++it)
            moved.insert(it.key() >= at ? it.key() + delta : it.key(), it.value());
        m_selectedTabs = std::move(moved);
    }

    // Fold state is keyed by the block's start, so it has to move with it.
    if (delta != 0 && !m_folds.isEmpty()) {
        QHash<qsizetype, bool> moved;
        moved.reserve(m_folds.size());
        for (auto it = m_folds.cbegin(); it != m_folds.cend(); ++it)
            moved.insert(it.key() >= at ? it.key() + delta : it.key(), it.value());
        m_folds = std::move(moved);
    }
}

void MarkdownEditor::applyEdit(qsizetype at, qsizetype removed, const QString &inserted)
{
    m_text.remove(at, removed);
    m_text.insert(at, inserted);
    if (m_vim.enabled && !m_vim.marks.isEmpty()) { // vim's marks follow their text
        for (qsizetype &mark : m_vim.marks) {
            if (mark >= at + removed)
                mark += inserted.size() - removed;
            else if (mark > at)
                mark = at;
        }
    }

    // The caret's block is shown raw, so a stale decoration inside it changes
    // nothing on screen; every other block is untouched by this edit. Shifting
    // the offsets is therefore enough until the debounced parse lands.
    ++m_textVersion;
    m_lines.applyEdit(at, removed, inserted); // the gutter needs current line starts
    const qsizetype delta = inserted.size() - removed;
    // shiftRanges already moves the end of the block the edit is in; moving it
    // here as well once made that block run on into the next one, drawn twice
    // for the 20 ms until the reparse. An edit that keeps the length still
    // changes the text, so the block relays out either way.
    shiftRanges(at + removed, delta);
    const qsizetype index = blockAt(at);
    if (index >= 0 && index < qsizetype(m_blocks.size())) {
        BlockView &view = m_blocks[size_t(index)];
        view.layout.reset();
        view.laidOut = false;
        view.heightEstimated = false;
        view.codeEndValid = false;
    }

    if (!m_applyingExternal) {
        m_cursor = m_anchor = at + inserted.size();
        m_activeBlock = blockAt(m_cursor);
        if (tableAround(m_cursor) >= 0 && !m_formattingTables)
            m_tableEdited = true;
    }

    setModified(true);
    updateLayout();
    if (!m_applyingExternal) {
        ensureCursorVisible();
        emit edited(at, removed, inserted);
    }
    m_parseTimer.start(); // the real parse catches up when typing pauses
    // Only while the find bar is actually searching: a keystroke with no
    // query must not pay for a document scan.
    if (!m_find.pattern.isEmpty())
        updateFindMatches();
    emit textChanged();
    viewport()->update();
}

void MarkdownEditor::insertText(const QString &text)
{
    if (m_mode == Mode::Reading || text.isEmpty())
        return;
    pushUndo();

    const Range selected = selection();
    applyEdit(selected.start, selected.length(), text);
}

void MarkdownEditor::removeSelectionOrChar(int direction)
{
    if (m_mode == Mode::Reading)
        return;

    const Range selected = selection();
    Range toRemove = selected;
    if (selected.length() == 0) {
        if (direction < 0 && m_cursor > 0)
            toRemove = {m_cursor - 1, m_cursor};
        else if (direction > 0 && m_cursor < m_text.size())
            toRemove = {m_cursor, m_cursor + 1};
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
    if (m_undo.isEmpty())
        return;
    m_redo.append({m_text, m_cursor});
    const Snapshot snapshot = m_undo.takeLast();
    const qsizetype before = m_text.size();
    m_text = snapshot.text;
    ++m_textVersion;
    m_cursor = m_anchor = qBound<qsizetype>(0, snapshot.cursor, m_text.size());
    setModified(true);
    reparse();
    emit textChanged();
    emit edited(0, before, m_text);
    viewport()->update();
}

void MarkdownEditor::redo()
{
    if (m_redo.isEmpty())
        return;
    m_undo.append({m_text, m_cursor});
    const Snapshot snapshot = m_redo.takeLast();
    const qsizetype before = m_text.size();
    m_text = snapshot.text;
    ++m_textVersion;
    m_cursor = m_anchor = qBound<qsizetype>(0, snapshot.cursor, m_text.size());
    setModified(true);
    reparse();
    emit textChanged();
    emit edited(0, before, m_text);
    viewport()->update();
}

void MarkdownEditor::applyExternalEdit(qsizetype at, qsizetype removed, const QString &inserted)
{
    at = qBound<qsizetype>(0, at, m_text.size());
    removed = qBound<qsizetype>(0, removed, m_text.size() - at);
    m_undo.clear();
    m_redo.clear();
    const auto follow = [&](qsizetype offset) {
        if (offset >= at + removed)
            return offset + inserted.size() - removed;
        return offset > at ? at + inserted.size() : offset;
    };
    const qsizetype cursor = follow(m_cursor);
    const qsizetype anchor = follow(m_anchor);
    if (at == 0 && removed == m_text.size()) {
        // An undo or redo over there: the whole text, as undo() takes it.
        m_text = inserted;
        ++m_textVersion;
        m_cursor = m_anchor = qMin(m_cursor, m_text.size());
        setModified(true);
        reparse();
        emit textChanged();
    } else {
        m_applyingExternal = true;
        applyEdit(at, removed, inserted);
        m_applyingExternal = false;
    }
    m_cursor = qMin(cursor, m_text.size());
    m_anchor = qMin(anchor, m_text.size());
    m_activeBlock = blockAt(m_cursor);
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
        {QStringLiteral("editor:insert-link"), tr("Insert Markdown link"), {QKeySequence(Qt::CTRL | Qt::Key_K)}},
        {QStringLiteral("editor:toggle-checklist-status"), tr("Toggle checkbox status"),
         {QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)}},
        {QStringLiteral("editor:open-search"), tr("Search current file"), {QKeySequence(Qt::CTRL | Qt::Key_F)}},
        {QStringLiteral("editor:open-search-replace"), tr("Search and replace in current file"),
         {QKeySequence(Qt::CTRL | Qt::Key_H)}},
        {QStringLiteral("editor:swap-line-up"), tr("Move line up"), {QKeySequence(Qt::ALT | Qt::Key_Up)}},
        {QStringLiteral("editor:swap-line-down"), tr("Move line down"), {QKeySequence(Qt::ALT | Qt::Key_Down)}},
        {QStringLiteral("noteAppC:duplicate-lines"), tr("Duplicate line"),
         {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)}},
        {QStringLiteral("noteAppC:insert-table"), tr("Insert table"), {}},
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
    if (m_mode == Mode::Reading && id != QLatin1String("editor:open-search"))
        return false; // nothing to edit; searching still works
    if (id == QLatin1String("editor:toggle-bold"))
        toggleBold();
    else if (id == QLatin1String("editor:toggle-italics"))
        toggleItalic();
    else if (id == QLatin1String("editor:toggle-highlight"))
        toggleHighlight();
    else if (id == QLatin1String("editor:insert-link"))
        insertLink();
    else if (id == QLatin1String("editor:toggle-checklist-status"))
        toggleTaskAtCursor();
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
    else if (id == QLatin1String("noteAppC:format-table"))
        formatTables(m_cursor);
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
    m_keymap.clear();
    for (const Command &command : commands()) {
        const auto own = bindings.constFind(command.id);
        for (const QKeySequence &keys : own != bindings.constEnd() ? *own : command.keys) {
            if (!keys.isEmpty())
                m_keymap.insert(keys[0].toCombined(), command.id);
        }
    }
}

void MarkdownEditor::keyPressEvent(QKeyEvent *event)
{
    // Ctrl pressed with the pointer already resting on a link previews it,
    // as in Obsidian, without the mouse having to move.
    if (event->key() == Qt::Key_Control && viewport()->underMouse()) {
        const QPoint global = QCursor::pos();
        updateLinkHover(viewport()->mapFromGlobal(global), true, global);
    }
    // Ctrl+Space and Shift+Tab in a code block with a running kernel, and the
    // popups they open (NotebookComplete.cpp).
    if (m_notebookIntrospector && handleNotebookIntrospectKey(event))
        return;
    // The slash popup's keys are handled here rather than left to QCompleter's
    // own filter on the popup, which only sees them while the popup holds the
    // keyboard - not, for one, when a platform does not give a popup focus.
    if (m_slashCompleter && m_slashCompleter->popup()->isVisible()) {
        QAbstractItemView *popup = m_slashCompleter->popup();
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
    if (m_completer->popup()->isVisible()) {
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
    if (!m_interacted) {
        m_interacted = true;
        updateLayout();
    }
    // Vim mode (VimMode.cpp). Off, this is the only cost; in insert mode it
    // claims Escape and nothing else.
    if (m_vim.enabled && m_mode != Mode::Reading && handleVimKey(event))
        return;

    // Rebindable commands first, then the keys every text field has.
    const QKeyCombination pressed(event->modifiers() & ~Qt::KeypadModifier,
                                  Qt::Key(event->key()));
    const auto bound = m_keymap.constFind(pressed.toCombined());
    if (bound != m_keymap.constEnd() && runCommand(*bound))
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
            m_anchor = 0;
            moveCursor(m_text.size(), true);
            return;
        case Qt::Key_C:
            if (selection().length() > 0) {
                QApplication::clipboard()->setText(
                    m_text.mid(selection().start, selection().length()));
            }
            return;
        case Qt::Key_X:
            if (selection().length() > 0) {
                QApplication::clipboard()->setText(
                    m_text.mid(selection().start, selection().length()));
                removeSelectionOrChar(0);
            }
            return;
        case Qt::Key_V:
            // Plain text only with Shift, as Obsidian does; otherwise an
            // image on the clipboard is saved as an attachment (PasteDrop.cpp).
            if (shift)
                insertText(cellSafe(QApplication::clipboard()->text()));
            else
                insertMimeData(QApplication::clipboard()->mimeData());
            return;
        default:
            break;
        }
    }

    m_goalX = -1;
    if (handleTableKey(event))
        return;
    switch (event->key()) {
    // In a drawn table the arrows cross from one cell into the next.
    // An entity is one character on screen, so one step for the caret.
    case Qt::Key_Left: {
        const qsizetype target = stepOverSubstitution(m_cursor > 0 ? m_cursor - 1 : 0, -1);
        moveCursor(shift ? target : snapToTableCell(target, m_cursor), shift);
        return;
    }
    case Qt::Key_Right: {
        const qsizetype target = stepOverSubstitution(m_cursor + 1, 1);
        moveCursor(shift ? target : snapToTableCell(target, m_cursor), shift);
        return;
    }
    case Qt::Key_Up:
        moveVertically(-1, shift);
        return;
    case Qt::Key_Down:
        moveVertically(1, shift);
        return;
    case Qt::Key_Home:
        moveCursor(m_text.lastIndexOf(QLatin1Char('\n'), qMax<qsizetype>(0, m_cursor - 1)) + 1,
                   shift);
        return;
    case Qt::Key_End: {
        const qsizetype next = m_text.indexOf(QLatin1Char('\n'), m_cursor);
        moveCursor(next < 0 ? m_text.size() : next, shift);
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
            insertText(QString(m_tabWidth, QLatin1Char(' ')));
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
    if (m_notebookInspect && m_notebookInspect->isVisible())
        hideNotebookIntrospection();
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    // A finger is bigger than a pointer: every small target grows for it.
    const bool touch = event->pointingDevice()
                       && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen;
    m_touchSlop = touch ? 10 : 0;
    if (touch && pressSelectionHandle(event->pos()))
        return;
    m_touchHandles = false;

    // A hooked embed's own image (a `.canvas`, say) takes the press instead
    // of the caret: a plain click may open a card on release, and a drag
    // pans it - neither is a place the caret can land.
    if (const qsizetype embed = hookedEmbedBlockAt(event->pos()); embed >= 0) {
        m_embedDrag.blockIndex = embed;
        m_embedDrag.startPoint = event->pos();
        m_embedDrag.lastPoint = event->pos();
        m_embedDrag.moved = false;
        return;
    }

    const QString target = linkAt(event->pos());
    if (!target.isEmpty() && event->modifiers().testFlag(Qt::ControlModifier)) {
        emit linkActivated(target);
        return;
    }
    // A footnote never leaves the note, so it jumps straight to an offset
    // rather than going through linkActivated the way a real link does.
    if (event->modifiers().testFlag(Qt::ControlModifier) && jumpToFootnote(event->pos()))
        return;

    // A click on a cell a short row lacks gives the row that cell first.
    fillShortRowAt(event->pos());
    if (handleBlockClick(event->pos()))
        return;

    for (const BlockView &view : m_blocks) {
        if (view.listTextX.empty() || !view.laidOut)
            continue;
        for (const ListItemSpan &item : listItemsIn(view.span.range)) {
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

    m_goalX = -1;
    m_interacted = true;
    // Measured against the layout the reader is looking at now, before this
    // click can change what is revealed. A double-click selects the word
    // found here, not under a layout the first click has since shifted.
    m_pressOffset = offsetAt(event->pos());
    if (!event->modifiers().testFlag(Qt::ShiftModifier))
        m_pressOffset = snapToTableCell(m_pressOffset, m_pressOffset);
    m_pressing = true;
    if (event->pointingDevice() && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen) {
        m_pressPoint = event->pos();
        m_longPress.start();
    }
    moveCursor(m_pressOffset, event->modifiers().testFlag(Qt::ShiftModifier));
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::watchStyleFiles()
{
    m_configWatcher = new QFileSystemWatcher(this);
    // The files, and the directory they live in. An editor that saves by
    // writing a new file and renaming it over the old one - most do - makes
    // inotify drop the watch on the file, and a file that does not exist yet
    // cannot be watched at all; the directory sees both happen.
    const QStringList files = {CalloutStyles::defaultPath(), LanguageStyles::defaultPath()};
    const QString directory = QFileInfo(files.first()).absolutePath();
    if (QFileInfo(directory).isDir())
        m_configWatcher->addPath(directory);
    for (const QString &file : files) {
        if (QFileInfo::exists(file))
            m_configWatcher->addPath(file);
    }
    connect(m_configWatcher, &QFileSystemWatcher::fileChanged, this,
            &MarkdownEditor::reloadStyleFiles);
    connect(m_configWatcher, &QFileSystemWatcher::directoryChanged, this,
            &MarkdownEditor::reloadStyleFiles);
}

void MarkdownEditor::reloadStyleFiles()
{
    // Put back any watch a rename-over-save took away.
    for (const QString &file : {CalloutStyles::defaultPath(), LanguageStyles::defaultPath()}) {
        if (QFileInfo::exists(file) && !m_configWatcher->files().contains(file))
            m_configWatcher->addPath(file);
    }
    m_calloutStyles = CalloutStyles::load();
    m_languageStyles = LanguageStyles::load();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

QString MarkdownEditor::selectedText() const
{
    const Range range = selection();
    return m_text.mid(range.start, range.length());
}

void MarkdownEditor::mouseReleaseEvent(QMouseEvent *event)
{
    m_longPress.stop();
    if (m_embedDrag.blockIndex >= 0) {
        const qsizetype block = m_embedDrag.blockIndex;
        const bool wasClick = !m_embedDrag.moved;
        const QPoint pos = event->pos();
        m_embedDrag = EmbedDrag{};
        if (wasClick && event->button() == Qt::LeftButton) {
            applyFileEmbedInput(block, {FileEmbedInputKind::Click, embedLocalPoint(block, pos),
                                        {}, event->modifiers()});
        }
        return;
    }
    if (releaseSelectionHandle())
        return;
    if (event->button() != Qt::LeftButton || !m_pressing) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    // Only now does the caret's block decide whether to show its source: a
    // plain click reveals it, a drag that selected something does not.
    m_pressing = false;
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_embedDrag.blockIndex >= 0) {
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            m_embedDrag = EmbedDrag{}; // the release was missed - it left the window, say
            return;
        }
        if (!m_embedDrag.moved
            && (event->pos() - m_embedDrag.startPoint).manhattanLength() >= 4)
            m_embedDrag.moved = true;
        const QPoint delta = event->pos() - m_embedDrag.lastPoint;
        if (m_embedDrag.moved && !delta.isNull()) {
            applyFileEmbedInput(m_embedDrag.blockIndex,
                                {FileEmbedInputKind::Drag, embedLocalPoint(m_embedDrag.blockIndex, event->pos()),
                                 delta, event->modifiers()});
            m_embedDrag.lastPoint = event->pos();
        }
        return;
    }
    if (dragSelectionHandle(event->pos()))
        return;
    if (event->buttons().testFlag(Qt::LeftButton)) {
        if (m_longPress.isActive()) {
            // A finger always wobbles a little; only a real move is a drag.
            if ((event->pos() - m_pressPoint).manhattanLength() < 12)
                return;
            m_longPress.stop();
        }
        moveCursor(offsetAt(event->pos()), true);
        return;
    }
    // Which code block the pointer is over, since that is when one without a
    // language shows its "Copy" label. Only a change repaints.
    qsizetype hovered = -1;
    const qreal y = event->position().y() + verticalScrollBar()->value();
    for (const BlockView &view : m_blocks) {
        if (view.y - 8 > y)
            break;
        if (view.span.kind == BlockSpan::Kind::Code && y <= view.y + view.height + 8) {
            hovered = firstPiece(view).span.range.start;
            break;
        }
    }
    if (hovered != m_hoveredCodeBlock) {
        m_hoveredCodeBlock = hovered;
        viewport()->update();
    }

    const bool clickable = !linkAt(event->pos()).isEmpty() || codeLabelAt(event->pos());
    viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::IBeamCursor);
    updateLinkHover(event->pos(), event->modifiers().testFlag(Qt::ControlModifier),
                    event->globalPosition().toPoint());
}

void MarkdownEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
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
    selectWordAt(m_pressOffset);
}

void MarkdownEditor::selectWordAt(qsizetype offset)
{
    qsizetype start = offset;
    qsizetype end = offset;
    while (start > 0 && m_text.at(start - 1).isLetterOrNumber())
        --start;
    while (end < m_text.size() && m_text.at(end).isLetterOrNumber())
        ++end;
    m_anchor = start;
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

    BlockView *table = scrollableTableAt(pos);
    if (table) {
        // A horizontal wheel, or shift with a vertical one, pans the table -
        // the gesture Obsidian answers to.
        const QPoint pixels = event->pixelDelta();
        const QPoint degrees = event->angleDelta();
        qreal dx = pixels.x() != 0 ? pixels.x() : degrees.x() / 2.0;
        if (qFuzzyIsNull(dx) && event->modifiers().testFlag(Qt::ShiftModifier))
            dx = pixels.y() != 0 ? pixels.y() : degrees.y() / 2.0;

        if (!qFuzzyIsNull(dx)) {
            const qreal overflow = table->tableWidth - contentWidth();
            const qreal wanted = qBound<qreal>(0, table->tableScroll - dx, overflow);
            if (!qFuzzyCompare(wanted + 1, table->tableScroll + 1)) {
                table->tableScroll = wanted;
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
    m_cursorOn = true;
    m_blink.start();
    viewport()->update();
}

bool MarkdownEditor::focusNextPrevChild(bool)
{
    return false;
}

void MarkdownEditor::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);
    if (m_notebookInspect && m_notebookInspect->isVisible())
        hideNotebookIntrospection();
    m_blink.stop();
    m_cursorOn = false;
    viewport()->update();
}

void MarkdownEditor::inputMethodEvent(QInputMethodEvent *event)
{
    // Vim's normal and visual modes take keys as commands: composed text
    // would be typed into the note without an insert.
    if (m_vim.enabled && m_vim.mode != VimState::Mode::Insert) {
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
        return m_text;
    case Qt::ImCursorPosition:
        return int(m_cursor);
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}

} // namespace noteAppC
