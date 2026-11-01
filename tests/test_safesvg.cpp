// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// safeSvg() / decodeImage(): an SVG may draw nothing of the disk but itself. Qt's SVG plugin loads an
// <image href> from any file, so each reference a vault's SVG can make is tried raw (what Qt does) and
// through the helper (what the app now does).

#include <gtest/gtest.h>

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTemporaryDir>

#include "ImageLoad.h"
#include "SafeSvg.h"

using namespace noteAppC;

namespace {

qsizetype redPixels(const QImage &image)
{
    qsizetype count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            count += c.alpha() > 200 && c.red() > 200 && c.green() < 60 && c.blue() < 60;
        }
    }
    return count;
}

QImage redPicture()
{
    QImage red(40, 40, QImage::Format_RGB32);
    red.fill(Qt::red);
    return red;
}

QByteArray pngBytes()
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    redPicture().save(&buffer, "PNG");
    return bytes;
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

/// An SVG, 40x40, whose body is `body`.
QByteArray svgWith(const QString &body)
{
    return (QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
                           "width='40' height='40' viewBox='0 0 40 40'>%1</svg>")
                .arg(body))
        .toUtf8();
}

QImage drawn(const QByteArray &svg)
{
    QImage image(40, 40, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QSvgRenderer renderer(svg);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
}

struct Disk {
    QTemporaryDir dir;
    QString outside;  // a red picture outside the vault
    QString outsideSVG; // an SVG that draws red
    QString vault;
    Disk()
    {
        vault = dir.filePath(QStringLiteral("vault"));
        outside = dir.filePath(QStringLiteral("outside.png"));
        writeFile(outside, pngBytes());
        outsideSVG = dir.filePath(QStringLiteral("outside.svg"));
        writeFile(outsideSVG, svgWith(QStringLiteral("<rect width='40' height='40' fill='red'/>")));
        QDir().mkpath(vault);
    }
};

} // namespace

// Each way a picture can be named from inside an SVG that Qt follows (found by trying them raw): every one
// draws nothing of the disk once the SVG has been through the helper.
TEST(SafeSvg, NoReferenceToTheDiskSurvives)
{
    Disk d;
    const QString abs = d.outside;
    const QString svgAbs = d.outsideSVG;
    const QString nested = QString::fromLatin1(
        svgWith(QStringLiteral("<image href='%1' width='40' height='40'/>").arg(abs)).toBase64());
    const struct { const char *name; QString body; } cases[] = {
        {"image href", QStringLiteral("<image href='%1' width='40' height='40'/>").arg(abs)},
        {"image xlink:href", QStringLiteral("<image xlink:href='%1' width='40' height='40'/>").arg(abs)},
        {"image file://", QStringLiteral("<image href='file://%1' width='40' height='40'/>").arg(abs)},
        {"relative", QStringLiteral("<image href='../outside.png' width='40' height='40'/>")},
        {"use of another file", QStringLiteral("<use href='%1#x'/>").arg(svgAbs)},
        {"feImage", QStringLiteral("<filter id='f'><feImage href='%1' width='40' height='40'/></filter>"
                                   "<rect width='40' height='40' filter='url(#f)'/>").arg(abs)},
        {"css url()", QStringLiteral("<style>rect{fill:url('%1')}</style><rect width='40' height='40'/>").arg(abs)},
        {"css @import", QStringLiteral("<style>@import url('%1');</style><rect width='40' height='40'/>").arg(svgAbs)},
        {"in a pattern", QStringLiteral("<pattern id='p' width='40' height='40' patternUnits='userSpaceOnUse'>"
                                        "<image href='%1' width='40' height='40'/></pattern>"
                                        "<rect width='40' height='40' fill='url(#p)'/>").arg(abs)},
        {"nested svg", QStringLiteral("<image href='data:image/svg+xml;base64,%1' width='40' height='40'/>").arg(nested)},
        {"svg called png", QStringLiteral("<image href='data:image/png;base64,%1' width='40' height='40'/>").arg(nested)},
        {"unencoded data", QStringLiteral("<image href='data:image/png,%1' width='40' height='40'/>").arg(abs)},
    };
    for (const auto &c : cases) {
        const QByteArray safe = safeSvg(svgWith(c.body));
        EXPECT_EQ(redPixels(drawn(safe)), 0) << c.name;
        if (!QByteArray(c.name).startsWith("css")) // Qt does not follow a CSS url(): the text may stay
            EXPECT_FALSE(safe.contains(abs.toUtf8())) << c.name << ": " << safe.toStdString();
    }
}

