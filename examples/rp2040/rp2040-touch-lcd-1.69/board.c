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
 * Waveshare RP2040-Touch-LCD-1.69 bring-up.
 *
 * Board-specific: the 1.69" rounded-rectangle 240x280 ST7789V2 panel on SPI1 + PWM backlight, and
 * the CST816S capacitive touch controller on I2C1 (shared with the QMI8658 IMU, unused here). Pin
 * map and the ST7789V2 register sequence follow the vendor reference (the RP2040-Touch-LCD-1.69
 * demo code; identical pins to the round 1.28" sibling board).
 *
 * The visible 240x280 sits at a +20-row offset inside the controller's 240x320 GRAM (portrait,
 * MADCTL 0x00); lcd_set_window() applies it, so everything above addresses (0,0)..(239,279).
 *
 * The panel is write-only for us: MISO (GP12) is left unclaimed. CS is driven manually so one
 * assertion can span a whole set_window + pixel-stream sequence.
 */

#include "board.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include <stdio.h>

/* --- Display: ST7789V2 on SPI1 --- */
#define LCD_SPI spi1
#define LCD_SPI_HZ (62500 * 1000) /* clk_peri(125 MHz) / 2 — the fastest even divisor; ~2× the vendor's
                                   * 40 MHz (which the pico rounds down to 31.25). Halves the full-screen
                                   * flush time (the swipe repaints the whole panel each frame). If a unit
                                   * shows sparkle/tearing, drop to 31250*1000. */
#define LCD_Y_OFFSET 20               /* the 280 visible rows start at GRAM row 20 (240x320 GRAM) */
#define PIN_LCD_DC 8
#define PIN_LCD_CS 9
#define PIN_LCD_SCLK 10
#define PIN_LCD_MOSI 11
#define PIN_LCD_RST 13
#define PIN_LCD_BL 25 /* PWM slice 4, channel B */

/* --- Touch: CST816S on I2C1 (shared with the QMI8658 IMU) --- */
#define TP_I2C i2c1
#define TP_I2C_HZ (400 * 1000)
#define PIN_TP_SDA 6
#define PIN_TP_SCL 7
#define PIN_TP_INT 21
#define PIN_TP_RST 22
#define CST816_ADDR 0x15
#define CST816_REG_FINGER_NUM 0x02 /* 0 = no contact */
#define CST816_REG_XPOS_H 0x03    /* XposH/L, YposH/L are consecutive from here */
#define CST816_REG_CHIP_ID 0xA7   /* reads 0xB5 on a CST816S */
#define CST816_REG_DIS_AUTO_SLEEP 0xFE

/* --- IMU: QMI8658 on the SAME I2C1 bus. The demo uses only the ACCELEROMETER (steps + the bubble
       level's tilt), so the gyro is left disabled. --- */
#define QMI_ADDR_L 0x6A         /* SA0 low */
#define QMI_ADDR_H 0x6B         /* SA0 high (this board) */
#define QMI_REG_WHOAMI 0x00     /* reads 0x05 */
#define QMI_REG_CTRL1 0x02      /* serial-interface config */
#define QMI_REG_CTRL2 0x03      /* accel: full-scale + ODR */
#define QMI_REG_CTRL5 0x06      /* accel/gyro LPF */
#define QMI_REG_CTRL7 0x08      /* sensor enable bits (bit0 = accel, bit1 = gyro) */
#define QMI_REG_AX_L 0x35       /* accel X/Y/Z, 6 bytes, little-endian int16 */
#define QMI_CTRL2_4G_125HZ 0x16 /* accel: ±4g (0x1<<4) | ODR 125 Hz (0x06) */

/*----------------------------------------------------------------------------------------------------------------------
 - GC9A01A display
 ---------------------------------------------------------------------------------------------------------------------*/

static inline void lcd_cs(bool select)
{
    gpio_put(PIN_LCD_CS, select ? 0 : 1);
}

/** @brief Sends one command byte (DC low). CS must already be asserted. */
static void lcd_cmd(uint8_t cmd)
{
    gpio_put(PIN_LCD_DC, 0);
    spi_write_blocking(LCD_SPI, &cmd, 1);
}

/** @brief Sends command parameter bytes (DC high). CS must already be asserted. */
static void lcd_data(const uint8_t* data, size_t len)
{
    gpio_put(PIN_LCD_DC, 1);
    spi_write_blocking(LCD_SPI, data, len);
}

