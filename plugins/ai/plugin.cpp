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
    gPlugin.onCommand("rewrite", rewriteSelection);
    gPlugin.onCommand("continue", continueWriting);
    gPlugin.onCommand("list-models", listModels);
    gPlugin.onCodeBlock("ai", renderAiBlock);
    return 0;
}

} // namespace

NOTEAPPC_PLUGIN_ENTRY(gPlugin, aiPluginInit)
