// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include "AiConfig.h"

#include <QString>
#include <QStringList>

namespace noteAppC::ai {

/// One provider call's outcome. `ok == false` covers every way a call can
/// fail to produce an answer - a network error, a non-2xx status, a
/// malformed or unexpected response body, or a missing/unset API key - so a
/// caller only ever needs to check `ok` and show `error`, never guess which
/// failure mode it hit.
struct AiResult {
    bool ok = false;
    QString text;   ///< the answer, when ok
    QString error;  ///< human-readable, when !ok
};

/// One blocking chat-style call against `config.provider`'s endpoint.
/// `systemPrompt` may be empty. Blocking is correct here: the plugin host
/// process (tools/plugin_host/HostIO.cpp) already services exactly one
/// request at a time on its own thread, the same as any command - see
/// docs/PLUGINS.md's "Timeouts". Never throws and never crashes on a
/// malformed response: every JSON access is a safe, defaulting Qt lookup.
AiResult runChat(const AiConfig &config, const QString &systemPrompt, const QString &userPrompt);

/// A system prompt (may be empty) and the user message for one call.
struct ChatPrompt {
    QString system;
    QString user;
};

/// The prompt for translating `text` from `from` to `to` (each "Name
/// (code)" or a bare name; `from` empty means detect it). A TranslateGemma
/// model gets the one user message it was trained on and no system prompt;
/// that template names the source, so an empty `from` is Slovenian there.
ChatPrompt translationPrompt(const QString &model, const QString &from, const QString &to, const QString &text);

/// GET `/api/tags` (Ollama only - the only one of the three whose API
/// exposes what is locally installed). `ok == false` with `error` set to
/// say so when `config.provider` is not Ollama, before any request is made.
struct AiModelsResult {
    bool ok = false;
    QStringList models;
    QString error;
};
AiModelsResult listOllamaModels(const AiConfig &config);

} // namespace noteAppC::ai
