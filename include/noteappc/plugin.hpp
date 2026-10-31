/* Header-only C++ convenience wrapper over plugin.h (docs/PLUGINS.md, Tier
 * 2). Optional: examples/plugins/word-stats is plain C and does not include
 * this. It exists for a future first-party plugin (an AI integration,
 * say) that will want RAII and std::function instead of
 * writing the noteappc_plugin table by hand.
 *
 * Usage - define once, at file scope, in the plugin's single translation
 * unit:
 *
 *     noteAppC::Plugin plugin;
 *     int main_init() {
 *         plugin.onCommand("count-words", [] { ... return 0; });
 *         plugin.onCodeBlock("calc", [](const std::string &source, int, float) {
 *             return noteAppC::BlockResult::ofText("42");
 *         });
 *         return 0;
 *     }
 *     NOTEAPPC_PLUGIN_ENTRY(plugin, main_init)
 *
 * Everything here just adapts the C table to std::function and back; it adds
 * no behaviour the C ABI does not already have; the host neither knows nor
 * cares whether a plugin used it.
 */
#ifndef NOTEAPPC_PLUGIN_HPP
#define NOTEAPPC_PLUGIN_HPP

#include "plugin.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace noteAppC {

/// A code block render result built with one of the two factories - never
/// construct one with both text and an image set, only one is used.
struct BlockResult {
    std::string text;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    static BlockResult ofText(std::string text)
    {
        BlockResult result;
        result.text = std::move(text);
        return result;
    }
    static BlockResult ofImage(int width, int height, std::vector<uint8_t> rgba)
    {
        BlockResult result;
        result.width = width;
        result.height = height;
        result.rgba = std::move(rgba);
        return result;
    }
};

/// Thin RAII/std::function front for noteappc_plugin. One instance per
/// plugin process (the host loads exactly one plugin per host, so exactly
/// one Plugin object is ever alive).
class Plugin {
public:
    using CommandFn = std::function<int()>;
    using CodeBlockFn = std::function<BlockResult(const std::string &source, int widthPx,
                                                   float devicePixelRatio)>;
    using VaultEventFn = std::function<void(const std::string &event, const std::string &path)>;

    void onCommand(std::string commandID, CommandFn fn) { m_commands[std::move(commandID)] = std::move(fn); }
    void onCodeBlock(std::string language, CodeBlockFn fn) { m_codeBlocks[std::move(language)] = std::move(fn); }
    void onVaultEvent(VaultEventFn fn) { m_vaultEvent = std::move(fn); }

    /// The host table, valid for the plugin's lifetime once init() has run.
    const noteappc_host *host() const { return m_host; }

    // --- called only through the C trampolines NOTEAPPC_PLUGIN_ENTRY wires up --

    int init(const noteappc_host *host)
    {
        m_host = host;
        return 0;
    }

    int runCommand(const char *commandID)
    {
        const auto it = m_commands.find(commandID ? commandID : "");
        if (it == m_commands.end())
            return 1;
        return it->second();
    }

    int renderCodeBlock(const char *language, const char *source, int32_t widthPx,
                        float devicePixelRatio, noteappc_block_result *out)
    {
        const auto it = m_codeBlocks.find(language ? language : "");
        if (it == m_codeBlocks.end())
            return 1;
        const BlockResult result = it->second(source ? source : "", widthPx, devicePixelRatio);
        std::memset(out, 0, sizeof(*out));
        if (!result.rgba.empty()) {
            out->image.width = result.width;
            out->image.height = result.height;
            out->image.rgba = static_cast<uint8_t *>(std::malloc(result.rgba.size()));
            if (!out->image.rgba)
                return 1;
            std::memcpy(out->image.rgba, result.rgba.data(), result.rgba.size());
            return 0;
        }
        out->text = static_cast<char *>(std::malloc(result.text.size() + 1));
        if (!out->text)
            return 1;
        std::memcpy(out->text, result.text.c_str(), result.text.size() + 1);
        return 0;
    }

    void vaultEvent(const char *event, const char *path)
    {
        if (m_vaultEvent)
            m_vaultEvent(event ? event : "", path ? path : "");
    }

private:
    const noteappc_host *m_host = nullptr;
    std::unordered_map<std::string, CommandFn> m_commands;
    std::unordered_map<std::string, CodeBlockFn> m_codeBlocks;
    VaultEventFn m_vaultEvent;
};

} // namespace noteAppC

/// Wires a noteAppC::Plugin (`plugin`) into the C ABI the host dlsym()s.
/// `initFn` is a callable run once, before init() returns, to register
/// commands/code blocks - split out because C++ file-scope initialisation
/// order is not guaranteed across translation units, so registering inside
/// the plugin's own init is the only safe place.
#define NOTEAPPC_PLUGIN_ENTRY(plugin, initFn)                                                    \
    extern "C" {                                                                                 \
    static int noteappc_wrap_init(const noteappc_host *host)                                     \
    {                                                                                             \
        const int rc = (plugin).init(host);                                                      \
        if (rc != 0)                                                                              \
            return rc;                                                                            \
        return (initFn)();                                                                        \
    }                                                                                             \
    static int noteappc_wrap_run_command(const char *id) { return (plugin).runCommand(id); }      \
    static int noteappc_wrap_render_code_block(const char *language, const char *source,          \
                                               int32_t widthPx, float dpr,                         \
                                               noteappc_block_result *out)                         \
    {                                                                                              \
        return (plugin).renderCodeBlock(language, source, widthPx, dpr, out);                     \
    }                                                                                              \
    static void noteappc_wrap_vault_event(const char *event, const char *path)                    \
    {                                                                                              \
        (plugin).vaultEvent(event, path);                                                          \
    }                                                                                              \
    static const noteappc_plugin noteappc_wrap_table = {                                          \
        NOTEAPPC_PLUGIN_API,        noteappc_wrap_init,             nullptr,                      \
        noteappc_wrap_run_command,  noteappc_wrap_render_code_block, noteappc_wrap_vault_event};   \
    NOTEAPPC_EXPORT const noteappc_plugin *noteappc_plugin_entry(void) { return &noteappc_wrap_table; } \
    }

#endif /* NOTEAPPC_PLUGIN_HPP */
