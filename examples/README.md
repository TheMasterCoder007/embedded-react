# examples

End-to-end sample applications. Each example pins one engine + one backend + one bridge
into a runnable artifact, with whatever scaffolding the target platform needs
(linker scripts, IDF component manifests, Makefiles, etc.).

| Example | Target | Backend | Status |
|---|---|---|---|
| `linux/` | Linux desktop | `sdl/` | First end-to-end target (planned) |
| `stm32h7/` | STM32H7 + LTDC + LCD panel | `dma2d/` | First MCU bring-up (planned) |
| `esp32/` | ESP32-S3 dev board | `esp32-lcd/` | Planned |
| `raspberry-pi/` | RPi 4 / 5 with KMS | `opengl/` or `framebuffer/` | Planned |
| `dashboard-demo/` | Cross-platform UI demo (gauges, charts, controls) | any | Planned |
| `marine-display/` | Real-world dashboard reference app | `dma2d/` (STM32H7) | Planned |

None of these are buildable yet — each folder is currently a README placeholder. They'll
land alongside the corresponding backend implementations and the Flow A toolchain.
