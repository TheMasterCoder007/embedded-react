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
 * Owns the JSRuntime/JSContext lifecycle and the host globals (console, screen, __erNative, optional __erPersist),
 * and wraps load/pump/reset/error screen. No platform deps: the caller provides the backend, the app
 * bytes, and the frame loop. Shared by the desktop demo + simulator (examples/linux/host.c) and usable
 * directly from MCU firmware.
 */

#include "er_runtime.h"

#include "er_assets.h"        /* er_assets_load_pack (ERPK asset section of the container) */
#include "er_error_screen.h"  /* the error screen JS errors and load failures show */
#include "er_js_alloc.h"      /* default JS-heap allocator with working GC size accounting */
#include "er_scene.h"         /* er_reset */
#include "native_ui_bridge.h" /* er_bridge_install / _pump / _run_bytecode / _now_ms */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief QuickJS release this build is bytecode-compatible with — MUST match the FetchContent pin in
 *        bridges/quickjs/CMakeLists.txt and the tag the packager stamps into the container. A config
 *        built for a different QuickJS is rejected (ER_CONTAINER_BAD_QUICKJS) rather than run as garbage.
 */
#define ER_QUICKJS_TAG "v0.15.0"

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private (single-instance, like the engine + bridge)
 ---------------------------------------------------------------------------------------------------------------------*/

static JSRuntime* s_rt = NULL;
static JSContext* s_ctx = NULL;
static ErRuntimeConfig s_config;

/** @brief Bytes kept of the last uncaught JS error, and of its log line. */
#define LAST_ERROR_BYTES 512
#define EXCEPTION_LINE_BYTES 600

/** @brief Bytes in one line of a heap-accounting warning. */
#define WARNING_LINE_BYTES 160

/** @brief Last uncaught JS error (message + stack), for er_runtime_show_error / _last_error. */
static char s_last_error[LAST_ERROR_BYTES] = "";

/** @brief Result of the boot-time heap-accounting probe (see er_runtime_gc_accounting_ok). */
static bool s_gc_accounting_ok = true;

/** @brief QuickJS's own starting GC trigger, captured before any floor is applied, so that dropping the
 *         floor can hand the schedule back. Without it, a runtime left with the automatic collector off
 *         would never collect again — nothing would ever run to recompute the threshold. */
static size_t s_gc_threshold_default = 0;

/** @brief Added to the engine clock to make Date.now(): 0, so it reads as uptime, until the host calls
 *         er_runtime_set_wall_clock. Kept across er_runtime_reset, so the time survives a reload. */
static int64_t s_wall_offset_ms = 0;

/** @brief The error screen's title and hint for a JS error. */
#define JS_ERROR_TITLE "JS error"
#define JS_ERROR_HINT "fix the source and reload"

/** @brief Separates the message from the stack in s_last_error. */
#define STACK_SEPARATOR "\n\n"

/*----------------------------------------------------------------------------------------------------------------------
 - Console / logging
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Emits one log line to the configured sink (or stdout). */
static void emit_line(const char* line)
{
    if (s_config.log)
    {
        s_config.log(line);
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
    const int part_count = argc > 0 ? argc : 0;
    const char** parts = (const char**)calloc((size_t)(part_count > 0 ? part_count : 1), sizeof(char*));
    size_t total = 1; /* null terminator */
    for (int part_index = 0; part_index < part_count; part_index++)
    {
        parts[part_index] = JS_ToCString(ctx, argv[part_index]);
        total += (parts[part_index] ? strlen(parts[part_index]) : 0) + (part_index ? 1u : 0u);
    }
    char* line = (char*)malloc(total);
    if (line)
    {
        char* cursor = line;
        for (int part_index = 0; part_index < part_count; part_index++)
        {
            if (part_index)
            {
                *cursor++ = ' ';
            }
            if (parts[part_index])
            {
                const size_t part_length = strlen(parts[part_index]);
                memcpy(cursor, parts[part_index], part_length);
                cursor += part_length;
            }
        }
        *cursor = '\0';
        emit_line(line);
        free(line);
    }
    for (int part_index = 0; part_index < part_count; part_index++)
    {
        if (parts[part_index])
        {
            JS_FreeCString(ctx, parts[part_index]);
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

    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, s_config.screen_width));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, s_config.screen_height));
    JS_SetPropertyStr(ctx, screen, "scale", JS_NewFloat64(ctx, (double)s_config.screen_scale));
    JS_SetPropertyStr(ctx, global, "screen", screen);
    JS_FreeValue(ctx, global);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Persisted state (usePersistentState across reloads; opt-in via ErRuntimeConfig.install_persist)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum number of distinct usePersistentState keys retained across reloads. */
#define ER_PERSIST_MAX 64

