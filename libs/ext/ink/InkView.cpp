#include "InkView.h"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "MarkdownEditor.h"
#include "Settings.h"
#include "Theme.h"
#include "Vault.h"
#include "core/InkFormat.h"
#include "core/InkTools.h"

namespace noteAppC::ink {

/// What the pool hands back: the parsed document and how the parse went.
struct InkView::Parsed {
    InkDocument document;
    InkReadResult result;
};

namespace {

constexpr const char *kEraserModeKey = "ink.eraserMode"; ///< "stroke" or "split"
constexpr const char *kSelectShapeKey = "ink.selectShape"; ///< "lasso" or "rect"
constexpr const char *kShapeKindKey = "ink.shapeKind";   ///< a ShapeKind name: line, arrow, rect, ellipse
constexpr const char *kDrawWithMouseKey = "ink.drawWithMouse";
constexpr const char *kPressureByDeviceKey = "ink.pressureByDevice"; ///< a compact JSON object {"<device name>": <learned maximum>}

constexpr double kPxPerMM = 96.0 / 25.4; ///< document px are CSS px at 96 dpi
constexpr ShapeKind kShapeOrder[] = {ShapeKind::Line, ShapeKind::Arrow, ShapeKind::Rect, ShapeKind::Ellipse};

struct PagePreset {
    const char *key;
    const char *label;
    double widthMM, heightMM;
};
constexpr PagePreset kPagePresets[] = {{"a4", QT_TR_NOOP("A4"), 210, 297}, {"a5", QT_TR_NOOP("A5"), 148, 210},
                                       {"letter", QT_TR_NOOP("Letter"), 215.9, 279.4}, {"custom", QT_TR_NOOP("Custom"), 0, 0}};

/// The pen colours, as theme tokens of the page's own theme (the page is paper: see InkView::paperTheme).
struct Swatch {
    const char *token;
    const char *name;
};
constexpr Swatch kPenSwatches[] = {{"--text-normal", QT_TR_NOOP("Ink")},     {"--color-red", QT_TR_NOOP("Red")},
                                   {"--color-blue", QT_TR_NOOP("Blue")},     {"--color-green", QT_TR_NOOP("Green")},
                                   {"--color-yellow", QT_TR_NOOP("Yellow")}, {"--color-purple", QT_TR_NOOP("Purple")}};

QIcon swatchIcon(const QColor &colour)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(colour);
    QPainter painter(&pixmap);
    painter.setPen(QColor(0x80, 0x80, 0x80));
    painter.drawRect(0, 0, 15, 15);
    return QIcon(pixmap);
}

QColor opaque(QColor colour)
{
    colour.setAlpha(255);
    return colour;
}

QString shapeKindLabel(ShapeKind kind)
{
    switch (kind) {
    case ShapeKind::Arrow: return InkView::tr("Arrow");
    case ShapeKind::Rect: return InkView::tr("Rectangle");
    case ShapeKind::Ellipse: return InkView::tr("Ellipse");
    default: return InkView::tr("Line");
    }
}

QJsonObject parsePressureMap(const QString &stored)
{
    return QJsonDocument::fromJson(stored.toUtf8()).object(); // malformed: an empty object, nothing applied
}

} // namespace

QList<QPair<QString, QVariant>> InkView::layoutChoices()
{
    return {{tr("Fixed"), layoutName(LayoutKind::Fixed)},
            {tr("Continuous"), layoutName(LayoutKind::ContinuousVertical)},
            {tr("Semi-infinite"), layoutName(LayoutKind::SemiInfinite)},
            {tr("Infinite"), layoutName(LayoutKind::Infinite)}};
}

QList<QPair<QString, QVariant>> InkView::patternChoices()
{
    const std::pair<Pattern, QString> labels[] = {
        {Pattern::None, tr("None")},           {Pattern::Lines, tr("Lines")},         {Pattern::Grid, tr("Grid")},
        {Pattern::Dots, tr("Dots")},           {Pattern::LinesMargin, tr("Lines with margin")}, {Pattern::GridMajor, tr("Grid with major lines")},
        {Pattern::MusicStaff, tr("Music staff")}, {Pattern::Calligraphy, tr("Calligraphy")}, {Pattern::Seyes, tr("Seyes")},
        {Pattern::Cornell, tr("Cornell")},     {Pattern::HexGrid, tr("Hexagonal grid")}};
    QList<QPair<QString, QVariant>> choices;
    for (const auto &[pattern, label] : labels)
        choices.append({label, patternName(pattern)});
    return choices;
}

