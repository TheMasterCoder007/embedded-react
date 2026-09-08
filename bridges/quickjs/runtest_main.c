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
 * Headless runtime-test harness for the QuickJS bridge.
 *
 * Loads a bundled runtime test (produced by esbuild), installs the same globals the device host
 * provides — NativeUI (the bridge), a `screen` object, and console — plus a render backend that
 * paints into a host framebuffer, so the engine's font/layout/paint path runs without a window and
 * a test can read back what was drawn through __pixel(). After evaluating the bundle it
 * exits non-zero if the script threw or recorded any failures in globalThis.__runtime_failed.
 *
 * Usage: er-bridge-quickjs-runtest <bundle.js>
 */

#include "er_runtime.h" /* er_js_new_context — the device's lite intrinsic profile */
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
 - Functions: Private — framebuffer backend + shims
 ---------------------------------------------------------------------------------------------------------------------*/

/* The backend paints into a plain host framebuffer rather than dropping the output, so a test can
 * read a pixel back through __pixel() and assert what actually got drawn. A stubbed blend_rect would
 * discard every anti-aliased span, which is most of what the vector and text paths emit. */
static uint32_t s_fb[RT_SCREEN_W * RT_SCREEN_H];

/** @brief Rounds (v / 255) the way the engine's own blend does. */
static uint32_t rt_div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

/** @brief Source-over of one premultiplied pixel onto the opaque framebuffer. */
static void rt_over(uint32_t* d, uint32_t pa, uint32_t pr, uint32_t pg, uint32_t pb)
{
    if (pa == 0U)
    {
        return;
    }
    if (pa >= 255U)
    {
        *d = 0xFF000000U | (pr << 16) | (pg << 8) | pb;
        return;
    }
    const uint32_t inv = 255U - pa;
    const uint32_t r = pr + rt_div255(((*d >> 16) & 0xFFU) * inv);
    const uint32_t g = pg + rt_div255(((*d >> 8) & 0xFFU) * inv);
    const uint32_t b = pb + rt_div255((*d & 0xFFU) * inv);
    *d = 0xFF000000U | (r << 16) | (g << 8) | b;
}

/** @brief fill_rect. @param argb color. @param x x. @param y y. @param w w. @param h h. @param ctx unused. */
static void rt_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U)
    {
        return;
    }
    for (int row = y; row < y + h; row++)
    {
        for (int col = x; col < x + w; col++)
        {
            if (row < 0 || row >= RT_SCREEN_H || col < 0 || col >= RT_SCREEN_W)
            {
                continue;
            }
            rt_over(&s_fb[row * RT_SCREEN_W + col],
                    a,
                    rt_div255(((argb >> 16) & 0xFFU) * a),
                    rt_div255(((argb >> 8) & 0xFFU) * a),
                    rt_div255((argb & 0xFFU) * a));
        }
    }
}

/** @brief blend_rect. @param src rows. @param st stride. @param alpha global alpha. @param x x. @param y y.
 * @param w w. @param h h. @param ctx unused. */
static void rt_blend(const void* src, int st, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* sp = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)st);
        for (int col = 0; col < w; col++)
        {
            const int fx = x + col, fy = y + row;
            if (fx < 0 || fx >= RT_SCREEN_W || fy < 0 || fy >= RT_SCREEN_H)
            {
                continue;
            }
            const uint32_t p = sp[col];
            uint32_t pa = (p >> 24) & 0xFFU;
            if (pa == 0U)
            {
                continue;
            }
            uint32_t pr = (p >> 16) & 0xFFU, pg = (p >> 8) & 0xFFU, pb = p & 0xFFU;
            if (alpha < 255U)
            {
                pa = rt_div255(pa * alpha);
                pr = rt_div255(pr * alpha);
                pg = rt_div255(pg * alpha);
                pb = rt_div255(pb * alpha);
            }
            rt_over(&s_fb[fy * RT_SCREEN_W + fx], pa, pr, pg, pb);
        }
    }
}

/** @brief copy_rect — an opaque blend. @param s src. @param st stride. @param x x. @param y y. @param w w.
 * @param h h. @param c ctx. */
static void rt_copy(const void* s, int st, int x, int y, int w, int h, void* c)
{
    rt_blend(s, st, 255U, x, y, w, h, c);
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
/**
 * @brief __touch(phase, x, y[, finger]): injects one touch event (phase 0 down / 1 move / 2 up / 3 cancel)
 *        and flushes coalesced moves — lets a runtime test drive engine-native gestures (a Dial drag, a
 *        responder negotiation) through the real bridge event path. The optional finger index (default 0)
 *        exercises the engine's per-finger touch slots for multi-touch scenarios. Test runner only; the
 *        device host feeds touches from its panel driver.
 */
static JSValue rt_touch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    int32_t phase = 0, x = 0, y = 0, finger = 0;
    if (argc >= 3)
    {
        JS_ToInt32(ctx, &phase, argv[0]);
        JS_ToInt32(ctx, &x, argv[1]);
        JS_ToInt32(ctx, &y, argv[2]);
    }
    if (argc >= 4)
    {
        JS_ToInt32(ctx, &finger, argv[3]);
    }
    embedded_renderer_touch((uint8_t)finger, (ERTouchPhase)phase, x, y);
    embedded_renderer_flush_touch();
    return JS_UNDEFINED;
}

