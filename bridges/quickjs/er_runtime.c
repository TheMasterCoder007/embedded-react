/*
 * er_runtime — portable QuickJS host core for Flow A (see er_runtime.h).
 *
 * Owns the JSRuntime/JSContext lifecycle and the host globals (console, screen, optional __erPersist),
 * and wraps load/pump/reset/error-overlay. No platform deps: the caller provides the backend, the app
 * bytes, and the frame loop. Shared by the desktop demo + simulator (examples/linux/host.c) and usable
 * directly from MCU firmware.
 */

#include "er_runtime.h"

#include "er_assets.h"        /* er_assets_load_pack (ERPK asset section of the container) */
#include "er_scene.h"         /* er_reset */
#include "native_ui_bridge.h" /* er_bridge_install / er_bridge_pump / er_bridge_run_bytecode */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief QuickJS release this build is bytecode-compatible with — MUST match the FetchContent pin in
 *        bridges/quickjs/CMakeLists.txt and the tag the packager stamps into the container. A config
 *        built for a different QuickJS is rejected (ER_CONTAINER_BAD_QJS) rather than run as garbage.
 */
#define ER_QUICKJS_TAG "v0.15.0"

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private (single-instance, like the engine + bridge)
 ---------------------------------------------------------------------------------------------------------------------*/

static JSRuntime* s_rt = NULL;
static JSContext* s_ctx = NULL;
static ErRuntimeConfig s_cfg;

/** @brief Last uncaught JS error (message + stack), for er_runtime_show_error / _last_error. */
static char s_last_error[512] = "";

/** @brief Error-overlay app: builds a red "JS error" screen from the `__error` global (RN redbox). */
static const char* k_error_app =
    "const W = screen.width, H = screen.height;\n"
    "const msg = (typeof __error === 'string' && __error) ? __error : 'Unknown error';\n"
    "const root = NativeUI.createNode('View');\n"
    "NativeUI.setProps(root, { width: W, height: H, backgroundColor: '#7a0b0b',\n"
    "                          flexDirection: 'column', padding: 24, gap: 12 });\n"
    "const title = NativeUI.createNode('Text');\n"
    "NativeUI.setProps(title, { text: 'JS error', color: '#ffffff', fontSize: 24, fontWeight: 'bold' });\n"
    "NativeUI.appendChild(root, title);\n"
    "const body = NativeUI.createNode('Text');\n"
    "NativeUI.setProps(body, { text: msg, color: '#ffd9d9', fontSize: 14, width: W - 48, numberOfLines: 20 });\n"
    "NativeUI.appendChild(root, body);\n"
    "const hint = NativeUI.createNode('Text');\n"
    "NativeUI.setProps(hint, { text: 'fix the source and reload', color: '#ff9b9b', fontSize: 12 });\n"
    "NativeUI.appendChild(root, hint);\n"
    "NativeUI.setRoot(root);\n"
    "NativeUI.commit();\n";

/*----------------------------------------------------------------------------------------------------------------------
 - Console / logging
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Emits one log line to the configured sink (or stdout). */
static void emit_line(const char* line)
{
    if (s_cfg.log)
    {
        s_cfg.log(line);
    }
    else
    {
        fputs(line, stdout);
        fputc('\n', stdout);
    }
}

/** @brief console.log/warn/error: joins arguments with spaces into one line for the sink. */
static JSValue rt_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    const int n = argc > 0 ? argc : 0;
    const char** parts = (const char**)calloc((size_t)(n > 0 ? n : 1), sizeof(char*));
    size_t total = 1; /* null terminator */
    for (int i = 0; i < n; i++)
    {
        parts[i] = JS_ToCString(ctx, argv[i]);
        total += (parts[i] ? strlen(parts[i]) : 0) + (i ? 1u : 0u);
    }
    char* line = (char*)malloc(total);
    if (line)
    {
        char* p = line;
        for (int i = 0; i < n; i++)
        {
            if (i)
            {
                *p++ = ' ';
            }
            if (parts[i])
            {
                const size_t l = strlen(parts[i]);
                memcpy(p, parts[i], l);
                p += l;
            }
        }
        *p = '\0';
        emit_line(line);
        free(line);
    }
    for (int i = 0; i < n; i++)
    {
        if (parts[i])
        {
            JS_FreeCString(ctx, parts[i]);
        }
    }
    free(parts);
    return JS_UNDEFINED;
}

/** @brief Installs `console` (log/warn/error all routed to the sink). */
static void install_console(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JSValue log = JS_NewCFunction(ctx, rt_console_log, "log", 1);

    JS_SetPropertyStr(ctx, console, "log", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "warn", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "error", log);
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/** @brief Injects the `screen` global ({ width, height, scale }). */
static void install_screen(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue screen = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, s_cfg.screen_width));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, s_cfg.screen_height));
    JS_SetPropertyStr(ctx, screen, "scale", JS_NewFloat64(ctx, (double)s_cfg.screen_scale));
    JS_SetPropertyStr(ctx, global, "screen", screen);
    JS_FreeValue(ctx, global);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Persisted state (usePersistentState across reloads; opt-in via cfg.install_persist)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum number of distinct usePersistentState keys retained across reloads. */