InkView::InkView(Settings *settings, QWidget *parent)
    : ModuleView(parent), m_settings(settings), m_area(new QScrollArea(this)), m_canvas(new InkCanvas),
      m_toolBar(new QToolBar(this))
{
    setObjectName(QStringLiteral("inkView"));
    m_canvas->setObjectName(QStringLiteral("inkCanvas"));
    m_area->setWidget(m_canvas);
    m_area->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_area->setWidgetResizable(false);

    buildTools(new QActionGroup(this));
    m_canvas->setTextRenderer([this](const QString &markdown, const QSizeF &size, qreal scale) { return renderText(markdown, size, scale); });
    m_canvas->setTextEditorFactory({[this](const QString &markdown) { return makeTextEditor(markdown); },
                                    [](const QWidget *editor) { return static_cast<const MarkdownEditor *>(editor)->noteText(); },
                                    [this](QWidget *editor, double zoom) {
                                        Theme paper = paperTheme(false);
                                        paper.setToken(QStringLiteral("--font-text-size"), QString::number(std::max(6, qRound(paper.value(QStringLiteral("--font-text-size")).toInt() * zoom))));
                                        static_cast<MarkdownEditor *>(editor)->setTheme(paper);
                                    }});
    buildToolBar();
    buildPageMenu();

    // The notices: above the canvas, outside the toolbar, whose overflow menu would hide them in a narrow window.
    const auto notice = [this](const char *name, const QString &text, bool visible) {
        auto *label = new QLabel(text, this);
        label->setObjectName(QLatin1String(name));
        label->setWordWrap(true);
        label->setMargin(6);
        label->setVisible(visible);
        return label;
    };
    m_readOnlyLabel = notice("inkReadOnlyLabel", tr("Read-only: written by a newer Notolith"), false);
    m_loadingLabel = notice("inkLoadingLabel", tr("Loading..."), false);
    m_errorLabel = notice("inkErrorLabel", QString(), false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_readOnlyLabel);
    layout->addWidget(m_loadingLabel);
    layout->addWidget(m_errorLabel);
    layout->addWidget(m_area, 1);

    connect(m_canvas, &InkCanvas::documentEdited, this, &InkView::onEdited);
    connect(m_canvas, &InkCanvas::textEditChanged, this, &InkView::onEdited); // typing lights the modified mark, not only the commit
    connect(m_canvas, &InkCanvas::toolChanged, this, [this](InkCanvas::Tool tool) { // the keys P E L S T (R14) move the toolbar too
        m_penAction->setChecked(tool == InkCanvas::Tool::Pen);
        m_textAction->setChecked(tool == InkCanvas::Tool::Text);
        m_eraserAction->setChecked(tool == InkCanvas::Tool::Eraser);
        m_selectAction->setChecked(tool == InkCanvas::Tool::Lasso);
        m_shapeAction->setChecked(tool == InkCanvas::Tool::Shape);
    });
    connect(m_canvas, &InkCanvas::shapeKindChanged, this, [this](ShapeKind kind) { // Shift+S moves the menu and the face too
        showShapeKind(kind);
        remember(kShapeKindKey, shapeKindName(kind));
    });
    connect(m_canvas, &InkCanvas::selectShapeChanged, this, [this](InkCanvas::SelectShape shape) { // Shift+L moves the toolbar too
        const bool rect = shape == InkCanvas::SelectShape::Rect;
        m_selectRectAction->setChecked(rect);
        m_selectLassoAction->setChecked(!rect);
        remember(kSelectShapeKey, rect ? QStringLiteral("rect") : QStringLiteral("lasso"));
    });
    connect(m_canvas, &InkCanvas::eraserModeChanged, this, [this](InkCanvas::EraserMode mode) { // Shift+E moves the toolbar too
        const bool split = mode == InkCanvas::EraserMode::Split;
        m_splitAction->setChecked(split);
        remember(kEraserModeKey, split ? QStringLiteral("split") : QStringLiteral("stroke"));
    });
    connect(m_canvas, &InkCanvas::zoomChanged, this, &InkView::showZoom);
    connect(m_canvas, &InkCanvas::learnedPressureChanged, this, &InkView::learnedPressure);
    m_pressureSave = new QTimer(this);
    m_pressureSave->setSingleShot(true);
    m_pressureSave->setInterval(kPressureSaveMs);
    connect(m_pressureSave, &QTimer::timeout, this, [this] {
        if (m_settings)
            m_settings->save();
    });
    connect(m_canvas, &InkCanvas::selectionChanged, this, &InkView::updateEditActions);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &InkView::updateEditActions);
    connect(m_canvas, &InkCanvas::panRequested, this, [this](QPoint delta) {
        m_area->horizontalScrollBar()->setValue(m_area->horizontalScrollBar()->value() - delta.x());
        m_area->verticalScrollBar()->setValue(m_area->verticalScrollBar()->value() - delta.y());
    });
    connect(m_canvas, &InkCanvas::originShifted, this, [this](QPoint delta) { // the Infinite sheet grew left or up: stay on the same ink
        m_area->horizontalScrollBar()->setValue(m_area->horizontalScrollBar()->value() + delta.x());
        m_area->verticalScrollBar()->setValue(m_area->verticalScrollBar()->value() + delta.y());
    });
    if (m_settings) {
        // R14: what each pen's pressure range was learned to be, from the first sample of this session.
        const QVariant seed = m_settings->value(QStringLiteral("ink.pressureMax")); // 1.0: nothing known, learn it
        if (seed.isValid() && seed.toDouble() > 0)
            m_canvas->setPressureSeed(seed.toDouble());
        const QJsonObject learned = parsePressureMap(m_settings->text(QLatin1String(kPressureByDeviceKey)));
        for (auto it = learned.begin(); it != learned.end(); ++it)
            if (it.value().isDouble())
                m_canvas->setLearnedPressure(it.key(), it.value().toDouble());
        const double width = m_settings->value(QStringLiteral("ink.defaultPenWidth")).toDouble();
        if (width >= m_penWidth->minimum() && width <= m_penWidth->maximum())
            m_penWidth->setValue(width);
        applySettings(*m_settings); // the window calls applySettings() again after every change
    }
    showZoom(m_canvas->zoom());
    showShapeKind(m_canvas->shapeKind());
    onEdited();
}

InkView::~InkView()
{
    // ~QWidget hides the canvas and takes the focus from an open editor after this body has run and these members are
    // gone: a stroke finished or a box committed by that would call onEdited() and friends on a half-destroyed view.
    m_canvas->disconnect(this);
    m_canvas->discardTextEdit();
    flushSettings(); // a pen learned a moment ago is not lost to a closing tab (the window flushes first when it goes itself)
}

void InkView::flushSettings()
{
    if (!m_pressureSave->isActive())
        return;
    m_pressureSave->stop();
    if (m_settings)
        m_settings->save();
}

/// Another tab's toolbar toggle (draw with mouse, the eraser, select and shape choices) lands in the shared settings
/// without telling this tab; it shows them when it is shown again.
void InkView::showEvent(QShowEvent *event)
{
    ModuleView::showEvent(event);
    if (m_settings)
        applySettings(*m_settings);
    updateEditActions();
}

/// Delete, Copy and Paste of a selection: enabled with a selection, and Paste with ink on the clipboard.
void InkView::updateEditActions()
{
    const bool selected = m_toolsOn && !m_canvas->selection().empty();
    m_deleteAction->setEnabled(selected);
    m_copyAction->setEnabled(selected);
    const QMimeData *clip = QGuiApplication::clipboard()->mimeData();
    m_pasteAction->setEnabled(m_toolsOn && clip && clip->hasFormat(QString::fromLatin1(kClipboardMime)));
}

