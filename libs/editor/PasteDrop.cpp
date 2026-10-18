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
/// Pasted HTML is left as plain text: Obsidian converts it to Markdown, and
/// that conversion is not built.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

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

} // namespace

void MarkdownEditor::setAttachmentSaver(AttachmentSaver saver)
{
    d->m_attachmentSaver = std::move(saver);
}

void MarkdownEditor::setFileImporter(FileImporter importer)
{
    d->m_fileImporter = std::move(importer);
}

void MarkdownEditor::insertMimeData(const QMimeData *mime)
{
    if (!mime)
        return;

    if (const QByteArray bytes = imageBytes(mime); !bytes.isEmpty()) {
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
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
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
    else if (mime->hasText())
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
    moveCursor(offsetAt(event->position().toPoint()), false);
    insertMimeData(event->mimeData());
    event->acceptProposedAction();
}

} // namespace noteAppC
