---
title: 'Guides'
description: 'Board-by-board setup, hot reload, the AOT subset, performance and memory.'
---

The concepts pages explain how Embedded React works. These guides are about getting a particular
thing done.

## Boards

Each verified board has an example firmware project in the repository and a guide here that walks
from a fresh checkout to the app on the panel.

| Board                                  | Flow   | Display                                  | Guide                                                        |
| -------------------------------------- | ------ | ---------------------------------------- | ------------------------------------------------------------ |
| Waveshare ESP32-S3-Touch-LCD-7         | A      | 7" 800×480 RGB, capacitive touch         | [ESP32-S3](./boards/esp32-s3.md)                             |
| ESP32-2432S028R "Cheap Yellow Display" | B      | 2.4" 240×320 SPI, resistive touch        | [ESP32 CYD](./boards/esp32-cyd.md)                           |
| Waveshare RP2040-Touch-LCD-1.69        | B      | 1.69" 240×280 SPI, capacitive touch, IMU | [RP2040](./boards/rp2040.md)                                 |
| Linux desktop                          | A or B | SDL window                               | [Linux](./boards/linux.md)                                   |
| STM32H7 with SDRAM                     | A      | LTDC panel via Chrom-ART                 | [STM32H7](./boards/stm32h7.md) (example project in progress) |
| Raspberry Pi                           |        |                                          | [Raspberry Pi](./boards/raspberry-pi.md) (planned)           |

Not sure which flow your board wants? [Two flows](../concepts/two-flows.md) has the decision table.

## Working with the toolchain

- [Hot reload](./hot-reload.md): the three edit-and-see loops, in the browser, on the desktop and on
  a board over USB, and how state survives each.
- [The AOT subset](./aot-subset.md): what the ahead-of-time compiler accepts, what it refuses and
  how it tells you, for apps headed to a board without external RAM.

## Fitting the hardware

- [Performance](./performance.md): where a frame's time goes, the habits that keep an app fast on a
  microcontroller, and the on-device overlay that shows you which subsystem to blame for a slow frame.
- [Memory](./memory.md): the engine's compile-time pools, the JavaScript heap in Flow A, and how the
  three board examples were sized to fit.
