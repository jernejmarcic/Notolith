// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "RemoteImages.h"

#include "ImageDecoder.h"
#include "RemoteImagePolicy.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>

namespace noteAppC::remoteimage {

namespace {
constexpr int kFetchesAtOnce = 4;
constexpr int kDecodesAtOnce = 2;
}

RemoteImages::RemoteImages(const Config &config, QObject *parent)
    : QObject(parent), m_allowlist(config.allowlistPath), m_cache(config.cacheDir), m_fetcher(config.manager, this)
{
    m_decoders.setMaxThreadCount(kDecodesAtOnce);
}

namespace {
RemoteImages *gReplacement = nullptr;
}

void RemoteImages::useForTests(RemoteImages *replacement)
{
    gReplacement = replacement;
}

RemoteImages &RemoteImages::instance()
{
    if (gReplacement)
        return *gReplacement;
    static RemoteImages images({ImageAllowlist::defaultPath(), ImageCache::defaultDir(), nullptr});
    return images;
}

void RemoteImages::answer(const QUrl &url, const QImage &image, const QString &error)
{
    m_pending.remove(url);
    if (error.isEmpty())
        emit loaded(url, image);
    else
        emit failed(url, error);
}

void RemoteImages::load(const QUrl &url, bool sameOriginOnly)
{
    if (m_pending.contains(url))
        return;
    m_pending.insert(url);
    const auto refuse = [this, url](const QString &why) {
        QTimer::singleShot(0, this, [this, url, why] { answer(url, {}, why); });
    };
    if (const QString why = refusal(url); !why.isEmpty()) {
        refuse(why);
        return;
    }
    // No download for an image that could not be decoded afterwards.
    if (const QString why = decoderUnavailable(); !why.isEmpty()) {
        refuse(why);
        return;
    }
    const QByteArray cached = m_cache.read(url);
    if (!cached.isEmpty() && sniffFormat(cached) != ImageFormat::None) {
        decode(url, cached); // the original goes through the helper like any other
        return;
    }
    m_waiting.emplace_back(url, sameOriginOnly);
    pump();
}

void RemoteImages::pump()
{
    while (m_fetching < kFetchesAtOnce && !m_waiting.empty()) {
        const auto [url, sameOriginOnly] = m_waiting.front();
        m_waiting.pop_front();
        startFetch(url, sameOriginOnly);
    }
}

void RemoteImages::startFetch(const QUrl &url, bool sameOriginOnly)
{
    ++m_fetching;
    m_fetcher.fetch(url, [this, url](ImageFetcher::Result result) {
        --m_fetching;
        pump();
        if (!result.error.isEmpty()) {
            answer(url, {}, result.error);
            return;
        }
        if (sniffFormat(result.bytes) == ImageFormat::None) {
            answer(url, {}, QCoreApplication::translate("RemoteImage", "Not loaded: not a PNG, JPEG, GIF or WebP image."));
            return;
        }
        m_cache.write(url, result.bytes);
        decode(url, result.bytes);
    }, sameOriginOnly);
}

void RemoteImages::decode(const QUrl &url, const QByteArray &bytes)
{
    const ImageFormat format = sniffFormat(bytes);
    QPointer<RemoteImages> self(this);
    (void)QtConcurrent::run(&m_decoders, [self, url, bytes, format] {
        const DecodedImage decoded = decodeInSandbox(bytes, format);
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, url, decoded] {
            if (self)
                self->answer(url, decoded.image, decoded.error);
        }, Qt::QueuedConnection);
    });
}

} // namespace noteAppC::remoteimage
