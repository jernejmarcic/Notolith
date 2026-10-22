#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include "CrdtDoc.h"

namespace noteAppC {

class MarkdownEditor;
class ShareEngine;

/// One shared note in the window: keeps the editors showing it and the
/// engine's document (docs/SHARE.md) in step, in both directions. Alive for
/// as long as the engine knows the share, whether or not a tab shows the note;
/// UI thread, like the engine and its documents.
///
/// An editor's own edits arrive through its edited() signal and go out as
/// updates; the other editors showing the same note get them from
/// MainWindow's existing fan-out (connected first), not from here. Edits from
/// other people arrive as ShareEngine::remoteEdits and are made in every
/// editor with applyExternalEdit(), which emits no edited(): nothing loops.
class ShareSession : public QObject {
    Q_OBJECT
public:
    ShareSession(ShareEngine *engine, QByteArray shareID, QObject *parent = nullptr);

    const QByteArray &shareID() const { return m_id; }
    QString text() const;
    int peersHere() const;

    /// A file name (without ".md") for a note someone else called `title`: no
    /// path separators, no "..", no leading dots, at most 100 characters; the
    /// share id's first 8 hex digits when nothing is left.
    static QString fileNameFor(const QString &title, const QByteArray &shareID);

    /// Shows the note in `view`, whose text is `diskText`, what the file
    /// holds. The first view after none had folded in what the file gained
    /// since it and the document last agreed (an edit made while the
    /// application was closed); the view then takes the document's text,
    /// which also holds what others typed meanwhile. False when the file's
    /// change overlapped the document's own edits and was left out, the
    /// document winning. Attaching an attached view does nothing.
    bool attach(MarkdownEditor *view, const QString &diskText);
    /// Stops following every view not in `keep` (a tab moved to another note).
    void retain(const QList<MarkdownEditor *> &keep);
    bool hasViews() const;

    /// The file changed under the note: `disk` is its text now, `base` what
    /// it held before. The change, found as one hunk, is made in the
    /// document and in every view, and sent on. False, and nothing done, when
    /// it overlaps what the document gained since `base`: the caller then
    /// keeps what the editors show.
    bool foldInDisk(const QString &base, const QString &disk);
    /// The editors were set to `text` without their edited() signal (a note
    /// restored over the file): the document follows, as one local edit.
    void adoptText(const QString &text);
    /// The file now holds `text`, which the document does too.
    void noteSaved(const QString &text);

signals:
    /// A batch from other people, or a fold-in, was made in the views: they
    /// differ from the file now; the window arms its autosave.
    void remoteApplied();

private:
    void onLocalEdit(MarkdownEditor *from, qsizetype at, qsizetype removed, const QString &inserted);
    void onRemoteEdits(const QByteArray &shareID, const QList<CrdtEdit> &edits);
    void applyToViews(qsizetype at, qsizetype removed, const QString &inserted);
    /// Makes every view hold the document's text.
    void followDocument();
    void send(const QByteArray &update);
    /// The other members' carets, in every view.
    void showCarets();
    void sendCaret(MarkdownEditor *view);

    ShareEngine *m_engine;
    QByteArray m_id;
    QList<QPointer<MarkdownEditor>> m_views;
};

} // namespace noteAppC
