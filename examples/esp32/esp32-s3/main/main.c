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
 * embedded-react — ESP32-S3 host, Phase 3 (RGB display + GT911 touch).
 *
 * The uploaded-config model (peer to the desktop demo): this firmware brings up the Waveshare 7" RGB
 * panel (board.c) + GT911 touch and the portable QuickJS host core (er_runtime, JS heap in PSRAM),
 * then loads its UI + logic as a config container (app.erpkg) from a dedicated flash partition —
 * memory-mapped and verified (CRC + QuickJS-version), not embedded in the firmware image. The config
 * carries its own assets (images + fonts), so they are NOT compiled in. No config / a corrupt or
 * version-incompatible one paints an on-screen "Couldn't load config" panel and the loop keeps running
 * (so it stays visible), exactly like the desktop. If the panel fails to init, a no-op backend keeps
 * the JS stack running and logging over UART.
 *
 * Flash a config to the 'config' partition separately from the firmware (see ../README.md):
 *   parttool.py write_partition --partition-name=config \
 *       --input ../../bridges/quickjs/js/dist/app.erpkg
 *
 * See ../README.md.
 */

#include "board.h"
#include "er_hotreload.h" /* er_hotreload_apply (on-device hot reload) */
#include "er_runtime.h"
#include "er_scene.h"
#include "esp32_lcd_backend.h"
#include "native_renderer.h"
#include "perf_overlay.h"
#include "quickjs.h" /* JSMallocFunctions (PSRAM JS heap) */

#if ER_HOTRELOAD
#include "hotreload_usb.h" /* USB-Serial-JTAG receiver shim (this example's transport) */
#endif

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_partition.h"
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

/* Whole-screen rotation (clockwise): 0 / 90 / 180 / 270. The panel is physically BOARD_LCD_WIDTH ×
   BOARD_LCD_HEIGHT (800×480); for 90/270 the LOGICAL screen the app renders into is the swap (480×800,
   a portrait UI). The backend maps logical→physical at present; touch is remapped by the inverse. */
#ifndef ER_DISPLAY_ROTATION
#define ER_DISPLAY_ROTATION 0
#endif

/* Logical screen the app sizes its root from (`screen` global) — panel size, swapped for 90/270. */
#if (ER_DISPLAY_ROTATION == 90) || (ER_DISPLAY_ROTATION == 270)
#define SCREEN_W BOARD_LCD_HEIGHT
#define SCREEN_H BOARD_LCD_WIDTH
#else
#define SCREEN_W BOARD_LCD_WIDTH
#define SCREEN_H BOARD_LCD_HEIGHT
#endif

/** @brief Remaps a physical (panel) touch point to logical (app) coords by the inverse rotation. */
static void remap_touch(int* x, int* y)
{
    const int px = *x, py = *y;
    switch (ER_DISPLAY_ROTATION)
    {
        case 90:
            *x = py;
            *y = BOARD_LCD_WIDTH - 1 - px;
            break;
        case 180:
            *x = BOARD_LCD_WIDTH - 1 - px;
            *y = BOARD_LCD_HEIGHT - 1 - py;
            break;
        case 270:
            *x = BOARD_LCD_HEIGHT - 1 - py;
            *y = px;
            break;
        default:
            break;
    }
}

/* QuickJS stack-overflow guard. Kept below the main task stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE,
   set large in sdkconfig.defaults) so deep React/QuickJS recursion is caught before it corrupts the
   FreeRTOS task stack. Tune together with the task stack size if you hit "stack overflow" from JS. */
#define ER_JS_MAX_STACK (48 * 1024)

/* Hard cap on the JS heap (all of it lives in PSRAM via er_js_mf). A runaway app fails with a JS
   out-of-memory error — caught and shown by the error overlay — instead of exhausting the PSRAM the
   framebuffers and engine pools share. */
#define ER_JS_MEMORY_LIMIT (4u * 1024u * 1024u)

