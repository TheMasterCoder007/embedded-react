/*
 * embedded-react — ESP32-S3 host, Phase 3 (RGB display + GT911 touch).
 *
 * Boots a QuickJS runtime with its heap in PSRAM, installs the NativeUI bridge + host globals,
 * loads the precompiled bytecode bundle (embedded in flash), brings up the Waveshare 7" RGB panel
 * (board.c), and runs the React app drawing to it via the esp32-lcd backend. Polls the GT911 touch
 * controller each frame and feeds presses to the engine (so the on-screen buttons work). If the
 * panel fails to init, it falls back to a no-op backend so the JS stack still runs and logs over UART.
 *
 * See ../README.md.
 */

#include "board.h"
#include "er_scene.h"
#include "esp32_lcd_backend.h"
#include "image_data.h" /* generated: er_register_baked_images() — the demo's weather icons */
#include "native_renderer.h"
#include "native_ui_bridge.h"
#include "perf_overlay.h"
#include "quickjs.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

static const char* TAG = "embedded-react";

/* Waveshare ESP32-S3-Touch-LCD-7 panel resolution. The app sizes its root from `screen`. */
#define SCREEN_W 800
#define SCREEN_H 480

/* QuickJS stack-overflow guard. Kept below the main task stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE,
   set large in sdkconfig.defaults) so deep React/QuickJS recursion is caught before it corrupts the
   FreeRTOS task stack. Tune together with the task stack size if you hit "stack overflow" from JS. */
#define ER_JS_MAX_STACK (48 * 1024)

/* Precompiled bytecode bundle, embedded in flash via EMBED_FILES (see main/CMakeLists.txt). */
extern const uint8_t qbc_start[] asm("_binary_app_bundle_qbc_start");
extern const uint8_t qbc_end[] asm("_binary_app_bundle_qbc_end");

/*----------------------------------------------------------------------------------------------------------------------
 - QuickJS heap → PSRAM
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief PSRAM-backed calloc for the JS heap. */
static void* er_js_calloc(void* opaque, size_t count, size_t size)
{
    (void)opaque;
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM);
}

/** @brief PSRAM-backed malloc for the JS heap. */
static void* er_js_malloc(void* opaque, size_t size)
{
    (void)opaque;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

/** @brief Frees a JS-heap allocation. */
static void er_js_free(void* opaque, void* ptr)
{
    (void)opaque;
    heap_caps_free(ptr);
}

/** @brief PSRAM-backed realloc for the JS heap. */
static void* er_js_realloc(void* opaque, void* ptr, size_t size)
{
    (void)opaque;
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
}

/** @brief Reports the usable size of a JS-heap allocation. */
static size_t er_js_usable_size(const void* ptr)
{
    return heap_caps_get_allocated_size((void*)ptr);
}

static const JSMallocFunctions er_js_mf = {
    .js_calloc = er_js_calloc,
    .js_malloc = er_js_malloc,
    .js_free = er_js_free,
    .js_realloc = er_js_realloc,
    .js_malloc_usable_size = er_js_usable_size,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Host globals (console / screen)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief console.log/warn/error → the UART log. Space-separated, one line per call.
 *
 * @return JS_UNDEFINED, or JS_EXCEPTION if an argument cannot be stringified.
 */
static JSValue host_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    char line[256];
    size_t off = 0;
    for (int i = 0; i < argc && off < sizeof(line) - 1; i++)
    {
        const char* s = JS_ToCString(ctx, argv[i]);
        if (!s)
        {
            return JS_EXCEPTION;
        }
        off += (size_t)snprintf(line + off, sizeof(line) - off, "%s%s", i ? " " : "", s);
        JS_FreeCString(ctx, s);
    }
    ESP_LOGI("js", "%s", line);
    return JS_UNDEFINED;
}

/** @brief Installs a `console` (log/warn/error → UART) onto the global scope. */
static void install_console(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JSValue log = JS_NewCFunction(ctx, host_console_log, "log", 1);
    JS_SetPropertyStr(ctx, console, "log", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "warn", JS_DupValue(ctx, log));
    JS_SetPropertyStr(ctx, console, "error", log);
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/** @brief Injects a `screen` global ({ width, height, scale }) so the app can size its root. */
static void install_screen(JSContext* ctx, int width, int height)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, width));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, height));
    JS_SetPropertyStr(ctx, screen, "scale", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, global, "screen", screen);
    JS_FreeValue(ctx, global);
}

/** @brief Prints a pending JS exception (message + stack) to the log. */
static void report_exception(JSContext* ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    ESP_LOGE(TAG, "JS exception: %s", msg ? msg : "(unknown)");
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
            ESP_LOGE(TAG, "%s", st);
            JS_FreeCString(ctx, st);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

/*----------------------------------------------------------------------------------------------------------------------
 - No-op render backend (display fallback)
 ---------------------------------------------------------------------------------------------------------------------*/

static void noop_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}
static void noop_copy(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}
static void noop_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

