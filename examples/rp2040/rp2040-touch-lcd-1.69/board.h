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

#include "pico_spi_lcd_backend.h"

#include <stdbool.h>
#include <stdint.h>

/* Waveshare RP2040-Touch-LCD-1.69 — 1.69" rounded-rectangle 240x280 ST7789V2 SPI panel. */
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 280

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Brings up the ST7789V2 SPI panel (SPI1) + PWM backlight.
     *
     * Inits the display SPI bus at 40 MHz, runs the ST7789V2 register init sequence (the vendor's
     * reference values: MADCTL 0x00 / RGB portrait, 16-bit color, inversion ON), clears GRAM, and
     * ramps the backlight. Pass the ops from board_lcd_ops() to er_pico_spi_lcd_backend_init().
     * The 240x280 visible area sits at a +20-row offset in the controller's 240x320 GRAM; the
     * window ops below apply it, so callers address (0,0)..(239,279).
     *
     * @return true on success (SPI init is infallible on this board — reserved for future checks).
     */
    bool board_display_init(void);

    /**
     * @brief The panel ops (set_window + write_pixels) for the render backend.
     *
     * Valid after board_display_init().
     */
    const ErPicoLcdPanelOps* board_lcd_ops(void);

    /**
     * @brief Sets the backlight brightness, 0–100 (PWM on GP25).
     */
    void board_backlight(int percent);

    /**
     * @brief Bring-up aid: draws four 60-row color bands (R/G/B/W) straight through the panel ops.
     *
     * Bypasses the engine and framebuffer entirely — clean bands prove the panel driver end to end.
     */
    void board_lcd_test_pattern(void);

    /**
     * @brief Brings up the CST816S capacitive touch controller (I2C1, polled).
     *
     * Hardware-resets the chip, verifies its chip ID, and disables auto-sleep so polling keeps
     * working when the screen is idle.
     *
     * @return true on success; false if the chip didn't answer (logged).
     */
    bool board_touch_init(void);

    /**
     * @brief Polls the CST816S for the latest touch state.
     *
     * @param[out] x        Receives the touch X in screen pixels [0..BOARD_LCD_WIDTH) when pressed.
     * @param[out] y        Receives the touch Y in screen pixels [0..BOARD_LCD_HEIGHT) when pressed.
     * @param[out] pressed  true while a finger is in contact, false otherwise.
     *
     * @return true on a successful poll; false on an I2C error (treat as "not pressed").
     */
    bool board_touch_read(int* x, int* y, bool* pressed);

    /**
     * @brief Brings up the QMI8658 IMU on the shared I2C1 bus for accelerometer-based sensing.
     *
     * Probes the two possible addresses (0x6A/0x6B), verifies WHO_AM_I, and configures the
     * accelerometer (±4g @125 Hz; gyro left off — unused). Call after board_touch_init() (inits I2C1).
     *
     * @return true on success; false if the chip didn't answer (steps + level are then disabled).
     */
    bool board_imu_init(void);

    /**
     * @brief Reads the latest raw accelerometer sample (little-endian int16 per axis, ±4g full-scale).
     *
     * @param[out] ax  Receives X acceleration (LSB; 1 g ≈ 8192).
     * @param[out] ay  Receives Y acceleration.
     * @param[out] az  Receives Z acceleration.
     *
     * @return true on a successful read; false if the IMU is absent or the I2C read failed.
     */
    bool board_imu_read_accel(int16_t* ax, int16_t* ay, int16_t* az);

#ifdef __cplusplus
}
#endif

#endif