TEST(SafeSvg, AnOrdinarySvgStillDraws)
{
    const QString png = QString::fromLatin1(pngBytes().toBase64());
    const struct { const char *name; QString body; } cases[] = {
        {"shape", QStringLiteral("<rect width='40' height='40' fill='red'/>")},
        {"fragment use", QStringLiteral("<defs><rect id='a' width='40' height='40' fill='red'/></defs><use href='#a'/>")},
        {"fragment xlink use", QStringLiteral("<defs><rect id='a' width='40' height='40' fill='red'/></defs><use xlink:href='#a'/>")},
        {"gradient", QStringLiteral("<linearGradient id='g'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='red'/></linearGradient>"
                                    "<rect width='40' height='40' fill='url(#g)'/>")},
        {"style", QStringLiteral("<style>rect{fill:red}</style><rect width='40' height='40'/>")},
        {"text and entities", QStringLiteral("<rect width='40' height='40' fill='red'/><text x='1' y='9'>a &amp; b &lt;</text>")},
        {"embedded png", QStringLiteral("<image href='data:image/png;base64,%1' width='40' height='40'/>").arg(png)},
        {"embedded png xlink", QStringLiteral("<image xlink:href='data:image/png;base64,%1' width='40' height='40'/>").arg(png)},
    };
    for (const auto &c : cases)
        EXPECT_GT(redPixels(drawn(safeSvg(svgWith(c.body)))), 1000) << c.name;
}

TEST(SafeSvg, WhatIsNotWellFormedDrawsNothing)
{
    for (const QByteArray &bad : {QByteArray(), QByteArray("not xml"), QByteArray("<svg><rect></svg>"), QByteArray("<svg>"),
                                  QByteArray("<svg/><svg/>"),
                                  QByteArray("<!DOCTYPE svg [<!ENTITY a \"&b;\">]><svg>&a;</svg>")})
        EXPECT_TRUE(safeSvg(bad).isEmpty()) << bad.toStdString();
}

// An external entity is never read: the reference is dropped with the declaration.
TEST(SafeSvg, AnExternalEntityIsNotRead)
{
    const QByteArray safe = safeSvg("<!DOCTYPE svg [<!ENTITY x SYSTEM \"file:///etc/hostname\">]><svg><text>&x;</text></svg>");
    EXPECT_FALSE(safe.contains("file:"));
    EXPECT_FALSE(safe.contains("DOCTYPE"));
    EXPECT_FALSE(safe.contains("&x;"));
}

TEST(SafeSvg, TheDTDAndTheMarkupAroundTheRootAreDropped)
{
    const QByteArray safe = safeSvg(
        "<?xml version='1.0'?><?xml-stylesheet href='x.css'?><!DOCTYPE svg [<!ENTITY color 'red'>]>"
        "<!-- note --><svg xmlns='http://www.w3.org/2000/svg' width='40' height='40'><!-- c -->"
        "<rect width='40' height='40' fill='&color;'/></svg>");
    ASSERT_FALSE(safe.isEmpty());
    EXPECT_FALSE(safe.contains("DOCTYPE"));
    EXPECT_FALSE(safe.contains("ENTITY"));
    EXPECT_FALSE(safe.contains("stylesheet"));
    EXPECT_FALSE(safe.contains("<!--"));
    EXPECT_GT(redPixels(drawn(safe)), 1000) << "an internal entity is expanded, not lost";
}

