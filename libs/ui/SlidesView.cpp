// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "SlidesView.h"

#include <QKeyEvent>
#include <QLabel>
#include <QVBoxLayout>

#include "Parser.h"

namespace noteAppC {

SlidesView::SlidesView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("slidesView"));
    m_editor = new MarkdownEditor(this);
    m_editor->setMode(MarkdownEditor::Mode::Reading);
    m_editor->installEventFilter(this);
    m_counter = new QLabel(this);
    m_counter->setObjectName(QStringLiteral("slidesCounter"));
    m_counter->setAlignment(Qt::AlignRight);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 12);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_counter);
}

QStringList SlidesView::split(const QString &text)
{
    QStringList slides;
    qsizetype start = 0;
    for (const BlockSpan &block : Parser().parseAll(text).decorations.blocks) {
        if (block.kind == BlockSpan::Kind::Frontmatter) {
            start = block.range.end;
        } else if (block.kind == BlockSpan::Kind::Rule) {
            slides << text.mid(start, block.range.start - start);
            start = block.range.end;
        }
    }
    slides << text.mid(start);
    // A rule at the very start or end, or two in a row, makes no slide; the
    // blank lines around a rule are not the slide's.
    slides.removeIf([](const QString &slide) { return slide.trimmed().isEmpty(); });
    for (QString &slide : slides) {
        while (slide.startsWith(QLatin1Char('\n')))
            slide.remove(0, 1);
        while (slide.endsWith(QLatin1Char('\n')))
            slide.chop(1);
    }
    return slides;
}

void SlidesView::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_scaledFor = 0;
    applyScale();
    setStyleSheet(QStringLiteral("QWidget#slidesView { background: %1; } QLabel#slidesCounter { color: %2; }")
                      .arg(theme.color(QStringLiteral("--background-primary")).name(),
                           theme.color(QStringLiteral("--text-faint")).name()));
}

void SlidesView::setLinkResolver(std::function<QString(const QString &)> resolver)
{
    m_editor->setLinkResolver(std::move(resolver));
}

void SlidesView::setFeatures(const MarkdownEditor::Features &features)
{
    m_editor->setFeatures(features);
}

void SlidesView::present(const QStringList &slides, int first)
{
    m_slides = slides.isEmpty() ? QStringList{QString()} : slides;
    showSlide(qBound(0, first, int(m_slides.size()) - 1));
    m_editor->setFocus();
}

void SlidesView::showSlide(int index)
{
    m_current = index;
    m_editor->setNoteText(m_slides.at(index));
    m_counter->setText(QStringLiteral("%1 / %2").arg(index + 1).arg(m_slides.size()));
}

void SlidesView::applyScale()
{
    // A slide fills the screen as Obsidian's do: the text grows with the
    // view, never below the note's own size.
    const int base = m_theme.value(QStringLiteral("--font-text-size")).toInt();
    const int size = qMax(base, height() / 22);
    if (size == m_scaledFor || size <= 0)
        return;
    m_scaledFor = size;
    Theme scaled = m_theme;
    scaled.setToken(QStringLiteral("--font-text-size"), QString::number(size));
    m_editor->setTheme(scaled);
}

void SlidesView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyScale();
}

bool SlidesView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_editor || event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);
    const int last = int(m_slides.size()) - 1;
    switch (static_cast<QKeyEvent *>(event)->key()) {
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_PageDown:
    case Qt::Key_Space:
        showSlide(qMin(m_current + 1, last));
        return true;
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_PageUp:
    case Qt::Key_Backspace:
        showSlide(qMax(m_current - 1, 0));
        return true;
    case Qt::Key_Home:
        showSlide(0);
        return true;
    case Qt::Key_End:
        showSlide(last);
        return true;
    case Qt::Key_Escape:
        emit finished();
        return true;
    default:
        return QWidget::eventFilter(watched, event);
    }
}

} // namespace noteAppC