static const EmbeddedRenderBackend k_noop_backend = {noop_fill, noop_copy, noop_blend, NULL, NULL, NULL};

/*----------------------------------------------------------------------------------------------------------------------
 - App
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Returns milliseconds since boot (engine-clock delta source). */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#if ER_PERF_OVERLAY
/*----------------------------------------------------------------------------------------------------------------------
 - Performance overlay metrics (gathered host-side; ER_PERF_OVERLAY only)
 ---------------------------------------------------------------------------------------------------------------------*/

static char s_perf_buf[4][24];
static const char* s_perf_lines[4] = {s_perf_buf[0], s_perf_buf[1], s_perf_buf[2], s_perf_buf[3]};
static int s_perf_nlines = 0;
static uint32_t s_perf_busy_us = 0; /* the previous frame's full work time (pump+commit+present) */

/**
 * @brief Accumulates frame stats and refreshes the overlay text ~twice a second.
 *
 * CPU load is the share of wall-clock the loop spends working vs. idling in vTaskDelay (LVGL-style),
 * averaged over the window. Returns true only on the frame the text changed, so the caller redraws
 * the panel then and leaves it persistent (and the per-frame flush tight) the rest of the time.
 *
 * @param[in] frame_start_us  esp_timer timestamp captured at the top of this frame.
 *
 * @return true when the displayed metrics were just updated.
 */
static bool perf_overlay_refresh(int64_t frame_start_us)
{
    static int64_t window_start = 0;
    static uint32_t frames = 0;
    static uint64_t busy_sum = 0;

    if (window_start == 0)
    {
        window_start = frame_start_us;
    }
    frames++;
    busy_sum += s_perf_busy_us;

    const int64_t elapsed = frame_start_us - window_start;
    if (elapsed < 500000)
    {
        return false;
    }

    const uint32_t fps = (uint32_t)((uint64_t)frames * 1000000ULL / (uint64_t)elapsed);
    uint32_t cpu = (uint32_t)(busy_sum * 100ULL / (uint64_t)elapsed);
    if (cpu > 100U)
    {
        cpu = 100U;
    }
    const uint32_t spiram_k = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
    const uint32_t iram_k = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U);
    snprintf(s_perf_buf[0], sizeof(s_perf_buf[0]), "FPS %u", (unsigned)fps);
    snprintf(s_perf_buf[1], sizeof(s_perf_buf[1]), "CPU %u%%", (unsigned)cpu);
    snprintf(s_perf_buf[2], sizeof(s_perf_buf[2]), "PSRAM %uK", (unsigned)spiram_k);
    snprintf(s_perf_buf[3], sizeof(s_perf_buf[3]), "IRAM %uK", (unsigned)iram_k);
    s_perf_nlines = 4;

    frames = 0;
    busy_sum = 0;
    window_start = frame_start_us;
    return true;
}
#endif

/* Target frame period for the adaptive pacer (~50 fps, just above the RGB panel's ~51 Hz refresh).
 * Light frames sleep the remainder up to this; heavy frames yield the minimum and run as fast as the
 * work allows. The real win needs fine ticks — see CONFIG_FREERTOS_HZ=1000 in sdkconfig.defaults. */
#define ER_TARGET_FRAME_MS 20U

/**
 * @brief Boots QuickJS + the engine, runs the embedded bytecode app, then drives the frame loop.
 */
