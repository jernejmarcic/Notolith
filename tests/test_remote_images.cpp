// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// Remote images in notes (docs/OFFLINE.md, libs/remoteimage, libs/editor/RemoteImageBlock.cpp).
//
// No test reaches the network: the network manager is a QNetworkAccessManager that answers from memory and
// the resolver is a table. The decoder helper is the real one, run under the real bubblewrap; those tests
// skip where bubblewrap is not installed.

#include <gtest/gtest.h>

#include "Bubblewrap.h"
#include "ConfigFile.h"
#include "ImageDecoder.h"
#include "ImageFetcher.h"
#include "ImageStore.h"
#include "Icons.h"
#include "ImageWire.h"
#include "MarkdownEditor.h"
#include "RemoteImagePolicy.h"
#include "RemoteImages.h"
#include "WaitFor.h"

#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <functional>
#include <random>

#include <unistd.h>

using namespace noteAppC;
using namespace noteAppC::remoteimage;

namespace {

// --- the network that is not there --------------------------------------------------------------

struct Answer {
    int status = 200;
    QByteArray body;
    QString location;
    qint64 claimedLength = -1; ///< a Content-Length that differs from the body's
    bool endless = false;      ///< 1 MiB chunks until aborted
    bool never = false;        ///< no answer at all
};

class FakeReply : public QNetworkReply {
public:
    FakeReply(const QNetworkRequest &request, const Answer &answer, QObject *parent) : QNetworkReply(parent), m_answer(answer)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setOpenMode(QIODevice::ReadOnly);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, answer.status);
        if (!answer.location.isEmpty())
            setRawHeader("Location", answer.location.toUtf8());
        if (answer.claimedLength >= 0)
            setHeader(QNetworkRequest::ContentLengthHeader, answer.claimedLength);
        QTimer::singleShot(0, this, [this] { start(); });
    }
    void abort() override
    {
        m_stopped = true;
        close();
    }
    qint64 bytesAvailable() const override { return m_buffer.size() - m_pos + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 max) override
    {
        const qint64 n = qMin<qint64>(max, m_buffer.size() - m_pos);
        std::memcpy(data, m_buffer.constData() + m_pos, size_t(n));
        m_pos += n;
        return n;
    }

private:
    void start()
    {
        if (m_answer.never || m_stopped)
            return;
        emit metaDataChanged();
        emit downloadProgress(0, m_answer.claimedLength >= 0 ? m_answer.claimedLength : m_answer.body.size());
        if (m_answer.endless) {
            pour();
            return;
        }
        m_buffer = m_answer.body;
        emit readyRead();
        emit downloadProgress(m_buffer.size(), m_answer.claimedLength >= 0 ? m_answer.claimedLength : m_buffer.size());
        setFinished(true);
        emit finished();
    }
    void pour()
    {
        if (m_stopped)
            return;
        m_buffer.append(QByteArray(1 << 20, 'x'));
        emit readyRead();
        emit downloadProgress(m_buffer.size(), -1);
        QTimer::singleShot(0, this, [this] { pour(); });
    }
    Answer m_answer;
    QByteArray m_buffer;
    qint64 m_pos = 0;
    bool m_stopped = false;
};

class FakeNetwork : public QNetworkAccessManager {
public:
    std::function<Answer(const QNetworkRequest &)> handler = [](const QNetworkRequest &) { return Answer(); };
    QList<QNetworkRequest> requests;

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.append(request);
        return new FakeReply(request, handler(request), this);
    }
};

// A name is its own table entry; anything else is a public address.
ImageFetcher::Resolver resolverFor(QHash<QString, QList<QHostAddress>> table = {})
{
    return [table](const QString &host, std::function<void(const QList<QHostAddress> &)> done) {
        done(table.contains(host) ? table.value(host) : QList<QHostAddress>{QHostAddress(QStringLiteral("93.184.216.34"))});
    };
}

ImageFetcher::Result fetchBlocking(ImageFetcher &fetcher, const QString &url)
{
    ImageFetcher::Result result;
    bool done = false;
    fetcher.fetch(QUrl(url), [&](ImageFetcher::Result r) {
        result = std::move(r);
        done = true;
    });
    EXPECT_TRUE(QTest::qWaitFor([&] { return done; }, 10000));
    return result;
}

// --- images --------------------------------------------------------------------------------------

QByteArray encoded(const QImage &image, const char *format, int quality = -1)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format, quality);
    return bytes;
}

// Qt reads GIF and cannot write it: a 1x1 one, byte for byte.
const QByteArray kTinyGIF = QByteArray::fromHex("47494638396101000100800000ffffff00000021f90401000000002c00000000010001000002024401003b");

// Nobody but the owner: read and write for a file, and enter for a directory.
bool privateTo(const QString &path, bool directory)
{
    const QFileDevice::Permissions all = QFileInfo(path).permissions();
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                                            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                           | (directory ? QFileDevice::ExeOwner : QFileDevice::Permissions());
    return !(all & others) && (all & owner) == owner && (directory || !(all & QFileDevice::ExeOwner));
}

QImage flat(int width, int height, QColor colour = QColor(200, 30, 60))
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(colour);
    return image;
}

// Something with the spread of a photograph, so the codecs have work to do.
QImage photoLike(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    std::mt19937 noise(7);
    for (int y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int n = int(noise() % 24);
            row[x] = qRgb(int(127 + 100 * std::sin(x / 37.0)) + n, int(127 + 100 * std::sin(y / 53.0)) + n,
                          int(127 + 100 * std::sin((x + y) / 71.0)) + n);
        }
    }
    return image;
}

QString skipUnlessSandbox()
{
    return decoderUnavailable();
}

} // namespace

// --- URL rules -------------------------------------------------------------------------------------

TEST(RemoteImagePolicy, OnlyHttpsToAPublicHostIsFetched)
{
    EXPECT_TRUE(refusal(QUrl(QStringLiteral("https://example.com/a.png"))).isEmpty());
    EXPECT_TRUE(refusal(QUrl(QStringLiteral("https://93.184.216.34/a.png"))).isEmpty());
    EXPECT_TRUE(refusal(QUrl(QStringLiteral("https://[2606:2800:220:1::1]/a.png"))).isEmpty());

    EXPECT_FALSE(refusal(QUrl(QStringLiteral("http://example.com/a.png"))).isEmpty()) << "http";
    EXPECT_FALSE(refusal(QUrl(QStringLiteral("ftp://example.com/a.png"))).isEmpty());
    EXPECT_FALSE(refusal(QUrl(QStringLiteral("file:///etc/passwd"))).isEmpty());
    EXPECT_FALSE(refusal(QUrl(QStringLiteral("https://user:pass@example.com/a.png"))).isEmpty()) << "user:pass";
    EXPECT_FALSE(refusal(QUrl(QStringLiteral("https://user@example.com/a.png"))).isEmpty());
    EXPECT_FALSE(refusal(QUrl(QStringLiteral("https:///a.png"))).isEmpty()) << "no host";
}

