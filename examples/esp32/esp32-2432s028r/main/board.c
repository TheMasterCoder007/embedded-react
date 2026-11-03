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
 * DIYmall ESP32-2432S028R "Cheap Yellow Display" (CYD) v3 bring-up.
 *
 * Board-specific: the 240x320 ST7789 SPI panel (HSPI) + backlight, and the XPT2046 resistive touch
 * controller on a SEPARATE SPI bus (VSPI). Pin map from the community CYD references (randomnerdtutorials
 * / mischianti / witnessmenow's ESP32-Cheap-Yellow-Display). This is the v3 board (two USB ports:
 * micro + USB-C) which uses ST7789 — the v1/v2 (single micro-USB) use ILI9341 instead (swap the panel
 * constructor below if you have that one).
 *
 * TUNING (verify on your unit):
 *   - Colors look inverted (photo-negative)? toggle BOARD_LCD_INVERT.
 *   - Red/blue swapped? toggle BOARD_LCD_BGR.
 *   - Image mirrored / wrong orientation? adjust the swap_xy / mirror calls in board_display_init.
 *   - Touch maps to the wrong place? tune the TOUCH_* calibration constants (and the axis swap/flip).
 */

#include "board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "board";

/** @brief An SPI or reset pin the board doesn't wire. */
#define PIN_UNUSED -1

/* --- Display: ST7789 on HSPI (SPI2_HOST) --- */
#define LCD_SPI_HOST SPI2_HOST
#define PIN_LCD_SCLK 14
#define PIN_LCD_MOSI 13
#define PIN_LCD_MISO PIN_UNUSED /* panel is write-only; GPIO12 (board MISO) left unclaimed */
#define PIN_LCD_CS 15
#define PIN_LCD_DC 2
#define PIN_LCD_RST PIN_UNUSED /* ST7789 reset is software (no dedicated GPIO) */
#define PIN_LCD_BACKLIGHT 21
#define LCD_PCLK_HZ (40 * 1000 * 1000) /* 40 MHz for snappy flushes; if this unit glitches, step down (26/30) */

#define BOARD_LCD_INVERT false /* this unit shows correct brightness without inversion */
#define BOARD_LCD_BGR false    /* set true if red/blue are swapped */

/* ST7789 bus setup: 8-bit commands and parameters, RGB565 pixels, and a DMA transfer big enough for ~80-row bands. */
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BITS_PER_PIXEL 16
#define LCD_TRANS_QUEUE_DEPTH 10
#define LCD_MAX_TRANSFER_ROWS 80

/* Orientation.
 *
 * swap_xy on its own is a REFLECTION, not a rotation — exchanging the axes flips handedness. Portrait
 * is correct at mirror (X=true, Y=false), so simply adding the swap leaves that reflection in place and
 * the whole image comes out mirrored. A true 90° turn is swap + EXACTLY ONE mirror flipped, which
 * leaves two valid settings: (true,true) and (false,false). They differ by 180°, i.e. they are the
 * clockwise and counter-clockwise quarter turns.
 *
 * The touch flips are not independent of that choice — the glass does not move, so which way the image
 * turned decides where a tap lands. Both therefore hang off ONE switch below so they cannot disagree:
 * if the display is rotated the wrong way round, flip BOARD_ROTATE_CW and the touch follows. */
#define BOARD_ROTATE_CW 1

#if BOARD_ROTATE_90
#define BOARD_LCD_MIRROR_X (BOARD_ROTATE_CW ? true : false)
#define BOARD_LCD_MIRROR_Y (BOARD_ROTATE_CW ? true : false)
#else
#define BOARD_LCD_MIRROR_X true /* this unit's panel scans X reversed (text was mirrored) */
#define BOARD_LCD_MIRROR_Y false
#endif

/* --- Touch: XPT2046 on VSPI (SPI3_HOST), separate bus --- */
#define TOUCH_SPI_HOST SPI3_HOST
#define PIN_TOUCH_SCLK 25
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39
#define PIN_TOUCH_CS 33
#define PIN_TOUCH_IRQ 36 /* PENIRQ: low while touched */
#define TOUCH_SPI_HZ (2 * 1000 * 1000)
#define TOUCH_MAX_TRANSFER_BYTES 32

/* XPT2046 reads: a command byte, then a 12-bit result in the top of the next two bytes. Differential mode. */
#define XPT2046_CMD_READ_X 0xD0
#define XPT2046_CMD_READ_Y 0x90
#define XPT2046_TRANSFER_BYTES 3
#define XPT2046_RESULT_SHIFT 4
#define BITS_PER_BYTE 8