static void run_app(void)
{
    /* Bring up the RGB panel and draw to it; fall back to the no-op backend if it fails so the JS
       stack still runs (and logs why) over UART. */
    esp_lcd_panel_handle_t panel = NULL;
    const bool display =
        board_display_init(&panel) && er_esp32_lcd_backend_init(panel, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    if (display)
    {
        ESP_LOGI(TAG, "display backend active");
    }
    else
    {
        ESP_LOGW(TAG, "display init failed — falling back to no-op backend (headless)");
        embedded_renderer_set_backend(&k_noop_backend);
    }

    /* Register the demo's baked weather icons now that the backend (and image registry) is up, before
     * the app mounts and creates <Image> nodes that look them up by name. */
    er_register_baked_images();

    /* Bring up touch (shares the panel's I2C bus). Optional: the UI still renders without it. */
    const bool touch = display && board_touch_init();
    if (display && !touch)
    {
        ESP_LOGW(TAG, "touch init failed — display works but input is disabled");
    }

    JSRuntime* rt = JS_NewRuntime2(&er_js_mf, NULL);
    if (!rt)
    {
        ESP_LOGE(TAG, "JS_NewRuntime2 failed (PSRAM exhausted?)");
        return;
    }
    JS_SetMaxStackSize(rt, ER_JS_MAX_STACK);

    JSContext* ctx = JS_NewContext(rt);
    JS_UpdateStackTop(rt); /* record this task's stack base for the overflow guard */

    install_console(ctx);
    install_screen(ctx, SCREEN_W, SCREEN_H);
    er_bridge_install(ctx);

    const size_t qbc_len = (size_t)(qbc_end - qbc_start);
    ESP_LOGI(TAG, "loading %u bytes of bytecode", (unsigned)qbc_len);

    JSValue result = er_bridge_run_bytecode(ctx, qbc_start, qbc_len);
    if (JS_IsException(result))
    {
        report_exception(ctx);
    }
    JS_FreeValue(ctx, result);
    er_bridge_pump(ctx); /* settle mount-time promises/effects */

    /* Prove layout + paint ran by reporting the painted region of the first commit. */
    er_commit();
    if (display)
    {
        er_esp32_lcd_present(); /* push the mount's paint to the panel (engine doesn't auto-present) */
    }
    ERRect dirty = {0, 0, 0, 0};
    const bool painted = er_get_dirty_rect(&dirty);
    ESP_LOGI(TAG, "first commit painted=%d dirty=%d,%d %dx%d", (int)painted, dirty.x, dirty.y, dirty.w, dirty.h);
    ESP_LOGI(TAG,
             "free PSRAM after boot: %u, free internal: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Frame loop: poll touch, pump JS (promises + timers), commit, present, advance animations. */
    uint32_t prev = now_ms();
    uint32_t frame = 0;
    bool touch_down = false; /* press state machine: turns GT911 snapshots into down/move/up */
    int touch_x = 0;
    int touch_y = 0;
    while (true)
    {
        /* Forward touch to the engine before pumping so a press lands in the same frame's render. */
        if (touch)
        {
            int tx = 0;
            int ty = 0;
            bool pressed = false;
            if (board_touch_read(&tx, &ty, &pressed))
            {
                if (pressed)
                {
                    embedded_renderer_touch(0, touch_down ? ER_TOUCH_MOVE : ER_TOUCH_DOWN, tx, ty);
                    touch_down = true;
                    touch_x = tx;
                    touch_y = ty;
                }
                else if (touch_down)
                {
                    embedded_renderer_touch(0, ER_TOUCH_UP, touch_x, touch_y);
                    touch_down = false;
                }
            }
        }

        const int64_t frame_start_us = esp_timer_get_time();
        er_bridge_pump(ctx); /* drain JS promises + fire due timers */
        er_commit();         /* lay out (if needed) + paint the changed region into the backend */
#if ER_PERF_OVERLAY
        /* Render the overlay text only when the metrics change (~twice a second; glyph rendering is the
           expensive part) and snapshot that region. The backend then re-composites the snapshot on top
           of EVERY present (a cheap RGB565 copy), so the overlay stays flicker-free over the
           double-buffered panel without per-frame text rendering, and as its own tight region so it
           never enlarges the app's per-frame dirty box. */
        if (display && perf_overlay_refresh(frame_start_us))
        {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            er_esp32_lcd_overlay_begin(); /* save the app dirty box; capture restores it */
            er_perf_overlay_draw(SCREEN_W, SCREEN_H, s_perf_lines, s_perf_nlines, &ox, &oy, &ow, &oh);
            er_esp32_lcd_overlay_capture(ox, oy, ow, oh);
        }
#endif
        if (display)
        {
            er_esp32_lcd_present(); /* flush the app region + re-composite the overlay */
        }

        const uint32_t now = now_ms();
        embedded_renderer_tick(now - prev);
        prev = now;

        if ((++frame % 120U) == 0U)
        {
            ESP_LOGI(TAG, "alive: %u frames, %u ms uptime", (unsigned)frame, (unsigned)now);
        }

        /* Adaptive pacing: sleep only the remainder up to ER_TARGET_FRAME_MS so heavy frames are not
           taxed by a fixed delay (the old fixed 16 ms was ~10 ms at 100 Hz ticks — pure overhead on a
           30 ms frame). Always block at least one tick so the idle task runs (watchdog); that floor is
           10 ms at 100 Hz but 1 ms at 1000 Hz, which is why sdkconfig.defaults sets CONFIG_FREERTOS_HZ. */
        const uint32_t used_ms = (uint32_t)((esp_timer_get_time() - frame_start_us) / 1000);
#if ER_PERF_OVERLAY
        s_perf_busy_us = used_ms * 1000U; /* work time (excl. sleep) for the CPU-load metric */
#endif
        const uint32_t sleep_ms = (used_ms >= ER_TARGET_FRAME_MS) ? 0U : (ER_TARGET_FRAME_MS - used_ms);
        TickType_t sleep_ticks = pdMS_TO_TICKS(sleep_ms);
        if (sleep_ticks == 0U)
        {
            sleep_ticks = 1U; /* always yield >= 1 tick so the idle task is fed */
        }
        vTaskDelay(sleep_ticks);
    }
}

/** @brief IDF entry point. */
void app_main(void)
{
    ESP_LOGI(TAG, "embedded-react ESP32-S3 host — Phase 2 (RGB display)");
    ESP_LOGI(TAG,
             "PSRAM free: %u bytes, internal free: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0)
    {
        ESP_LOGE(TAG, "no PSRAM detected — check CONFIG_SPIRAM in sdkconfig.defaults");
        return;
    }

    run_app();
}
