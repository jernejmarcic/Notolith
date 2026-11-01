// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "MediaPlayback.h"

#ifdef NOTEAPPC_HAVE_MEDIA
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#endif

namespace noteAppC {

struct MediaPlayback::Private {
    QString path;
    QImage frame;
#ifdef NOTEAPPC_HAVE_MEDIA
    QMediaPlayer player;
    QAudioOutput audio;
    QVideoSink sink;
#endif
};

MediaPlayback::MediaPlayback(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
#ifdef NOTEAPPC_HAVE_MEDIA
    d->player.setAudioOutput(&d->audio);
    d->player.setVideoSink(&d->sink);
    connect(&d->player, &QMediaPlayer::positionChanged, this, &MediaPlayback::changed);
    connect(&d->player, &QMediaPlayer::durationChanged, this, &MediaPlayback::changed);
    connect(&d->player, &QMediaPlayer::playbackStateChanged, this, &MediaPlayback::changed);
    connect(&d->sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        d->frame = frame.toImage();
        emit changed();
    });
#endif
}

MediaPlayback::~MediaPlayback() = default;

bool MediaPlayback::isAvailable()
{
#ifdef NOTEAPPC_HAVE_MEDIA
    return true;
#else
    return false;
#endif
}

void MediaPlayback::play(const QString &path)
{
#ifdef NOTEAPPC_HAVE_MEDIA
    if (path != d->path) {
        d->player.stop();
        d->frame = QImage();
        d->path = path;
        d->player.setSource(QUrl::fromLocalFile(path));
    }
    d->player.play();
#else
    Q_UNUSED(path);
#endif
}

void MediaPlayback::pause()
{
#ifdef NOTEAPPC_HAVE_MEDIA
    d->player.pause();
#endif
}

void MediaPlayback::stop()
{
#ifdef NOTEAPPC_HAVE_MEDIA
    d->player.stop();
#endif
}

void MediaPlayback::seek(qint64 milliseconds)
{
#ifdef NOTEAPPC_HAVE_MEDIA
    d->player.setPosition(milliseconds);
#else
    Q_UNUSED(milliseconds);
#endif
}

QString MediaPlayback::path() const
{
    return d->path;
}

bool MediaPlayback::isPlaying() const
{
#ifdef NOTEAPPC_HAVE_MEDIA
    return d->player.playbackState() == QMediaPlayer::PlayingState;
#else
    return false;
#endif
}

qint64 MediaPlayback::position() const
{
#ifdef NOTEAPPC_HAVE_MEDIA
    return d->player.position();
#else
    return 0;
#endif
}

qint64 MediaPlayback::duration() const
{
#ifdef NOTEAPPC_HAVE_MEDIA
    return d->player.duration();
#else
    return 0;
#endif
}

QImage MediaPlayback::frame() const
{
    return d->frame;
}

} // namespace noteAppC
