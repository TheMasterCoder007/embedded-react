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

    /**
     * @brief Brings up the GT911 capacitive touch controller on the shared I2C bus.
     *
     * Must be called after board_display_init() (which creates the I2C bus + CH422G). Runs the GT911
     * reset sequence (TP_RST on CH422G EXIO1, INT on GPIO4) to latch the I2C address, then probes the
     * controller. Single-touch is all the engine needs.
     *
     * @return true on success; false if the controller did not respond (logged).
     */
    bool board_touch_init(void);

    /**
     * @brief Polls the GT911 for the latest touch state.
     *
     * Reads the controller's buffer-status register; only reports when a fresh sample is ready (so the
     * caller's press/release state machine holds between updates). Coordinates are in panel pixels.
     *
     * @param[out] x        Receives the touch X (panel pixels) when pressed.
     * @param[out] y        Receives the touch Y (panel pixels) when pressed.
     * @param[out] pressed  Receives true if a finger is in contact, false on release.
     *
     * @return true if a fresh sample was read (x/y/pressed valid); false if no new data this poll.
     */
    bool board_touch_read(int* x, int* y, bool* pressed);

#ifdef __cplusplus
}
#endif

#endif
