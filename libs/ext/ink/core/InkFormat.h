#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include "InkDocument.h"

/// The .ink format, version 1 (spec section 5): UTF-8 JSON written compact by
/// QJsonDocument (no custom emitter), numbers with three decimals, unknown keys
/// and item types kept and written back.
/// A reader seeing the gzip magic (1f 8b) inflates first; a version above
/// 1 opens read-only. Documented for users in docs/INK.md.
namespace noteAppC::ink {

struct InkReadResult {
    bool ok = false;
    bool readOnly = false;
    int version = 0;
    QString error;
};

/// Files above this parse off the UI thread (InkView, Track S1 Task 11).
constexpr qint64 kOffThreadParseBytes = 256 * 1024;

InkReadResult readInk(const QByteArray &bytes, InkDocument &out, const QString &nameForErrors = QString());
QByteArray writeInk(const InkDocument &document);
bool loadInkFile(const QString &path, InkDocument &out, InkReadResult *result = nullptr);
/// Temporary file and rename: for tests and tools without a Vault. The
/// modules save through writeInk() + Vault::writeBytes (spec section 5).
bool saveInkFile(const QString &path, const InkDocument &document, QString *error = nullptr);

double roundThree(double value);
QString colorName(const QColor &color);
QColor colorFromName(const QString &name, const QColor &fallback);
QJsonObject settingsToJSON(const DocumentSettings &settings);
DocumentSettings settingsFromJSON(const QJsonObject &object);

} // namespace noteAppC::ink
