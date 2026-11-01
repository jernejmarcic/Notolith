// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
/// A remote image in a note: `![alt](https://host/x.png)` or `<img src="https://...">`. Only in a build with
/// NOTEAPPC_WITH_NETWORK; the default build shows such an image as the text it is.
///
/// Laying a note out never asks the network for anything. An image shows as a small placeholder, the host
/// and a "Load image" button (and "Always load from <host>" when there is room), until the person clicks
/// one, or until its origin was remembered by an earlier "Always". The fetch, the cache, the rules for what
/// may be fetched and the sandboxed decode are libs/remoteimage; this file draws the placeholder and puts
/// the picture in the block when it arrives.
///
/// An image in a table cell is not drawn: it stays the source text it is in a build without the option.

#ifdef NOTEAPPC_HAVE_NETWORK

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include "RemoteImagePolicy.h"
#include "RemoteImages.h"

#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace noteAppC {

namespace {

using remoteimage::RemoteImages;

constexpr qreal kPlaceholderHeight = 56;
constexpr qint64 kMaxRemoteBytes = 256 * 1024 * 1024; ///< decoded pixels held at once; a flush, not an LRU, an image asks again

QUrl parsed(const QString &url)
{
    return QUrl(url, QUrl::TolerantMode);
}

struct Buttons {
    QRectF load;
    QRectF always; ///< null when there is no room
    QString alwaysText;
};

Buttons buttonsFor(const QRectF &rect, const QFont &font, const QString &host)
{
    const QFontMetricsF metrics(font);
    const qreal height = metrics.height() + 8;
    const qreal top = rect.bottom() - 7 - height;
    Buttons out;
    out.load = QRectF(rect.left() + 10, top, metrics.horizontalAdvance(MarkdownEditor::tr("Load image")) + 20, height);
    const qreal left = out.load.right() + 8;
    const qreal room = rect.right() - 10 - left - 20;
    const QString text = MarkdownEditor::tr("Always load from %1").arg(host);
    if (room >= metrics.horizontalAdvance(MarkdownEditor::tr("Always load from %1").arg(QStringLiteral("...")))) {
        out.alwaysText = metrics.elidedText(text, Qt::ElideMiddle, int(room));
        out.always = QRectF(left, top, metrics.horizontalAdvance(out.alwaysText) + 20, height);
    }
    return out;
}

} // namespace

QString MarkdownEditor::remoteImageURL(const EmbedSpan &embed)
{
    const QString &written = embed.raw.isEmpty() ? embed.target : embed.raw;
    if (!remoteimage::isRemoteImageTarget(written))
        return {};
    return parsed(written).adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
}

MarkdownEditor::RemoteShow MarkdownEditor::remoteImageShow(const QString &url)
{
    RemoteShow show;
    const QUrl address = parsed(url);
    if (const QString why = remoteimage::refusal(address); !why.isEmpty()) {
        show.notice = why; // no button: http, a password, a local address
        return show;
    }
    RemoteImages &images = RemoteImages::instance();
    const bool clicked = d->m_remoteSession.contains(url);
    if (!images.isAllowed(address) && !clicked) {
        show.canLoad = true; // nothing has been asked of the network
        return show;
    }
    if (const auto it = d->m_remoteImages.constFind(url); it != d->m_remoteImages.constEnd()) {
        show.pixmap = *it;
        return show;
    }
    if (const QString error = d->m_remoteErrors.value(url); !error.isEmpty()) {
        show.notice = error;
        show.canLoad = true; // a click tries again
        return show;
    }
    if (!d->m_remoteConnected) {
        d->m_remoteConnected = true;
        connect(&images, &remoteimage::RemoteImages::loaded, this,
                [this](const QUrl &loaded, const QImage &image) { deliverRemoteImage(loaded, image, {}); });
        connect(&images, &remoteimage::RemoteImages::failed, this,
                [this](const QUrl &failed, const QString &why) { deliverRemoteImage(failed, {}, why); });
    }
    // An allowed origin was agreed to, not wherever it sends the request: only a click follows a redirect elsewhere.
    images.load(address, !clicked); // a no-op for an image already on its way
    show.notice = tr("Loading...");
    return show;
}

void MarkdownEditor::loadRemoteImage(const QString &url, bool always)
{
    const QString key = parsed(url).adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
    if (always)
        RemoteImages::instance().allowOrigin(parsed(key));
    d->m_remoteSession.insert(key);
    d->m_remoteErrors.remove(key);
    relayRemoteImageBlocks(key); // the layout starts the load
}

bool MarkdownEditor::layoutRemoteImage(BlockView &view, const EmbedSpan &embed, const QString &url)
{
    const RemoteShow show = remoteImageShow(url);
    if (!show.pixmap.isNull()) {
        // Sized as a local picture is; the pixels stay in m_remoteImages only.
        return layoutPixmap(view, embed, show.pixmap, QStringLiteral("remote:") + url);
    }
    BlockExtras &extras = view.extrasMut();
    extras.isFileCard = true;
    extras.remoteURL = url;
    extras.remoteNotice = show.notice;
    extras.remoteCanLoad = show.canLoad;
    extras.fileCardPath.clear();
    extras.fileCardLabel = remoteimage::asciiHost(parsed(url));
    extras.fileCardSize.clear();
    extras.fileCardCover = QPixmap();
    extras.image = QPixmap();
    extras.embedPanel = false;
    extras.cells.clear();
    extras.fileCardRect = QRectF(0, 0, contentWidth(), kPlaceholderHeight);
    view.layout.reset();
    view.height = kPlaceholderHeight;
    hideWholeBlockText(view);
    view.laidOut = true;
    return true;
}