TEST(RemoteImagePolicy, InternalAddressesAndLocalhostAreRefused)
{
    for (const char *host : {"127.0.0.1", "127.1.2.3", "10.0.0.5", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.169.254",
                             "100.64.0.1", "0.0.0.0", "[::1]", "[::]", "[fd00::1]", "[fe80::1]", "[::ffff:127.0.0.1]",
                             "[::ffff:10.1.1.1]", "[64:ff9b::7f00:1]", "[2002:7f00:1::]", "localhost", "LOCALHOST", "localhost.",
                             "a.localhost", "deep.a.localhost"}) {
        EXPECT_FALSE(refusal(QUrl(QStringLiteral("https://") + QString::fromLatin1(host) + QStringLiteral("/a.png"))).isEmpty())
            << host;
    }
    // One step outside each range is the internet.
    for (const char *host : {"172.32.0.1", "100.128.0.1", "11.0.0.1", "192.169.0.1", "169.255.0.1"})
        EXPECT_TRUE(refusal(QUrl(QStringLiteral("https://") + QString::fromLatin1(host) + QStringLiteral("/a.png"))).isEmpty()) << host;
}

TEST(RemoteImagePolicy, TunnelledIPv4InTheLocalNAT64PrefixAndTeredoIsInternal)
{
    for (const char *host : {"[64:ff9b:1::7f00:1]", "[64:ff9b:1::a00:1]", "[64:ff9b:1:ffff:ffff:ffff:ffff:ffff]", "[64:ff9b:1::]",
                             "[2001::1]", "[2001:0:4136:e378:8000:63bf:3fff:fdd2]", "[2001:0:ffff:ffff:ffff:ffff:ffff:ffff]"}) {
        EXPECT_FALSE(refusal(QUrl(QStringLiteral("https://") + QString::fromLatin1(host) + QStringLiteral("/a.png"))).isEmpty()) << host;
    }
    EXPECT_TRUE(isInternalAddress(QHostAddress(QStringLiteral("64:ff9b:1::7f00:1"))));
    EXPECT_TRUE(isInternalAddress(QHostAddress(QStringLiteral("2001:0:1:2:3:4:5:6"))));
    // Neighbouring prefixes are the internet.
    for (const char *host : {"[2001:4860:4860::8888]", "[2606:2800:220:1::1]"})
        EXPECT_TRUE(refusal(QUrl(QStringLiteral("https://") + QString::fromLatin1(host) + QStringLiteral("/a.png"))).isEmpty()) << host;
}

TEST(RemoteImagePolicy, BadPortsAndOddIPv4LookingHostsAreRefusedHere)
{
    for (const char *url : {"https://example.com:99999/a.png", "https://example.com:0/a.png", "https://example.com:-1/a.png",
                            "https://example.com:http/a.png", "https://1.2.3.4.5/a.png", "https://256.1.1.1/a.png",
                            "https://0.0.0.0.0/a.png", "https://1.2.3.4:0/a.png", "https://999.999.999.999/a.png", "https://a.b.12/a.png",
                            "https://a.0x1f/a.png", "https://exa mple.com/a.png"}) {
        EXPECT_FALSE(refusal(QUrl(QString::fromLatin1(url), QUrl::TolerantMode)).isEmpty()) << url;
    }
    for (const char *url : {"https://example.com:8443/a.png", "https://example.com:443/a.png", "https://1.2.3.4:8443/a.png",
                            "https://3com.com/a.png", "https://example.co2/a.png", "https://my-host.example.com/a.png"}) {
        EXPECT_TRUE(refusal(QUrl(QString::fromLatin1(url), QUrl::TolerantMode)).isEmpty()) << url;
    }
}

TEST(RemoteImagePolicy, AnInternationalHostIsShownAndRememberedInASCII)
{
    const QUrl real(QStringLiteral("https://apple.com/a.png"));
    const QUrl lookalike(QStringLiteral("https://\u0430pple.com/a.png")); // Cyrillic a
    EXPECT_EQ(asciiHost(real), QStringLiteral("apple.com"));
    EXPECT_EQ(asciiHost(lookalike), QStringLiteral("xn--pple-43d.com"));
    EXPECT_EQ(originOf(lookalike), QStringLiteral("https://xn--pple-43d.com"));
    EXPECT_NE(originOf(lookalike), originOf(real));
    EXPECT_EQ(asciiHost(QUrl(QStringLiteral("https://B\u00fccher.example/"))), QStringLiteral("xn--bcher-kva.example"));
    EXPECT_EQ(asciiHost(QUrl(QStringLiteral("https://[2606:2800:220:1::1]/"))), QStringLiteral("2606:2800:220:1::1"));
    EXPECT_EQ(originOf(QUrl(QStringLiteral("https://[2606:2800:220:1::1]:8443/"))), QStringLiteral("https://[2606:2800:220:1::1]:8443"));
    EXPECT_TRUE(refusal(lookalike).isEmpty()) << "a look-alike is a name like any other, it is only shown honestly";
}

TEST(RemoteImagePolicy, AnOriginIsSchemeHostAndPort)
{
    EXPECT_EQ(originOf(QUrl(QStringLiteral("https://Example.COM/a/b.png?x=1#f"))), QStringLiteral("https://example.com"));
    EXPECT_EQ(originOf(QUrl(QStringLiteral("https://example.com:443/a.png"))), QStringLiteral("https://example.com"));
    EXPECT_EQ(originOf(QUrl(QStringLiteral("https://example.com:8443/a.png"))), QStringLiteral("https://example.com:8443"));
    EXPECT_NE(originOf(QUrl(QStringLiteral("https://cdn.example.com/a.png"))), originOf(QUrl(QStringLiteral("https://example.com/a.png"))));
}

// --- the fetcher ---------------------------------------------------------------------------------------

