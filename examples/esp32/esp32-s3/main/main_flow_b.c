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

#if ER_HOTRELOAD
#include "driver/usb_serial_jtag.h"
#include "erb_package_reload.h"
#endif

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

#define US_PER_MS 1000

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
    return (uint32_t)(esp_timer_get_time() / US_PER_MS);
}

#if ER_HOTRELOAD
/* The native USB port carries the logs out and the reload in, as it does in the Flow A host (hotreload_usb.c). */
#define RELOAD_CHUNK_BYTES 4096U
#define RELOAD_USB_RX_BYTES 8192U
#define RELOAD_USB_TX_BYTES 4096U
#define RELOAD_READ_BYTES 512

/* The RGB panel can't keep scanning through a flash write and glitches, so the backlight stays off from the first write
 * until this long after the last, by when the app has drawn. */
#define RELOAD_DARK_AFTER_WRITE_MS 250U

static ErbPackageReload s_reload;
static uint32_t s_reload_buffer[ERB_PACKAGE_RELOAD_BUFFER_BYTES(RELOAD_CHUNK_BYTES) / sizeof(uint32_t)];
static ErbPackageSlots s_flash_slots;
static bool s_panel_dark;
static uint32_t s_last_write_ms;

/** @brief Turns the backlight off ahead of a flash write. */
static void panel_go_dark(void)
{
    if (!s_panel_dark)
    {
        board_display_set_backlight(false);
        s_panel_dark = true;
    }
}

static bool dark_erase(unsigned slot, uint32_t offset, uint32_t bytes, void* context)
{
    panel_go_dark();
    const bool erased = s_flash_slots.erase(slot, offset, bytes, context);
    s_last_write_ms = now_ms();
    return erased;
}

static bool dark_program(unsigned slot, uint32_t offset, const void* data, uint32_t bytes, void* context)
{
    panel_go_dark();
    const bool programmed = s_flash_slots.program(slot, offset, data, bytes, context);
    s_last_write_ms = now_ms();
    return programmed;
}

/** @brief Prints one line to the host. */
static void reload_log(const char* line, void* context)
{
    (void)context;
    ESP_LOGI(TAG, "%s", line);
}

/** @brief Hands the receiver everything the USB port has. */
static void reload_poll(void)
{
    uint8_t bytes[RELOAD_READ_BYTES];
    for (;;)
    {
        const int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0);
        if (count <= 0)
        {
            break;
        }
        erb_package_reload_feed(&s_reload, bytes, (size_t)count);
    }
    if (s_panel_dark && now_ms() - s_last_write_ms >= RELOAD_DARK_AFTER_WRITE_MS)
    {
        board_display_set_backlight(true);
        s_panel_dark = false;
    }
}
#endif

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
        .screen_width = SCREEN_WIDTH,
        .screen_height = SCREEN_HEIGHT,
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
#if ER_HOTRELOAD
    s_flash_slots = slots;
    ErbPackageSlots reload_slots = slots;
    reload_slots.erase = dark_erase;
    reload_slots.program = dark_program;
    erb_package_reload_init(
        &s_reload, &reload_slots, &config, &boot, s_reload_buffer, RELOAD_CHUNK_BYTES, reload_log, NULL);
    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = RELOAD_USB_RX_BYTES,
        .tx_buffer_size = RELOAD_USB_TX_BYTES,
    };
    /* The console may have installed the driver already, which is just as good. */
    const esp_err_t error = usb_serial_jtag_driver_install(&usb_config);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "hot reload disabled: %s", esp_err_to_name(error));
    }
#endif
}

/** @brief Boots the panel + engine, loads the package, then drives the frame loop. */
static void run_app(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    const bool panel_up = board_display_init(&panel);
    if (!panel_up || !er_esp32_lcd_backend_init(panel, SCREEN_WIDTH, SCREEN_HEIGHT, ER_DISPLAY_ROTATION))
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
    uint32_t previous_ms = boot_ms;
    bool touch_down = false;
    int last_touch_x = 0;
    int last_touch_y = 0;
    while (true)
    {
        const int64_t frame_start_us = esp_timer_get_time();

        if (touch)
        {
            int touch_x = 0;
            int touch_y = 0;
            bool pressed = false;
            if (board_touch_read(&touch_x, &touch_y, &pressed))
            {
                remap_touch(&touch_x, &touch_y);
                if (pressed)
                {
                    embedded_renderer_touch(0, touch_down ? ER_TOUCH_MOVE : ER_TOUCH_DOWN, touch_x, touch_y);
                    touch_down = true;
                    last_touch_x = touch_x;
                    last_touch_y = touch_y;
                }
                else if (touch_down)
                {
                    embedded_renderer_touch(0, ER_TOUCH_UP, last_touch_x, last_touch_y);
                    touch_down = false;
#if ER_HOTRELOAD
                    erb_package_reload_tap(&s_reload);
#endif
                }
            }
        }

        er_commit();
        er_esp32_lcd_present();

        const uint32_t now = now_ms();
        const uint32_t delta_ms = now - previous_ms;
        embedded_renderer_tick(delta_ms);
        erb_tick(delta_ms);
        previous_ms = now;

#if ER_HOTRELOAD
        reload_poll();
#endif

        if (s_boot_report[0] && now - boot_ms >= BOOT_REPORT_AFTER_MS)
        {
            ESP_LOGI(TAG, "%s", s_boot_report);
            s_boot_report[0] = '\0';
        }

#ifdef ER_BOARD_CRASH_TEST
        crash_test(now - boot_ms);
#endif

        /* Adaptive pacing: sleep the remainder up to the target; always yield >=1 tick so the idle task is fed. */
        const uint32_t used_ms = (uint32_t)((esp_timer_get_time() - frame_start_us) / US_PER_MS);
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
