# The board-agnostic ESP32 LCD render backend (backends/esp32-lcd) as an ESP-IDF component, compiled
# from EMBEDDED_REACT_SRC_DIR (local monorepo or fetched). It implements EmbeddedRenderBackend over an
# esp_lcd panel handle (ARGB8888 framebuffer + RGB565 flush), so it needs the engine + esp_lcd.
set(BK "${EMBEDDED_REACT_SRC_DIR}/backends/esp32-lcd")

idf_component_register(
    SRCS "${BK}/renderer_backend.c"
    INCLUDE_DIRS "${BK}"
    REQUIRES engine esp_lcd
)
