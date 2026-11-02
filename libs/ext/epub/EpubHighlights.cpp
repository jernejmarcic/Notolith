// SPDX-License-Identifier: GPL-3.0-or-later
#include "EpubHighlights.h"

#include "Vault.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace noteAppC {

namespace {

const QList<QString> kColorNames = {QStringLiteral("yellow"), QStringLiteral("green"),
                                    QStringLiteral("blue"), QStringLiteral("pink"),
                                    QStringLiteral("orange")};

// The quote travels inside the hidden %% %% comment percent-encoded (a
// single %XX-escaped token, so the line-oriented marker regex below never
// has to worry about a quote containing '%', a literal newline, or the
// closing "%%" itself); the *readable* copy a person sees is the blockquote
// regenerated next to it, which needs none of that.
QString encodeQuote(const QString &quote)
{
    QString oneLine = quote;
    oneLine.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return QString::fromUtf8(QUrl::toPercentEncoding(oneLine));
}

QString decodeQuote(const QString &encoded)
{
    return QUrl::fromPercentEncoding(encoded.toUtf8());
}

const QRegularExpression &markerRe()
{
    static const QRegularExpression re(
        QStringLiteral("^%%\\s*epub-highlight\\s+locator=(\\S+)\\s+color=(\\S+)\\s+quote=(\\S*)\\s*%%\\s*$"));
    return re;
}

const QString kStartMarker = QStringLiteral("<!-- epub:generated:start -->");
const QString kEndMarker = QStringLiteral("<!-- epub:generated:end -->");

} // namespace

QString EpubLocator::toString() const
{
    return QStringLiteral("epub:%1:%2-%3").arg(spineIndex).arg(start).arg(end);
}

std::optional<EpubLocator> EpubLocator::parse(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^epub:(\\d+):(\\d+)-(\\d+)$"));
    const QRegularExpressionMatch match = re.match(text.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    EpubLocator locator;
    locator.spineIndex = match.captured(1).toInt();
    locator.start = match.captured(2).toLongLong();
    locator.end = match.captured(3).toLongLong();
    if (!locator.isValid())
        return std::nullopt;
    return locator;
}

const QList<QString> &EpubHighlights::colorNames()
{
    return kColorNames;
}

QString EpubHighlights::sidecarPath(const QString &bookPath)
{
    const QFileInfo info(bookPath);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
        + QStringLiteral(" highlights.md");
}

QList<EpubHighlight> EpubHighlights::parse(const QString &markdown)
{
    QList<EpubHighlight> out;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch match = markerRe().match(lines[i]);
        if (!match.hasMatch())
            continue;
        const auto locator = EpubLocator::parse(match.captured(1));
        if (!locator)
            continue;

        EpubHighlight highlight;
        highlight.locator = *locator;
        highlight.color = match.captured(2);
        highlight.quote = decodeQuote(match.captured(3));

        // The note is everything after the marker up to the next entry
        // boundary - another marker, the generated section's own end, or the
        // "---" this class writes between entries - trimmed of blank edges
        // but otherwise left exactly as the reader typed it.
        QStringList noteLines;
        for (qsizetype j = i + 1; j < lines.size(); ++j) {
            const QString trimmed = lines[j].trimmed();
            if (markerRe().match(lines[j]).hasMatch() || trimmed == kEndMarker || trimmed == QLatin1String("---"))
                break;
            noteLines << lines[j];
        }
        while (!noteLines.isEmpty() && noteLines.first().trimmed().isEmpty())
            noteLines.removeFirst();
        while (!noteLines.isEmpty() && noteLines.last().trimmed().isEmpty())
            noteLines.removeLast();
        highlight.note = noteLines.join(QLatin1Char('\n'));

        out.append(highlight);
    }
    return out;
}

bool EpubHighlights::writeSidecar(const QString &sidecarPath, const QString &bookFileName,
                                  const QString &bookTitle, const QList<EpubHighlight> &live,
                                  const std::function<QString(int)> &chapterTitle, QString *error)
{
    Note existingNote;
    const bool hadFile = Vault::read(sidecarPath, existingNote, nullptr);
    const QString existing = hadFile ? existingNote.text : QString();

    // Keep whatever free-text note already exists for a locator still in
    // `live`; a highlight with no prior entry starts with an empty one.
    QHash<QString, QString> priorNoteByLocator;
    for (const EpubHighlight &old : parse(existing))
        priorNoteByLocator.insert(old.locator.toString(), old.note);

    QString before;
    QString after;
    const qsizetype startAt = existing.indexOf(kStartMarker);
    const qsizetype endAt = existing.indexOf(kEndMarker);
    if (startAt >= 0 && endAt > startAt) {
        before = existing.left(startAt);
        after = existing.mid(endAt + kEndMarker.size());
    } else {
        // No sidecar yet, or a hand-edited one with no markers: write a
        // fresh header. Nothing of the user's is lost either way - a file
        // with no markers has never had a generated section to begin with.
        before = QStringLiteral("# %1 — highlights\n\nLinks back into [[%2]]. Everything between the "
                                "markers below is rebuilt by \"Extract highlights\"; write anywhere else in "
                                "this file and it stays exactly as written.\n\n")
                    .arg(bookTitle.isEmpty() ? bookFileName : bookTitle, bookFileName);
        after = QStringLiteral("\n");
    }

    QList<EpubHighlight> sorted = live;
    std::sort(sorted.begin(), sorted.end(), [](const EpubHighlight &a, const EpubHighlight &b) {
        if (a.locator.spineIndex != b.locator.spineIndex)
            return a.locator.spineIndex < b.locator.spineIndex;
        return a.locator.start < b.locator.start;
    });

    QString generated = kStartMarker + QLatin1Char('\n');
    int lastSpine = -2;
    for (const EpubHighlight &highlight : std::as_const(sorted)) {
        if (highlight.locator.spineIndex != lastSpine) {
            lastSpine = highlight.locator.spineIndex;
            QString title = chapterTitle ? chapterTitle(lastSpine) : QString();
            if (title.isEmpty())
                title = QStringLiteral("Chapter %1").arg(lastSpine + 1);
            generated += QStringLiteral("\n### %1\n\n").arg(title);
        }
        const QString note = priorNoteByLocator.value(highlight.locator.toString(), highlight.note);
        generated += QStringLiteral("> %1\n\n").arg(highlight.quote);
        generated += QStringLiteral("%% epub-highlight locator=%1 color=%2 quote=%3 %%\n")
                        .arg(highlight.locator.toString(), highlight.color, encodeQuote(highlight.quote));
        if (!note.isEmpty())
            generated += note + QLatin1Char('\n');
        generated += QStringLiteral("\n---\n");
    }
    generated += QLatin1Char('\n') + kEndMarker;

    Note newNote = existingNote; // keeps the existing file's CRLF/BOM, if any
    newNote.text = before + generated + after;
    return Vault::write(sidecarPath, newNote, error);
}

} // namespace noteAppC
