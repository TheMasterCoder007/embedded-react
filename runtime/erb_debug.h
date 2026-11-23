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

/** @brief Raised when a field of ErbDebug or ErbDebugControl changes, or an event is added. */
#define ERB_DEBUG_VERSION 3u

/**
 * @brief Set to 1 to build the VM's breakpoint hook, for breakpoints, stepping and a stop at a fault from GDB. Off,
 *        the VM has no code for it. A debug build's option: on, the VM tests one flag on every instruction.
 */
#ifndef ERB_DEBUG_HOOK
#define ERB_DEBUG_HOOK 0
#endif

/** @brief Places the hook can stop at, for breakpoints: 8 bytes each. */
#ifndef ERB_DEBUG_STOPS
#define ERB_DEBUG_STOPS 16u
#endif

    /** @brief When a step ends, in ErbDebugControl::step. */
    typedef enum
    {
        ERB_DEBUG_STEP_NONE = 0,
        ERB_DEBUG_STEP_INTO, /**< Outside the range, or in another function. */
        ERB_DEBUG_STEP_OVER, /**< Outside the range in step_base's frame, or back in its caller. */
        ERB_DEBUG_STEP_OUT,  /**< Back in the caller of step_base's frame. */
        ERB_DEBUG_STEP_ANY,  /**< At the next instruction the app runs. */
    } ErbDebugStep;

    /** @brief Why the runtime called erb_debug_break(), in ErbDebugControl::event. */
    typedef enum
    {
        ERB_DEBUG_AT_STOP = 0, /**< At a stop, or where a step ends. */
        ERB_DEBUG_AT_RETURN,   /**< A step reached the return to the runtime. */
        ERB_DEBUG_AT_LOAD,     /**< A package loaded and none of its code has run: a debugger writes its stops. */
        ERB_DEBUG_AT_FAULT,    /**< The app faulted and nothing has handled it yet: ErbDebug::error says why. */
        ERB_DEBUG_AT_REFUSED,  /**< A load was refused: ErbDebug::error says why. */
    } ErbDebugEvent;

    /** @brief A place to stop: a function and an instruction in it. */
    typedef struct
    {
        uint16_t function;
        uint16_t reserved;
        uint32_t offset; /**< A word offset. */
    } ErbDebugStop;

    /**
     * @brief Where the hook stops. A debugger writes it while the board is halted; the VM tests armed before each
     *        instruction, and calls erb_debug_break() at a stop or at the end of a step. The runtime also calls it
     *        after each load, when the app faults and when a load is refused.
     */
    typedef struct
    {
        uint8_t armed; /**< Set while there are stops or a step. */
        uint8_t step;  /**< An ErbDebugStep. */
        uint8_t event; /**< An ErbDebugEvent, set by the runtime before it calls erb_debug_break(). */
        uint8_t reserved;
        uint16_t step_function;
        uint16_t stop_count;
        uint32_t step_from; /**< The step's instructions in step_function: [from, to). */
        uint32_t step_to;
        uint32_t step_base; /**< The frame the step started in, in cells. */
        ErbDebugStop stops[ERB_DEBUG_STOPS];
    } ErbDebugControl;

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
        volatile bool* halted; /**< Set by a debugger when the board halts: the next erb_tick() counts no time. */
        volatile ErbDebugControl* control; /**< The hook's, or NULL when the firmware is built without it. */
        uint16_t stop_capacity;            /**< ERB_DEBUG_STOPS, or 0 without the hook. */
    } ErbDebug;

    extern const ErbDebug erb_debug;

#if ERB_DEBUG_HOOK
    extern volatile ErbDebugControl erb_debug_control;

    /** @brief Does nothing: a debugger's breakpoint on it is where the hook stops the app. */
    void erb_debug_break(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ERB_DEBUG_H */
