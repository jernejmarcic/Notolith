#include "Icons.h"

#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace noteAppC {

namespace {
QHash<QString, QIcon> &cache()
{
    static QHash<QString, QIcon> icons;
    return icons;
}
} // namespace

QIcon Icons::icon(const QString &name, const QColor &color, int size)
{
    const QString key = name + QLatin1Char('|') + color.name() + QLatin1Char('|')
                        + QString::number(size);
    const auto cached = cache().constFind(key);
    if (cached != cache().constEnd())
        return *cached;

    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QByteArray svg = file.readAll();
    svg.replace("currentColor", color.name().toLatin1());

    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return {};

    // Two device pixel ratios, so the strokes stay crisp on a HiDPI screen.
    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(0, 0, size, size));
        painter.end();
        icon.addPixmap(pixmap);
    }

    cache().insert(key, icon);
    return icon;
}

void Icons::clearCache()
{
    cache().clear();
}

} // namespace noteAppC
