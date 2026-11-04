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
 * This is the device-side mirror of the host's ERHR transport frame (packages/embedded-react/hotreload/
 * protocol.mjs). It is a transport-agnostic, allocation-free state machine: you feed it whatever bytes
 * arrive on your channel, and it scans for the frame magic, parses the header, collects the payload
 * into a caller-provided staging buffer, and verifies the transport CRC. When a complete, intact frame
 * is ready, the caller hands the payload (an .erpkg / ERCF container) to its loader — for a QuickJS app,
 * er_hotreload_apply() in er_runtime.h.
 *
 * Split of responsibilities (the "portable protocol + thin per-board shim" model):
 *   - THIS FILE  — the wire protocol + parser. No threads, no malloc, no platform headers.
 *   - per board  — a small shim that (a) reads bytes from the actual channel and feeds them here, and
 *                  (b) applies a ready frame from the task that owns the runtime/frame loop.
 *
 * Threading: ErHotReload is single-consumer — feed it from ONE task. Applying a frame must run on the
 * task that owns the runtime/frame loop. If you parse on a separate RX task, signal the frame-loop task
 * to apply (see the ESP32-S3 example).
 *
 * Wire format (little-endian) — see protocol.mjs for the authoritative description:
 *   magic "ERHR" (4) | proto_version u8 (=1) | message_type u8 | flags u16 | payload_len u32 | crc u32
 *   then payload_len bytes of .erpkg. The CRC is CRC-32/IEEE over the payload (same polynomial as the
 *   ERCF container's own CRC), so a truncated/corrupt transfer is caught before the runtime is touched.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Wire constants (keep in lockstep with protocol.mjs)
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_HOTRELOAD_HEADER_SIZE 16u
#define ER_HOTRELOAD_MAGIC_SIZE 4u
#define ER_HOTRELOAD_PROTOCOL_VERSION 1u
#define ER_HOTRELOAD_MESSAGE_LOAD_CONTAINER 1u
#define ER_HOTRELOAD_MESSAGE_PACKAGE_BEGIN 2u /* Flow B: see erb_package_reload.h */
#define ER_HOTRELOAD_MESSAGE_PACKAGE_CHUNK 3u

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Result of feeding bytes to the parser. */
typedef enum
{
    ER_HOTRELOAD_NEED_MORE = 0, /**< All input consumed; no frame completed yet — feed more. */
    ER_HOTRELOAD_FRAME_READY,   /**< A complete, CRC-valid frame is in the staging buffer (see out_length). */
    ER_HOTRELOAD_BAD_CRC,       /**< A framed payload failed its CRC; parser resynced — keep feeding. */
    ER_HOTRELOAD_OVERFLOW,      /**< payload_len exceeded the staging buffer; frame dropped — keep feeding. */
    ER_HOTRELOAD_BAD_VERSION,   /**< Unsupported proto version (or empty payload); frame dropped — keep feeding. */
} ErHotReloadStatus;

/** @brief Parser state. Treat the fields as private; initialize with er_hotreload_init. */
typedef struct
{
    uint8_t* buffer; /**< Caller-owned payload staging buffer (e.g. a PSRAM block). */
    size_t capacity; /**< Capacity of @ref buffer in bytes (the largest container it can hold). */

    uint8_t phase;                                 /**< 0 = scan magic, 1 = header, 2 = payload. */
    uint8_t magic_window[ER_HOTRELOAD_MAGIC_SIZE]; /**< Rolling 4-byte window used to find the magic at any offset. */
    uint8_t magic_window_length;                   /**< Bytes currently in @ref magic_window. */
    uint8_t header[ER_HOTRELOAD_HEADER_SIZE];      /**< Header bytes accumulated (magic + fields). */
    uint8_t header_received;                       /**< Header bytes collected so far. */
    uint8_t message_type;                          /**< message_type of the frame in progress / last completed. */
    uint32_t payload_length;                       /**< Declared payload length of the frame in progress. */
    uint32_t payload_crc;                          /**< Declared payload CRC of the frame in progress. */
    uint32_t payload_received;                     /**< Payload bytes collected so far. */
} ErHotReload;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initializes a parser around a caller-owned staging buffer.
 *
 * @param[in] parser    Parser to initialize.
 * @param[in] buffer    Staging buffer that received payloads are assembled into; must outlive @p parser and,
 *                      once a frame is applied, must stay live for as long as the loaded app references it
 *                      (the container's image pixels / font bitmaps point into it — see er_runtime_load_container).
 * @param[in] capacity  Size of @p buffer. Sets the largest container this device can receive in one frame.
 */
void er_hotreload_init(ErHotReload* parser, uint8_t* buffer, size_t capacity);

/**
 * @brief Feeds received bytes to the parser, stopping at the first completed (or rejected) frame.
 *
 * Typical drain loop over a chunk read from the channel:
 * @code
 *   size_t offset = 0;
 *   while (offset < data_length) {
 *       size_t consumed = 0, payload_length = 0;
 *       ErHotReloadStatus status =
 *           er_hotreload_feed(&parser, data + offset, data_length - offset, &consumed, &payload_length);
 *       offset += consumed;
 *       if (status == ER_HOTRELOAD_FRAME_READY)      { handoff(parser.buf, payload_length); }
 *       else if (status == ER_HOTRELOAD_NEED_MORE)   { break; }                  // all consumed; wait for more
 *       else                                  { log_reject(status); }     // resynced; loop continues
 *   }
 * @endcode
 *
 * @param[in]  parser       Parser.
 * @param[in]  data         Received bytes.
 * @param[in]  data_length  Number of bytes in @p data.
 * @param[out] consumed     Receives how many bytes of @p data were processed (so the caller re-feeds the rest).
 * @param[out] out_length   On ER_HOTRELOAD_FRAME_READY, receives the payload length now in @c parser->buf. May be NULL.
 *
 * @return ER_HOTRELOAD_NEED_MORE when @p data is exhausted with no frame done; otherwise the frame outcome.
 */
ErHotReloadStatus
er_hotreload_feed(ErHotReload* parser, const uint8_t* data, size_t data_length, size_t* consumed, size_t* out_length);

/** @brief A short human-readable string for an ErHotReloadStatus (for logging). */
const char* er_hotreload_status_name(ErHotReloadStatus status);

#endif
