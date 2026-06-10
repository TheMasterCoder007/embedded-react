#ifndef EMBEDDED_REACT_ESP32_SPI_LCD_BACKEND_H
#define EMBEDDED_REACT_ESP32_SPI_LCD_BACKEND_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialises the lean SPI-LCD render backend (for no-PSRAM ESP32 boards) and registers it.
     *
     * The internal-RAM counterpart to backends/esp32-lcd: it keeps a single RGB565 "canonical"
     * framebuffer in INTERNAL DRAM (no PSRAM, no double-buffering, no rotation, no overlay) that the
     * engine's fill/copy/blend composite into, tracks the dirty bounding box, and on present pushes the
     * changed rows to the panel as FULL-WIDTH bands — so the source rows are contiguous in the
     * framebuffer and no staging buffer is needed. RAM cost is just one framebuffer: width*height*2
     * bytes (e.g. 240x320 = 150 KB), which fits an ESP32-WROOM-32's internal RAM.
     *
     * The caller owns the panel: initialise it first (SPI bus, ST7789/ILI9341 panel, reset, color
     * inversion, MADCTL/orientation, backlight) and keep it alive for the backend's lifetime. Physical
     * orientation is the panel's job (esp_lcd_panel_swap_xy / mirror) — this backend always renders the
     * logical buffer 1:1 to the panel (no software rotation), so pass the size the panel presents.
     *
     * Built for the DIYmall ESP32-2432S028R "Cheap Yellow Display" (ST7789 SPI, 240x320), but
     * panel-agnostic — any esp_lcd RGB565 panel handle works.
     *
     * @param[in] panel   An initialised esp_lcd panel handle (e.g. an ST7789 SPI panel).
     * @param[in] io      The panel's IO handle (for the color-transfer-done callback; draw_bitmap is
     *                    async, so present() waits on it before reusing the bounce buffer).
     * @param[in] width   Framebuffer / panel width in pixels.
     * @param[in] height  Framebuffer / panel height in pixels.
     *
     * @return true on success; false on a framebuffer allocation failure (not enough internal RAM).
     */
    bool er_esp32_spi_lcd_backend_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, int width, int height);

    /**
     * @brief Flushes the painted (dirty) rows of the framebuffer to the panel.
     *
     * Call once per frame after er_commit(). Pushes the dirty bounding box as full-width bands via
     * esp_lcd_panel_draw_bitmap (blocking SPI). No-op if nothing changed since the last present.
     */
    void er_esp32_spi_lcd_present(void);

#ifdef __cplusplus
}
#endif

#endif