/** @brief Readings averaged per touch poll, to settle the resistive noise. */
#define TOUCH_SAMPLES 4

/* XPT2046 raw → screen, calibrated for this unit by tapping the 4 corners of the DISPLAYED image (so
 * the display mirror is already accounted for). No axis swap or flip needed here. To recalibrate a
 * different unit: re-enable the raw-touch ESP_LOGI below, tap TL/TR/BR/BL, and set these min/max. */
#define TOUCH_X_MIN 170
#define TOUCH_X_MAX 1895
#define TOUCH_Y_MIN 100
#define TOUCH_Y_MAX 1852
/* The display is un-mirrored via panel mirror_x, but the resistive panel reports PHYSICAL coords, so
 * touch X ends up flipped relative to the app's layout (left/right buttons swap). Flip it back here.
 *
 * These are applied AFTER the rotation swap above, i.e. in SCREEN space. Rotating exchanges which raw
 * axis feeds which screen axis, so the portrait X-flip moves to screen Y in landscape. If a tap lands
 * mirrored on one axis on a real board, flip that one — they are independent of the display mirrors.
 *
 * Derived, not guessed. Portrait maps raw->screen as sx = 239 - u, sy = v (u,v being the raw axes
 * scaled to the screen). Rotating that image 90° CW sends a portrait point (px,py) to (319-py, px):
 *   sx = 319 - v  and  sy = 239 - u   ->  both axes flipped.
 * Counter-clockwise sends it to (py, 239-px):
 *   sx = v  and  sy = u               ->  neither flipped.
 * Anything else — one flipped, one not — is a mirror, which is what the first attempt here produced. */
#if BOARD_ROTATE_90
#define TOUCH_FLIP_X (BOARD_ROTATE_CW ? true : false)
#define TOUCH_FLIP_Y (BOARD_ROTATE_CW ? true : false)
#else
#define TOUCH_FLIP_X true
#define TOUCH_FLIP_Y false
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Display
 ---------------------------------------------------------------------------------------------------------------------*/

