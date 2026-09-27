# examples

Board firmware projects. Each pins one engine + one backend + one flow into a runnable artifact,
with whatever the platform needs (IDF component manifests, the Pico SDK's CMake, linker fragments).
A new board usually starts as a copy of the closest one with a new `board.c`.

**Docs:** one [guide per board](https://embedded-react.dev/guides), from a fresh checkout to the
app on the panel, and [Your first board](https://embedded-react.dev/getting-started/first-board)
for choosing a flow.

| Example | Board | Backend | Flow | Status |
|---|---|---|---|---|
| `linux/` | Linux / macOS / Windows desktop | `sdl/` | A | Working |
| `linux-aot/` | Linux / macOS / Windows desktop | `sdl/` | B | Working |
| `esp32/esp32-s3/` | Waveshare ESP32-S3-Touch-LCD-7, 800×480 RGB, PSRAM | `esp32-lcd/` | A | Verified on hardware |
| `esp32/esp32-2432s028r/` | ESP32 "Cheap Yellow Display", 240×320 SPI, no PSRAM | `esp32-spi-lcd/` | B | Verified on hardware |
| `rp2040/rp2040-touch-lcd-1.69/` | Waveshare RP2040-Touch-LCD-1.69, 240×280 SPI | `pico-spi-lcd/` | B | Verified on hardware |
| `stm32h7/` | STM32H7 + LTDC panel | `dma2d/` | A | Planned (the backend runs on hardware; no board project yet) |
| `raspberry-pi/` | Raspberry Pi 4 / 5 | `opengl/` or `framebuffer/` | | Planned |
| `dashboard-demo/`, `marine-display/` | Reference apps | any | | Planned |
