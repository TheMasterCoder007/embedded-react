# examples

End-to-end sample applications. Each example pins one engine + one backend + one bridge
into a runnable artifact, with whatever scaffolding the target platform needs
(linker scripts, IDF component manifests, Makefiles, etc.).

| Example | Target | Backend | Flow | Status |
|---|---|---|---|---|
| `linux/` | Linux desktop | `sdl/` | A (QuickJS) | **Implemented** |
| `linux-aot/` | Linux desktop | `sdl/` | B (AOT) | **Implemented** |
| `esp32/esp32-s3/` | ESP32-S3 dev board | `esp32-lcd/` | A (QuickJS, PSRAM) | **Implemented** |
| `esp32/esp32-2432s028r/` | "Cheap Yellow Display" ESP32 (no PSRAM) | `esp32-spi-lcd/` | B (AOT) | **Implemented** |
| `rp2040/rp2040-touch-lcd-1.69/` | Waveshare RP2040 1.69" round-corner touch | `pico-spi-lcd/` | B (AOT) | **Implemented** |
| `stm32h7/` | STM32H7 + LTDC + LCD panel | `dma2d/` | — | Planned (README only) |
| `raspberry-pi/` | RPi 4 / 5 with KMS | `opengl/` or `framebuffer/` | — | Planned (README only) |
| `dashboard-demo/` | Cross-platform UI demo (gauges, charts, controls) | any | — | Planned (README only) |
| `marine-display/` | Real-world dashboard reference app | `dma2d/` (STM32H7) | — | Planned (README only) |

The implemented examples are buildable today; the `stm32h7/`, `raspberry-pi/`, `dashboard-demo/`,
and `marine-display/` folders are still README placeholders awaiting their backends.
