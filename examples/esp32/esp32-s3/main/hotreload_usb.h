/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HOTRELOAD_USB_H
#define HOTRELOAD_USB_H

/*
 * hotreload_usb — the ESP32-S3 per-board shim for on-device hot reload (the thin transport hook the
 * portable er_hotreload parser plugs into).
 *
 * Transport: the chip's native USB-Serial-JTAG (GPIO19/20), on this board's "USB" port. The console is
 * bound to it (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG, sdkconfig.defaults), which keeps the port enumerated
 * after boot and installs the usb_serial_jtag driver this shim reads from — so the one port carries
 * device logs OUT and `embedded-react dev --device <port>` uploads IN, at full-speed USB rates with USB
 * flow control. The frame magic + CRC let the parser ignore log echoes (see er_hotreload.h). Flashing
 * stays on the separate CH343/UART0 port. (Routing GPIO19/20 to USB instead of CAN — driving the CH422G
 * USB_SEL line low, required for the port to enumerate at all — is handled in board.c.)
 *
 * Memory model (single buffer, no flicker): one PSRAM staging buffer. er_hotreload_apply loads with
 * copy_assets=true, so the new app's assets are copied into their own block and its bytecode into the JS
 * heap — nothing references the staging buffer afterward. That lets the RX task refill the buffer with
 * the next upload while the old app keeps running (and stays on screen): no teardown until the moment a
 * complete frame is applied, and no second full-size buffer to starve the JS heap.
 *
 * Concurrency (keeps er_runtime single-threaded): a background RX task reads + parses bytes; the load
 * (the only thing that touches er_runtime) runs on the frame-loop task via er_hotreload_usb_pump().
 */

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Starts the hot-reload receiver: attaches the USB-Serial-JTAG driver, allocates the PSRAM
 *        staging buffer, and spawns the background RX task.
 *
 * Call once after er_runtime_init() succeeds. Returns false (and leaves hot reload off) if PSRAM/driver
 * setup fails; the firmware then just runs the flashed config.
 *
 * @param[in] max_container_bytes  Largest .erpkg this device will accept in one upload; one buffer of
 *                                 this size is allocated in PSRAM. Keep it just above your biggest app so
 *                                 the JS heap keeps the rest of PSRAM.
 *
 * @return true if the receiver is running.
 */
bool er_hotreload_usb_start(size_t max_container_bytes);

/**
 * @brief Frame-loop hook: loads a freshly received container, if one is ready.
 *
 * Call once per frame from the task that owns er_runtime (before commit/present). When a complete upload
 * has arrived it loads the new container (er_hotreload_apply); the old app stays on screen until then.
 * No-op on frames with nothing pending.
 */
void er_hotreload_usb_pump(void);

#endif
