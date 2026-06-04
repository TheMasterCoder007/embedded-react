#ifndef BOARD_H
#define BOARD_H

#include "esp_lcd_panel_ops.h"

#include <stdbool.h>

/* Waveshare ESP32-S3-Touch-LCD-7 panel resolution. */
#define BOARD_LCD_WIDTH 800
#define BOARD_LCD_HEIGHT 480

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Brings up the Waveshare 7" RGB panel and returns its esp_lcd handle.
     *
     * Inits I2C + the CH422G IO expander (releases LCD reset, enables the DISP/backlight line), then
     * creates and initialises the 800x480 RGB panel (pins/timings per the board). The framebuffer
     * lives in PSRAM. Pass the returned handle to er_esp32_lcd_backend_init().
     *
     * @param[out] out_panel  Receives the initialised panel handle on success.
     *
     * @return true on success; false on any init failure (logged).
     */
    bool board_display_init(esp_lcd_panel_handle_t* out_panel);

#ifdef __cplusplus
}
#endif

#endif
