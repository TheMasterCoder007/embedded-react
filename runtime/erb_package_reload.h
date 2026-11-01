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

#ifndef ERB_PACKAGE_RELOAD_H
#define ERB_PACKAGE_RELOAD_H

/*
 * erb_package_reload — on-device hot reload for Flow B (`embedded-react dev --device --aot`).
 *
 * The board feeds it what arrives on its serial channel, from the thread that drives the engine. A package comes as
 * ERHR frames (er_hotreload.h) and goes straight into the staging slot, so no board needs RAM for a whole package:
 *   PACKAGE_BEGIN  package_bytes u32
 *   PACKAGE_CHUNK  offset u32, then the bytes
 *
 * The host sends one frame and waits for the reply, so nothing arrives while flash is being written and a UART with
 * no flow control loses nothing. Each reply is one line through the log callback:
 *   "erb reload: ready <chunk_bytes>"  send chunks of this size
 *   "erb reload: got <bytes>"          send from this offset
 *   "dropped frame: <reason>"          send that frame again
 *   "hot reload: ok (...)"             the new app is running
 *   "hot reload: <reason>"             it isn't
 *
 * The whole package goes through erb_load(), so one that fails verification leaves the running app as it was. Its slot
 * gets a header only once it has loaded, so a reset always comes back to the last package that did.
 */

#include "er_hotreload.h"
#include "erb_package_slots.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The bytes in front of a chunk: its offset. */
#define ERB_PACKAGE_RELOAD_CHUNK_HEADER_BYTES 4u

/** @brief The buffer for chunks of @p chunk_bytes, which must be a multiple of ERB_PACKAGE_SLOT_PAGE_BYTES. */
#define ERB_PACKAGE_RELOAD_BUFFER_BYTES(chunk_bytes) ((chunk_bytes) + ERB_PACKAGE_RELOAD_CHUNK_HEADER_BYTES)

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A receiver. Treat the fields as private. */
typedef struct
{
    ErbPackageSlots slots;
    ErbConfig config;
    ErHotReload parser;
    void (*log)(const char* line, void* context);
    void* log_context;
    uint32_t* buffer;
    uint32_t chunk_bytes;
    int active;             /**< The slot the running app came from, or ERB_PACKAGE_SLOT_NONE. */
    int staging;            /**< The slot the next package goes into. */
    uint32_t package_bytes; /**< The package coming in. */
    uint32_t received;      /**< Its bytes so far. */
    uint32_t erased_to;     /**< The slot's erased bytes so far. */
    bool receiving;         /**< A package is coming in. */
    bool reason_up;         /**< The error screen is up over a running app. */
} ErbPackageReload;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Readies a receiver, after erb_package_slots_boot().
 *
 * @param[out] reload        The receiver.
 * @param[in]  slots         The board's slots, with erase and program set; copied.
 * @param[in]  config        What packages load with; copied.
 * @param[in]  boot          From erb_package_slots_boot().
 * @param[in]  buffer        RAM for one chunk, ERB_PACKAGE_RELOAD_BUFFER_BYTES(@p chunk_bytes) long; kept.
 * @param[in]  chunk_bytes   The chunk size. A bigger one is faster and takes more RAM.
 * @param[in]  log           Prints one line to the host.
 * @param[in]  log_context   Passed to @p log.
 */
void erb_package_reload_init(ErbPackageReload* reload,
                             const ErbPackageSlots* slots,
                             const ErbConfig* config,
                             const ErbPackageSlotBoot* boot,
                             uint32_t* buffer,
                             uint32_t chunk_bytes,
                             void (*log)(const char* line, void* context),
                             void* log_context);

/**
 * @brief Takes bytes from the host. Call it between frames: it writes flash, and may replace the app.
 *
 * @param[in] reload  The receiver.
 * @param[in] data    The bytes.
 * @param[in] length  How many.
 *
 * @return true when a new app started, so anything the board looked up in the old one needs looking up again.
 */
bool erb_package_reload_feed(ErbPackageReload* reload, const uint8_t* data, size_t length);

/**
 * @brief Tells the receiver the panel was tapped: a rejected package's reason goes, showing the app still running.
 *        Touch doesn't reach the app while the error screen is up, so the tap does nothing else.
 *
 * @param[in] reload  The receiver.
 */
void erb_package_reload_tap(ErbPackageReload* reload);

#endif