TEST(ImageFetcherTest, SendsTheFixedAnonymousRequest)
{
    FakeNetwork net;
    net.handler = [](const QNetworkRequest &) {
        Answer answer;
        answer.body = "bytes";
        return answer;
    };
    ImageFetcher fetcher(&net);
    fetcher.setResolver(resolverFor());
    const auto result = fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png"));
    EXPECT_EQ(result.error, QString());
    EXPECT_EQ(result.bytes, QByteArray("bytes"));
    ASSERT_EQ(net.requests.size(), 1);
    const QNetworkRequest &request = net.requests.first();
    EXPECT_EQ(request.header(QNetworkRequest::UserAgentHeader).toString(), QStringLiteral("Notolith"));
    EXPECT_FALSE(request.hasRawHeader("Referer"));
    EXPECT_FALSE(request.hasRawHeader("Cookie"));
    EXPECT_FALSE(request.hasRawHeader("Authorization"));
    EXPECT_EQ(request.rawHeader("Accept-Encoding"), QByteArray("identity"));
    EXPECT_EQ(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
    EXPECT_EQ(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
    EXPECT_EQ(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), int(QNetworkRequest::Manual));
}

TEST(ImageFetcherTest, TheCookieJarKeepsNothing)
{
    FakeNetwork net;
    ImageFetcher fetcher(&net);
    ASSERT_NE(net.cookieJar(), nullptr);
    const QUrl url(QStringLiteral("https://example.com/"));
    EXPECT_FALSE(net.cookieJar()->setCookiesFromUrl({QNetworkCookie("a", "b")}, url));
    EXPECT_TRUE(net.cookieJar()->cookiesForUrl(url).isEmpty());
}

TEST(ImageFetcherTest, RefusedAddressesSendNothing)
{
    FakeNetwork net;
    ImageFetcher fetcher(&net);
    fetcher.setResolver(resolverFor({{QStringLiteral("evil.example"), {QHostAddress(QStringLiteral("10.0.0.7"))}},
                                     {QStringLiteral("mixed.example"), {QHostAddress(QStringLiteral("93.184.216.34")), QHostAddress(QStringLiteral("127.0.0.1"))}},
                                     {QStringLiteral("gone.example"), {}}}));
    for (const char *url : {"http://example.com/a.png", "https://u:p@example.com/a.png", "https://127.0.0.1/a.png", "https://[::1]/a.png",
                            "https://localhost/a.png", "https://evil.example/a.png", "https://mixed.example/a.png",
                            "https://gone.example/a.png"}) {
        EXPECT_FALSE(fetchBlocking(fetcher, QString::fromLatin1(url)).error.isEmpty()) << url;
    }
    EXPECT_TRUE(net.requests.isEmpty());
}

TEST(ImageFetcherTest, RedirectsAreCheckedAtEveryHop)
{
    FakeNetwork net;
    ImageFetcher fetcher(&net);
    fetcher.setResolver(resolverFor());
    const auto redirectTo = [&net](const QString &location) {
        net.requests.clear();
        net.handler = [location](const QNetworkRequest &) {
            Answer answer;
            answer.status = 302;
            answer.location = location;
            return answer;
        };
    };
    redirectTo(QStringLiteral("http://example.com/b.png"));
    EXPECT_NE(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).error.indexOf(QStringLiteral("http")), -1);
    EXPECT_EQ(net.requests.size(), 1) << "the http target was never asked for";

    redirectTo(QStringLiteral("https://127.0.0.1/b.png"));
    EXPECT_FALSE(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).error.isEmpty());
    EXPECT_EQ(net.requests.size(), 1);

    redirectTo(QStringLiteral("https://user:pw@example.com/b.png"));
    EXPECT_FALSE(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).error.isEmpty());
    EXPECT_EQ(net.requests.size(), 1);

    // At most 3: the 1st request and 3 followed redirects are 4 requests, and the 4th redirect is refused.
    net.requests.clear();
    net.handler = [](const QNetworkRequest &request) {
        Answer answer;
        answer.status = 302;
        answer.location = request.url().path() + QLatin1Char('x'); // relative: resolved against the request
        return answer;
    };
    const auto loop = fetchBlocking(fetcher, QStringLiteral("https://example.com/a"));
    EXPECT_FALSE(loop.error.isEmpty());
    EXPECT_EQ(net.requests.size(), 4);

    // Three redirects are allowed.
    net.requests.clear();
    net.handler = [](const QNetworkRequest &request) {
        Answer answer;
        if (request.url().path().size() < 5) {
            answer.status = 302;
            answer.location = request.url().path() + QLatin1Char('x');
        } else {
            answer.body = "arrived";
        }
        return answer;
    };
    const auto ok = fetchBlocking(fetcher, QStringLiteral("https://example.com/a"));
    EXPECT_EQ(ok.error, QString());
    EXPECT_EQ(ok.bytes, QByteArray("arrived"));
    EXPECT_EQ(net.requests.size(), 4);
}

TEST(ImageFetcherTest, AnUnclickedLoadDoesNotFollowARedirectToAnotherOrigin)
{
    FakeNetwork net;
    ImageFetcher fetcher(&net);
    fetcher.setResolver(resolverFor());
    net.handler = [](const QNetworkRequest &request) {
        Answer answer;
        if (request.url().host() == QLatin1String("example.com") && request.url().path() == QLatin1String("/a.png")) {
            answer.status = 302;
            answer.location = QStringLiteral("https://other.example/b.png");
        } else if (request.url().host() == QLatin1String("example.com") && request.url().path() == QLatin1String("/same")) {
            answer.status = 302;
            answer.location = QStringLiteral("/b.png");
        } else {
            answer.body = "arrived";
        }
        return answer;
    };
    ImageFetcher::Result result;
    bool done = false;
    fetcher.fetch(QUrl(QStringLiteral("https://example.com/a.png")), [&](ImageFetcher::Result r) {
        result = std::move(r);
        done = true;
    }, true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return done; }, 10000));
    EXPECT_NE(result.error.indexOf(QStringLiteral("other.example")), -1) << qPrintable(result.error);
    EXPECT_EQ(net.requests.size(), 1) << "nothing was sent to the other origin";

    // Inside its own origin a redirect is followed, and a click (no restriction) follows it anywhere.
    EXPECT_EQ(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).bytes, QByteArray("arrived"));
    net.requests.clear();
    done = false;
    fetcher.fetch(QUrl(QStringLiteral("https://example.com/same")), [&](ImageFetcher::Result r) {
        result = std::move(r);
        done = true;
    }, true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return done; }, 10000));
    EXPECT_EQ(result.error, QString());
    EXPECT_EQ(result.bytes, QByteArray("arrived"));
    EXPECT_EQ(net.requests.size(), 2);
}