/* Label of the data partition the config container (app.erpkg) is flashed into (see partitions.csv). */
#define ER_CONFIG_PARTITION_LABEL "config"

/* On-device hot reload over USB-Serial-JTAG. DEV-ONLY and OFF by default — the standard dev workflow is
 * the web simulator (`npx embedded-react dev`); a project opts a board in by building with ER_HOTRELOAD=1
 * (see CMakeLists). ER_HOTRELOAD_MAX_BYTES sizes the single PSRAM staging buffer — the largest hot-reload
 * frame the device accepts in one upload. With the vendor/app split, an edit ships only the APP frame (app
 * bytecode + its assets). A vendor change (rare — a new dependency) is delivered by re-flashing the config
 * partition, not over hot reload. Raise this only if an app's bytecode + assets outgrow it. */
#ifndef ER_HOTRELOAD
#define ER_HOTRELOAD 0
#endif
#ifndef ER_HOTRELOAD_MAX_BYTES
#define ER_HOTRELOAD_MAX_BYTES (512 * 1024)
#endif

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
 - Host log sink (console.* + runtime errors → UART)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief er_runtime log callback: one already-formatted line (console.* and error capture) → UART. */
static void uart_log(const char* line)
{
    ESP_LOGI("js", "%s", line);
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

static const EmbeddedRenderBackend k_noop_backend = {
    noop_fill,
    noop_copy,
    noop_blend,
    NULL,
    NULL,
    NULL, /* fill / copy / blend / wait / frame_ready / ctx */
    0,    /* band_height: 0 = classic full-framebuffer path */
    NULL,
    NULL /* band_begin / band_flush (unused without banding) */
};

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

/** @brief Microsecond clock for the engine's temporary ER_PROF instrumentation. */
uint32_t er_prof_now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

/* Target frame period for the adaptive pacer (~50 fps, just above the RGB panel's ~51 Hz refresh).
 * Light frames sleep the remainder up to this; heavy frames yield the minimum and run as fast as the
 * work allows. The real win needs fine ticks — see CONFIG_FREERTOS_HZ=1000 in sdkconfig.defaults. */
#define ER_TARGET_FRAME_MS 20U

/**
 * @brief Memory-maps the 'config' partition and loads the container it holds via er_runtime.
 *
 * The mapping is kept for the process lifetime — the container's image pixels / font bitmaps are
 * referenced in place from flash, never copied. The partition size is an upper bound; the loader walks
 * the container to find its true length and verifies its CRC + QuickJS version.
 *
 * @param[out] out_status  Receives the load status (valid only when the partition was found + mapped).
 *
 * @return true if the 'config' partition exists and was mapped (then *out_status holds the result);
 *         false if there is no config partition / it could not be mapped.
 */
static bool load_config_partition(ErContainerStatus* out_status)
{
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ER_CONFIG_PARTITION_LABEL);
    if (!part)
    {
        ESP_LOGE(TAG, "no '%s' partition — add it to partitions.csv", ER_CONFIG_PARTITION_LABEL);
        return false;
    }
    const void* mapped = NULL;
    esp_partition_mmap_handle_t handle;
    const esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mmap '%s' failed: %s", ER_CONFIG_PARTITION_LABEL, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "config partition mapped (%u bytes) — loading container", (unsigned)part->size);
    *out_status = er_runtime_load_container(mapped, part->size); /* mapping kept for the process */
    return true;
}

#if ERUI_RENDER_WORKERS > 1
/*----------------------------------------------------------------------------------------------------------------------
 - Multi-core rendering: worker 1 on CPU core 1
 ---------------------------------------------------------------------------------------------------------------------*/

/* The engine never creates threads — this example provides render worker 1 as a FreeRTOS task
 * pinned to core 1 (the main/render task is pinned to core 0 by ESP-IDF's default main-task
 * affinity, so xPortGetCoreID doubles as the worker id). The engine forks each commit's render
 * into horizontal slices across both cores; see EmbeddedRenderWorkers in native_renderer.h. */
static TaskHandle_t s_render_worker;
static SemaphoreHandle_t s_render_worker_done;