#define ER_PERSIST_MAX 64

/** @brief key → JSON-value store that outlives the recreated JS context. */
static struct
{
    char* key;
    char* val;
} s_persist[ER_PERSIST_MAX];
static int s_persist_count = 0;

/** @brief Heap-duplicates a C string; NULL on allocation failure. */
static char* persist_dup(const char* s)
{
    const size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d)
    {
        memcpy(d, s, n);
    }
    return d;
}

/** @brief __erPersist.get(key) → stored JSON string, or undefined. */
static JSValue js_persist_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key)
    {
        return JS_UNDEFINED;
    }
    JSValue r = JS_UNDEFINED;
    for (int i = 0; i < s_persist_count; i++)
    {
        if (s_persist[i].val && strcmp(s_persist[i].key, key) == 0)
        {
            r = JS_NewString(ctx, s_persist[i].val);
            break;
        }
    }
    JS_FreeCString(ctx, key);
    return r;
}

/** @brief __erPersist.set(key, jsonString) → stores it, replacing any prior value. */
static JSValue js_persist_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 2)
    {
        return JS_UNDEFINED;
    }
    const char* key = JS_ToCString(ctx, argv[0]);
    const char* val = JS_ToCString(ctx, argv[1]);
    if (key && val)
    {
        int idx = -1;
        for (int i = 0; i < s_persist_count; i++)
        {
            if (strcmp(s_persist[i].key, key) == 0)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0 && s_persist_count < ER_PERSIST_MAX)
        {
            idx = s_persist_count++;
            s_persist[idx].key = persist_dup(key);
            s_persist[idx].val = NULL;
        }
        if (idx >= 0 && s_persist[idx].key)
        {
            free(s_persist[idx].val);
            s_persist[idx].val = persist_dup(val);
        }
    }
    if (key)
    {
        JS_FreeCString(ctx, key);
    }
    if (val)
    {
        JS_FreeCString(ctx, val);
    }
    return JS_UNDEFINED;
}

/** @brief Installs the `__erPersist` global (get/set) backed by the host-side store. */
static void install_persist(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue persist = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, persist, "get", JS_NewCFunction(ctx, js_persist_get, "get", 1));
    JS_SetPropertyStr(ctx, persist, "set", JS_NewCFunction(ctx, js_persist_set, "set", 2));
    JS_SetPropertyStr(ctx, global, "__erPersist", persist);
    JS_FreeValue(ctx, global);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Context lifecycle + eval
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Creates a fresh context and installs the bridge + host globals (used at init and reset). */
static void install_globals(void)
{
    s_ctx = JS_NewContext(s_rt);
    if (!s_ctx)
    {
        return;
    }
    install_console(s_ctx);
    install_screen(s_ctx);
    er_bridge_install(s_ctx);
    if (s_cfg.install_persist)
    {
        install_persist(s_ctx);
    }
}