static void lcd_data1(uint8_t v)
{
    lcd_data(&v, 1);
}

/**
 * @brief The ST7789V2 register init sequence — the vendor's reference values verbatim.
 *
 * The meaningful ones: MADCTL 0x36 = 0x00 (portrait, RGB order, top-left origin), COLMOD 0x3A =
 * 0x05 (16-bit RGB565), porch/gate/VCOM/power tuning (0xB2..0xD6), the two gamma banks (0xE0/0xE1),
 * 0x21 inversion ON (this panel shows correct colors inverted), 0x11 sleep-out, 0x29 display-on.
 */
static void lcd_init_regs(void)
{
    static const struct
    {
        uint8_t cmd;
        uint8_t n;
        uint8_t data[14];
    } seq[] = {
        {0x36, 1, {0x00}}, /* MADCTL: portrait, RGB */
        {0x3A, 1, {0x05}}, /* COLMOD: 16 bpp */
        {0xB2, 5, {0x0B, 0x0B, 0x00, 0x33, 0x35}},
        {0xB7, 1, {0x11}},
        {0xBB, 1, {0x35}},
        {0xC0, 1, {0x2C}},
        {0xC2, 1, {0x01}},
        {0xC3, 1, {0x0D}},
        {0xC4, 1, {0x20}},
        {0xC6, 1, {0x13}},
        {0xD0, 2, {0xA4, 0xA1}},
        {0xD6, 1, {0xA1}},
        {0xE0, 14, {0xF0, 0x06, 0x0B, 0x0A, 0x09, 0x26, 0x29, 0x33, 0x41, 0x18, 0x16, 0x15, 0x29, 0x2D}},
        {0xE1, 14, {0xF0, 0x04, 0x08, 0x08, 0x07, 0x03, 0x28, 0x32, 0x40, 0x3B, 0x19, 0x18, 0x2A, 0x2E}},
        {0xE4, 3, {0x25, 0x00, 0x00}},
        {0x21, 0, {0}}, /* display inversion ON */
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
    {
        lcd_cmd(seq[i].cmd);
        if (seq[i].n)
        {
            lcd_data(seq[i].data, seq[i].n);
        }
    }
    lcd_cmd(0x11); /* sleep out */
    sleep_ms(120);
    /* NB: display-on (0x29) is intentionally NOT sent here — board_display_init() clears GRAM to black
     * first, then turns the display on, so the panel never shows its random power-on GRAM ("rainbow"). */
}

/** @brief Panel op: opens a write window (CASET/RASET, inclusive, +LCD_Y_OFFSET) and issues RAMWR. */
static void lcd_set_window(int x0, int y0, int x1, int y1)
{
    y0 += LCD_Y_OFFSET;
    y1 += LCD_Y_OFFSET;
    uint8_t xs[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t ys[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    lcd_cmd(0x2A);
    lcd_data(xs, 4);
    lcd_cmd(0x2B);
    lcd_data(ys, 4);
    lcd_cmd(0x2C);
}

/** @brief Panel op: streams pixels (already in panel byte order) into the open window. */
static void lcd_write_pixels(const uint16_t* px, size_t count)
{
    gpio_put(PIN_LCD_DC, 1);
    spi_write_blocking(LCD_SPI, (const uint8_t*)px, count * 2);
}

static const ErPicoLcdPanelOps s_lcd_ops = {
    .set_window = lcd_set_window,
    .write_pixels = lcd_write_pixels,
};

const ErPicoLcdPanelOps* board_lcd_ops(void)
{
    return &s_lcd_ops;
}

void board_backlight(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    pwm_set_gpio_level(PIN_LCD_BL, (uint16_t)percent);
}

bool board_display_init(void)
{
    /* Kill the backlight the instant we run. GP25 floats at reset and the panel's GRAM powers up as
     * random noise, so a floating-high backlight shows a "rainbow" until the first frame. Drive it low
     * as a plain GPIO FIRST (fastest possible), then switch it to PWM (still off). main() only ramps it
     * up after the first real frame is on the panel — so the user sees black → watch face, never noise. */
    gpio_init(PIN_LCD_BL);
    gpio_set_dir(PIN_LCD_BL, GPIO_OUT);
    gpio_put(PIN_LCD_BL, 0);
    gpio_set_function(PIN_LCD_BL, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(PIN_LCD_BL);
    pwm_set_wrap(slice, 100);
    pwm_set_clkdiv(slice, 50.0f);
    pwm_set_gpio_level(PIN_LCD_BL, 0); /* 0% duty — stays off through init */
    pwm_set_enabled(slice, true);

    /* SPI bus + control GPIOs. */
    spi_init(LCD_SPI, LCD_SPI_HZ);
    gpio_set_function(PIN_LCD_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LCD_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_LCD_CS);
    gpio_set_dir(PIN_LCD_CS, GPIO_OUT);
    gpio_put(PIN_LCD_CS, 1);
    gpio_init(PIN_LCD_DC);
    gpio_set_dir(PIN_LCD_DC, GPIO_OUT);
    gpio_init(PIN_LCD_RST);
    gpio_set_dir(PIN_LCD_RST, GPIO_OUT);

    /* Hardware reset, then the register sequence. CS stays asserted from here on — the panel is the
     * bus's only device, and a single long assertion lets present() stream frames back-to-back. */
    gpio_put(PIN_LCD_RST, 1);
    sleep_ms(100);
    gpio_put(PIN_LCD_RST, 0);
    sleep_ms(100);
    gpio_put(PIN_LCD_RST, 1);
    sleep_ms(100);
    lcd_cs(true);
    lcd_init_regs();

    /* Clear GRAM to black while the display is still OFF, so the very first thing it shows is black. */
    lcd_set_window(0, 0, BOARD_LCD_WIDTH - 1, BOARD_LCD_HEIGHT - 1);
    static uint16_t black_row[BOARD_LCD_WIDTH];
    for (int i = 0; i < BOARD_LCD_WIDTH; i++)
    {
        black_row[i] = 0x0000;
    }
    for (int y = 0; y < BOARD_LCD_HEIGHT; y++)
    {
        lcd_write_pixels(black_row, BOARD_LCD_WIDTH);
    }

    /* Now turn the display on — GRAM is black, so no power-on noise is ever visible. The backlight stays
     * off until main() has presented the first frame (board_backlight). */
    lcd_cmd(0x29);
    sleep_ms(20);

    printf("board: ST7789V2 panel up (%dx%d, SPI %.1f MHz)\n",
           BOARD_LCD_WIDTH,
           BOARD_LCD_HEIGHT,
           (double)spi_get_baudrate(LCD_SPI) / 1e6);
    return true;
}

void board_lcd_test_pattern(void)
{
    /* Four horizontal 60-row bands (red / green / blue / white) written straight through the panel
     * ops — no engine, no framebuffer. If these land as clean full-width bands in the right order and
     * position, the panel driver (init + windowing + pixel streaming) is proven good and any on-screen
     * corruption is upstream. RGB565 big-endian: R=0xF800→{F8,00}, G=0x07E0→{07,E0}, B=0x001F→{00,1F}. */
    static const uint16_t colors[4] = {
        (uint16_t)__builtin_bswap16(0xF800),
        (uint16_t)__builtin_bswap16(0x07E0),
        (uint16_t)__builtin_bswap16(0x001F),
        (uint16_t)__builtin_bswap16(0xFFFF),
    };
    static uint16_t row[BOARD_LCD_WIDTH];
    lcd_set_window(0, 0, BOARD_LCD_WIDTH - 1, BOARD_LCD_HEIGHT - 1);
    for (int y = 0; y < BOARD_LCD_HEIGHT; y++)
    {
        const uint16_t c = colors[y / (BOARD_LCD_HEIGHT / 4)];
        for (int i = 0; i < BOARD_LCD_WIDTH; i++)
        {
            row[i] = c;
        }
        lcd_write_pixels(row, BOARD_LCD_WIDTH);
    }

    /* Sub-window addressing check: a 40x40 BLACK square whose top-left should land at (100, 70) —
     * i.e. exactly at the green band's top edge, horizontally centered. If it appears anywhere else,
     * CASET/RASET addressing is off; if it lands there but the engine's frame is still displaced,
     * the fault is upstream of the panel driver. Written as one 40*40 burst (tests multi-row bursts
     * inside a partial window, which is exactly what present() does). */
    static uint16_t sq[40 * 40];
    for (int i = 0; i < 40 * 40; i++)
    {
        sq[i] = 0x0000;
    }
    lcd_set_window(100, BOARD_LCD_HEIGHT / 4, 139, BOARD_LCD_HEIGHT / 4 + 39);
    lcd_write_pixels(sq, 40 * 40);
}

/*----------------------------------------------------------------------------------------------------------------------
 - CST816S capacitive touch
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Millisecond timestamp of the last CST816 INT falling edge (its "I have a report" pulse). */
static volatile uint32_t s_tp_int_ms;

/** @brief INT falling-edge ISR: stamps the pulse time. The chip pulses INT repeatedly while a finger
 *  is down (point mode, NorScanPer=1), so recency of this stamp == "a touch is in progress". */
static void tp_int_isr(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;
    s_tp_int_ms = to_ms_since_boot(get_absolute_time());
}

/** @brief Writes one CST816S register. Returns false on a NAK/timeout. */
static bool tp_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_write_timeout_us(TP_I2C, CST816_ADDR, buf, 2, false, 2000) == 2;
}

/** @brief Reads @p n consecutive CST816S registers starting at @p reg. Returns false on error. */
static bool tp_read(uint8_t reg, uint8_t* out, size_t n)
{
    if (i2c_write_timeout_us(TP_I2C, CST816_ADDR, &reg, 1, true, 2000) != 1)
    {
        return false;
    }
    return i2c_read_timeout_us(TP_I2C, CST816_ADDR, out, n, false, 2000) == (int)n;
}

bool board_touch_init(void)
{
    i2c_init(TP_I2C, TP_I2C_HZ);
    gpio_set_function(PIN_TP_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_TP_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_TP_SDA);
    gpio_pull_up(PIN_TP_SCL);
    /* INT is the chip's report strobe — an edge ISR stamps it (a 60 Hz poll would miss the ~ms pulse). */
    gpio_init(PIN_TP_INT);
    gpio_set_dir(PIN_TP_INT, GPIO_IN);
    gpio_pull_up(PIN_TP_INT);
    gpio_set_irq_enabled_with_callback(PIN_TP_INT, GPIO_IRQ_EDGE_FALL, true, tp_int_isr);

    /* Hardware reset. */
    gpio_init(PIN_TP_RST);
    gpio_set_dir(PIN_TP_RST, GPIO_OUT);
    gpio_put(PIN_TP_RST, 0);
    sleep_ms(50);
    gpio_put(PIN_TP_RST, 1);
    sleep_ms(100);

    /* Probe the chip ID. Board revisions ship different CSTxxx variants (CST816S=0xB4, T=0xB5,
     * D=0xB6...) — they all speak the same register map, so any ACK is good enough; the ID is just
     * logged. Retry a few times: the controller boots slowly after its reset. */
    uint8_t id = 0;
    bool acked = false;
    for (int i = 0; i < 5 && !acked; i++)
    {
        acked = tp_read(CST816_REG_CHIP_ID, &id, 1);
        if (!acked)
        {
            sleep_ms(50);
        }
    }
    if (!acked)
    {
        printf("board: CST816 touch not answering on I2C1 (SDA=%d SCL=%d addr 0x%02X)\n",
               PIN_TP_SDA,
               PIN_TP_SCL,
               CST816_ADDR);
        return false;
    }
    /* The vendor init sequence (CST816S_init in the reference code), each write checked:
     *   0xFE DisAutoSleep=1  — keep the controller awake (asleep it NAKs every poll)
     *   0xFA IrqCtl=0x41     — point mode (EnTouch|OnceWLP): report coordinates continuously
     *   0xED IrqPluseWidth=1, 0xEE NorScanPer=1 — INT pulse width / scan period, vendor values */
    const bool w_sleep = tp_write(CST816_REG_DIS_AUTO_SLEEP, 0x01);
    const bool w_irq = tp_write(0xFA, 0x41);
    const bool w_pw = tp_write(0xED, 0x01);
    const bool w_scan = tp_write(0xEE, 0x01);
    printf("board: CST816 touch ready (chip id 0x%02X; cfg writes sleep=%d irq=%d pw=%d scan=%d)\n",
           id,
           (int)w_sleep,
           (int)w_irq,
           (int)w_pw,
           (int)w_scan);
    return true;
}

bool board_touch_read(int* x, int* y, bool* pressed)
{
    /* INT-gated: the CST816 auto-sleeps a few seconds after boot/last touch and NAKs I2C while
     * asleep (DisAutoSleep does not prevent it on this chip) — but a touch wakes it and it strobes
     * INT for every report while the finger is down. So: no recent INT pulse → no contact, and no
     * I2C traffic at all; a recent pulse → the chip is awake and the point registers are fresh. */
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((uint32_t)(now - s_tp_int_ms) > 60U)
    {
        *pressed = false;
        return true;
    }

    /* FingerNum + XposH/L + YposH/L are consecutive (0x02..0x06) — one burst read per poll. */
    uint8_t buf[5];
    if (!tp_read(CST816_REG_FINGER_NUM, buf, sizeof(buf)))
    {
        *pressed = false; /* mid-wake hiccup — treat as no contact; the next INT pulse retries */
        return false;
    }
    if (buf[0] == 0)
    {
        *pressed = false;
        return true;
    }
    int px = ((buf[1] & 0x0F) << 8) | buf[2];
    int py = ((buf[3] & 0x0F) << 8) | buf[4];
    if (px > BOARD_LCD_WIDTH - 1)
        px = BOARD_LCD_WIDTH - 1;
    if (py > BOARD_LCD_HEIGHT - 1)
        py = BOARD_LCD_HEIGHT - 1;
    *x = px;
    *y = py;
    *pressed = true;
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - QMI8658 IMU (accelerometer — for step counting). Shares I2C1 with the touch controller.
 ---------------------------------------------------------------------------------------------------------------------*/

static uint8_t s_qmi_addr; /* resolved at init (0x6A or 0x6B); 0 until a chip answers */

/** @brief Writes one QMI8658 register. Returns false on a NAK/timeout. */
static bool qmi_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_write_timeout_us(TP_I2C, s_qmi_addr, buf, 2, false, 2000) == 2;
}

/** @brief Reads @p n consecutive QMI8658 registers from @p reg. Returns false on error. */
static bool qmi_read(uint8_t reg, uint8_t* out, size_t n)
{
    if (i2c_write_timeout_us(TP_I2C, s_qmi_addr, &reg, 1, true, 2000) != 1)
    {
        return false;
    }
    return i2c_read_timeout_us(TP_I2C, s_qmi_addr, out, n, false, 2000) == (int)n;
}

bool board_imu_init(void)
{
    /* I2C1 is already up (board_touch_init ran first). Probe both possible addresses; WHO_AM_I = 0x05. */
    const uint8_t candidates[2] = {QMI_ADDR_H, QMI_ADDR_L};
    uint8_t who = 0;
    bool found = false;
    for (int i = 0; i < 2 && !found; i++)
    {
        s_qmi_addr = candidates[i];
        if (qmi_read(QMI_REG_WHOAMI, &who, 1) && who == 0x05)
        {
            found = true;
        }
    }
    if (!found)
    {
        printf("board: QMI8658 IMU not found on I2C1 (SDA=%d SCL=%d)\n", PIN_TP_SDA, PIN_TP_SCL);
        s_qmi_addr = 0;
        return false;
    }

    /* CTRL1 serial defaults + auto-increment; CTRL5 no LPF; CTRL2 accel ±4g @125 Hz; CTRL7 enable the
     * accelerometer only (bit0) — the gyro (bit1) stays off; nothing here uses it. */
    qmi_write(QMI_REG_CTRL1, 0x60);
    qmi_write(QMI_REG_CTRL5, 0x00);
    qmi_write(QMI_REG_CTRL2, QMI_CTRL2_4G_125HZ);
    qmi_write(QMI_REG_CTRL7, 0x01);
    printf("board: QMI8658 IMU ready (addr 0x%02X, accel ±4g @125Hz)\n", s_qmi_addr);
    return true;
}

bool board_imu_read_accel(int16_t* ax, int16_t* ay, int16_t* az)
{
    if (!s_qmi_addr)
    {
        return false;
    }
    uint8_t b[6];
    if (!qmi_read(QMI_REG_AX_L, b, sizeof(b)))
    {
        return false;
    }
    *ax = (int16_t)((b[1] << 8) | b[0]); /* little-endian int16 per axis */
    *ay = (int16_t)((b[3] << 8) | b[2]);
    *az = (int16_t)((b[5] << 8) | b[4]);
    return true;
}