TEST(SafeSvg, DecodeImageAndReadImageCleanWhateverTheFileIsCalled)
{
    Disk d;
    const QByteArray evil = svgWith(QStringLiteral("<image href='%1' width='40' height='40'/>").arg(d.outside));
    const QByteArray evilRelative = svgWith(QStringLiteral("<image href='../outside.png' width='40' height='40'/>"));
    writeFile(d.vault + QStringLiteral("/evil.svg"), evil);
    writeFile(d.vault + QStringLiteral("/evil-relative.svg"), evilRelative);
    writeFile(d.vault + QStringLiteral("/looks-like.png"), evil); // an SVG by content, a PNG by name
    writeFile(d.vault + QStringLiteral("/ok.svg"), svgWith(QStringLiteral("<rect width='40' height='40' fill='red'/>")));
    writeFile(d.vault + QStringLiteral("/ok.png"), pngBytes());
    writeFile(d.vault + QStringLiteral("/binary.bin"), QByteArray("\x01\x02\x03 not a picture"));
    for (const char *name : {"evil.svg", "evil-relative.svg", "looks-like.png"}) {
        const QImage image = readImage(d.vault + QLatin1Char('/') + QLatin1String(name));
        EXPECT_EQ(redPixels(image), 0) << name;
    }
    EXPECT_EQ(redPixels(decodeImage(evil)), 0);
    EXPECT_GT(redPixels(readImage(d.vault + QStringLiteral("/ok.svg"))), 1000);
    EXPECT_GT(redPixels(readImage(d.vault + QStringLiteral("/ok.png"))), 1000);
    EXPECT_TRUE(readImage(d.vault + QStringLiteral("/binary.bin")).isNull());
    EXPECT_TRUE(readImage(d.vault + QStringLiteral("/missing.png")).isNull());
}

// What Qt's plugins read beyond Obsidian's set (eps, psd, tiff, raw, pdf...) is never decoded: the format
// is told by the first bytes, and the reader is then forced to it. kimg_eps would run ghostscript on a
// %!PS file whatever it is called.
const QByteArray kEPS = QByteArray("%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 30 30\n1 0 0 setrgbcolor 0 0 30 30 rectfill\nshowpage\n");

TEST(ImageFormat, OnlyObsidiansSetIsRecognised)
{
    const QImage red = redPicture();
    QByteArray bytes[5];
    const char *const names[5] = {"png", "jpeg", "gif", "bmp", "webp"};
    for (int i = 0; i < 5; ++i) {
        QBuffer b(&bytes[i]);
        b.open(QIODevice::WriteOnly);
        if (!red.save(&b, names[i]))
            continue; // no plugin to write it here
        EXPECT_EQ(imageFormat(bytes[i]), QByteArray(names[i])) << names[i];
    }
    EXPECT_EQ(imageFormat("<svg xmlns='http://www.w3.org/2000/svg'/>"), QByteArray("svg"));
    EXPECT_EQ(imageFormat("\xef\xbb\xbf  \n<?xml version='1.0'?><svg/>"), QByteArray("svg"));
    EXPECT_EQ(imageFormat(QByteArray("\0\0\0\x1c" "ftypavif\0\0\0\0avifmif1", 20)), QByteArray("avif"));
    EXPECT_EQ(imageFormat(QByteArray("\0\0\0\x18" "ftypavis\0\0\0\0", 16)), QByteArray("avif"));
    // major brand mif1, avif among the compatible brands
    EXPECT_EQ(imageFormat(QByteArray("\0\0\0\x18" "ftypmif1\0\0\0\0" "heicavif", 24)), QByteArray("avif"));
    // mif1 with no avif is HEIC; and brands past the size the box gives are not read
    EXPECT_TRUE(imageFormat(QByteArray("\0\0\0\x18" "ftypmif1\0\0\0\0" "heicmiaf", 24)).isEmpty());
    EXPECT_TRUE(imageFormat(QByteArray("\0\0\0\x10" "ftypmif1\0\0\0\0" "avif", 20)).isEmpty());
    for (const QByteArray &other : {kEPS, QByteArray("8BPS\0\x01\0\0\0\0\0\0"), QByteArray("II*\0\x08\0\0\0"), QByteArray("MM\0*\0\0\0\x08"),
                                    QByteArray("%PDF-1.7\n"), QByteArray("\0\0\0\x18" "ftypheic\0\0\0\0", 16), QByteArray("gimp xcf file"),
                                    QByteArray("RIFF\0\0\0\0WAVE"), QByteArray("GIF"), QByteArray("\x76\x2f\x31\x01"), QByteArray("")})
        EXPECT_TRUE(imageFormat(other).isEmpty()) << other.left(12).toStdString();
}

