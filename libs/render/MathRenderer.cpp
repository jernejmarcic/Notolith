#include "MathRenderer.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QSvgGenerator>

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

    /// One MicroTeX parse; null with `error` set when it throws or lays out nothing.
    std::unique_ptr<tex::TeXRender> attempt(const QString &latex, qreal pointSize, const QColor &colour,
                                            int maxWidth) const
    {
        try {
            std::unique_ptr<tex::TeXRender> render(
                tex::LaTeX::parse(latex.toStdWString(), maxWidth, float(pointSize),
                                  float(pointSize) / 3.0f, colour.rgba()));
            if (!render || render->getWidth() <= 0 || render->getHeight() <= 0) {
                error = QStringLiteral("the formula laid out empty");
                return nullptr;
            }
            return render;
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        } catch (...) {
            error = QStringLiteral("could not parse the formula");
        }
        return nullptr;
    }

    /// The parse step render() and renderSvg() share; only the paint device
    /// differs afterwards. MicroTeX owns the returned render. A formula it
    /// refuses is tried again in the shapes MathJax (and so Obsidian) accepts
    /// and MicroTeX does not: the starred `align*` family, and alignment
    /// `&`s outside any environment.
    std::unique_ptr<tex::TeXRender> parse(const QString &latex, qreal pointSize,
                                          const QColor &colour, int maxWidth) const
    {
        error.clear();
        if (latex.trimmed().isEmpty() || maxWidth <= 0)
            return nullptr;
        if (!ensureInitialised(&error))
            return nullptr;
        if (auto render = attempt(latex, pointSize, colour, maxWidth))
            return render;
        const QString firstError = error;

        static const QRegularExpression starred(
            QStringLiteral(R"(\\(begin|end)\{(align|alignat|flalign|gather|multline|equation)\*\})"));
        QString retry = latex;
        retry.replace(starred, QStringLiteral(R"(\\\1{\2})"));
        if (retry != latex) {
            if (auto render = attempt(retry, pointSize, colour, maxWidth))
                return render;
        }
        // Only with a row break to align across: an `&` in a one-line formula is
        // a typo for `\&` far more often than an alignment, and drawing it as
        // a blank would hide the mistake.
        if (retry.contains(QLatin1Char('&')) && retry.contains(QLatin1String("\\\\"))
            && !retry.contains(QLatin1String("\\begin{"))) {
            // `a &= b \\ c &= d` as an align would set it: right, left, right, left...
            if (auto render = attempt(QStringLiteral("\\begin{array}{rlrlrlrl}") + retry
                                          + QStringLiteral("\\end{array}"),
                                      pointSize, colour, maxWidth))
                return render;
        }
        error = firstError;
        return nullptr;
    }
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
    const auto render = d->parse(latex, pointSize, colour, maxWidth);
    if (!render)
        return {};

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

QString MathRenderer::renderSvg(const QString &latex, qreal pointSize, const QColor &colour,
                                int maxWidth) const
{
    const auto render = d->parse(latex, pointSize, colour, maxWidth);
    if (!render)
        return {};

    const int width = qMin(render->getWidth(), maxWidth);
    const int height = render->getHeight();

    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        QSvgGenerator generator;
        generator.setOutputDevice(&buffer);
        generator.setSize(QSize(width, height));
        generator.setViewBox(QRect(0, 0, width, height));
        // No title/description: this SVG is inlined directly into a page,
        // and those would surface as a tooltip over every formula.
        QPainter painter(&generator);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        tex::Graphics2D_qt graphics(&painter);
        render->draw(graphics, 0, 0);
        painter.end();
    }
    return QString::fromUtf8(buffer.data());
}

} // namespace noteAppC
