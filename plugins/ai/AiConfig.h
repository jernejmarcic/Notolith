// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QString>

namespace noteAppC::ai {

/// Which HTTP shape to speak - Ollama's own `/api/chat`, or one of the two
/// widely-copied chat-completion shapes.
enum class Provider { Ollama, OpenAICompatible, Anthropic };

/// This plugin's own runtime configuration - not `plugin.ini` (the
/// manifest, which ships with the plugin and is not what a person edits day
/// to day). Read from `path()`, outside the vault (docs/PLUGINS.md's AI
/// plugin). Nothing in here is a secret: `apiKeyEnv` is the NAME of an
/// environment variable, never a key's value - the value is read straight
/// from the process environment at call time (AiProvider.cpp) and is never
/// written to this or any other file.
struct AiConfig {
    Provider provider = Provider::Ollama;
    QString baseURL = QStringLiteral("http://localhost:11434");
    QString model = QStringLiteral("llama3.2");
    QString apiKeyEnv;           ///< empty for Ollama, required for the other two
    int httpTimeoutMs = 120000;  ///< curl's own timeout; keep it under plugin.ini's timeout-ms
    /// `translate`'s languages, each "Name (code)" or just a name. An empty
    /// `translateFrom` lets a general model detect the source; TranslateGemma
    /// has no such slot, so it falls back to Slovenian there (translationPrompt).
    QString translateTo = QStringLiteral("English (en)");
    QString translateFrom;
    QString translateModel;      ///< empty: `model`
    int askTimeoutMs = 120000;   ///< how long "Ask a question" waits for the person to type

    /// `$XDG_CONFIG_HOME/notolith/plugins/ai/ai.ini` - the same helper
    /// PluginRegistry::configPath() uses, so this resolves to the directory
    /// PluginHost::sandboxArguments() bound read-only for every plugin.
    static QString path();

    /// Missing file or missing keys fall back to Ollama on localhost -
    /// exactly the same shape a config file with an empty [ai] section
    /// would produce, so "no ai.ini yet" and "an ai.ini that only sets one
    /// key" behave the same way.
    static AiConfig load();
};

} // namespace noteAppC::ai