TEST(ImageFetcherTest, ABodyOverTwentyMiBIsAbortedByLengthOrWhileReading)
{
    FakeNetwork net;
    ImageFetcher fetcher(&net);
    fetcher.setResolver(resolverFor());

    net.handler = [](const QNetworkRequest &) {
        Answer answer;
        answer.claimedLength = 21 * 1024 * 1024;
        answer.body = "tiny";
        return answer;
    };
    EXPECT_NE(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).error.indexOf(QStringLiteral("20 MiB")), -1)
        << "by Content-Length";

    net.handler = [](const QNetworkRequest &) {
        Answer answer;
        answer.endless = true; // no length, and it never stops
        return answer;
    };
    EXPECT_NE(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).error.indexOf(QStringLiteral("20 MiB")), -1)
        << "while reading";

    net.handler = [](const QNetworkRequest &) {
        Answer answer;
        answer.body = QByteArray(20 * 1024 * 1024, 'x'); // exactly the limit is allowed
        return answer;
    };
    const auto exact = fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png"));
    EXPECT_EQ(exact.error, QString());
    EXPECT_EQ(exact.bytes.size(), 20 * 1024 * 1024);
}

TEST(ImageFetcherTest, ANonAnswerEndsAtTheDeadline)
{
    FakeNetwork net;
    net.handler = [](const QNetworkRequest &) {
        Answer answer;
        answer.never = true;
        return answer;
    };
    ImageFetcher fetcher(&net);
    fetcher.setResolver(resolverFor());
    fetcher.setTimeoutForTests(100);
    QElapsedTimer clock;
    clock.start();
    EXPECT_FALSE(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).error.isEmpty());
    EXPECT_LT(clock.elapsed(), 3000);
}

TEST(ImageFetcherTest, AnErrorStatusIsAFailure)
{
    FakeNetwork net;
    net.handler = [](const QNetworkRequest &) {
        Answer answer;
        answer.status = 404;
        answer.body = "not found";
        return answer;
    };
    ImageFetcher fetcher(&net);
    fetcher.setResolver(resolverFor());
    EXPECT_NE(fetchBlocking(fetcher, QStringLiteral("https://example.com/a.png")).error.indexOf(QStringLiteral("404")), -1);
}

// --- magic bytes ---------------------------------------------------------------------------------------------

TEST(ImageWireTest, TheFormatIsTheMagicBytesNotAnythingElse)
{
    EXPECT_EQ(sniffFormat(encoded(flat(4, 4), "PNG")), ImageFormat::PNG);
    EXPECT_EQ(sniffFormat(encoded(flat(4, 4), "JPEG")), ImageFormat::JPEG);
    EXPECT_EQ(sniffFormat(kTinyGIF), ImageFormat::GIF);
    EXPECT_EQ(sniffFormat(QByteArray("RIFF\x24\0\0\0WEBPVP8 ", 16)), ImageFormat::WebP);

    EXPECT_EQ(sniffFormat(QByteArray("<svg xmlns='http://www.w3.org/2000/svg'/>")), ImageFormat::None);
    EXPECT_EQ(sniffFormat(encoded(flat(4, 4), "BMP")), ImageFormat::None);
    EXPECT_EQ(sniffFormat(QByteArray("RIFF\x24\0\0\0WAVEfmt ", 16)), ImageFormat::None) << "RIFF, but not WebP";
    EXPECT_EQ(sniffFormat(QByteArray("%PDF-1.7")), ImageFormat::None);
    EXPECT_EQ(sniffFormat(QByteArray()), ImageFormat::None);
    EXPECT_EQ(sniffFormat(QByteArray("\x89PNG")), ImageFormat::None) << "a cut header";
}

// --- what the app accepts back from the helper ------------------------------------------------------------

TEST(ImageWireTest, ARealReplyIsAccepted)
{
    QByteArray reply = encodeHeader(ReplyStatus::Ok, 3, 2, ImageFormat::PNG);
    reply.append(QByteArray(3 * 2 * 4, char(0x7f)));
    const DecodedImage decoded = validateReply(reply, ImageFormat::PNG);
    EXPECT_EQ(decoded.error, QString());
    EXPECT_EQ(decoded.image.size(), QSize(3, 2));
}

TEST(ImageWireTest, ALyingHeaderIsRefused)
{
    const auto reply = [](ReplyStatus status, quint32 w, quint32 h, ImageFormat format, qsizetype pixelBytes) {
        QByteArray out = encodeHeader(status, w, h, format);
        out.append(QByteArray(pixelBytes, 'p'));
        return out;
    };
    const auto refused = [](const QByteArray &bytes, ImageFormat expected = ImageFormat::PNG) {
        return !validateReply(bytes, expected).error.isEmpty();
    };
    EXPECT_FALSE(refused(reply(ReplyStatus::Ok, 2, 2, ImageFormat::PNG, 16)));
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 2, 2, ImageFormat::PNG, 15))) << "a pixel short";
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 2, 2, ImageFormat::PNG, 17))) << "a byte over";
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 100, 100, ImageFormat::PNG, 16))) << "claims 100x100, sends 2x2";
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 0, 2, ImageFormat::PNG, 0)));
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 2, 0, ImageFormat::PNG, 0)));
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 5000, 5000, ImageFormat::PNG, 0))) << "over 16 megapixels";
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 0xffffffffu, 0xffffffffu, ImageFormat::PNG, 16))) << "the product wraps in 32 bits";
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 65536, 65536, ImageFormat::PNG, 16))) << "so does this one";
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 2, 2, ImageFormat::JPEG, 16))) << "not the format the app sniffed";
    EXPECT_TRUE(refused(reply(ReplyStatus::Ok, 2, 2, ImageFormat::None, 16)));
    EXPECT_TRUE(refused(reply(ReplyStatus::DecodeFailed, 2, 2, ImageFormat::PNG, 16))) << "an error status carries no image";
    EXPECT_TRUE(refused(QByteArray("NIMG")));
    EXPECT_TRUE(refused(QByteArray()));
    QByteArray badMagic = reply(ReplyStatus::Ok, 2, 2, ImageFormat::PNG, 16);
    badMagic[0] = 'X';
    EXPECT_TRUE(refused(badMagic));
}

// --- the helper, for real -------------------------------------------------------------------------------------------

TEST(ImageDecoderTest, ASmallPNGRoundTripsThroughTheSandbox)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    const QImage source = flat(20, 10, QColor(10, 200, 30));
    const DecodedImage decoded = decodeInSandbox(encoded(source, "PNG"), ImageFormat::PNG);
    ASSERT_EQ(decoded.error, QString());
    EXPECT_EQ(decoded.image.size(), QSize(20, 10));
    EXPECT_EQ(decoded.image.pixelColor(5, 5), QColor(10, 200, 30));
}

