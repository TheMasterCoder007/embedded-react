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

#ifndef ERB_INTERNAL_H
#define ERB_INTERNAL_H

/*
 * Shared by the verifier (erb_verify.c), the VM (erb_vm.c) and the runtime (erb_runtime.c). Not part of the
 * runtime's API.
 *
 * Every table of a verified program sits 4-aligned in a 4-aligned container, and every record field sits at
 * an offset of its own size, so fields are read with plain loads.
 */

#include "erb_format.h"
#include "erb_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Reading records
 ---------------------------------------------------------------------------------------------------------------------*/

static inline uint16_t erb_u16(const uint8_t* bytes)
{
    uint16_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static inline uint32_t erb_u32(const uint8_t* bytes)
{
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static inline float erb_f32(const uint8_t* bytes)
{
    float value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

/** @brief True when @p value is a multiple of 4, the format's alignment. */
static inline bool erb_aligned4(uintptr_t value)
{
    return (value & 3u) == 0u;
}

/* Bit sets, in 32-bit words. */
#define ERB_BIT_WORDS(bits) (((bits) + 31u) / 32u)

static inline bool erb_bit_get(const uint32_t* words, uint32_t bit)
{
    return (words[bit / 32u] >> (bit % 32u)) & 1u;
}

static inline void erb_bit_set(uint32_t* words, uint32_t bit)
{
    words[bit / 32u] |= 1u << (bit % 32u);
}

/*----------------------------------------------------------------------------------------------------------------------
 - A verified program
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief One arena region. */
typedef struct
{
    uint32_t offset;
    uint32_t bytes;
    uint32_t count;
    bool present;
} ErbRegion;

/** @brief Where everything in a verified package is. Every count and index here has been checked. */
typedef struct
{
    const uint8_t* section;
    uint32_t section_length;
    uint16_t flags;
    uint16_t engine_abi;
    uint32_t build_id;

    /* The panel that matched the screen. */
    uint32_t arena_bytes;
    uint16_t node_count;
    uint16_t root_template;
    uint16_t boot_function;
    uint16_t update_function;

    const uint8_t* table[ERB_TABLE_COUNT_];
    uint32_t table_length[ERB_TABLE_COUNT_];

    uint32_t string_count;
    const uint32_t* const32;
    uint32_t const32_count;
    const uint8_t* const64;
    uint32_t const64_count;
    ErbRegion regions[ERB_REGION_COUNT_];
    uint32_t dirty_words;
    const uint8_t* slots;
    uint32_t slot_count;
    const uint8_t* enum_sets;
    uint32_t enum_set_count;
    const uint8_t* enum_values;
    uint32_t enum_value_count;
    const uint8_t* rows;
    uint32_t row_count;
    const uint8_t* fields;
    uint32_t field_count;
    const uint8_t* prop_sets;
    uint32_t prop_set_count;
    const uint8_t* prop_entries;
    uint32_t prop_entry_count;
    const uint8_t* templates;
    uint32_t template_count;
    const uint8_t* nodes;
    uint32_t node_record_count;
    const uint8_t* bindings;
    uint32_t binding_count;
    const uint8_t* functions;
    uint32_t function_count;
    const uint8_t* types;
    uint32_t types_length;
    const uint32_t* code;
    uint32_t code_words;
    const uint8_t* anims;
    uint32_t anim_count;
    const uint8_t* tapes;
    uint32_t tape_count;
    const uint8_t* paints;
    uint32_t paint_count;
    const uint8_t* gradients;
    uint32_t gradient_count;
    const uint8_t* stops;
    uint32_t stop_count;
    const uint8_t* imports;
    uint32_t import_count;
    const uint8_t* inputs;
    uint32_t input_count;

    const uint8_t* debug;
    uint32_t debug_length;
    const uint8_t* assets;
    uint32_t assets_length;

    uint32_t largest_tape;                               /**< Floats in the largest tape (the scratch must hold it). */
    uint16_t natives[ERB_MAX_IMPORTS];                   /**< Each import's index in ErbConfig::natives. */
    uint32_t needs[ERB_RES_COUNT_];                      /**< Each resource's declared need (0 when not named). */
    uint32_t counting[ERB_BIT_WORDS(ERB_MAX_FUNCTIONS)]; /**< Entry points whose worst case exceeds the budget. */
} ErbProgram;

/* Record accessors. */
static inline const uint8_t* erb_slot(const ErbProgram* program, uint32_t index)
{
    return program->slots + index * ERB_SLOT_SIZE;
}

static inline const uint8_t* erb_row(const ErbProgram* program, uint32_t index)
{
    return program->rows + index * ERB_ROW_SIZE;
}

static inline const uint8_t* erb_function(const ErbProgram* program, uint32_t index)
{
    return program->functions + index * ERB_FUNCTION_SIZE;
}

static inline const uint8_t* erb_template(const ErbProgram* program, uint32_t index)
{
    return program->templates + index * ERB_TEMPLATE_SIZE;
}

static inline const uint8_t* erb_anim(const ErbProgram* program, uint32_t index)
{
    return program->anims + index * ERB_ANIM_SIZE;
}

static inline const uint8_t* erb_tape(const ErbProgram* program, uint32_t index)
{
    return program->tapes + ERB_TAPE_HEADER_SIZE + index * ERB_TAPE_SIZE;
}

/** @brief A string constant, NUL-terminated in place. */
static inline const char* erb_string(const ErbProgram* program, uint32_t index)
{
    const uint8_t* table = program->table[ERB_TABLE_STRINGS];
    return (const char*)table + erb_u32(table + ERB_STRINGS_OFFSETS_AT + index * sizeof(uint32_t));
}

/** @brief The string index of value @p value of enum set @p set. */
static inline uint16_t erb_enum_string(const ErbProgram* program, uint32_t set, uint32_t value)
{
    const uint16_t first = erb_u16(program->enum_sets + set * ERB_SET_SIZE + ERB_SET_FIRST_AT);
    return erb_u16(program->enum_values + (first + value) * sizeof(uint16_t));
}

static inline uint16_t erb_enum_count(const ErbProgram* program, uint32_t set)
{
    return erb_u16(program->enum_sets + set * ERB_SET_SIZE + ERB_SET_COUNT_AT);
}

/** @brief Bytes a slot takes in the arena. */
uint32_t erb_slot_bytes(const ErbProgram* program, const uint8_t* slot);

/** @brief Bytes a row field takes. */
uint32_t erb_field_bytes(uint8_t type, uint16_t capacity);

/** @brief Register cells a value type takes. */
static inline uint32_t erb_type_cells(uint8_t type)
{
    return (type == ERB_T_INT64 || type == ERB_T_DOUBLE || type == ERB_T_NODE) ? 2u : 1u;
}

/** @brief Instruction length in words, or 0 for an opcode this format doesn't have. */
uint32_t erb_op_words(uint8_t op);

/*----------------------------------------------------------------------------------------------------------------------
 - Values
 *
 * A string register is 0 (the empty string), a constant's index + 1, or ERB_STRING_ARENA | an arena offset.
 * A node register is two cells: the engine tag + 1 (0 is no node), then the serial. An anim register is
 * ERB_PACK16(running-animation entry index, generation); 0 is none.
 ---------------------------------------------------------------------------------------------------------------------*/

#define ERB_STRING_ARENA 0x80000000u

/*----------------------------------------------------------------------------------------------------------------------
 - Arena records
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A timer (ERB_TIMER_SIZE bytes). */
typedef struct
{
    int32_t interval_ms;
    int32_t remaining_ms;
    uint16_t generation;
    uint16_t function;
    uint8_t active;
    uint8_t repeat;
    uint16_t reserved;
} ErbTimer;

/** @brief A running animation descriptor (ERB_ANIM_ENTRY_SIZE bytes): one per descriptor in a started tree. */
typedef struct
{
    uint16_t descriptor; /**< Descriptor index. */
    uint16_t parent;     /**< Entry of the group it runs in, or ERB_NONE for the tree's root. */
    uint16_t generation; /**< Generation, so a stale anim register doesn't match a reused entry. */
    uint16_t engine;     /**< The engine's handle while a leaf runs. */
    uint16_t step;       /**< Sequence: the child running. Parallel and stagger: children still running. */
    uint16_t passes;     /**< Loop: passes finished. */
    uint16_t done_fn;    /**< Root: the function to run when the tree finishes, or ERB_NONE. */
    uint8_t state;       /**< ErbAnimState. */
    uint8_t first;       /**< 1 when this entry is the first of its tree's block. */
    float value;         /**< Leaf: its to value. Loop: the start value it resets to. Delay: ms left. */
} ErbAnimEntry;

/** @brief A queued event (ERB_EVENT_SIZE bytes); also the current event's data. */
typedef struct
{
    uint16_t function;
    uint16_t instance;
    uint8_t kind;     /**< ErbEventKind. */
    uint8_t finished; /**< ANIM_DONE: the animation ran to its end. */
    uint16_t reserved;
    int16_t x;         /**< Touch x, or layout x. */
    int16_t y;         /**< Touch y, or layout y. */
    int16_t dx;        /**< Touch dx, or layout width. */
    int16_t dy;        /**< Touch dy, or layout height. */
    float motion_x;    /**< vx, or scroll x. */
    float motion_y;    /**< vy, or scroll y. */
    float value;       /**< value. */
    float value_start; /**< value_start. */
} ErbEvent;

/** @brief What a queued event runs. */
typedef enum
{
    ERB_EVENT_CALL = 0,   /**< Run a function, then the update. */
    ERB_EVENT_EFFECT = 1, /**< Run an effect queued by an update pass. */
    ERB_EVENT_UPDATE = 2, /**< Only run the update (an input or the clock changed). */
} ErbEventKind;

/*----------------------------------------------------------------------------------------------------------------------
 - Verifier
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Checks a whole package against @p config (steps 1 to 6 of a load) and fills @p program.
 *
 * Reads only the package: a running app is untouched whatever the result.
 *
 * @return true when every check passed; else @p error says which failed.
 */
bool erb_verify(
    const void* package, size_t package_bytes, const ErbConfig* config, ErbProgram* program, ErbError* error);

/**
 * @brief Finds the source line for a function and offset in the debug table, if there is one.
 *
 * @param[out] file  Receives the file name, or NULL.
 * @param[out] line  Receives the line, or 0.
 */
void erb_debug_lookup(const ErbProgram* program, uint16_t function, uint32_t offset, const char** file, uint32_t* line);

/*----------------------------------------------------------------------------------------------------------------------
 - VM and runtime
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The running app, shared by the runtime and the VM. */
typedef struct
{
    ErbProgram program;
    ErbConfig config;
    uint8_t* arena;
    bool loaded;
    bool faulted;
    bool in_vm;
    uint32_t budget;
    ErbEvent event;          /**< The current event's data. */
    int64_t wall_clock_base; /**< Epoch ms at wall_clock_set_ms. */
    uint64_t wall_clock_set_ms;
    ErbError error;
} ErbRuntime;

extern ErbRuntime g_erb;

/**
 * @brief Runs one function to its end.
 *
 * @param[in]  function  Function index.
 * @param[in]  args      Its parameter cells (params of them).
 * @param[out] result    Receives its result cells, when it has one; may be NULL.
 *
 * @return ERB_OK, or the fault that stopped it (g_erb.error is filled).
 */
ErbStatus erb_vm_run(uint16_t function, const int32_t* args, int32_t* result);

/** @brief The props scratch: PROPS starts it, PSET fills it, COMMIT hands it to a node. BUILD reuses it. */
extern ERProps g_erb_props;

/** @brief Most descriptors one ASTART may start (the verifier checks each tree against it). */
#define ERB_MAX_ANIM_TREE 32u

/* What the VM asks of the runtime. Those returning ErbStatus return ERB_OK or a fault (filling g_erb.error). */
ErbStatus erb_rt_fault(ErbStatus status, const char* detail, const char* name, uint16_t function, uint32_t offset);
void erb_rt_report(const char* what, uint16_t function, uint32_t offset);
void erb_rt_set_dirty(uint16_t bit);
ERNode* erb_rt_node(const int32_t* cells);
void erb_rt_set_node(int32_t* cells, ERNode* node);
void erb_rt_props(uint16_t set);
void erb_rt_pset_int(uint16_t prop, int32_t value);
void erb_rt_pset_float(uint16_t prop, float value);
bool erb_rt_pset_string(uint16_t prop, const char* value);
ErbStatus erb_rt_build(uint16_t template_index, ERNode* parent, ERNode* before, int32_t instance, ERNode** out);
void erb_rt_destroy(ERNode* node);
void erb_rt_commit_tape(ERNode* node, uint16_t tape, const float* floats);
ErbStatus erb_rt_queue_effect(uint16_t function);
int32_t erb_rt_timer_start(uint16_t function, int32_t interval_ms, bool repeat);
void erb_rt_timer_clear(int32_t id);
uint32_t erb_rt_anim_start(uint16_t descriptor, uint16_t done_function, const int32_t* to_cells);
void erb_rt_anim_stop(uint32_t handle);

#endif
