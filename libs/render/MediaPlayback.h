// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <memory>

namespace noteAppC {

/// Plays one audio or video file at a time, for `![[clip.mp4]]` and
/// `![[song.mp3]]` embeds, through Qt Multimedia. A video's frames arrive as
/// images for the editor to paint into the embed's block, so the player
/// scrolls with the note like any other block - no child widget to keep in
/// step with the view.
///
/// Optional: in a build without Qt Multimedia `isAvailable()` is false and
/// every call does nothing, and the editor offers to open the file instead.
class MediaPlayback : public QObject {
    Q_OBJECT

public:
    explicit MediaPlayback(QObject *parent = nullptr);
    ~MediaPlayback() override;

    static bool isAvailable();

    /// Starts `path`, or resumes it if it is the one already loaded; any other
    /// file that was playing stops.
    void play(const QString &path);
    void pause();
    void stop();
    void seek(qint64 milliseconds);

    QString path() const;
    bool isPlaying() const;
    qint64 position() const;
    qint64 duration() const;
    /// The latest video frame; null for audio or before the first frame.
    QImage frame() const;

signals:
    /// Position, state, duration or the frame moved on: time to repaint.
    void changed();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace noteAppC
