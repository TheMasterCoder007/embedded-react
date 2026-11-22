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
 * Where the engine keeps its pools and nodes, for a debugger reading a halted board. Nothing reads it on the board:
 * the GDB script finds it by its symbol, so it doesn't depend on the engine's private names or struct layouts.
 */

#ifndef ER_DEBUG_H
#define ER_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief ERDebug::magic: "ERDE". */
#define ER_DEBUG_MAGIC 0x45445245u

/** @brief Raised when a field of ERDebug or ERDebugPool changes. */
#define ER_DEBUG_VERSION 2u

    /** @brief The engine's pools, in ERDebug::pools. */
    typedef enum
    {
        ER_DEBUG_POOL_NODES = 0,
        ER_DEBUG_POOL_VECTOR_NODES,
        ER_DEBUG_POOL_ANIMATIONS,
        ER_DEBUG_POOL_ANIMATION_GROUPS,
        ER_DEBUG_POOL_ANIMATION_VALUES,
        ER_DEBUG_POOL_IMAGES,
        ER_DEBUG_POOL_FONTS,
        ER_DEBUG_POOL_COUNT_,
    } ERDebugPoolId;

    /** @brief A pool: an array of records, each with a bool that is set while it is in use. */
    typedef struct
    {
        const void* records;
        uint16_t count;   /**< Records in the array. */
        uint16_t stride;  /**< Bytes from one record to the next. */
        uint16_t used_at; /**< The in-use bool's offset in a record. */
        uint16_t reserved;
    } ERDebugPool;

    /** @brief Offsets in a node record (ER_DEBUG_POOL_NODES). Tags are uint16_t, ER_INVALID_TAG for none. */
    typedef struct
    {
        uint16_t tag_at;
        uint16_t parent_at;
        uint16_t first_child_at;
        uint16_t next_sibling_at;
        uint16_t serial_at; /**< uint32_t. */
        uint16_t type_at;   /**< An ERNodeType, type_bytes long. */
        uint16_t type_bytes;
        uint16_t rect_at;     /**< The laid-out rect: int16_t x, y, width, height, on the screen before scrolling. */
        uint16_t scroll_x_at; /**< float: a ScrollView's offset, which moves its children. */
        uint16_t scroll_y_at;
    } ERDebugNodeFields;

    /** @brief The descriptor. */
    typedef struct
    {
        uint32_t magic;
        uint16_t version;
        uint16_t pool_count; /**< ER_DEBUG_POOL_COUNT_. */
        const ERDebugPool* pools[ER_DEBUG_POOL_COUNT_];
        ERDebugNodeFields node;
        const uint16_t* root_tag; /**< The root node's tag. */
        volatile bool* halted;    /**< Set by a debugger when the board halts: the next embedded_renderer_tick() counts
                                       no time. */
    } ERDebug;

    extern const ERDebug er_debug;

#ifdef __cplusplus
}
#endif

#endif /* ER_DEBUG_H */