/// The tools: five checkable actions in one group (the keys and the toolbar move each other through InkCanvas::toolChanged),
/// and the toggles and kinds that belong to them.
void InkView::buildTools(QActionGroup *tools)
{
    const auto tool = [this, tools](const char *name, const QString &text, const QString &tip, InkCanvas::Tool which) {
        QAction *action = tools->addAction(text);
        action->setObjectName(QLatin1String(name));
        action->setCheckable(true);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, which] { m_canvas->setTool(which); });
        return action;
    };
    m_penAction = tool("inkTool_pen", tr("Pen"), tr("Pen (P)"), InkCanvas::Tool::Pen);
    m_penAction->setChecked(true);
    m_eraserAction = tool("inkTool_eraser", tr("Eraser"), tr("Eraser (E)"), InkCanvas::Tool::Eraser);
    m_selectAction = tool("inkTool_lasso", tr("Select"), tr("Select with a lasso or a rectangle, then move, resize, copy or delete (L; Shift+L for the other shape)"),
                          InkCanvas::Tool::Lasso);
    m_shapeAction = tool("inkTool_shape", tr("Line"), tr("Draw a line, arrow, rectangle or ellipse by dragging (S; Shift+S for the next kind)"),
                         InkCanvas::Tool::Shape);
    m_textAction = tool("inkTool_text", tr("Text"), tr("Click to write a text box, or to edit one; Markdown, like a note (T)"), InkCanvas::Tool::Text);

    m_splitAction = new QAction(tr("Erase parts"), this); // checked: only the touched part of a stroke goes; unchecked: the whole stroke
    m_splitAction->setObjectName(QStringLiteral("inkEraserSplitAction"));
    m_splitAction->setCheckable(true);
    m_splitAction->setToolTip(tr("Erase parts of strokes instead of whole strokes (Shift+E)"));
    connect(m_splitAction, &QAction::triggered, this, [this](bool on) {
        m_canvas->setEraserMode(on ? InkCanvas::EraserMode::Split : InkCanvas::EraserMode::Stroke);
    });

    auto *selectMenu = new QMenu(this);
    auto *selectShapes = new QActionGroup(this);
    m_selectLassoAction = selectShapes->addAction(tr("Lasso"));
    m_selectLassoAction->setObjectName(QStringLiteral("inkSelectLassoAction"));
    m_selectRectAction = selectShapes->addAction(tr("Rectangle"));
    m_selectRectAction->setObjectName(QStringLiteral("inkSelectRectAction"));
    m_selectRectAction->setToolTip(tr("Select with a rectangle instead of a lasso (Shift+L)"));
    for (QAction *a : {m_selectLassoAction, m_selectRectAction}) {
        a->setCheckable(true);
        selectMenu->addAction(a);
        connect(a, &QAction::triggered, this, [this, a] {
            m_canvas->setTool(InkCanvas::Tool::Lasso); // one click is enough: choosing the shape also picks the select tool
            m_canvas->setSelectShape(a == m_selectRectAction ? InkCanvas::SelectShape::Rect : InkCanvas::SelectShape::Lasso);
        });
    }
    m_selectLassoAction->setChecked(true);
    m_selectAction->setMenu(selectMenu);

    auto *shapeMenu = new QMenu(this);
    auto *shapeKinds = new QActionGroup(this);
    const char *shapeNames[4] = {"inkShapeLineAction", "inkShapeArrowAction", "inkShapeRectAction", "inkShapeEllipseAction"};
    for (size_t i = 0; i < std::size(kShapeOrder); ++i) {
        QAction *a = shapeKinds->addAction(shapeKindLabel(kShapeOrder[i]));
        a->setObjectName(QLatin1String(shapeNames[i]));
        a->setCheckable(true);
        a->setChecked(kShapeOrder[i] == m_canvas->shapeKind());
        connect(a, &QAction::triggered, this, [this, kind = kShapeOrder[i]] {
            m_canvas->setShapeKind(kind);
            m_canvas->setTool(InkCanvas::Tool::Shape); // one click is enough: choosing a kind also picks the tool
        });
        shapeMenu->addAction(a);
        m_shapeKindActions[i] = a;
    }
    m_shapeAction->setMenu(shapeMenu);

    m_mouseAction = new QAction(tr("Draw with mouse"), this);
    m_mouseAction->setObjectName(QStringLiteral("inkDrawWithMouseAction"));
    m_mouseAction->setCheckable(true);
    m_mouseAction->setToolTip(tr("Off, the mouse scrolls and Ctrl+drag selects; a pen always draws"));
    connect(m_mouseAction, &QAction::triggered, this, [this](bool on) {
        m_canvas->setDrawWithMouse(on);
        if (m_settings) { // a bool, not text: not remember()
            m_settings->setValue(QLatin1String(kDrawWithMouseKey), on);
            m_settings->save();
        }
    });

    m_deleteAction = new QAction(tr("Delete"), this);
    m_deleteAction->setObjectName(QStringLiteral("inkDeleteAction"));
    m_deleteAction->setToolTip(tr("Delete the selection (Delete)"));
    connect(m_deleteAction, &QAction::triggered, m_canvas, [this] { m_canvas->deleteSelection(); });
    m_copyAction = new QAction(tr("Copy"), this);
    m_copyAction->setObjectName(QStringLiteral("inkCopyAction"));
    m_copyAction->setToolTip(tr("Copy the selection (Ctrl+C)"));
    connect(m_copyAction, &QAction::triggered, m_canvas, [this] { m_canvas->copySelection(); });
    m_pasteAction = new QAction(tr("Paste"), this);
    m_pasteAction->setObjectName(QStringLiteral("inkPasteAction"));
    m_pasteAction->setToolTip(tr("Paste what was copied (Ctrl+V)"));
    connect(m_pasteAction, &QAction::triggered, m_canvas, [this] { m_canvas->paste(); });
    for (QAction *a : {m_deleteAction, m_copyAction, m_pasteAction})
        a->setEnabled(false);

    m_undoAction = new QAction(tr("Undo"), this);
    m_redoAction = new QAction(tr("Redo"), this);
    connect(m_undoAction, &QAction::triggered, m_canvas, &InkCanvas::undo);
    connect(m_redoAction, &QAction::triggered, m_canvas, &InkCanvas::redo);
}

