/*
 * Headless runtime-test harness for the QuickJS bridge.
 *
 * Loads a bundled runtime test (produced by esbuild), installs the same globals the device host
 * provides — NativeUI (the bridge), a `screen` object, and console — plus a no-op render backend
 * so the engine's font/layout/paint path runs without a window. After evaluating the bundle it
 * exits non-zero if the script threw or recorded any failures in globalThis.__runtime_failed.
 *
 * Usage: er-bridge-quickjs-runtest <bundle.js>
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "native_ui_bridge.h"
#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define RT_SCREEN_W 480
#define RT_SCREEN_H 320

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — no-op backend + shims
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief No-op fill. @param argb c. @param x x. @param y y. @param w w. @param h h. @param ctx c. */
static void noop_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/** @brief No-op copy. @param s src. @param st stride. @param x x. @param y y. @param w w. @param h h. @param c ctx. */
static void noop_copy(const void* s, int st, int x, int y, int w, int h, void* c)
{
    (void)s;
    (void)st;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
}

/** @brief No-op blend. @param s src. @param st stride. @param a alpha. @param x x. @param y y. @param w w. @param h h.
 * @param c ctx. */
static void noop_blend(const void* s, int st, uint8_t a, int x, int y, int w, int h, void* c)
{
    (void)s;
    (void)st;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
}

/**
 * @brief console.log/warn/error implementation printing space-separated args to stdout.
 *
 * @param[in] ctx       QuickJS context.
 * @param[in] this_val  JS this (unused).
 * @param[in] argc      Argument count.
 * @param[in] argv      Argument values.
 *
 * @return JS_UNDEFINED, or JS_EXCEPTION if an argument cannot be stringified.
 */
static JSValue rt_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++)
    {
        const char* str = JS_ToCString(ctx, argv[i]);
        if (!str)
        {
            return JS_EXCEPTION;
        }
        printf("%s%s", i ? " " : "", str);
        JS_FreeCString(ctx, str);
    }
    printf("\n");
    return JS_UNDEFINED;
}

/**
 * @brief Installs console (log/warn/error) and a `screen` global onto the context.
 *
 * @param[in] ctx  QuickJS context.
 */
static void rt_install_globals(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue console = JS_NewObject(ctx);
    JSValue log = JS_NewCFunction(ctx, rt_console_log, "log", 1);
    JS_SetPropertyStr(ctx, console, "log", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "warn", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "error", log);
    JS_SetPropertyStr(ctx, global, "console", console);

    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, RT_SCREEN_W));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, RT_SCREEN_H));
    JS_SetPropertyStr(ctx, screen, "scale", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, global, "screen", screen);

    JS_FreeValue(ctx, global);
}

/**
 * @brief Reads a whole file into a newly allocated, null-terminated buffer.
 *
 * @param[in]  path     File path.
 * @param[out] out_len  Receives the byte length (excluding the appended terminator).
 *
 * @return Heap buffer the caller must free(), or NULL on failure.
 */
static char* read_file(const char* path, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    const size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/**
 * @brief Returns true when a path ends in ".qbc" (a compiled bytecode blob).
 *
 * @param[in] path  File path.
 *
 * @return true for a bytecode path, false for JS source.
 */
static bool is_bytecode_path(const char* path)
{
    const size_t n = strlen(path);
    return n >= 4 && strcmp(path + n - 4, ".qbc") == 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Runs a bundled runtime test under the QuickJS bridge and engine.
 *
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[1] = path to the bundled test JS.
 *
 * @return 0 when the test evaluates cleanly with no recorded failures; non-zero otherwise.
 */
int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <bundle.js>\n", argv[0]);
        return 2;
    }

    size_t src_len = 0;
    char* src = read_file(argv[1], &src_len);
    if (!src)
    {
        fprintf(stderr, "could not read '%s'\n", argv[1]);
        return 2;
    }
    const bool bytecode = is_bytecode_path(argv[1]);

    static const EmbeddedRenderBackend backend = {noop_fill, noop_copy, noop_blend, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    rt_install_globals(ctx);
    er_bridge_install(ctx);

    int status = 0;
    JSValue result = bytecode ? er_bridge_run_bytecode(ctx, (const uint8_t*)src, src_len)
                              : JS_Eval(ctx, src, strlen(src), argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
    {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "JS exception: %s\n", msg ? msg : "(unknown)");
        if (msg)
        {
            JS_FreeCString(ctx, msg);
        }
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack))
        {
            const char* st = JS_ToCString(ctx, stack);
            if (st)
            {
                fprintf(stderr, "%s\n", st);
                JS_FreeCString(ctx, st);
            }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exc);
        status = 1;
    }
    JS_FreeValue(ctx, result);

    /* Drain any Promise jobs the test queued at top level so async assertions are recorded
       before we read the failure counter. Timer-based tests advance the clock via NativeUI.tick,
       which pumps on its own. */
    if (status == 0)
    {
        er_bridge_pump(ctx);
    }

    /* Read globalThis.__runtime_failed (the harness.js failure counter). */
    if (status == 0)
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue failed = JS_GetPropertyStr(ctx, global, "__runtime_failed");
        int32_t n = 0;
        JS_ToInt32(ctx, &n, failed);
        JS_FreeValue(ctx, failed);
        JS_FreeValue(ctx, global);
        if (n > 0)
        {
            status = 1;
        }
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(src);
    return status;
}
