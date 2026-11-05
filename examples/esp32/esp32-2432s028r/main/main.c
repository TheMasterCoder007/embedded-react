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
 * embedded-react — ESP32-2432S028R "Cheap Yellow Display" host (Flow B).
 *
 * The no-PSRAM peer of the ESP32-S3 example: NO QuickJS and no JS at runtime — exactly what fits a plain
 * ESP32-WROOM-32's internal RAM. It brings up the CYD's ST7789 SPI panel + XPT2046 touch (board.c), registers
 * the lean SPI render backend, loads the active package from its flash slot, and runs the frame loop: poll
 * touch, pump animations, commit, present, tick. The package is written separately from the firmware
 * (`embedded-react flash`), so changing the UI needs no firmware rebuild.
 *
 * See ../README.md.
 */

#include "board.h"
#include "er_error_screen.h"
#include "er_scene.h"
#include "erb_package_slots_esp32.h"
#include "erb_runtime.h"
#include "esp32_spi_lcd_backend.h"
#include "native_renderer.h"

#if ER_HOTRELOAD
#include "driver/uart.h"
#include "erb_package_reload.h"
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static const char* TAG = "embedded-react";

/** @brief The app's mutable state. A package states what it needs, and the thermostat takes under 1 KB. */
#ifndef ERB_ARENA_BYTES
#define ERB_ARENA_BYTES (2 * 1024)
#endif

static uint32_t s_arena[ERB_ARENA_BYTES / sizeof(uint32_t)];

/** @brief Longest a rejected package's reason stays on screen before the last good one runs; a tap ends it sooner. */
#define REJECTED_HOLD_MS 10000U

/** @brief Room for one line of erb_error_message(). */
#define ERROR_MESSAGE_BYTES 160

/** @brief Target frame period for the adaptive pacer (~60 fps); heavy frames just run as fast as work allows.
 *  Also sets the touch sample rate (one poll per frame), so a smaller value = finer drag tracking. */
#define ER_TARGET_FRAME_MS 16U

#define US_PER_MS 1000

#if ER_HOTRELOAD
/* The UART moves to the host's speed once the board is up, so the console reads at this rate from then on. Its RX ring
 * only has to hold a frame: the host waits for each reply, so nothing arrives while flash is being written. */
#define RELOAD_UART UART_NUM_0
#define RELOAD_UART_BAUD 460800
#define RELOAD_CHUNK_BYTES 1024U
#define RELOAD_UART_RX_BYTES 2048
#define RELOAD_READ_BYTES 128

static ErbPackageReload s_reload;
static uint32_t s_reload_buffer[ERB_PACKAGE_RELOAD_BUFFER_BYTES(RELOAD_CHUNK_BYTES) / sizeof(uint32_t)];

/** @brief Prints one line to the host. */
static void reload_log(const char* line, void* context)
{
    (void)context;
    ESP_LOGI(TAG, "%s", line);
}

/** @brief Hands the receiver everything the UART has. */
static void reload_poll(void)
{
    uint8_t bytes[RELOAD_READ_BYTES];
    for (;;)
    {
        const int count = uart_read_bytes(RELOAD_UART, bytes, sizeof(bytes), 0);
        if (count <= 0)
        {
            return;
        }
        erb_package_reload_feed(&s_reload, bytes, (size_t)count);
    }
}
#endif

/** @brief Returns milliseconds since boot (engine-clock delta source). */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / US_PER_MS);
}

/**
 * @brief Keeps the error screen up until the panel is tapped or REJECTED_HOLD_MS passes, then takes it down.
 *
 * @param[in] touch  Whether touch came up, so a tap can end it.
 */