void InkView::buildToolBar()
{
    m_toolBar->addActions({m_penAction, m_eraserAction, m_splitAction, m_selectAction, m_shapeAction, m_textAction});
    for (QAction *withMenu : {m_selectAction, m_shapeAction}) {
        if (auto *button = qobject_cast<QToolButton *>(m_toolBar->widgetForAction(withMenu)))
            button->setPopupMode(QToolButton::MenuButtonPopup); // the face picks the tool, the arrow opens the menu
    }
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_mouseAction);
    m_toolBar->addSeparator();

    m_penWidth = new QDoubleSpinBox(this);
    m_penWidth->setObjectName(QStringLiteral("inkPenWidthSpin"));
    m_penWidth->setRange(0.5, 40.0);
    m_penWidth->setSingleStep(0.5);
    m_penWidth->setDecimals(1);
    m_penWidth->setValue(m_canvas->penStyle().width);
    m_penWidth->setToolTip(tr("Pen width, in document px"));
    connect(m_penWidth, &QDoubleSpinBox::valueChanged, this, [this](double width) {
        StrokeStyle style = m_canvas->penStyle();
        style.width = width;
        m_canvas->setPenStyle(style);
    });
    m_penWidth->setKeyboardTracking(false); // a typed number applies on Enter, so an applied change is always a finished one
    connect(m_penWidth, &QDoubleSpinBox::editingFinished, m_canvas, qOverload<>(&QWidget::setFocus)); // the keys P E L S T need the canvas
    connect(m_penWidth, &QDoubleSpinBox::valueChanged, this, [this] { // an arrow click is a whole edit: no Enter follows it
        if (m_penWidth->hasFocus())
            m_canvas->setFocus();
    });
    m_toolBar->addWidget(m_penWidth);
    m_editWidgets << m_penWidth;

    auto *colours = new QButtonGroup(this);
    for (int i = 0; i < int(std::size(kPenSwatches)); ++i) {
        auto *swatch = new QToolButton(this);
        swatch->setObjectName(QStringLiteral("inkPenColor_%1").arg(i));
        swatch->setCheckable(true);
        swatch->setAutoRaise(true);
        swatch->setToolTip(tr(kPenSwatches[i].name));
        colours->addButton(swatch, i);
        connect(swatch, &QToolButton::clicked, this, [this, i] { selectPenColour(i); });
        m_toolBar->addWidget(swatch);
        m_penSwatches << swatch;
        m_editWidgets << swatch;
    }
    refreshSwatches();

    m_curveCombo = new QComboBox(this);
    m_curveCombo->setObjectName(QStringLiteral("inkPressureCurveCombo"));
    m_curveCombo->setToolTip(tr("How the pen's pressure changes the width"));
    const std::pair<PressureCurve, QString> curves[] = {{PressureCurve::Const, tr("Constant")},   {PressureCurve::Linear, tr("Linear")},
                                                        {PressureCurve::Sqrt, tr("Square root")}, {PressureCurve::Cbrt, tr("Cube root")},
                                                        {PressureCurve::Pow2, tr("Squared")},     {PressureCurve::Pow3, tr("Cubed")}};
    for (const auto &[curve, label] : curves)
        m_curveCombo->addItem(label, pressureCurveName(curve));
    m_curveCombo->setCurrentIndex(m_curveCombo->findData(pressureCurveName(m_canvas->penStyle().pressureCurve)));
    connect(m_curveCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        StrokeStyle style = m_canvas->penStyle();
        style.pressureCurve = pressureCurveFromName(m_curveCombo->itemData(index).toString());
        m_canvas->setPenStyle(style);
    });
    connect(m_curveCombo, &QComboBox::activated, m_canvas, qOverload<>(&QWidget::setFocus));
    m_toolBar->addWidget(m_curveCombo);
    m_editWidgets << m_curveCombo;

    m_toolBar->addSeparator();
    m_toolBar->addActions({m_undoAction, m_redoAction, m_deleteAction, m_copyAction, m_pasteAction});
    m_toolBar->addSeparator();

    m_zoomCombo = new QComboBox(this);
    m_zoomCombo->setObjectName(QStringLiteral("inkZoomCombo"));
    m_zoomCombo->setToolTip(tr("Zoom (Ctrl+wheel)"));
    for (double zoom : {0.5, 1.0, 2.0})
        m_zoomCombo->addItem(QString::number(qRound(zoom * 100)) + QLatin1Char('%'), zoom);
    connect(m_zoomCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QVariant zoom = m_zoomCombo->itemData(index);
        if (zoom.isValid())
            m_canvas->setZoom(zoom.toDouble());
    });
    connect(m_zoomCombo, &QComboBox::activated, m_canvas, qOverload<>(&QWidget::setFocus));
    m_toolBar->addWidget(m_zoomCombo);
}

