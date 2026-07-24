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

#ifndef EMBEDDED_REACT_PICO_SPI_LCD_BACKEND_H
#define EMBEDDED_REACT_PICO_SPI_LCD_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief The panel operations the board supplies to the backend.
     *
     * The backend is deliberately platform-neutral C99 (built for the RP2040 / pico-sdk examples, but
     * nothing in it is pico-specific): it owns the framebuffer, compositing, and dirty tracking, and
     * hands finished pixel runs to these two callbacks. The board owns the panel: SPI bus, GPIOs, the
     * controller init sequence (GC9A01A, ST7789, ...), orientation, and backlight.
     */
    typedef struct
    {
        /**
         * @brief Opens a write window on the panel (CASET/RASET + RAMWR on MIPI-DCS controllers).
         *
         * Coordinates are inclusive pixel bounds. After this call the panel must be ready to accept
         * (x1-x0+1) * (y1-y0+1) pixels via write_pixels().
         */
        void (*set_window)(int x0, int y0, int x1, int y1);

        /**
         * @brief Streams @p count RGB565 pixels (already in PANEL BYTE ORDER — the backend stores the
         * framebuffer big-endian, so the buffer bytes go straight onto the wire) into the open window.
         */
        void (*write_pixels)(const uint16_t* px, size_t count);
    } ErPicoLcdPanelOps;

    /**
     * @brief Initialises the SPI-LCD render backend and registers it with the engine.
     *
     * Keeps ONE canonical RGB565 framebuffer in SRAM (width * height * 2 bytes — 112.5 KB at 240x240,
     * comfortably inside the RP2040's 264 KB) that the engine's fill/copy/blend callbacks composite
     * into at full 8-bit precision, tracks the dirty bounding box, and on present pushes exactly the
     * dirty rect through the board's panel ops. Pixels are stored byte-swapped (big-endian RGB565, the
     * order SPI panels expect MSB-first), so present() writes rows straight from the framebuffer with
     * no staging buffer.
     *
     * @param[in] ops     Panel ops (copied; the struct needn't outlive the call).
     * @param[in] width   Framebuffer / panel width in pixels.
     * @param[in] height  Framebuffer / panel height in pixels.
     *
     * @return true on success; false if the framebuffer allocation failed or args are invalid.
     */
    bool er_pico_spi_lcd_backend_init(const ErPicoLcdPanelOps* ops, int width, int height);

    /**
     * @brief Flushes the dirty rect of the framebuffer to the panel.
     *
     * Call once per frame after er_commit(). Opens one window over the dirty bounding box and streams
     * it row by row (one contiguous write when the box spans the full width). No-op if nothing changed
     * since the last present; the first present pushes the whole frame.
     */
    void er_pico_spi_lcd_present(void);

#ifdef __cplusplus
}
#endif

#endif
