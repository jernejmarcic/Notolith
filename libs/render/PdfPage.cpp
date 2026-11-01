// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "PdfPage.h"

#include "ModuleRegistry.h"
#include "PdfPageFace.h"

#include <QDir>
#include <QLibrary>

#include <dlfcn.h>

namespace noteAppC {

namespace {

/// The four functions of PdfPageFace.h, once found.
struct Face {
    const char *(*abi)() = nullptr;
    int (*count)(const QString &) = nullptr;
    QSizeF (*size)(const QString &, int) = nullptr;
    QImage (*render)(const QString &, int, int, bool) = nullptr;
    bool ok() const { return abi && count && size && render; }
};

/// From `library`, or - null - from what the process already holds: a binary that links the
/// pdfview code itself (the bench, test_pdfview) has the functions without any file to find.
Face resolveFrom(QLibrary *library)
{
    const auto find = [library](const char *name) {
        return library ? reinterpret_cast<void *>(library->resolve(name)) : dlsym(RTLD_DEFAULT, name);
    };
    Face face;
    face.abi = reinterpret_cast<const char *(*)()>(find("notolith_pdf_abi"));
    face.count = reinterpret_cast<int (*)(const QString &)>(find("notolith_pdf_page_count"));
    face.size = reinterpret_cast<QSizeF (*)(const QString &, int)>(find("notolith_pdf_page_size"));
    face.render = reinterpret_cast<QImage (*)(const QString &, int, int, bool)>(find("notolith_pdf_render"));
    // A library of another build keeps its names and changes what they mean: refused, never called.
    if (!face.ok() || ModuleRegistry::ABItag() != QLatin1String(face.abi()))
        return {};
    return face;
}

/// Found once, on the calling thread (a function-local static: concurrent first calls wait for it).
/// A library that opens but is refused stays mapped; nothing here ever unloads.
const Face &face()
{
    static const Face found = [] {
        if (const Face linked = resolveFrom(nullptr); linked.ok())
            return linked;
        for (const QString &directory : ModuleRegistry::defaultDirectories()) {
            auto *library = new QLibrary(QDir(directory).filePath(QStringLiteral("libnotolith-pdfpage.so")));
            library->setLoadHints(QLibrary::ResolveAllSymbolsHint); // a missing symbol fails here, by name
            if (library->load()) {
                if (const Face loaded = resolveFrom(library); loaded.ok())
                    return loaded;
            }
        }
        return Face{};
    }();
    return found;
}

} // namespace

bool PdfPage::isAvailable()
{
    return face().ok();
}

int PdfPage::pageCount(const QString &path)
{
    const Face &pdf = face();
    return pdf.ok() ? pdf.count(path) : 0;
}

QSizeF PdfPage::pageSize(const QString &path, int page)
{
    const Face &pdf = face();
    return pdf.ok() ? pdf.size(path, page) : QSizeF();
}

QImage PdfPage::render(const QString &path, int page, int widthPixels, bool hideAnnotations)
{
    const Face &pdf = face();
    return pdf.ok() ? pdf.render(path, page, widthPixels, hideAnnotations) : QImage();
}

} // namespace noteAppC
