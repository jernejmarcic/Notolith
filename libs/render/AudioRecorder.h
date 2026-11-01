// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace noteAppC {

/// Records the default microphone into one file, for Obsidian's audio
/// recorder, through Qt Multimedia. Only ever on an explicit start(); a
/// build without Qt Multimedia, or a machine with no input, is not
/// available and start() says why.
class AudioRecorder : public QObject {
    Q_OBJECT

public:
    explicit AudioRecorder(QObject *parent = nullptr);
    ~AudioRecorder() override;

    static bool isAvailable();
    /// The container this machine can write: "webm" as Obsidian's, else
    /// "ogg", else "m4a"; empty when it can write none of them.
    static QString fileSuffix();

    /// `path` should end in fileSuffix().
    bool start(const QString &path, QString *error = nullptr);
    void stop();
    bool isRecording() const;

signals:
    /// The file is complete - or, with `error` set, is not.
    void finished(const QString &path, const QString &error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace noteAppC
