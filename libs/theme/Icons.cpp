#include "Icons.h"
#include "ConfigFile.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QRawFont>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSvgRenderer>
#include <QUrl>

namespace noteAppC {

namespace {

QHash<QString, QIcon> &cache()
{
    static QHash<QString, QIcon> icons;
    return icons;
}

IconsNotifier &notifierInstance()
{
    static IconsNotifier instance;
    return instance;
}

QNetworkAccessManager &networkManager()
{
    static QNetworkAccessManager manager;
    return manager;
}

// A URL remembered here is not retried on every paint. clearCache() forgets
// both sets, so a theme change - the one place that already invalidates every
// pixmap - also gives a fetch that failed once more try.
QSet<QString> &pendingURLs()
{
    static QSet<QString> urls;
    return urls;
}

QSet<QString> &failedURLs()
{
    static QSet<QString> urls;
    return urls;
}

bool isRemoteURL(const QString &name)
{
    return name.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || name.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

bool isFileURL(const QString &name)
{
    return name.startsWith(QLatin1String("file://"), Qt::CaseInsensitive);
}

QString localPathOf(const QString &name)
{
    return isFileURL(name) ? QUrl(name).toLocalFile() : name;
}

// Remote icons are re-fetched at will, so they are cache.
QString cacheDirectory()
{
    return appDir(XDGroot::Cache, QStringLiteral("icons"));
}

// Keyed by a hash of the URL, not the URL itself, since a URL can be longer
// than a filesystem allows in one path component. The suffix rides along so
// the loader below knows whether to reach for QSvgRenderer or QImage without
// re-fetching anything; a cache hit with an unexpected suffix still decodes,
// because every image plugin sniffs its own header regardless of file name.
QString cacheFilePath(const QString &url)
{
    const QByteArray hash =
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString suffix = QFileInfo(QUrl(url).path()).suffix().toLower();
    if (suffix.isEmpty())
        suffix = QStringLiteral("img");
    return QDir(cacheDirectory()).filePath(QString::fromLatin1(hash) + QLatin1Char('.') + suffix);
}

// The source's own size scaled to fit inside `size` and centred, so a wide
// photo and a square glyph both land in the same box instead of one
// stretching to fill it. An empty natural size (no intrinsic size to go on)
// falls back to filling the square, which is the old, unchanged behaviour for
// every bundled Lucide icon.
QRectF centeredTarget(QSizeF natural, int size)
{
    if (natural.isEmpty())
        return QRectF(0, 0, size, size);
    const QSizeF fitted = natural.scaled(size, size, Qt::KeepAspectRatio);
    return QRectF((size - fitted.width()) / 2.0, (size - fitted.height()) / 2.0, fitted.width(),
                  fitted.height());
}

QIcon iconFromSVG(QByteArray svg, const QColor &color, int size)
{
    // A no-op on any SVG that does not spell the token, which is exactly what
    // keeps a user's own icon from being tinted: nothing here special-cases
    // "is this the bundled Lucide path", the replace just never matches.
    svg.replace("currentColor", color.name().toLatin1());

    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return {};
    const QRectF target = centeredTarget(renderer.viewBoxF().size(), size);

    // Two device pixel ratios, so the strokes stay crisp on a HiDPI screen.
    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, target);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon iconFromRaster(const QByteArray &bytes, int size)
{
    QImage image;
    if (!image.loadFromData(bytes) || image.isNull())
        return {};
    const QRectF target = centeredTarget(image.size(), size);

    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, image);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon iconFromBytes(const QByteArray &bytes, const QString &suffix, const QColor &color, int size)
{
    // Only an SVG is ever recoloured; a raster image is a user's own picture
    // and is drawn exactly as supplied.
    if (suffix == QLatin1String("svg"))
        return iconFromSVG(bytes, color, size);
    return iconFromRaster(bytes, size);
}

QIcon iconFromFile(const QString &path, const QColor &color, int size)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return iconFromBytes(file.readAll(), QFileInfo(path).suffix().toLower(), color, size);
}

QIcon iconFromResource(const QString &name, const QColor &color, int size)
{
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return iconFromSVG(file.readAll(), color, size);
}

// Runs at most once per URL until clearCache(). A caller sees a null icon
// while this is in flight or after it fails; either way Icons::icon() itself
// never waits on it.
void startFetch(const QString &url)
{
    if (pendingURLs().contains(url) || failedURLs().contains(url))
        return;
    pendingURLs().insert(url);

    QNetworkReply *reply = networkManager().get(QNetworkRequest(QUrl(url)));
    QObject::connect(reply, &QNetworkReply::finished, &notifierInstance(), [url, reply] {
        pendingURLs().remove(url);
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            failedURLs().insert(url);
            return;
        }

        const QByteArray bytes = reply->readAll();
        const QString path = cacheFilePath(url);
        ensurePrivateDir(QFileInfo(path).absolutePath());
        // Through a temporary file and rename(2), like every other write in
        // this codebase (Vault, Scheme::save) - a reader must never see a
        // half-written cache file.
        QSaveFile file(path);
        if (bytes.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(bytes) < 0
            || !file.commit()) {
            failedURLs().insert(url);
            return;
        }
        emit notifierInstance().iconReady(url);
    });
}

QIcon iconFromRemote(const QString &url, const QColor &color, int size)
{
    const QString path = cacheFilePath(url);
    if (QFile::exists(path))
        return iconFromFile(path, color, size);
    startFetch(url);
    return {};
}

// `U+E73C`, case-insensitive, 4-6 hex digits - the form a Nerd Font
// cheatsheet gives when the glyph itself is not convenient to paste.
bool nerdFontHexForm(const QString &name, char32_t *codepoint)
{
    static const QRegularExpression pattern(QStringLiteral("^[Uu]\\+([0-9A-Fa-f]{4,6})$"));
    const QRegularExpressionMatch match = pattern.match(name);
    if (!match.hasMatch())
        return false;
    *codepoint = static_cast<char32_t>(match.captured(1).toUInt(nullptr, 16));
    return true;
}

// The glyph character itself, pasted straight in. Exactly one code point and
// outside ASCII, so an ordinary single-letter string like "a" - which is
// exactly one code point too - is never mistaken for a glyph lookup.
bool nerdFontGlyphLiteral(const QString &name, char32_t *codepoint)
{
    const QList<uint> points = name.toUcs4();
    if (points.size() != 1 || points.first() <= 0x7F)
        return false;
    *codepoint = static_cast<char32_t>(points.first());
    return true;
}

bool isNerdFontForm(const QString &name, char32_t *codepoint)
{
    return nerdFontHexForm(name, codepoint) || nerdFontGlyphLiteral(name, codepoint);
}

// Chosen once per process: QFontDatabase::families() is a linear scan over
// every installed font, not something to repeat on every icon() call, and
// the answer cannot change without a restart (a font installed mid-process
// would need a cache clear elsewhere, which nothing here does - fonts are
// not icons or remote fetches, they are the platform's own state).
QString nerdFontFamily()
{
    static const QString family = [] {
        for (const QString &candidate : QFontDatabase::families()) {
            if (candidate.contains(QStringLiteral("Nerd Font"), Qt::CaseInsensitive))
                return candidate;
        }
        return QString();
    }();
    return family;
}

QIcon iconFromNerdFontGlyph(char32_t codepoint, const QColor &color, int size)
{
    const QString family = nerdFontFamily();
    if (family.isEmpty())
        return {}; // no Nerd Font installed: never fall back to a wrong glyph

    QFont font(family);
    // Measured at a large reference size so the fit below is not thrown off
    // by hinting rounding at the tiny pixel sizes icons are actually drawn
    // at; QPainter::scale below maps it down to `size` exactly.
    font.setPixelSize(256);

    // A font can carry the "Nerd Font" name and still lack this exact
    // codepoint. Qt would otherwise substitute a fallback font's glyph or a
    // tofu box, either of which is a wrong icon that looks like a right one.
    if (!QRawFont::fromFont(font).supportsCharacter(codepoint))
        return {};

    const QString glyph = QString::fromUcs4(&codepoint, 1);
    const QFontMetricsF metrics(font);
    const QRectF bounds = metrics.tightBoundingRect(glyph);
    if (bounds.isEmpty())
        return {};
    // The glyph's own ink, not its advance box, fills the square - the same
    // "fit and centre" rule centeredTarget applies to an SVG viewBox or an
    // image's natural size.
    const qreal scale = qMin(size / bounds.width(), size / bounds.height());

    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.translate(size / 2.0, size / 2.0);
        painter.scale(scale, scale);
        painter.translate(-bounds.center());
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QPointF(0, 0), glyph);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace

QIcon Icons::icon(const QString &name, const QColor &color, int size)
{
    const QString key =
        name + QLatin1Char('|') + color.name() + QLatin1Char('|') + QString::number(size);
    const auto cached = cache().constFind(key);
    if (cached != cache().constEnd())
        return *cached;

    QIcon result;
    char32_t codepoint = 0;
    if (isRemoteURL(name))
        result = iconFromRemote(name, color, size);
    else if (isFileURL(name) || QDir::isAbsolutePath(name))
        result = iconFromFile(localPathOf(name), color, size);
    else if (isNerdFontForm(name, &codepoint))
        result = iconFromNerdFontGlyph(codepoint, color, size);
    else
        result = iconFromResource(name, color, size);

    // A pending fetch or a genuinely missing name must not be cached under
    // this key, or a later repaint would keep returning the same null icon
    // forever once the download lands. The miss path above costs a hash and
    // a stat, cheap enough to redo on every paint until then.
    if (!result.isNull())
        cache().insert(key, result);
    return result;
}

void Icons::clearCache()
{
    cache().clear();
    failedURLs().clear();
}

QObject *Icons::notifier()
{
    return &notifierInstance();
}

} // namespace noteAppC
