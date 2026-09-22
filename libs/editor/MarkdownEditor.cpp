#include "MarkdownEditor.h"

#include "Icons.h"
#include "PdfPage.h"
#include "Vault.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QGuiApplication>
#include <QScrollBar>
#ifdef NOTEAPPC_HAVE_OPENGLWIDGETS
#include <QOpenGLWidget>
#endif
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QStringListModel>
#include <QtConcurrent>
#include <QTextLine>
#include <algorithm>
#include <cstdio>

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
    horizontalScrollBar()->setRange(0, 0);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        updateLayout(); // blocks scrolling into view need their real layout
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
        adoptDecorations(std::move(job.decorations), std::move(job.properties));
        m_parsedVersion = job.version;
        // The buffer moved on while we were parsing: go round again.
        if (m_parsedVersion != m_textVersion)
            startParse();
    });

    m_blink.setInterval(QApplication::cursorFlashTime() / 2);
    connect(&m_blink, &QTimer::timeout, this, [this] {
        m_cursorOn = !m_cursorOn;
        viewport()->update();
    });

    m_completer = new QCompleter(new QStringListModel(this), this);
    m_completer->setWidget(this);
    m_completer->popup()->setObjectName(QStringLiteral("completerPopup"));
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
    m_mode = mode;
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::setNoteText(const QString &text)
{
    m_text = text;
    ++m_textVersion;
    m_cursor = m_anchor = 0;
    m_activeBlock = -1;
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
        adoptDecorations({}, {});
        startParse();
    } else {
        reparse();
    }
    if (!m_find.pattern.isEmpty())
        updateFindMatches();
    setModified(false);
    verticalScrollBar()->setValue(0);
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
        parts << DisplayMap::build(m_text, view.span.range, hidden).text();
    }
    return parts.join(QString());
}

void MarkdownEditor::enterBlockAt(qsizetype offset)
{
    m_interacted = true;
    moveCursor(offset, false);
    updateLayout();
    viewport()->update();
}

int MarkdownEditor::wordCount() const
{
    int words = 0;
    bool inWord = false;
    for (const QChar c : m_text) {
        const bool part = c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('\'');
        if (part && !inWord)
            ++words;
        inWord = part;
    }
    return words;
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
    std::sort(decorations.syntax.begin(), decorations.syntax.end(),
              [](const Range &l, const Range &r) { return l.start < r.start; });
    std::sort(decorations.styles.begin(), decorations.styles.end(),
              [](const StyleSpan &l, const StyleSpan &r) { return l.range.start < r.range.start; });
}

} // namespace

void MarkdownEditor::reparse()
{
    m_parseTimer.stop();
    Parser::Result result = m_parser.parseAll(m_text);
    sortDecorations(result.decorations);
    m_parsedVersion = m_textVersion;
    adoptDecorations(std::move(result.decorations), std::move(result.metadata.properties));
}

void MarkdownEditor::startParse()
{
    if (m_parseWatcher.isRunning())
        return; // the finished handler starts the next one

    const QString snapshot = m_text;
    const quint64 version = m_textVersion;
    m_parseWatcher.setFuture(QtConcurrent::run([snapshot, version] {
        Parser::Result result = threadParser().parseAll(snapshot);
        ParseJob job;
        job.decorations = std::move(result.decorations);
        job.properties = std::move(result.metadata.properties);
        sortDecorations(job.decorations);
        job.version = version;
        return job;
    }));
}

