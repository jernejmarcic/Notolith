// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include "InkDocument.h"

/// The .ink format, version 1 (spec section 5): UTF-8 JSON written compact by
/// QJsonDocument (no custom emitter), numbers with three decimals, unknown keys
/// and item types kept and written back.
/// A reader refuses input above kMaxInflatedBytes, then on the gzip magic (1f 8b) inflates first (one
/// member, capped at the same, nothing after it). Unknown keys are kept at every level.
/// A `version` that is a number other than exactly 1 (and at least 1) opens
/// read-only; any other version is refused, as is a wrong-typed `items`,
/// `document` or nested container, a shape with too few points, and a stroke or shape whose width, a coordinate or a
/// pressure is not a number (never read as 0). The document settings (format, background, extent) are the lenient part:
/// a wrong-typed scalar there takes its default, and a settings block that is unusable as a whole is refused.
/// Non-finite numbers are written as 0, never as null.
/// A text box item is `{"type":"textbox","t":<int>,"layer":"user","rect":[x,y,width,height],"markdown":"<text>"}`
/// (document px; `rect` is four numbers or absent). The host's own reader of .ink files for indexing
/// duplicates the keys "format", "version", "items", "type", "textbox" and "markdown" as string literals (it ignores "rect"): change them here and there together.
/// Documented for users in docs/INK.md.
namespace noteAppC::ink {

struct InkReadResult {
    bool ok = false;
    bool readOnly = false;
    int version = 0;
    QString error;
};

/// Files above this parse off the UI thread (InkView).
constexpr qint64 kOffThreadParseBytes = 256 * 1024;

/// The most a file may unpack to (and the most a file on disk or a plain JSON buffer may be): a heavy
/// note is about 2 MB, and the parse peaks near ten times the file, so 64 MiB bounds it near 640 MB.
constexpr qint64 kMaxInflatedBytes = qint64(64) << 20;

InkReadResult readInk(const QByteArray &bytes, InkDocument &out, const QString &nameForErrors = QString());
InkReadResult readInk(const QByteArray &bytes, InkDocument &out, const QString &nameForErrors, qint64 maxInflatedBytes);
QByteArray writeInk(const InkDocument &document);
bool loadInkFile(const QString &path, InkDocument &out, InkReadResult *result = nullptr);
/// Temporary file and rename: for tests and tools without a Vault. The
/// modules save through writeInk() + Vault::writeBytes (spec section 5).
bool saveInkFile(const QString &path, const InkDocument &document, QString *error = nullptr);

/// One item as a .ink file writes it (raw's unknown keys first, the model's keys over them) and back. The reader's
/// checks apply: false and `*error` for an item the reader would refuse. The clipboard (InkTools.h) uses both.
QJsonObject itemToJSON(const Item &item);
bool itemFromJSON(const QJsonObject &object, Item *item, QString *error);

double roundThree(double value);
QString colorName(const QColor &color);
QColor colorFromName(const QString &name, const QColor &fallback);
QJsonObject settingsToJSON(const DocumentSettings &settings);
DocumentSettings settingsFromJSON(const QJsonObject &object);

} // namespace noteAppC::ink
