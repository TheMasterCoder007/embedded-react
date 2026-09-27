# backends

Reference implementations of `EmbeddedRenderBackend`: the five function pointers (`fill_rect`,
`copy_rect`, `blend_rect`, `wait`, `frame_ready`) plus the optional extensions (banded rendering,
the opaque `copy_rect_fmt` blit, multiple display buffers). The engine is portable; backends are
not. Each folder is one rendering API or peripheral, not one chip.

**Docs:** [Engine and backends](https://embedded-react.dev/concepts/engine-and-backends) for the
contract, [Writing a backend](https://embedded-react.dev/internals/writing-a-backend) for the
conventions (including the inclusive/exclusive dirty-rectangle rule that bites most often).

| Backend | Hardware | Status |
|---|---|---|
| `dma2d/` | STM32 DMA2D (Chrom-ART) hardware blitter, F4/F7/H7/U5, SDK-free | Runs on hardware |
| `esp32-lcd/` | ESP32-S3 `esp_lcd` RGB panels, framebuffer in PSRAM | Runs on hardware |
| `esp32-spi-lcd/` | SPI panels on a no-PSRAM ESP32: banded RGB565 through internal RAM | Runs on hardware |
| `pico-spi-lcd/` | SPI panels on the RP2040: one RGB565 framebuffer, dirty-rect flush | Runs on hardware |
| `sdl/` | SDL2 window: the desktop host and the test target | Working |
| `software/` | A CPU compositor into an ARGB8888 framebuffer; the reference compositor behind the browser simulator | Working |
| `web/` | The WebAssembly present layer over `software/` | Working |
| `framebuffer/` | Linux `/dev/fb0` | Planned |
| `opengl/` | OpenGL ES 2.0 | Planned |

Each implemented backend's README has its C API and build integration.
