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

#ifndef ER_CONTAINER_H
#define ER_CONTAINER_H

/*
 * er_container — reader for the ERCF container (.erpkg), the file a config partition or a hot-reload
 * frame carries. It checks the structure and the CRC and hands back the sections; running them is up to
 * the caller (er_runtime_load_container for a QuickJS app). No allocation and no engine calls.
 *
 * Layout (little-endian): "ERCF" | format_version u32 | crc32 u32 (over everything after it) |
 *   tag (u16 len + bytes) | section_count u32 | sections[ type u32 | len u32 | bytes ]
 * The tag names what the program was built for (the QuickJS release, for Flow A). Section type 0 is
 * alignment padding: zero bytes the emitter inserts so the asset pack starts 4-byte aligned relative to
 * the container, since asset pixels are read in place. Loaders skip unknown types, so a new section type
 * never breaks an older loader. The caller must place the container at a 4-byte-aligned base (a flash
 * linker section or a malloc'd buffer both are).
 *
 * The writer is packages/embedded-react/assets/emit-container.mjs.
 */

#include <stddef.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The only container format version this reader accepts. */
#define ER_CONTAINER_FORMAT_VERSION 1u

/** @brief Most sections one container may hold. A Flow A container uses up to 3: the app bytecode, the asset pack
 *         and the vendor bytecode of the hot-reload split. */
#define ER_CONTAINER_MAX_SECTIONS 8u

/** @brief Section types. */
enum
{
    ER_SECTION_PADDING = 0,         /**< Alignment padding; skipped. */
    ER_SECTION_BYTECODE = 1,        /**< The app, as QuickJS bytecode. */
    ER_SECTION_ASSET_PACK = 2,      /**< An ERPK asset pack (see er_assets.h). */
    ER_SECTION_VENDOR_BYTECODE = 3, /**< The framework half of the hot-reload split, as QuickJS bytecode. */
};

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Result of reading or loading a container (see er_container_status_str). */
typedef enum
{
    ER_CONTAINER_OK = 0,      /**< Read (or loaded and ran) cleanly. */
    ER_CONTAINER_BAD_MAGIC,   /**< Not an ERCF container. */
    ER_CONTAINER_BAD_VERSION, /**< Unsupported container format version (or malformed structure). */
    ER_CONTAINER_BAD_CRC,     /**< Integrity check failed — corrupt or partially written. */
    ER_CONTAINER_BAD_QJS,     /**< Built for a different QuickJS version than this firmware (er_runtime). */
    ER_CONTAINER_LOAD_FAILED, /**< No bytecode section, or the app threw on evaluation (er_runtime). */
} ErContainerStatus;

/** @brief One section of a container. @ref data points into the container buffer. */
typedef struct
{
    uint32_t type;       /**< Section type (ER_SECTION_*, or a type this reader doesn't know). */
    const uint8_t* data; /**< Section bytes. */
    uint32_t length;     /**< Section length in bytes. */
} ErContainerSection;

/** @brief A container's tag and sections, as er_container_read or er_container_scan found them. */
typedef struct
{
    const uint8_t* tag;     /**< Tag bytes (not NUL-terminated). */
    uint16_t tag_length;    /**< Tag length in bytes. */
    uint32_t section_count; /**< Entries used in @ref sections. */
    ErContainerSection sections[ER_CONTAINER_MAX_SECTIONS];
    size_t size; /**< Exact container size; the buffer it came from may be larger. */
} ErContainer;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads a container: checks the magic, the format version and every section bound, then the CRC.
 *
 * @p buffer_length may be an UPPER BOUND, not the exact byte count: the reader walks the sections (each bounds-checked
 * against @p buffer_length) to find the container's true end, then takes the CRC over exactly that. So you can pass the
 * full size of a memory-mapped config partition without knowing the stored container's length.
 *
 * @param[in]  buffer         Container bytes.
 * @param[in]  buffer_length  Buffer length, or an upper bound.
 * @param[out] container      Receives the tag and sections; only meaningful on ER_CONTAINER_OK.
 *
 * @return ER_CONTAINER_OK, ER_CONTAINER_BAD_MAGIC, ER_CONTAINER_BAD_VERSION or ER_CONTAINER_BAD_CRC.
 */
ErContainerStatus er_container_read(const void* buffer, size_t buffer_length, ErContainer* container);

/**
 * @brief Like er_container_read, without the CRC check: a structural scan, safe on malformed input.
 *
 * For deciding how to load a container before loading it (see er_runtime_container_has_vendor). Don't run
 * anything from a scan; er_container_read is what verifies the bytes.
 *
 * @param[in]  buffer         Container bytes.
 * @param[in]  buffer_length  Buffer length, or an upper bound.
 * @param[out] container      Receives the tag and sections; only meaningful on ER_CONTAINER_OK.
 *
 * @return ER_CONTAINER_OK, ER_CONTAINER_BAD_MAGIC or ER_CONTAINER_BAD_VERSION.
 */
ErContainerStatus er_container_scan(const void* buffer, size_t buffer_length, ErContainer* container);

/** @brief Returns a short human-readable string for an ErContainerStatus (for logging). */
const char* er_container_status_str(ErContainerStatus status);

/**
 * @brief CRC-32/IEEE (zlib polynomial 0xEDB88320), table-free.
 *
 * The ONE checksum of the delivery pipeline: the hot-reload transport frame, the host-built container and
 * the container as the device verifies it are all this function. It is shared, rather than kept privately
 * by each of them, because a divergence does not fail loudly — a frame or container simply stops loading
 * with a CRC mismatch and nothing says why.
 *
 * The JS side (packages/embedded-react/assets/emit-container.mjs, which hotreload/protocol.mjs imports) is the
 * other copy and cannot share this one; it carries a note pointing back here, and container.unit.test.js
 * pins both to the standard "123456789" -> 0xCBF43926 check vector.
 *
 * @param[in] bytes       Bytes to checksum.
 * @param[in] byte_count  Number of bytes.
 *
 * @return The CRC as an unsigned 32-bit value.
 */
uint32_t er_crc32(const uint8_t* bytes, size_t byte_count);

#endif
