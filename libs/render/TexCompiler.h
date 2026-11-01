// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QString>

namespace noteAppC {

/// Compiles a `.tex` embed that is a full document (one with
/// `\documentclass`) to a PDF, with whatever TeX engine `PATH` turns up -
/// pdflatex, from TeX Live, is the one this project measures against. A
/// `.tex` file of maths alone never reaches this: MicroTeX renders that
/// directly, the same as any other formula, and isFullDocument is how the
/// caller tells the two apart before deciding which path to take.
///
/// Meant to be called from a worker thread: `compile` runs the process
/// synchronously and can take the full 30 s timeout, so it must never be
/// called from the UI thread - the same convention PdfPage::render keeps for
/// its own, cheaper, blocking call.
class TexCompiler {
public:
    struct Result {
        bool ok = false;
        QString pdfPath; ///< set when ok - the compiled PDF, under the cache root
        QString log;     ///< pdflatex's own output, tail end - set on a failed compile
        QString error;   ///< set when there was no engine, or the file could not be read
    };

    /// Whether `texSource` looks like a full document rather than maths
    /// alone - a plain search for `\documentclass`, not a real parse: a
    /// document that only mentions the command in a comment is the one
    /// corner this does not get right, and is not worth a real TeX tokenizer
    /// to fix.
    static bool isFullDocument(const QString &texSource);

    /// True once an engine was found on `PATH`.
    static bool isAvailable();
    /// The engine's absolute path, or empty - for a one-line "no TeX found"
    /// note in the embed instead of a silent failure.
    static QString enginePath();

    /// Compiles `texPath`'s current bytes - read here, not by the caller, so
    /// the cache key is always the file exactly as it now stands - in a
    /// temporary directory outside the vault, never next to the note, with
    /// no shell escape and a time and memory limit. Cached under
    /// `$XDG_CACHE_HOME/notolith/tex/<hash of the bytes>`; a second call with
    /// unchanged bytes returns the cached result (success or the same
    /// failure) without running the engine again.
    static Result compile(const QString &texPath);
};

} // namespace noteAppC
