# examples/esp32

ESP32-family hosts, one self-contained ESP-IDF project per board. Each fetches its dependencies at
configure time, so it can be copied out of the repository and built on its own.

| Folder | Board | Flow | Guide |
|---|---|---|---|
| `esp32-s3/` | Waveshare ESP32-S3-Touch-LCD-7 (800×480 RGB, PSRAM) | A, QuickJS runtime | [ESP32-S3](https://embedded-react.dev/guides/boards/esp32-s3) |
| `esp32-2432s028r/` | "Cheap Yellow Display" (240×320 SPI, no PSRAM) | B, compiled C | [ESP32 CYD](https://embedded-react.dev/guides/boards/esp32-cyd) |

Build and flash from inside the board's folder:

```bash
cd examples/esp32/esp32-s3
idf.py set-target esp32s3
idf.py build flash monitor
```
