/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * er_runtime — portable QuickJS host core for Flow A (see er_runtime.h).
 *
 * Owns the JSRuntime/JSContext lifecycle and the host globals (console, screen, optional __erPersist),
 * and wraps load/pump/reset/error-overlay. No platform deps: the caller provides the backend, the app
 * bytes, and the frame loop. Shared by the desktop demo + simulator (examples/linux/host.c) and usable
 * directly from MCU firmware.
 */

#include "er_runtime.h"

#include "er_assets.h"                 /* er_assets_load_pack (ERPK asset section of the container) */
#include "er_scene.h"                  /* er_reset, er_now_ms */
#include "native_ui_bridge.h"          /* er_bridge_install / er_bridge_pump / er_bridge_run_bytecode */
#include "overlay/message_overlay.qbc.h" /* precompiled error-overlay app (works on parser-less builds) */

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

/* The message-overlay app (RN redbox) ships as precompiled bytecode — see overlay/message_overlay.js
 * for the source, what it renders, and how to regenerate message_overlay.qbc.c. */

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

/** @brief performance.now() — the engine's monotonic millisecond clock (React's scheduler clock). */
static JSValue rt_perf_now(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, (double)er_now_ms());
}

/**
 * @brief Installs a `performance` global with `now()` on the engine clock.
 *
 * Required by the lite profile: without a `performance` global, React's scheduler falls back to
 * `Date.now` at module load and would throw on a context without the Date intrinsic.
 */
static void install_performance(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now", JS_NewCFunction(ctx, rt_perf_now, "now", 0));
    JS_SetPropertyStr(ctx, global, "performance", perf);
    JS_FreeValue(ctx, global);
}

JSContext* er_js_new_context(JSRuntime* rt, uint32_t extra_intrinsics)
{
    JSContext* ctx = JS_NewContextRaw(rt);
    if (!ctx)
    {
        return NULL;
    }
    /* The lite profile: exactly what the React runtime bundle needs (verified against the shipped
     * bundle — RegExp for color/path parsing, Promise for the scheduler's microtask channel, Map/Set
     * for fibers, JSON for usePersistentState). Everything else is opt-in via extra_intrinsics so an
     * unused feature never costs device flash (the linker drops what is never referenced). */
    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicPromise(ctx);
    if (extra_intrinsics & ER_JS_INTRINSIC_DATE)
    {
        JS_AddIntrinsicDate(ctx);
    }
    if (extra_intrinsics & ER_JS_INTRINSIC_PROXY)
    {
        JS_AddIntrinsicProxy(ctx);
    }
    if (extra_intrinsics & ER_JS_INTRINSIC_TYPED_ARRAYS)
    {
        JS_AddIntrinsicTypedArrays(ctx);
    }
    if (extra_intrinsics & ER_JS_INTRINSIC_WEAK_REF)
    {
        JS_AddIntrinsicWeakRef(ctx);
    }
    if (extra_intrinsics & ER_JS_INTRINSIC_BIGINT)
    {
        JS_AddIntrinsicBigInt(ctx);
    }
    if (extra_intrinsics & ER_JS_INTRINSIC_EVAL)
    {
        /* On a QJS_DISABLE_PARSER build this installs an eval() that throws "not supported". */
        JS_AddIntrinsicEval(ctx);
    }
    install_performance(ctx);
    return ctx;
}

