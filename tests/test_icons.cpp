// Icon resolution: a Lucide id from the bundled resource, a local file named
// by an absolute or file:// path, and a remote URL that must never block a
// repaint even when nothing is cached yet.

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#ifdef NOTEAPPC_HAVE_NETWORK
#include <QSignalSpy>
#endif
#include <QTemporaryDir>

#include "Icons.h"
#include "Perf.h"

using namespace noteAppC;

namespace {

QString writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    return path;
}

// A minimal Lucide-shaped glyph: a filled square using currentColor, so the
// same fixture also proves recolouring outside the bundled qrc resource.
QByteArray glyphSVG()
{
    return QByteArrayLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
        "<rect x='0' y='0' width='24' height='24' fill='currentColor'/></svg>");
}

// True where the render actually painted in exactly this colour, opaque -
// ignoring the anti-aliased rim a stroke leaves at the edge.
bool containsOpaquePixel(const QImage &image, const QColor &color)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) == 255 && qRed(pixel) == color.red()
                && qGreen(pixel) == color.green() && qBlue(pixel) == color.blue())
                return true;
        }
    }
    return false;
}

// U+E73C (a Devicons glyph) ships in every Nerd Font patched font regardless
// of base typeface, so it is present whichever family QFontDatabase::families()
// happens to list first - the point of these tests is the drawing code, not
// which specific glyph a particular font carries.
constexpr char32_t kNerdFontCodepoint = 0xE73C;

