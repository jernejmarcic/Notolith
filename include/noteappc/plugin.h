/* Public C ABI for a noteAppC runtime plugin (docs/PLUGINS.md, Tier 2).
 *
 * A plugin is a shared library (`libfoo.so`) loaded with dlopen() inside
 * `noteAppC-plugin-host`, never inside the app itself - see
 * tools/plugin_host. This header is the only thing a plugin author needs:
 * plain C, so the ABI survives a difference in compiler, libstdc++ or Qt
 * version between the app and a third-party plugin built against an older
 * noteAppC. C++ authors get a thin, optional header-only wrapper in
 * plugin.hpp; a plugin can also just include this file and write C, the way
 * examples/plugins/word-stats does.
 *
 * Every string is UTF-8 and NUL-terminated. Unless documented otherwise, a
 * `const char *` the host passes into a plugin call is owned by the host and
 * is only valid for the duration of that call - copy it if it is needed
 * later. A `const char *` a *host table* function returns is owned by the
 * host and stays valid only until the next call to any host table function -
 * the getenv() convention - copy it before calling the table again.
 */
#ifndef NOTEAPPC_PLUGIN_H
#define NOTEAPPC_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to either struct below. A plugin built
 * against a different value than the host implements is refused before
 * init() runs (checked against noteappc_plugin::api by the host). */
#define NOTEAPPC_PLUGIN_API 1

#if defined(_WIN32)
#define NOTEAPPC_EXPORT __declspec(dllexport)
#else
#define NOTEAPPC_EXPORT __attribute__((visibility("default")))
#endif

/* A rendered code block image: top-left origin, row-major, 4 bytes per
 * pixel, straight (not premultiplied) alpha - the same layout QImage::Format
 * ARGB32 reads if the host swaps R/B, which it does. Allocated by the
 * plugin with malloc(); the host frees it with free() once it has copied the
 * bytes out, immediately after the render_code_block call returns. */
typedef struct noteappc_image {
    int32_t width;
    int32_t height;
    uint8_t *rgba; /* width * height * 4 bytes, or NULL */
} noteappc_image;

/* A plugin's answer to a fenced code block: either `text` (rendered as an
 * ordinary Markdown-ish text block, e.g. a monospace panel) or `image`, never
 * both. `text` is malloc()'d by the plugin; the host frees it. Leaving both
 * NULL/empty means "could not render", shown as an error in the block. */
typedef struct noteappc_block_result {
    char *text;          /* malloc'd UTF-8, or NULL */
    noteappc_image image; /* image.rgba non-NULL means "use this instead" */
} noteappc_block_result;

/* What the host offers a plugin. Every call after init() is synchronous from
 * the plugin's point of view - the host may be relaying it across the
 * protocol to the app process and blocking while it waits - so a plugin must
 * not call back into these from a signal handler or another thread without
 * its own locking. Functions gated by a capability the user did not grant
 * fail as documented per-function; the request is also refused and logged on
 * the app side, so a plugin cannot probe for a permission by trying it.
 *
 * Every function pointer is non-NULL for the lifetime of the plugin: one a
 * build has no use for (e.g. clipboard_read when the plugin never asked for
 * the `clipboard` permission) still points at a stub that reports the
 * capability as refused, so a plugin never needs a NULL check before calling
 * through the table. */
typedef struct noteappc_host {
    uint32_t api;

    /* read-notes. NULL (path/text) or an empty list (list_notes) on
     * failure or when the permission was not granted - a plugin cannot tell
     * the two apart, which is intentional: neither is actionable. */
    const char *(*active_note_path)(void);
    const char *(*active_note_text)(void);
    /* `vault_path` is relative to the vault root, forward slashes. */
    const char *(*read_note)(const char *vault_path);
    /* NUL-separated, double-NUL-terminated list of vault-relative paths -
     * plain C has no string list, and this needs no allocation on the
     * plugin's side to walk. Empty (a single NUL) when there are none or the
     * permission was refused. */
    const char *(*list_notes)(void);

    /* write-notes. Both return 0 on success, non-zero (and log() explains
     * why) on failure or refusal. */
    int (*replace_selection)(const char *utf8);
    int (*write_note)(const char *vault_path, const char *utf8);

    /* clipboard. clipboard_read follows the same "NULL means refused or
     * empty" rule as read_note. */
    const char *(*clipboard_read)(void);
    int (*clipboard_write)(const char *utf8);

    /* Always available - neither needs a capability. show_notice puts a
     * transient message in the app's status area; log goes to the plugin's
     * line of the app's log, visible on the settings page. */
    void (*show_notice)(const char *utf8);
    void (*log)(const char *utf8);
} noteappc_host;

/* What a plugin offers the host. A NULL function pointer other than
 * shutdown/render_code_block/vault_event means "this plugin does not do
 * that" - the host only calls run_command for a command id the plugin's own
 * plugin.ini declared, so run_command may be NULL for a code-block-only
 * plugin, and render_code_block may be NULL for a command-only one. */
typedef struct noteappc_plugin {
    uint32_t api;

    /* Called once after the host process starts, before anything else.
     * `host` is valid until shutdown() returns; the plugin should not keep
     * calling through it afterwards. Non-zero return aborts startup. */
    int (*init)(const noteappc_host *host);
    /* Called once, before the host process exits. May be NULL. */
    void (*shutdown)(void);

    /* `command_id` is the plugin's own id from plugin.ini's [provides]
     * commands list, not the app's qualified "pluginID:commandID" form.
     * Runs on the app's UI-thread budget (docs/PLUGINS.md: ~100 ms before a
     * plugin is flagged slow) - never blocks on the network or disk without
     * a plan for that budget. Non-zero return shows as a failed command. */
    int (*run_command)(const char *command_id);

    /* `language` is one this plugin declared in [provides] code-blocks.
     * `source` is the fenced block's body, fence lines stripped.
     * `width_px`/`device_pixel_ratio` describe the space available if the
     * plugin renders an image; ignored for a text result. Runs off the UI
     * thread; the result is cached by (language, source, width, theme).
     * Non-zero return, or a result with neither text nor image set, shows
     * as a failed block. */
    int (*render_code_block)(const char *language, const char *source, int32_t width_px,
                             float device_pixel_ratio, noteappc_block_result *out);

    /* `event` is "note-opened", "note-saved" or "note-renamed"; `path` is
     * vault-relative, and for a rename is the *new* path (the old one was
     * already gone by the time this fires). Fire-and-forget: the return
     * value, if any, is ignored, and the app does not wait for it. May be
     * NULL if the plugin did not declare any [provides] events. */
    void (*vault_event)(const char *event, const char *path);
} noteappc_plugin;

/* The one symbol the host looks up with dlsym(), name exactly
 * "noteappc_plugin_entry". Must return a pointer valid for the life of the
 * process; a plugin typically returns the address of a static const table. */
typedef const noteappc_plugin *(*noteappc_plugin_entry_fn)(void);
NOTEAPPC_EXPORT const noteappc_plugin *noteappc_plugin_entry(void);

#ifdef __cplusplus
}
#endif

#endif /* NOTEAPPC_PLUGIN_H */
