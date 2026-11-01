// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
/// Paste and drop, as Obsidian does: an image pasted or dropped is saved as
/// an attachment and embedded, a file dropped from outside the vault is
/// copied in and embedded, a file already in the vault is linked without
/// copying, and plain text is inserted as text. The editor does not know the
/// vault - saving an image's bytes and importing a dropped file's path are
/// both handed back to the window through m_attachmentSaver/m_fileImporter
/// (setAttachmentSaver/setFileImporter). With neither set, an image paste or
/// drop does nothing and a dropped file's path is inserted as text, so a
/// vault-less editor (a loose file, a test) is unaffected.
///
/// Pasted rich text (text/html) is converted to Markdown (PasteHTML.cpp), as
/// Obsidian does, except inside a fenced block; Ctrl+Shift+V pastes the plain
/// text. Text copied from the editor itself carries no HTML, so it goes in as is.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"
#include "PasteHTML.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QUrl>

namespace noteAppC {

namespace {

/// Obsidian's name for a pasted image; a second paste in the same second
/// gets " 1" from the attachment saver, the way the audio recorder's own
/// names do.
QString pastedImageName()
{
    return QStringLiteral("Pasted image ")
         + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"))
         + QStringLiteral(".png");
}

/// PNG bytes for whatever image `mime` carries, empty when it carries none.
QByteArray imageBytes(const QMimeData *mime)
{
    QImage image;
    if (mime->hasImage())
        image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull() && mime->hasFormat(QStringLiteral("image/png")))
        image.loadFromData(mime->data(QStringLiteral("image/png")), "PNG");
    if (image.isNull())
        return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

/// The schemes a link in a note is followed for (MainWindow::followLink): any other URL is dropped as plain text.
bool isLinkScheme(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto");
}

/// A browser's dropped link as Markdown. Firefox puts "URL\ntitle" pairs (UTF-16) in text/x-moz-url;
/// Chrome puts the page title in text/plain beside the URL on some platforms. Failing both, the host
/// (the address for mailto:), never the whole URL.
QString browserLink(const QUrl &url, const QHash<QString, QString> &mozTitles, const QString &plain, bool alone)
{
    QString title = mozTitles.value(url.toString());
    if (title.isEmpty() && alone && !plain.isEmpty() && !plain.contains(QLatin1Char('\n')) && QUrl(plain) != url)
        title = plain;
    if (title.isEmpty())
        title = url.scheme().toLower() == QLatin1String("mailto") ? url.path() : url.host();
    title.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('['), QStringLiteral("\\["))
        .replace(QLatin1Char(']'), QStringLiteral("\\]"));
    QString target = QString::fromLatin1(url.toEncoded());
    target.replace(QLatin1Char('('), QStringLiteral("%28")).replace(QLatin1Char(')'), QStringLiteral("%29"));
    return QLatin1Char('[') + title + QStringLiteral("](") + target + QLatin1Char(')');
}

} // namespace

void MarkdownEditor::setAttachmentSaver(AttachmentSaver saver)
{
    d->m_attachmentSaver = std::move(saver);
}

void MarkdownEditor::setFileImporter(FileImporter importer)
{
    d->m_fileImporter = std::move(importer);
}

void MarkdownEditor::insertMimeData(const QMimeData *mime, bool dropped)
{
    if (!mime)
        return;

    // Rich text with text beside it (a spreadsheet, a page) is pasted as text even when
    // the source also offers a picture of it; an image alone (Chrome's "copy image",
    // an <img> in the HTML and no text) is still an attachment.
    const bool richText = mime->hasHtml() && !mime->text().trimmed().isEmpty();
    if (const QByteArray bytes = richText ? QByteArray() : imageBytes(mime); !bytes.isEmpty()) {
        if (d->m_attachmentSaver) {
            const QString link = d->m_attachmentSaver(pastedImageName(), bytes);
            if (!link.isEmpty())
                insertText(cellSafe(link));
        }
        // No saver configured: an image paste/drop does nothing.
        return;
    }

    if (mime->hasUrls()) {
        QStringList parts;
        QHash<QString, QString> mozTitles;
        if (const QByteArray moz = mime->data(QStringLiteral("text/x-moz-url")); !moz.isEmpty()) {
            QString text = QString::fromUtf16(reinterpret_cast<const char16_t *>(moz.constData()), moz.size() / 2);
            text.remove(QChar(0)).remove(QChar(0xFEFF)); // a NUL-terminated buffer, a byte-order mark
            const QStringList lines = text.split(QLatin1Char('\n'));
            for (qsizetype i = 0; i + 1 < lines.size(); i += 2)
                mozTitles.insert(QUrl(lines.at(i).trimmed()).toString(), lines.at(i + 1).trimmed());
        }
        const QList<QUrl> urls = mime->urls();
        const QString plain = mime->text().trimmed();
        for (const QUrl &url : urls) {
            if (!url.isLocalFile()) {
                if (dropped && url.isValid() && isLinkScheme(url)) {
                    parts << browserLink(url, mozTitles, plain, urls.size() == 1);
                } else if (url.isValid() && (dropped || selection().length() == 0)) {
                    // A paste is the URL as copied; a drop of javascript:, data: ... is text, never a link.
                    parts << url.toString();
                    if (url.toString().size() > 2000)
                        emit noticeRaised(tr("A %n character URL was inserted as plain text", nullptr, int(url.toString().size())));
                }
                continue;
            }
            const QString path = url.toLocalFile();
            if (d->m_fileImporter) {
                const QString link = d->m_fileImporter(path);
                if (!link.isEmpty())
                    parts << link;
            } else {
                parts << path; // no importer: the path itself, as text
            }
        }
        if (!parts.isEmpty())
            insertText(cellSafe(parts.join(QLatin1Char('\n'))));
        return;
    }

    // A copied cell rectangle carries its pipe table as text/markdown beside the
    // TSV a spreadsheet takes; a note wants the table.
    if (mime->hasFormat(QStringLiteral("text/markdown")))
        insertText(cellSafe(QString::fromUtf8(mime->data(QStringLiteral("text/markdown")))));
    else if (mime->hasHtml() && !inCodeFenceAt(cursorOffset())) {
        const QString plain = mime->hasText() ? mime->text() : QString();
        const QString md = HTMLtoMarkdown(mime->html(), plain);
        if (!md.isNull())
            insertText(cellSafe(md));
        else if (!plain.isEmpty())
            insertText(cellSafe(plain));
    } else if (mime->hasText())
        insertText(cellSafe(mime->text()));
}

void MarkdownEditor::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (d->m_mode == Mode::Reading || !mime
        || !(mime->hasUrls() || mime->hasText() || mime->hasImage()))
        return;
    event->acceptProposedAction();
}

void MarkdownEditor::dragMoveEvent(QDragMoveEvent *event)
{
    if (d->m_mode == Mode::Reading)
        return;
    event->acceptProposedAction();
    // The drop indicator caret: the real caret, moved to track the pointer.
    // Nothing is edited by a drag that never drops, so there is nothing to
    // restore if it leaves without one.
    moveCursor(offsetAt(event->position().toPoint()), false);
}

void MarkdownEditor::dropEvent(QDropEvent *event)
{
    if (d->m_mode == Mode::Reading) {
        event->ignore();
        return;
    }
    const TypingScope typed(d->m_typing);
    moveCursor(offsetAt(event->position().toPoint()), false);
    insertMimeData(event->mimeData(), true);
    event->acceptProposedAction();
}

} // namespace noteAppC
