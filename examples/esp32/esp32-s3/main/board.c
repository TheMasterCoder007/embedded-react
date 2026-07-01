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
 * Waveshare ESP32-S3-Touch-LCD-7 display bring-up.
 *
 * Board-specific: the 800x480 RGB panel pins/timings and the CH422G I2C IO-expander that drives the
 * LCD reset (EXIO3) and the DISP/backlight line (EXIO2). Pin map is from the Waveshare schematic
 * (docs.waveshare.com/ESP32-S3-Touch-LCD-7). RGB timings are the standard Waveshare-7" set (PCLK
 * ~21 MHz) — tune the porches here if the image is shifted/rolling.
 *
 * The CH422G is hand-driven (no maintained standalone IDF component exists): its "registers" are
 * distinct I2C addresses — WR_SET (0x24) sets the IO mode, WR_IO (0x38) latches the 8 output bits.
 * If the backlight stays off, the CH422G config byte / output mask below are the things to tune.
 */

#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "board";

/* --- I2C (shared bus for the CH422G + GT911 touch) --- */
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA 8
#define BOARD_I2C_SCL 9

/* --- CH422G (7-bit I2C addresses of its command registers) --- */
#define CH422G_ADDR_WR_SET 0x24 /* system setting: IO mode */
#define CH422G_ADDR_WR_IO 0x38  /* latch for output bits IO0..IO7 (= EXIO0..EXIO7) */
#define CH422G_MODE_OUTPUT 0x01 /* enable push-pull output on the IO pins */

/* CH422G output-bit assignments (Waveshare ESP32-S3-Touch-LCD-7). */
#define EXIO_TP_RST_BIT (1U << 1)  /* touch reset (GT911) */
#define EXIO_DISP_BL_BIT (1U << 2) /* DISP / backlight enable */
#define EXIO_LCD_RST_BIT (1U << 3) /* LCD reset (active low) */
#define EXIO_USB_SEL_BIT (1U << 5) /* USB_SEL: LOW = native USB (GPIO19/20 → main USB-C); HIGH = CAN */

/* --- GT911 capacitive touch (shares the I2C bus; INT=GPIO4, RST=CH422G EXIO1) --- */
#define BOARD_TOUCH_INT_GPIO 4 /* TP_IRQ — driven during reset to latch the I2C address, then input */
#define GT911_ADDR 0x5D        /* default address when INT is held low across the reset rising edge */
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS 0x814E /* bit7 = buffer ready, bits0..3 = number of touch points */
#define GT911_REG_POINT1 0x8150 /* first point coords: [xL, xH, yL, yH] (track id is the prior reg, 0x814F) */
#define GT911_STATUS_READY 0x80
#define GT911_POINT_COUNT_MASK 0x0F

static i2c_master_bus_handle_t s_i2c_bus;    /* shared bus (CH422G + GT911 touch) */
static i2c_master_dev_handle_t s_ch422g_io;  /* device at WR_IO  (0x38) */
static i2c_master_dev_handle_t s_ch422g_set; /* device at WR_SET (0x24) */
static uint8_t s_ch422g_latch = 0xFF;        /* tracked output state; default all-high (de-asserts resets) */

/** @brief Writes the tracked CH422G output latch to the chip. */
static esp_err_t ch422g_flush(void)
{
    return i2c_master_transmit(s_ch422g_io, &s_ch422g_latch, 1, 100);
}

/** @brief Sets/clears CH422G output bit(s) (by mask) and latches. */
static esp_err_t ch422g_set(uint8_t mask, bool high)
{
    if (high)
        s_ch422g_latch |= mask;
    else
        s_ch422g_latch &= (uint8_t)~mask;
    return ch422g_flush();
}

/**
 * @brief Creates the shared I2C bus + CH422G, sequences LCD reset, and enables DISP/backlight.
 *
 * @return ESP_OK on success.
 */
static esp_err_t board_io_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");

    const i2c_device_config_t set_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = CH422G_ADDR_WR_SET, .scl_speed_hz = 100000};
    const i2c_device_config_t io_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = CH422G_ADDR_WR_IO, .scl_speed_hz = 100000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &set_cfg, &s_ch422g_set), TAG, "ch422g set");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &io_cfg, &s_ch422g_io), TAG, "ch422g io");

    /* Put the IO pins in push-pull output mode, then drive the default latch (all high). */
    const uint8_t mode = CH422G_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_ch422g_set, &mode, 1, 100), TAG, "ch422g mode");
    ESP_RETURN_ON_ERROR(ch422g_flush(), TAG, "ch422g latch");

    /* Pulse the LCD reset low->high (active low), keep touch out of reset, enable DISP/backlight. */
    ESP_RETURN_ON_ERROR(ch422g_set(EXIO_LCD_RST_BIT, false), TAG, "lcd rst low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ch422g_set(EXIO_LCD_RST_BIT, true), TAG, "lcd rst high");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(ch422g_set(EXIO_DISP_BL_BIT | EXIO_TP_RST_BIT, true), TAG, "backlight");

    /* Route GPIO19/20 to the chip's native USB on the main USB-C port (the latch defaults all-high, which
       selects CAN via the on-board FSUSB42UMX mux and leaves the native USB disconnected). Driving
       USB_SEL low connects the native USB — required for on-device hot reload over it. */
    ESP_RETURN_ON_ERROR(ch422g_set(EXIO_USB_SEL_BIT, false), TAG, "usb sel");
    return ESP_OK;
}

