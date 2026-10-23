#pragma once

#include <link.h>

#include <QFileInfo>
#include <QString>
#include <QStringList>

/// Every shared object mapped into this process, by canonical path: what a test asks to learn whether a library
/// was loaded (a module, poppler, protobuf) without caring how.
inline QStringList loadedObjects()
{
    QStringList out;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *data) {
            if (info->dlpi_name && *info->dlpi_name)
                static_cast<QStringList *>(data)->append(QFileInfo(QString::fromLocal8Bit(info->dlpi_name)).canonicalFilePath());
            return 0;
        },
        &out);
    return out;
}

/// The mapped objects whose file name contains `needle` (e.g. "libpoppler-qt6").
inline QStringList loadedObjectsMatching(const QString &needle)
{
    return loadedObjects().filter(needle);
}
