# backends/esp32-spi-lcd

Lean **SPI-LCD render backend** for **no-PSRAM ESP32** boards (e.g. the original ESP32-WROOM-32). The
internal-RAM counterpart to [`backends/esp32-lcd`](../esp32-lcd) (which targets PSRAM RGB panels on the
ESP32-S3).

```
engine (ARGB8888 fill/copy/blend)  →  one RGB565 framebuffer (internal RAM)  →  esp_lcd SPI panel
```

## Why a separate backend

`backends/esp32-lcd` keeps its canonical framebuffer **and** a staging buffer in **PSRAM** and uses the
RGB-panel double-buffer API — none of which exists on a plain ESP32. This backend is stripped to fit
internal DRAM:

- **One** RGB565 framebuffer in internal RAM — `width × height × 2` bytes (240×320 = **150 KB**). No
  PSRAM, no double-buffer, no software rotation, no overlay.
- Present pushes the dirty box as **full-width bands**: because each band spans the full width, its rows
  are contiguous in the framebuffer, so it's handed straight to `esp_lcd_panel_draw_bitmap` — **no
  staging buffer** (saves another 150 KB).

This is the **Flow B (AOT)** display path: the no-PSRAM ESP32 runs the compiled-C app (no QuickJS), and
the only large RAM cost is this single framebuffer.

## API

```c
bool er_esp32_spi_lcd_backend_init(esp_lcd_panel_handle_t panel, int width, int height);
void er_esp32_spi_lcd_present(void);   // call once per frame, after er_commit()
```

The **caller owns the panel** — bring it up first (SPI bus, ST7789/ILI9341 panel, reset, color
inversion, MADCTL/orientation, backlight) and pass the handle. Physical orientation is the panel's job
(`esp_lcd_panel_swap_xy` / `mirror`); this backend renders the logical buffer 1:1 to the panel.

## Used by

[`examples/esp32/esp32-2432s028r`](../../examples/esp32/esp32-2432s028r) — the DIYmall **ESP32-2432S028R
"Cheap Yellow Display"** (ST7789 SPI, 240×320). Panel-agnostic, though: any `esp_lcd` RGB565 panel works.
