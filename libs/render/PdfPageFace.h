// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>

/// What libnotolith-pdfpage.so exports and PdfPage.cpp resolves: the C face of
/// the pdfview package's poppler half. C linkage, so a lookup by name needs no
/// mangling; the Qt types in the signatures are the same on both sides because
/// the first function names the build (notolith_pdf_abi), and PdfPage.cpp refuses
/// a library built for another Notolith. The signatures return C++ types (QImage, QSizeF) under C linkage: fine on
/// GCC and Clang within one ABI tag, and Clang's -Wreturn-type-c-linkage says so. Do not "fix" the warning on one
/// side only: both sides are rebuilt together or refused, and a pointer-based face would need both changed at once.
extern "C" {
const char *notolith_pdf_abi();
int notolith_pdf_page_count(const QString &path);
QSizeF notolith_pdf_page_size(const QString &path, int page);
QImage notolith_pdf_render(const QString &path, int page, int widthPixels, bool hideAnnotations);
}
