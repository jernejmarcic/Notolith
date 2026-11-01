// SPDX-License-Identifier: GPL-3.0-or-later
/* word-stats: the example plugin docs/PLUGINS.md's Tier 2 asks for, plain C
 * against include/noteappc/plugin.h with no wrapper - the shortest path to a
 * real plugin, which is also the best proof the C ABI actually works. It
 * doubles as tests/test_plugins_host.cpp's end-to-end fixture: "count-words"
 * exercises a normal command, "calc" a normal code block, and "hang"/"crash"
 * deliberately misbehave so the host's timeout-kill and crash-survival paths
 * have something real to kill.
 */
#include <noteappc/plugin.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static const noteappc_host *g_host;

static int on_init(const noteappc_host *host)
{
    g_host = host;
    return 0;
}

static void on_shutdown(void) { }

static int countWords(const char *text)
{
    int count = 0;
    int inWord = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            inWord = 0;
        } else {
            if (!inWord)
                ++count;
            inWord = 1;
        }
    }
    return count;
}

static int on_run_command(const char *commandID)
{
    if (strcmp(commandID, "count-words") == 0) {
        const char *text = g_host->active_note_text();
        char message[64];
        snprintf(message, sizeof message, "%d words", countWords(text ? text : ""));
        g_host->show_notice(message);
        return 0;
    }
    if (strcmp(commandID, "hang") == 0) {
        /* Test fixture only - PluginHost's per-request timeout (docs/PLUGINS.md:
         * "a request that does not answer in time kills and later restarts
         * the host"). A real command must never do this. */
        for (;;)
            sleep(3600);
    }
    if (strcmp(commandID, "crash") == 0) {
        /* Test fixture only - proves a crashed host does not take the app
         * (or, in tests/test_plugins_host.cpp, the test process) with it. */
        abort();
    }
    if (strncmp(commandID, "read-path:", 10) == 0) {
        /* Test fixture only - proves the sandbox, not permission checking:
         * a plain fopen() from inside the host process, on a path the test
         * put outside the plugin's own directory. Under bwrap this fails
         * with ENOENT no matter what the path is, because the mount
         * namespace never had it; without bwrap it would succeed like any
         * other process running as the same user. */
        FILE *file = fopen(commandID + 10, "rb");
        if (!file) {
            g_host->show_notice("cannot-read");
            return 0;
        }
        char buffer[256];
        const size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
        buffer[n] = '\0';
        fclose(file);
        g_host->show_notice(buffer[0] ? buffer : "empty");
        return 0;
    }
    return 1;
}

/* calc: one arithmetic line in, "line = result" out. A single operator, no
 * precedence, no parentheses - Limit: this is a test fixture's
 * calculator, not noteAppC's own; a real one would need a proper
 * recursive-descent parser, add it if calc ever ships as more than a demo. */
static void evaluateLine(const char *line, char *out, size_t outSize)
{
    char op = 0;
    const char *opAt = NULL;
    for (const char *p = line; *p; ++p) {
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/') {
            if (*p == '-' && p == line)
                continue; /* a leading '-' is a sign, not an operator */
            op = *p;
            opAt = p;
            break;
        }
    }
    if (!op) {
        snprintf(out, outSize, "%s = ?", line);
        return;
    }
    const double a = atof(line);
    const double b = atof(opAt + 1);
    double result = 0;
    switch (op) {
    case '+':
        result = a + b;
        break;
    case '-':
        result = a - b;
        break;
    case '*':
        result = a * b;
        break;
    case '/':
        result = b != 0 ? a / b : 0;
        break;
    }
    snprintf(out, outSize, "%s = %g", line, result);
}

static int on_render_code_block(const char *language, const char *source, int32_t widthPx,
                                float devicePixelRatio, noteappc_block_result *out)
{
    (void)widthPx;
    (void)devicePixelRatio;
    if (strcmp(language, "calc") != 0)
        return 1;
    memset(out, 0, sizeof(*out));

    size_t capacity = 256;
    size_t length = 0;
    char *text = malloc(capacity);
    if (!text)
        return 1;
    text[0] = '\0';

    const char *lineStart = source;
    for (;;) {
        const char *lineEnd = strchr(lineStart, '\n');
        const size_t lineLen = lineEnd ? (size_t)(lineEnd - lineStart) : strlen(lineStart);
        char line[256];
        const size_t copyLen = lineLen < sizeof(line) - 1 ? lineLen : sizeof(line) - 1;
        memcpy(line, lineStart, copyLen);
        line[copyLen] = '\0';

        if (copyLen > 0) {
            char rendered[300];
            evaluateLine(line, rendered, sizeof rendered);
            const size_t renderedLen = strlen(rendered);
            while (length + renderedLen + 2 > capacity) {
                capacity *= 2;
                char *grown = realloc(text, capacity);
                if (!grown) {
                    free(text);
                    return 1;
                }
                text = grown;
            }
            memcpy(text + length, rendered, renderedLen);
            length += renderedLen;
            text[length++] = '\n';
            text[length] = '\0';
        }
        if (!lineEnd)
            break;
        lineStart = lineEnd + 1;
    }

    out->text = text; /* the host frees this once it has copied the bytes out */
    return 0;
}

static void on_vault_event(const char *event, const char *path)
{
    (void)event;
    (void)path;
}

static const noteappc_plugin kPlugin = {
    NOTEAPPC_PLUGIN_API, on_init, on_shutdown, on_run_command, on_render_code_block, on_vault_event,
};

NOTEAPPC_EXPORT const noteappc_plugin *noteappc_plugin_entry(void)
{
    return &kPlugin;
}
