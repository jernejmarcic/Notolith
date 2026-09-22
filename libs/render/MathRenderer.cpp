#include "MathRenderer.h"

#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QCoreApplication>
#include <QStandardPaths>

#include <latex.h>
#include <platform/qt/graphic_qt.h>
#include <render.h>

namespace noteAppC {

namespace {

/// MicroTeX reads its symbol tables and font metrics from disk. The build
/// copies them next to the binary; an installed build finds them under the
/// data directory.
QString resourceRoot()
{
    static const QString found = [] {
        QStringList candidates;
#ifdef NOTEAPPC_MICROTEX_RES
        candidates << QStringLiteral(NOTEAPPC_MICROTEX_RES);
#endif
        candidates << QCoreApplication::applicationDirPath()
                          + QStringLiteral("/../share/noteAppC/microtex/res");
        const QString data = QStandardPaths::locate(QStandardPaths::AppDataLocation,
                                                    QStringLiteral("microtex/res"),
                                                    QStandardPaths::LocateDirectory);
        if (!data.isEmpty())
            candidates << data;

        for (const QString &path : std::as_const(candidates)) {
            if (QFileInfo::exists(path + QStringLiteral("/base.xml"))
                || QFileInfo(path).isDir())
                return QDir(path).absolutePath();
        }
        return QString();
    }();
    return found;
}

bool ensureInitialised(QString *error)
{
    static bool tried = false;
    static bool ok = false;
    static QString failure;

    if (!tried) {
        tried = true;
        const QString root = resourceRoot();
        if (root.isEmpty()) {
            failure = QStringLiteral("MicroTeX resources not found");
        } else {
            try {
                tex::LaTeX::init(root.toStdString());
                ok = true;
            } catch (const std::exception &e) {
                failure = QString::fromUtf8(e.what());
            } catch (...) {
                failure = QStringLiteral("MicroTeX failed to initialise");
            }
        }
    }
    if (!ok && error)
        *error = failure;
    return ok;
}

} // namespace

struct MathRenderer::Private {
    mutable QString error;
};

MathRenderer::MathRenderer()
    : d(std::make_unique<Private>())
{
}

MathRenderer::~MathRenderer() = default;

bool MathRenderer::isAvailable()
{
    QString ignored;
    return ensureInitialised(&ignored);
}

QString MathRenderer::lastError() const
{
    return d->error;
}

QImage MathRenderer::render(const QString &latex, qreal pointSize, const QColor &colour,
                            int maxWidth, qreal devicePixelRatio) const
{
    d->error.clear();
    if (latex.trimmed().isEmpty() || maxWidth <= 0)
        return {};
    if (!ensureInitialised(&d->error))
        return {};

    // MicroTeX owns the returned render.
    std::unique_ptr<tex::TeXRender> render;
    try {
        render.reset(tex::LaTeX::parse(latex.toStdWString(), maxWidth, float(pointSize),
                                       float(pointSize) / 3.0f, colour.rgba()));
    } catch (const std::exception &e) {
        d->error = QString::fromUtf8(e.what());
        return {};
    } catch (...) {
        d->error = QStringLiteral("could not parse the formula");
        return {};
    }
    if (!render || render->getWidth() <= 0 || render->getHeight() <= 0) {
        d->error = QStringLiteral("the formula laid out empty");
        return {};
    }

    const int width = qMin(render->getWidth(), maxWidth);
    QImage image(QSize(width, render->getHeight()) * devicePixelRatio,
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.scale(devicePixelRatio, devicePixelRatio);
    tex::Graphics2D_qt graphics(&painter);
    render->draw(graphics, 0, 0);
    painter.end();

    return image;
}

} // namespace noteAppC