/// The Page menu: one panel of controls (a menu of widgets) and "Add page". Each change is one history step.
void InkView::buildPageMenu()
{
    m_pageButton = new QToolButton(this);
    m_pageButton->setObjectName(QStringLiteral("inkPageButton"));
    m_pageButton->setText(tr("Page"));
    m_pageButton->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(m_pageButton);
    auto *panel = new QWidget(menu);
    panel->setObjectName(QStringLiteral("inkPagePanel"));
    auto *form = new QFormLayout(panel);

    m_pageFormat = new QComboBox(panel);
    m_pageFormat->setObjectName(QStringLiteral("inkPageFormatCombo"));
    for (const PagePreset &preset : kPagePresets)
        m_pageFormat->addItem(tr(preset.label), QLatin1String(preset.key));
    form->addRow(tr("Format"), m_pageFormat);
    const auto millimetres = [panel](const char *name) {
        auto *spin = new QDoubleSpinBox(panel);
        spin->setObjectName(QLatin1String(name));
        spin->setRange(20, 3000); // a page between 2 cm and 3 m: never an empty or a runaway one (DocumentSettings::isUsable)
        spin->setDecimals(1);
        spin->setSuffix(QStringLiteral(" mm"));
        spin->setKeyboardTracking(false); // a size is applied when it is entered, not at every digit
        return spin;
    };
    m_pageWidth = millimetres("inkPageWidthSpin");
    m_pageHeight = millimetres("inkPageHeightSpin");
    form->addRow(tr("Width"), m_pageWidth);
    form->addRow(tr("Height"), m_pageHeight);

    // The colours of the two built-in themes: the page is paper, and these are its papers.
    auto *colours = new QButtonGroup(panel);
    colours->setExclusive(false); // a page colour of the file's own matches none of them: none is then checked
    auto *colourRow = new QHBoxLayout;
    const struct { Theme::Mode mode; const char *token; const char *name; } papers[] = {
        {Theme::Mode::Light, "--background-primary", QT_TR_NOOP("White")}, {Theme::Mode::Light, "--background-secondary", QT_TR_NOOP("Light grey")},
        {Theme::Mode::Dark, "--background-primary", QT_TR_NOOP("Dark")}, {Theme::Mode::Dark, "--background-secondary", QT_TR_NOOP("Dark grey")}};
    for (int i = 0; i < int(std::size(papers)); ++i) {
        const QColor colour = opaque(Theme::builtin(papers[i].mode).color(QLatin1String(papers[i].token)));
        auto *swatch = new QToolButton(panel);
        swatch->setObjectName(QStringLiteral("inkPageColor_%1").arg(i));
        swatch->setCheckable(true);
        swatch->setAutoRaise(true);
        swatch->setIcon(swatchIcon(colour));
        swatch->setToolTip(tr(papers[i].name));
        swatch->setProperty("color", colour);
        colours->addButton(swatch, i);
        connect(swatch, &QToolButton::clicked, this, [this, colour] { changePage([colour](DocumentSettings &s) { s.background.color = colour; }); });
        colourRow->addWidget(swatch);
        m_pageSwatches << swatch;
    }
    colourRow->addStretch(1);
    form->addRow(tr("Colour"), colourRow);

    m_pageLayout = new QComboBox(panel);
    m_pageLayout->setObjectName(QStringLiteral("inkPageLayoutCombo"));
    for (const auto &[label, name] : layoutChoices())
        m_pageLayout->addItem(label, name);
    form->addRow(tr("Layout"), m_pageLayout);
    m_pagePattern = new QComboBox(panel);
    m_pagePattern->setObjectName(QStringLiteral("inkPagePatternCombo"));
    for (const auto &[label, name] : patternChoices())
        m_pagePattern->addItem(label, name);
    form->addRow(tr("Pattern"), m_pagePattern);
    m_patternSize = new QDoubleSpinBox(panel);
    m_patternSize->setObjectName(QStringLiteral("inkPatternSizeSpin"));
    m_patternSize->setRange(4, 200); // above DocumentSettings::isUsable's 1e-3 floor, and a pattern a pen can follow
    m_patternSize->setDecimals(1);
    m_patternSize->setSuffix(QStringLiteral(" px"));
    m_patternSize->setKeyboardTracking(false);
    form->addRow(tr("Pattern size"), m_patternSize);

    auto *widgetAction = new QWidgetAction(menu);
    widgetAction->setDefaultWidget(panel);
    menu->addAction(widgetAction);
    menu->addSeparator();
    m_addPageAction = menu->addAction(tr("Add page"));
    m_addPageAction->setObjectName(QStringLiteral("inkAddPageAction"));
    connect(m_addPageAction, &QAction::triggered, this, &InkView::addPage);
    m_pageButton->setMenu(menu);
    m_toolBar->addWidget(m_pageButton);
    m_editWidgets << m_pageButton;

    // The controls change the document; syncPageMenu() (signals blocked) is what makes them show it.
    connect(m_pageFormat, &QComboBox::currentIndexChanged, this, [this](int index) {
        const PagePreset &preset = kPagePresets[index];
        if (preset.widthMM <= 0)
            return; // Custom: the two sizes below say what it is
        changePage([&preset](DocumentSettings &s) {
            s.format.width = preset.widthMM * kPxPerMM;
            s.format.height = preset.heightMM * kPxPerMM;
        });
    });
    connect(m_pageWidth, &QDoubleSpinBox::valueChanged, this, [this](double mm) { changePage([mm](DocumentSettings &s) { s.format.width = mm * kPxPerMM; }); });
    connect(m_pageHeight, &QDoubleSpinBox::valueChanged, this, [this](double mm) { changePage([mm](DocumentSettings &s) { s.format.height = mm * kPxPerMM; }); });
    connect(m_pageLayout, &QComboBox::currentIndexChanged, this, [this](int index) {
        const LayoutKind layout = layoutFromName(m_pageLayout->itemData(index).toString());
        changePage([layout](DocumentSettings &s) { s.layout = layout; });
    });
    connect(m_pagePattern, &QComboBox::currentIndexChanged, this, [this](int index) {
        const Pattern pattern = patternFromName(m_pagePattern->itemData(index).toString());
        changePage([pattern](DocumentSettings &s) { s.background.pattern = pattern; });
    });
    connect(m_patternSize, &QDoubleSpinBox::valueChanged, this, [this](double px) {
        changePage([px](DocumentSettings &s) {
            const QSizeF old = s.background.patternSize; // the spin shows the width: scale both sides, a file's own ratio stays
            s.background.patternSize = old.width() > 0 ? QSizeF(px, old.height() * px / old.width()) : QSizeF(px, px);
        });
    });
}

void InkView::setToolsEnabled(bool on)
{
    for (QAction *action : {m_penAction, m_eraserAction, m_splitAction, m_selectAction, m_selectLassoAction, m_selectRectAction, m_shapeAction, m_textAction})
        action->setEnabled(on);
    for (QWidget *widget : std::as_const(m_editWidgets))
        widget->setEnabled(on);
    m_toolsOn = on;
    updateEditActions();
}

void InkView::remember(const char *key, const QString &value)
{
    if (!m_settings || m_settings->text(QLatin1String(key)) == value)
        return;
    m_settings->setValue(QLatin1String(key), value);
    m_settings->save();
}

void InkView::learnedPressure(const QString &device, double max)
{
    if (!m_settings)
        return;
    QJsonObject map = parsePressureMap(m_settings->text(QLatin1String(kPressureByDeviceKey))); // other views may have learned another pen
    map.insert(device, max);
    m_settings->setValue(QLatin1String(kPressureByDeviceKey), QString::fromUtf8(QJsonDocument(map).toJson(QJsonDocument::Compact)));
    if (!m_pressureSave->isActive())
        m_pressureSave->start(); // a press harder than ever raises it at every sample: one write for the run, not one per sample
}