/**
 * @brief __layoutPasses(): the engine's executed-layout-pass count (er_layout_pass_count).
 *
 * One per commit that re-solved layout, so a test that changes something sized can read it before
 * and after a frame to count the commits that frame actually paid for. Test runner only.
 */
static JSValue rt_layout_passes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewUint32(ctx, er_layout_pass_count());
}

/**
 * @brief __dirtyRect(): the last commit's repainted region as [x, y, w, h] (er_get_dirty_rect).
 *
 * The only engine output a runtime test can read back — pixels aren't observable headless. A commit
 * that painted nothing reports the previous one's region, so read it right after the commit you mean.
 * Test runner only.
 */
static JSValue rt_dirty_rect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    ERRect r = {0, 0, 0, 0};
    er_get_dirty_rect(&r);
    JSValue out = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, out, 0, JS_NewInt32(ctx, r.x));
    JS_SetPropertyUint32(ctx, out, 1, JS_NewInt32(ctx, r.y));
    JS_SetPropertyUint32(ctx, out, 2, JS_NewInt32(ctx, r.w));
    JS_SetPropertyUint32(ctx, out, 3, JS_NewInt32(ctx, r.h));
    return out;
}

/**
 * @brief __pixel(x, y): the framebuffer word at (x, y) as 0xAARRGGBB, or 0 when off-screen.
 *
 * What the engine actually painted, after anti-aliasing and blending. Test runner only.
 */
static JSValue rt_pixel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    int32_t x = -1, y = -1;
    if (argc >= 2)
    {
        JS_ToInt32(ctx, &x, argv[0]);
        JS_ToInt32(ctx, &y, argv[1]);
    }
    if (x < 0 || x >= RT_SCREEN_W || y < 0 || y >= RT_SCREEN_H)
    {
        return JS_NewUint32(ctx, 0);
    }
    return JS_NewUint32(ctx, s_fb[y * RT_SCREEN_W + x]);
}

static void rt_install_globals(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__touch", JS_NewCFunction(ctx, rt_touch, "__touch", 4));
    JS_SetPropertyStr(ctx, global, "__layoutPasses", JS_NewCFunction(ctx, rt_layout_passes, "__layoutPasses", 0));
    JS_SetPropertyStr(ctx, global, "__dirtyRect", JS_NewCFunction(ctx, rt_dirty_rect, "__dirtyRect", 0));
    JS_SetPropertyStr(ctx, global, "__pixel", JS_NewCFunction(ctx, rt_pixel, "__pixel", 2));

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
 * @param[in] argv  argv[1] = path to the bundled test JS; --typed-arrays adds that intrinsic.
 *
 * @return 0 when the test evaluates cleanly with no recorded failures; non-zero otherwise.
 */
int main(int argc, char** argv)
{
    const char* path = NULL;
    uint32_t intrinsics = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--typed-arrays") == 0)
        {
            intrinsics |= ER_JS_INTRINSIC_TYPED_ARRAYS;
        }
        else if (!path)
        {
            path = argv[i];
        }
    }
    if (!path)
    {
        fprintf(stderr, "usage: %s [--typed-arrays] <bundle.js>\n", argv[0]);
        return 2;
    }

    size_t src_len = 0;
    char* src = read_file(path, &src_len);
    if (!src)
    {
        fprintf(stderr, "could not read '%s'\n", path);
        return 2;
    }
    const bool bytecode = is_bytecode_path(path);

    static const EmbeddedRenderBackend backend = {rt_fill, rt_copy, rt_blend, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    JSRuntime* rt = JS_NewRuntime();
    /* The device's lite intrinsic profile (not a full JS_NewContext): a bundle that only works with
     * intrinsics a device build strips would pass here and then fail on hardware. EVAL is required so
     * JS_Eval can run .js test bundles (bytecode-only device builds don't have it). Anything beyond
     * the lite set has to be asked for, per test, so the default stays the device's profile. */
    JSContext* ctx = er_js_new_context(rt, ER_JS_INTRINSIC_EVAL | intrinsics);
    rt_install_globals(ctx);
    er_bridge_install(ctx);

    int status = 0;
    JSValue result = bytecode ? er_bridge_run_bytecode(ctx, (const uint8_t*)src, src_len)
                              : JS_Eval(ctx, src, strlen(src), path, JS_EVAL_TYPE_GLOBAL);
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
