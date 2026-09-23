#include "AiConfig.h"

#include <QDir>
#include <QSettings>

namespace noteAppC::ai {

QString AiConfig::path()
{
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    return base + QStringLiteral("/noteAppC/ai.ini");
}

AiConfig AiConfig::load()
{
    AiConfig config;
    // Read-only: this plugin never writes ai.ini, and the sandbox only
    // binds the directory read-only anyway (PluginHost::sandboxArguments).
    const QSettings ini(path(), QSettings::IniFormat);

    const QString providerName =
        ini.value(QStringLiteral("ai/provider"), QStringLiteral("ollama")).toString().trimmed().toLower();
    if (providerName == QLatin1String("anthropic")) {
        config.provider = Provider::Anthropic;
        config.baseURL = QStringLiteral("https://api.anthropic.com");
        config.model = QStringLiteral("claude-sonnet-5");
    } else if (providerName == QLatin1String("openai")) {
        config.provider = Provider::OpenAICompatible;
        config.baseURL = QStringLiteral("https://api.openai.com");
        config.model = QStringLiteral("gpt-4o-mini");
    } // else Provider::Ollama - the struct's own defaults already fit

    config.baseURL = ini.value(QStringLiteral("ai/base-url"), config.baseURL).toString();
    config.model = ini.value(QStringLiteral("ai/model"), config.model).toString();
    config.apiKeyEnv = ini.value(QStringLiteral("ai/api-key-env"), config.apiKeyEnv).toString();
    // Kept under plugin.ini's own timeout-ms ceiling (300000) with room to
    // spare, so a slow call's own timeout fires first - a clean AiResult
    // error instead of PluginHost killing the whole host process.
    config.httpTimeoutMs =
        qBound(1000, ini.value(QStringLiteral("ai/timeout-ms"), config.httpTimeoutMs).toInt(), 280000);
    return config;
}

} // namespace noteAppC::ai