void InkView::selectPenColour(int index)
{
    m_penColourIndex = index;
    m_penSwatches[index]->setChecked(true);
    StrokeStyle style = m_canvas->penStyle();
    style.color = m_penSwatches[index]->property("color").value<QColor>();
    m_canvas->setPenStyle(style);
}

/// The pen colours come from the page's theme (white paper: dark ink, dark paper: light ink); the one picked stays picked.
void InkView::refreshSwatches()
{
    m_swatchPage = m_canvas->document().settings().background.color;
    const Theme paper = paperTheme(false);
    for (int i = 0; i < m_penSwatches.size(); ++i) {
        const QColor colour = opaque(paper.color(QLatin1String(kPenSwatches[i].token)));
        m_penSwatches[i]->setProperty("color", colour);
        m_penSwatches[i]->setIcon(swatchIcon(colour));
    }
    selectPenColour(m_penColourIndex);
}

void InkView::showZoom(double zoom)
{
    const QSignalBlocker blocker(m_zoomCombo);
    int index = -1;
    for (int i = 0; i < 3; ++i)
        if (qFuzzyCompare(m_zoomCombo->itemData(i).toDouble(), zoom))
            index = i;
    if (index < 0) { // a pinch or Ctrl+wheel: a fourth entry shows where it is
        if (m_zoomCombo->count() == 3)
            m_zoomCombo->addItem(QString());
        m_zoomCombo->setItemText(3, QString::number(qRound(zoom * 100)) + QLatin1Char('%'));
        index = 3;
    } else if (m_zoomCombo->count() > 3) {
        m_zoomCombo->removeItem(3);
    }
    m_zoomCombo->setCurrentIndex(index);
}

void InkView::showShapeKind(ShapeKind kind)
{
    for (size_t i = 0; i < std::size(kShapeOrder); ++i)
        m_shapeKindActions[i]->setChecked(kShapeOrder[i] == kind);
    m_shapeAction->setText(shapeKindLabel(kind)); // the face shows the kind
}

void InkView::changePage(const std::function<void(DocumentSettings &)> &edit)
{
    if (m_canvas->isReadOnly())
        return;
    m_canvas->commitText();
    InkDocument &document = m_canvas->document();
    const DocumentSettings old = document.settings();
    DocumentSettings next = old;
    edit(next);
    const auto paged = [](LayoutKind layout) { return layout == LayoutKind::Fixed || layout == LayoutKind::ContinuousVertical; };
    if (paged(next.layout) && (next.layout != old.layout || next.format.width != old.format.width || next.format.height != old.format.height)) {
        // Pages are the format's size, as many as there were: the extent follows a new format or the way into a paged layout.
        const double pages = std::max(1.0, std::ceil(old.extent.height() / old.format.height - 1e-9));
        next.extent = QRectF(old.extent.topLeft(), QSizeF(next.format.width, pages * next.format.height));
    }
    if (!document.setSettings(next)) { // a size the file could not be read back with: nothing changed
        emit statusMessage(tr("That page setting cannot be used; the page is unchanged"), 6000);
        syncPageMenu();
        return;
    }
    if (next.layout == LayoutKind::ContinuousVertical)
        document.growToContain(document.contentBounds()); // a smaller page must not cut ink off the end of the document
    document.commit(); // an owner-side edit is a history step at once, or discardLiveEdits() would take it back with a cancelled erase
    m_canvas->documentReplaced();
    onEdited();
}

void InkView::addPage()
{
    InkDocument &document = m_canvas->document();
    if (m_canvas->isReadOnly() || document.pageCount() >= InkDocument::kMaxPages)
        return;
    m_canvas->commitText();
    document.addPage();
    document.commit();
    m_canvas->documentReplaced();
    onEdited();
}

