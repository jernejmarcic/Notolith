#pragma once

#include "Engine.h"

namespace noteAppC {

/// `engine = native` (plugin.ini's default): dlopen()s the plugin's `.so`
/// and calls straight through the C ABI in plugin.h. This is the only
/// engine that exists today - see QuickJSEngine.h for the seam a scripted
/// engine would plug into.
class NativeEngine : public Engine {
public:
    ~NativeEngine() override;

    bool load(const PluginManifest &manifest, const noteappc_host *host, QString *error) override;
    void unload() override;
    int runCommand(const QString &commandID) override;
    int renderCodeBlock(const QString &language, const QString &source, int widthPx,
                        float devicePixelRatio, noteappc_block_result *out) override;
    void vaultEvent(const QString &event, const QString &path) override;

private:
    void *m_handle = nullptr;
    const noteappc_plugin *m_table = nullptr;
};

} // namespace noteAppC
