// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "BulletGlyphs.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QSignalBlocker>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegularExpression>

#include <cmath>

namespace noteAppC {

namespace {

struct Entry {
    BulletGlyph glyph;
    const char *name;
    const char *title;
};

constexpr Entry kGlyphs[] = {
    {BulletGlyph::Dot, "dot", "Dot"},
    {BulletGlyph::Ring, "ring", "Ring"},
    {BulletGlyph::Square, "square", "Square"},
    {BulletGlyph::Diamond, "diamond", "Diamond"},
    {BulletGlyph::Dash, "dash", "Dash"},
    {BulletGlyph::Arrow, "arrow", "Arrow"},
    {BulletGlyph::Star, "star", "Star"},
    {BulletGlyph::Check, "check", "Check"},
    {BulletGlyph::Turn, "turn", "Turn arrow"},
    {BulletGlyph::AsTyped, "typed", "As typed"},
};

} // namespace

const QStringList &bulletGlyphNames()
{
    static const QStringList names = [] {
        QStringList list;
        for (const Entry &entry : kGlyphs)
            list.append(QLatin1String(entry.name));
        return list;
    }();
    return names;
}

QString bulletGlyphName(BulletGlyph glyph)
{
    for (const Entry &entry : kGlyphs) {
        if (entry.glyph == glyph)
            return QLatin1String(entry.name);
    }
    return QStringLiteral("dot");
}

QString bulletGlyphTitle(BulletGlyph glyph)
{
    for (const Entry &entry : kGlyphs) {
        if (entry.glyph == glyph)
            return QObject::tr(entry.title);
    }
    return {};
}

BulletGlyph bulletGlyphFromName(const QString &name, BulletGlyph fallback)
{
    for (const Entry &entry : kGlyphs) {
        if (name.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0)
            return entry.glyph;
    }
    return fallback;
}

BulletGlyphMap BulletGlyphMap::parse(const QString &spec)
{
    BulletGlyphMap map;
    // "-=dash,*=dot,+=diamond": entries split on a comma or whitespace.
    for (const QString &entry : spec.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        if (equals != 1)
            continue;
        const QChar marker = entry.at(0);
        map.set(marker, bulletGlyphFromName(entry.mid(2), map.glyphFor(marker)));
    }
    return map;
}

QString BulletGlyphMap::toSpec() const
{
    return QStringLiteral("-=%1,*=%2,+=%3")
        .arg(bulletGlyphName(dash), bulletGlyphName(star), bulletGlyphName(plus));
}

BulletGlyph BulletGlyphMap::glyphFor(QChar marker) const
{
    switch (marker.unicode()) {
    case u'*':
        return star;
    case u'+':
        return plus;
    default:
        return dash; // "-", and anything a parser hands over that is not a bullet
    }
}

void BulletGlyphMap::set(QChar marker, BulletGlyph glyph)
{
    switch (marker.unicode()) {
    case u'-':
        dash = glyph;
        break;
    case u'*':
        star = glyph;
        break;
    case u'+':
        plus = glyph;
        break;
    default:
        break;
    }
}

void paintBulletGlyph(QPainter &painter, BulletGlyph glyph, QChar marker, const QPointF &centre,
                      const QColor &color, const QFont &font)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    switch (glyph) {
    case BulletGlyph::Dot:
        painter.drawEllipse(centre, 2.2, 2.2);
        break;
    case BulletGlyph::Ring:
        painter.setPen(QPen(color, 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(centre, 2.6, 2.6);
        break;
    case BulletGlyph::Square:
        painter.drawRoundedRect(QRectF(centre.x() - 2.2, centre.y() - 2.2, 4.4, 4.4), 0.8, 0.8);
        break;
    case BulletGlyph::Diamond:
        painter.drawPolygon(QPolygonF({{centre.x(), centre.y() - 3.2},
                                       {centre.x() + 3.2, centre.y()},
                                       {centre.x(), centre.y() + 3.2},
                                       {centre.x() - 3.2, centre.y()}}));
        break;
    case BulletGlyph::Dash:
        // Wider than a dot is tall: a rounded bar, the way an en dash sits.
        painter.drawRoundedRect(QRectF(centre.x() - 4.5, centre.y() - 1.1, 9, 2.2), 1.1, 1.1);
        break;
    case BulletGlyph::Arrow:
        painter.drawPolygon(QPolygonF({{centre.x() - 2.6, centre.y() - 3.4},
                                       {centre.x() + 3.2, centre.y()},
                                       {centre.x() - 2.6, centre.y() + 3.4}}));
        break;
    case BulletGlyph::Star: {
        QPolygonF star;
        for (int i = 0; i < 10; ++i) {
            const qreal radius = i % 2 == 0 ? 4.0 : 1.7;
            const qreal angle = -M_PI / 2 + i * M_PI / 5;
            star << QPointF(centre.x() + radius * std::cos(angle), centre.y() + radius * std::sin(angle));
        }
        painter.drawPolygon(star);
        break;
    }
    case BulletGlyph::Check: {
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath tick;
        tick.moveTo(centre.x() - 3.2, centre.y() + 0.2);
        tick.lineTo(centre.x() - 1.0, centre.y() + 2.6);
        tick.lineTo(centre.x() + 3.4, centre.y() - 2.8);
        painter.drawPath(tick);
        break;
    }
    case BulletGlyph::Turn: {
        // U+21B3 as a path (a font's coverage of it is not guaranteed): down the
        // stem, along the bar, and an open arrowhead at its end.
        painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath turn;
        turn.moveTo(centre.x() - 3.5, centre.y() - 4.5);
        turn.lineTo(centre.x() - 3.5, centre.y() + 1.5);
        turn.lineTo(centre.x() + 3.5, centre.y() + 1.5);
        turn.moveTo(centre.x() + 1.5, centre.y() - 0.5);
        turn.lineTo(centre.x() + 3.5, centre.y() + 1.5);
        turn.lineTo(centre.x() + 1.5, centre.y() + 3.5);
        painter.drawPath(turn);
        break;
    }
    case BulletGlyph::AsTyped:
        painter.setPen(color);
        painter.setFont(font);
        painter.drawText(QRectF(centre.x() - 8, centre.y() - 10, 16, 20), Qt::AlignCenter, QString(marker));
        break;
    }
    painter.restore();
}

namespace {

constexpr QChar kMarkers[3] = {QLatin1Char('-'), QLatin1Char('*'), QLatin1Char('+')};

/// A glyph as a small icon for a drop-down item, in the widget's text colour.
QIcon glyphIcon(BulletGlyph glyph, QChar marker, const QWidget &widget)
{
    const qreal ratio = widget.devicePixelRatioF();
    QPixmap pixmap(QSize(20, 16) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paintBulletGlyph(painter, glyph, marker, QPointF(10, 8), widget.palette().color(QPalette::Text),
                     widget.font());
    return QIcon(pixmap);
}

} // namespace

BulletGlyphChooser::BulletGlyphChooser(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    for (int i = 0; i < 3; ++i) {
        // The marker as the note writes it, then what it is drawn as.
        auto *label = new QLabel(QString(kMarkers[i]), this);
        auto *combo = new QComboBox(this);
        combo->setObjectName(QStringLiteral("bulletGlyphCombo_") + QString(kMarkers[i]));
        for (const QString &name : bulletGlyphNames()) {
            const BulletGlyph glyph = bulletGlyphFromName(name, BulletGlyph::Dot);
            combo->addItem(glyphIcon(glyph, kMarkers[i], *this), bulletGlyphTitle(glyph), name);
        }
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this] { emit specChanged(spec()); });
        layout->addWidget(label);
        layout->addWidget(combo);
        m_combos[i] = combo;
    }
}

void BulletGlyphChooser::setSpec(const QString &spec)
{
    const BulletGlyphMap map = BulletGlyphMap::parse(spec);
    for (int i = 0; i < 3; ++i) {
        const QSignalBlocker blocker(m_combos[i]);
        m_combos[i]->setCurrentIndex(m_combos[i]->findData(bulletGlyphName(map.glyphFor(kMarkers[i]))));
    }
}

QString BulletGlyphChooser::spec() const
{
    BulletGlyphMap map;
    for (int i = 0; i < 3; ++i)
        map.set(kMarkers[i], bulletGlyphFromName(m_combos[i]->currentData().toString(), BulletGlyph::Dot));
    return map.toSpec();
}

QComboBox *BulletGlyphChooser::comboFor(QChar marker) const
{
    for (int i = 0; i < 3; ++i) {
        if (kMarkers[i] == marker)
            return m_combos[i];
    }
    return nullptr;
}

} // namespace noteAppC
