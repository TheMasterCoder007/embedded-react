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
#include "host_harness.h"
#include "native_renderer.h"
#include "native_ui_bridge.h"
#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define RT_SCREEN_WIDTH 480
#define RT_SCREEN_HEIGHT 320

/** @brief Largest channel value, and the half of it that rounds a division by it. */
#define RT_CHANNEL_MAX 255U
#define RT_CHANNEL_ROUND 127U

/** @brief Alpha bits of an opaque framebuffer pixel. */
#define RT_OPAQUE_ALPHA_BITS 0xFF000000U

/** @brief Ending of a compiled bytecode path. */
#define RT_BYTECODE_SUFFIX ".qbc"
#define RT_BYTECODE_SUFFIX_LENGTH 4

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — framebuffer backend + shims
 ---------------------------------------------------------------------------------------------------------------------*/

/* The backend paints into a plain host framebuffer rather than dropping the output, so a test can
 * read a pixel back through __pixel() and assert what actually got drawn. A stubbed blend_rect would
 * discard every anti-aliased span, which is most of what the vector and text paths emit. */
static uint32_t s_framebuffer[RT_SCREEN_WIDTH * RT_SCREEN_HEIGHT];

/** @brief Rounds (value / 255) the way the engine's own blend does. */
static uint32_t rt_div255(uint32_t value)
{
    return (value + RT_CHANNEL_ROUND) / RT_CHANNEL_MAX;
}

/** @brief Source-over of one premultiplied pixel onto the opaque framebuffer. */
static void rt_over(uint32_t* destination, uint32_t alpha, uint32_t red, uint32_t green, uint32_t blue)
{
    if (alpha == 0U)
    {
        return;
    }
    if (alpha >= RT_CHANNEL_MAX)
    {
        *destination = RT_OPAQUE_ALPHA_BITS | (red << 16) | (green << 8) | blue;
        return;
    }
    const uint32_t inverse_alpha = RT_CHANNEL_MAX - alpha;
    const uint32_t blended_red = red + rt_div255(((*destination >> 16) & 0xFFU) * inverse_alpha);
    const uint32_t blended_green = green + rt_div255(((*destination >> 8) & 0xFFU) * inverse_alpha);
    const uint32_t blended_blue = blue + rt_div255((*destination & 0xFFU) * inverse_alpha);
    *destination = RT_OPAQUE_ALPHA_BITS | (blended_red << 16) | (blended_green << 8) | blended_blue;
}

/** @brief fill_rect. @param argb Color. @param x Left. @param y Top. @param width Width. @param height Height.
 * @param ctx  Unused. */
static void rt_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U)
    {
        return;
    }
    for (int row = y; row < y + height; row++)
    {
        for (int column = x; column < x + width; column++)
        {
            if (row < 0 || row >= RT_SCREEN_HEIGHT || column < 0 || column >= RT_SCREEN_WIDTH)
            {
                continue;
            }
            rt_over(&s_framebuffer[row * RT_SCREEN_WIDTH + column],
                    alpha,
                    rt_div255(((argb >> 16) & 0xFFU) * alpha),
                    rt_div255(((argb >> 8) & 0xFFU) * alpha),
                    rt_div255((argb & 0xFFU) * alpha));
        }
    }
}

/** @brief blend_rect. @param source Rows. @param stride Row stride. @param alpha Global alpha. @param x Left.
 * @param y  Top. @param width Width. @param height Height. @param ctx Unused. */
static void rt_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int frame_x = x + column, frame_y = y + row;
            if (frame_x < 0 || frame_x >= RT_SCREEN_WIDTH || frame_y < 0 || frame_y >= RT_SCREEN_HEIGHT)
            {
                continue;
            }
            const uint32_t pixel = source_row[column];
            uint32_t pixel_alpha = (pixel >> 24) & 0xFFU;
            if (pixel_alpha == 0U)
            {
                continue;
            }
            uint32_t pixel_red = (pixel >> 16) & 0xFFU, pixel_green = (pixel >> 8) & 0xFFU, pixel_blue = pixel & 0xFFU;
            if (alpha < RT_CHANNEL_MAX)
            {
                pixel_alpha = rt_div255(pixel_alpha * alpha);
                pixel_red = rt_div255(pixel_red * alpha);
                pixel_green = rt_div255(pixel_green * alpha);
                pixel_blue = rt_div255(pixel_blue * alpha);
            }
            rt_over(
                &s_framebuffer[frame_y * RT_SCREEN_WIDTH + frame_x], pixel_alpha, pixel_red, pixel_green, pixel_blue);
        }
    }
}

/** @brief copy_rect — an opaque blend. @param source Rows. @param stride Row stride. @param x Left. @param y Top.
 * @param width  Width. @param height Height. @param ctx Unused. */
