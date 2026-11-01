// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>

namespace Poppler {
class Document;
}

namespace noteAppC {

/// The poppler half of PdfPage (libs/render/PdfPage.h, whose class comment is the contract
/// for these four): one Poppler::Document per thread, reopened when the file's mtime moves.
/// Lives in libnotolith-pdfpage.so, which the host's PdfPage resolves through PdfPageFace.h
/// and the pdfview module links; every caller inside a process reaches this one copy, so
/// the first-render guard in `warmUp` is process-wide.
class PdfPageImpl {
public:
    static int pageCount(const QString &path);
    static QSizeF pageSize(const QString &path, int page);
    static QImage render(const QString &path, int page, int widthPixels, bool hideAnnotations = false);
    /// For code that renders or extracts text through its own Poppler::Document: run once
    /// before it, on any thread. Cheap after the first call in the process.
    static void warmUp(Poppler::Document *document);
};

} // namespace noteAppC