static void render_worker_main(void* arg)
{
    (void)arg;
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        er_render_worker_exec(1);
        xSemaphoreGive(s_render_worker_done);
    }
}

static void render_worker_dispatch(int worker, void* ctx)
{
    (void)worker; /* only worker 1 exists */
    (void)ctx;
    xTaskNotifyGive(s_render_worker);
}

static void render_worker_sync(void* ctx)
{
    (void)ctx;
    xSemaphoreTake(s_render_worker_done, portMAX_DELAY);
}

static int render_worker_id(void)
{
    return xPortGetCoreID(); /* workers are core-pinned: core id == worker id */
}

/** @brief Spawns the core-1 render worker and hands it to the engine. */
static void install_render_workers(void)
{
    static const EmbeddedRenderWorkers workers = {
        .count = 2,
        .dispatch = render_worker_dispatch,
        .sync = render_worker_sync,
        .worker_id = render_worker_id,
        .ctx = NULL,
    };
    /* 24 KB: the worker runs the full render recursion (nested composites + transform emits with
     * their on-stack column arrays) but never QuickJS, so it needs render-depth headroom only —
     * 12 KB measurably overflowed on the banded-fade pages. */
    s_render_worker_done = xSemaphoreCreateBinary();
    if (!s_render_worker_done
        || xTaskCreatePinnedToCore(render_worker_main, "er_worker", 24576, NULL, 5, &s_render_worker, 1) != pdPASS)
    {
        ESP_LOGW(TAG, "render worker creation failed — rendering stays single-core");
        return;
    }
    embedded_renderer_set_workers(&workers);
    ESP_LOGI(TAG, "multi-core rendering: worker 1 pinned to core 1");
}
#endif /* ERUI_RENDER_WORKERS > 1 */

/**
 * @brief Boots the engine + er_runtime, loads the config from flash, then drives the frame loop.
 */
