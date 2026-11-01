// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "NativeEngine.h"

#include <dlfcn.h>

namespace noteAppC {

NativeEngine::~NativeEngine()
{
    unload();
}

bool NativeEngine::load(const PluginManifest &manifest, const noteappc_host *host, QString *error)
{
    // RTLD_NOW rather than RTLD_LAZY: a plugin with a genuinely missing
    // symbol should fail here, loudly, and never mid-command with the app
    // mid-way through waiting on a response.
    m_handle = dlopen(manifest.libraryPath().toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
    if (!m_handle) {
        if (error)
            *error = QStringLiteral("dlopen failed: %1").arg(QString::fromLocal8Bit(dlerror()));
        return false;
    }

    dlerror(); // clear any stale error before dlsym, the documented way to tell a NULL result from a legitimately NULL symbol
    void *symbol = dlsym(m_handle, "noteappc_plugin_entry");
    const char *symbolError = dlerror();
    if (!symbol || symbolError) {
        if (error)
            *error = QStringLiteral("dlsym(noteappc_plugin_entry) failed: %1")
                        .arg(symbolError ? QString::fromLocal8Bit(symbolError) : QStringLiteral("symbol not found"));
        unload();
        return false;
    }

    const auto entry = reinterpret_cast<noteappc_plugin_entry_fn>(symbol);
    m_table = entry();
    if (!m_table) {
        if (error)
            *error = QStringLiteral("noteappc_plugin_entry() returned null");
        unload();
        return false;
    }
    if (m_table->api != NOTEAPPC_PLUGIN_API) {
        if (error)
            *error = QStringLiteral("plugin built for API %1, host is API %2")
                        .arg(m_table->api)
                        .arg(NOTEAPPC_PLUGIN_API);
        m_table = nullptr;
        unload();
        return false;
    }
    if (manifest.apiVersion() != 0 && manifest.apiVersion() != static_cast<int>(m_table->api)) {
        if (error)
            *error = QStringLiteral("plugin.ini declares api = %1, the library reports %2")
                        .arg(manifest.apiVersion())
                        .arg(m_table->api);
        m_table = nullptr;
        unload();
        return false;
    }

    if (!m_table->init) {
        if (error)
            *error = QStringLiteral("plugin has no init()");
        m_table = nullptr;
        unload();
        return false;
    }
    if (const int rc = m_table->init(host); rc != 0) {
        if (error)
            *error = QStringLiteral("init() returned %1").arg(rc);
        m_table = nullptr;
        unload();
        return false;
    }
    return true;
}

void NativeEngine::unload()
{
    if (m_table && m_table->shutdown)
        m_table->shutdown();
    m_table = nullptr;
    if (m_handle) {
        dlclose(m_handle);
        m_handle = nullptr;
    }
}

int NativeEngine::runCommand(const QString &commandID)
{
    if (!m_table || !m_table->run_command)
        return 1;
    return m_table->run_command(commandID.toUtf8().constData());
}

int NativeEngine::renderCodeBlock(const QString &language, const QString &source, int widthPx,
                                  float devicePixelRatio, noteappc_block_result *out)
{
    if (!m_table || !m_table->render_code_block)
        return 1;
    return m_table->render_code_block(language.toUtf8().constData(), source.toUtf8().constData(), widthPx,
                                      devicePixelRatio, out);
}

void NativeEngine::vaultEvent(const QString &event, const QString &path)
{
    if (m_table && m_table->vault_event)
        m_table->vault_event(event.toUtf8().constData(), path.toUtf8().constData());
}

} // namespace noteAppC
