# backends/esp32-spi-lcd

SPI-LCD backend for **no-PSRAM ESP32** boards: the internal-RAM counterpart to `esp32-lcd`. By default
it renders banded (`ER_LCD_BANDED=1`): the engine repaints only the dirty rows, one full-width strip
at a time, into two ping-pong DMA-capable RGB565 band buffers (`width × ER_LCD_BANDED_ROWS × 2` bytes
each, about 19 KB at 240 wide and 40 rows), and the panel's own GRAM retains everything else. Full
16-bit color at a fraction of a framebuffer's RAM. A full-framebuffer mode (RGB565, or RGB332 with
`ER_SPI_LCD_FB8=1`) exists for boards with a big enough block.

**Docs:** [ESP32 CYD guide](https://embedded-react.dev/guides/boards/esp32-cyd#how-it-fits-without-psram)
(how it fits, tuning, the `ER_SPI_LCD_SWAP_BGR` panel-order flag) and
[Writing a backend](https://embedded-react.dev/internals/writing-a-backend#optional-extensions) for
the banded contract.

## API

```c
bool er_esp32_spi_lcd_backend_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, int width, int height);
void er_esp32_spi_lcd_present(void);   // full-framebuffer mode: once per frame after er_commit(). Banded: a no-op.
```

The caller owns the panel: bring it up first (SPI bus, ST7789/ILI9341 panel and IO handle, reset,
inversion, MADCTL orientation, backlight) and pass both handles; the IO handle registers the
transfer-done callback that paces DMA. Used by `examples/esp32/esp32-2432s028r`; any `esp_lcd` RGB565
panel works.
