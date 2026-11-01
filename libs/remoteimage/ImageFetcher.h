// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// Downloads one remote image's bytes, under the rules of a note's image: https only, no user:pass, no
// internal address (checked on the literal and on every address a name resolves to, at every redirect),
// at most 3 redirects, no cookies, no credentials, no Referer, User-Agent "Notolith", at most 20 MiB and
// 30 s in all. It looks at no image: what the bytes are is the caller's question.
//
// Limit: a name is resolved here for the check and again by the network manager for the request, so a
// name that changes in between (DNS rebinding) gets through. The same limit ReferenceBlock.cpp has;
// pinning the address needs a socket of our own.

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkAccessManager;

namespace noteAppC::remoteimage {

class ImageFetcher : public QObject {
    Q_OBJECT
public:
    struct Result {
        QByteArray bytes;
        QString error; ///< empty on success
    };
    /// Resolves `host` and calls `done` (from the event loop or at once) with its addresses, none on failure.
    using Resolver = std::function<void(const QString &host, std::function<void(const QList<QHostAddress> &)> done)>;

    static constexpr int kMaxRedirects = 3;
    static constexpr int kTimeoutMs = 30'000;

    /// `manager` is used as it is, apart from its cookie jar, which is replaced by one that keeps nothing;
    /// a test hands in one that answers from memory. Without one the fetcher makes its own.
    explicit ImageFetcher(QNetworkAccessManager *manager = nullptr, QObject *parent = nullptr);
    void setResolver(Resolver resolver) { m_resolver = std::move(resolver); }
    void setTimeoutForTests(int ms) { m_timeoutMs = ms; }

    /// Calls `done` once, from the event loop. Never when the fetcher is gone first. With `sameOriginOnly` (a
    /// load nobody clicked: an origin the person allowed earlier) a redirect to another origin is not followed:
    /// the answer is an error naming the host it went to, and nothing is sent there.
    void fetch(const QUrl &url, std::function<void(Result)> done, bool sameOriginOnly = false);

private:
    struct Job;
    void step(const std::shared_ptr<Job> &job, const QUrl &url, int hops);
    void request(const std::shared_ptr<Job> &job, const QUrl &url, int hops);
    void finish(const std::shared_ptr<Job> &job, Result result);

    QNetworkAccessManager *m_manager;
    Resolver m_resolver;
    int m_timeoutMs = kTimeoutMs;
};

} // namespace noteAppC::remoteimage
