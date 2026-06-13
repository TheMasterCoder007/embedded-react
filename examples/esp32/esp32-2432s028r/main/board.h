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

#ifndef BOARD_H
#define BOARD_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include <stdbool.h>

/* DIYmall ESP32-2432S028R "Cheap Yellow Display" — 2.8" 240x320 SPI panel (portrait native). */
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 320

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Brings up the CYD's ST7789 SPI panel (HSPI) + backlight and returns its esp_lcd handle.
     *
     * Inits the display SPI bus, the ST7789 panel (reset, init, color inversion ON — the standard CYD
     * fix), and turns on the backlight (GPIO 21). Pass the returned handle to
     * er_esp32_spi_lcd_backend_init(). 240x320 portrait, rendered 1:1 (no rotation).
     *
     * @param[out] out_panel  Receives the initialised panel handle on success.
     * @param[out] out_io     Receives the panel IO handle (for the backend's transfer-done callback).
     *
     * @return true on success; false on any init failure (logged).
     */
    bool board_display_init(esp_lcd_panel_handle_t* out_panel, esp_lcd_panel_io_handle_t* out_io);

    /**
     * @brief Brings up the XPT2046 resistive touch controller on its own SPI bus (VSPI).
     *
     * @return true on success; false on a bus/device init failure (logged).
     */
    bool board_touch_init(void);

    /**
     * @brief Polls the XPT2046 for the latest touch state (PENIRQ-gated, averaged, calibrated).
     *
     * @param[out] x        Receives the touch X in screen pixels [0..BOARD_LCD_WIDTH) when pressed.
     * @param[out] y        Receives the touch Y in screen pixels [0..BOARD_LCD_HEIGHT) when pressed.
     * @param[out] pressed  true while a finger is in contact, false on release.
     *
     * @return true (x/y/pressed always valid; resistive touch is polled, not buffered).
     */
    bool board_touch_read(int* x, int* y, bool* pressed);

#ifdef __cplusplus
}
#endif

#endif
