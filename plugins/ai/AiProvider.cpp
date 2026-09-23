#include "AiProvider.h"

#include <curl/curl.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace noteAppC::ai {

namespace {

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buffer = static_cast<QByteArray *>(userdata);
    const size_t bytes = size * nmemb;
    // A cap against a server that streams forever - generous for any answer
    // a code block or a notice could use, well short of PluginFrameReader's
    // own 32 MiB frame ceiling once this travels back over the protocol.
    // Returning short of `bytes` is libcurl's documented way to abort a
    // transfer from inside a write callback.
    constexpr qsizetype kMaxResponseBytes = 8 * 1024 * 1024;
    if (buffer->size() + qsizetype(bytes) > kMaxResponseBytes)
        return 0;
    buffer->append(ptr, qsizetype(bytes));
    return bytes;
}

/// RAII for curl_slist - every early return below (a config error found
/// before curl_easy_perform even runs) must not leak the header list.
struct CurlSlist {
    curl_slist *list = nullptr;
    ~CurlSlist()
    {
        if (list)
            curl_slist_free_all(list);
    }
};

struct HttpResult {
    bool ok = false;  ///< false only for a transport-level failure - curl itself did not complete the call
    long status = 0;
    QByteArray body;
    QString error;    ///< set when !ok
};

/// One request. `body` empty means GET. Never throws; a libcurl failure
/// (connection refused, DNS failure, the sandbox's network being unshared,
/// curl's own CURLOPT_TIMEOUT_MS expiring) comes back as `!ok`, never an
/// exception or a crash - this is the one place in the plugin that touches
/// the network, so every caller below inherits that guarantee for free.
///
/// libcurl's global init is deliberately not called here: curl_easy_init()
/// performs it automatically the first time it is needed if nothing already
/// has (documented behaviour), which is enough for a plugin host process
/// that only ever calls curl from this one, single-threaded call path.
HttpResult performRequest(const QString &url, const QStringList &headers, const QByteArray &body, int timeoutMs)
{
    HttpResult result;
    CURL *curl = curl_easy_init();
    if (!curl) {
        result.error = QStringLiteral("could not initialise libcurl");
        return result;
    }

    QByteArray responseBody;
    const QByteArray urlUtf8 = url.toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(qMin(timeoutMs, 10000)));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // never chase a redirect away from the configured base URL

    CurlSlist slist;
    for (const QString &header : headers)
        slist.list = curl_slist_append(slist.list, header.toUtf8().constData());
    if (slist.list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist.list);

    if (!body.isEmpty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.constData());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result.error = QStringLiteral("network error: %1").arg(QString::fromLatin1(curl_easy_strerror(rc)));
        curl_easy_cleanup(curl);
        return result;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    result.ok = true;
    result.status = status;
    result.body = responseBody;
    return result;
}

/// The NAME of the environment variable ai.ini declared, resolved to its
/// VALUE - read straight from this process's own environment (forwarded
/// into the sandbox by PluginHost::sandboxArguments from `[plugin]
/// env-passthrough`, never from a file) and never copied anywhere else -
/// not returned to the caller as anything but this one QString, which the
/// caller uses only to build an HTTP header for this one request.
QString resolveApiKey(const AiConfig &config, QString *error)
{
    if (config.apiKeyEnv.isEmpty()) {
        *error = QStringLiteral("ai.ini has no api-key-env set for this provider");
        return {};
    }
    const QByteArray value = qgetenv(config.apiKeyEnv.toUtf8().constData());
    if (value.isEmpty()) {
        *error = QStringLiteral("environment variable %1 is not set").arg(config.apiKeyEnv);
        return {};
    }
    return QString::fromUtf8(value);
}

QJsonArray chatMessages(const QString &systemPrompt, const QString &userPrompt, bool systemAsMessage)
{
    QJsonArray messages;
    if (systemAsMessage && !systemPrompt.isEmpty()) {
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), systemPrompt}});
    }
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), userPrompt}});
    return messages;
}

