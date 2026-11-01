// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "ShareSession.h"

#include <algorithm>

#include <QColor>

#include "MarkdownEditor.h"
#include "ShareEngineBase.h"

namespace noteAppC {

namespace {
constexpr qsizetype kMaxFileNameChars = 100;
}

ShareSession::ShareSession(ShareEngineBase *engine, QByteArray shareID, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_id(std::move(shareID))
{
    connect(engine, &ShareEngineBase::remoteEdits, this, &ShareSession::onRemoteEdits);
    connect(engine, &ShareEngineBase::presenceChanged, this, [this](const QByteArray &id) {
        if (id == m_id)
            showCarets();
    });
}

void ShareSession::sendCaret(MarkdownEditor *view)
{
    if (view->hasFocus())
        m_engine->setCaret(m_id, view->anchorOffset(), view->cursorOffset());
}

void ShareSession::showCarets()
{
    QList<MarkdownEditor::RemoteCaret> carets;
    for (const ShareEngineBase::Caret &caret : m_engine->carets(m_id)) {
        // One colour per device, the same on every screen that shows it.
        const int hue = caret.device.isEmpty() ? 0 : quint8(caret.device.at(0)) * 360 / 256;
        carets.append({caret.name, QColor::fromHsv(hue, 170, 230), caret.anchor, caret.head});
    }
    for (MarkdownEditor *view : std::as_const(m_views))
        if (view)
            view->setRemoteCarets(carets);
}

QString ShareSession::text() const
{
    const CrdtDoc *doc = m_engine->doc(m_id);
    return doc ? doc->text() : QString();
}

int ShareSession::peersHere() const
{
    return m_engine->peersHere(m_id);
}

QString ShareSession::fileNameFor(const QString &title, const QByteArray &shareID)
{
    QString name;
    for (const QChar c : title) {
        if (c != u'/' && c != u'\\' && c.unicode() >= 0x20 && c.unicode() != 0x7f)
            name += c;
    }
    while (name.contains(QLatin1String("..")))
        name.remove(QLatin1String(".."));
    name = name.trimmed();
    while (name.startsWith(u'.'))
        name.remove(0, 1);
    if (name.size() > kMaxFileNameChars) {
        name.truncate(kMaxFileNameChars);
        if (name.back().isHighSurrogate())
            name.chop(1);
    }
    name = name.trimmed();
    return name.isEmpty() ? QString::fromLatin1(shareID.toHex().left(8)) : name;
}

bool ShareSession::hasViews() const
{
    return std::any_of(m_views.cbegin(), m_views.cend(), [](const auto &view) { return !view.isNull(); });
}

bool ShareSession::attach(MarkdownEditor *view, const QString &diskText)
{
    CrdtDoc *doc = m_engine->doc(m_id);
    if (!doc || m_views.contains(view))
        return true;
    bool folded = true;
    if (!hasViews()) {
        m_views.clear();
        if (const QString base = m_engine->base(m_id); diskText != base)
            folded = foldInDisk(base, diskText);
    }
    m_views.append(view);
    connect(view, &MarkdownEditor::edited, this,
            [this, view](qsizetype at, qsizetype removed, const QString &inserted) {
                onLocalEdit(view, at, removed, inserted);
                sendCaret(view); // typing moves the caret without a cursorMoved
                showCarets(); // a caret typed at stays ahead of the text, as its position says, not after it
            });
    connect(view, &MarkdownEditor::cursorMoved, this, [this, view] { sendCaret(view); });
    const QString now = doc->text();
    if (view->noteText() != now) {
        const MarkdownEditor::Hunk hunk = MarkdownEditor::hunkBetween(view->noteText(), now);
        view->applyExternalEdit(hunk.at, hunk.removed, hunk.inserted);
        emit remoteApplied();
    }
    showCarets();
    return folded;
}

void ShareSession::retain(const QList<MarkdownEditor *> &keep)
{
    for (qsizetype i = m_views.size() - 1; i >= 0; --i) {
        MarkdownEditor *view = m_views.at(i);
        if (view && keep.contains(view))
            continue;
        if (view) {
            disconnect(view, nullptr, this, nullptr);
            view->setRemoteCarets({});
        }
        m_views.removeAt(i);
    }
    if (!hasViews())
        m_engine->clearCaret(m_id); // nobody here looks at the note: the others stop drawing our caret
}

void ShareSession::send(const QByteArray &update)
{
    m_engine->broadcast(m_id, update);
}

void ShareSession::applyToViews(qsizetype at, qsizetype removed, const QString &inserted, bool sendCarets)
{
    for (const QPointer<MarkdownEditor> &view : std::as_const(m_views)) {
        if (view) {
            view->applyExternalEdit(at, removed, inserted);
            if (sendCarets)
                sendCaret(view); // the text moved under it: the others hold the offset from before
        }
    }
}

void ShareSession::followDocument()
{
    const QString now = text();
    for (const QPointer<MarkdownEditor> &view : std::as_const(m_views)) {
        if (!view || view->noteText() == now)
            continue;
        const MarkdownEditor::Hunk hunk = MarkdownEditor::hunkBetween(view->noteText(), now);
        view->applyExternalEdit(hunk.at, hunk.removed, hunk.inserted);
        sendCaret(view);
    }
}

void ShareSession::onLocalEdit(MarkdownEditor *from, qsizetype at, qsizetype removed, const QString &inserted)
{
    CrdtDoc *doc = m_engine->doc(m_id);
    if (!doc)
        return;
    const QString &now = from->noteText(); // after the edit
    const qsizetype end = at + inserted.size();
    if (at == 0 && end == now.size()) {
        // The whole text changed hands: an undo or redo, or select-all and
        // type. What others typed meanwhile must stay, so send the difference.
        adoptText(now);
        return;
    }

    // The document takes no edit that splits a surrogate pair (Yrs rounds
    // the offsets to the pair's end and so deletes or keeps the wrong unit)
    // and stores a lone surrogate as is or as U+FFFD; the editor has no such
    // rule (a one-unit Delete or Backspace on an emoji). Such an edit is
    // widened to the whole pair for the document, an insertion between the
    // halves goes after the pair, and the views then follow the document, so
    // the two never differ. The text is only fetched when the neighbourhood of
    // the edit shows halves of a pair.
    bool care = (at > 0 && now.at(at - 1).isHighSurrogate()) || (end < now.size() && now.at(end).isLowSurrogate());
    for (const QChar c : inserted)
        care = care || c.isSurrogate();
    qsizetype wideAt = at;
    qsizetype wideRemoved = removed;
    if (care) {
        const QString before = doc->text();
        const auto splits = [&](qsizetype i) {
            return i > 0 && i < before.size() && before.at(i - 1).isHighSurrogate() && before.at(i).isLowSurrogate();
        };
        if (removed == 0) {
            if (splits(at))
                ++wideAt;
        } else {
            if (splits(at)) {
                --wideAt;
                ++wideRemoved;
            }
            if (splits(at + removed))
                ++wideRemoved;
        }
    }
    const QByteArray update = doc->localEdit(wideAt, wideRemoved, inserted);
    if (update.isEmpty() && (removed > 0 || !inserted.isEmpty())) {
        adoptText(now); // refused: the editor and the document were not in step
        return;
    }
    send(update);
    if (care && doc->text() != now)
        followDocument();
}

void ShareSession::adoptText(const QString &text)
{
    CrdtDoc *doc = m_engine->doc(m_id);
    if (!doc)
        return;
    const QString before = doc->text();
    if (before == text)
        return;
    const MarkdownEditor::Hunk hunk = MarkdownEditor::hunkBetween(before, text);
    send(doc->localEdit(hunk.at, hunk.removed, hunk.inserted));
}

void ShareSession::onRemoteEdits(const QByteArray &shareID, const QList<CrdtEdit> &edits)
{
    if (shareID != m_id || !hasViews())
        return;
    // The caret goes out once, after the whole batch: the document is ahead of the view
    // until the last edit is in, and a position encoded then would name text the view lacks.
    for (const CrdtEdit &edit : edits)
        applyToViews(edit.at, edit.removed, edit.inserted, false);
    for (const QPointer<MarkdownEditor> &view : std::as_const(m_views))
        if (view)
            sendCaret(view);
    showCarets(); // held by their characters: they stand where those characters went
    emit remoteApplied();
}

bool ShareSession::foldInDisk(const QString &base, const QString &disk)
{
    CrdtDoc *doc = m_engine->doc(m_id);
    if (!doc)
        return false;
    const MarkdownEditor::Hunk fileEdit = MarkdownEditor::hunkBetween(base, disk);
    if (fileEdit.removed == 0 && fileEdit.inserted.isEmpty())
        return true;
    const MarkdownEditor::Hunk own = MarkdownEditor::hunkBetween(base, doc->text()); // in the file's coordinates
    qsizetype at = fileEdit.at;
    if (fileEdit.at + fileEdit.removed > own.at) {
        if (fileEdit.at < own.at + own.removed)
            return false;
        at += own.inserted.size() - own.removed;
    }
    send(doc->localEdit(at, fileEdit.removed, fileEdit.inserted));
    applyToViews(at, fileEdit.removed, fileEdit.inserted);
    // The document holds everything the file does now, so the next fold-in
    // starts from this text, not from the one before.
    m_engine->persist(m_id, disk);
    if (hasViews())
        emit remoteApplied();
    return true;
}

void ShareSession::noteSaved(const QString &text)
{
    adoptText(text);
    m_engine->persist(m_id, text);
}

} // namespace noteAppC
