# backends/esp32-lcd

ESP32-S3 backend for `esp_lcd` RGB panels: CPU-side fill / copy / blend (with the S3's PIE SIMD unit
for the blend inner loops), the LCD peripheral DMAs the framebuffer out. The host calls
`er_esp32_lcd_present()` after each `er_commit()`. It picks direct mode (compositing straight into the
panel's rotating framebuffers, using the engine's multi-buffer damage replay) or canonical mode (a
separate framebuffer, copied and optionally rotated on present) at init.

**Docs:** [ESP32-S3 guide, the backend](https://embedded-react.dev/guides/boards/esp32-s3#the-backend).

Options: `ER_LCD_DIRECT` (default 1), `ER_LCD_FB_RGB565` (default 1; 0 for an ARGB8888 canonical
build), `ER_LCD_PIE` (default 1; an init-time self-test falls back to the scalar path on any mismatch).
Used by `examples/esp32/esp32-s3`.
