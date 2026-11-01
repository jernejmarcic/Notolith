// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "AudioRecorder.h"

#include <utility>

#ifdef NOTEAPPC_HAVE_MEDIA
#include <QAudioDevice>
#include <QAudioInput>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QUrl>
#endif

namespace noteAppC {

#ifdef NOTEAPPC_HAVE_MEDIA

namespace {

QMediaFormat formatFor(const QString &suffix)
{
    QMediaFormat format(suffix == QLatin1String("webm")  ? QMediaFormat::WebM
                        : suffix == QLatin1String("ogg") ? QMediaFormat::Ogg
                                                         : QMediaFormat::Mpeg4Audio);
    format.setAudioCodec(suffix == QLatin1String("m4a") ? QMediaFormat::AudioCodec::AAC
                                                        : QMediaFormat::AudioCodec::Opus);
    return format;
}

} // namespace

struct AudioRecorder::Impl {
    QMediaCaptureSession session;
    QAudioInput input;
    QMediaRecorder recorder;
    QString path;
};

AudioRecorder::AudioRecorder(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    d->session.setAudioInput(&d->input);
    d->session.setRecorder(&d->recorder);
    connect(&d->recorder, &QMediaRecorder::recorderStateChanged, this, [this](QMediaRecorder::RecorderState state) {
        if (state != QMediaRecorder::StoppedState || d->path.isEmpty())
            return;
        // The backend may have settled on another name than the one asked for.
        const QString asked = std::exchange(d->path, QString());
        const QString actual = d->recorder.actualLocation().toLocalFile();
        emit finished(actual.isEmpty() ? asked : actual, d->recorder.errorString());
    });
}

AudioRecorder::~AudioRecorder()
{
    d->recorder.stop();
}

bool AudioRecorder::isAvailable()
{
    return !QMediaDevices::audioInputs().isEmpty() && !fileSuffix().isEmpty();
}

QString AudioRecorder::fileSuffix()
{
    for (const QString &suffix : {QStringLiteral("webm"), QStringLiteral("ogg"), QStringLiteral("m4a")}) {
        if (formatFor(suffix).isSupported(QMediaFormat::Encode))
            return suffix;
    }
    return {};
}

bool AudioRecorder::start(const QString &path, QString *error)
{
    if (!isAvailable()) {
        if (error)
            *error = tr("No microphone, or no format this machine can record");
        return false;
    }
    if (isRecording())
        return true;
    d->input.setDevice(QMediaDevices::defaultAudioInput());
    d->recorder.setMediaFormat(formatFor(fileSuffix()));
    d->recorder.setQuality(QMediaRecorder::HighQuality);
    d->recorder.setOutputLocation(QUrl::fromLocalFile(path));
    d->path = path;
    d->recorder.record();
    return true;
}

void AudioRecorder::stop()
{
    d->recorder.stop();
}

bool AudioRecorder::isRecording() const
{
    return d->recorder.recorderState() == QMediaRecorder::RecordingState;
}

#else

struct AudioRecorder::Impl {};

AudioRecorder::AudioRecorder(QObject *parent) : QObject(parent) {}
AudioRecorder::~AudioRecorder() = default;
bool AudioRecorder::isAvailable() { return false; }
QString AudioRecorder::fileSuffix() { return {}; }
bool AudioRecorder::start(const QString &, QString *error)
{
    if (error)
        *error = tr("This build has no Qt Multimedia");
    return false;
}
void AudioRecorder::stop() {}
bool AudioRecorder::isRecording() const { return false; }

#endif

} // namespace noteAppC