TEST(ImageDecoderTest, TheHelperRunsUnderTheSharedSandboxCaps)
{
    sandbox::overrideCgroupLimits(true);
    sandbox::Command command = decoderCommand();
    sandbox::forgetCgroupProbe();
    if (command.program.isEmpty())
        GTEST_SKIP() << "no bubblewrap or no syscall filter here";
    if (command.filterFD >= 0)
        ::close(command.filterFD);
    const QStringList &args = command.arguments;
    EXPECT_TRUE(args.contains(QStringLiteral("MemoryMax=1073741824"))) << "a cgroup memory cap";
    EXPECT_TRUE(args.contains(QStringLiteral("TasksMax=256")));
    EXPECT_TRUE(args.contains(QStringLiteral("--unshare-all")));
    EXPECT_TRUE(args.contains(QStringLiteral("--cap-drop")));
    EXPECT_TRUE(args.contains(QStringLiteral("--seccomp")));
    const qsizetype shm = args.indexOf(QStringLiteral("/dev/shm"));
    ASSERT_GT(shm, 3);
    EXPECT_EQ(args.at(shm - 1), QStringLiteral("--tmpfs"));
    EXPECT_EQ(args.at(shm - 3), QStringLiteral("--size")) << "/dev/shm is sized, not half the RAM";
    EXPECT_EQ(args.at(shm - 2), QStringLiteral("67108864"));
}

TEST(ImageDecoderTest, TheAnswerToCanItRunIsTheSameEveryTime)
{
    EXPECT_EQ(decoderUnavailable(), decoderUnavailable());
}

TEST(ImageDecoderTest, JPEGAndGIFDecodeToo)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    const DecodedImage jpeg = decodeInSandbox(encoded(flat(33, 17), "JPEG"), ImageFormat::JPEG);
    ASSERT_EQ(jpeg.error, QString());
    EXPECT_EQ(jpeg.image.size(), QSize(33, 17));
    const DecodedImage gif = decodeInSandbox(kTinyGIF, ImageFormat::GIF);
    ASSERT_EQ(gif.error, QString());
    EXPECT_EQ(gif.image.size(), QSize(1, 1));
}

TEST(ImageDecoderTest, TheHelperRefusesAnImageOverSixteenMegapixels)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    const QByteArray big = encoded(flat(4100, 4100), "PNG"); // 16.8 MP, a few dozen kB on the wire
    ASSERT_LT(big.size(), 1024 * 1024);
    const DecodedImage over = decodeInSandbox(big, ImageFormat::PNG);
    EXPECT_TRUE(over.image.isNull());
    EXPECT_NE(over.error.indexOf(QStringLiteral("16 megapixels")), -1) << qPrintable(over.error);

    const DecodedImage atCap = decodeInSandbox(encoded(flat(4000, 4000), "PNG"), ImageFormat::PNG); // exactly 16 MP
    EXPECT_EQ(atCap.error, QString());
    EXPECT_EQ(atCap.image.size(), QSize(4000, 4000));
}

TEST(ImageDecoderTest, TheHelperRefusesWhatIsNotAnImageAndWhatIsBroken)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    // The app sniffs first; the helper does not trust that and sniffs again.
    EXPECT_FALSE(decodeInSandbox(QByteArray("<svg xmlns='http://www.w3.org/2000/svg'/>"), ImageFormat::PNG).error.isEmpty());
    QByteArray cut = encoded(flat(64, 64), "PNG");
    cut.truncate(cut.size() / 2);
    EXPECT_FALSE(decodeInSandbox(cut, ImageFormat::PNG).error.isEmpty());
    // The helper says PNG; the app had sniffed JPEG: the answer does not match.
    EXPECT_FALSE(decodeInSandbox(encoded(flat(8, 8), "PNG"), ImageFormat::JPEG).error.isEmpty());
}

// --- the allowlist and the cache -------------------------------------------------------------------------------------------

TEST(ImageAllowlistTest, LivesInTheStateDirectoryAndNeverInTheVault)
{
    QTemporaryDir vault;
    ASSERT_TRUE(vault.isValid());
    QFile note(vault.filePath(QStringLiteral("note.md")));
    ASSERT_TRUE(note.open(QIODevice::WriteOnly));
    note.write("![](https://example.com/a.png)\n");
    note.close();

    const QString path = ImageAllowlist::defaultPath();
    EXPECT_TRUE(path.startsWith(xdgRoot(XDGroot::State) + QStringLiteral("/") + QString(kAppDirName) + QStringLiteral("/"))) << qPrintable(path);
    EXPECT_FALSE(path.startsWith(vault.path()));
    QFile::remove(path);

    ImageAllowlist list(path);
    EXPECT_FALSE(list.contains(QStringLiteral("https://example.com")));
    EXPECT_TRUE(list.add(QStringLiteral("https://example.com")));
    EXPECT_TRUE(list.add(QStringLiteral("https://cdn.example.com:8443")));
    EXPECT_TRUE(privateTo(path, false)) << "0600";
    EXPECT_TRUE(privateTo(QFileInfo(path).absolutePath(), true)) << "0700";

    ImageAllowlist reread(path);
    EXPECT_TRUE(reread.contains(QStringLiteral("https://example.com")));
    EXPECT_TRUE(reread.contains(QStringLiteral("https://cdn.example.com:8443")));
    EXPECT_FALSE(reread.contains(QStringLiteral("https://example.com:8443")));

    EXPECT_EQ(QDir(vault.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("note.md")})
        << "nothing was written into the vault";

    EXPECT_TRUE(reread.clear());
    EXPECT_FALSE(ImageAllowlist(path).contains(QStringLiteral("https://example.com")));
    QFile::remove(path);
}