/** @brief key → JSON-value store that outlives the recreated JS context. */
static struct
{
    char* key;
    char* value;
} s_persist[ER_PERSIST_MAX];
static int s_persist_count = 0;

/** @brief Heap-duplicates a C string; NULL on allocation failure. */
static char* persist_dup(const char* text)
{
    const size_t size = strlen(text) + 1;
    char* copy = (char*)malloc(size);
    if (copy)
    {
        memcpy(copy, text, size);
    }
    return copy;
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
    JSValue result = JS_UNDEFINED;
    for (int entry_index = 0; entry_index < s_persist_count; entry_index++)
    {
        if (s_persist[entry_index].value && strcmp(s_persist[entry_index].key, key) == 0)
        {
            result = JS_NewString(ctx, s_persist[entry_index].value);
            break;
        }
    }
    JS_FreeCString(ctx, key);
    return result;
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
    const char* value = JS_ToCString(ctx, argv[1]);
    if (key && value)
    {
        int entry_index = -1;
        for (int candidate = 0; candidate < s_persist_count; candidate++)
        {
            if (strcmp(s_persist[candidate].key, key) == 0)
            {
                entry_index = candidate;
                break;
            }
        }
        if (entry_index < 0 && s_persist_count < ER_PERSIST_MAX)
        {
            entry_index = s_persist_count++;
            s_persist[entry_index].key = persist_dup(key);
            s_persist[entry_index].value = NULL;
        }
        if (entry_index >= 0 && s_persist[entry_index].key)
        {
            free(s_persist[entry_index].value);
            s_persist[entry_index].value = persist_dup(value);
        }
    }
    if (key)
    {
        JS_FreeCString(ctx, key);
    }
    if (value)
    {
        JS_FreeCString(ctx, value);
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
 - Native modules (nativeModule() calls ErRuntimeConfig.natives through `__erNative`)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Native functions running now: a post from inside one is refused, as Flow B refuses one while the VM runs. */
static int s_native_calls = 0;

/** @brief An app has loaded into the current context. */
static bool s_app_loaded = false;

/** @brief Whether a signature is a result, ':', then up to ERB_NATIVE_MAX_PARAMETERS parameters of types the bridge
 * passes.
 */
static bool native_signature_valid(const char* signature)
{
    if (!signature || signature[0] == '\0' || !strchr("ibflsv", signature[0]) || signature[1] != ':')
    {
        return false;
    }
    const char* parameters = signature + 2;
    for (const char* code = parameters; *code; code++)
    {
        if (!strchr("ibfls", *code))
        {
            return false;
        }
    }
    return strlen(parameters) <= ERB_NATIVE_MAX_PARAMETERS;
}

/**
 * @brief __erNative.find(name, version, signature) → the native function's index, or -1 when the firmware has none;
 *        with `magic`, findEvent() and the native event's.
 */
static JSValue js_native_find(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    (void)this_val;
    const bool event = magic != 0;
    if (argc < 3)
    {
        return JS_ThrowTypeError(ctx, "find() takes a name, a version and a signature");
    }
    int32_t version = 0;
    if (JS_ToInt32(ctx, &version, argv[1]))
    {
        return JS_EXCEPTION;
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    const char* signature = JS_ToCString(ctx, argv[2]);
    int32_t found = -1;
    if (name && signature && native_signature_valid(signature) && (!event || signature[0] == 'v'))
    {
        for (uint16_t index = 0; index < s_config.native_count; index++)
        {
            const ErbNative* native = &s_config.natives[index];
            if ((native->callback == NULL) == event && native->name && native->signature && native->version == version
                && strcmp(native->name, name) == 0 && strcmp(native->signature, signature) == 0)
            {
                found = index;
                break;
            }
        }
    }
    const bool converted = name && signature;
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, signature);
    return converted ? JS_NewInt32(ctx, found) : JS_EXCEPTION;
}

/** @brief __erNative.call(index, ...arguments) → the native's result, its arguments converted by its signature. */
static JSValue js_native_call(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    int32_t index = -1;
    if (argc < 1 || JS_ToInt32(ctx, &index, argv[0]))
    {
        return JS_EXCEPTION;
    }
    if (index < 0 || index >= s_config.native_count || !s_config.natives[index].callback
        || !native_signature_valid(s_config.natives[index].signature))
    {
        return JS_ThrowRangeError(ctx, "no native function %d", (int)index);
    }
    const ErbNative* native = &s_config.natives[index];
    const char* parameters = native->signature + 2;
    const int parameter_count = (int)strlen(parameters);
    if (argc - 1 != parameter_count)
    {
        return JS_ThrowTypeError(ctx,
                                 "%s() takes %d argument%s, not %d",
                                 native->name ? native->name : "a native function",
                                 parameter_count,
                                 parameter_count == 1 ? "" : "s",
                                 argc - 1);
    }

    /* Each argument into its cells. A string is read in place, so its C string is kept until the call returns. */
    int32_t cells[ERB_NATIVE_MAX_CELLS] = {0};
    const char* texts[ERB_NATIVE_MAX_PARAMETERS] = {NULL};
    const uint32_t held = erb_native_texts_held();
    bool converted = true;
    int cell = 0;
    for (int parameter = 0; parameter < parameter_count && converted; parameter++)
    {
        JSValueConst argument = argv[parameter + 1];
        switch (parameters[parameter])
        {
            case 'i':
                converted = JS_ToInt32(ctx, &cells[cell++], argument) == 0;
                break;
            case 'b':
            {
                const int truth = JS_ToBool(ctx, argument);
                converted = truth >= 0;
                cells[cell++] = truth > 0;
                break;
            }
            case 'l':
            {
                int64_t whole = 0;
                converted = JS_ToInt64(ctx, &whole, argument) == 0;
                erb_native_set_int64(&cells[cell], whole);
                cell += 2;
                break;
            }
            case 's':
                texts[parameter] = JS_ToCString(ctx, argument);
                converted = texts[parameter] != NULL;
                cells[cell++] = erb_native_text(texts[parameter]);
                break;
            default:
            {
                /* A number goes through a C float, as in Flow B. */
                double number = 0.0;
                converted = JS_ToFloat64(ctx, &number, argument) == 0;
                const float single = (float)number;
                memcpy(&cells[cell++], &single, sizeof(single));
                break;
            }
        }
    }

    int32_t result[2] = {0, 0};
    bool succeeded = false;
    if (converted)
    {
        s_native_calls++;
        succeeded = native->callback(cells, result, s_config.native_context);
        s_native_calls--;
    }
    JSValue value = JS_UNDEFINED;
    if (converted && succeeded)
    {
        switch (native->signature[0])
        {
            case 'i':
                value = JS_NewInt32(ctx, result[0]);
                break;
            case 'b':
                value = JS_NewBool(ctx, result[0] != 0);
                break;
            case 'l':
                value = JS_NewInt64(ctx, erb_native_int64(result));
                break;
            case 's':
                value = JS_NewString(ctx, erb_native_string(result[0]));
                break;
            case 'f':
            {
                float single;
                memcpy(&single, &result[0], sizeof(single));
                value = JS_NewFloat64(ctx, (double)single);
                break;
            }
            default:
                break;
        }
    }
    erb_native_texts_release(held);
    for (int parameter = 0; parameter < parameter_count; parameter++)
    {
        JS_FreeCString(ctx, texts[parameter]);
    }
    if (!converted)
    {
        return JS_EXCEPTION;
    }
    if (!succeeded)
    {
        return JS_ThrowPlainError(ctx, "native module failed: %s", native->name ? native->name : "");
    }
    return value;
}

/** @brief Installs the `__erNative` global (find/findEvent/call) over ErRuntimeConfig.natives. */
static void install_native(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue native = JS_NewObject(ctx);
    JS_SetPropertyStr(
        ctx, native, "find", JS_NewCFunctionMagic(ctx, js_native_find, "find", 3, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(
        ctx, native, "findEvent", JS_NewCFunctionMagic(ctx, js_native_find, "findEvent", 3, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, native, "call", JS_NewCFunction(ctx, js_native_call, "call", 1));
    JS_SetPropertyStr(ctx, global, "__erNative", native);
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
    return JS_NewFloat64(ctx, (double)er_bridge_now_ms());
}

/**
 * @brief Installs a `performance` global with `now()` on the engine clock.
 *
 * React's scheduler reads `performance.now` when it exists and only falls back to `Date.now` without
 * it; this keeps the scheduler on the monotonic clock, which re-setting the wall clock cannot move.
 */
static void install_performance(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue performance = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, performance, "now", JS_NewCFunction(ctx, rt_perf_now, "now", 0));
    JS_SetPropertyStr(ctx, global, "performance", performance);
    JS_FreeValue(ctx, global);
}

/** @brief Date.now() without the Date intrinsic — the engine clock plus the host-fed wall-clock offset. */
static JSValue rt_date_now(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt64(ctx, s_wall_offset_ms + (int64_t)er_bridge_now_ms());
}

/** @brief `Date.length`, the argument count the constructor declares. */
#define DATE_CONSTRUCTOR_LENGTH 7

/** @brief `Date()` / `new Date()` without the Date intrinsic: throws, naming the flag that provides them. */
static JSValue rt_date_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Date objects need ER_JS_INTRINSIC_DATE; only Date.now() is built in");
}

/**
 * @brief Installs a stand-in `Date` whose only member is `now()`, for a context without the Date intrinsic.
 *
 * Without it `Date.now()` is a ReferenceError that kills whatever handler called it. Constructing a
 * date still throws, but says why, and `x instanceof Date` is false instead of a throw.
 */
static void install_date_now(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue date =
        JS_NewCFunction2(ctx, rt_date_ctor, "Date", DATE_CONSTRUCTOR_LENGTH, JS_CFUNC_constructor_or_func, 0);
    JSValue proto = JS_NewObject(ctx);
    JS_SetConstructor(ctx, date, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, date, "now", JS_NewCFunction(ctx, rt_date_now, "now", 0));
    JS_SetPropertyStr(ctx, global, "Date", date);
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
    if (!(extra_intrinsics & ER_JS_INTRINSIC_DATE))
    {
        install_date_now(ctx);
    }
    return ctx;
}

void er_runtime_set_wall_clock(int64_t epoch_ms)
{
    s_wall_offset_ms = epoch_ms - (int64_t)er_bridge_now_ms();
}

/** @brief Creates a fresh context and installs the bridge + host globals (used at init and reset). */
static void install_globals(void)
{
    /* ER_JS_INTRINSIC_EVAL is always requested: the C-level JS_Eval that backs
     * er_runtime_load_source (desktop demo, web sim) only works once the Eval intrinsic has installed
     * the context's eval hook. On a QJS_DISABLE_PARSER device build the hook doesn't exist and both
     * JS-level eval() and er_runtime_load_source fail gracefully — bytecode is the only load path. */
    s_app_loaded = false;
    s_ctx = er_js_new_context(s_rt, s_config.extra_intrinsics | ER_JS_INTRINSIC_EVAL);
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
    install_native(s_ctx);
    if (s_config.install_persist)
    {
        install_persist(s_ctx);
    }
    if (s_config.install_host_globals)
    {
        s_config.install_host_globals(s_ctx);
    }
}

/**
 * @brief Logs a JS exception (its message, then its stack) through the log sink and keeps it as the last error.
 *
 * @param[in] ctx        The context it was thrown in.
 * @param[in] exception  The thrown value; borrowed.
 * @param[in] where      What threw, such as "timer callback".
 */
static void record_exception(JSContext* ctx, JSValueConst exception, const char* where)
{
    const char* message = JS_ToCString(ctx, exception);
    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
    const char* stack_text = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);

    char headline[EXCEPTION_LINE_BYTES];
    snprintf(
        headline, sizeof(headline), "JS %s%sexception: %s", where, *where ? " " : "", message ? message : "(unknown)");
    emit_line(headline);
    if (stack_text)
    {
        emit_line(stack_text);
    }
    snprintf(s_last_error,
             sizeof(s_last_error),
             "%s%s%s",
             message ? message : "(unknown)",
             stack_text ? STACK_SEPARATOR : "",
             stack_text ? stack_text : "");

    if (message)
    {
        JS_FreeCString(ctx, message);
    }
    if (stack_text)
    {
        JS_FreeCString(ctx, stack_text);
    }
    JS_FreeValue(ctx, stack);
}

/** @brief A JS exception nothing caught, from the bridge: logged, kept, and shown on the error screen. */
static void rt_uncaught(JSContext* ctx, JSValueConst exception, const char* where)
{
    record_exception(ctx, exception, where);
    er_runtime_show_error();
}

/** @brief Reports an uncaught exception to the log sink + captures it; frees the eval result. */
static bool rt_report(JSValue result)
{
    bool ok = true;
    if (JS_IsException(result))
    {
        JSValue exception = JS_GetException(s_ctx);
        record_exception(s_ctx, exception, "");
        JS_FreeValue(s_ctx, exception);
        ok = false;
    }
    JS_FreeValue(s_ctx, result);
    return ok;
}

/**
 * @brief Verifies the new runtime can account for heap bytes, and warns loudly through the log sink if not.
 *
 * QuickJS runs its GC — and enforces ErRuntimeConfig.memory_limit — purely off
 * js_malloc_usable_size(). A host that supplies JSMallocFunctions whose usable_size returns 0 (or
 * reads the wrong heap's header) silently gets NO garbage collection and NO memory cap: JS garbage
 * accumulates until the heap is exhausted, which looks exactly like a leak in the app or the engine.
 * The bridge's own default allocator always reports correctly (er_js_alloc.h), so this only fires for
 * a host-supplied allocator — but that is precisely the case that used to fail undiagnosed for hours.
 */
static void check_gc_accounting(void)
{
    size_t reported = 0;
    const ErJsAllocHealth health = er_js_probe_alloc_health(s_rt, &reported);
    s_gc_accounting_ok = (health == ER_JS_ALLOC_OK);
    if (s_gc_accounting_ok)
    {
        return;
    }

    char line[WARNING_LINE_BYTES];
    emit_line("embedded-react: WARNING - the JS heap has no byte accounting.");
    snprintf(line, sizeof(line), "  js_malloc_usable_size() reported %zu bytes for a 1000-byte allocation.", reported);
    emit_line(line);
    emit_line("  Consequences: the garbage collector never runs (JS garbage grows until the heap is");
    emit_line("  exhausted) and ErRuntimeConfig.memory_limit cannot cap the heap.");
    emit_line("  Fix: give js_malloc_usable_size a real implementation for your heap (e.g."
              " tlsf_block_size / heap_caps_get_allocated_size), or leave malloc_functions NULL to use the"
              " bridge default.");
}

/**
 * @brief Applies ErRuntimeConfig.gc_threshold as a FLOOR under QuickJS's automatic-GC trigger.
 *
 * QuickJS resets the trigger to live x 1.5 after every collection, so a threshold set once at init is
 * gone the first time the collector runs. Re-asserting it (from er_runtime_pump, once a frame — two
 * cheap accessor calls) is what turns the config value into an actual schedule: collect at most once
 * per `gc_threshold` bytes of allocation, however small the live set happens to be.
 */
static void apply_gc_floor(void)
{
    if (s_rt && s_config.gc_threshold && JS_GetGCThreshold(s_rt) < s_config.gc_threshold)
    {
        JS_SetGCThreshold(s_rt, s_config.gc_threshold);
    }
}

/**
 * @brief Warns when the GC floor sits at or above the memory limit — the AUTOMATIC collector can then
 *        never fire before the cap, so a growing app throws out-of-memory unless the host collects
 *        manually (er_runtime_run_gc) before the heap reaches it. Deliberate in manual-GC mode (a
 *        SIZE_MAX floor), which is why this warns rather than fails.
 */
static void check_gc_threshold(void)
{
    if (!s_config.gc_threshold || !s_config.memory_limit || s_config.gc_threshold < s_config.memory_limit)
    {
        return;
    }
    char line[WARNING_LINE_BYTES];
    emit_line("embedded-react: WARNING - gc_threshold is not below memory_limit.");
    snprintf(line,
             sizeof(line),
             "  gc_threshold = %zu bytes, memory_limit = %zu bytes.",
             s_config.gc_threshold,
             s_config.memory_limit);
    emit_line(line);
    emit_line("  The heap reaches the limit before the AUTOMATIC collector is allowed to run, so a growing");
    emit_line("  app fails with a JS out-of-memory error unless er_runtime_run_gc() is called before the heap");
    emit_line("  gets there. If this floor is deliberate (manual-GC mode), make sure those calls happen;");
    emit_line("  otherwise leave headroom - a threshold of about half the limit is a reasonable start.");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_runtime_init(const ErRuntimeConfig* config)
{
    s_config = *config;
    if (s_config.screen_scale <= 0.0f)
    {
        s_config.screen_scale = 1.0f;
    }
    /* Back to the optimistic default before anything can fail: this outlives the runtime it describes,
       and a verdict from a PREVIOUS runtime must not survive into an init that never got to probe. */
    s_gc_accounting_ok = true;
    /* Never JS_NewRuntime(): its default allocator answers js_malloc_usable_size with 0 on any platform
       outside Apple/Win32/glibc (bare-metal newlib, Emscripten), which silently disables the GC and the
       memory limit. er_js_default_malloc_functions() always accounts for bytes — see er_js_alloc.h. */
    s_rt =
        JS_NewRuntime2(s_config.malloc_functions ? s_config.malloc_functions : er_js_default_malloc_functions(), NULL);
    if (!s_rt)
    {
        return false;
    }
    check_gc_accounting();
    if (s_config.max_stack_size)
    {
        JS_SetMaxStackSize(s_rt, s_config.max_stack_size);
    }
    if (s_config.memory_limit)
    {
        JS_SetMemoryLimit(s_rt, s_config.memory_limit);
    }
    s_gc_threshold_default = JS_GetGCThreshold(s_rt); /* before the floor, so it is QuickJS's own value */
    check_gc_threshold();
    apply_gc_floor();
    install_globals();
    er_bridge_set_uncaught_handler(rt_uncaught);
    return s_ctx != NULL;
}

JSContext* er_runtime_context(void)
{
    return s_ctx;
}

bool er_runtime_load_bytecode(const void* buffer, size_t length)
{
    const bool ok = rt_report(er_bridge_run_bytecode(s_ctx, (const uint8_t*)buffer, length));
    if (ok)
    {
        s_app_loaded = true;
        er_error_screen_hide();
        er_bridge_pump(s_ctx);
    }
    return ok;
}

uint8_t* er_runtime_compile_bytecode(const char* source, size_t length, size_t* out_length)
{
    /* Debug info kept: this entry point serves the dev loop (hot reload / sim), where stack-trace
     * line numbers matter more than blob size. Release packaging passes strip = true. */
    return er_runtime_compile_bytecode_ex(source, length, out_length, false);
}

uint8_t* er_runtime_compile_bytecode_ex(const char* source, size_t length, size_t* out_length, bool strip)
{
    if (out_length)
    {
        *out_length = 0;
    }
    /* A throwaway runtime — compiling must not touch the app runtime (or require it to exist). Same
       allocator as the app runtime: on-device compilation churns a lot of garbage, so it needs a live GC. */
    JSRuntime* rt = JS_NewRuntime2(er_js_default_malloc_functions(), NULL);
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
    JSValue compiled = JS_Eval(ctx, source, length, "<app>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    uint8_t* bytecode_copy = NULL;
    if (JS_IsException(compiled))
    {
        /* Print the compile error (message + stack) to stderr → the host's console. */
        JSValue exception = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, exception);
        fprintf(stderr, "er_runtime_compile_bytecode: %s\n", message ? message : "(unknown)");
        if (message)
        {
            JS_FreeCString(ctx, message);
        }
        JS_FreeValue(ctx, exception);
    }
    else
    {
        size_t bytecode_length = 0;
        const int write_flags =
            JS_WRITE_OBJ_BYTECODE | (strip ? (JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG) : 0);
        uint8_t* bytecode = JS_WriteObject(ctx, &bytecode_length, compiled, write_flags);
        if (bytecode)
        {
            bytecode_copy = (uint8_t*)malloc(bytecode_length);
            if (bytecode_copy)
            {
                memcpy(bytecode_copy, bytecode, bytecode_length);
                if (out_length)
                {
                    *out_length = bytecode_length;
                }
            }
            js_free(ctx, bytecode);
        }
    }
    JS_FreeValue(ctx, compiled);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return bytecode_copy;
}

void er_runtime_free(void* buffer)
{
    free(buffer);
}

/* --- ERCF config container ------------------------------------------------------------------------
 * The reader (er_container.c) checks the structure and the CRC; this half checks the tag against the
 * QuickJS this firmware links and runs the sections. */

const char* er_runtime_container_status_name(ErContainerStatus status)
{
    return er_container_status_name(status);
}

bool er_runtime_container_has_vendor(const void* buffer, size_t length)
{
    /* A structural-only scan (no CRC) for a vendor section: present → the container is a full boot/reload
     * that carries the framework; absent → an app-only frame for an incremental soft reload. A malformed
     * blob returns false; the loader rejects it for real afterward. */
    ErContainer container;
    if (er_container_scan(buffer, length, &container) != ER_CONTAINER_OK)
    {
        return false;
    }
    for (uint32_t section_index = 0; section_index < container.section_count; section_index++)
    {
        if (container.sections[section_index].type == ER_SECTION_VENDOR_BYTECODE)
        {
            return true;
        }
    }
    return false;
}

/** @brief True when the engine would register all of a container's packs: its registries drop what doesn't fit. */
static bool asset_packs_fit(const ErContainer* container, bool copy_assets)
{
    ErAssetsInfo info;
    if (!er_assets_check_container(container, &info))
    {
        return false;
    }
    ERPoolLimits limits;
    er_pool_limits(&limits);
    /* A copy is word-aligned, and a v2 pack aligns its pixels from its own first byte. */
    const bool odd_pixels = !copy_assets && info.odd_rgb565 != 0u;
    /* The registry doesn't store empty images, so they take no slot. */
    return !odd_pixels && info.images - info.empty_images <= limits.images && info.font_families <= limits.font_families
           && info.largest_family <= limits.font_sizes;
}

/** @brief The container's last section of @p type, or NULL. */
static const ErContainerSection* find_section(const ErContainer* container, uint32_t type)
{
    const ErContainerSection* found = NULL;
    for (uint32_t section_index = 0; section_index < container->section_count; section_index++)
    {
        if (container->sections[section_index].type == type)
        {
            found = &container->sections[section_index];
        }
    }
    return found;
}

/** @brief Every check a container load makes before it touches the engine. */
static ErContainerStatus check_container(const void* buffer, size_t length, bool copy_assets, ErContainer* container)
{
    /* Integrity, then version compatibility, then the asset packs, together. */
    const ErContainerStatus status = er_container_read(buffer, length, container);
    if (status != ER_CONTAINER_OK)
    {
        return status;
    }
    if (container->tag_length != strlen(ER_QUICKJS_TAG)
        || memcmp(container->tag, ER_QUICKJS_TAG, container->tag_length) != 0)
    {
        return ER_CONTAINER_BAD_QUICKJS;
    }
    if (!asset_packs_fit(container, copy_assets))
    {
        return ER_CONTAINER_BAD_ASSETS;
    }
    return find_section(container, ER_SECTION_BYTECODE) ? ER_CONTAINER_OK : ER_CONTAINER_LOAD_FAILED;
}

ErContainerStatus er_runtime_load_container(const void* buffer, size_t length)
{
    return er_runtime_load_container_ex(buffer, length, false);
}

/** @brief Loads a container check_container passed: registers its asset packs, then runs its bytecode. */
static ErContainerStatus load_checked_container(const ErContainer* container, bool copy_assets)
{
    const ErContainerSection* vendor = find_section(container, ER_SECTION_VENDOR_BYTECODE);
    const ErContainerSection* bytecode = find_section(container, ER_SECTION_BYTECODE);

    /* Register the asset packs first (so <Image>/<Text> resolve when the app mounts). This can still fail
     * when memory runs out. Then run the vendor chunk if present — it installs the module registry +
     * require() shim the app resolves its imports through — and finally the app last, regardless of section
     * order. A container with no vendor section is a plain monolithic app and loads exactly as before. */
    for (uint32_t section_index = 0; section_index < container->section_count; section_index++)
    {
        const ErContainerSection* section = &container->sections[section_index];
        if (section->type == ER_SECTION_ASSET_PACK
            && !er_assets_load_pack_ex(section->data, section->length, copy_assets))
        {
            return ER_CONTAINER_BAD_ASSETS;
        }
    }
    if (vendor && !er_runtime_load_bytecode(vendor->data, vendor->length))
    {
        return ER_CONTAINER_LOAD_FAILED;
    }
    return er_runtime_load_bytecode(bytecode->data, bytecode->length) ? ER_CONTAINER_OK : ER_CONTAINER_LOAD_FAILED;
}

ErContainerStatus er_runtime_load_container_ex(const void* buffer, size_t length, bool copy_assets)
{
    /* Every check runs before anything is registered, so a container that fails one leaves the engine as it
     * was. */
    ErContainer container;
    const ErContainerStatus status = check_container(buffer, length, copy_assets, &container);
    return status == ER_CONTAINER_OK ? load_checked_container(&container, copy_assets) : status;
}

ErContainerStatus er_hotreload_apply(const void* erpkg, size_t erpkg_length)
{
    /* Two reload modes, chosen by what the container carries:
     *   - With a vendor section (a full boot/reload): tear the context down to a clean slate first, then
     *     load — the vendor runs, then the app.
     *   - App-only frame (the incremental case): DON'T reset. Keep the resident vendor + context, so
     *     react/reconciler stay parsed and __erPersist survives; loading just re-evaluates the app, which
     *     re-renders into the resident root (a "soft" reload).
     * Either way assets are copied (copy_assets=true) since @p erpkg is a staging buffer the receiver
     * reuses for the next upload — that copy lets the old UI stay on screen until the new one mounts. The
     * container's checks run before the reset, so one that fails them keeps the resident context (the vendor
     * and persisted state). On failure, paint the on-screen panel. */
    ErContainer container;
    ErContainerStatus status = check_container(erpkg, erpkg_length, true, &container);
    if (status == ER_CONTAINER_OK)
    {
        if (find_section(&container, ER_SECTION_VENDOR_BYTECODE))
        {
            er_runtime_reset();
        }
        status = load_checked_container(&container, true);
    }
    if (status != ER_CONTAINER_OK)
    {
        er_runtime_show_message(
            "Hot reload failed", er_container_status_name(status), "Fix the app and save again to re-send.");
    }
    return status;
}

bool er_runtime_load_source(const char* source, size_t length, const char* name)
{
    const bool ok = rt_report(JS_Eval(s_ctx, source, length, name ? name : "<app>", JS_EVAL_TYPE_GLOBAL));
    if (ok)
    {
        s_app_loaded = true;
        er_error_screen_hide();
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
    apply_gc_floor();
}

/** @brief Posts an event, once the caller has checked the runtime can take it. */
static bool post_native(int native, const int32_t* arguments)
{
    if (!s_ctx || !s_app_loaded || native < 0 || native >= s_config.native_count)
    {
        return false;
    }
    const ErbNative* event = &s_config.natives[native];
    if (event->callback || !native_signature_valid(event->signature) || event->signature[0] != 'v')
    {
        return false;
    }
    const char* codes = event->signature + 2;
    const uint32_t params = (uint32_t)strlen(codes);
    if (params && !arguments)
    {
        return false;
    }

    /* nativeModule() installs the dispatcher when the app declares a module; with none, nothing subscribes. */
    JSValue global = JS_GetGlobalObject(s_ctx);
    JSValue dispatcher = JS_GetPropertyStr(s_ctx, global, "__erNativeEvent");
    JS_FreeValue(s_ctx, global);
    if (JS_IsFunction(s_ctx, dispatcher))
    {
        /* The arguments as the app reads them, converted by the signature as a native's result is. */
        JSValue values = JS_NewArray(s_ctx);
        uint32_t cell = 0;
        for (uint32_t param = 0; param < params; param++)
        {
            JSValue value;
            switch (codes[param])
            {
                case 'i':
                    value = JS_NewInt32(s_ctx, arguments[cell++]);
                    break;
                case 'b':
                    value = JS_NewBool(s_ctx, arguments[cell++] != 0);
                    break;
                case 'l':
                    value = JS_NewInt64(s_ctx, erb_native_int64(&arguments[cell]));
                    cell += 2;
                    break;
                case 's':
                    value = JS_NewString(s_ctx, erb_native_string(arguments[cell++]));
                    break;
                default:
                {
                    float single;
                    memcpy(&single, &arguments[cell++], sizeof(single));
                    value = JS_NewFloat64(s_ctx, (double)single);
                    break;
                }
            }
            JS_SetPropertyUint32(s_ctx, values, param, value);
        }
        JSValue call_arguments[2] = {JS_NewInt32(s_ctx, native), values};
        er_bridge_dispatch(s_ctx, dispatcher, 2, call_arguments, "native event");
        JS_FreeValue(s_ctx, call_arguments[0]);
        JS_FreeValue(s_ctx, values);
    }
    JS_FreeValue(s_ctx, dispatcher);
    return true;
}

bool er_runtime_native_post(int native, const int32_t* arguments)
{
    /* From inside a native call, the call's strings are still held; it lets go of the post's with its own. */
    if (s_native_calls > 0)
    {
        return false;
    }
    const bool posted = post_native(native, arguments);
    erb_native_texts_release(0);
    return posted;
}

bool er_runtime_reset(void)
{
    if (s_ctx)
    {
        JS_FreeContext(s_ctx);
        s_ctx = NULL;
    }
    er_error_screen_hide();
    er_reset();
    install_globals();
    return s_ctx != NULL;
}

bool er_runtime_gc_accounting_ok(void)
{
    return s_gc_accounting_ok;
}

size_t er_runtime_gc_threshold(void)
{
    return s_rt ? JS_GetGCThreshold(s_rt) : 0u;
}

void er_runtime_set_gc_threshold(size_t bytes)
{
    s_config.gc_threshold = bytes;
    check_gc_threshold();
    if (s_rt)
    {
        JS_SetGCThreshold(s_rt, bytes ? bytes : s_gc_threshold_default);
    }
}

void er_runtime_run_gc(void)
{
    if (s_rt)
    {
        JS_RunGC(s_rt);
        apply_gc_floor();
    }
}

const char* er_runtime_last_error(void)
{
    return s_last_error;
}

void er_runtime_show_message(const char* title, const char* body, const char* hint)
{
    const ERErrorScreen screen = {
        .title = title,
        .reason = body,
        .hint = hint,
    };
    er_error_screen_show(s_config.screen_width, s_config.screen_height, &screen);
}

void er_runtime_show_error(void)
{
    /* The message on its own line, the stack as the detail. */
    char message[ERUI_ERROR_SCREEN_LINE_BYTES];
    const char* stack = strstr(s_last_error, STACK_SEPARATOR);
    size_t message_length = stack ? (size_t)(stack - s_last_error) : strlen(s_last_error);
    if (message_length >= sizeof(message))
    {
        message_length = sizeof(message) - 1u;
    }
    memcpy(message, s_last_error, message_length);
    message[message_length] = '\0';
    const ERErrorScreen screen = {
        .title = JS_ERROR_TITLE,
        .reason = message,
        .hint = JS_ERROR_HINT,
        .detail = stack ? stack + strlen(STACK_SEPARATOR) : NULL,
    };
    er_error_screen_show(s_config.screen_width, s_config.screen_height, &screen);
}

void er_runtime_clear_persist(void)
{
    for (int entry_index = 0; entry_index < s_persist_count; entry_index++)
    {
        free(s_persist[entry_index].key);
        free(s_persist[entry_index].value);
        s_persist[entry_index].key = NULL;
        s_persist[entry_index].value = NULL;
    }
    s_persist_count = 0;
}

void er_runtime_shutdown(void)
{
    er_bridge_set_uncaught_handler(NULL);
    if (s_ctx)
    {
        JS_FreeContext(s_ctx);
        s_ctx = NULL;
    }
    if (s_rt)
    {
        /* Before the runtime goes: the bridge caches state against it, and the next JSRuntime can be
         * handed the same address (see er_bridge_release_runtime). */
        er_bridge_release_runtime();
        JS_FreeRuntime(s_rt);
        s_rt = NULL;
    }
}