void InkView::syncPageMenu()
{
    const DocumentSettings &s = m_canvas->document().settings();
    const double widthMM = s.format.width / kPxPerMM, heightMM = s.format.height / kPxPerMM;
    int format = int(std::size(kPagePresets)) - 1; // Custom unless a preset's size
    for (int i = 0; i + 1 < int(std::size(kPagePresets)); ++i)
        if (std::abs(kPagePresets[i].widthMM - widthMM) < 0.05 && std::abs(kPagePresets[i].heightMM - heightMM) < 0.05)
            format = i;
    const auto show = [](QComboBox *combo, const QVariant &data) {
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(std::max(0, combo->findData(data)));
    };
    const auto showValue = [](QDoubleSpinBox *spin, double value) {
        const QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    show(m_pageFormat, QLatin1String(kPagePresets[format].key));
    showValue(m_pageWidth, widthMM);
    showValue(m_pageHeight, heightMM);
    show(m_pageLayout, layoutName(s.layout));
    show(m_pagePattern, patternName(s.background.pattern));
    showValue(m_patternSize, s.background.patternSize.width());
    for (QToolButton *swatch : std::as_const(m_pageSwatches))
        swatch->setChecked(swatch->property("color").value<QColor>() == s.background.color);
    const bool paged = s.layout == LayoutKind::Fixed || s.layout == LayoutKind::ContinuousVertical;
    m_addPageAction->setEnabled(paged && !m_canvas->isReadOnly());
}

void InkView::applySettings(const Settings &settings)
{
    const bool mouse = settings.value(QLatin1String(kDrawWithMouseKey)).toBool();
    m_canvas->setDrawWithMouse(mouse);
    m_mouseAction->setChecked(mouse); // the Settings row and the toolbar toggle are one setting
    m_canvas->setRawInput(settings.value(QStringLiteral("ink.rawInput")).toBool());
    const QVariant mb = settings.value(QStringLiteral("ink.tileCacheMB")); // R5; unregistered (a bare test) keeps the default
    m_canvas->setTileCacheMB(mb.isValid() ? mb.toInt() : TileCache::kDefaultMaxBytes / (1024 * 1024));
    if (const int eraser = settings.value(QStringLiteral("ink.eraserWidth")).toInt(); eraser > 0)
        m_canvas->setEraserWidth(eraser);
    applyStoredChoices(settings);
}

/// The eraser mode, the select shape and the shape kind this and earlier sessions ended with.
void InkView::applyStoredChoices(const Settings &settings)
{
    if (const QString mode = settings.text(QLatin1String(kEraserModeKey)); !mode.isEmpty())
        m_canvas->setEraserMode(mode == QLatin1String("split") ? InkCanvas::EraserMode::Split : InkCanvas::EraserMode::Stroke);
    if (const QString shape = settings.text(QLatin1String(kSelectShapeKey)); !shape.isEmpty())
        m_canvas->setSelectShape(shape == QLatin1String("rect") ? InkCanvas::SelectShape::Rect : InkCanvas::SelectShape::Lasso);
    bool ok = false;
    const ShapeKind kind = shapeKindFromName(settings.text(QLatin1String(kShapeKindKey)), &ok);
    if (ok && std::find(std::begin(kShapeOrder), std::end(kShapeOrder), kind) != std::end(kShapeOrder))
        m_canvas->setShapeKind(kind);
}

void InkView::onEdited()
{
    // A page layout is a column of pages centred in the view; a sheet starts at the left edge, so growing it
    // left or up can be followed by scrolling (a centred widget that gets wider moves by half the growth).
    const auto layout = m_canvas->document().settings().layout;
    const bool sheet = layout == LayoutKind::SemiInfinite || layout == LayoutKind::Infinite;
    const Qt::Alignment want = (sheet ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignTop;
    if (m_area->alignment() != want)
        m_area->setAlignment(want);
    m_undoAction->setEnabled(m_canvas->document().canUndo() && !m_readOnly);
    m_redoAction->setEnabled(m_canvas->document().canRedo() && !m_readOnly);
    if (m_canvas->document().settings().background.color != m_swatchPage)
        refreshSwatches(); // a new page colour (a page change, an undo, a load): the ink swatches follow it
    syncPageMenu();
    const bool modified = isModified();
    if (modified != m_lastModified) {
        m_lastModified = modified;
        emit modificationChanged(modified);
    }
}

InkView::DiskStamp InkView::stampOf(const QString &path)
{
    const QFileInfo info(path); // a new object: stats the file now
    return info.exists() ? DiskStamp{info.size(), info.lastModified().toMSecsSinceEpoch()} : DiskStamp{};
}

bool InkView::load(const QString &path, QString *error)
{
    if (m_settings)
        applySettings(*m_settings); // R5: the tile budget is read when a file opens
    if (m_loadWatcher) { // a parse of an older load is still running: its result is not wanted
        m_loadWatcher->disconnect(this);
        m_loadWatcher->deleteLater();
        m_loadWatcher = nullptr;
        m_loadingLabel->setVisible(false);
    }
    const DiskStamp stamp = stampOf(path); // before reading: a change during the read is seen at the next save
    if (stamp.size > kOffThreadParseBytes && stamp.size <= kMaxInflatedBytes) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = tr("%1 could not be opened: %2").arg(QFileInfo(path).fileName(), file.errorString());
            return false;
        }
        const QByteArray bytes = file.readAll(); // the read is cheap, the parse is not (about ten times the file in time and memory)
        const QString name = QFileInfo(path).fileName();
        m_loadingLabel->setVisible(true);
        m_canvas->setReadOnly(true); // no pen on a document that is about to be replaced
        m_loadWatcher = new QFutureWatcher<Parsed>(this);
        connect(m_loadWatcher, &QFutureWatcherBase::finished, this, [this, path, stamp, name] {
            Parsed parsed = m_loadWatcher->result();
            m_loadWatcher->deleteLater();
            m_loadWatcher = nullptr;
            m_loadingLabel->setVisible(false);
            if (!parsed.result.ok) {
                m_stamp = stamp; // the unreadable file is looked at once, not at every autosave (as in resolveExternalChange)
                emit statusMessage(parsed.result.error, 8000);
                if (m_path.isEmpty()) { // nothing was ever shown: a blank tab that takes a pen and cannot save is a trap
                    m_loadFailed = true;
                    m_canvas->setReadOnly(true);
                    setToolsEnabled(false);
                    m_errorLabel->setText(parsed.result.error);
                    m_errorLabel->setVisible(true);
                } else {
                    m_canvas->setReadOnly(m_readOnly); // the document it had before this load
                }
                return;
            }
            m_canvas->document() = std::move(parsed.document);
            adopt(path, stamp, parsed.result);
        });
        m_loadWatcher->setFuture(QtConcurrent::run([bytes, name] {
            Parsed parsed;
            parsed.result = readInk(bytes, parsed.document, name);
            return parsed;
        }));
        return true;
    }
    InkReadResult result;
    if (!loadInkFile(path, m_canvas->document(), &result)) {
        m_canvas->setReadOnly(m_readOnly || m_loadFailed); // an interrupted big load had made it read-only for its parse
        if (error) *error = result.error;
        return false;
    }
    adopt(path, stamp, result);
    return true;
}

/// The document is in the canvas: take over what the file said about it.
void InkView::adopt(const QString &path, const DiskStamp &stamp, const InkReadResult &result)
{
    m_path = path;
    m_stamp = stamp;
    m_loadFailed = false;
    m_errorLabel->setVisible(false);
    m_readOnly = result.readOnly;
    m_canvas->setReadOnly(m_readOnly); // not setEnabled(false): a disabled widget would swallow wheel and touch pan too
    setToolsEnabled(!m_readOnly);
    m_readOnlyLabel->setVisible(m_readOnly);
    if (m_readOnly)
        emit statusMessage(tr("%1 was written by a newer Notolith; opened read-only").arg(QFileInfo(path).fileName()), 6000);
    {
        const QSignalBlocker noCompensation(m_canvas); // a different document is not a shift of the old one: no scrolling to compensate
        m_canvas->documentReplaced();
    }
    onEdited(); // a reload of a modified view clears the mark: the signal goes out from here
}

/// "<name> (conflict yyyy-MM-dd HHmmss).ink" beside the file; the sync engine's own copies are
/// "(sync conflict, <device>, <stamp>)" but a module has no device ID. A second one in the same
/// second gets a counter.
QString InkView::conflictPathFor(const QString &path)
{
    const QFileInfo info(path);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmmss"));
    const QDir dir = info.dir();
    QString candidate = QStringLiteral("%1 (conflict %2).ink").arg(info.completeBaseName(), stamp);
    for (int n = 2; dir.exists(candidate); ++n)
        candidate = QStringLiteral("%1 (conflict %2 %3).ink").arg(info.completeBaseName(), stamp).arg(n);
    return dir.filePath(candidate);
}