TEST(ImageFormat, AFileOfAnotherFormatDecodesToNothingWhateverItIsCalled)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(dir.filePath(name));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        return file.fileName();
    };
    EXPECT_TRUE(readImage(write(QStringLiteral("evil.eps"), kEPS)).isNull());
    EXPECT_TRUE(readImage(write(QStringLiteral("pic.png"), kEPS)).isNull()) << "an EPS called .png";
    EXPECT_TRUE(decodeImage(kEPS).isNull());
    EXPECT_FALSE(isPictureFile(dir.filePath(QStringLiteral("pic.png"))));
    // A format only a plugin writes, when one is here: the same picture as a PNG draws, as a TIFF it does not.
    for (const char *format : {"tiff", "ico", "tga", "ppm"}) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!redPicture().save(&buffer, format))
            continue;
        const QString path = write(QStringLiteral("x.png"), bytes);
        EXPECT_TRUE(readImage(path).isNull()) << format;
        EXPECT_TRUE(decodeImage(bytes).isNull()) << format;
    }
}

// Each allowed format a writer exists for here still draws, forced to its own reader.
TEST(ImageFormat, EachAllowedFormatStillDraws)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    int drawn = 0;
    for (const char *format : {"png", "jpeg", "gif", "bmp", "webp", "avif"}) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!redPicture().save(&buffer, format)) {
            std::printf("no writer for %s here\n", format);
            continue;
        }
        const QImage fromBytes = decodeImage(bytes);
        ASSERT_FALSE(fromBytes.isNull()) << format;
        EXPECT_GT(redPixels(fromBytes), 1000) << format;
        QFile file(dir.filePath(QStringLiteral("wrong-name.dat")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        file.close();
        EXPECT_FALSE(readImage(file.fileName(), 20).isNull()) << format;
        EXPECT_LE(readImage(file.fileName(), 20).width(), 20) << format;
        ++drawn;
    }
    EXPECT_GE(drawn, 3);
    // Qt writes no GIF: a one-pixel red one, by hand.
    const QByteArray gif = QByteArray::fromBase64("R0lGODlhAQABAIAAAP8AAAAAACwAAAAAAQABAAACAkQBADs=");
    EXPECT_EQ(imageFormat(gif), QByteArray("gif"));
    const QImage pixel = decodeImage(gif);
    ASSERT_FALSE(pixel.isNull());
    EXPECT_EQ(pixel.pixelColor(0, 0).red(), 255);
}

// The cost of the cleaning on an SVG of a typical size (a diagram, 50 kB), against the 10 ms budget.
TEST(SafeSvg, CostOnATypicalFiftyKilobyteSvg)
{
    QString body;
    for (int i = 0; body.size() < 50000; ++i)
        body += QStringLiteral("<g transform='translate(%1,%2)'><rect x='0' y='0' width='30' height='12' rx='2' fill='#c33' "
                               "stroke='#222'/><text x='4' y='10' font-size='8'>node %1</text></g>\n").arg(i % 80).arg(i / 80);
    const QByteArray svg = svgWith(body);
    ASSERT_GT(svg.size(), 50000);
    QElapsedTimer timer;
    timer.start();
    constexpr int runs = 20;
    for (int i = 0; i < runs; ++i)
        ASSERT_FALSE(safeSvg(svg).isEmpty());
    const double ms = double(timer.nsecsElapsed()) / 1e6 / runs;
    std::printf("safeSvg on %lld bytes: %.2f ms per call\n", (long long)svg.size(), ms);
    EXPECT_LT(ms, 10.0);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
