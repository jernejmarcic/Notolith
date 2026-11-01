#include "ImageFetcher.h"

#include "ImageWire.h"
#include "RemoteImagePolicy.h"

#include <QCoreApplication>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>

#include <memory>

namespace noteAppC::remoteimage {

namespace {

// Keeps nothing and sends nothing.
class NoCookies : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
};

} // namespace

struct ImageFetcher::Job {
    std::function<void(Result)> done;
    QByteArray buffer;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> deadline;
    bool finished = false;
};

ImageFetcher::ImageFetcher(QNetworkAccessManager *manager, QObject *parent)
    : QObject(parent), m_manager(manager ? manager : new QNetworkAccessManager(this))
{
    m_manager->setCookieJar(new NoCookies(m_manager));
    m_resolver = [this](const QString &host, std::function<void(const QList<QHostAddress> &)> done) {
        QHostInfo::lookupHost(host, this, [done](const QHostInfo &info) {
            done(info.error() == QHostInfo::NoError ? info.addresses() : QList<QHostAddress>());
        });
    };
}

void ImageFetcher::fetch(const QUrl &url, std::function<void(Result)> done)
{
    auto job = std::make_shared<Job>();
    job->done = std::move(done);
    job->deadline = new QTimer(this);
    job->deadline->setSingleShot(true);
    connect(job->deadline, &QTimer::timeout, this, [this, job] { finish(job, {{}, tr("Not loaded: the server took over 30 seconds.")}); });
    job->deadline->start(m_timeoutMs);
    // From the event loop, so a refusal never lands inside the caller's own layout.
    QTimer::singleShot(0, this, [this, job, url] { step(job, url, 0); });
}

void ImageFetcher::finish(const std::shared_ptr<Job> &job, Result result)
{
    if (job->finished)
        return;
    job->finished = true;
    if (job->deadline)
        job->deadline->deleteLater();
    if (job->reply) {
        job->reply->disconnect();
        job->reply->abort();
        job->reply->deleteLater();
    }
    job->done(std::move(result));
}

void ImageFetcher::step(const std::shared_ptr<Job> &job, const QUrl &url, int hops)
{
    if (job->finished)
        return;
    if (const QString why = refusal(url); !why.isEmpty()) {
        finish(job, {{}, why});
        return;
    }
    QString host = url.host();
    const QHostAddress literal(host);
    if (!literal.isNull()) { // refusal() has already judged a literal
        request(job, url, hops);
        return;
    }
    m_resolver(host, [this, job, url, hops, host](const QList<QHostAddress> &addresses) {
        if (job->finished)
            return;
        if (addresses.isEmpty()) {
            finish(job, {{}, tr("Not loaded: could not find %1.").arg(host)});
            return;
        }
        for (const QHostAddress &address : addresses) {
            if (isInternalAddress(address)) {
                finish(job, {{}, tr("Not loaded: %1 is a local address.").arg(host)});
                return;
            }
        }
        request(job, url, hops);
    });
}

void ImageFetcher::request(const std::shared_ptr<Job> &job, const QUrl &url, int hops)
{
    QNetworkRequest request(url);
    // Followed here, hop by hop, so every host is checked.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Notolith"));
    // No compression: the bytes counted are the bytes that arrive, and a compressed body is no image.
    request.setRawHeader("Accept-Encoding", "identity");
    QNetworkReply *reply = m_manager->get(request);
    job->reply = reply;
    job->buffer.clear();
    const auto tooBig = [this, job] { finish(job, {{}, tr("Not loaded: the image is over 20 MiB.")}); };
    connect(reply, &QNetworkReply::downloadProgress, this, [tooBig](qint64 received, qint64 total) {
        if (received > kMaxBodyBytes || total > kMaxBodyBytes)
            tooBig();
    });
    connect(reply, &QNetworkReply::readyRead, this, [job, reply, tooBig] {
        job->buffer += reply->readAll();
        if (job->buffer.size() > kMaxBodyBytes)
            tooBig();
    });
    connect(reply, &QNetworkReply::finished, this, [this, job, reply, hops] {
        if (job->finished)
            return;
        job->buffer += reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400 && reply->hasRawHeader("Location")) {
            const QUrl next = reply->url().resolved(QUrl(QString::fromUtf8(reply->rawHeader("Location"))));
            job->reply = nullptr;
            reply->deleteLater();
            if (hops >= kMaxRedirects)
                finish(job, {{}, tr("Not loaded: too many redirects.")});
            else
                step(job, next, hops + 1); // step() applies every rule again, https included
            return;
        }
        if (job->buffer.size() > kMaxBodyBytes)
            finish(job, {{}, tr("Not loaded: the image is over 20 MiB.")});
        else if (reply->error() != QNetworkReply::NoError || status != 200)
            finish(job, {{}, tr("Not loaded: the server answered %1.").arg(status ? QString::number(status) : reply->errorString())});
        else
            finish(job, {std::move(job->buffer), {}});
    });
}

} // namespace noteAppC::remoteimage