TEST(ImageCacheTest, NamedByTheHashOfTheURLPrivateAndCapped)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString dir = root.filePath(QStringLiteral("cache/remote-images"));
    ImageCache cache(dir, 1000);
    const auto url = [](int n) { return QUrl(QStringLiteral("https://example.com/%1.png").arg(n)); };

    ASSERT_TRUE(cache.write(url(0), QByteArray(400, 'a')));
    EXPECT_TRUE(privateTo(dir, true)) << "0700";
    const QString expectedName = QString::fromLatin1(
        QCryptographicHash::hash(QByteArray("https://example.com/0.png"), QCryptographicHash::Sha256).toHex());
    EXPECT_EQ(QFileInfo(cache.pathFor(url(0))).fileName(), expectedName);
    EXPECT_TRUE(privateTo(cache.pathFor(url(0)), false)) << "0600";
    EXPECT_EQ(cache.read(url(0)), QByteArray(400, 'a'));
    EXPECT_EQ(cache.pathFor(QUrl(QStringLiteral("https://example.com/0.png#frag"))), cache.pathFor(url(0)));

    // Older files go first. The times are set by hand: a fast disk gives the writes one clock tick.
    const QDateTime base = QDateTime::currentDateTime().addDays(-1);
    const auto age = [&](int n, int minutes) {
        QFile file(cache.pathFor(url(n)));
        ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        ASSERT_TRUE(file.setFileTime(base.addSecs(minutes * 60), QFileDevice::FileModificationTime));
    };
    age(0, 0);
    ASSERT_TRUE(cache.write(url(1), QByteArray(400, 'b')));
    age(1, 1);
    EXPECT_EQ(cache.totalBytes(), 800);
    ASSERT_TRUE(cache.write(url(2), QByteArray(400, 'c'))); // 1200 > 1000: the oldest, url(0), goes
    EXPECT_EQ(cache.totalBytes(), 800);
    EXPECT_TRUE(cache.read(url(0)).isEmpty());
    EXPECT_FALSE(cache.read(url(1)).isEmpty());
    EXPECT_FALSE(cache.read(url(2)).isEmpty()) << "the file just written stays";
    age(2, 2);
    ASSERT_TRUE(cache.write(url(3), QByteArray(900, 'd'))); // 1700: url(1) and url(2) both go
    EXPECT_LE(cache.totalBytes(), 1000);
    EXPECT_TRUE(cache.read(url(1)).isEmpty());
    EXPECT_TRUE(cache.read(url(2)).isEmpty());
    EXPECT_EQ(cache.read(url(3)).size(), 900);
}

// --- the whole path, and the editor ---------------------------------------------------------------------------------

namespace {

// A RemoteImages on a fake network and temporary directories, standing in for the process's one.
struct Rig {
    QTemporaryDir dirs;
    FakeNetwork net;
    std::unique_ptr<RemoteImages> images;
    QByteArray image = encoded(flat(40, 20), "PNG");

    Rig()
    {
        net.handler = [this](const QNetworkRequest &) {
            Answer answer;
            answer.body = image;
            return answer;
        };
        images = std::make_unique<RemoteImages>(
            RemoteImages::Config{dirs.filePath(QStringLiteral("state/hosts")), dirs.filePath(QStringLiteral("cache")), &net});
        images->fetcher().setResolver(resolverFor());
        RemoteImages::useForTests(images.get());
    }
    ~Rig() { RemoteImages::useForTests(nullptr); }
};

std::unique_ptr<MarkdownEditor> editorWith(const QString &text)
{
    auto editor = std::make_unique<MarkdownEditor>();
    editor->resize(900, 600);
    editor->setNoteText(text);
    return editor;
}

} // namespace

TEST(RemoteImagesTest, TheWholePathFetchesCachesAndDecodes)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    Rig rig;
    const QUrl url(QStringLiteral("https://example.com/a.png"));
    QSignalSpy loaded(rig.images.get(), &RemoteImages::loaded);
    rig.images->load(url);
    WAIT_TRUE(loaded.count() == 1, 15000);
    EXPECT_EQ(loaded.first().at(1).value<QImage>().size(), QSize(40, 20));
    EXPECT_EQ(rig.net.requests.size(), 1);
    EXPECT_EQ(rig.images->cache().read(url), rig.image);

    // The second time the original comes from the cache, and goes through the helper again.
    rig.images->load(url);
    WAIT_TRUE(loaded.count() == 2, 15000);
    EXPECT_EQ(rig.net.requests.size(), 1);
}

TEST(RemoteImagesTest, WhatIsNotAnImageIsNeverDecodedOrCached)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    Rig rig;
    rig.image = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>"; // whatever the Content-Type says
    const QUrl url(QStringLiteral("https://example.com/a.png"));
    QSignalSpy failed(rig.images.get(), &RemoteImages::failed);
    rig.images->load(url);
    WAIT_TRUE(failed.count() == 1, 15000);
    EXPECT_NE(failed.first().at(1).toString().indexOf(QStringLiteral("PNG, JPEG, GIF or WebP")), -1);
    EXPECT_TRUE(rig.images->cache().read(url).isEmpty());
}

TEST(RemoteImagesTest, RefusedURLsFailWithoutAnyRequest)
{
    Rig rig;
    QSignalSpy failed(rig.images.get(), &RemoteImages::failed);
    rig.images->load(QUrl(QStringLiteral("http://example.com/a.png")));
    rig.images->load(QUrl(QStringLiteral("https://10.1.2.3/a.png")));
    WAIT_TRUE(failed.count() == 2, 5000);
    EXPECT_TRUE(rig.net.requests.isEmpty());
}

TEST(RemoteImageEditorTest, NothingIsFetchedUntilTheClick)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    Rig rig;
    const QString url = QStringLiteral("https://example.com/a.png");
    auto editor = editorWith(QStringLiteral("![](") + url + QStringLiteral(")\n"));
    EXPECT_TRUE(editor->isFileCardAt(0)) << "a placeholder";
    QTest::qWait(150);
    EXPECT_TRUE(rig.net.requests.isEmpty());
    EXPECT_TRUE(editor->isFileCardAt(0));

    editor->loadRemoteImage(url, false);
    WAIT_TRUE(!editor->isFileCardAt(0), 15000);
    EXPECT_EQ(rig.net.requests.size(), 1);
    EXPECT_EQ(rig.images->allowedCount(), 0) << "a plain click remembers nothing";
}

TEST(RemoteImageEditorTest, AnHTMLImageAndAnImageInTextAreHeldBackToo)
{
    Rig rig;
    auto editor = editorWith(QStringLiteral("Text <img src=\"https://example.com/a.png\" width=\"50\"> and ![x](https://example.com/b.png) in a line.\n"));
    editor->documentHeight();
    QTest::qWait(150);
    EXPECT_EQ(editor->embedCellCountAt(0), 2);
    EXPECT_TRUE(rig.net.requests.isEmpty());
}

TEST(RemoteImageEditorTest, HttpAndLocalAddressesStayPlaceholdersWhateverIsClicked)
{
    Rig rig;
    const QString http = QStringLiteral("http://example.com/a.png");
    const QString local = QStringLiteral("https://192.168.1.10/a.png");
    auto editor = editorWith(QStringLiteral("![](") + http + QStringLiteral(")\n\n![](") + local + QStringLiteral(")\n"));
    const qsizetype second = editor->noteText().indexOf(QStringLiteral("![](https://192"));
    EXPECT_TRUE(editor->isFileCardAt(0));
    EXPECT_TRUE(editor->isFileCardAt(second));
    editor->loadRemoteImage(http, false);
    editor->loadRemoteImage(local, true);
    QTest::qWait(200);
    EXPECT_TRUE(editor->isFileCardAt(0));
    EXPECT_TRUE(editor->isFileCardAt(second));
    EXPECT_TRUE(rig.net.requests.isEmpty());
}