MarkdownEditor::EmbedCell MarkdownEditor::remoteImageCell(const EmbedSpan &embed, const QString &url, qreal maxWidth)
{
    EmbedCell cell;
    cell.source = embed.range;
    const RemoteShow show = remoteImageShow(url);
    if (!show.pixmap.isNull()) {
        const QSizeF size = embedImageSize(embed, show.pixmap.size(), maxWidth);
        cell.image = show.pixmap;
        cell.rect = QRectF(0, 0, size.width(), size.height());
        return cell;
    }
    cell.isFileCard = true;
    cell.label = remoteimage::asciiHost(parsed(url));
    cell.remoteURL = url;
    cell.remoteNotice = show.notice;
    cell.remoteCanLoad = show.canLoad;
    cell.rect = QRectF(0, 0, qMin<qreal>(maxWidth, embed.html ? 280 : maxWidth), kPlaceholderHeight);
    return cell;
}

void MarkdownEditor::paintRemotePlaceholder(QPainter &painter, const QRectF &rect, const QString &host,
                                            const QString &notice, bool canLoad) const
{
    painter.save();
    QPainterPath rounded;
    rounded.addRoundedRect(rect, 6, 6);
    painter.fillPath(rounded, d->m_theme.color(QStringLiteral("--background-primary-alt")));
    const QColor text = d->m_theme.color(QStringLiteral("--text-normal"));
    QFont font = d->m_theme.uiFont();
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    // The host is where a click goes; the notice, when there is one, follows it.
    const QString title = notice.isEmpty() ? host : host + QStringLiteral(" - ") + notice;
    painter.setPen(text);
    painter.drawText(QRectF(rect.left() + 10, rect.top() + 5, rect.width() - 20, metrics.height() + 2),
                     Qt::AlignVCenter | Qt::AlignLeft, metrics.elidedText(title, Qt::ElideRight, int(rect.width() - 20)));
    if (canLoad) {
        const Buttons buttons = buttonsFor(rect, font, host);
        const auto draw = [&](const QRectF &box, const QString &label) {
            QPainterPath path;
            path.addRoundedRect(box, 4, 4);
            painter.fillPath(path, d->m_theme.color(QStringLiteral("--background-secondary")));
            painter.setPen(d->m_theme.color(QStringLiteral("--background-modifier-border")));
            painter.drawPath(path);
            painter.setPen(d->m_theme.color(QStringLiteral("--text-accent")));
            painter.drawText(box, Qt::AlignCenter, label);
        };
        draw(buttons.load, tr("Load image"));
        if (!buttons.always.isNull())
            draw(buttons.always, buttons.alwaysText);
    }
    painter.restore();
}

bool MarkdownEditor::handleRemoteImageClick(const QString &url, bool canLoad, const QRectF &rect, const QPoint &point)
{
    if (!rect.contains(point))
        return false;
    if (canLoad) {
        const Buttons buttons = buttonsFor(rect, d->m_theme.uiFont(), remoteimage::asciiHost(parsed(url)));
        if (buttons.load.contains(point))
            loadRemoteImage(url, false);
        else if (!buttons.always.isNull() && buttons.always.contains(point))
            loadRemoteImage(url, true);
    }
    return true; // the placeholder takes its own clicks
}

void MarkdownEditor::deliverRemoteImage(const QUrl &url, const QImage &image, const QString &error)
{
    const QString key = url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
    if (!d->m_remoteSession.contains(key) && !RemoteImages::instance().isAllowed(url))
        return; // another editor's image
    if (image.isNull()) {
        d->m_remoteErrors.insert(key, error);
    } else {
        qint64 held = qint64(image.width()) * image.height() * 4;
        for (const QPixmap &pixmap : std::as_const(d->m_remoteImages))
            held += qint64(pixmap.width()) * pixmap.height() * 4;
        if (held > kMaxRemoteBytes)
            d->m_remoteImages.clear();
        d->m_remoteImages.insert(key, QPixmap::fromImage(image));
        d->m_remoteErrors.remove(key);
    }
    relayRemoteImageBlocks(key);
}

void MarkdownEditor::relayRemoteImageBlocks(const QString &url)
{
    bool touched = false;
    for (const EmbedSpan &embed : std::as_const(d->m_decorations.embeds)) {
        if (remoteImageURL(embed) != url)
            continue;
        const qsizetype index = blockAt(embed.range.start);
        if (index < 0)
            continue;
        BlockView &block = d->m_blocks[size_t(index)];
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

} // namespace noteAppC

#endif // NOTEAPPC_HAVE_NETWORK
