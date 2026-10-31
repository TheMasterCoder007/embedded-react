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
 * embedded-react — ESP32-S3 host, the Flow B build (sdkconfig.defaults.flow-b).
 *
 * The same panel and touch as the Flow A host in main.c, with no QuickJS: it reserves an arena, loads the active
 * package from its flash slot at the panel's size, and runs the frame loop. The package is written separately from
 * the firmware (`embedded-react flash`), so changing the UI needs no firmware rebuild.
 *
 * See ../README.md.
 */

#include "board.h"
#include "er_scene.h"
#include "erb_package_slots_esp32.h"
#include "erb_runtime.h"
#include "esp32_lcd_backend.h"
#include "host_common.h"
#include "native_renderer.h"

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>

static const char* TAG = "embedded-react";

/** @brief The app's mutable state, in fast internal RAM since the VM's registers live here. */
#ifndef ERB_ARENA_BYTES
#define ERB_ARENA_BYTES (32 * 1024)
#endif

static uint32_t s_arena[ERB_ARENA_BYTES / sizeof(uint32_t)];

/** @brief Room for one line of erb_error_message(). */
#define ERROR_MESSAGE_BYTES 160

/* Bring-up flips the CH422G's USB_SEL mux, and the native-USB console re-enumerates for about a second: the window
 * the package loads in, so what happened is said again from the frame loop, once the console is back. */
#define BOOT_REPORT_AFTER_MS 3000U
#define BOOT_REPORT_BYTES (ERROR_MESSAGE_BYTES + 64)
#define DISPLAY_FAILED_REPEAT_MS 2000U

static char s_boot_report[BOOT_REPORT_BYTES];

/* A forced failure, to check the error screen on hardware: `idf.py -DER_BOARD_CRASH_TEST=1` panics 5 s after boot,
 * and =2 hangs, which the task watchdog turns into a reset. Each shows the error screen after the reset, naming it,
 * then the app comes back and it happens again. */
#ifdef ER_BOARD_CRASH_TEST
#define CRASH_TEST_AFTER_MS 5000U
#define CRASH_TEST_PANIC 1
#define CRASH_TEST_HANG 2

/** @brief Fails the way ER_BOARD_CRASH_TEST asks, once the time has come. */
static void crash_test(uint32_t since_boot_ms)
{
    if (since_boot_ms < CRASH_TEST_AFTER_MS)
    {
        return;
    }
#if ER_BOARD_CRASH_TEST == CRASH_TEST_PANIC
    *(volatile uint32_t*)0u = 0u;
#elif ER_BOARD_CRASH_TEST == CRASH_TEST_HANG
    for (;;)
    {
    }
#endif
}
#endif

/** @brief Returns milliseconds since boot (engine-clock delta source). */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Loads the active package at the panel's size. With none that loads, the error screen stays up and the
 *        frame loop keeps running under it.
 *
 * @param[in] touch  Whether touch came up.
 */
static void load_package(bool touch)
{
    ErbPackageSlots slots;
    erb_package_slots_esp32_open(&slots);

    const ErbConfig config = {
        .arena = s_arena,
        .arena_bytes = sizeof(s_arena),
        .screen_w = SCREEN_W,
        .screen_h = SCREEN_H,
    };
    ErbPackageSlotBoot boot;
    const bool running = erb_package_slots_boot(&slots, &config, &boot);
    char message[ERROR_MESSAGE_BYTES] = "";
    if (boot.error.status != ERB_OK)
    {
        erb_error_message(&boot.error, message, sizeof(message));
        erb_package_slots_show_error(&boot);
    }
    if (running)
    {
        snprintf(s_boot_report,
                 sizeof(s_boot_report),
                 "package running from slot %c; slot %c is staging%s%s%s",
                 'a' + boot.active,
                 'a' + boot.staging,
                 message[0] ? "; rejected the newest: " : "",
                 message,
                 boot.clear_failed ? "; couldn't clear its header" : "");
        if (boot.error.status != ERB_OK)
        {
            hold_error_screen(touch);
        }
    }
    else
    {
        snprintf(s_boot_report, sizeof(s_boot_report), "package not loaded: %s", message);
    }
}

/** @brief Boots the panel + engine, loads the package, then drives the frame loop. */
static void run_app(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    const bool panel_up = board_display_init(&panel);
    if (!panel_up || !er_esp32_lcd_backend_init(panel, SCREEN_W, SCREEN_H, ER_DISPLAY_ROTATION))
    {
        /* Say so until someone reads the log: the console may not be back yet. */
        for (;;)
        {
            ESP_LOGE(
                TAG, "display init failed: %s", panel_up ? "the render backend failed" : board_display_last_error());
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_FAILED_REPEAT_MS));
        }
    }
#if ERUI_RENDER_WORKERS > 1
    install_render_workers(); /* after display init, so its heap needs come first */
#endif

    const bool touch = board_touch_init();
    if (!touch)
    {
        ESP_LOGW(TAG, "touch init failed — UI renders but input is disabled");
    }

    show_reset_fault(touch);
    load_package(touch);

    /* Frame loop. The press state machine turns GT911 snapshots into down/move/up for the engine. */
    const uint32_t boot_ms = now_ms();
    uint32_t prev = boot_ms;
    bool touch_down = false;
    int touch_x = 0;
    int touch_y = 0;
    while (true)
    {
        const int64_t frame_start_us = esp_timer_get_time();

        if (touch)
        {
            int tx = 0;
            int ty = 0;
            bool pressed = false;
            if (board_touch_read(&tx, &ty, &pressed))
            {
                remap_touch(&tx, &ty);
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

        er_commit();
        er_esp32_lcd_present();

        const uint32_t now = now_ms();
        const uint32_t dt = now - prev;
        embedded_renderer_tick(dt);
        erb_tick(dt);
        prev = now;

        if (s_boot_report[0] && now - boot_ms >= BOOT_REPORT_AFTER_MS)
        {
            ESP_LOGI(TAG, "%s", s_boot_report);
            s_boot_report[0] = '\0';
        }

#ifdef ER_BOARD_CRASH_TEST
        crash_test(now - boot_ms);
#endif

        /* Adaptive pacing: sleep the remainder up to the target; always yield >=1 tick so the idle task is fed. */
        const uint32_t used_ms = (uint32_t)((esp_timer_get_time() - frame_start_us) / 1000);
        const uint32_t sleep_ms = (used_ms >= ER_TARGET_FRAME_MS) ? 0U : (ER_TARGET_FRAME_MS - used_ms);
        TickType_t ticks = pdMS_TO_TICKS(sleep_ms);
        if (ticks == 0U)
        {
            ticks = 1U;
        }
        vTaskDelay(ticks);
    }
}

/** @brief IDF entry point. */
void app_main(void)
{
    ESP_LOGI(TAG, "embedded-react ESP32-S3 host — Flow B (no QuickJS)");
    run_app();
}