TEST(RemoteImageEditorTest, AlwaysLoadFromRemembersTheOriginAndOnlyThatOrigin)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    Rig rig;
    const QString mine = QStringLiteral("https://example.com/a.png");
    const QString other = QStringLiteral("https://other.example/b.png");
    const QString text = QStringLiteral("![](") + mine + QStringLiteral(")\n\n![](") + other + QStringLiteral(")\n");
    auto editor = editorWith(text);
    const qsizetype second = text.indexOf(QStringLiteral("![](https://other"));
    editor->loadRemoteImage(mine, true);
    WAIT_TRUE(!editor->isFileCardAt(0), 15000);
    EXPECT_TRUE(rig.images->isAllowed(QUrl(mine)));
    EXPECT_TRUE(rig.images->isAllowed(QUrl(QStringLiteral("https://example.com/other/path.gif"))));
    EXPECT_FALSE(rig.images->isAllowed(QUrl(other)));
    EXPECT_TRUE(editor->isFileCardAt(second)) << "another host still asks";
    EXPECT_TRUE(QFileInfo::exists(rig.dirs.filePath(QStringLiteral("state/hosts"))));

    // A new note with the image in it: the allowed origin loads without a click, the other does not.
    rig.net.requests.clear();
    auto again = editorWith(text);
    WAIT_TRUE(!again->isFileCardAt(0), 15000);
    EXPECT_TRUE(again->isFileCardAt(second));
    for (const QNetworkRequest &request : std::as_const(rig.net.requests))
        EXPECT_EQ(request.url().host(), QStringLiteral("example.com"));

    rig.images->clearAllowlist();
    auto cleared = editorWith(text);
    QTest::qWait(150);
    EXPECT_TRUE(cleared->isFileCardAt(0)) << "after Clear list it asks again";
}

TEST(RemoteImageEditorTest, ALoadHoldsForTheNoteItWasClickedIn)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    Rig rig;
    const QString url = QStringLiteral("https://example.com/a.png");
    const QString text = QStringLiteral("![](") + url + QStringLiteral(")\n");
    auto editor = editorWith(text);
    editor->loadRemoteImage(url, false);
    WAIT_TRUE(!editor->isFileCardAt(0), 15000);
    editor->setNoteText(QStringLiteral("another note\n\n") + text); // the same image in the next note
    EXPECT_TRUE(editor->isFileCardAt(editor->noteText().indexOf(QStringLiteral("!["))));
}

TEST(RemoteImageEditorTest, AnAllowedOriginDoesNotRedirectOffToAnotherOneUntilClicked)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    Rig rig;
    rig.net.handler = [&rig](const QNetworkRequest &request) {
        Answer answer;
        if (request.url().host() == QLatin1String("example.com")) {
            answer.status = 302;
            answer.location = QStringLiteral("https://other.example/b.png");
        } else {
            answer.body = rig.image;
        }
        return answer;
    };
    const QString url = QStringLiteral("https://example.com/a.png");
    rig.images->allowOrigin(QUrl(url));
    auto editor = editorWith(QStringLiteral("![](") + url + QStringLiteral(")\n"));
    WAIT_TRUE(rig.net.requests.size() == 1, 5000);
    QTest::qWait(300);
    EXPECT_EQ(rig.net.requests.size(), 1) << "other.example was never asked";
    EXPECT_TRUE(editor->isFileCardAt(0)) << "a placeholder, for a click";

    editor->loadRemoteImage(url, false);
    WAIT_TRUE(!editor->isFileCardAt(0), 15000);
    ASSERT_EQ(rig.net.requests.size(), 3);
    EXPECT_EQ(rig.net.requests.last().url().host(), QStringLiteral("other.example"));
}

TEST(RemoteImageEditorTest, AnHTMLBlockImageIsNeverFetchedByAnything)
{
    Rig rig;
    auto editor = editorWith(QStringLiteral("<div><img src=\"https://example.com/a.png\"> <img src='https://example.com/b.png'></div>\n\n<figure><img src=\"//example.com/c.png\"></figure>\n"));
    editor->documentHeight();
    QTest::qWait(300);
    EXPECT_TRUE(rig.net.requests.isEmpty());
}

// --- remote icons: the same path as an image -----------------------------------------------------------------------

namespace {

const QColor kIconColour(0, 0, 255);

struct IconRig : Rig {
    IconRig()
    {
        Icons::setRemoteAllowed(false);
        Icons::clearCache();
    }
    ~IconRig() { Icons::setRemoteAllowed(false); }
};

} // namespace

TEST(RemoteIconTest, WhileRemoteContentIsOffNothingIsSent)
{
    IconRig rig;
    EXPECT_TRUE(Icons::icon(QStringLiteral("https://example.com/off.png"), kIconColour, 32).isNull());
    QTest::qWait(200);
    EXPECT_TRUE(rig.net.requests.isEmpty());
}