QString extractAnswer(Provider provider, const QJsonObject &object)
{
    switch (provider) {
    case Provider::Ollama:
        return object.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
    case Provider::OpenAICompatible: {
        const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty())
            return {};
        return choices.first().toObject().value(QStringLiteral("message")).toObject()
                   .value(QStringLiteral("content")).toString();
    }
    case Provider::Anthropic: {
        for (const QJsonValue &block : object.value(QStringLiteral("content")).toArray()) {
            const QJsonObject blockObject = block.toObject();
            if (blockObject.value(QStringLiteral("type")).toString() == QLatin1String("text"))
                return blockObject.value(QStringLiteral("text")).toString();
        }
        return {};
    }
    }
    return {};
}

} // namespace

AiResult runChat(const AiConfig &config, const QString &systemPrompt, const QString &userPrompt)
{
    AiResult result;
    if (userPrompt.trimmed().isEmpty()) {
        result.error = QStringLiteral("nothing to send - the note or selection is empty");
        return result;
    }

    QString url;
    QStringList headers = {QStringLiteral("Content-Type: application/json")};
    QJsonObject body;
    body.insert(QStringLiteral("model"), config.model);
    QString keyError;

    switch (config.provider) {
    case Provider::Ollama:
        url = config.baseURL + QStringLiteral("/api/chat");
        body.insert(QStringLiteral("messages"), chatMessages(systemPrompt, userPrompt, true));
        body.insert(QStringLiteral("stream"), false);
        break;
    case Provider::OpenAICompatible:
        url = config.baseURL + QStringLiteral("/v1/chat/completions");
        body.insert(QStringLiteral("messages"), chatMessages(systemPrompt, userPrompt, true));
        if (const QString key = resolveApiKey(config, &keyError); key.isEmpty()) {
            result.error = keyError;
            return result;
        } else {
            headers << QStringLiteral("Authorization: Bearer ") + key;
        }
        break;
    case Provider::Anthropic:
        url = config.baseURL + QStringLiteral("/v1/messages");
        // Anthropic's Messages API takes the system prompt as its own top-
        // level field, never as a "system"-role message.
        body.insert(QStringLiteral("messages"), chatMessages(QString(), userPrompt, false));
        body.insert(QStringLiteral("max_tokens"), 1024);
        if (!systemPrompt.isEmpty())
            body.insert(QStringLiteral("system"), systemPrompt);
        if (const QString key = resolveApiKey(config, &keyError); key.isEmpty()) {
            result.error = keyError;
            return result;
        } else {
            headers << QStringLiteral("x-api-key: ") + key;
            headers << QStringLiteral("anthropic-version: 2023-06-01");
        }
        break;
    }

    const HttpResult http =
        performRequest(url, headers, QJsonDocument(body).toJson(QJsonDocument::Compact), config.httpTimeoutMs);
    if (!http.ok) {
        result.error = http.error;
        return result;
    }
    if (http.status < 200 || http.status >= 300) {
        result.error =
            QStringLiteral("HTTP %1 from %2: %3").arg(http.status).arg(url, QString::fromUtf8(http.body.left(200)));
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(http.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("malformed JSON response from %1").arg(url);
        return result;
    }

    const QString text = extractAnswer(config.provider, doc.object());
    if (text.isEmpty()) {
        result.error = QStringLiteral("unexpected response shape from %1").arg(url);
        return result;
    }
    result.ok = true;
    result.text = text;
    return result;
}

AiModelsResult listOllamaModels(const AiConfig &config)
{
    AiModelsResult result;
    if (config.provider != Provider::Ollama) {
        result.error = QStringLiteral("model listing is only supported for the ollama provider");
        return result;
    }

    const HttpResult http =
        performRequest(config.baseURL + QStringLiteral("/api/tags"), {}, {}, config.httpTimeoutMs);
    if (!http.ok) {
        result.error = http.error;
        return result;
    }
    if (http.status < 200 || http.status >= 300) {
        result.error = QStringLiteral("HTTP %1 from %2").arg(http.status).arg(config.baseURL);
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(http.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("malformed JSON response from %1").arg(config.baseURL);
        return result;
    }
    for (const QJsonValue &value : doc.object().value(QStringLiteral("models")).toArray()) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            result.models << name;
    }
    result.ok = true;
    return result;
}

} // namespace noteAppC::ai
