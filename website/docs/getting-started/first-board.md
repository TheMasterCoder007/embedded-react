---
title: "Your first board"
sidebar_label: "First board"
description: "Pick a board and a flow, build the firmware, and flash your app."
---

Shipping an app has two halves that meet at one generated artifact:

1. **The app.** You build your JSX with the `embedded-react` CLI. This produces the artifact and
   never touches your firmware.
2. **The firmware.** A C project that brings up the display and touch input, then hands the
   artifact to the engine. You will usually start from one of the example projects.

## Pick a flow

Your board decides this for you more often than not.

| | Flow A: runtime | Flow B: ahead of time |
|---|---|---|
| Choose it when | Your board has external RAM for the JavaScript heap: PSRAM on an ESP32-S3, or SDRAM on an STM32H7 | Your chip has only its internal RAM (classic ESP32, RP2040, most STM32 boards) |
| Build command | `npx embedded-react build` | `npx embedded-react build --aot` |
| Produces | `dist/app.erpkg`: bytecode, assets and a checksum in one file | `dist/app.gen.c`, `app.gen.h` and `assets.generated.c` |
| Firmware uses it by | Loading it at runtime from flash | Compiling it into the firmware image |
| To update the UI | Replace `app.erpkg`. **No firmware rebuild** | Recompile and reflash |
| API | Everything | [A subset](/guides/aot-subset) |

[The two flows](/concepts/two-flows) explains what happens on the device in each case.

## Verified boards

Each of these has an example firmware project in the repository and a guide here.

| Board | Display | Flow | Toolchain | Guide |
|---|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-7 | 800×480 RGB, capacitive touch | A | ESP-IDF v6 | [ESP32-S3](/guides/boards/esp32-s3) |
| ESP32-2432S028R "Cheap Yellow Display" | 240×320 SPI, resistive touch | B | ESP-IDF v5.3+ | [ESP32 CYD](/guides/boards/esp32-cyd) |
| Waveshare RP2040-Touch-LCD-1.69 | 240×280 SPI, capacitive touch | B | Pico SDK | [RP2040](/guides/boards/rp2040) |
| Linux desktop | SDL window | A or B | CMake, SDL2 | [Linux](/guides/boards/linux) |

No board yet? The [Linux host](/guides/boards/linux) runs the same firmware-side code in a desktop
window, which makes it a good way to learn the C side before hardware arrives.

## Flow A: runtime

The firmware and your app are **two independent artifacts**. Flash the firmware once; after that,
changing the UI means replacing one file.

**1. Build and flash the firmware.** From a copy of
[`examples/esp32/esp32-s3`](https://github.com/TheMasterCoder007/embedded-react/tree/master/examples/esp32/esp32-s3),
with ESP-IDF exported in your shell:

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

With no app loaded yet, the panel shows **"No config loaded"**. That is the firmware working.

**2. Build your app and write it to the board.** From your app project:

```bash
npx embedded-react build
parttool.py write_partition --partition-name=config --input dist/app.erpkg
```

`parttool.py` ships with ESP-IDF. The board restarts into your app, and the log shows
`React mounted at 800x480`. From now on, repeat only step 2.

:::info[Hot reload on the device]
A firmware built with `-DER_HOTRELOAD=1` accepts live updates over USB: `npm run dev:device`
re-packs and streams your app on every save. It is a development-only option, off by default. See
[Hot reload](/guides/hot-reload).
:::

## Flow B: ahead of time

Here the app becomes C source that is compiled into the firmware, so every UI change is a rebuild
and a reflash.

**1. Compile your app to C.** Pass the panel size, so a responsive app resolves to the right layout:

```bash
npx embedded-react build --aot --screen 240x320
```

This writes `dist/app.gen.c`, `dist/app.gen.h` and `dist/assets.generated.c`. The `thermostat` and
`watch-face` templates wrap this as `npm run build:aot`.

**2. Build and flash the firmware** with those files compiled in. For the Cheap Yellow Display:

```bash
idf.py set-target esp32
idf.py -p PORT flash monitor
```

The board guides cover where the generated files go and the wiring for each example.

The ahead-of-time compiler accepts a subset of the API, and tells you at build time, with the file
and line, when an app steps outside it. [The AOT subset](/guides/aot-subset) lists what is and is not
supported.

## Bringing up a different board

The engine is not tied to the boards above. It needs two things from you: a **backend**, which is a
handful of callbacks that fill, copy and blend pixels and push them to your display, and a **frame
loop** that feeds it touch input and time. Several backends already exist (RGB parallel and SPI LCDs
on ESP32, SPI on the RP2040, Chrom-ART on STM32, SDL on the desktop), so a new board is often a
matter of pin and panel configuration.

- [Engine and backends](/concepts/engine-and-backends): how the pieces fit.
- [Writing a backend](/internals/writing-a-backend): when none of the existing ones fit.
- [Memory](/guides/memory): sizing the engine's buffers for a small chip.
