// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "NotebookOutputs.h"

#include <QRegularExpression>

namespace noteAppC::notebook {

/// A traceback line arrives with ANSI colour escapes (real Python
/// tracebacks are colourised by IPython); stripped rather than mapped to
/// Qt colours - see NotebookOutput's own comment for the upgrade path if
/// coloured tracebacks turn out to matter.
QString stripAnsi(const QString &text)
{
    static const QRegularExpression escape(QStringLiteral("\x1b\\[[0-9;]*m"));
    QString out = text;
    out.remove(escape);
    return out;
}

/// A carriage return rewrites the line it is on (tqdm, a progress counter):
/// only what follows the last one on a line stays. Limit: whole-line
/// rewrites, not a terminal's overwrite-in-place, which is all those print.
QString resolveCarriageReturns(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (!text.contains(QLatin1Char('\r')))
        return text;
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        const qsizetype cr = line.lastIndexOf(QLatin1Char('\r'));
        if (cr >= 0)
            line = line.mid(cr + 1);
    }
    return lines.join(QLatin1Char('\n'));
}

/// What a notebook shows of one display bundle, richest first, as Jupyter
/// frontends choose: a picture, then HTML, then LaTeX and Markdown, then the
/// text every kernel sends. Nothing recognised: an empty Text output, which
/// the caller drops.
NotebookOutput outputFromMimeBundle(const QJsonObject &data)
{
    NotebookOutput output;
    const auto text = [&data](const char *mime) { return data.value(QLatin1String(mime)).toString(); };
    if (data.contains(QLatin1String("image/png"))) {
        output.kind = NotebookOutput::Kind::ImagePng;
        output.imageBytes = QByteArray::fromBase64(text("image/png").toLatin1());
        output.alt = stripAnsi(text("text/plain")); // the twin a notebook file keeps beside it
    } else if (data.contains(QLatin1String("image/jpeg"))) {
        output.kind = NotebookOutput::Kind::ImagePng; // the editor reads either
        output.imageBytes = QByteArray::fromBase64(text("image/jpeg").toLatin1());
        output.alt = stripAnsi(text("text/plain"));
    } else if (data.contains(QLatin1String("image/svg+xml"))) {
        output.kind = NotebookOutput::Kind::ImageSvg;
        output.text = text("image/svg+xml");
        output.alt = stripAnsi(text("text/plain"));
    } else if (data.contains(QLatin1String("text/html"))) {
        output.kind = NotebookOutput::Kind::Html;
        output.text = text("text/html");
        output.alt = stripAnsi(text("text/plain"));
    } else if (data.contains(QLatin1String("text/latex"))) {
        // Through the maths renderer the Markdown path already has.
        output.kind = NotebookOutput::Kind::Markdown;
        const QString tex = text("text/latex").trimmed();
        output.text = tex.startsWith(QLatin1Char('$')) ? tex : QStringLiteral("$$\n") + tex + QStringLiteral("\n$$");
    } else if (data.contains(QLatin1String("text/markdown"))) {
        output.kind = NotebookOutput::Kind::Markdown;
        output.text = text("text/markdown");
    } else if (data.contains(QLatin1String("text/plain"))) {
        output.kind = NotebookOutput::Kind::Text;
        output.text = stripAnsi(text("text/plain"));
    }
    return output;
}

} // namespace noteAppC::notebook
