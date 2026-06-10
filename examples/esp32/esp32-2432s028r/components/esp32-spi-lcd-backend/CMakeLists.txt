# The lean SPI-LCD render backend (backends/esp32-spi-lcd) as an ESP-IDF component, compiled from
# EMBEDDED_REACT_SRC_DIR. Renders in horizontal strips through a small RGB565 band buffer and flushes
# each to the esp_lcd panel — the no-PSRAM display path. Needs the engine + esp_lcd.
set(BK "${EMBEDDED_REACT_SRC_DIR}/backends/esp32-spi-lcd")

idf_component_register(
    SRCS "${BK}/renderer_backend.c"
    INCLUDE_DIRS "${BK}"
    REQUIRES engine esp_lcd
)

# BANDED RGB565: a full 240x320 RGB565 framebuffer (150 KB) does NOT fit the ESP32's fragmented internal
# DRAM (largest block ~110 KB). Instead the engine renders the dirty rows in horizontal strips through a
# small RGB565 band buffer (240 x 40 = ~19 KB, DMA-capable); the ST7789's own GRAM retains the rest of
# the frame. Result: full 16-bit color (crisp anti-aliased text) at LESS RAM than the old RGB332 fb
# (75 KB). Tune the strip height with ER_LCD_BANDED_ROWS (default 40). For a PSRAM board you could drop
# banding and use a full RGB565 fb instead.
# ER_SPI_LCD_SWAP_BGR: this CYD's ST7789 displays a stored RGB565 word as BGR(byteswap(word)) — it wants
# the two color bytes swapped AND red/blue in BGR order. Verified on-device (without it, gray renders
# pastel-green and blue renders pink). A panel-specific quirk, so it lives here, not in the engine.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_LCD_BANDED=1 ER_LCD_BANDED_ROWS=40 ER_SPI_LCD_SWAP_BGR=1)
