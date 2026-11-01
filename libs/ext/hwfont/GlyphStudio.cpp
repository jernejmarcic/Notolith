// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "GlyphStudio.h"

#include <cmath>

#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTabletEvent>
#include <QVBoxLayout>

#include "Settings.h"
#include "Theme.h"
#include "Vault.h"
#include "InkInput.h"

namespace noteAppC::hwfont {

namespace {
constexpr double kBarrelClickSlopPx = 4.0; ///< a barrel press that moves further than this is not a click
constexpr int kStatusMs = 4000;

QString glyphText(char32_t codepoint) { return QString::fromUcs4(&codepoint, 1); }

QString vaultRootOf(const ModuleHost &host) { return host.vault && host.vault->isValid() ? host.vault->root() : QString(); }

int settingOr(const ModuleHost &host, const char *key, int fallback)
{
    const int value = host.settings ? host.settings->integer(QLatin1String(key)) : 0;
    return value >= 1 ? value : fallback;
}
} // namespace

/// The ascender, x-height and baseline lines over the cell. A child of the canvas that lets every
/// pointer event through, so the pen reaches the canvas underneath.
class GlyphStudio::Guides : public QWidget {
public:
    Guides(const GlyphMetrics &metrics, QWidget *canvas) : QWidget(canvas), m_metrics(metrics)
    {
        setObjectName(QStringLiteral("glyphGuides"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        resize(canvas->size());
    }
    void setColor(const QColor &color)
    {
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(QPen(m_color, 1, Qt::DashLine));
        for (const double y : {m_metrics.ascender, m_metrics.xHeight, m_metrics.baseline})
            painter.drawLine(QPointF(0, y), QPointF(width(), y));
    }

private:
    GlyphMetrics m_metrics;
    QColor m_color{128, 128, 128, 160};
};

GlyphStudio::GlyphStudio(ModuleHost &host, const QString &fontName, QWidget *parent)
    : ModuleView(parent), m_store(vaultRootOf(host), fontName), m_metrics(GlyphStore::defaultMetrics()),
      m_set(defaultGlyphSet(settingOr(host, "hwfont.samplesPerLetter", 3), settingOr(host, "hwfont.samplesPerPunctuation", 1))),
      m_canvas(new ink::InkCanvas), m_glyphLabel(new QLabel), m_countLabel(new QLabel), m_errorLabel(new QLabel),
      m_list(new QListWidget), m_doneColor(128, 128, 128), m_vaultRoot(vaultRootOf(host)), m_fontName(fontName),
      m_settings(host.settings)
{
    setObjectName(QStringLiteral("glyphStudio"));
    m_canvas->setObjectName(QStringLiteral("glyphCanvas"));
    m_canvas->document().reset(GlyphStore::samplePage(), {}, {});
    m_canvas->documentReplaced();
    m_canvas->setFixedSize(m_canvas->sizeHint());
    m_canvas->setDrawWithMouse(true); // a drawing surface: ink.drawWithMouse is for the notes, not for the cell
    setFocusProxy(m_canvas);          // the host's setFocus() on the tab lands on the cell, where the keys are
    m_canvas->installEventFilter(this);
    // F22: a glyph cell has no selection, so the held barrel button must not turn a stroke into a lasso.
    m_canvas->setHeldLassoEnabled(false);
    m_guides = new Guides(m_metrics, m_canvas);
    connect(m_canvas, &ink::InkCanvas::documentEdited, this, &GlyphStudio::onEdited);

    m_glyphLabel->setObjectName(QStringLiteral("glyphTarget"));
    QFont big = m_glyphLabel->font();
    big.setPointSize(40);
    m_glyphLabel->setFont(big);
    m_countLabel->setObjectName(QStringLiteral("glyphProgress"));
    m_errorLabel->setObjectName(QStringLiteral("glyphError"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_errorLabel->hide();

    const auto addButton = [this](const char *name, const QString &text, const QString &tip, auto slot) {
        auto *button = new QPushButton(text);
        button->setObjectName(QLatin1String(name));
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus); // the canvas keeps the keys: Enter, Backspace, Ctrl+Z
        connect(button, &QPushButton::clicked, this, slot);
        connect(button, &QPushButton::clicked, m_canvas, qOverload<>(&QWidget::setFocus)); // the keys stay with the cell
        return button;
    };
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(addButton("glyphSaveButton", tr("Save"), tr("Save this sample and move on (Enter)"),
                                 [this] { saveCurrentAndAdvance(); }));
    buttons->addWidget(addButton("glyphDiscardButton", tr("Discard"), tr("Clear the cell without saving (Backspace)"),
                                 [this] { discardCurrent(); }));
    buttons->addWidget(addButton("glyphUndoButton", tr("Undo stroke"), tr("Undo the last stroke (Ctrl+Z)"),
                                 [this] { m_canvas->undo(); }));
    buttons->addWidget(addButton("glyphRedoButton", tr("Redo"), tr("Redo the stroke (Ctrl+Shift+Z or Ctrl+Y)"),
                                 [this] { m_canvas->redo(); }));
    buttons->addWidget(addButton("glyphSkipButton", tr("Skip"), tr("Next glyph without saving (Page Down)"),
                                 [this] { skipToNext(); }));
    buttons->addWidget(addButton("glyphNewFontButton", tr("New font..."), tr("Start another handwriting font"),
                                 [this] { startNewFont(); }));
    buttons->addStretch(1);

    auto *header = new QHBoxLayout;
    header->addWidget(m_glyphLabel);
    header->addWidget(m_countLabel, 1);
    auto *left = new QVBoxLayout;
    left->addLayout(header);
    left->addWidget(m_errorLabel);
    left->addWidget(m_canvas, 0, Qt::AlignLeft);
    left->addLayout(buttons);
    left->addStretch(1);

    m_list->setObjectName(QStringLiteral("glyphList"));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setFixedWidth(150);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        retarget(char32_t(item->data(Qt::UserRole).toUInt()));
    });

    auto *layout = new QHBoxLayout(this);
    layout->addLayout(left, 1);
    layout->addWidget(m_list);
    m_canvas->setFocus();

    refreshList();
    chooseStartTarget();
    if (vaultRootOf(host).isEmpty())
        showError(tr("No vault is open: the samples have nowhere to go."));
    else if (!m_store.isValid())
        showError(tr("\"%1\" is not a usable font name; choose another in Settings (hwfont.fontName).").arg(fontName));
}

GlyphStudio::~GlyphStudio()
{
    // As InkView: what ~QWidget's hide and focus-out make the canvas do must not reach this half-destroyed studio.
    m_canvas->removeEventFilter(this);
    m_canvas->disconnect(this);
    m_canvas->discardTextEdit();
}

/// The first glyph to ask for is the first letter that needs samples: a font is written from "a".
void GlyphStudio::chooseStartTarget()
{
    m_target = nextNeedingMore(U'a' - 1);
    if (m_target == 0 && !m_set.empty())
        m_target = m_set.front().codepoint;
    showTarget();
}

void GlyphStudio::startNewFont()
{
    std::optional<QString> asked;
    if (m_askFontName) {
        asked = m_askFontName();
    } else {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("New font"), tr("Name of the new font"), QLineEdit::Normal, QString(), &ok);
        if (ok)
            asked = text;
    }
    if (!asked)
        return; // cancelled
    if (m_vaultRoot.isEmpty()) {
        showError(tr("No vault is open: the samples have nowhere to go."));
        return;
    }
    const QString name = asked->trimmed();
    if (!GlyphStore::isValidFontName(name)) {
        showError(tr("\"%1\" is not a usable font name: use letters, digits and spaces, no slashes, no \"..\", not starting with a dot.").arg(*asked));
        return;
    }
    m_fontName = name;
    m_store = GlyphStore(m_vaultRoot, name);
    if (m_settings) {
        m_settings->setValue(QStringLiteral("hwfont.fontName"), name);
        if (!m_settings->save())
            emit statusMessage(tr("Could not store the font name in the settings"), 6000);
    }
    showError(QString());
    refreshList();
    chooseStartTarget();
    emit statusMessage(tr("Writing the font \"%1\"").arg(name), kStatusMs);
}

bool GlyphStudio::needsMore(char32_t codepoint) const
{
    const auto it = std::find_if(m_set.begin(), m_set.end(), [codepoint](const GlyphTarget &t) { return t.codepoint == codepoint; });
    const auto have = m_have.find(codepoint);
    return it != m_set.end() && (have == m_have.end() ? 0 : have->second) < it->wanted;
}

/// The next glyph after `after` (by code point, wrapping round) that wants more samples; 0 when none does.
char32_t GlyphStudio::nextNeedingMore(char32_t after) const
{
    char32_t first = 0;
    for (const GlyphTarget &target : m_set) {
        if (!needsMore(target.codepoint))
            continue;
        if (target.codepoint > after)
            return target.codepoint;
        if (first == 0)
            first = target.codepoint;
    }
    return first;
}

void GlyphStudio::refreshList()
{
    m_have.clear();
    m_list->clear();
    for (const auto &[target, have] : m_store.progress(m_set)) {
        m_have[target.codepoint] = have;
        auto *item = new QListWidgetItem(QStringLiteral("%1 %2/%3").arg(glyphText(target.codepoint)).arg(have).arg(target.wanted));
        item->setData(Qt::UserRole, uint(target.codepoint));
        if (have >= target.wanted)
            item->setForeground(m_doneColor);
        m_list->addItem(item);
        if (target.codepoint == m_target)
            m_list->setCurrentItem(item);
    }
}

void GlyphStudio::showTarget()
{
    m_glyphLabel->setText(glyphText(m_target));
    const auto it = std::find_if(m_set.begin(), m_set.end(), [this](const GlyphTarget &t) { return t.codepoint == m_target; });
    const int have = m_have.count(m_target) ? m_have.at(m_target) : 0;
    m_countLabel->setText(it == m_set.end() ? QString()
                          : have >= it->wanted ? tr("extra sample (%1 of %2 done)").arg(have).arg(it->wanted)
                                               : tr("%1 of %2").arg(have + 1).arg(it->wanted));
    for (int i = 0; i < m_list->count(); ++i) {
        if (char32_t(m_list->item(i)->data(Qt::UserRole).toUInt()) == m_target) {
            m_list->setCurrentRow(i);
            m_list->scrollToItem(m_list->item(i));
            break;
        }
    }
}

void GlyphStudio::showError(const QString &text)
{
    m_errorLabel->setText(text);
    m_errorLabel->setVisible(!text.isEmpty());
}

void GlyphStudio::onEdited()
{
    const bool modified = isModified();
    if (modified != m_lastModified) {
        m_lastModified = modified;
        emit modificationChanged(modified);
    }
}

void GlyphStudio::clearCell()
{
    m_canvas->document().reset(GlyphStore::samplePage(), {}, {});
    m_canvas->documentReplaced();
    onEdited();
}

bool GlyphStudio::commitCell(QString *error)
{
    if (!isModified())
        return true;
    QString why;
    if (!m_store.saveSample(m_target, m_canvas->document(), m_metrics, &why)) {
        showError(why);
        emit statusMessage(tr("Could not save %1: %2").arg(glyphText(m_target), why), 8000);
        if (error)
            *error = why;
        return false; // the strokes stay in the cell
    }
    showError(QString());
    clearCell();
    refreshList();
    return true;
}

bool GlyphStudio::save(QString *error)
{
    return commitCell(error);
}

void GlyphStudio::saveCurrentAndAdvance()
{
    if (!isModified()) {
        emit statusMessage(tr("Write the letter first"), kStatusMs);
        return;
    }
    const char32_t saved = m_target;
    if (!commitCell(nullptr))
        return;
    const auto it = std::find_if(m_set.begin(), m_set.end(), [saved](const GlyphTarget &t) { return t.codepoint == saved; });
    emit statusMessage(tr("Saved %1 %2/%3").arg(glyphText(saved)).arg(m_have[saved]).arg(it == m_set.end() ? 0 : it->wanted), kStatusMs);
    if (const char32_t next = nextNeedingMore(saved))
        m_target = next;
    else
        emit statusMessage(tr("Every glyph has its samples"), kStatusMs);
    showTarget();
}

void GlyphStudio::discardCurrent()
{
    clearCell();
}

void GlyphStudio::retarget(char32_t codepoint)
{
    m_target = codepoint;
    showTarget();
}

void GlyphStudio::skipToNext()
{
    if (const char32_t next = nextNeedingMore(m_target))
        retarget(next);
}

void GlyphStudio::setTheme(const Theme &theme)
{
    const QColor faint = theme.color(QStringLiteral("--text-faint"));
    m_guides->setColor(faint);
    m_doneColor = faint;
    refreshList();
}

/// The pen's barrel click: a press with kHeldLassoButton and its release within the slop saves the cell.
/// Both are consumed, so the canvas never sees them and draws nothing; a press that travels further is
/// consumed too (no lasso in a cell).
bool GlyphStudio::handleBarrel(QTabletEvent *event)
{
    switch (event->type()) {
    case QEvent::TabletPress:
        if (!(event->buttons() & ink::kHeldLassoButton))
            return false;
        m_barrelDown = true;
        m_barrelMoved = false;
        m_barrelStart = event->position();
        event->accept();
        return true;
    case QEvent::TabletMove:
        if (!m_barrelDown)
            return false;
        if (std::hypot(event->position().x() - m_barrelStart.x(), event->position().y() - m_barrelStart.y()) > kBarrelClickSlopPx)
            m_barrelMoved = true;
        event->accept();
        return true;
    case QEvent::TabletRelease: {
        if (!m_barrelDown)
            return false;
        m_barrelDown = false;
        event->accept();
        if (!m_barrelMoved)
            saveCurrentAndAdvance();
        return true;
    }
    default:
        return false;
    }
}

void GlyphStudio::showEvent(QShowEvent *event)
{
    ModuleView::showEvent(event);
    m_canvas->setFocus();
}

bool GlyphStudio::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_canvas)
        return ModuleView::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        return handleBarrel(static_cast<QTabletEvent *>(event));
    case QEvent::TabletLeaveProximity:
        m_barrelDown = false;
        return false;
    case QEvent::KeyPress: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->modifiers() & ~Qt::KeypadModifier)
            return false; // Ctrl+Z and friends are the canvas's own
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            saveCurrentAndAdvance();
            return true;
        case Qt::Key_Backspace:
            discardCurrent();
            return true;
        case Qt::Key_PageDown:
            skipToNext();
            return true;
        case Qt::Key_Escape:
            return true; // does nothing, and does not reach the host either
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

} // namespace noteAppC::hwfont
