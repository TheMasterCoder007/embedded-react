# examples/esp32 — ESP32 hosts

ESP32-family host integrations, one folder per board/variant. Each subfolder is a self-contained
ESP-IDF project (FetchContents its deps), so paths in their READMEs are relative to the **repo root**
and IDF commands run from **inside the variant folder**.

```
esp32/
  esp32-s3/     ESP32-S3 + PSRAM — Flow A (QuickJS interprets precompiled bytecode). The current,
                working integration (Waveshare ESP32-S3-Touch-LCD-7). See esp32-s3/README.md.
```

## Planned

- **A no-PSRAM ESP32 variant** (its own folder here) — without external RAM the QuickJS heap has
  nowhere to live, so this one will use the **AOT path** (JSX compiled to C against `er_scene.h`, no
  JS runtime) once Flow A and its examples are complete. Not started yet.

Build/flash a specific variant from its own folder, e.g.:

```bash
cd examples/esp32/esp32-s3
idf.py set-target esp32s3
idf.py build flash monitor
```