/// The file changed since this view read or wrote it (another machine's sync, another tab): never
/// overwrite it. Nothing local to lose: show the disk version. Local edits: keep them in a conflict
/// file, then show the disk version.
bool InkView::resolveExternalChange(QString *error)
{
    const QString name = QFileInfo(m_path).fileName();
    QString conflict;
    if (isModified()) {
        conflict = conflictPathFor(m_path);
        if (!Vault::writeBytes(conflict, writeInk(m_canvas->document()), error))
            return false; // the edits stay in the view; the disk file is untouched
    }
    const DiskStamp now = stampOf(m_path);
    QString why;
    if (!load(m_path, &why)) {
        m_stamp = now; // the unreadable file is looked at once, not at every autosave
        if (error)
            *error = conflict.isEmpty() ? tr("%1 changed on disk and cannot be read: %2").arg(name, why)
                                        : tr("%1 changed on disk and cannot be read (%2); your edits are in %3")
                                              .arg(name, why, QFileInfo(conflict).fileName());
        return false;
    }
    emit statusMessage(conflict.isEmpty()
                           ? tr("%1 changed on disk: reloaded").arg(name)
                           : tr("%1 changed on disk while you were editing it: your version is in %2")
                                 .arg(name, QFileInfo(conflict).fileName()),
                       8000);
    return true;
}

bool InkView::save(QString *error)
{
    if (m_loadWatcher) { // the canvas holds the document the load is about to replace
        if (error) *error = tr("still loading");
        return false;
    }
    if (m_readOnly) {
        if (error) *error = tr("read-only: written by a newer Notolith");
        return false;
    }
    if (m_path.isEmpty()) { // nothing loaded: there is no file to write
        if (error) *error = tr("no file");
        return false;
    }
    m_canvas->commitText(); // a box still being typed is part of what is saved
    if (const DiskStamp now = stampOf(m_path); now.size >= 0 && now != m_stamp)
        return resolveExternalChange(error);
    // The vault's atomic replace (spec section 5): writeInk() builds the bytes, the host writes them.
    if (!Vault::writeBytes(m_path, writeInk(m_canvas->document()), error))
        return false;
    m_stamp = stampOf(m_path);
    m_canvas->document().markSaved();
    onEdited();
    return true;
}

/// The theme the text on a page is drawn in. It is the page's, not the app's: the page is paper (white by default,
/// whatever the app theme is), so a dark app theme would put light text on it. `clear` leaves the background
/// transparent (a picture over the page, so the ruling shows through); otherwise it is the page's colour (the editor
/// over a box covers what is under it). The app's font overrides do carry over.
Theme InkView::paperTheme(bool clear) const
{
    const QColor page = m_canvas->document().settings().background.color;
    Theme paper = Theme::builtin(page.lightness() < 128 ? Theme::Mode::Dark : Theme::Mode::Light);
    paper.setToken(QStringLiteral("--background-primary"), clear ? QStringLiteral("transparent") : page.name());
    if (m_hostTheme)
        paper.setFontOverrides(m_hostTheme->interfaceFontOverride(), m_hostTheme->textFontOverride(), m_hostTheme->monospaceFontOverride());
    return paper;
}

/// A text box is drawn by the host's note editor, as a canvas text card is (libs/ext/canvas/CanvasRender.cpp,
/// makeContentEditor): Reading mode, no scroll bars, the Markdown parsed before the grab, no padding and no readable
/// line length (a 700 px column would centre the text of a wide box). The widget is laid out at the box's size in
/// document px and painted onto a pixmap of size * scale device px, so the text is vector-sharp at any zoom.
/// Runs on the UI thread only (R5): a widget grab.
QPixmap InkView::renderText(const QString &markdown, const QSizeF &size, qreal scale) const
{
    const QSize box(std::max(1, qRound(size.width())), std::max(1, qRound(size.height())));
    MarkdownEditor editor;
    editor.setTheme(paperTheme(true));
    editor.setMode(MarkdownEditor::Mode::Reading);
    editor.setFrameShape(QFrame::NoFrame);
    // The app's style sheet paints a background under every scroll area; this one is cleared so the page shows through.
    editor.setStyleSheet(QStringLiteral("QAbstractScrollArea, QAbstractScrollArea > QWidget { background: transparent; border: none; }"));
    editor.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor.setViewportPadding(0, 0);
    editor.setReadableLineLength(false);
    editor.resize(box);
    editor.setNoteText(markdown);
    editor.finishParse(); // a long box parses on the pool otherwise: take it now rather than grabbing blank text
    QPixmap picture(QSize(std::max(1, qRound(box.width() * scale)), std::max(1, qRound(box.height() * scale))));
    picture.setDevicePixelRatio(scale);
    picture.fill(Qt::transparent);
    editor.render(&picture, QPoint(), QRegion(), QWidget::DrawChildren); // not DrawWindowBackground
    return picture;
}

/// The editor over a box while it is typed in: the same editor, in live preview, laid out like the picture (no padding,
/// the page's colour behind it, a thin frame) so that the text does not move when the edit ends.
QWidget *InkView::makeTextEditor(const QString &markdown) const
{
    const QColor page = m_canvas->document().settings().background.color;
    auto *editor = new MarkdownEditor;
    editor->setTheme(paperTheme(false));
    editor->setMode(MarkdownEditor::Mode::LivePreview);
    editor->setStyleSheet(QStringLiteral("QAbstractScrollArea { background: %1; border: 1px solid %2; } QAbstractScrollArea > QWidget { background: %1; border: none; }")
                              .arg(page.name(), palette().highlight().color().name()));
    editor->setViewportPadding(0, 0);
    editor->setReadableLineLength(false);
    editor->setNoteText(markdown);
    editor->setCursorOffset(markdown.size());
    return editor;
}

void InkView::setTheme(const Theme &theme)
{
    m_hostTheme = theme;
    m_canvas->invalidateTextCache(); // the fonts may have changed
    QPalette palette = m_area->viewport()->palette();
    palette.setColor(QPalette::Window, theme.color(QStringLiteral("--background-secondary")));
    m_area->viewport()->setPalette(palette);
    m_area->viewport()->setAutoFillBackground(true);
}

} // namespace noteAppC::ink
