// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "PdfView.h"

#include "FindBar.h"
#include "PdfArgs.h"
#include "PdfExtract.h"
#include "PdfPagesArea.h"

#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSplitter>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace noteAppC {

namespace {

QIcon colorSwatchIcon(const QColor &color)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(pixmap.rect().adjusted(1, 1, -1, -1));
    return QIcon(pixmap);
}

} // namespace

PdfView::PdfView(QWidget *parent)
    : ModuleView(parent)
{
    m_pages = new PdfPagesArea(this);
    connect(m_pages, &PdfPagesArea::modificationChanged, this, &ModuleView::modificationChanged);
    connect(m_pages, &PdfPagesArea::currentPageChanged, this, [this](int) { refreshPageField(); });
    connect(m_pages, &PdfPagesArea::zoomChanged, this, [this](double zoom) {
        if (m_zoomLabel)
            m_zoomLabel->setText(tr("%1%").arg(qRound(zoom * 100)));
    });
    connect(m_pages, &PdfPagesArea::selectionChanged, this, &PdfView::refreshToolButtons);
    connect(m_pages, &PdfPagesArea::openExternalUrl, this,
           [](const QString &url) {
               // The web and mail only, as a note's links: a PDF's javascript:
               // or file: URI is never handed to another program.
               const QUrl target(url);
               const QString scheme = target.scheme().toLower();
               if (scheme == QLatin1String("http") || scheme == QLatin1String("https")
                   || scheme == QLatin1String("mailto"))
                   QDesktopServices::openUrl(target);
           });
    connect(m_pages, &PdfPagesArea::findStatusChanged, this, [this](int current, int total) {
        if (m_findBar)
            m_findBar->setStatus(current, total, true);
    });

    buildToolbar();
    buildOutlinePanel();

    m_findBar = new FindBar(this);
    m_findBar->setReplaceShown(false); // a PDF's text is read-only here - no replace
    m_findBar->hide();
    connect(m_findBar, &FindBar::queryChanged, this, &PdfView::applyFindQuery);
    connect(m_findBar, &FindBar::stepped, this, [this](int direction) { m_pages->findStep(direction); });
    connect(m_findBar, &FindBar::dismissed, this, [this] {
        m_findBar->hide();
        m_pages->findClear();
        m_pages->setFocus();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolbar);
    layout->addWidget(m_splitter, 1);

    // The find bar floats over the page canvas, top-right, the same corner
    // FindBar occupies over MarkdownEditor.
    m_findBar->setParent(m_pages->viewport());
}

void PdfView::buildToolbar()
{
    m_toolbar = new QToolBar(this);
    m_toolbar->setMovable(false);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toolbar->setIconSize(QSize(16, 16));

    m_pageField = new QLineEdit(m_toolbar);
    m_pageField->setFixedWidth(48);
    m_pageField->setAlignment(Qt::AlignCenter);
    connect(m_pageField, &QLineEdit::returnPressed, this, [this] {
        bool ok = false;
        const int page = m_pageField->text().toInt(&ok);
        if (ok)
            m_pages->goToPage(page - 1);
        refreshPageField();
        m_pages->setFocus();
    });
    m_toolbar->addWidget(m_pageField);
    m_pageCountLabel = new QLabel(m_toolbar);
    m_toolbar->addWidget(m_pageCountLabel);
    m_toolbar->addSeparator();

    m_toolbar->addAction(tr("PgUp"), this, [this] { m_pages->previousPage(); })->setToolTip(tr("Previous page"));
    m_toolbar->addAction(tr("PgDn"), this, [this] { m_pages->nextPage(); })->setToolTip(tr("Next page"));
    m_toolbar->addSeparator();

    m_toolbar->addAction(tr("−"), this, [this] { m_pages->zoomOut(); })->setToolTip(tr("Zoom out"));
    m_zoomLabel = new QLabel(tr("100%"), m_toolbar);
    m_zoomLabel->setFixedWidth(40);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_toolbar->addWidget(m_zoomLabel);
    m_toolbar->addAction(tr("+"), this, [this] { m_pages->zoomIn(); })->setToolTip(tr("Zoom in"));
    m_toolbar->addAction(tr("Fit width"), this, [this] { m_pages->fitWidth(); });
    m_toolbar->addAction(tr("Fit page"), this, [this] { m_pages->fitPage(); });
    m_toolbar->addSeparator();

    auto *tools = new QActionGroup(this);
    tools->setExclusive(true);
    const auto addTool = [&](const QString &text, PdfPagesArea::Tool tool) {
        QAction *action = m_toolbar->addAction(text);
        action->setCheckable(true);
        action->setActionGroup(tools);
        connect(action, &QAction::triggered, this, [this, tool] { m_pages->setTool(tool); });
        return action;
    };
    m_toolSelect = addTool(tr("Select"), PdfPagesArea::Tool::Select);
    m_toolSelect->setChecked(true);
    m_toolHighlight = addTool(tr("Highlight"), PdfPagesArea::Tool::Highlight);
    m_toolUnderline = addTool(tr("Underline"), PdfPagesArea::Tool::Underline);
    m_toolStrikeOut = addTool(tr("Strike"), PdfPagesArea::Tool::StrikeOut);
    m_toolInk = addTool(tr("Ink"), PdfPagesArea::Tool::Ink);
    m_toolNote = addTool(tr("Note"), PdfPagesArea::Tool::Note);
    m_toolRectangle = addTool(tr("Box"), PdfPagesArea::Tool::Rectangle);

    auto *colorButton = new QToolButton(m_toolbar);
    colorButton->setText(tr("Colour"));
    colorButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    colorButton->setIcon(colorSwatchIcon(m_pages->annotationColor()));
    colorButton->setPopupMode(QToolButton::InstantPopup);
    auto *colorMenu = new QMenu(colorButton);
    static const QList<QColor> kPalette = {QColor(0xFF, 0xEB, 0x3B), QColor(0x8B, 0xC3, 0x4A),
                                           QColor(0x4F, 0xC3, 0xF7), QColor(0xFF, 0x8A, 0x80),
                                           QColor(0xCE, 0x93, 0xD8)};
    for (const QColor &color : kPalette) {
        QAction *action = colorMenu->addAction(colorSwatchIcon(color), color.name());
        connect(action, &QAction::triggered, this, [this, color, colorButton] {
            m_pages->setAnnotationColor(color);
            colorButton->setIcon(colorSwatchIcon(color));
        });
    }
    colorButton->setMenu(colorMenu);
    m_toolbar->addWidget(colorButton);
    m_toolbar->addSeparator();

    m_outlineToggle = m_toolbar->addAction(tr("Contents"));
    m_outlineToggle->setCheckable(true);
    connect(m_outlineToggle, &QAction::toggled, this, [this](bool on) {
        if (m_outline)
            m_outline->setVisible(on);
        if (on)
            refreshOutline();
    });
}

void PdfView::buildOutlinePanel()
{
    m_splitter = new QSplitter(this);
    m_outline = new QTreeWidget(m_splitter);
    m_outline->setHeaderHidden(true);
    m_outline->setVisible(false);
    connect(m_outline, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const int page = item->data(0, Qt::UserRole).toInt();
        const double top = item->data(0, Qt::UserRole + 1).toDouble();
        if (page >= 0)
            m_pages->goToPage(page, top);
    });
    m_splitter->addWidget(m_outline);
    m_splitter->addWidget(m_pages);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({220, 800});
}

