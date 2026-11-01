// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
/* plugins/ai/plugin.cpp - the noteAppC AI plugin (docs/PLUGINS.md, Tier 2).
 * Talks to a local Ollama server, or an OpenAI-compatible/Anthropic HTTP
 * API - AiProvider.cpp does the actual HTTP call and response parsing, kept
 * separate so it can be unit-tested directly against a fake local server
 * with no plugin host or sandbox in the way (tests/test_ai_plugin.cpp).
 * Uses plugin.hpp's C++ wrapper rather than writing the noteappc_plugin
 * table by hand - unlike word-stats, this plugin already needs Qt (QSettings,
 * QJson) for AiConfig/AiProvider, so std::function costs nothing extra.
 */
#include "AiConfig.h"
#include "AiProvider.h"

#include <noteappc/plugin.hpp>

using noteAppC::ai::AiConfig;
using noteAppC::ai::AiResult;

namespace {

noteAppC::Plugin gPlugin;

/* Every command below shows the actual failure as a notice (and, for the
 * two notice-only commands, also logs the full text - a status-bar notice
 * is one line and a real answer can run longer) and returns 0 either way:
 * run_command's non-zero path means "the plugin itself is broken", which is
 * a different, rarer thing than "the language model call failed" - the
 * latter is an ordinary, expected outcome (no network, no server running,
 * a bad key) that the user is already told about through the notice, so a
 * second, more generic "plugin command failed" message on top of it would
 * only be noise. */

int summarize()
{
    const noteappc_host *host = gPlugin.host();
    const QString text = QString::fromUtf8(host->active_note_text());
    if (text.trimmed().isEmpty()) {
        host->show_notice("Nothing to summarize - the note is empty.");
        return 0;
    }
    const AiResult result = noteAppC::ai::runChat(
        AiConfig::load(), QStringLiteral("Summarize notes concisely, a few sentences at most."), text);
    if (!result.ok) {
        host->show_notice((QStringLiteral("Summarize failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    host->log(result.text.toUtf8().constData());
    host->show_notice(result.text.toUtf8().constData());
    return 0;
}

int askAboutSelection()
{
    const noteappc_host *host = gPlugin.host();
    const QString selection = QString::fromUtf8(host->active_selection_text());
    if (selection.trimmed().isEmpty()) {
        host->show_notice("Select some text first.");
        return 0;
    }
    const AiResult result = noteAppC::ai::runChat(
        AiConfig::load(),
        QStringLiteral("The user selected the following text in their note. If it poses a question, "
                       "answer it; otherwise explain or comment on it briefly."),
        selection);
    if (!result.ok) {
        host->show_notice((QStringLiteral("Ask failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    host->log(result.text.toUtf8().constData());
    host->show_notice(result.text.toUtf8().constData());
    return 0;
}

/// "AI: Ask a question": the question is typed into a small dialog the app
/// shows (host->prompt), the answer lands at the caret - or, with a
/// selection, which an insert would overwrite, in the status line and the
/// log instead. Only the question is sent: unlike summarize, the note is not
/// context (neither is it for an ```ai block).
int askQuestion()
{
    const noteappc_host *host = gPlugin.host();
    const AiConfig config = AiConfig::load();
    const char *typed = host->prompt("Ask the model a question:", 0, config.askTimeoutMs);
    const QString question = typed ? QString::fromUtf8(typed).trimmed() : QString();
    if (question.isEmpty()) {
        host->show_notice(typed ? "Ask a question: nothing typed." : "Ask a question: cancelled.");
        return 0;
    }
    const AiResult result = noteAppC::ai::runChat(config, QString(), question);
    if (!result.ok) {
        host->show_notice((QStringLiteral("Ask a question failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    const QByteArray answer = result.text.toUtf8();
    const bool selected = !QString::fromUtf8(host->active_selection_text()).isEmpty();
    if (selected || host->replace_selection(answer.constData()) != 0) {
        host->log(answer.constData());
        host->show_notice(answer.constData());
    }
    return 0;
}

int rewriteSelection()
{
    const noteappc_host *host = gPlugin.host();
    const QString selection = QString::fromUtf8(host->active_selection_text());
    if (selection.trimmed().isEmpty()) {
        host->show_notice("Select some text first.");
        return 0;
    }
    const AiResult result = noteAppC::ai::runChat(
        AiConfig::load(),
        QStringLiteral("Rewrite the given text to improve clarity and flow. Keep the same meaning "
                       "and language. Reply with only the rewritten text, no commentary."),
        selection);
    if (!result.ok) {
        host->show_notice((QStringLiteral("Rewrite failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    host->replace_selection(result.text.toUtf8().constData());
    return 0;
}

int continueWriting()
{
    const noteappc_host *host = gPlugin.host();
    const QString text = QString::fromUtf8(host->active_note_text());
    if (text.trimmed().isEmpty()) {
        host->show_notice("Nothing to continue - the note is empty.");
        return 0;
    }
    const AiResult result = noteAppC::ai::runChat(
        AiConfig::load(),
        QStringLiteral("Continue writing the given note naturally, matching its tone and style. "
                       "Reply with only the continuation text, no commentary."),
        text);
    if (!result.ok) {
        host->show_notice((QStringLiteral("Continue failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    // replace_selection is really "insert at the caret, replacing any
    // active selection" (MarkdownEditor::insertText) - exactly "continue
    // writing" wants when there is no selection.
    host->replace_selection(result.text.toUtf8().constData());
    return 0;
}

/// The selection, translated in place; with nothing selected, the whole
/// note into a new note beside it ("Note (en).md") - never over the original,
/// and never over a note that already exists.
int translate()
{
    const noteappc_host *host = gPlugin.host();
    AiConfig config = AiConfig::load();
    if (!config.translateModel.isEmpty())
        config.model = config.translateModel;
    const QString selection = QString::fromUtf8(host->active_selection_text());
    const bool wholeNote = selection.trimmed().isEmpty();
    const QString text = wholeNote ? QString::fromUtf8(host->active_note_text()) : selection;
    if (text.trimmed().isEmpty()) {
        host->show_notice("Nothing to translate - the note is empty.");
        return 0;
    }

    QString target;
    if (wholeNote) {
        const QString path = QString::fromUtf8(host->active_note_path());
        if (path.isEmpty()) { // outside the vault: nowhere beside it to write
            host->show_notice("Translate: select the text - this note is outside the vault.");
            return 0;
        }
        const QString to = config.translateTo.trimmed();
        const qsizetype open = to.lastIndexOf(QLatin1Char('('));
        const QString code = open > 0 && to.endsWith(QLatin1Char(')')) ? to.mid(open + 1, to.size() - open - 2) : to;
        target = (path.endsWith(QLatin1String(".md")) ? path.chopped(3) : path) + QStringLiteral(" (%1).md").arg(code);
        if (!QString::fromUtf8(host->read_note(target.toUtf8().constData())).isEmpty()) {
            host->show_notice((QStringLiteral("Translate: ") + target + QStringLiteral(" already exists.")).toUtf8().constData());
            return 0;
        }
    }

    const auto prompt = noteAppC::ai::translationPrompt(config.model, config.translateFrom, config.translateTo, text);
    const AiResult result = noteAppC::ai::runChat(config, prompt.system, prompt.user);
    if (!result.ok) {
        host->show_notice((QStringLiteral("Translate failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    if (!wholeNote) {
        host->replace_selection(result.text.toUtf8().constData());
    } else if (host->write_note(target.toUtf8().constData(), result.text.toUtf8().constData()) == 0) {
        host->show_notice((QStringLiteral("Translated into ") + target).toUtf8().constData());
    } else {
        host->show_notice((QStringLiteral("Translate: could not write ") + target).toUtf8().constData());
    }
    return 0;
}

int proofreadSelection()
{
    const noteappc_host *host = gPlugin.host();
    const QString selection = QString::fromUtf8(host->active_selection_text());
    if (selection.trimmed().isEmpty()) {
        host->show_notice("Select some text first.");
        return 0;
    }
    const AiResult result = noteAppC::ai::runChat(
        AiConfig::load(),
        QStringLiteral("Fix spelling, grammar and punctuation in the given text. Change nothing else: keep its "
                       "language, wording, Markdown and line breaks. Reply with only the corrected text."),
        selection);
    if (!result.ok) {
        host->show_notice((QStringLiteral("Proofread failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    host->replace_selection(result.text.toUtf8().constData());
    return 0;
}

int listModels()
{
    const noteappc_host *host = gPlugin.host();
    const auto result = noteAppC::ai::listOllamaModels(AiConfig::load());
    if (!result.ok) {
        host->show_notice((QStringLiteral("List models failed: ") + result.error).toUtf8().constData());
        return 0;
    }
    const QString joined = result.models.join(QStringLiteral(", "));
    host->log(joined.toUtf8().constData());
    host->show_notice(joined.isEmpty() ? "No models installed." : joined.toUtf8().constData());
    return 0;
}

noteAppC::BlockResult renderAiBlock(const std::string &source, int /*widthPx*/, float /*devicePixelRatio*/)
{
    const QString prompt = QString::fromStdString(source);
    if (prompt.trimmed().isEmpty())
        return noteAppC::BlockResult::ofText("*(empty ai block)*");
    const AiResult result = noteAppC::ai::runChat(AiConfig::load(), QString(), prompt);
    if (!result.ok)
        return noteAppC::BlockResult::ofText((QStringLiteral("> [!failure] ") + result.error).toStdString());
    return noteAppC::BlockResult::ofText(result.text.toStdString());
}

int aiPluginInit()
{
    gPlugin.onCommand("summarize", summarize);
    gPlugin.onCommand("ask", askAboutSelection);
    gPlugin.onCommand("ask-question", askQuestion);
    gPlugin.onCommand("rewrite", rewriteSelection);
    gPlugin.onCommand("continue", continueWriting);
    gPlugin.onCommand("translate", translate);
    gPlugin.onCommand("proofread", proofreadSelection);
    gPlugin.onCommand("list-models", listModels);
    gPlugin.onCodeBlock("ai", renderAiBlock);
    return 0;
}

} // namespace

NOTEAPPC_PLUGIN_ENTRY(gPlugin, aiPluginInit)
