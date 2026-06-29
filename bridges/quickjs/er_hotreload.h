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

#ifndef ER_HOTRELOAD_H
#define ER_HOTRELOAD_H

/*
 * er_hotreload — the portable receiver for on-device hot reload over a byte stream (USB-CDC, a
 * USB-Serial-JTAG console, or a UART).
 *
 * This is the device-side mirror of the host's ERHR transport frame (bridges/quickjs/js/hotreload/
 * protocol.mjs). It is a transport-agnostic, allocation-free state machine: you feed it whatever bytes
 * arrive on your channel, and it scans for the frame magic, parses the header, collects the payload
 * into a caller-provided staging buffer, and verifies the transport CRC. When a complete, intact frame
 * is ready, the caller hands the payload (an .erpkg / ERCF container) to er_runtime — usually via the
 * er_hotreload_apply() convenience below.
 *
 * Split of responsibilities (the "portable protocol + thin per-board shim" model):
 *   - THIS FILE  — the wire protocol + parser. No threads, no malloc, no platform headers.
 *   - per board  — a small shim that (a) reads bytes from the actual channel and feeds them here, and
 *                  (b) applies a ready frame from the task that owns the runtime/frame loop.
 *
 * Threading: ErHotReload is single-consumer — feed it from ONE task. er_hotreload_apply() (and all
 * er_runtime_* calls) must run on the task that owns the runtime/frame loop. If you parse on a separate
 * RX task, signal the frame-loop task to apply (see the ESP32-S3 example).
 *
 * Wire format (little-endian) — see protocol.mjs for the authoritative description:
 *   magic "ERHR" (4) | proto_version u8 (=1) | msg_type u8 | flags u16 | payload_len u32 | crc u32
 *   then payload_len bytes of .erpkg. The CRC is CRC-32/IEEE over the payload (same polynomial as the
 *   ERCF container's own CRC), so a truncated/corrupt transfer is caught before the runtime is touched.
 */

#include "er_runtime.h" /* ErContainerStatus (er_hotreload_apply return) */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Wire constants (keep in lockstep with protocol.mjs)
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_HR_HEADER_SIZE 16u
#define ER_HR_PROTO_VERSION 1u
#define ER_HR_MSG_LOAD_CONTAINER 1u

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Result of feeding bytes to the parser. */
typedef enum
{
    ER_HR_NEED_MORE = 0, /**< All input consumed; no frame completed yet — feed more. */
    ER_HR_FRAME_READY,   /**< A complete, CRC-valid frame is in the staging buffer (see out_len). */
    ER_HR_BAD_CRC,       /**< A framed payload failed its CRC; parser resynced — keep feeding. */
    ER_HR_OVERFLOW,      /**< payload_len exceeded the staging buffer; frame dropped — keep feeding. */
    ER_HR_BAD_VERSION,   /**< Unsupported proto version (or empty payload); frame dropped — keep feeding. */
} ErHotReloadStatus;

/** @brief Parser state. Treat the fields as private; initialize with er_hotreload_init. */
typedef struct
{
    uint8_t* buf; /**< Caller-owned payload staging buffer (e.g. a PSRAM block). */
    size_t cap;   /**< Capacity of @ref buf in bytes (the largest container it can hold). */

    uint8_t phase;                  /**< 0 = scan magic, 1 = header, 2 = payload. */
    uint8_t win[4];                 /**< Rolling 4-byte window used to find the magic at any offset. */
    uint8_t win_len;                /**< Bytes currently in @ref win. */
    uint8_t hdr[ER_HR_HEADER_SIZE]; /**< Header bytes accumulated (magic + fields). */
    uint8_t hdr_got;                /**< Header bytes collected so far. */
    uint8_t msg_type;               /**< msg_type of the frame in progress / last completed. */
    uint32_t payload_len;           /**< Declared payload length of the frame in progress. */
    uint32_t payload_crc;           /**< Declared payload CRC of the frame in progress. */
    uint32_t payload_got;           /**< Payload bytes collected so far. */
} ErHotReload;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initializes a parser around a caller-owned staging buffer.
 *
 * @param[in] hr   Parser to initialize.
 * @param[in] buf  Staging buffer that received payloads are assembled into; must outlive @p hr and,
 *                 once a frame is applied, must stay live for as long as the loaded app references it
 *                 (the container's image pixels / font bitmaps point into it — see er_runtime_load_container).
 * @param[in] cap  Size of @p buf. Sets the largest container this device can receive in one frame.
 */
void er_hotreload_init(ErHotReload* hr, uint8_t* buf, size_t cap);

/**
 * @brief Feeds received bytes to the parser, stopping at the first completed (or rejected) frame.
 *
 * Typical drain loop over a chunk read from the channel:
 * @code
 *   size_t off = 0;
 *   while (off < n) {
 *       size_t consumed = 0, out_len = 0;
 *       ErHotReloadStatus st = er_hotreload_feed(&hr, data + off, n - off, &consumed, &out_len);
 *       off += consumed;
 *       if (st == ER_HR_FRAME_READY)      { handoff(hr.buf, out_len); }
 *       else if (st == ER_HR_NEED_MORE)   { break; }                  // all consumed; wait for more
 *       else                              { log_reject(st); }         // resynced; loop continues
 *   }
 * @endcode
 *
 * @param[in]  hr        Parser.
 * @param[in]  data      Received bytes.
 * @param[in]  n         Number of bytes in @p data.
 * @param[out] consumed  Receives how many bytes of @p data were processed (so the caller re-feeds the rest).
 * @param[out] out_len   On ER_HR_FRAME_READY, receives the payload length now in @c hr->buf. May be NULL.
 *
 * @return ER_HR_NEED_MORE when @p data is exhausted with no frame done; otherwise the frame outcome.
 */
ErHotReloadStatus er_hotreload_feed(ErHotReload* hr, const uint8_t* data, size_t n, size_t* consumed, size_t* out_len);

/** @brief A short human-readable string for an ErHotReloadStatus (for logging). */
const char* er_hotreload_status_str(ErHotReloadStatus status);

/**
 * @brief Convenience: reset the runtime and load a freshly received container, painting the on-screen
 *        error panel on failure (so a bad hot reload is visible, like a bad boot config).
 *
 * Shared by every board's shim so they stay thin. MUST be called from the task that owns the runtime and
 * frame loop (it calls er_runtime_reset + er_runtime_load_container_ex). It loads with copy_assets=true,
 * so @p erpkg does NOT need to stay live afterward — the receiver may immediately reuse the staging
 * buffer for the next upload (which is what lets the old UI stay on screen during a reload, no teardown).
 *
 * @param[in] erpkg  Container bytes received in a frame (the parser's staging buffer).
 * @param[in] len    Payload length from er_hotreload_feed.
 *
 * @return ER_CONTAINER_OK, or the specific load failure (already shown on-screen + returned for logging).
 */
ErContainerStatus er_hotreload_apply(const void* erpkg, size_t len);

#endif
