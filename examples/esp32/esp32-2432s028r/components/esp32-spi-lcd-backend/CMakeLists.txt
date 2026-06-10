# The lean SPI-LCD render backend (backends/esp32-spi-lcd) as an ESP-IDF component, compiled from
# EMBEDDED_REACT_SRC_DIR. Keeps one RGB565 framebuffer in internal RAM and flushes full-width bands to
# the esp_lcd panel — the no-PSRAM display path. Needs the engine + esp_lcd.
set(BK "${EMBEDDED_REACT_SRC_DIR}/backends/esp32-spi-lcd")

idf_component_register(
    SRCS "${BK}/renderer_backend.c"
    INCLUDE_DIRS "${BK}"
    REQUIRES engine esp_lcd
)

# RGB332 (1 B/px) canonical framebuffer: 240x320 = 75 KB, which fits the ESP32's largest contiguous
# internal block (~110 KB). A full RGB565 fb (150 KB) does NOT fit the fragmented internal DRAM here.
# Trade-off: 256 colors / banding on anti-aliased text. (For a PSRAM board, drop this for RGB565.)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_SPI_LCD_FB8=1)
