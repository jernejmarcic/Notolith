#include "MarkdownEditor.h"

#include "Icons.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStringListModel>
#include <QtConcurrent>
#include <QTextLine>
#include <algorithm>

namespace noteAppC {

namespace {

// The source's own blank lines make the vertical rhythm, exactly as the file
// reads; only headings get extra air, the way Obsidian styles them.
constexpr int kBlockSpacing = 0;
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
        adoptDecorations(std::move(job.decorations));
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
    m_lineHeights.clear();
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
    reparse();
    setModified(false);
    verticalScrollBar()->setValue(0);
    viewport()->update();
}

void MarkdownEditor::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modificationChanged(m_modified);
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
    Decorations decorations = m_parser.parseAll(m_text).decorations;
    sortDecorations(decorations);
    m_parsedVersion = m_textVersion;
    adoptDecorations(std::move(decorations));
}

void MarkdownEditor::startParse()
{
    if (m_parseWatcher.isRunning())
        return; // the finished handler starts the next one

    const QString snapshot = m_text;
    const quint64 version = m_textVersion;
    m_parseWatcher.setFuture(QtConcurrent::run([snapshot, version] {
        ParseJob job;
        job.decorations = threadParser().parseAll(snapshot).decorations;
        sortDecorations(job.decorations);
        job.version = version;
        return job;
    }));
}

void MarkdownEditor::adoptDecorations(Decorations decorations)
{
    m_decorations = std::move(decorations);
    rebuildBlocks();
    indexDecorations();
    invalidateLayouts();
    updateLayout();
    viewport()->update();
}