static void run_app(void)
{
    /* Bring up the RGB panel and draw to it; fall back to the no-op backend if it fails so the JS
       stack still runs (and logs why) over UART. */
    esp_lcd_panel_handle_t panel = NULL;
    const bool display =
        board_display_init(&panel) && er_esp32_lcd_backend_init(panel, SCREEN_W, SCREEN_H, ER_DISPLAY_ROTATION);
    if (display)
    {
        ESP_LOGI(TAG, "display backend active");
    }
    else
    {
        ESP_LOGW(TAG, "display init failed — falling back to no-op backend (headless)");
        embedded_renderer_set_backend(&k_noop_backend);
    }

#if ERUI_RENDER_WORKERS > 1
    /* Fork render passes across both CPU cores (after display init so its heap needs come first). */
    install_render_workers();
#endif

    /* No baked assets are compiled in: the config container carries its own images + fonts and they
       are registered when it loads (er_runtime_load_container) — just like the desktop demo. */

    /* Bring up touch (shares the panel's I2C bus). Optional: the UI still renders without it. */
    const bool touch = display && board_touch_init();
    if (display && !touch)
    {
        ESP_LOGW(TAG, "touch init failed — display works but input is disabled");
    }

    /* Boot the portable QuickJS host core with the JS heap in PSRAM and the stack guard set. */
    const ErRuntimeConfig rt_cfg = {
        .screen_width = SCREEN_W,
        .screen_height = SCREEN_H,
        .screen_scale = 1.0f,
        /* With hot reload wired (Phase 4), install the persist store so usePersistentState survives a
           live reload — the same behavior as the desktop simulator. Off when hot reload is compiled out. */
        .install_persist = (ER_HOTRELOAD != 0),
        .log = uart_log,
        .malloc_functions = &er_js_mf, /* JS heap → PSRAM */
        .max_stack_size = ER_JS_MAX_STACK,
        .memory_limit = ER_JS_MEMORY_LIMIT,
    };
    if (!er_runtime_init(&rt_cfg))
    {
        ESP_LOGE(TAG, "er_runtime_init failed (PSRAM exhausted?)");
        return;
    }

#if ER_HOTRELOAD
    /* Bring up the hot-reload receiver: `embedded-react dev --device <port>` streams a fresh app.erpkg
       over USB-Serial-JTAG on every save and the frame loop swaps it in live (see below). Non-fatal if
       it can't start — the flashed config still runs. */
    const bool hot_reload = er_hotreload_usb_start(ER_HOTRELOAD_MAX_BYTES);
    if (!hot_reload)
    {
        ESP_LOGW(TAG, "hot reload unavailable — running the flashed config only");
    }
#endif

    /* Load the UI + logic from the config partition. On any failure, paint the on-screen panel and
       keep going — the loop below presents it and feeds the watchdog (firmware doesn't halt because a
       config is missing/bad, mirroring the desktop demo). */
    ErContainerStatus cfg_status = ER_CONTAINER_LOAD_FAILED;
    if (!load_config_partition(&cfg_status))
    {
        er_runtime_show_message("No config loaded",
                                "No config partition was found or it could not be mapped.",
                                "Flash an app.erpkg to the 'config' partition, then reboot.");
    }
    else if (cfg_status != ER_CONTAINER_OK)
    {
        const char* reason = er_runtime_container_status_str(cfg_status);
        ESP_LOGE(TAG, "config rejected: %s", reason);
        er_runtime_show_message("Couldn't load config", reason, "Re-pack with `npm run pack` and re-flash the config.");
    }
    else
    {
        ESP_LOGI(TAG, "config loaded");
    }

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
                remap_touch(&tx, &ty); /* physical panel coords -> logical (rotated) app coords */
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

#if ER_HOTRELOAD
        /* Load a freshly received container, if one is ready, on THIS task (the runtime owner) so the
           reset/load never races the RX task. The old app keeps running until this swaps it in — no
           teardown beforehand. No-op on frames with nothing pending. */
        er_hotreload_usb_pump();
#endif

        const int64_t frame_start_us = esp_timer_get_time();
        er_runtime_pump(); /* drain JS promises + fire due timers */
        const int64_t t_pump_us = esp_timer_get_time();
        er_commit();       /* lay out (if needed) + paint the changed region into the backend */
        const int64_t t_commit_us = esp_timer_get_time();
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
        const int64_t t_present_us = esp_timer_get_time();

        const uint32_t now = now_ms();
        embedded_renderer_tick(now - prev);
        prev = now;

        /* Frame-phase trace: where the frame time goes, averaged over the log window. */
        static uint32_t s_tr_pump = 0, s_tr_commit = 0, s_tr_present = 0, s_tr_n = 0;
        s_tr_pump += (uint32_t)(t_pump_us - frame_start_us);
        s_tr_commit += (uint32_t)(t_commit_us - t_pump_us);
        s_tr_present += (uint32_t)(t_present_us - t_commit_us);
        s_tr_n++;

        if ((++frame % 30U) == 0U)
        {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
            static int s_pie_diag_logged = 0;
            if (!s_pie_diag_logged && !er_esp32_lcd_pie_enabled())
            {
                extern const char* er_pie_diag(void);
                const char* diag = er_pie_diag();
                if (diag[0] != '\0')
                {
                    ESP_LOGW(TAG, "PIE self-test diag: %s", diag);
                }
                s_pie_diag_logged = 1;
            }
#endif
            ESP_LOGI(TAG,
                     "alive: %u frames, %u ms uptime | avg us/frame: pump=%u commit=%u present=%u | display=%d "
                     "pie=%d int_free=%u",
                     (unsigned)frame,
                     (unsigned)now,
                     (unsigned)(s_tr_pump / s_tr_n),
                     (unsigned)(s_tr_commit / s_tr_n),
                     (unsigned)(s_tr_present / s_tr_n),
                     (int)display,
                     (int)er_esp32_lcd_pie_enabled(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_tr_pump = s_tr_commit = s_tr_present = s_tr_n = 0;
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