void MarkdownEditor::adoptDecorations(Decorations decorations, QList<Property> properties)
{
    m_decorations = std::move(decorations);
    m_properties = std::move(properties);
    m_lines = SourceMap(m_text);
    rebuildBlocks();
    indexDecorations();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
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

    std::vector<BlockView> blocks;
    qsizetype cursor = 0;
    const auto addGap = [&](qsizetype from, qsizetype to) {
        if (to <= from)
            return;
        BlockView view;
        view.span = {BlockSpan::Kind::Paragraph, {from, to}, 0};
        blocks.push_back(std::move(view));
    };

    for (const BlockSpan &span : std::as_const(spans)) {
        if (!span.range.isValid() || span.range.start < cursor)
            continue; // nested or overlapping: the outer block owns it
        addGap(cursor, span.range.start);

        BlockView view;
        view.span = span;
        // A block owns the newline that ends it, so the blank lines left over
        // in the gap are exactly the blank lines the file has.
        if (view.span.range.end < m_text.size()
            && m_text.at(view.span.range.end) == QLatin1Char('\n'))
            ++view.span.range.end;
        blocks.push_back(std::move(view));
        cursor = view.span.range.end;
    }
    addGap(cursor, m_text.size());

    if (blocks.empty()) {
        BlockView view;
        view.span = {BlockSpan::Kind::Paragraph, {0, m_text.size()}, 0};
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

    // The subpath is not part of what resolves to a file.
    QString name = target;
    const qsizetype hash = name.indexOf(QLatin1Char('#'));
    if (hash >= 0)
        name.truncate(hash);
    const bool missing = !name.isEmpty() && m_resolver(name).isEmpty();
    m_unresolved.insert(target, missing);
    return missing;
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
    if (span.kind == BlockSpan::Kind::Code || span.kind == BlockSpan::Kind::Frontmatter) {
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

QList<QTextLayout::FormatRange> MarkdownEditor::formatsFor(const BlockView &view) const
{
    QList<QTextLayout::FormatRange> formats = formatsIn(view, view.span.range, view.map);
    formats += codeFormatsFor(view);
    return formats;
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

    for (const ListItemSpan &item : m_decorations.listItems) {
        if (!item.done || !item.range.overlaps(block))
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
        case StyleSpan::Kind::Highlight:
            format.setBackground(highlight);
            format.setForeground(m_theme.color(QStringLiteral("--color-base-00")));
            break;
        case StyleSpan::Kind::Math:
        case StyleSpan::Kind::Html:
            format.setForeground(m_theme.color(QStringLiteral("--color-purple")));
            break;
        case StyleSpan::Kind::Heading:
        case StyleSpan::Kind::Quote:
            continue; // carried by the block font
        }
        add(span.range, format);
    }
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
        for (int i = view.firstStyle; i < view.lastStyle; ++i) {
            const StyleSpan &span = m_decorations.styles.at(i);
            if (span.kind == StyleSpan::Kind::CodeBlock && span.range.overlaps(view.span.range)) {
                *language = span.payload;
                break;
            }
        }
    }
    return content;
}

QList<QTextLayout::FormatRange> MarkdownEditor::codeFormatsFor(const BlockView &view) const
{
    if (view.span.kind != BlockSpan::Kind::Code)
        return nestedCodeFormatsFor(view);

    QString language;
    const Range content = codeContentOf(view, &language);
    if (!content.isValid() || content.length() <= 0 || language.isEmpty())
        return {};

    const QString code = m_text.mid(content.start, content.length());
    QList<QTextLayout::FormatRange> formats = m_code.highlight(code, language);

    // Highlighter offsets are relative to the code; the layout wants display
    // offsets. Nothing inside a fence is ever hidden, so the shift is uniform.
    const qsizetype shift = view.map.toDisplay(content.start);
    for (QTextLayout::FormatRange &range : formats)
        range.start += int(shift);
    return formats;
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

        const Range range = view.map.toDisplay(span.range);
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

const ListItemSpan *MarkdownEditor::listItemAt(qsizetype source) const
{
    for (const ListItemSpan &item : m_decorations.listItems) {
        // The marker is hidden, so a line starts exactly where it ends.
        if (item.marker.end == source || item.range.start == source)
            return &item;
    }
    return nullptr;
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
    const qreal x = contentLeft() + kListIndent * (1 + item.indent) - box - 8;
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
    if (!view.layout || view.folded)
        return;

    QColor guide = m_theme.color(QStringLiteral("--text-faint"));
    guide.setAlphaF(0.4f);

    painter.save();
    painter.setPen(QPen(guide, 1));
    for (const ListItemSpan &item : m_decorations.listItems) {
        if (item.indent <= 0 || !item.range.overlaps(view.span.range))
            continue;

        const QRectF band = bandOf(view, item.range);
        if (band.height() <= 0)
            continue;
        // One rule per level the item sits under, never under its own marker.
        for (int level = 1; level <= item.indent; ++level) {
            const qreal x = left + kListIndent * level - 8;
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
    if (!view.layout || view.span.kind != BlockSpan::Kind::List || m_mode == Mode::Source
        || view.active)
        return;

    const QColor muted = m_theme.color(QStringLiteral("--text-muted"));

    for (const ListItemSpan &item : m_decorations.listItems) {
        if (!item.range.overlaps(view.span.range))
            continue;

        const qsizetype display = view.map.toDisplay(item.marker.end);
        const QTextLine line = view.layout->lineForTextPosition(int(display));
        if (!line.isValid())
            continue;

        const qreal indent = kListIndent * (1 + item.indent);
        const qreal baseline = top + line.y();

        if (item.task) {
            const qreal box = QFontMetricsF(view.layout->font()).height() * 0.8;
            paintCheckBox(painter, QRectF(left + indent - box - 8,
                                          baseline + (line.height() - box) / 2, box, box),
                          item.done);
            continue;
        }

        painter.save();
        painter.setPen(muted);
        if (item.ordered) {
            QFont font = view.layout->font();
            painter.setFont(font);
            painter.drawText(QRectF(left, baseline, indent - 8, line.height()),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1.").arg(item.number));
        } else {
            const qreal dot = 4;
            painter.setBrush(muted);
            painter.drawEllipse(QPointF(left + indent - 12,
                                        baseline + line.height() / 2), dot / 2, dot / 2);
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
    for (const CalloutSpan &callout : m_decorations.callouts) {
        if (callout.range.start == view.span.range.start)
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
    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    QPainterPath rounded;
    rounded.addRoundedRect(panel, 6, 6);
    painter.fillPath(rounded, style.background);
    painter.fillRect(QRectF(panel.left(), panel.top(), 3, panel.height()), style.bar);

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

bool MarkdownEditor::layoutEmbed(BlockView &view)
{
    view.image = QPixmap();
    view.imagePath.clear();
    view.embedLabel.clear();
    view.embedPanel = false;

    const EmbedSpan *embed = soleEmbedOf(view);
    if (!embed || !m_resolver)
        return false;

    const QString path = m_resolver(embed->target);
    if (path.isEmpty())
        return false;

    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QByteArrayList images = QImageReader::supportedImageFormats();

    if (images.contains(suffix.toUtf8()))
        return layoutImage(view, *embed, path);
    if (suffix == QLatin1String("pdf"))
        return layoutPdf(view, *embed, path);
    // A note embed is rendered by the transclusion path, not here.
    if (Vault::isNote(path))
        return false;
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
    view.laidOut = true;
    return true;
}

bool MarkdownEditor::layoutPdf(BlockView &view, const EmbedSpan &embed, const QString &path)
{
    // Obsidian addresses a page with `#page=3`; page one otherwise.
    int page = 0;
    static const QRegularExpression pageRef(QStringLiteral("#page=(\\d+)"),
                                            QRegularExpression::CaseInsensitiveOption);
    const auto match = pageRef.match(embed.subpath);
    if (match.hasMatch())
        page = qMax(0, match.captured(1).toInt() - 1);

    const int width = qMin(embed.width > 0 ? embed.width : contentWidth(), contentWidth());
    const QString key = QStringLiteral("%1|%2|%3").arg(path).arg(page).arg(width);

    QPixmap pixmap = m_images.value(key);
    if (pixmap.isNull()) {
        const QImage rendered = PdfPage::render(path, page, width);
        if (rendered.isNull())
            return false;
        pixmap = QPixmap::fromImage(rendered);
        m_images.insert(key, pixmap);
    }

    const int height = embed.height > 0
                           ? embed.height
                           : int(qreal(width) * pixmap.height() / qMax(1, pixmap.width()));

    view.image = pixmap;
    view.imageSize = QSize(width, height);
    view.imagePath = path;
    view.embedLabel = PdfPage::pageCount(path) > 1
                          ? tr("%1  -  page %2").arg(QFileInfo(path).fileName()).arg(page + 1)
                          : QFileInfo(path).fileName();
    view.embedPanel = true;
    view.layout.reset();
    view.cells.clear();
    view.textTop = codeHeaderHeight();
    view.height = height + view.textTop;
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
        // be dropped again if the formula turns out not to render.
        const Range hideRange{math.formula.start, math.range.end};
        auto it = std::find(hidden.begin(), hidden.end(), hideRange);
        if (it == hidden.end())
            continue;

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

            QList<Range> hidden;
            for (int i = view.firstSyntax; i < view.lastSyntax; ++i) {
                const Range &marker = m_decorations.syntax.at(i);
                if (marker.overlaps(cell.source))
                    hidden.append(marker);
            }
            cell.map = DisplayMap::build(m_text, cell.source, hidden);

            QString shown = cell.map.text().trimmed();
            cell.layout = std::make_unique<QTextLayout>(shown);
            cell.layout->setCacheEnabled(true);
            QTextOption wrap;
            wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            cell.layout->setTextOption(wrap);
            QFont cellFont = font;
            if (r == 0)
                cellFont.setWeight(QFont::DemiBold);
            cell.layout->setFont(cellFont);
            cell.layout->setFormats(formatsIn(view, cell.source, cell.map));

            natural[c] = qMax(natural.at(c),
                              QFontMetricsF(cellFont).horizontalAdvance(shown) + 2 * padX);
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
    view.headerRows = 1;
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
    const qreal padX = 10;
    const qreal padY = 6;

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
        painter.setPen(QPen(border, 1));
        painter.drawRect(rect);

        const qreal width = rect.width() - 2 * padX;
        qreal offset = 0;
        if (cell.layout->lineCount() > 0) {
            const qreal lineWidth = cell.layout->lineAt(0).naturalTextWidth();
            if (cell.alignment.testFlag(Qt::AlignRight))
                offset = width - lineWidth;
            else if (cell.alignment.testFlag(Qt::AlignHCenter))
                offset = (width - lineWidth) / 2;
        }

        painter.setPen(text);
        // Each cell clips its own text, so a long word cannot bleed into the
        // next column.
        painter.save();
        painter.setClipRect(rect.adjusted(1, 1, -1, -1), Qt::IntersectClip);
        cell.layout->draw(&painter, QPointF(rect.left() + padX + qMax<qreal>(0, offset),
                                            rect.top() + padY));
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
    view.cells.clear();
    view.callout = nullptr;
    view.options = nullptr;
    view.propertyRows.clear();
    view.image = QPixmap();
    view.imageCentred = false;
    view.textTop = 0;
    view.folded = false;
    view.inlineMaths.clear();

    // Obsidian never shows raw YAML in live preview, only in source mode, so
    // the properties table does not step aside for the caret the way a
    // paragraph does.
    if (view.span.kind == BlockSpan::Kind::Frontmatter && m_mode != Mode::Source
        && layoutProperties(view))
        return;

    const bool renderMedia = m_mode != Mode::Source && !view.active;
    if (renderMedia && layoutEmbed(view))
        return;
    if (renderMedia && layoutMath(view))
        return;

    const bool renderTable = view.span.kind == BlockSpan::Kind::Table
                             && m_mode != Mode::Source && !view.active;
    if (renderTable && layoutTable(view))
        return;

    if (renderMedia && view.span.kind == BlockSpan::Kind::Code)
        view.options = codeOptionsFor(view);

    const bool reveal = m_mode == Mode::Source
                        || (m_mode == Mode::LivePreview && view.active);
    QList<Range> hidden;
    if (!reveal) {
        hidden.reserve(view.lastSyntax - view.firstSyntax);
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i)
            hidden.append(m_decorations.syntax.at(i));
        // The parser always adds an inline formula's hide range; whether it
        // actually stays hidden depends on MicroTeX being able to draw it,
        // which only the editor knows.
        layoutInlineMaths(view, hidden);
    }
    view.map = reveal ? DisplayMap::identity(m_text, view.span.range)
                      : DisplayMap::build(m_text, view.span.range, hidden);
    for (BlockView::InlineMath &formula : view.inlineMaths)
        formula.displayPos = view.map.toDisplay(formula.sourcePos);

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
            if (callout.depth > 0 && view.span.range.contains(callout.range.start))
                view.nested.push_back(
                    {&callout,
                     m_calloutStyles.resolve(callout.type, callout.attributes, m_theme)});
        }
    }

    view.layout = std::make_unique<QTextLayout>(shown);
    view.layout->setCacheEnabled(true);
    // A callout may name its own body font, size and effects; everything else
    // takes the font its kind implies.
    view.layout->setFont(view.callout ? view.calloutStyle.bodyFont(m_theme) : fontFor(view.span));

    QTextOption option;
    // `#| wrap: false` is Quarto asking for a listing that does not reflow;
    // everything else wraps inside the column.
    option.setWrapMode(view.options && !view.options->wrap
                           ? QTextOption::NoWrap
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
    view.layout->setFormats(formats);

    const qreal listIndent = view.span.kind == BlockSpan::Kind::List ? kListIndent : 0;
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
        const ListItemSpan *item = listIndent > 0 ? listItemAt(source) : nullptr;
        const qreal nesting = kCalloutIndent * nestingDepthAt(view, source);
        const qreal indent = nesting
                             + (listIndent > 0 ? listIndent * (1 + (item ? item->indent : 0))
                                               : codeGutter);
        line.setLineWidth(contentWidth() - indent
                          - (view.span.kind == BlockSpan::Kind::Callout ? 28 : 0));
        line.setPosition(QPointF(indent, height));
        height += lineHeight;
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
        // below it; an empty callout still shows its title.
        view.textTop = calloutHeaderHeight(view.calloutStyle);
        view.height = height;
    } else if (view.options) {
        view.textTop = codeHeaderHeight();
    }

    view.folded = isFoldable(view) && m_folds.value(view.span.range.start, defaultFoldOf(view));
    view.height = view.folded ? 0 : view.height + codeCaptionHeight(view);
    view.height += view.textTop;
    view.laidOut = true;
}

void MarkdownEditor::updateLayout()
{
    if (m_blocks.empty())
        return;

    // Obsidian keeps the caret's own block raw even when the pane loses
    // focus, so interaction - not focus - is what reveals it.
    for (qsizetype i = 0; i < qsizetype(m_blocks.size()); ++i) {
        BlockView &view = m_blocks[size_t(i)];
        const bool active = m_mode == Mode::LivePreview && m_interacted && i == m_activeBlock;
        if (active != view.active) {
            view.active = active;
            view.layout.reset();
            view.laidOut = false;
            view.heightEstimated = false;
        }
    }

    const auto place = [this] {
        qreal y = m_topPadding;
        for (BlockView &view : m_blocks) {
            view.y = y;
            if (!view.laidOut && !view.heightEstimated) {
                view.height = estimatedHeight(view);
                view.heightEstimated = true;
            }
            y += view.height + kBlockSpacing;
        }
        m_totalHeight = y + m_topPadding;
    };

    place();

    // Lay out what is on screen, plus a screen either side so scrolling does
    // not stutter, plus the caret's block wherever it is.
    const qreal scroll = verticalScrollBar()->value();
    const qreal top = scroll - viewport()->height();
    const qreal bottom = scroll + 2 * viewport()->height();

    bool heightsChanged = false;
    for (qsizetype i = 0; i < qsizetype(m_blocks.size()); ++i) {
        BlockView &view = m_blocks[size_t(i)];
        const bool nearViewport = view.y + view.height >= top && view.y <= bottom;
        if (!nearViewport && i != m_activeBlock)
            continue;
        if (view.laidOut)
            continue;

        const qreal before = view.height;
        layoutBlock(view);
        heightsChanged = heightsChanged || !qFuzzyCompare(before + 1, view.height + 1);
    }

    if (heightsChanged)
        place();
    updateScrollRange();
}

void MarkdownEditor::updateScrollRange()
{
    const int page = viewport()->height();
    verticalScrollBar()->setPageStep(page);
    verticalScrollBar()->setSingleStep(qMax(1, int(QFontMetricsF(font()).height())));
    verticalScrollBar()->setRange(0, qMax(0, int(m_totalHeight) - page));
}

// --- painting -------------------------------------------------------------

void MarkdownEditor::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(viewport()->rect(), m_theme.color(QStringLiteral("--background-primary")));

    const qreal scroll = verticalScrollBar()->value();
    const qreal left = contentLeft();
    const QColor text = m_theme.color(QStringLiteral("--text-normal"));
    const QColor selectionColour = m_theme.color(QStringLiteral("--interactive-accent"));

    const Range selected = selection();
    painter.setPen(text);

    for (const BlockView &view : m_blocks) {
        const qreal top = view.y - scroll;
        if (top + view.height < 0)
            continue;
        if (top > viewport()->height())
            break;

        if (view.span.kind == BlockSpan::Kind::Frontmatter && m_mode != Mode::Source
            && !m_properties.isEmpty()) {
            paintProperties(painter, view, left, top);
            painter.setPen(text);
            continue;
        }

        if (view.callout) {
            paintCallout(painter, view, left, top);
            painter.setPen(view.calloutStyle.color.isValid() ? view.calloutStyle.color : text);
        } else {
            painter.setPen(text);
        }

        if (view.span.kind == BlockSpan::Kind::Code) {
            paintCodeChrome(painter, view, left, top);
            painter.setPen(text);
        }

        if (view.folded)
            continue; // the header is all a folded block shows

        QList<QTextLayout::FormatRange> selectionRanges;
        if (selected.length() > 0 && selected.overlaps(view.span.range)) {
            const Range shown = view.map.toDisplay(
                {qMax(selected.start, view.span.range.start),
                 qMin(selected.end, view.span.range.end)});
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

        if (view.embedPanel)
            paintEmbedChrome(painter, view, left, top);

        if (!view.image.isNull()) {
            const qreal imageX =
                view.imageCentred ? left + (contentWidth() - view.imageSize.width()) / 2.0 : left;
            const QRectF target(imageX, top + view.textTop, view.imageSize.width(),
                                view.imageSize.height());
            painter.drawPixmap(target.toRect(), view.image);
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
            if (m_indentGuides && view.span.kind == BlockSpan::Kind::List)
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
            paintListMarkers(painter, view, left + indent, top + drop);
            paintGutter(painter, view, top + drop);
            if (clip)
                painter.restore();
        }
    }

    if (hasFocus() && m_cursorOn && m_mode != Mode::Reading) {
        const QPointF point = cursorPoint();
        if (!point.isNull() || m_cursor == 0) {
            painter.setPen(QPen(m_theme.color(QStringLiteral("--text-normal")), 2));
            const qreal lineHeight = QFontMetricsF(font()).height() * 1.4;
            painter.drawLine(QPointF(point.x(), point.y()),
                             QPointF(point.x(), point.y() + lineHeight));
        }
    }
}

QPointF MarkdownEditor::cursorPoint() const
{
    const qsizetype index = blockAt(m_cursor);
    if (index < 0 || index >= qsizetype(m_blocks.size()))
        return {};
    const BlockView &view = m_blocks.at(size_t(index));
    if (!view.layout)
        return QPointF(contentLeft(), view.y - verticalScrollBar()->value());

    const qsizetype display = view.map.toDisplay(m_cursor);
    const QTextLine line = view.layout->lineForTextPosition(int(display));
    if (!line.isValid())
        return QPointF(contentLeft(), view.y - verticalScrollBar()->value());

    const qreal x = line.cursorToX(int(display));
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
            // Clicking any cell puts the caret in it; the block then reveals
            // its source, which is where the editing happens for now.
            for (const Cell &cell : view.cells) {
                if (cell.rect.translated(-view.tableScroll, view.y).contains(QPointF(x, y)))
                    return cell.source.start;
            }
            return view.span.range.start;
        }
        if (!view.layout)
            continue;
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
    const qsizetype offset = offsetAt(viewportPoint);
    for (const StyleSpan &span : m_decorations.styles) {
        if (span.kind == StyleSpan::Kind::Link && span.range.contains(offset))
            return span.payload;
    }
    return {};
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
    const qsizetype previousBlock = m_activeBlock;

    m_cursor = clamped;
    if (!keepAnchor)
        m_anchor = clamped;

    m_activeBlock = blockAt(m_cursor);
    if (m_activeBlock != previousBlock)
        updateLayout(); // the two blocks swap between raw and rendered

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
    moveCursor(offsetAt(target), keepAnchor);
    m_goalX = goal;
}

Range MarkdownEditor::selection() const
{
    return {qMin(m_cursor, m_anchor), qMax(m_cursor, m_anchor)};
}

// --- editing --------------------------------------------------------------

void MarkdownEditor::pushUndo()
{
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
    const auto shift = [at, delta](Range &range) {
        if (range.start >= at)
            range.start += delta;
        if (range.end >= at)
            range.end += delta;
    };
    for (BlockView &view : m_blocks)
        shift(view.span.range);
    for (Range &range : m_decorations.syntax)
        shift(range);
    for (StyleSpan &span : m_decorations.styles)
        shift(span.range);
    for (BlockSpan &span : m_decorations.blocks)
        shift(span.range);
    for (Property &property : m_properties)
        shift(property.range);

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

    // The caret's block is shown raw, so a stale decoration inside it changes
    // nothing on screen; every other block is untouched by this edit. Shifting
    // the offsets is therefore enough until the debounced parse lands.
    ++m_textVersion;
    m_lines = SourceMap(m_text); // the gutter needs current line starts
    const qsizetype delta = inserted.size() - removed;
    shiftRanges(at + removed, delta);
    if (delta != 0) {
        const qsizetype index = blockAt(at);
        if (index >= 0 && index < qsizetype(m_blocks.size())) {
            BlockView &view = m_blocks[size_t(index)];
            view.span.range.end += delta;
            view.layout.reset();
            view.laidOut = false;
            view.heightEstimated = false;
        }
    }

    m_cursor = m_anchor = at + inserted.size();
    m_activeBlock = blockAt(m_cursor);

    setModified(true);
    updateLayout();
    ensureCursorVisible();
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
    m_text = snapshot.text;
    ++m_textVersion;
    m_cursor = m_anchor = qBound<qsizetype>(0, snapshot.cursor, m_text.size());
    setModified(true);
    reparse();
    emit textChanged();
    viewport()->update();
}

void MarkdownEditor::redo()
{
    if (m_redo.isEmpty())
        return;
    m_undo.append({m_text, m_cursor});
    const Snapshot snapshot = m_redo.takeLast();
    m_text = snapshot.text;
    ++m_textVersion;
    m_cursor = m_anchor = qBound<qsizetype>(0, snapshot.cursor, m_text.size());
    setModified(true);
    reparse();
    emit textChanged();
    viewport()->update();
}

// --- events ---------------------------------------------------------------

void MarkdownEditor::keyPressEvent(QKeyEvent *event)
{
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

    const bool alt = event->modifiers().testFlag(Qt::AltModifier);
    if (alt && !control) {
        switch (event->key()) {
        case Qt::Key_Up:
            moveLines(-1);
            return;
        case Qt::Key_Down:
            moveLines(1);
            return;
        default:
            break;
        }
    }

    if (control) {
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            toggleTaskAtCursor();
            return;
        case Qt::Key_B:
            toggleBold();
            return;
        case Qt::Key_I:
            toggleItalic();
            return;
        case Qt::Key_K:
            insertLink();
            return;
        case Qt::Key_H:
            if (shift) {
                toggleHighlight();
                return;
            }
            showFind(true);
            return;
        case Qt::Key_F:
            showFind(false);
            return;
        case Qt::Key_D:
            if (shift) {
                duplicateLines();
                return;
            }
            break;
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
            insertText(QApplication::clipboard()->text());
            return;
        default:
            break;
        }
    }

    m_goalX = -1;
    switch (event->key()) {
    case Qt::Key_Left:
        moveCursor(m_cursor > 0 ? m_cursor - 1 : 0, shift);
        return;
    case Qt::Key_Right:
        moveCursor(m_cursor + 1, shift);
        return;
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
        if (!deleteAutoPair())
            removeSelectionOrChar(-1);
        updateSuggestions();
        return;
    case Qt::Key_Delete:
        removeSelectionOrChar(1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!handleListEnter())
            insertText(QStringLiteral("\n"));
        return;
    case Qt::Key_Tab:
        if (!handleListIndent(1))
            insertText(QString(m_tabWidth, QLatin1Char(' ')));
        return;
    case Qt::Key_Backtab: // Shift+Tab
        if (handleListIndent(-1))
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

    if (!event->text().isEmpty() && event->text().at(0).isPrint()) {
        // Auto-pairing owns the brackets and quotes, "[[" included: the first
        // "[" opens "[]", the second sees the "]" ahead of it and opens
        // another, so the suggester still gets its "[[]]".
        if (!handleAutoPair(event->text().at(0)))
            insertText(event->text());
        updateSuggestions();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void MarkdownEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }

    const QString target = linkAt(event->pos());
    if (!target.isEmpty() && event->modifiers().testFlag(Qt::ControlModifier)) {
        emit linkActivated(target);
        return;
    }

    if (handleBlockClick(event->pos()))
        return;

    for (const BlockView &view : m_blocks) {
        // An active block shows its raw source and paints no box, so there is
        // nothing there to click.
        if (view.span.kind != BlockSpan::Kind::List || !view.laidOut || view.active)
            continue;
        for (const ListItemSpan &item : m_decorations.listItems) {
            if (!item.task || !item.range.overlaps(view.span.range))
                continue;
            if (checkboxRect(view, item).adjusted(-3, -3, 3, 3).contains(event->pos())) {
                toggleTask(item);
                return;
            }
        }
    }

    m_goalX = -1;
    m_interacted = true;
    moveCursor(offsetAt(event->pos()), event->modifiers().testFlag(Qt::ShiftModifier));
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons().testFlag(Qt::LeftButton)) {
        moveCursor(offsetAt(event->pos()), true);
        return;
    }
    const bool overLink = !linkAt(event->pos()).isEmpty();
    viewport()->setCursor(overLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void MarkdownEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QString target = linkAt(event->pos());
    if (!target.isEmpty()) {
        emit linkActivated(target);
        return;
    }

    // Select the word under the caret.
    const qsizetype at = offsetAt(event->pos());
    qsizetype start = at;
    qsizetype end = at;
    while (start > 0 && m_text.at(start - 1).isLetterOrNumber())
        --start;
    while (end < m_text.size() && m_text.at(end).isLetterOrNumber())
        ++end;
    m_anchor = start;
    moveCursor(end, true);
}

void MarkdownEditor::wheelEvent(QWheelEvent *event)
{
    BlockView *table = scrollableTableAt(event->position().toPoint());
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

void MarkdownEditor::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);
    m_blink.stop();
    m_cursorOn = false;
    viewport()->update();
}

void MarkdownEditor::inputMethodEvent(QInputMethodEvent *event)
{
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