static void hold_error_screen(bool touch)
{
    for (uint32_t waited = 0; waited < REJECTED_HOLD_MS; waited += ER_TARGET_FRAME_MS)
    {
        er_commit();
        er_esp32_spi_lcd_present();
        int touch_x = 0;
        int touch_y = 0;
        bool pressed = false;
        if (touch && board_touch_read(&touch_x, &touch_y, &pressed) && pressed)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ER_TARGET_FRAME_MS));
    }
    er_error_screen_hide();
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
        .screen_width = BOARD_LCD_WIDTH,
        .screen_height = BOARD_LCD_HEIGHT,
    };
    ErbPackageSlotBoot boot;
    const bool running = erb_package_slots_boot(&slots, &config, &boot);
    if (boot.error.status != ERB_OK)
    {
        char message[ERROR_MESSAGE_BYTES];
        erb_error_message(&boot.error, message, sizeof(message));
        ESP_LOGE(TAG, "package not loaded: %s", message);
        erb_package_slots_show_error(&boot);
    }
    if (running)
    {
        ESP_LOGI(TAG, "package running from slot %c; slot %c is staging", 'a' + boot.active, 'a' + boot.staging);
        if (boot.clear_failed)
        {
            ESP_LOGE(TAG, "couldn't clear the rejected package's header: the next flash may replace the last good one");
        }
        if (boot.error.status != ERB_OK)
        {
            hold_error_screen(touch);
        }
    }
#if ER_HOTRELOAD
    erb_package_reload_init(&s_reload, &slots, &config, &boot, s_reload_buffer, RELOAD_CHUNK_BYTES, reload_log, NULL);
    if (uart_driver_install(RELOAD_UART, RELOAD_UART_RX_BYTES, 0, 0, NULL, 0) == ESP_OK
        && uart_set_baudrate(RELOAD_UART, RELOAD_UART_BAUD) == ESP_OK)
    {
        ESP_LOGI(TAG, "on-device hot reload ready, at %d baud", RELOAD_UART_BAUD);
    }
    else
    {
        ESP_LOGE(TAG, "hot reload disabled: the UART driver didn't start");
    }
#endif
}

/** @brief Boots the panel + engine, loads the package, then drives the frame loop. */
static void run_app(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (!board_display_init(&panel, &io))
    {
        ESP_LOGE(TAG, "display init failed — halting");
        return;
    }
    if (!er_esp32_spi_lcd_backend_init(panel, io, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT))
    {
        ESP_LOGE(TAG, "render backend init failed (out of internal RAM?) — halting");
        return;
    }
    const bool touch = board_touch_init();
    if (!touch)
    {
        ESP_LOGW(TAG, "touch init failed — UI renders but input is disabled");
    }

    load_package(touch);

    /* The software keyboard is compiled in via ERUI_ONSCREEN_KEYBOARD=1 (components/engine/CMakeLists.txt)
     * and auto-shows whenever a TextInput is focused — no runtime call needed. Customise its layout/colours
     * with er_keyboard_set_config(&config) here if desired. */
    ESP_LOGI(TAG, "free internal RAM after boot: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Frame loop. The press state machine turns XPT2046 polls into down/move/up for the engine. */
    uint32_t previous_ms = now_ms();
    bool touch_down = false;
    int last_touch_x = 0, last_touch_y = 0;
    while (true)
    {
        const int64_t frame_start_us = esp_timer_get_time();

        if (touch)
        {
            int touch_x = 0, touch_y = 0;
            bool pressed = false;
            if (board_touch_read(&touch_x, &touch_y, &pressed))
            {
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
        er_esp32_spi_lcd_present();

        const uint32_t now = now_ms();
        const uint32_t delta_ms = now - previous_ms;
        embedded_renderer_tick(delta_ms);
        erb_tick(delta_ms); /* advance the app's timers (setInterval/setTimeout); no-op if the app has none */
        previous_ms = now;

#if ER_HOTRELOAD
        reload_poll();
#endif

        /* Adaptive pacing: sleep the remainder up to the target; always yield >=1 tick for the idle task. */
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
    ESP_LOGI(TAG, "embedded-react ESP32-2432S028R (CYD) host — Flow B (no QuickJS)");
    ESP_LOGI(TAG, "free internal RAM: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    run_app();
}