/** @brief Reports an uncaught exception to the log sink + captures it; frees the eval result. */
static bool rt_report(JSValue result)
{
    bool ok = true;
    if (JS_IsException(result))
    {
        JSValue exc = JS_GetException(s_ctx);
        const char* msg = JS_ToCString(s_ctx, exc);
        JSValue stack = JS_GetPropertyStr(s_ctx, exc, "stack");
        const char* st = JS_IsUndefined(stack) ? NULL : JS_ToCString(s_ctx, stack);

        char head[600];
        snprintf(head, sizeof(head), "JS exception: %s", msg ? msg : "(unknown)");
        emit_line(head);
        if (st)
        {
            emit_line(st);
        }
        snprintf(s_last_error, sizeof(s_last_error), "%s%s%s", msg ? msg : "(unknown)", st ? "\n\n" : "", st ? st : "");

        if (msg)
        {
            JS_FreeCString(s_ctx, msg);
        }
        if (st)
        {
            JS_FreeCString(s_ctx, st);
        }
        JS_FreeValue(s_ctx, stack);
        JS_FreeValue(s_ctx, exc);
        ok = false;
    }
    JS_FreeValue(s_ctx, result);
    return ok;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_runtime_init(const ErRuntimeConfig* cfg)
{
    s_cfg = *cfg;
    if (s_cfg.screen_scale <= 0.0f)
    {
        s_cfg.screen_scale = 1.0f;
    }
    s_rt = JS_NewRuntime();
    if (!s_rt)
    {
        return false;
    }
    install_globals();
    return s_ctx != NULL;
}

JSContext* er_runtime_context(void)
{
    return s_ctx;
}

bool er_runtime_load_bytecode(const void* buf, size_t len)
{
    const bool ok = rt_report(er_bridge_run_bytecode(s_ctx, (const uint8_t*)buf, len));
    if (ok)
    {
        er_bridge_pump(s_ctx);
    }
    return ok;
}

/* --- ERCF config container ------------------------------------------------------------------------
 * Layout (little-endian): "ERCF" | format_version u32 | crc32 u32 (over everything after it) |
 *   qjs_tag (u16 len + bytes) | section_count u32 | sections[ type u32 | len u32 | bytes ]
 * Section types: 1 = QuickJS bytecode, 2 = ERPK asset pack. */

static uint16_t rd_le16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** @brief Standard CRC-32 (IEEE / zlib polynomial 0xEDB88320), branchless inner loop. */
static uint32_t crc32_bytes(const uint8_t* p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

const char* er_runtime_container_status_str(ErContainerStatus status)
{
    switch (status)
    {
        case ER_CONTAINER_OK:
            return "ok";
        case ER_CONTAINER_BAD_MAGIC:
            return "not an ERCF container";
        case ER_CONTAINER_BAD_VERSION:
            return "unsupported or malformed container";
        case ER_CONTAINER_BAD_CRC:
            return "CRC mismatch (corrupt or partially written)";
        case ER_CONTAINER_BAD_QJS:
            return "built for a different QuickJS version";
        case ER_CONTAINER_LOAD_FAILED:
            return "app load failed";
        default:
            return "unknown";
    }
}

ErContainerStatus er_runtime_load_container(const void* vbuf, size_t len)
{
    const uint8_t* buf = (const uint8_t*)vbuf;
    if (!buf || len < 12u || memcmp(buf, "ERCF", 4) != 0)
    {
        return ER_CONTAINER_BAD_MAGIC;
    }
    if (rd_le32(buf + 4) != 1u)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    if (rd_le32(buf + 8) != crc32_bytes(buf + 12, len - 12u))
    {
        return ER_CONTAINER_BAD_CRC;
    }

    size_t off = 12u;
    if (off + 2u > len)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    const uint16_t tlen = rd_le16(buf + off);
    off += 2u;
    if (off + tlen > len)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    if (tlen != strlen(ER_QUICKJS_TAG) || memcmp(buf + off, ER_QUICKJS_TAG, tlen) != 0)
    {
        return ER_CONTAINER_BAD_QJS;
    }
    off += tlen;
    if (off + 4u > len)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    const uint32_t nsec = rd_le32(buf + off);
    off += 4u;

    /* Register asset sections as we encounter them (so <Image>/<Text> resolve when the app mounts);
     * defer the bytecode so it runs last regardless of section order. */
    const uint8_t* bytecode = NULL;
    uint32_t bytecode_len = 0;
    for (uint32_t i = 0; i < nsec; i++)
    {
        if (off + 8u > len)
        {
            return ER_CONTAINER_BAD_VERSION;
        }
        const uint32_t type = rd_le32(buf + off);
        const uint32_t slen = rd_le32(buf + off + 4u);
        off += 8u;
        if (off + slen > len)
        {
            return ER_CONTAINER_BAD_VERSION;
        }
        const uint8_t* sdata = buf + off;
        off += slen;
        if (type == 2u)
        {
            er_assets_load_pack(sdata, slen);
        }
        else if (type == 1u)
        {
            bytecode = sdata;
            bytecode_len = slen;
        }
    }
    if (!bytecode)
    {
        return ER_CONTAINER_LOAD_FAILED;
    }
    return er_runtime_load_bytecode(bytecode, bytecode_len) ? ER_CONTAINER_OK : ER_CONTAINER_LOAD_FAILED;
}

bool er_runtime_load_source(const char* src, size_t len, const char* name)
{
    const bool ok = rt_report(JS_Eval(s_ctx, src, len, name ? name : "<app>", JS_EVAL_TYPE_GLOBAL));
    if (ok)
    {
        er_bridge_pump(s_ctx);
    }
    return ok;
}

void er_runtime_pump(void)
{
    if (s_ctx)
    {
        er_bridge_pump(s_ctx);
    }
}

bool er_runtime_reset(void)
{
    if (s_ctx)
    {
        JS_FreeContext(s_ctx);
        s_ctx = NULL;
    }
    er_reset();
    install_globals();
    return s_ctx != NULL;
}

const char* er_runtime_last_error(void)
{
    return s_last_error;
}

void er_runtime_show_error(void)
{
    if (!s_ctx)
    {
        return;
    }
    /* Clear the (possibly half-built) scene, publish the error text, and run the redbox app in the
     * current context. The caller's next commit paints it. */
    er_reset();
    JSValue global = JS_GetGlobalObject(s_ctx);
    JS_SetPropertyStr(s_ctx, global, "__error", JS_NewString(s_ctx, s_last_error));
    JS_FreeValue(s_ctx, global);
    JS_FreeValue(s_ctx, JS_Eval(s_ctx, k_error_app, strlen(k_error_app), "<error-overlay>", JS_EVAL_TYPE_GLOBAL));
    er_bridge_pump(s_ctx);
}

void er_runtime_clear_persist(void)
{
    for (int i = 0; i < s_persist_count; i++)
    {
        free(s_persist[i].key);
        free(s_persist[i].val);
        s_persist[i].key = NULL;
        s_persist[i].val = NULL;
    }
    s_persist_count = 0;
}

void er_runtime_shutdown(void)
{
    if (s_ctx)
    {
        JS_FreeContext(s_ctx);
        s_ctx = NULL;
    }
    if (s_rt)
    {
        JS_FreeRuntime(s_rt);
        s_rt = NULL;
    }
}
