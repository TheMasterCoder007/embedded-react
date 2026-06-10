# backends/esp32-spi-lcd

Lean **SPI-LCD render backend** for **no-PSRAM ESP32** boards (e.g. the original ESP32-WROOM-32). The
internal-RAM counterpart to [`backends/esp32-lcd`](../esp32-lcd) (which targets PSRAM RGB panels on the
ESP32-S3).

```
engine (ARGB8888 fill/copy/blend)  →  RGB565 band buffer (internal RAM)  →  esp_lcd SPI panel (GRAM)
```

## Why a separate backend

`backends/esp32-lcd` keeps its canonical framebuffer **and** a staging buffer in **PSRAM** and uses the
RGB-panel double-buffer API — none of which exists on a plain ESP32. This backend is stripped to fit
internal DRAM, and offers two compile-time modes:

### Banded RGB565 (default for tight RAM — `ER_LCD_BANDED=1`)

A full 240×320 RGB565 framebuffer is 150 KB and does **not** fit the ESP32's fragmented internal DRAM
(largest contiguous block ~110 KB). In banded mode the **engine repaints only the dirty rows, one
horizontal strip at a time**, into a small **DMA-capable RGB565 band buffer** (`240 × ER_LCD_BANDED_ROWS
× 2` ≈ **19 KB** at the default 40 rows). Each full-width strip is handed straight to
`esp_lcd_panel_draw_bitmap` (no bounce); the panel's **own GRAM retains everything outside the dirty
rows**. Net: **full 16-bit color at a fraction of the RAM**, and crisp anti-aliased text. The engine
drives the strips via the `band_height` / `band_begin` / `band_flush` fields of `EmbeddedRenderBackend`,
so banding is an LCD-agnostic engine capability — any band backend opts in the same way.

### Full framebuffer (`ER_LCD_BANDED` unset)

One canonical framebuffer in internal RAM, flushed as full-width bands through a small DMA bounce buffer.
Pixel format is selectable: **RGB565** (`width × height × 2`; 240×320 = 150 KB — needs a board with a big
enough block) or **RGB332** (`ER_SPI_LCD_FB8=1`, 1 B/px = 75 KB; 256 colors, coarser anti-aliased text).
Banded RGB565 generally supersedes the RGB332 fallback — it gives 16-bit color in *less* RAM.

This is the **Flow B (AOT)** display path: the no-PSRAM ESP32 runs the compiled-C app (no QuickJS).

### Panel pixel order (`ER_SPI_LCD_SWAP_BGR`)

Some SPI panels display a stored RGB565 word as `BGR(byteswap(word))` rather than the standard layout —
the **Cheap Yellow Display** ST7789 is one: it wants the two color bytes swapped **and** red/blue in BGR
order. Set `ER_SPI_LCD_SWAP_BGR=1` and `fb_store` pre-compensates (telltale if it's wrong: gray renders
pastel-green, blue renders pink, while pure red/white still look right). Leave it unset (default) for a
standard RGB565 panel.

## API

```c
bool er_esp32_spi_lcd_backend_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, int width, int height);
void er_esp32_spi_lcd_present(void);   // full-fb mode: call once per frame after er_commit(). Banded: no-op.
```

The **caller owns the panel** — bring it up first (SPI bus, ST7789/ILI9341 panel + its IO handle, reset,
color inversion, MADCTL/orientation, backlight) and pass both handles. The IO handle is needed to register
the transfer-done callback that paces DMA. Physical orientation is the panel's job
(`esp_lcd_panel_swap_xy` / `mirror`); this backend renders the logical buffer 1:1 to the panel.

## Used by

[`examples/esp32/esp32-2432s028r`](../../examples/esp32/esp32-2432s028r) — the DIYmall **ESP32-2432S028R
"Cheap Yellow Display"** (ST7789 SPI, 240×320). Panel-agnostic, though: any `esp_lcd` RGB565 panel works.