TEST(RemoteIconTest, AnIconIsFetchedUnderTheImageRulesAndDecodedInTheHelper)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    IconRig rig;
    Icons::setRemoteAllowed(true);
    const QString url = QStringLiteral("https://example.com/icon-ok.png");
    QSignalSpy ready(Icons::notifier(), SIGNAL(iconReady(QString)));
    QElapsedTimer clock;
    clock.start();
    EXPECT_TRUE(Icons::icon(url, kIconColour, 32).isNull()) << "never waits for the network";
    EXPECT_LT(clock.elapsed(), 1000);
    WAIT_TRUE(ready.count() == 1, 15000);
    EXPECT_EQ(ready.first().first().toString(), url);
    EXPECT_FALSE(Icons::icon(url, kIconColour, 32).isNull());
    ASSERT_EQ(rig.net.requests.size(), 1);
    const QNetworkRequest &request = rig.net.requests.first();
    EXPECT_EQ(request.header(QNetworkRequest::UserAgentHeader).toString(), QStringLiteral("Notolith"));
    EXPECT_EQ(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
    EXPECT_EQ(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
    EXPECT_EQ(request.rawHeader("Accept-Encoding"), QByteArray("identity"));
    EXPECT_FALSE(rig.images->cache().read(QUrl(url)).isEmpty()) << "the bytes are in the images' cache, not an icons directory";
}

TEST(RemoteIconTest, ARemoteSVGIsRefusedAndAnOldIconsCacheIsNeverRead)
{
    if (!skipUnlessSandbox().isEmpty())
        GTEST_SKIP() << qPrintable(skipUnlessSandbox());
    IconRig rig;
    Icons::setRemoteAllowed(true);
    const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><rect width='24' height='24' fill='currentColor'/></svg>";
    rig.image = svg;
    const QString url = QStringLiteral("https://example.com/icon-svg.svg");
    // What an older build left in the icons cache: decoded in this process, so it is never read now.
    QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString old = QDir(cacheHome.path()).filePath(QStringLiteral("notolith/icons/%1.svg").arg(QString::fromLatin1(hash)));
    QDir().mkpath(QFileInfo(old).absolutePath());
    QFile file(old);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(svg);
    file.close();

    QSignalSpy ready(Icons::notifier(), SIGNAL(iconReady(QString)));
    EXPECT_TRUE(Icons::icon(url, kIconColour, 32).isNull());
    WAIT_TRUE(rig.net.requests.size() == 1, 5000);
    QTest::qWait(400);
    EXPECT_EQ(ready.count(), 0);
    EXPECT_TRUE(Icons::icon(url, kIconColour, 32).isNull());
    EXPECT_EQ(rig.net.requests.size(), 1) << "a refusal is remembered, not retried on every paint";
    EXPECT_TRUE(rig.images->cache().read(QUrl(url)).isEmpty()) << "an SVG is not kept either";
}

TEST(RemoteIconTest, HttpLocalAndLoopbackIconsAreNeverRequested)
{
    IconRig rig;
    Icons::setRemoteAllowed(true);
    for (const char *url : {"http://example.com/i.png", "https://127.0.0.1/i.png", "https://169.254.169.254/i.png", "https://localhost/i.png",
                            "https://user:pw@example.com/i.png", "https://[64:ff9b:1::7f00:1]/i.png"}) {
        EXPECT_TRUE(Icons::icon(QString::fromLatin1(url), kIconColour, 32).isNull()) << url;
    }
    QTest::qWait(400);
    EXPECT_TRUE(rig.net.requests.isEmpty());
}

// --- timing, not a check ---------------------------------------------------------------------------------------------------

namespace {

// The helper kept running between images (--serve), here only to measure what that would save. Not
// shipped: a helper that stays alive is a helper a hostile image can wait in.
class WarmHelper {
public:
    WarmHelper()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        const QString helper = helperPath();
        sandbox::Command command = sandbox::bubblewrapCommand(m_dir->path(), helper, {QStringLiteral("--serve")}, {},
                                                              {QFileInfo(helper).absolutePath()});
        sandbox::Limits limits;
        limits.addressSpace = rlim_t(3) * 1024 * 1024 * 1024;
        m_process.setChildProcessModifier([limits, fd = command.filterFD] { sandbox::prepareChild(limits, fd); });
        m_process.start(command.program, command.arguments);
        ::close(command.filterFD);
        m_process.waitForStarted(5000);
    }
    DecodedImage decode(const QByteArray &bytes, ImageFormat format)
    {
        QByteArray frame;
        for (int shift = 0; shift < 32; shift += 8)
            frame.append(char((bytes.size() >> shift) & 0xff));
        frame += bytes;
        m_process.write(frame);
        QByteArray reply;
        qsizetype want = kHeaderSize;
        while (reply.size() < want) {
            if (!m_process.waitForReadyRead(10000))
                return {QImage(), QStringLiteral("no answer")};
            reply += m_process.readAllStandardOutput();
            if (want == kHeaderSize && reply.size() >= kHeaderSize) {
                const auto w = quint32(uchar(reply[8])) | quint32(uchar(reply[9])) << 8 | quint32(uchar(reply[10])) << 16 | quint32(uchar(reply[11])) << 24;
                const auto h = quint32(uchar(reply[12])) | quint32(uchar(reply[13])) << 8 | quint32(uchar(reply[14])) << 16 | quint32(uchar(reply[15])) << 24;
                if (reply[4] == 0)
                    want = qsizetype(kHeaderSize + qint64(w) * h * 4);
            }
        }
        return validateReply(reply, format);
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QProcess m_process;
};

double medianMs(std::function<void()> work, int runs)
{
    QList<double> times;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer clock;
        clock.start();
        work();
        times.append(clock.nsecsElapsed() / 1e6);
    }
    std::sort(times.begin(), times.end());
    return times.at(times.size() / 2);
}

} // namespace

// Run by hand: test_remote_images --gtest_also_run_disabled_tests --gtest_filter='*Latency*'
TEST(ImageDecoderTest, DISABLED_Latency)
{
    ASSERT_TRUE(skipUnlessSandbox().isEmpty());
    struct Sample {
        const char *name;
        QByteArray bytes;
        ImageFormat format;
    };
    const QList<Sample> samples = {{"1 MP PNG", encoded(photoLike(1000, 1000), "PNG"), ImageFormat::PNG},
                                   {"4 MP JPEG", encoded(photoLike(2000, 2000), "JPEG", 85), ImageFormat::JPEG}};
    WarmHelper warm;
    for (const Sample &sample : samples) {
        const double inProcess = medianMs([&] { QImage::fromData(sample.bytes); }, 9);
        const double cold = medianMs([&] { ASSERT_EQ(decodeInSandbox(sample.bytes, sample.format).error, QString()); }, 9);
        warm.decode(sample.bytes, sample.format); // the first one pays for the spawn
        const double warmMs = medianMs([&] { ASSERT_EQ(warm.decode(sample.bytes, sample.format).error, QString()); }, 9);
        std::printf("LATENCY %-9s %7lld bytes  in-process decode %6.1f ms  cold spawn %6.1f ms  warm %6.1f ms\n", sample.name,
                    static_cast<long long>(sample.bytes.size()), inProcess, cold, warmMs);
    }
    // What the spawn alone costs: an image the helper rejects from its first bytes.
    std::printf("LATENCY spawn only (not an image) %6.1f ms\n",
                medianMs([&] { decodeInSandbox(QByteArray("x"), ImageFormat::PNG); }, 9));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Run by hand, the helper is the one this build made.
    if (qEnvironmentVariableIsEmpty("NOTEAPPC_IMAGE_HELPER"))
        qputenv("NOTEAPPC_IMAGE_HELPER", NOTEAPPC_TEST_HELPER);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("noteAppC-test"));
    QCoreApplication::setApplicationName(QStringLiteral("noteAppC-test"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