void PdfView::refreshOutline()
{
    if (!m_outline)
        return;
    m_outline->clear();
    const std::function<void(QTreeWidgetItem *, const QList<PdfOutlineItem> &)> add =
        [&](QTreeWidgetItem *parent, const QList<PdfOutlineItem> &items) {
            for (const PdfOutlineItem &entry : items) {
                auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_outline);
                item->setText(0, entry.title);
                item->setData(0, Qt::UserRole, entry.page);
                item->setData(0, Qt::UserRole + 1, entry.top);
                add(item, entry.children);
            }
        };
    add(nullptr, m_pages->outline());
    m_outline->expandAll();
}

void PdfView::refreshPageField()
{
    if (m_pageField)
        m_pageField->setText(QString::number(m_pages->currentPage() + 1));
    if (m_pageCountLabel)
        m_pageCountLabel->setText(tr("of %1").arg(m_pages->pageCount()));
}

void PdfView::refreshToolButtons()
{
    // Nothing beyond the checked tool today - kept as its own slot (rather
    // than inlined at the connect site) because the colour button's swatch
    // updating to match a newly selected annotation's own colour is the
    // obvious next thing here, when that is asked for.
}

void PdfView::applyFindQuery()
{
    m_pages->findStart(m_findBar->pattern());
}

bool PdfView::load(const QString &path, QString *error)
{
    if (!m_pages->load(path, error))
        return false;
    refreshPageField();
    if (m_zoomLabel)
        m_zoomLabel->setText(tr("%1%").arg(qRound(m_pages->zoom() * 100)));
    if (m_outlineToggle)
        m_outlineToggle->setEnabled(!m_pages->outline().isEmpty());
    if (m_outline)
        m_outline->clear();
    return true;
}

bool PdfView::save(QString *error)
{
    return m_pages->save(error);
}

bool PdfView::isModified() const
{
    return m_pages->isModified();
}

QString PdfView::path() const
{
    return m_pages->path();
}

void PdfView::setFilePath(const QString &path)
{
    m_pages->setPath(path);
}

QUndoStack *PdfView::undoStack() const
{
    return m_pages->undoStack();
}

void PdfView::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_pages->setTheme(theme);
    if (m_findBar)
        m_findBar->setTheme(theme);
}

void PdfView::openFragment(const QString &subpath)
{
    // Reuses Obsidian's own `#page=` grammar (libs/render/PdfArgs.h) rather
    // than reparsing it - a plain `#page=N` parses to one page in `pages`.
    const PdfEmbedArgs args = parsePdfEmbedArgs(subpath);
    if (!args.ok() || args.pages.isEmpty())
        return;
    const QList<int> resolved = resolvePdfPages(args, m_pages->pageCount());
    if (!resolved.isEmpty())
        m_pages->goToPage(resolved.first());
}

bool PdfView::extractAnnotations(const QString &notePath, QString *error)
{
    const QString path = m_pages->path();
    if (path.isEmpty()) {
        if (error)
            *error = tr("no PDF open");
        return false;
    }
    const QString target = notePath.isEmpty() ? defaultAnnotationsNotePath(path) : notePath;
    return extractAnnotationsToNote(*m_pages->document(), QFileInfo(path).fileName(), target, error);
}

void PdfView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Find)) {
        m_findBar->show();
        m_findBar->raise();
        m_findBar->move(m_pages->viewport()->width() - m_findBar->sizeHint().width() - 12, 8);
        m_findBar->focusSearch();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_findBar->isVisible()) {
        m_findBar->hide();
        m_pages->findClear();
        m_pages->setFocus();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace noteAppC