bool board_display_init(esp_lcd_panel_handle_t* out_panel, esp_lcd_panel_io_handle_t* out_io)
{
    /* Backlight on (plain GPIO high; swap for LEDC PWM if you want dimming). */
    gpio_config_t backlight_config = {.pin_bit_mask = 1ULL << PIN_LCD_BACKLIGHT, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&backlight_config);
    gpio_set_level(PIN_LCD_BACKLIGHT, 1);

    const spi_bus_config_t bus_config = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = PIN_UNUSED,
        .quadhd_io_num = PIN_UNUSED,
        .max_transfer_sz = BOARD_LCD_WIDTH * LCD_MAX_TRANSFER_ROWS * (int)sizeof(uint16_t),
    };
    if (spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK)
    {
        ESP_LOGE(TAG, "display SPI bus init failed");
        return false;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = LCD_TRANS_QUEUE_DEPTH,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io) != ESP_OK)
    {
        ESP_LOGE(TAG, "panel IO init failed");
        return false;
    }

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = BOARD_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_config, &panel) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed");
        return false;
    }

    if (esp_lcd_panel_reset(panel) != ESP_OK || esp_lcd_panel_init(panel) != ESP_OK)
    {
        ESP_LOGE(TAG, "panel reset/init failed");
        return false;
    }
    esp_lcd_panel_invert_color(panel, BOARD_LCD_INVERT);
    esp_lcd_panel_swap_xy(panel, BOARD_ROTATE_90 ? true : false);
    esp_lcd_panel_mirror(panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    ESP_LOGI(TAG,
             "ST7789 panel up: %dx%d (invert=%d, bgr=%d)",
             BOARD_LCD_WIDTH,
             BOARD_LCD_HEIGHT,
             BOARD_LCD_INVERT,
             BOARD_LCD_BGR);
    *out_panel = panel;
    *out_io = io;
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - XPT2046 resistive touch
 ---------------------------------------------------------------------------------------------------------------------*/

static spi_device_handle_t s_touch;

/** @brief Reads one 12-bit XPT2046 channel (XPT2046_CMD_READ_X or XPT2046_CMD_READ_Y), differential mode. */
static int xpt2046_read(uint8_t command)
{
    uint8_t transmit[XPT2046_TRANSFER_BYTES] = {command, 0, 0};
    uint8_t receive[XPT2046_TRANSFER_BYTES] = {0, 0, 0};
    spi_transaction_t transaction = {
        .length = XPT2046_TRANSFER_BYTES * BITS_PER_BYTE, .tx_buffer = transmit, .rx_buffer = receive};
    if (spi_device_polling_transmit(s_touch, &transaction) != ESP_OK)
    {
        return -1;
    }
    /* 12-bit result is in the upper bits of the two bytes following the command byte. */
    return ((receive[1] << 8) | receive[2]) >> XPT2046_RESULT_SHIFT;
}

bool board_touch_init(void)
{
    /* PENIRQ as input. GPIO 36 is an input-only pad with NO internal pull-up (the board has an
     * external one), so don't request GPIO_PULLUP_ENABLE — it errors. */
    gpio_config_t irq_config = {.pin_bit_mask = 1ULL << PIN_TOUCH_IRQ, .mode = GPIO_MODE_INPUT};
    gpio_config(&irq_config);

    const spi_bus_config_t bus_config = {
        .sclk_io_num = PIN_TOUCH_SCLK,
        .mosi_io_num = PIN_TOUCH_MOSI,
        .miso_io_num = PIN_TOUCH_MISO,
        .quadwp_io_num = PIN_UNUSED,
        .quadhd_io_num = PIN_UNUSED,
        .max_transfer_sz = TOUCH_MAX_TRANSFER_BYTES,
    };
    if (spi_bus_initialize(TOUCH_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK)
    {
        ESP_LOGE(TAG, "touch SPI bus init failed");
        return false;
    }
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = TOUCH_SPI_HZ, .mode = 0, .spics_io_num = PIN_TOUCH_CS, .queue_size = 1};
    if (spi_bus_add_device(TOUCH_SPI_HOST, &device_config, &s_touch) != ESP_OK)
    {
        ESP_LOGE(TAG, "XPT2046 add-device failed");
        return false;
    }
    ESP_LOGI(TAG, "XPT2046 touch ready");
    return true;
}

/** @brief Linearly maps a raw reading in [lo,hi] to a screen coordinate in [0, span), clamped. */
static int map_axis(int raw, int low, int high, int span)
{
    int value = (raw - low) * span / (high - low);
    if (value < 0)
        value = 0;
    if (value > span - 1)
        value = span - 1;
    return value;
}

bool board_touch_read(int* x, int* y, bool* pressed)
{
    if (gpio_get_level(PIN_TOUCH_IRQ) != 0)
    {
        *pressed = false; /* PENIRQ high → no contact */
        return true;
    }

    /* Average a few samples to settle the resistive noise. */
    int sum_x = 0, sum_y = 0, samples = 0;
    for (int sample = 0; sample < TOUCH_SAMPLES; sample++)
    {
        const int reading_x = xpt2046_read(XPT2046_CMD_READ_X);
        const int reading_y = xpt2046_read(XPT2046_CMD_READ_Y);
        if (reading_x >= 0 && reading_y >= 0)
        {
            sum_x += reading_x;
            sum_y += reading_y;
            samples++;
        }
    }
    if (samples == 0)
    {
        *pressed = false;
        return true;
    }
    const int raw_x = sum_x / samples;
    const int raw_y = sum_y / samples;
    /* ESP_LOGI(TAG, "raw touch %d,%d", rawx, rawy); // re-enable to recalibrate a different unit */

    /* Raw -> screen. The touch controller is bolted to the GLASS, so it keeps reporting in the panel's
       physical frame no matter how the display is rotated: when the image is rotated 90°, the raw axes
       must be exchanged here to follow it. The calibration range travels with its own raw axis, which
       is why raw_y is mapped against TOUCH_Y_* and lands on screen X. */
#if BOARD_ROTATE_90
    const int screen_x = map_axis(raw_y, TOUCH_Y_MIN, TOUCH_Y_MAX, BOARD_LCD_WIDTH);
    const int screen_y = map_axis(raw_x, TOUCH_X_MIN, TOUCH_X_MAX, BOARD_LCD_HEIGHT);
#else
    const int screen_x = map_axis(raw_x, TOUCH_X_MIN, TOUCH_X_MAX, BOARD_LCD_WIDTH);
    const int screen_y = map_axis(raw_y, TOUCH_Y_MIN, TOUCH_Y_MAX, BOARD_LCD_HEIGHT);
#endif
    *x = TOUCH_FLIP_X ? (BOARD_LCD_WIDTH - 1 - screen_x) : screen_x;
    *y = TOUCH_FLIP_Y ? (BOARD_LCD_HEIGHT - 1 - screen_y) : screen_y;
    *pressed = true;
    return true;
}
