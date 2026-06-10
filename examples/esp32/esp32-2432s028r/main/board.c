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

/* --- Display: ST7789 on HSPI (SPI2_HOST) --- */
#define LCD_SPI_HOST SPI2_HOST
#define PIN_LCD_SCLK 14
#define PIN_LCD_MOSI 13
#define PIN_LCD_MISO -1 /* panel is write-only; GPIO12 (board MISO) left unclaimed */
#define PIN_LCD_CS 15
#define PIN_LCD_DC 2
#define PIN_LCD_RST -1 /* ST7789 reset is software (no dedicated GPIO) */
#define PIN_LCD_BL 21
#define LCD_PCLK_HZ (20 * 1000 * 1000) /* CYD SPI can glitch at 40 MHz; 20 is safe */

#define BOARD_LCD_INVERT false /* this unit shows correct brightness without inversion */
#define BOARD_LCD_BGR false    /* set true if red/blue are swapped */
#define BOARD_LCD_MIRROR_X true /* this unit's panel scans X reversed (text was mirrored) */
#define BOARD_LCD_MIRROR_Y false

/* --- Touch: XPT2046 on VSPI (SPI3_HOST), separate bus --- */
#define TOUCH_SPI_HOST SPI3_HOST
#define PIN_TP_SCLK 25
#define PIN_TP_MOSI 32
#define PIN_TP_MISO 39
#define PIN_TP_CS 33
#define PIN_TP_IRQ 36 /* PENIRQ: low while touched */
#define TOUCH_SPI_HZ (2 * 1000 * 1000)

/* XPT2046 raw → screen, calibrated for this unit by tapping the 4 corners of the DISPLAYED image (so
 * the display mirror is already accounted for). No axis swap or flip needed here. To recalibrate a
 * different unit: re-enable the raw-touch ESP_LOGI below, tap TL/TR/BR/BL, and set these min/max. */
#define TOUCH_X_MIN 170
#define TOUCH_X_MAX 1895
#define TOUCH_Y_MIN 100
#define TOUCH_Y_MAX 1852
/* The display is un-mirrored via panel mirror_x, but the resistive panel reports PHYSICAL coords, so
 * touch X ends up flipped relative to the app's layout (left/right buttons swap). Flip it back here. */
#define TOUCH_FLIP_X true
#define TOUCH_FLIP_Y false

/*----------------------------------------------------------------------------------------------------------------------
 - Display
 ---------------------------------------------------------------------------------------------------------------------*/

bool board_display_init(esp_lcd_panel_handle_t* out_panel, esp_lcd_panel_io_handle_t* out_io)
{
    /* Backlight on (plain GPIO high; swap for LEDC PWM if you want dimming). */
    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_WIDTH * 80 * (int)sizeof(uint16_t), /* up to ~80-row bands */
    };
    if (spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK)
    {
        ESP_LOGE(TAG, "display SPI bus init failed");
        return false;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io) != ESP_OK)
    {
        ESP_LOGE(TAG, "panel IO init failed");
        return false;
    }

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = BOARD_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &panel) != ESP_OK)
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
    /* Portrait 240x320, top-left origin. Adjust swap_xy/mirror here if the image is rotated/mirrored. */
    esp_lcd_panel_swap_xy(panel, false);
    esp_lcd_panel_mirror(panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    ESP_LOGI(TAG, "ST7789 panel up: %dx%d (invert=%d, bgr=%d)", BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, BOARD_LCD_INVERT, BOARD_LCD_BGR);
    *out_panel = panel;
    *out_io = io;
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - XPT2046 resistive touch
 ---------------------------------------------------------------------------------------------------------------------*/

static spi_device_handle_t s_tp;

/** @brief Reads one 12-bit XPT2046 channel (cmd: 0xD0=X, 0x90=Y), differential mode. */
static int xpt2046_read(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0, 0};
    uint8_t rx[3] = {0, 0, 0};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx, .rx_buffer = rx};
    if (spi_device_polling_transmit(s_tp, &t) != ESP_OK)
    {
        return -1;
    }
    /* 12-bit result is in the upper bits of the two bytes following the command byte. */
    return ((rx[1] << 8) | rx[2]) >> 4;
}

bool board_touch_init(void)
{
    /* PENIRQ as input. GPIO 36 is an input-only pad with NO internal pull-up (the board has an
     * external one), so don't request GPIO_PULLUP_ENABLE — it errors. */
    gpio_config_t irq = {.pin_bit_mask = 1ULL << PIN_TP_IRQ, .mode = GPIO_MODE_INPUT};
    gpio_config(&irq);

    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_TP_SCLK,
        .mosi_io_num = PIN_TP_MOSI,
        .miso_io_num = PIN_TP_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    if (spi_bus_initialize(TOUCH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK)
    {
        ESP_LOGE(TAG, "touch SPI bus init failed");
        return false;
    }
    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = TOUCH_SPI_HZ, .mode = 0, .spics_io_num = PIN_TP_CS, .queue_size = 1};
    if (spi_bus_add_device(TOUCH_SPI_HOST, &devcfg, &s_tp) != ESP_OK)
    {
        ESP_LOGE(TAG, "XPT2046 add-device failed");
        return false;
    }
    ESP_LOGI(TAG, "XPT2046 touch ready");
    return true;
}

/** @brief Linearly maps a raw reading in [lo,hi] to a screen coordinate in [0, span), clamped. */
static int map_axis(int raw, int lo, int hi, int span)
{
    int v = (raw - lo) * span / (hi - lo);
    if (v < 0)
        v = 0;
    if (v > span - 1)
        v = span - 1;
    return v;
}

bool board_touch_read(int* x, int* y, bool* pressed)
{
    if (gpio_get_level(PIN_TP_IRQ) != 0)
    {
        *pressed = false; /* PENIRQ high → no contact */
        return true;
    }

    /* Average a few samples to settle the resistive noise. */
    int sx = 0, sy = 0, n = 0;
    for (int i = 0; i < 4; i++)
    {
        const int rx = xpt2046_read(0xD0); /* X channel */
        const int ry = xpt2046_read(0x90); /* Y channel */
        if (rx >= 0 && ry >= 0)
        {
            sx += rx;
            sy += ry;
            n++;
        }
    }
    if (n == 0)
    {
        *pressed = false;
        return true;
    }
    const int rawx = sx / n;
    const int rawy = sy / n;
    /* ESP_LOGI(TAG, "raw touch %d,%d", rawx, rawy); // re-enable to recalibrate a different unit */

    const int px = map_axis(rawx, TOUCH_X_MIN, TOUCH_X_MAX, BOARD_LCD_WIDTH);
    const int py = map_axis(rawy, TOUCH_Y_MIN, TOUCH_Y_MAX, BOARD_LCD_HEIGHT);
    *x = TOUCH_FLIP_X ? (BOARD_LCD_WIDTH - 1 - px) : px;
    *y = TOUCH_FLIP_Y ? (BOARD_LCD_HEIGHT - 1 - py) : py;
    *pressed = true;
    return true;
}