/** @brief Creates a fresh context and installs the bridge + host globals (used at init and reset). */
static void install_globals(void)
{
    /* ER_JS_INTRINSIC_EVAL is always requested: the C-level JS_Eval that backs
     * er_runtime_load_source (desktop demo, web sim) only works once the Eval intrinsic has installed
     * the context's eval hook. On a QJS_DISABLE_PARSER device build the hook doesn't exist and both
     * JS-level eval() and er_runtime_load_source fail gracefully — bytecode is the only load path. */
    s_ctx = er_js_new_context(s_rt, s_cfg.extra_intrinsics | ER_JS_INTRINSIC_EVAL);
    if (!s_ctx)
    {
        return;
    }
    /* Record the calling task's stack base for the max-stack-size overflow guard. Must run on the task
     * that will execute JS (the host's frame-loop task) — er_runtime_init/reset are called from it. */
    JS_UpdateStackTop(s_rt);
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
    s_rt = s_cfg.malloc_functions ? JS_NewRuntime2(s_cfg.malloc_functions, NULL) : JS_NewRuntime();
    if (!s_rt)
    {
        return false;
    }
    if (s_cfg.max_stack_size)
    {
        JS_SetMaxStackSize(s_rt, s_cfg.max_stack_size);
    }
    if (s_cfg.memory_limit)
    {
        JS_SetMemoryLimit(s_rt, s_cfg.memory_limit);
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

uint8_t* er_runtime_compile_bytecode(const char* src, size_t len, size_t* out_len)
{
    /* Debug info kept: this entry point serves the dev loop (hot reload / sim), where stack-trace
     * line numbers matter more than blob size. Release packaging passes strip = true. */
    return er_runtime_compile_bytecode_ex(src, len, out_len, false);
}

uint8_t* er_runtime_compile_bytecode_ex(const char* src, size_t len, size_t* out_len, bool strip)
{
    if (out_len)
    {
        *out_len = 0;
    }
    /* A throwaway runtime — compiling must not touch the app runtime (or require it to exist). */
    JSRuntime* rt = JS_NewRuntime();
    if (!rt)
    {
        return NULL;
    }
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx)
    {
        JS_FreeRuntime(rt);
        return NULL;
    }

    /* COMPILE_ONLY returns the program's function bytecode without running it, so the bundle's
       references to NativeUI/screen/console are left unresolved (resolved at load time). */
    JSValue obj = JS_Eval(ctx, src, len, "<app>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    uint8_t* out = NULL;
    if (JS_IsException(obj))
    {
        /* Print the compile error (message + stack) to stderr → the host's console. */
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "er_runtime_compile_bytecode: %s\n", msg ? msg : "(unknown)");
        if (msg)
        {
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
    }
    else
    {
        size_t bc_len = 0;
        const int wr_flags =
            JS_WRITE_OBJ_BYTECODE | (strip ? (JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG) : 0);
        uint8_t* bc = JS_WriteObject(ctx, &bc_len, obj, wr_flags);
        if (bc)
        {
            out = (uint8_t*)malloc(bc_len);
            if (out)
            {
                memcpy(out, bc, bc_len);
                if (out_len)
                {
                    *out_len = bc_len;
                }
            }
            js_free(ctx, bc);
        }
    }
    JS_FreeValue(ctx, obj);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return out;
}

void er_runtime_free(void* p)
{
    free(p);
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

/** @brief Most sections a single container may hold (we use up to 3: app bytecode + asset pack + an
 * optional vendor bytecode chunk for the incremental-hot-reload split). */
#define ER_CONTAINER_MAX_SECTIONS 8u

bool er_runtime_container_has_vendor(const void* vbuf, size_t len)
{
    /* A structural-only scan (no CRC) for a SECTION_VENDOR_BYTECODE (type 3): present → the container is a
     * full boot/reload that carries the framework; absent → an app-only frame for an incremental soft
     * reload. The caller uses this to decide whether to reset before loading. Every field is bounds-checked
     * against @p len, so a malformed blob simply returns false (the loader rejects it for real afterward). */
    const uint8_t* buf = (const uint8_t*)vbuf;
    if (!buf || len < 12u || memcmp(buf, "ERCF", 4) != 0)
    {
        return false;
    }
    size_t off = 12u;
    if (off + 2u > len)
    {
        return false;
    }
    const uint16_t tlen = rd_le16(buf + off);
    off += 2u;
    if (off + (size_t)tlen + 4u > len)
    {
        return false;
    }
    off += tlen;
    const uint32_t nsec = rd_le32(buf + off);
    off += 4u;
    if (nsec > ER_CONTAINER_MAX_SECTIONS)
    {
        return false;
    }
    for (uint32_t i = 0; i < nsec; i++)
    {
        if (off + 8u > len)
        {
            return false;
        }
        const uint32_t type = rd_le32(buf + off);
        const uint32_t slen = rd_le32(buf + off + 4u);
        off += 8u;
        if (off + slen > len)
        {
            return false;
        }
        if (type == 3u) /* SECTION_VENDOR_BYTECODE */
        {
            return true;
        }
        off += slen;
    }
    return false;
}

ErContainerStatus er_runtime_load_container(const void* vbuf, size_t len)
{
    return er_runtime_load_container_ex(vbuf, len, false);
}

ErContainerStatus er_runtime_load_container_ex(const void* vbuf, size_t len, bool copy_assets)
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
    const uint32_t stored_crc = rd_le32(buf + 8);

    /* Walk the structure FIRST, bounds-checking every field against `len`, to learn the exact payload
     * end. This lets `len` be an upper bound — e.g. the full size of a flash/config partition the
     * container was written into — rather than the exact byte count. The CRC is then taken over only
     * the true payload, so trailing partition bytes don't corrupt the check. The walk reads unverified
     * length fields, but each is bounds-checked, so a malformed blob is rejected, never over-read. */
    size_t off = 12u;
    if (off + 2u > len)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    const uint16_t tlen = rd_le16(buf + off);
    off += 2u;
    if (off + (size_t)tlen + 4u > len)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    const uint8_t* tag = buf + off;
    off += tlen;
    const uint32_t nsec = rd_le32(buf + off);
    off += 4u;
    if (nsec > ER_CONTAINER_MAX_SECTIONS)
    {
        return ER_CONTAINER_BAD_VERSION;
    }

    struct
    {
        uint32_t type;
        const uint8_t* data;
        uint32_t len;
    } secs[ER_CONTAINER_MAX_SECTIONS];
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
        secs[i].type = type;
        secs[i].data = buf + off;
        secs[i].len = slen;
        off += slen;
    }
    const size_t payload_end = off; /* exact end of the container within the (possibly larger) buffer */

    /* Integrity, then version compatibility — both before touching the engine. */
    if (stored_crc != crc32_bytes(buf + 12, payload_end - 12u))
    {
        return ER_CONTAINER_BAD_CRC;
    }
    if (tlen != strlen(ER_QUICKJS_TAG) || memcmp(tag, ER_QUICKJS_TAG, tlen) != 0)
    {
        return ER_CONTAINER_BAD_QJS;
    }

    /* Verified: register asset packs first (so <Image>/<Text> resolve when the app mounts). Then run the
     * vendor chunk (type 3) if present — it installs the module registry + require() shim the app resolves
     * its imports through — and finally the app (type 1) last, regardless of section order. A container
     * with no vendor section is a plain monolithic app and loads exactly as before. */
    const uint8_t* vendor = NULL;
    uint32_t vendor_len = 0;
    const uint8_t* bytecode = NULL;
    uint32_t bytecode_len = 0;
    for (uint32_t i = 0; i < nsec; i++)
    {
        if (secs[i].type == 2u) /* SECTION_ASSET_PACK */
        {
            er_assets_load_pack_ex(secs[i].data, secs[i].len, copy_assets);
        }
        else if (secs[i].type == 1u) /* SECTION_BYTECODE (app) */
        {
            bytecode = secs[i].data;
            bytecode_len = secs[i].len;
        }
        else if (secs[i].type == 3u) /* SECTION_VENDOR_BYTECODE */
        {
            vendor = secs[i].data;
            vendor_len = secs[i].len;
        }
    }
    if (!bytecode)
    {
        return ER_CONTAINER_LOAD_FAILED;
    }
    if (vendor && !er_runtime_load_bytecode(vendor, vendor_len))
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

void er_runtime_show_message(const char* title, const char* body, const char* hint)
{
    if (!s_ctx)
    {
        return;
    }
    /* Clear the (possibly half-built) scene, publish the strings, and run the overlay app (precompiled
     * bytecode, so this works on parser-less builds) in the current context. The caller's next commit
     * paints it. */
    er_reset();
    JSValue global = JS_GetGlobalObject(s_ctx);
    JS_SetPropertyStr(s_ctx, global, "__title", JS_NewString(s_ctx, title ? title : "Error"));
    JS_SetPropertyStr(s_ctx, global, "__error", JS_NewString(s_ctx, body ? body : ""));
    JS_SetPropertyStr(s_ctx, global, "__hint", JS_NewString(s_ctx, hint ? hint : ""));
    JS_FreeValue(s_ctx, global);
    JSValue r = er_bridge_run_bytecode(s_ctx, er_overlay_qbc, er_overlay_qbc_len);
    if (JS_IsException(r))
    {
        /* The overlay itself failed (e.g. a bytecode/engine version mismatch) — swallow the exception
         * so the caller's frame loop keeps running; the log still carries the original error. */
        JS_FreeValue(s_ctx, JS_GetException(s_ctx));
        emit_line("er_runtime_show_message: overlay bytecode failed to run (stale message_overlay.qbc.c?)");
    }
    JS_FreeValue(s_ctx, r);
    er_bridge_pump(s_ctx);
}

void er_runtime_show_error(void)
{
    er_runtime_show_message("JS error", s_last_error, "fix the source and reload");
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
