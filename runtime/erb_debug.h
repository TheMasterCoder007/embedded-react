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

/*
 * Where the Flow B runtime keeps the running app, for a debugger reading a halted board. Nothing reads it on the
 * board: the GDB script finds it by its symbol, so it doesn't depend on the runtime's private names or structs.
 */

#ifndef ERB_DEBUG_H
#define ERB_DEBUG_H

#include "er_debug.h"
#include "erb_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief ErbDebug::magic: "ERBD". */
#define ERB_DEBUG_MAGIC 0x44425245u

/** @brief Raised when a field of ErbDebug changes. */
#define ERB_DEBUG_VERSION 1u

    /** @brief Offsets in ErbError. */
    typedef struct
    {
        uint16_t status_at; /**< An ErbStatus, status_bytes long. */
        uint16_t status_bytes;
        uint16_t detail_at; /**< const char*. */
        uint16_t needed_at; /**< uint32_t. */
        uint16_t available_at;
        uint16_t resource_at; /**< uint16_t. */
        uint16_t function_at; /**< uint16_t. */
        uint16_t offset_at;   /**< uint32_t. */
        uint16_t name_at;     /**< const char*. */
        uint16_t build_id_at; /**< uint32_t. */
    } ErbDebugErrorFields;

    /** @brief The descriptor. */
    typedef struct
    {
        uint32_t magic;
        uint16_t version;
        uint16_t frame_cells;       /**< Cells of a VM call frame before its registers. */
        const ERDebug* engine;      /**< The engine's pools and nodes. */
        const char* const* reasons; /**< Each ErbStatus's reason, as erb_status_name() gives it. */
        uint16_t reason_count;
        uint16_t reserved;
        const ErbError* error; /**< The last fault or refused load. */
        ErbDebugErrorFields error_fields;
        const bool* loaded;              /**< A package is running. */
        const bool* faulted;             /**< It stopped on a fault. */
        const bool* in_vm;               /**< The VM is running one of its functions. */
        const uint32_t* build_id;        /**< The running package's build_id. */
        const uint16_t* panel;           /**< Its layout, by index. */
        uint8_t* const* arena;           /**< The arena's address. */
        const uint32_t* stack_offset;    /**< The register stack's offset in the arena, in bytes. */
        const uint16_t* function;        /**< The function the VM is running. */
        const uint32_t* program_counter; /**< Its instruction, as a word offset. */
        const uint32_t* frame_base;      /**< Its frame on the register stack, in cells. */
        volatile bool* update;           /**< Set by a debugger after it writes state: erb_tick() runs the update. */
    } ErbDebug;

    extern const ErbDebug erb_debug;

#ifdef __cplusplus
}
#endif

#endif /* ERB_DEBUG_H */