void MarkdownEditor::indexDecorations()
{
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

    // Prose breathes at 1.5; code is tighter, as it is in Obsidian.
    const bool tight = span.kind == BlockSpan::Kind::Code
                       || span.kind == BlockSpan::Kind::Frontmatter;
    const qreal height = QFontMetricsF(fontFor(span)).height() * (tight ? 1.3 : 1.5);
    m_lineHeights.insert(key, height);
    return height;
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

void MarkdownEditor::rebuildBlocks()
{
    QList<BlockSpan> spans = m_decorations.blocks;
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
            // The panel behind it is painted by the block; the token colours
            // come from KSyntaxHighlighting, applied after these.
            continue;
        case StyleSpan::Kind::Link:
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
        return {};

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

int MarkdownEditor::contentWidth() const
{
    const int available = viewport()->width() - 2 * m_minimumSideMargin;
    if (!m_readable)
        return qMax(120, available);
    return qMax(120, qMin(available, m_contentWidth));
}

int MarkdownEditor::contentLeft() const
{
    return qMax(m_minimumSideMargin, (viewport()->width() - contentWidth()) / 2);
}

bool MarkdownEditor::softBreakAt(qsizetype source) const
{
    if (m_strictLineBreaks)
        return false;
    // A hard break: two spaces or a backslash before the newline.
    if (source >= 2 && m_text.at(source - 1) == QLatin1Char(' ')
        && m_text.at(source - 2) == QLatin1Char(' '))
        return false;
    if (source >= 1 && m_text.at(source - 1) == QLatin1Char('\\'))
        return false;

    for (const Range &paragraph : m_decorations.paragraphs) {
        if (paragraph.start > source)
            break; // sorted by the walk order, which follows the document
        if (source > paragraph.start && source < paragraph.end)
            return true;
    }
    return false;
}

const CalloutSpan *MarkdownEditor::calloutFor(const BlockView &view) const
{
    for (const CalloutSpan &callout : m_decorations.callouts) {
        if (callout.range.start == view.span.range.start)
            return &callout;
    }
    return nullptr;
}

qreal MarkdownEditor::calloutHeaderHeight() const
{
    return QFontMetricsF(m_theme.textFont()).height() * 1.9;
}

void MarkdownEditor::paintCallout(QPainter &painter, const BlockView &view, qreal left,
                                  qreal top) const
{
    if (!view.callout)
        return;

    const QColor accent = m_theme.calloutColor(view.callout->type);
    QColor tint = accent;
    tint.setAlphaF(m_theme.isDark() ? 0.12f : 0.08f);

    const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
    QPainterPath rounded;
    rounded.addRoundedRect(panel, 6, 6);
    painter.fillPath(rounded, tint);
    painter.fillRect(QRectF(panel.left(), panel.top(), 3, panel.height()), accent);

    const qreal iconSize = QFontMetricsF(m_theme.textFont()).height();
    const QIcon icon = Icons::icon(Theme::calloutIcon(view.callout->type), accent, int(iconSize));
    const QRectF iconRect(panel.left() + 14, panel.top() + 10, iconSize, iconSize);
    icon.paint(&painter, iconRect.toRect());

    painter.save();
    QFont title = m_theme.textFont();
    title.setWeight(QFont::DemiBold);
    painter.setFont(title);
    painter.setPen(accent);
    painter.drawText(QRectF(iconRect.right() + 8, panel.top() + 8,
                            panel.width() - iconSize - 40, iconSize * 1.4),
                     Qt::AlignVCenter | Qt::AlignLeft, view.callout->title);
    painter.restore();
}

const TableSpan *MarkdownEditor::tableFor(const BlockView &view) const
{
    for (const TableSpan &table : m_decorations.tables) {
        if (table.range.start == view.span.range.start)
            return &table;
    }
    return nullptr;
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

    qreal total = 0;
    for (const qreal width : std::as_const(natural))
        total += width;
    const qreal available = contentWidth();
    const qreal scale = total > available ? available / total : 1.0;

    QList<qreal> widths;
    QList<qreal> offsets;
    qreal x = 0;
    for (const qreal width : std::as_const(natural)) {
        widths << width * scale;
        offsets << x;
        x += width * scale;
    }

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

    for (const Cell &cell : view.cells) {
        const QRectF rect = cell.rect.translated(left, top);
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
        cell.layout->draw(&painter, QPointF(rect.left() + padX + qMax<qreal>(0, offset),
                                            rect.top() + padY));
    }
}

void MarkdownEditor::layoutBlock(BlockView &view)
{
    view.cells.clear();
    view.callout = nullptr;
    const bool renderTable = view.span.kind == BlockSpan::Kind::Table
                             && m_mode != Mode::Source && !view.active;
    if (renderTable && layoutTable(view))
        return;

    const bool reveal = m_mode == Mode::Source
                        || (m_mode == Mode::LivePreview && view.active);
    QList<Range> hidden;
    if (!reveal) {
        hidden.reserve(view.lastSyntax - view.firstSyntax);
        for (int i = view.firstSyntax; i < view.lastSyntax; ++i)
            hidden.append(m_decorations.syntax.at(i));
    }
    view.map = reveal ? DisplayMap::identity(m_text, view.span.range)
                      : DisplayMap::build(m_text, view.span.range, hidden);

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

    view.layout = std::make_unique<QTextLayout>(shown);
    view.layout->setCacheEnabled(true);
    view.layout->setFont(fontFor(view.span));

    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    view.layout->setTextOption(option);
    view.layout->setFormats(formatsFor(view));

    const qreal lineHeight = lineHeightFor(view.span);
    const qreal headingAir = view.span.kind == BlockSpan::Kind::Heading ? lineHeight * 0.4 : 0;
    view.layout->beginLayout();
    qreal height = 0;
    while (true) {
        QTextLine line = view.layout->createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(contentWidth()
                          - (view.span.kind == BlockSpan::Kind::Callout ? 28 : 0));
        line.setPosition(QPointF(0, height));
        height += lineHeight;
    }
    view.layout->endLayout();
    view.height = qMax(height, lineHeight) + headingAir;

    view.callout = view.span.kind == BlockSpan::Kind::Callout ? calloutFor(view) : nullptr;
    if (view.callout) {
        // The title line is drawn by the callout header, so the body starts
        // below it; an empty callout still shows its title.
        view.height = height > 0 ? height + calloutHeaderHeight()
                                 : calloutHeaderHeight();
    }
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

        if (view.callout)
            paintCallout(painter, view, left, top);

        if (view.span.kind == BlockSpan::Kind::Code) {
            const QRectF panel(left - 14, top - 8, contentWidth() + 28, view.height + 16);
            QPainterPath rounded;
            rounded.addRoundedRect(panel, 6, 6);
            painter.fillPath(rounded, m_theme.color(QStringLiteral("--background-primary-alt")));

            QString language;
            codeContentOf(view, &language);
            if (!language.isEmpty()) {
                painter.save();
                QFont badge = m_theme.uiFont();
                badge.setPixelSize(m_theme.value(QStringLiteral("--font-ui-smaller")).toInt());
                painter.setFont(badge);
                painter.setPen(m_theme.color(QStringLiteral("--text-faint")));
                painter.drawText(panel.adjusted(0, 4, -10, 0),
                                 Qt::AlignTop | Qt::AlignRight, language);
                painter.restore();
                painter.setPen(text);
            }
        }

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

        if (!view.cells.empty()) {
            paintTable(painter, view, left, top);
            continue;
        }
        if (view.layout) {
            const qreal indent = view.callout ? 14 : 0;
            const qreal drop = view.callout ? calloutHeaderHeight() : 0;
            view.layout->draw(&painter, QPointF(left + indent, top + drop), selectionRanges);
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
    return QPointF(contentLeft() + x, view.y - verticalScrollBar()->value() + line.y());
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
                if (cell.rect.translated(0, view.y).contains(QPointF(x, y)))
                    return cell.source.start;
            }
            return view.span.range.start;
        }
        if (!view.layout)
            continue;
        const bool last = &view == &m_blocks.back();
        if (y > view.y + view.height + kBlockSpacing && !last)
            continue;

        for (int i = 0; i < view.layout->lineCount(); ++i) {
            const QTextLine line = view.layout->lineAt(i);
            const bool lastLine = i == view.layout->lineCount() - 1;
            if (y <= view.y + line.y() + line.height() || (lastLine && last)) {
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
            y += line.y();
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
    if (!m_undo.isEmpty() && m_lastEdit.elapsed() < kTypingBurstMs) {
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
}

void MarkdownEditor::applyEdit(qsizetype at, qsizetype removed, const QString &inserted)
{
    m_text.remove(at, removed);
    m_text.insert(at, inserted);

    // The caret's block is shown raw, so a stale decoration inside it changes
    // nothing on screen; every other block is untouched by this edit. Shifting
    // the offsets is therefore enough until the debounced parse lands.
    ++m_textVersion;
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
        removeSelectionOrChar(-1);
        updateSuggestions();
        return;
    case Qt::Key_Delete:
        removeSelectionOrChar(1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        insertText(QStringLiteral("\n"));
        return;
    case Qt::Key_Tab:
        insertText(QStringLiteral("    "));
        return;
    default:
        break;
    }

    if (!event->text().isEmpty() && event->text().at(0).isPrint()) {
        insertText(event->text());
        // Typing the second bracket closes the pair, as in Obsidian.
        if (event->text() == QLatin1String("[")
            && QStringView(m_text).mid(0, m_cursor).endsWith(QLatin1String("[["))) {
            const qsizetype at = m_cursor;
            applyEdit(at, 0, QStringLiteral("]]"));
            m_cursor = m_anchor = at; // stay between the brackets
            m_activeBlock = blockAt(m_cursor);
        }
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
