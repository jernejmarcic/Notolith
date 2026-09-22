#include "MarkdownEditor.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStringListModel>
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

    // ponytail: the parse is synchronous behind a short debounce. It measures
    // well under the keystroke budget on notes this size; move it to the
    // thread pool when a 1 MB note says otherwise.
    m_parseTimer.setSingleShot(true);
    m_parseTimer.setInterval(20);
    connect(&m_parseTimer, &QTimer::timeout, this, [this] {
        reparse();
        viewport()->update();
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
    m_text.remove(m_cursor - prefix->size(), prefix->size());
    m_cursor -= prefix->size();
    const bool closed = QStringView(m_text).mid(m_cursor).startsWith(QLatin1String("]]"));
    m_text.insert(m_cursor, closed ? completion : completion + QLatin1String("]]"));
    m_cursor += completion.size() + 2;
    m_anchor = m_cursor;

    setModified(true);
    reparse();
    m_activeBlock = blockAt(m_cursor);
    layoutAll();
    emit textChanged();
    viewport()->update();
}

MarkdownEditor::~MarkdownEditor() = default;

void MarkdownEditor::setTheme(const Theme &theme)
{
    m_theme = theme;
    setFont(theme.textFont());
    layoutAll();
    viewport()->update();
}

void MarkdownEditor::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    layoutAll();
    viewport()->update();
}

void MarkdownEditor::setNoteText(const QString &text)
{
    m_text = text;
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
    layoutAll();
    viewport()->update();
}

void MarkdownEditor::setContentWidth(int pixels)
{
    m_contentWidth = qMax(200, pixels);
    layoutAll();
    viewport()->update();
}

QString MarkdownEditor::displayText() const
{
    QStringList parts;
    parts.reserve(int(m_blocks.size()));
    for (const BlockView &view : m_blocks)
        parts << view.map.text();
    return parts.join(QString());
}

void MarkdownEditor::enterBlockAt(qsizetype offset)
{
    m_interacted = true;
    moveCursor(offset, false);
    layoutAll();
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

void MarkdownEditor::reparse()
{
    m_decorations = m_parser.parseAll(m_text).decorations;
    rebuildBlocks();
    layoutAll();
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
    for (qsizetype i = 0; i < qsizetype(m_blocks.size()); ++i) {
        const Range &range = m_blocks.at(size_t(i)).span.range;
        if (sourceOffset >= range.start && sourceOffset <= range.end)
            return i;
    }
    return m_blocks.empty() ? -1 : qsizetype(m_blocks.size()) - 1;
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
    QList<QTextLayout::FormatRange> formats;
    const Range block = view.span.range;

    const QColor accent = m_theme.color(QStringLiteral("--text-accent"));
    const QColor faint = m_theme.color(QStringLiteral("--text-faint"));
    const QColor codeColour = m_theme.color(QStringLiteral("--text-normal"));
    const QColor codeBackground = m_theme.color(QStringLiteral("--background-primary-alt"));
    const QColor highlight = m_theme.color(QStringLiteral("--color-yellow"));

    const auto add = [&](const Range &source, const QTextCharFormat &format) {
        const Range shown = view.map.toDisplay(source);
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
        for (const Range &marker : m_decorations.syntax) {
            if (!marker.overlaps(block))
                continue;
            QTextCharFormat format;
            format.setForeground(faint);
            add(marker, format);
        }
    }

    for (const StyleSpan &span : m_decorations.styles) {
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
        case StyleSpan::Kind::CodeBlock:
            format.setFont(m_theme.monospaceFont());
            format.setForeground(codeColour);
            format.setBackground(codeBackground);
            break;
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

void MarkdownEditor::layoutBlock(BlockView &view)
{
    const bool reveal = m_mode == Mode::Source
                        || (m_mode == Mode::LivePreview && view.active);
    view.map = reveal ? DisplayMap::identity(m_text, view.span.range)
                      : DisplayMap::build(m_text, view.span.range,
                                          m_decorations.hiddenFor(view.span.range,
                                                                  std::numeric_limits<qsizetype>::min()));

    // QTextLayout lays out one paragraph and ignores "\n"; U+2028 is the
    // character it does break on. Same length, so every display offset the
    // DisplayMap computed still points at the same glyph.
    QString shown = view.map.text();
    shown.replace(QLatin1Char('\n'), QChar::LineSeparator);
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

    const qreal lineHeight = QFontMetricsF(view.layout->font()).height() * 1.5;
    const qreal headingAir = view.span.kind == BlockSpan::Kind::Heading ? lineHeight * 0.4 : 0;
    view.layout->beginLayout();
    qreal height = 0;
    while (true) {
        QTextLine line = view.layout->createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(contentWidth());
        line.setPosition(QPointF(0, height));
        height += lineHeight;
    }
    view.layout->endLayout();
    view.height = qMax(height, lineHeight) + headingAir;
}

void MarkdownEditor::layoutAll()
{
    qreal y = m_topPadding;
    for (BlockView &view : m_blocks) {
        // Obsidian keeps the caret's own block raw even when the pane loses
        // focus, so interaction - not focus - is what reveals it.
        view.active = m_mode == Mode::LivePreview && m_interacted
                      && qsizetype(&view - m_blocks.data()) == m_activeBlock;
        layoutBlock(view);
        view.y = y;
        y += view.height + kBlockSpacing;
    }
    m_totalHeight = y + m_topPadding;
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

        if (view.span.kind == BlockSpan::Kind::Code) {
            QRectF background(left - 12, top - 6, contentWidth() + 24, view.height + 12);
            painter.fillRect(background,
                             m_theme.color(QStringLiteral("--background-primary-alt")));
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

        view.layout->draw(&painter, QPointF(left, top), selectionRanges);
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
        return {};

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
    layoutAll();
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
        layoutAll(); // the two blocks swap between raw and rendered

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

void MarkdownEditor::insertText(const QString &text)
{
    if (m_mode == Mode::Reading || text.isEmpty())
        return;
    pushUndo();

    const Range selected = selection();
    if (selected.length() > 0) {
        m_text.remove(selected.start, selected.length());
        m_cursor = selected.start;
    }
    m_text.insert(m_cursor, text);
    m_cursor += text.size();
    m_anchor = m_cursor;

    setModified(true);
    reparse();
    m_activeBlock = blockAt(m_cursor);
    layoutAll();
    ensureCursorVisible();
    emit textChanged();
    viewport()->update();
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
    m_text.remove(toRemove.start, toRemove.length());
    m_cursor = m_anchor = toRemove.start;

    setModified(true);
    reparse();
    m_activeBlock = blockAt(m_cursor);
    layoutAll();
    ensureCursorVisible();
    emit textChanged();
    viewport()->update();
}

void MarkdownEditor::undo()
{
    if (m_undo.isEmpty())
        return;
    m_redo.append({m_text, m_cursor});
    const Snapshot snapshot = m_undo.takeLast();
    m_text = snapshot.text;
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
        layoutAll();
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
            m_text.insert(m_cursor, QStringLiteral("]]"));
            reparse();
            layoutAll();
            viewport()->update();
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
    layoutAll();
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