bool board_display_init(esp_lcd_panel_handle_t* out_panel)
{
    if (board_io_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "CH422G / I2C init failed");
        return false;
    }

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16, /* RGB565 */
        .num_fbs = 2,     /* double-buffered: the backend draws the off-screen fb and swaps at vsync */
        .bounce_buffer_size_px = 10 * BOARD_LCD_WIDTH, /* smooth PSRAM-fb DMA, avoids tearing/underrun */
        .dma_burst_size = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1, /* DISP is on the CH422G (EXIO2), enabled above */
        .data_gpio_nums =
            {
                14,
                38,
                18,
                17,
                10, /* B3..B7 (blue,  5 bits) */
                39,
                0,
                45,
                48,
                47,
                21, /* G2..G7 (green, 6 bits) */
                1,
                2,
                42,
                41,
                40, /* R3..R7 (red,   5 bits) */
            },
        .timings =
            {
                .pclk_hz = 21 * 1000 * 1000,
                .h_res = BOARD_LCD_WIDTH,
                .v_res = BOARD_LCD_HEIGHT,
                .hsync_back_porch = 8,
                .hsync_front_porch = 8,
                .hsync_pulse_width = 4,
                .vsync_back_porch = 8,
                .vsync_front_porch = 8,
                .vsync_pulse_width = 4,
                .flags.pclk_active_neg = true,
            },
        .flags.fb_in_psram = true,
    };

    esp_lcd_panel_handle_t panel = NULL;
    if (esp_lcd_new_rgb_panel(&panel_config, &panel) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed");
        return false;
    }
    if (esp_lcd_panel_reset(panel) != ESP_OK || esp_lcd_panel_init(panel) != ESP_OK)
    {
        ESP_LOGE(TAG, "panel reset/init failed");
        return false;
    }

    ESP_LOGI(TAG, "RGB panel up: %dx%d", BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);

    *out_panel = panel;
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - GT911 touch
 ---------------------------------------------------------------------------------------------------------------------*/

static i2c_master_dev_handle_t s_gt911; /* GT911 device on the shared bus */

/**
 * @brief Reads a block of GT911 registers (16-bit big-endian register address).
 *
 * @param[in]  reg  Starting register address.
 * @param[out] buf  Destination buffer.
 * @param[in]  n    Number of bytes to read.
 *
 * @return ESP_OK on success.
 */
static esp_err_t gt911_read(uint16_t reg, uint8_t* buf, size_t n)
{
    const uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFU)};
    return i2c_master_transmit_receive(s_gt911, addr, sizeof(addr), buf, n, 100);
}

/**
 * @brief Writes a single GT911 register (16-bit big-endian register address).
 *
 * @param[in] reg  Register address.
 * @param[in] val  Byte to write.
 *
 * @return ESP_OK on success.
 */
static esp_err_t gt911_write_u8(uint16_t reg, uint8_t val)
{
    const uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFFU), val};
    return i2c_master_transmit(s_gt911, buf, sizeof(buf), 100);
}

/**
 * @brief Runs the GT911 reset sequence so it latches the I2C address GT911_ADDR.
 *
 * The GT911 samples its INT pin on the rising edge of RST to choose its address (INT low -> 0x5D).
 * RST is the CH422G EXIO1 line, INT is GPIO4. After the sequence INT is reconfigured as an input.
 */
static void gt911_reset_sequence(void)
{
    /* Drive INT (GPIO4) low. */
    gpio_config_t int_out = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_out);
    gpio_set_level(BOARD_TOUCH_INT_GPIO, 0);

    /* Hold RST low, keep INT low to select address 0x5D, then release RST (latches the address). */
    ch422g_set(EXIO_TP_RST_BIT, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TOUCH_INT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    ch422g_set(EXIO_TP_RST_BIT, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Release INT so the controller can use it as its data-ready interrupt line. */
    gpio_config_t int_in = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_in);
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool board_touch_init(void)
{
    if (!s_i2c_bus)
    {
        ESP_LOGE(TAG, "touch init before display init (no I2C bus)");
        return false;
    }

    gt911_reset_sequence();

    const i2c_device_config_t gt_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = GT911_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(s_i2c_bus, &gt_cfg, &s_gt911) != ESP_OK)
    {
        ESP_LOGE(TAG, "GT911 add-device failed");
        return false;
    }

    uint8_t id[4] = {0};
    if (gt911_read(GT911_REG_PRODUCT_ID, id, sizeof(id)) != ESP_OK)
    {
        ESP_LOGE(TAG, "GT911 not responding at 0x%02X", GT911_ADDR);
        return false;
    }
    ESP_LOGI(TAG, "GT911 up: product id '%c%c%c%c'", id[0], id[1], id[2], id[3]);
    return true;
}

bool board_touch_read(int* x, int* y, bool* pressed)
{
    uint8_t status = 0;
    if (gt911_read(GT911_REG_STATUS, &status, 1) != ESP_OK)
    {
        return false;
    }
    if (!(status & GT911_STATUS_READY))
    {
        return false; /* no fresh sample since the last poll */
    }

    bool ok = false;
    const int points = status & GT911_POINT_COUNT_MASK;
    if (points > 0)
    {
        uint8_t p[4];
        if (gt911_read(GT911_REG_POINT1, p, sizeof(p)) == ESP_OK)
        {
            *x = (int)(p[0] | (p[1] << 8));
            *y = (int)(p[2] | (p[3] << 8));
            *pressed = true;
            ok = true;
        }
    }
    else
    {
        *pressed = false; /* finger lifted */
        ok = true;
    }

    /* The controller latches the buffer until the status register is cleared. */
    gt911_write_u8(GT911_REG_STATUS, 0);
    return ok;
}
