# backends/esp32-lcd

ESP32-S3 (and forward-compatible variants) LCD peripheral + PSRAM framebuffer backend.
CPU-side fill / copy / blend; the LCD peripheral DMAs the framebuffer out to the panel.
The frame tick is typically driven from a FreeRTOS timer task.

**Status:** Stub. Real implementation lands during ESP32-S3 bring-up.
