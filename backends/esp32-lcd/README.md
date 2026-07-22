# backends/esp32-lcd

ESP32-S3 (and forward-compatible variants) LCD peripheral backend for `esp_lcd` RGB panels.
CPU-side fill / copy / blend; the LCD peripheral DMAs the framebuffer out to the panel. The
host drives the frame loop and calls `er_esp32_lcd_present()` after each `er_commit()`.

Two operating modes, chosen automatically at init:

- **Direct mode** (`ER_LCD_DIRECT`, default 1) — used when the panel is unrotated, the
  canonical format is RGB565, and the panel exposes 2–3 rotating framebuffers (`num_fbs`).
  The engine composites straight into the panel framebuffers; its multi-buffer damage replay
  (`er_set_display_buffer_count` / `er_display_present`) repaints whatever each buffer missed
  while the others were displayed. Present is just a cache writeback of the dirty window plus
  a buffer flip. With `num_fbs = 3` there is always a swapped-out buffer ready to draw into,
  so frames never wait on the display; with 2, the first write after a flip waits up to one
  refresh for the swap to land.
- **Canonical mode** — rotated panels, ARGB8888 canonical builds (`ER_LCD_FB_RGB565=0`), and
  single-framebuffer panels composite into a separate canonical framebuffer; present copies
  the dirty region to the panel (rotating it if configured).