bool hasNerdFontInstalled()
{
    for (const QString &family : QFontDatabase::families()) {
        if (family.contains(QStringLiteral("Nerd Font"), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace

class IconsTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        Icons::setRemoteAllowed(false);
        Icons::clearCache();
    }
};

TEST_F(IconsTest, LucideIDStillResolvesAndIsRecoloured)
{
    const QIcon icon = Icons::icon(QStringLiteral("check"), QColor(255, 0, 0), 48);
    ASSERT_FALSE(icon.isNull());
    EXPECT_TRUE(containsOpaquePixel(icon.pixmap(48, 48).toImage(), QColor(255, 0, 0)));
}

TEST_F(IconsTest, UnknownNameReturnsANullIcon)
{
    EXPECT_TRUE(Icons::icon(QStringLiteral("not-a-real-icon"), QColor(Qt::black), 24).isNull());
}

TEST_F(IconsTest, LocalSVGFileLoadsAndIsRecoloured)
{
    QTemporaryDir dir;
    const QString path = writeFile(dir.filePath(QStringLiteral("glyph.svg")), glyphSVG());

    const QIcon icon = Icons::icon(path, QColor(0, 200, 0), 32);
    ASSERT_FALSE(icon.isNull());
    EXPECT_TRUE(containsOpaquePixel(icon.pixmap(32, 32).toImage(), QColor(0, 200, 0)));
}

TEST_F(IconsTest, LocalRasterFileScalesWithoutDistortionAndIsNeverTinted)
{
    QTemporaryDir dir;

    // Twice as wide as tall: fitted into a square it must letterbox, not
    // stretch to fill it - and "recolour" must leave its own colour alone.
    QImage source(100, 50, QImage::Format_ARGB32);
    source.fill(QColor(30, 60, 200));
    const QString path = dir.filePath(QStringLiteral("photo.png"));
    ASSERT_TRUE(source.save(path, "PNG"));

    const int size = 40;
    const QIcon icon = Icons::icon(path, QColor(255, 0, 0), size); // red is a decoy
    ASSERT_FALSE(icon.isNull());
    const QImage rendered = icon.pixmap(size, size).toImage();

    // The letterboxed strip above the centred image: nothing painted there.
    EXPECT_EQ(qAlpha(rendered.pixel(size / 2, 2)), 0);
    // The image itself, centred, in its own colour - never the requested tint.
    const QRgb centre = rendered.pixel(size / 2, size / 2);
    EXPECT_EQ(qAlpha(centre), 255);
    EXPECT_EQ(qRed(centre), 30);
    EXPECT_EQ(qGreen(centre), 60);
    EXPECT_EQ(qBlue(centre), 200);
}

#ifndef NOTEAPPC_HAVE_NETWORK
TEST_F(IconsTest, ARemoteURLIsANullIconEvenWithAFileInTheOldCache)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QString url = QStringLiteral("https://example.invalid/deadline.svg");
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex();
    writeFile(QDir(cacheHome.path()).filePath(QStringLiteral("notolith/icons/%1.svg").arg(QString::fromLatin1(hash))),
              glyphSVG());
    EXPECT_TRUE(Icons::icon(url, QColor(0, 0, 255), 32).isNull()) << "the default build has no remote icons";
}
#endif

#ifdef NOTEAPPC_HAVE_NETWORK
// A remote icon that is on, fetched and decoded is tests/test_remote_images.cpp's (it has the network that answers
// from memory); what is checked here needs no request at all.
TEST_F(IconsTest, ARemoteURLIsNullAndNeverFetchedWhileRemoteContentIsOff)
{
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QString url = QStringLiteral("https://example.invalid/deadline.svg");
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex();
    writeFile(QDir(cacheHome.path()).filePath(QStringLiteral("notolith/icons/%1.svg").arg(QString::fromLatin1(hash))),
              glyphSVG());
    QSignalSpy ready(Icons::notifier(), SIGNAL(iconReady(QString)));
    EXPECT_TRUE(Icons::icon(url, QColor(0, 0, 255), 32).isNull());
    EXPECT_TRUE(Icons::icon(QStringLiteral("https://example.invalid/other.svg"), QColor(0, 0, 255), 32).isNull());
    EXPECT_EQ(ready.count(), 0);
}

#endif // NOTEAPPC_HAVE_NETWORK

// --- Nerd Font glyphs ------------------------------------------------------

TEST_F(IconsTest, NerdFontGlyphByHexCodepointIsDrawnInTint)
{
    if (!hasNerdFontInstalled())
        GTEST_SKIP() << "no Nerd Font family installed on this machine";

    const QIcon icon = Icons::icon(QStringLiteral("U+E73C"), QColor(255, 0, 0), 48);
    ASSERT_FALSE(icon.isNull());
    EXPECT_TRUE(containsOpaquePixel(icon.pixmap(48, 48).toImage(), QColor(255, 0, 0)));
}

TEST_F(IconsTest, NerdFontGlyphByLiteralCharacterIsDrawnInTint)
{
    if (!hasNerdFontInstalled())
        GTEST_SKIP() << "no Nerd Font family installed on this machine";

    char32_t codepoint = kNerdFontCodepoint;
    const QString glyph = QString::fromUcs4(&codepoint, 1);
    ASSERT_EQ(glyph.toUcs4().size(), 1); // sanity: really one code point

    const QIcon icon = Icons::icon(glyph, QColor(0, 200, 0), 48);
    ASSERT_FALSE(icon.isNull());
    EXPECT_TRUE(containsOpaquePixel(icon.pixmap(48, 48).toImage(), QColor(0, 200, 0)));

    // Both forms name the same glyph and must render identically.
    const QIcon byHex = Icons::icon(QStringLiteral("u+e73c"), QColor(0, 200, 0), 48);
    ASSERT_FALSE(byHex.isNull());
    EXPECT_EQ(byHex.pixmap(48, 48).toImage(), icon.pixmap(48, 48).toImage());
}

TEST_F(IconsTest, AsciiSingleCharacterIsNotTreatedAsAGlyph)
{
    // One code point, but inside ASCII: must fall through to the Lucide
    // resource lookup (which "a" is not) rather than a Nerd Font glyph, or a
    // one-letter icon name like a language's short code would silently draw
    // whatever the installed font maps that byte to.
    EXPECT_TRUE(Icons::icon(QStringLiteral("a"), QColor(Qt::black), 24).isNull());
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Icons::icon() reads XDG_CACHE_HOME on every call rather than caching
    // the directory once, so pointing it at a throwaway directory here keeps
    // every test - even ones that never touch it directly - off the
    // developer's real ~/.cache.
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