static void rt_copy(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    rt_blend(source, stride, RT_CHANNEL_MAX, x, y, width, height, ctx);
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
    for (int argument_index = 0; argument_index < argc; argument_index++)
    {
        const char* text = JS_ToCString(ctx, argv[argument_index]);
        if (!text)
        {
            return JS_EXCEPTION;
        }
        printf("%s%s", argument_index ? " " : "", text);
        JS_FreeCString(ctx, text);
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
    ERRect rect = {0, 0, 0, 0};
    er_get_dirty_rect(&rect);
    JSValue result = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, result, 0, JS_NewInt32(ctx, rect.x));
    JS_SetPropertyUint32(ctx, result, 1, JS_NewInt32(ctx, rect.y));
    JS_SetPropertyUint32(ctx, result, 2, JS_NewInt32(ctx, rect.width));
    JS_SetPropertyUint32(ctx, result, 3, JS_NewInt32(ctx, rect.height));
    return result;
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
    if (x < 0 || x >= RT_SCREEN_WIDTH || y < 0 || y >= RT_SCREEN_HEIGHT)
    {
        return JS_NewUint32(ctx, 0);
    }
    return JS_NewUint32(ctx, s_framebuffer[y * RT_SCREEN_WIDTH + x]);
}

/**
 * @brief __setWallClock(ms): er_runtime_set_wall_clock, the call a device host makes after reading an RTC
 *        or an SNTP sync. Test runner only.
 */
static JSValue rt_set_wall_clock(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    int64_t epoch_ms = 0;
    if (argc >= 1 && JS_ToInt64(ctx, &epoch_ms, argv[0]) < 0)
    {
        return JS_EXCEPTION; /* e.g. a Symbol: let the throw reach the test instead of setting 0 */
    }
    er_runtime_set_wall_clock(epoch_ms);
    return JS_UNDEFINED;
}

static void rt_install_globals(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__touch", JS_NewCFunction(ctx, rt_touch, "__touch", 4));
    JS_SetPropertyStr(ctx, global, "__layoutPasses", JS_NewCFunction(ctx, rt_layout_passes, "__layoutPasses", 0));
    JS_SetPropertyStr(ctx, global, "__dirtyRect", JS_NewCFunction(ctx, rt_dirty_rect, "__dirtyRect", 0));
    JS_SetPropertyStr(ctx, global, "__pixel", JS_NewCFunction(ctx, rt_pixel, "__pixel", 2));
    JS_SetPropertyStr(ctx, global, "__setWallClock", JS_NewCFunction(ctx, rt_set_wall_clock, "__setWallClock", 1));

    JSValue console = JS_NewObject(ctx);
    JSValue log = JS_NewCFunction(ctx, rt_console_log, "log", 1);
    JS_SetPropertyStr(ctx, console, "log", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "warn", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "error", log);
    JS_SetPropertyStr(ctx, global, "console", console);

    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, RT_SCREEN_WIDTH));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, RT_SCREEN_HEIGHT));
    JS_SetPropertyStr(ctx, screen, "scale", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, global, "screen", screen);

    JS_FreeValue(ctx, global);
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
    const size_t length = strlen(path);
    return length >= RT_BYTECODE_SUFFIX_LENGTH
           && strcmp(path + length - RT_BYTECODE_SUFFIX_LENGTH, RT_BYTECODE_SUFFIX) == 0;
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
    for (int argument_index = 1; argument_index < argc; argument_index++)
    {
        if (strcmp(argv[argument_index], "--typed-arrays") == 0)
        {
            intrinsics |= ER_JS_INTRINSIC_TYPED_ARRAYS;
        }
        else if (!path)
        {
            path = argv[argument_index];
        }
    }
    if (!path)
    {
        fprintf(stderr, "usage: %s [--typed-arrays] <bundle.js>\n", argv[0]);
        return 2;
    }

    size_t source_length = 0;
    char* source = er_host_read_file(path, &source_length);
    if (!source)
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
    JSValue result = bytecode ? er_bridge_run_bytecode(ctx, (const uint8_t*)source, source_length)
                              : JS_Eval(ctx, source, strlen(source), path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
    {
        JSValue exception = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, exception);
        fprintf(stderr, "JS exception: %s\n", message ? message : "(unknown)");
        if (message)
        {
            JS_FreeCString(ctx, message);
        }
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack))
        {
            const char* stack_text = JS_ToCString(ctx, stack);
            if (stack_text)
            {
                fprintf(stderr, "%s\n", stack_text);
                JS_FreeCString(ctx, stack_text);
            }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exception);
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
        int32_t failures = 0;
        JS_ToInt32(ctx, &failures, failed);
        JS_FreeValue(ctx, failed);
        JS_FreeValue(ctx, global);
        if (failures > 0)
        {
            status = 1;
        }
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(source);
    return status;
}
