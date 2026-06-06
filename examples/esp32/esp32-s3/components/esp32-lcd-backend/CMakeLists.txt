# The board-agnostic ESP32 LCD render backend (backends/esp32-lcd) as an ESP-IDF component, compiled
# from EMBEDDED_REACT_SRC_DIR (local monorepo or fetched). It implements EmbeddedRenderBackend over an
# esp_lcd panel handle (canonical framebuffer + RGB565 flush), so it needs the engine + esp_lcd.
set(BK "${EMBEDDED_REACT_SRC_DIR}/backends/esp32-lcd")

idf_component_register(
    SRCS "${BK}/renderer_backend.c"
    INCLUDE_DIRS "${BK}"
    REQUIRES engine esp_lcd
)

# Canonical framebuffer pixel format: 1 = RGB565 (default — no per-frame ARGB→565 convert pass, half
# the bandwidth/footprint), 0 = ARGB8888 (full-precision reference). Override with -DER_LCD_FB_RGB565=0.
if(NOT DEFINED ER_LCD_FB_RGB565)
    set(ER_LCD_FB_RGB565 1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_LCD_FB_RGB565=${ER_LCD_FB_RGB565})
