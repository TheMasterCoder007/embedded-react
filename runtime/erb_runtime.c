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
 * erb_runtime — loading and running a verified Flow B program: the arena, templates, events and update
 * passes, timers, the animation runner, host inputs, teardown and the error screen.
 *
 * The VM is never re-entered. An engine callback that arrives while it runs (a focus event, an animation
 * stopped by the handler) is queued in the arena and run when the current event is done.
 *
 * A fault inside an engine callback is only recorded; the next erb_tick() tears the app down and shows the
 * error screen, so the engine is never reset from inside its own dispatch.
 */

#include "er_error_screen.h"
#include "erb_internal.h"
#include "erb_props.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Longest report line, with its NUL. */
#define REPORT_LINE_SIZE 160u

/** @brief Timer generations cycle 1 to this, so a timer id (generation * timers + slot) stays positive. */
#define TIMER_GENERATIONS 0x7FFFu

/** @brief s_tree_parent of a tree's root. */
#define TREE_NO_PARENT UINT8_MAX

/** @brief The error screen's titles. */
#define ERROR_TITLE_LOAD "App didn't load"
#define ERROR_TITLE_RUN "App stopped"

/** @brief Longest reason, amounts or location line, with its NUL. */
#define ERROR_LINE_SIZE 128u

/** @brief Longest amount written with its separators and unit, with its NUL. */
#define AMOUNT_SIZE 24u

/** @brief An amount's digits are grouped in threes. */
#define AMOUNT_GROUP_DIGITS 3u

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Running-animation entry states. */
enum
{
    ANIM_FREE = 0,
    ANIM_RUNNING = 1, /**< A leaf the engine is animating, or a group whose children run. */
    ANIM_DELAY = 2,   /**< A delay leaf counting down `value` ms. */
    ANIM_WAIT = 3,    /**< A staggered child waiting `value` ms to start. */
    ANIM_DONE = 4,
    ANIM_STOPPED = 5, /**< Its tree is being stopped; engine callbacks for it are ignored. */
    ANIM_DELAY_DUE = 6, /**< A delay leaf whose time ran out this tick. */
    ANIM_WAIT_DUE = 7,  /**< A staggered child whose wait ran out this tick. */
};

ErbRuntime g_erb;
ERProps g_erb_props;

static ErbProgram s_candidate;
static ERVectorPaint s_paints[ERUI_VECTOR_PAINTS_MAX];
static ERVectorGradient s_gradients[ERUI_VECTOR_GRADS_MAX];
#if ERUI_ONSCREEN_KEYBOARD
static ERKeyboardKey s_keyboard_keys[ERB_MAX_KEYBOARD_KEYS];
static ERKeyboardRow s_keyboard_rows[ERB_MAX_KEYBOARD_ROWS];
static ERKeyboardLayer s_keyboard_layers[ERB_MAX_KEYBOARD_LAYERS];
static ERKeyboardConfig s_keyboard;
#endif
static uint32_t s_queue_head;
static uint32_t s_queue_count;
static uint32_t s_queue_effects; /**< Effects at the front of the queue, run before any other event. */
static bool s_fault_pending;
static uint16_t s_screen_w; /**< The panel size from the last erb_load(), for the error screen. */
static uint16_t s_screen_h;
static uint16_t s_anim_generation;
static uint16_t s_tree[ERB_MAX_ANIM_TREE];
static uint8_t s_tree_parent[ERB_MAX_ANIM_TREE];

typedef struct
{
    uint16_t function;
    uint32_t offset;
} Place;

static Place s_reported[ERB_REPORT_PLACES];
static uint32_t s_reported_count;

/*----------------------------------------------------------------------------------------------------------------------
 - Arena regions
 ---------------------------------------------------------------------------------------------------------------------*/

static uint8_t* region(uint8_t kind)
{
    return g_erb.arena + g_erb.program.regions[kind].offset;
}

static uint32_t* dirty_words(void)
{
    return (uint32_t*)region(ERB_REGION_DIRTY);
}

static ErbTimer* timers(void)
{
    return (ErbTimer*)region(ERB_REGION_TIMERS);
}

static ErbAnimEntry* anim_entries(void)
{
    return (ErbAnimEntry*)region(ERB_REGION_ANIMS);
}

static ErbEvent* queue_record(uint32_t position)
{
    const uint32_t capacity = g_erb.program.regions[ERB_REGION_EVENTS].count;
    return (ErbEvent*)region(ERB_REGION_EVENTS) + (s_queue_head + position) % capacity;
}

static uint8_t* slot_data(uint16_t slot)
{
    return g_erb.arena + erb_u32(erb_slot(&g_erb.program, slot) + ERB_SLOT_OFFSET_AT);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Faults and reports
 ---------------------------------------------------------------------------------------------------------------------*/

const char* erb_status_str(ErbStatus status)
{
    static const char* const reasons[ERB_STATUS_COUNT_] = {
        [ERB_OK] = "ok",
        [ERB_ERR_NO_PACKAGE] = "no package installed",
        [ERB_ERR_CORRUPT] = "not a package, or corrupt",
        [ERB_ERR_NOT_FLOW_B] = "not a Flow B package",
        [ERB_ERR_NEWER_FORMAT] = "built for a newer runtime",
        [ERB_ERR_ENGINE_ABI] = "built for another engine ABI",
        [ERB_ERR_NO_PANEL] = "no layout for this screen",
        [ERB_ERR_TOO_BIG] = "needs more than the firmware reserves",
        [ERB_ERR_TABLE] = "malformed table",
        [ERB_ERR_STRING] = "malformed string",
        [ERB_ERR_ARENA] = "bad arena layout",
        [ERB_ERR_STATE] = "bad state layout",
        [ERB_ERR_PROP] = "bad prop",
        [ERB_ERR_TEMPLATE] = "bad template",
        [ERB_ERR_BINDING] = "bad binding",
        [ERB_ERR_ANIMATION] = "bad animation",
        [ERB_ERR_TAPE] = "bad vector tape",
        [ERB_ERR_FUNCTION] = "bad function",
        [ERB_ERR_BYTECODE] = "bad bytecode",
        [ERB_ERR_CONTROL_FLOW] = "bad control flow",
        [ERB_ERR_RECURSION] = "recursive call",
        [ERB_ERR_STACK] = "stack too deep",
        [ERB_ERR_IMPORT] = "missing native module",
        [ERB_ERR_INPUT] = "bad input",
        [ERB_ERR_DEBUG] = "debug table does not match",
        [ERB_ERR_ASSETS] = "asset pack failed to load",
        [ERB_ERR_BUDGET] = "instruction budget exceeded",
        [ERB_ERR_UPDATE_LOOP] = "update loop",
        [ERB_ERR_QUEUE_FULL] = "event queue full",
        [ERB_ERR_LIST_INDEX] = "list read past its end",
        [ERB_ERR_TRAP] = "app stopped",
        [ERB_ERR_NATIVE] = "native module failed",
        [ERB_ERR_STALE_NODE] = "stale node handle",
        [ERB_ERR_OUT_OF_NODES] = "out of nodes",
    };
    return (unsigned)status < ERB_STATUS_COUNT_ ? reasons[status] : "unknown";
}

static const char* resource_name(uint16_t resource)
{
    switch (resource)
    {
        case ERB_RESOURCE_ARENA:
            return "arena";
        case ERB_RESOURCE_NODES:
            return "nodes";
        case ERB_RES_VECTOR_NODES:
            return "vector nodes";
        case ERB_RES_ANIM_VALUES:
            return "animated values";
        case ERB_RES_ANIMATIONS:
            return "animations";
        case ERB_RES_ANIM_GROUPS:
            return "animation groups";
        case ERB_RES_IMAGES:
            return "images";
        case ERB_RES_FONTS:
            return "fonts";
        case ERB_RES_GLYPHS:
            return "glyphs";
        case ERB_RES_EXTRA_GLYPHS:
            return "extra glyphs";
        default:
            return "an unknown resource";
    }
}

/**
 * @brief The length snprintf() wrote, cut to what fits.
 *
 * @param written   What snprintf() returned.
 * @param capacity  The buffer's size.
 */
static size_t written_length(int written, size_t capacity)
{
    if (written < 0 || capacity == 0u)
    {
        return 0u;
    }
    return (size_t)written < capacity ? (size_t)written : capacity - 1u;
}

/**
 * @brief Writes @p amount with its digits grouped by commas, and " B" when it counts bytes.
 *
 * @param[in]  amount  The amount.
 * @param[in]  bytes   true when it counts bytes.
 * @param[out] buffer  Receives the text; AMOUNT_SIZE bytes.
 */
static void format_amount(uint32_t amount, bool bytes, char* buffer)
{
    char digits[AMOUNT_SIZE];
    const size_t digit_count =
        written_length(snprintf(digits, sizeof(digits), "%lu", (unsigned long)amount), sizeof(digits));
    size_t length = 0u;
    for (size_t digit = 0u; digit < digit_count; digit++)
    {
        if (digit > 0u && (digit_count - digit) % AMOUNT_GROUP_DIGITS == 0u)
        {
            buffer[length++] = ',';
        }
        buffer[length++] = digits[digit];
    }
    buffer[length] = '\0';
    if (bytes)
    {
        strcat(buffer, " B");
    }
}

/**
 * @brief Writes the needed and available amounts of @p error, for the failures that have them.
 *
 * @return The length written, or 0 when @p error has no amounts.
 */
static size_t error_amounts(const ErbError* error, char* buffer, size_t capacity)
{
    char needed[AMOUNT_SIZE];
    char available[AMOUNT_SIZE];
    int written;
    switch (error->status)
    {
        case ERB_ERR_TOO_BIG:
            format_amount(error->needed, error->resource == ERB_RESOURCE_ARENA, needed);
            format_amount(error->available, error->resource == ERB_RESOURCE_ARENA, available);
            written = snprintf(buffer,
                               capacity,
                               "%s: needs %s, firmware reserves %s",
                               resource_name(error->resource),
                               needed,
                               available);
            break;
        case ERB_ERR_ENGINE_ABI:
            written = snprintf(buffer,
                               capacity,
                               "built for engine ABI %lu, this firmware runs %d to %d",
                               (unsigned long)error->needed,
                               ER_ABI_MIN,
                               ER_ABI);
            break;
        case ERB_ERR_NO_PANEL:
            written = snprintf(buffer,
                               capacity,
                               "this screen is %lu x %lu",
                               (unsigned long)error->needed,
                               (unsigned long)error->available);
            break;
        case ERB_ERR_STACK:
            format_amount(error->needed, false, needed);
            format_amount(error->available, false, available);
            written = snprintf(buffer, capacity, "needs %s cells, the arena has %s", needed, available);
            break;
        default:
            written = 0;
            if (capacity > 0u)
            {
                buffer[0] = '\0';
            }
            break;
    }
    return written_length(written, capacity);
}

/**
 * @brief Writes where @p error happened: the source line from the debug table, or the function and offset.
 *
 * @return The length written, or 0 when it happened outside the app's code.
 */
static size_t error_location(const ErbError* error, char* buffer, size_t capacity)
{
    int written = 0;
    if (error->file)
    {
        written = snprintf(buffer, capacity, "%s:%lu", error->file, (unsigned long)error->line);
    }
    else if (error->function != ERB_NONE)
    {
        written = snprintf(buffer,
                           capacity,
                           "function %u at %lu (build %08lx)",
                           (unsigned)error->function,
                           (unsigned long)error->offset,
                           (unsigned long)error->build_id);
    }
    else if (capacity > 0u)
    {
        buffer[0] = '\0';
    }
    return written_length(written, capacity);
}

/** @brief Writes the reason for @p error, with the module or trap message it names. */
static size_t error_reason(const ErbError* error, char* buffer, size_t capacity)
{
    return written_length(snprintf(buffer,
                                   capacity,
                                   "%s%s%s",
                                   erb_status_str(error->status),
                                   error->name ? ": " : "",
                                   error->name ? error->name : ""),
                          capacity);
}

size_t erb_error_message(const ErbError* error, char* buffer, size_t capacity)
{
    if (!buffer || capacity == 0u)
    {
        return 0u;
    }
    size_t length;
    switch (error->status)
    {
        case ERB_ERR_TOO_BIG:
        case ERB_ERR_ENGINE_ABI:
            length = error_amounts(error, buffer, capacity);
            break;
        case ERB_ERR_NO_PANEL:
            length = written_length(snprintf(buffer,
                                             capacity,
                                             "no layout for %lu x %lu",
                                             (unsigned long)error->needed,
                                             (unsigned long)error->available),
                                    capacity);
            break;
        case ERB_ERR_STACK:
            length = written_length(snprintf(buffer, capacity, "stack too deep: "), capacity);
            length += error_amounts(error, buffer + length, capacity - length);
            break;
        default:
            length = error_reason(error, buffer, capacity);
            break;
    }
    char location[ERROR_LINE_SIZE];
    if (length + 1u < capacity && error_location(error, location, sizeof(location)) > 0u)
    {
        length += written_length(
            snprintf(buffer + length, capacity - length, "%s%s", error->file ? " at " : " in ", location),
            capacity - length);
    }
    return length;
}

ErbStatus erb_rt_fault(ErbStatus status, const char* detail, const char* name, uint16_t function, uint32_t offset)
{
    ErbError* error = &g_erb.error;
    memset(error, 0, sizeof(*error));
    error->status = status;
    error->detail = detail;
    error->name = name;
    error->function = function;
    error->offset = offset;
    error->build_id = g_erb.program.build_id;
    if (function != ERB_NONE)
    {
        erb_debug_lookup(&g_erb.program, function, offset, &error->file, &error->line);
    }
    g_erb.faulted = true;
    s_fault_pending = true;
    return status;
}

void erb_rt_report(const char* what, uint16_t function, uint32_t offset)
{
    for (uint32_t index = 0; index < s_reported_count; index++)
    {
        if (s_reported[index].function == function && s_reported[index].offset == offset)
        {
            return;
        }
    }
    if (s_reported_count < ERB_REPORT_PLACES)
    {
        s_reported[s_reported_count].function = function;
        s_reported[s_reported_count].offset = offset;
        s_reported_count++;
    }
    if (g_erb.config.log)
    {
        char line[REPORT_LINE_SIZE];
        const char* file = NULL;
        uint32_t source_line = 0;
        erb_debug_lookup(&g_erb.program, function, offset, &file, &source_line);
        if (file)
        {
            snprintf(line, sizeof(line), "ERB: %s (%s:%lu)", what, file, (unsigned long)source_line);
        }
        else if (function == ERB_NONE)
        {
            snprintf(line, sizeof(line), "ERB: %s", what);
        }
        else
        {
            snprintf(
                line, sizeof(line), "ERB: %s (function %u at %lu)", what, (unsigned)function, (unsigned long)offset);
        }
        g_erb.config.log(line, g_erb.config.context);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Values the VM keeps
 ---------------------------------------------------------------------------------------------------------------------*/

void erb_rt_set_dirty(uint16_t bit)
{
    erb_bit_set(dirty_words(), bit);
}

ERNode* erb_rt_node(const int32_t* cells)
{
    if (cells[0] == 0)
    {
        return NULL;
    }
    const ERNodeRef ref = {(uint16_t)(cells[0] - 1), (uint32_t)cells[1]};
    return er_node_deref(ref);
}

void erb_rt_set_node(int32_t* cells, ERNode* node)
{
    const ERNodeRef ref = er_node_ref(node);
    cells[0] = node ? ref.tag + 1 : 0;
    cells[1] = node ? (int32_t)ref.serial : 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Props
 ---------------------------------------------------------------------------------------------------------------------*/

static void store(const ErbPropInfo* info, uint32_t bits)
{
    uint8_t* field = (uint8_t*)&g_erb_props + info->offset;
    switch (info->kind)
    {
        case ERB_PK_I16:
        case ERB_PK_U16:
        {
            const uint16_t value = (uint16_t)bits;
            memcpy(field, &value, sizeof(value));
            break;
        }
        case ERB_PK_COLOR:
        case ERB_PK_FLOAT:
            memcpy(field, &bits, sizeof(bits));
            break;
        default:
            *field = (uint8_t)bits;
            break;
    }
}

static int32_t clamp(int32_t value, int32_t low, int32_t high)
{
    return value < low ? low : value > high ? high : value;
}

void erb_rt_props(uint16_t set)
{
    er_props_default(&g_erb_props);
    if (set == ERB_NONE)
    {
        return;
    }
    const ErbProgram* program = &g_erb.program;
    const uint16_t first = erb_u16(program->prop_sets + set * ERB_SET_SIZE);
    const uint16_t count = erb_u16(program->prop_sets + set * ERB_SET_SIZE + 2u);
    for (uint32_t index = 0; index < count; index++)
    {
        const uint8_t* entry = program->prop_entries + (first + index) * ERB_PROP_SIZE;
        const ErbPropInfo* info = erb_prop_info(erb_u16(entry + ERB_PROP_ID_AT));
        const uint32_t value = erb_u32(entry + ERB_PROP_VALUE_AT);
        if (info->kind == ERB_PK_STRING || info->kind == ERB_PK_IMAGE)
        {
            strcpy((char*)&g_erb_props + info->offset, erb_string(program, value)); /* fits: checked at load */
        }
        else
        {
            store(info, value);
        }
    }
}

void erb_rt_pset_int(uint16_t prop, int32_t value)
{
    const ErbPropInfo* info = erb_prop_info(prop);
    switch (info->kind)
    {
        case ERB_PK_I16:
            store(info, (uint32_t)clamp(value, INT16_MIN, INT16_MAX));
            break;
        case ERB_PK_U8:
        case ERB_PK_OPACITY:
            store(info, (uint32_t)clamp(value, 0, UINT8_MAX));
            break;
        case ERB_PK_U16:
            store(info, (uint32_t)clamp(value, 0, UINT16_MAX));
            break;
        case ERB_PK_ENUM:
            store(info, (uint32_t)clamp(value, 0, info->max));
            break;
        case ERB_PK_BOOL:
            store(info, value != 0);
            break;
        case ERB_PK_FLOAT:
        {
            const float as_float = (float)value;
            uint32_t bits;
            memcpy(&bits, &as_float, sizeof(bits));
            store(info, bits);
            break;
        }
        default:
            store(info, (uint32_t)value);
            break;
    }
}

/** @brief A dimension as app_round_dim snaps it: halves up, clamped to int16, NaN as 0. */
static int32_t round_dim(float value)
{
    if (value != value)
    {
        return 0;
    }
    if (value < (float)INT16_MIN)
    {
        return INT16_MIN;
    }
    if (value > (float)INT16_MAX)
    {
        return INT16_MAX;
    }
    const double rounded = (double)value + 0.5;
    const int32_t truncated = (int32_t)rounded;
    return ((double)truncated > rounded) ? truncated - 1 : truncated;
}

void erb_rt_pset_float(uint16_t prop, float value)
{
    const ErbPropInfo* info = erb_prop_info(prop);
    switch (info->kind)
    {
        case ERB_PK_FLOAT:
        {
            uint32_t bits;
            memcpy(&bits, &value, sizeof(bits));
            store(info, bits);
            break;
        }
        case ERB_PK_OPACITY:
            /* As app_opacity: 0 at or below 0 (and NaN), 255 at or above 1, else rounded. */
            store(info,
                  !(value > 0.0f) ? 0u
                  : value >= 1.0f ? ERB_OPACITY_OPAQUE
                                  : (uint32_t)(value * ERB_OPACITY_OPAQUE + 0.5f));
            break;
        default:
            erb_rt_pset_int(prop, round_dim(value));
            break;
    }
}

bool erb_rt_pset_string(uint16_t prop, const char* value)
{
    const ErbPropInfo* info = erb_prop_info(prop);
    char* field = (char*)&g_erb_props + info->offset;
    const size_t length = strlen(value);
    const size_t take = length < info->max ? length : info->max;
    memcpy(field, value, take);
    field[take] = '\0';
    return take == length && (info->kind != ERB_PK_IMAGE || take == 0u || er_image_exists(field));
}

/*----------------------------------------------------------------------------------------------------------------------
 - Events
 ---------------------------------------------------------------------------------------------------------------------*/

static void on_event(ERNode* node, const EREventData* data, void* user_data);
static bool on_query(ERNode* node, const EREventData* data, void* user_data);
static void dispatch(const ErbEvent* event);

static ErbStatus queue_insert(uint32_t position, const ErbEvent* event)
{
    const uint32_t capacity = g_erb.program.regions[ERB_REGION_EVENTS].count;
    if (s_queue_count >= capacity)
    {
        return erb_rt_fault(ERB_ERR_QUEUE_FULL, "events keep raising each other", NULL, ERB_NONE, 0u);
    }
    for (uint32_t index = s_queue_count; index > position; index--)
    {
        *queue_record(index) = *queue_record(index - 1u);
    }
    *queue_record(position) = *event;
    s_queue_count++;
    return ERB_OK;
}

static void queue_pop(ErbEvent* event)
{
    *event = *queue_record(0);
    const uint32_t capacity = g_erb.program.regions[ERB_REGION_EVENTS].count;
    s_queue_head = (s_queue_head + 1u) % capacity;
    s_queue_count--;
    if (s_queue_effects)
    {
        s_queue_effects--;
    }
}

ErbStatus erb_rt_queue_effect(uint16_t function)
{
    ErbEvent event;
    memset(&event, 0, sizeof(event));
    event.function = function;
    event.kind = ERB_EVENT_EFFECT;
    const ErbStatus status = queue_insert(s_queue_effects, &event);
    if (status == ERB_OK)
    {
        s_queue_effects++;
    }
    return status;
}

/** @brief Runs one function with the VM, as one run with its own budget. */
static ErbStatus run(uint16_t function, int32_t instance, int32_t* result)
{
    g_erb.in_vm = true;
    const ErbStatus status = erb_vm_run(function, &instance, result);
    g_erb.in_vm = false;
    return status;
}

static bool any_dirty(void)
{
    const uint32_t* words = dirty_words();
    for (uint32_t index = 0; index < g_erb.program.dirty_words; index++)
    {
        if (words[index])
        {
            return true;
        }
    }
    return false;
}

/** @brief Update passes until nothing is dirty, running the effects each pass queues. */
static ErbStatus settle(void)
{
    uint32_t passes = 0;
    for (;;)
    {
        while (g_erb.program.dirty_words && any_dirty())
        {
            if (++passes > ERB_MAX_UPDATE_PASSES)
            {
                return erb_rt_fault(ERB_ERR_UPDATE_LOOP, "state keeps changing in effects", NULL, ERB_NONE, 0u);
            }
            /* The pass reads its own copy, so what it sets lands in the next pass. */
            uint32_t* words = dirty_words();
            memcpy(words + g_erb.program.dirty_words, words, g_erb.program.dirty_words * sizeof(uint32_t));
            memset(words, 0, g_erb.program.dirty_words * sizeof(uint32_t));
            if (g_erb.program.update_function != ERB_NONE)
            {
                const ErbStatus status = run(g_erb.program.update_function, 0, NULL);
                if (status != ERB_OK)
                {
                    return status;
                }
            }
        }
        if (!s_queue_effects)
        {
            return ERB_OK;
        }
        ErbEvent effect;
        queue_pop(&effect);
        const ErbStatus status = run(effect.function, 0, NULL);
        if (status != ERB_OK)
        {
            return status;
        }
    }
}

/** @brief Runs one event to its end: its function, the update passes, and their effects. */
static ErbStatus run_event(const ErbEvent* event)
{
    ErbStatus status = ERB_OK;
    if (event->kind != ERB_EVENT_UPDATE)
    {
        g_erb.event = *event;
        status = run(event->function, event->instance, NULL);
    }
    return status == ERB_OK ? settle() : status;
}

static void dispatch(const ErbEvent* event)
{
    if (!g_erb.loaded || g_erb.faulted)
    {
        return;
    }
    if (g_erb.in_vm)
    {
        queue_insert(s_queue_count, event);
        return;
    }
    if (run_event(event) != ERB_OK)
    {
        return;
    }
    while (s_queue_count && !g_erb.faulted)
    {
        ErbEvent next;
        queue_pop(&next);
        if (run_event(&next) != ERB_OK)
        {
            return;
        }
    }
}

static void on_event(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    if (!g_erb.loaded || g_erb.faulted)
    {
        return;
    }
    const uintptr_t packed = (uintptr_t)user_data;
    ErbEvent event;
    memset(&event, 0, sizeof(event));
    event.function = ERB_LOW16(packed);
    event.instance = ERB_HIGH16(packed);
    event.kind = ERB_EVENT_CALL;
    const uint8_t kind = erb_function(&g_erb.program, event.function)[ERB_FUNCTION_KIND_AT];
    if (kind == ER_EVENT_LAYOUT)
    {
        event.x = (int16_t)data->layout_rect.x;
        event.y = (int16_t)data->layout_rect.y;
        event.dx = (int16_t)data->layout_rect.w;
        event.dy = (int16_t)data->layout_rect.h;
    }
    else
    {
        event.x = (int16_t)data->x;
        event.y = (int16_t)data->y;
        event.dx = (int16_t)data->dx;
        event.dy = (int16_t)data->dy;
    }
    event.motion_x = kind == ER_EVENT_SCROLL ? data->scroll_x : data->vx;
    event.motion_y = kind == ER_EVENT_SCROLL ? data->scroll_y : data->vy;
    event.value = data->value;
    event.value_start = data->value_start;
    event.touches = (uint16_t)er_touch_active_count();
    dispatch(&event);
}

static bool on_query(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    /* A query answers now or not at all, so one that arrives while the VM runs says no. */
    if (!g_erb.loaded || g_erb.faulted || g_erb.in_vm)
    {
        return false;
    }
    const uintptr_t packed = (uintptr_t)user_data;
    memset(&g_erb.event, 0, sizeof(g_erb.event));
    g_erb.event.function = ERB_LOW16(packed);
    g_erb.event.instance = ERB_HIGH16(packed);
    g_erb.event.x = (int16_t)data->x;
    g_erb.event.y = (int16_t)data->y;
    g_erb.event.dx = (int16_t)data->dx;
    g_erb.event.dy = (int16_t)data->dy;
    g_erb.event.motion_x = data->vx;
    g_erb.event.motion_y = data->vy;
    g_erb.event.touches = (uint16_t)er_touch_active_count();
    int32_t answer = 0;
    if (run(g_erb.event.function, g_erb.event.instance, &answer) != ERB_OK)
    {
        return false;
    }
    ErbEvent update;
    memset(&update, 0, sizeof(update));
    update.kind = ERB_EVENT_UPDATE;
    dispatch(&update);
    return answer != 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Nodes and templates
 ---------------------------------------------------------------------------------------------------------------------*/

void erb_rt_load_paints(uint16_t tape)
{
    const ErbProgram* program = &g_erb.program;
    const uint8_t* record = erb_tape(program, tape);
    const uint16_t paint_first = erb_u16(record + ERB_TAPE_PAINT_FIRST_AT);
    const uint16_t paint_count = erb_u16(record + ERB_TAPE_PAINT_COUNT_AT);
    const uint16_t gradient_first = erb_u16(record + ERB_TAPE_GRADIENT_FIRST_AT);
    const uint16_t gradient_count = erb_u16(record + ERB_TAPE_GRADIENT_COUNT_AT);
    for (uint32_t index = 0; index < paint_count; index++)
    {
        const uint8_t* paint = program->paints + (paint_first + index) * ERB_PAINT_SIZE;
        ERVectorPaint* out = &s_paints[index];
        out->fill = erb_u32(paint + ERB_PAINT_FILL_AT);
        out->stroke = erb_u32(paint + ERB_PAINT_STROKE_AT);
        out->stroke_w = erb_f32(paint + ERB_PAINT_STROKE_WIDTH_AT);
        out->miter = erb_f32(paint + ERB_PAINT_MITER_AT);
        out->cap = paint[ERB_PAINT_CAP_AT];
        out->join = paint[ERB_PAINT_JOIN_AT];
        out->fill_rule = paint[ERB_PAINT_FILL_RULE_AT];
        out->fill_grad = (int16_t)erb_u16(paint + ERB_PAINT_FILL_GRADIENT_AT);
        out->stroke_grad = (int16_t)erb_u16(paint + ERB_PAINT_STROKE_GRADIENT_AT);
    }
    for (uint32_t index = 0; index < gradient_count; index++)
    {
        const uint8_t* gradient = program->gradients + (gradient_first + index) * ERB_GRADIENT_SIZE;
        ERVectorGradient* out = &s_gradients[index];
        memset(out, 0, sizeof(*out));
        out->type = gradient[ERB_GRADIENT_TYPE_AT];
        out->stop_count = gradient[ERB_GRADIENT_STOP_COUNT_AT];
        for (uint32_t stop = 0; stop < out->stop_count; stop++)
        {
            const uint8_t* record_stop =
                program->stops + (erb_u16(gradient + ERB_GRADIENT_STOP_FIRST_AT) + stop) * ERB_STOP_SIZE;
            out->stops[stop].color = erb_u32(record_stop + ERB_STOP_COLOR_AT);
            out->stops[stop].position = erb_f32(record_stop + ERB_STOP_POSITION_AT);
        }
        out->ax = erb_f32(gradient + ERB_GRADIENT_AX_AT);
        out->ay = erb_f32(gradient + ERB_GRADIENT_AY_AT);
        out->bx = erb_f32(gradient + ERB_GRADIENT_BX_AT);
        out->by = erb_f32(gradient + ERB_GRADIENT_BY_AT);
        out->r = erb_f32(gradient + ERB_GRADIENT_RADIUS_AT);
    }
}

void erb_rt_set_paint(uint8_t paint, uint8_t field, const int32_t* cell)
{
    ERVectorPaint* out = &s_paints[paint];
    switch (field)
    {
        case ERB_VPAINT_FILL:
            out->fill = (uint32_t)*cell;
            break;
        case ERB_VPAINT_STROKE:
            out->stroke = (uint32_t)*cell;
            break;
        case ERB_VPAINT_STROKE_WIDTH:
            memcpy(&out->stroke_w, cell, sizeof(out->stroke_w));
            break;
        case ERB_VPAINT_FILL_GRADIENT:
            out->fill_grad = (int16_t)*cell;
            break;
        default:
            out->stroke_grad = (int16_t)*cell;
            break;
    }
}

void erb_rt_set_gradient(uint8_t gradient, uint8_t field, const int32_t* cell)
{
    ERVectorGradient* out = &s_gradients[gradient];
    float* const geometry[] = {&out->ax, &out->ay, &out->bx, &out->by, &out->r};
    if (field <= ERB_VGRAD_RADIUS)
    {
        memcpy(geometry[field], cell, sizeof(float));
        return;
    }
    ERGradientStop* stop = &out->stops[(field - ERB_VGRAD_STOPS) / 2u];
    if ((field - ERB_VGRAD_STOPS) % 2u)
    {
        memcpy(&stop->position, cell, sizeof(stop->position));
    }
    else
    {
        stop->color = (uint32_t)*cell;
    }
}

void erb_rt_commit_tape(ERNode* node, uint16_t tape, const float* floats, bool own_paints)
{
    const uint8_t* record = erb_tape(&g_erb.program, tape);
    if (!own_paints)
    {
        erb_rt_load_paints(tape);
    }
    er_node_set_vector_ops(node,
                           floats,
                           erb_u16(record + ERB_TAPE_FLOAT_COUNT_AT),
                           s_paints,
                           erb_u16(record + ERB_TAPE_PAINT_COUNT_AT),
                           s_gradients,
                           erb_u16(record + ERB_TAPE_GRADIENT_COUNT_AT));
}

/** @brief Wires one template node's bindings. */
/** @brief Binds an animated value to @p prop through interpolation @p index, copied out of the package. */
static void bind_interpolated(ERNode* node, ERAnimProp prop, uint16_t index)
{
    const uint8_t* interp = erb_interp(&g_erb.program, index);
    uint32_t handle;
    memcpy(&handle, slot_data(erb_u16(interp + ERB_INTERP_SLOT_AT)), sizeof(handle));
    const uint8_t points = interp[ERB_INTERP_POINT_COUNT_AT];
    const ERExtrapolate left = (ERExtrapolate)interp[ERB_INTERP_EXTRAPOLATE_LEFT_AT];
    const ERExtrapolate right = (ERExtrapolate)interp[ERB_INTERP_EXTRAPOLATE_RIGHT_AT];
    if (interp[ERB_INTERP_FLAGS_AT] & ERB_INTERP_FLAG_COLOR)
    {
        ERColorInterpolation colors;
        memset(&colors, 0, sizeof(colors));
        for (uint8_t point = 0; point < points; point++)
        {
            colors.input_range[point] = erb_f32(interp + ERB_INTERP_INPUT_AT + point * 4u);
            colors.output_range[point] = erb_u32(interp + ERB_INTERP_OUTPUT_AT + point * 4u);
        }
        colors.point_count = points;
        colors.extrapolate_left = left;
        colors.extrapolate_right = right;
        er_anim_value_bind_color((ERAnimValueHandle)handle, node, prop, &colors);
        return;
    }
    ERInterpolation mapping;
    memset(&mapping, 0, sizeof(mapping));
    for (uint8_t point = 0; point < points; point++)
    {
        mapping.input_range[point] = erb_f32(interp + ERB_INTERP_INPUT_AT + point * 4u);
        mapping.output_range[point] = erb_f32(interp + ERB_INTERP_OUTPUT_AT + point * 4u);
    }
    mapping.point_count = points;
    mapping.extrapolate_left = left;
    mapping.extrapolate_right = right;
    er_anim_value_bind_interpolated((ERAnimValueHandle)handle, node, prop, &mapping);
}

static void bind(ERNode* node, const uint8_t* record, uint32_t instance)
{
    const ErbProgram* program = &g_erb.program;
    const uint16_t first = erb_u16(record + ERB_NODE_BINDINGS_AT);
    for (uint32_t index = 0; index < record[ERB_NODE_BINDING_COUNT_AT]; index++)
    {
        const uint8_t* binding = program->bindings + (first + index) * ERB_BINDING_SIZE;
        const uint16_t target = erb_u16(binding + ERB_BINDING_TARGET_AT);
        void* user_data = (void*)(uintptr_t)ERB_PACK16(target, instance);
        switch (binding[ERB_BINDING_KIND_AT])
        {
            case ERB_BIND_EVENT:
                er_event_set(node, (EREventType)binding[ERB_BINDING_EVENT_AT], on_event, user_data);
                break;
            case ERB_BIND_QUERY:
                er_responder_query_set(node, (ERResponderQuery)binding[ERB_BINDING_EVENT_AT], on_query, user_data);
                break;
            case ERB_BIND_ANIMATED:
            {
                uint32_t handle;
                memcpy(&handle, slot_data(target), sizeof(handle));
                er_anim_value_bind(
                    (ERAnimValueHandle)handle, node, (ERAnimProp)erb_prop_info(binding[ERB_BINDING_EVENT_AT])->anim);
                break;
            }
            case ERB_BIND_INTERPOLATED:
                bind_interpolated(node, (ERAnimProp)erb_prop_info(binding[ERB_BINDING_EVENT_AT])->anim, target);
                break;
            default:
            {
                const uint8_t* tape = erb_tape(program, target);
                erb_rt_commit_tape(
                    node, target, (const float*)(program->tapes + erb_u32(tape + ERB_TAPE_FLOATS_AT)), false);
                break;
            }
        }
    }
}

ErbStatus erb_rt_build(uint16_t template_index, ERNode* parent, ERNode* before, int32_t instance, ERNode** out)
{
    const ErbProgram* program = &g_erb.program;
    const uint8_t* template_record = erb_template(program, template_index);
    const uint16_t first = erb_u16(template_record + ERB_TEMPLATE_FIRST_NODE_AT);
    const uint16_t count = erb_u16(template_record + ERB_TEMPLATE_NODE_COUNT_AT);
    ERNode* parents[ERB_MAX_TEMPLATE_DEPTH];
    uint16_t owed[ERB_MAX_TEMPLATE_DEPTH];
    uint32_t depth = 0;
    *out = NULL;
    for (uint32_t index = 0; index < count; index++)
    {
        const uint8_t* record = program->nodes + (first + index) * ERB_NODE_SIZE;
        ERNode* node = er_node_create((ERNodeType)record[ERB_NODE_TYPE_AT]);
        if (!node)
        {
            return erb_rt_fault(ERB_ERR_OUT_OF_NODES, "the engine's node pool is empty", NULL, ERB_NONE, 0u);
        }
        erb_rt_props(erb_u16(record + ERB_NODE_PROPS_AT));
        er_node_set_props(node, &g_erb_props);

        const uint16_t handle = erb_u16(record + ERB_NODE_HANDLE_AT);
        if (handle != ERB_NONE)
        {
            const uint16_t capacity = erb_u16(erb_slot(program, handle) + ERB_SLOT_CAP_AT);
            if (capacity && (instance < 0 || (uint32_t)instance >= capacity))
            {
                er_node_destroy(node);
                return erb_rt_fault(ERB_ERR_LIST_INDEX, "row instance past its handle array", NULL, ERB_NONE, 0u);
            }
            int32_t cells[2];
            erb_rt_set_node(cells, node);
            memcpy(slot_data(handle) + (capacity ? (uint32_t)instance * 8u : 0u), cells, sizeof(cells));
        }
        bind(node, record, ERB_LOW16(instance));

        if (index == 0u)
        {
            *out = node;
            if (parent && before)
            {
                er_tree_insert_before(parent, node, before);
            }
            else if (parent)
            {
                er_tree_append_child(parent, node);
            }
        }
        else
        {
            owed[depth - 1u]--;
            er_tree_append_child(parents[depth - 1u], node);
        }
        const uint16_t children = erb_u16(record + ERB_NODE_CHILDREN_AT);
        if (children)
        {
            parents[depth] = node;
            owed[depth] = children;
            depth++;
        }
        while (depth && owed[depth - 1u] == 0u)
        {
            depth--;
        }
    }
    return ERB_OK;
}

void erb_rt_destroy(ERNode* node)
{
    /* Leaves first, without recursion: walk down to a childless node, unlink it and destroy it, again. */
    for (;;)
    {
        ERNode* parent = NULL;
        ERNode* leaf = node;
        for (ERNode* child = er_node_first_child(leaf); child; child = er_node_first_child(leaf))
        {
            parent = leaf;
            leaf = child;
        }
        if (!parent)
        {
            break;
        }
        er_tree_remove_child(parent, leaf);
        er_node_destroy(leaf);
    }
    ERNode* parent = er_node_parent(node);
    if (parent)
    {
        er_tree_remove_child(parent, node);
    }
    er_node_destroy(node);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Timers
 ---------------------------------------------------------------------------------------------------------------------*/

int32_t erb_rt_timer_start(uint16_t function, int32_t interval_ms, bool repeat)
{
    const uint32_t count = g_erb.program.regions[ERB_REGION_TIMERS].count;
    ErbTimer* table = timers();
    for (uint32_t index = 0; index < count; index++)
    {
        ErbTimer* timer = &table[index];
        if (!timer->active)
        {
            timer->interval_ms = interval_ms < 1 ? 1 : interval_ms;
            timer->remaining_ms = timer->interval_ms;
            /* An id carries the slot and the generation that owns it, so clearing a finished timer's id can't
             * stop whatever took the slot next. */
            timer->generation = (uint16_t)((timer->generation % TIMER_GENERATIONS) + 1u);
            timer->function = function;
            timer->repeat = repeat;
            timer->active = 1u;
            return (int32_t)(timer->generation * count + index);
        }
    }
    return -1;
}

void erb_rt_timer_clear(int32_t id)
{
    const uint32_t count = g_erb.program.regions[ERB_REGION_TIMERS].count;
    if (id < 0 || count == 0u)
    {
        return;
    }
    ErbTimer* timer = &timers()[(uint32_t)id % count];
    if (timer->active && timer->generation == (uint32_t)id / count)
    {
        timer->active = 0u;
    }
}

static void tick_timers(uint32_t elapsed_ms)
{
    const uint32_t count = g_erb.program.regions[ERB_REGION_TIMERS].count;
    ErbTimer* table = timers();
    for (uint32_t index = 0; index < count && g_erb.loaded && !g_erb.faulted; index++)
    {
        ErbTimer* timer = &table[index];
        if (!timer->active)
        {
            continue;
        }
        timer->remaining_ms -= (int32_t)elapsed_ms;
        if (timer->remaining_ms > 0)
        {
            continue;
        }
        if (timer->repeat)
        {
            timer->remaining_ms += timer->interval_ms;
            if (timer->remaining_ms <= 0)
            {
                timer->remaining_ms = timer->interval_ms; /* dt ran long; don't spiral */
            }
        }
        else
        {
            timer->active = 0u;
        }
        ErbEvent event;
        memset(&event, 0, sizeof(event));
        event.function = timer->function;
        event.kind = ERB_EVENT_CALL;
        dispatch(&event);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - The animation runner
 *
 * ASTART lays a descriptor tree out as one block of entries in the arena, the root first and each group's
 * children after it (the order the verifier walked it in). Leaves run on the engine; groups are run here,
 * from the engine's completion callbacks.
 ---------------------------------------------------------------------------------------------------------------------*/

static void anim_start_entry(uint32_t index);

static uint32_t anim_count(void)
{
    return g_erb.program.regions[ERB_REGION_ANIMS].count;
}

/** @brief The @p nth child entry of entry @p index (children follow their parent in its block). */
static uint32_t anim_child(uint32_t index, uint32_t nth)
{
    ErbAnimEntry* entries = anim_entries();
    for (uint32_t child = index + 1u; child < anim_count() && !entries[child].first; child++)
    {
        if (entries[child].parent == index && nth-- == 0u)
        {
            return child;
        }
    }
    return index; /* unreachable for a verified tree */
}

static void anim_free_block(uint32_t root)
{
    ErbAnimEntry* entries = anim_entries();
    entries[root].state = ANIM_FREE;
    entries[root].first = 0u;
    for (uint32_t index = root + 1u; index < anim_count() && !entries[index].first && entries[index].state != ANIM_FREE;
         index++)
    {
        entries[index].state = ANIM_FREE;
    }
}

static void anim_complete(uint32_t index, bool finished)
{
    ErbAnimEntry* entries = anim_entries();
    ErbAnimEntry* entry = &entries[index];
    entry->state = ANIM_DONE;
    if (entry->parent == ERB_NONE)
    {
        const uint16_t done = entry->done_fn;
        anim_free_block(index);
        if (done != ERB_NONE)
        {
            ErbEvent event;
            memset(&event, 0, sizeof(event));
            event.function = done;
            event.kind = ERB_EVENT_CALL;
            event.finished = finished;
            dispatch(&event);
        }
        return;
    }
    const uint32_t parent_index = entry->parent;
    ErbAnimEntry* parent = &entries[parent_index];
    if (parent->state != ANIM_RUNNING)
    {
        return;
    }
    const uint8_t* descriptor = erb_anim(&g_erb.program, parent->descriptor);
    const uint16_t children = erb_u16(descriptor + ERB_ANIM_COUNT_AT);
    switch (descriptor[ERB_ANIM_KIND_AT])
    {
        case ERB_ANIM_SEQUENCE:
            if (!finished)
            {
                anim_complete(parent_index, false);
            }
            else if (++parent->step < children)
            {
                anim_start_entry(anim_child(parent_index, parent->step));
            }
            else
            {
                anim_complete(parent_index, true);
            }
            break;
        case ERB_ANIM_LOOP:
        {
            const uint32_t iterations = erb_u32(descriptor + ERB_ANIM_AMOUNT_AT);
            if (!finished)
            {
                anim_complete(parent_index, false);
                break;
            }
            parent->passes++;
            if (iterations != 0u && parent->passes >= iterations)
            {
                anim_complete(parent_index, true);
                break;
            }
            /* A loop of an animated value starts each pass from where the first one did. */
            const uint32_t child = anim_child(parent_index, 0u);
            const uint8_t* child_descriptor = erb_anim(&g_erb.program, entries[child].descriptor);
            const uint16_t target = erb_u16(child_descriptor + ERB_ANIM_TARGET_AT);
            if (child_descriptor[ERB_ANIM_KIND_AT] <= ERB_ANIM_DECAY && target != ERB_NONE
                && erb_slot(&g_erb.program, target)[ERB_SLOT_TYPE_AT] == ERB_T_AVALUE)
            {
                uint32_t handle;
                memcpy(&handle, slot_data(target), sizeof(handle));
                er_anim_value_set((ERAnimValueHandle)handle, parent->value);
            }
            anim_start_entry(child);
            break;
        }
        default:
            /* Parallel and stagger: done when every child is, and not finished as soon as one stops, as the C
               output ends a joined run. The rest run on. */
            if (!finished)
            {
                anim_complete(parent_index, false);
            }
            else if (--parent->step == 0u)
            {
                anim_complete(parent_index, true);
            }
            break;
    }
}

static void anim_leaf_done(bool finished, void* user_data)
{
    if (!g_erb.loaded)
    {
        return;
    }
    const uintptr_t packed = (uintptr_t)user_data;
    const uint32_t index = ERB_LOW16(packed);
    ErbAnimEntry* entry = &anim_entries()[index];
    if (index < anim_count() && entry->generation == ERB_HIGH16(packed) && entry->state == ANIM_RUNNING)
    {
        anim_complete(index, finished);
    }
}

static void anim_start_entry(uint32_t index)
{
    const ErbProgram* program = &g_erb.program;
    ErbAnimEntry* entries = anim_entries();
    ErbAnimEntry* entry = &entries[index];
    const uint8_t* descriptor = erb_anim(program, entry->descriptor);
    const uint8_t kind = descriptor[ERB_ANIM_KIND_AT];
    const uint16_t target = erb_u16(descriptor + ERB_ANIM_TARGET_AT);
    const uint16_t children = erb_u16(descriptor + ERB_ANIM_COUNT_AT);
    entry->state = ANIM_RUNNING;
    entry->step = 0u;
    entry->passes = 0u;

    if (kind <= ERB_ANIM_DECAY)
    {
        if (target == ERB_NONE)
        {
            entry->state = ANIM_DELAY;
            entry->value = (float)erb_u32(descriptor + ERB_ANIM_AMOUNT_AT);
            return;
        }
        ERAnimConfig config;
        memset(&config, 0, sizeof(config));
        config.type = (ERAnimType)(kind - ERB_ANIM_TIMING);
        config.easing = (ERAnimEasing)descriptor[ERB_ANIM_EASING_AT];
        config.duration_ms = erb_u32(descriptor + ERB_ANIM_AMOUNT_AT);
        config.delay_ms = erb_u32(descriptor + ERB_ANIM_DELAY_MS_AT);
        const float param0 = erb_f32(descriptor + ERB_ANIM_PARAM0_AT);
        const float param1 = erb_f32(descriptor + ERB_ANIM_PARAM1_AT);
        const float param2 = erb_f32(descriptor + ERB_ANIM_PARAM2_AT);
        const float param3 = erb_f32(descriptor + ERB_ANIM_PARAM3_AT);
        if (kind == ERB_ANIM_SPRING)
        {
            config.stiffness = param0;
            config.damping = param1;
            config.mass = param2;
            config.velocity = param3;
        }
        else if (kind == ERB_ANIM_DECAY)
        {
            config.velocity = param0;
            config.deceleration = param1;
        }
        else
        {
            config.bezier_x1 = param0;
            config.bezier_y1 = param1;
            config.bezier_x2 = param2;
            config.bezier_y2 = param3;
        }
        config.loop = (descriptor[ERB_ANIM_FLAGS_AT] & ERB_ANIM_FLAG_LOOP) != 0u;
        config.loop_reverse = (descriptor[ERB_ANIM_FLAGS_AT] & ERB_ANIM_FLAG_LOOP_REVERSE) != 0u;
        config.on_complete = anim_leaf_done;
        config.on_complete_user_data = (void*)(uintptr_t)ERB_PACK16(index, entry->generation);

        ERAnimHandle handle = ER_ANIM_HANDLE_INVALID;
        const uint8_t* slot = erb_slot(program, target);
        if (slot[ERB_SLOT_TYPE_AT] == ERB_T_AVALUE)
        {
            uint32_t value;
            memcpy(&value, slot_data(target), sizeof(value));
            handle = er_anim_value_animate((ERAnimValueHandle)value, entry->value, &config);
        }
        else
        {
            int32_t cells[2];
            memcpy(cells, slot_data(target), sizeof(cells));
            ERNode* node = erb_rt_node(cells);
            if (node)
            {
                handle = er_anim_start(node,
                                       (ERAnimProp)erb_prop_info(erb_u16(descriptor + ERB_ANIM_PROP_AT))->anim,
                                       entry->value,
                                       &config);
            }
            else if (cells[0] != 0 && (program->flags & ERB_FLAG_DEBUG))
            {
                /* An empty handle only means the node isn't built yet; this one outlived its node. */
                erb_rt_fault(ERB_ERR_STALE_NODE, "a node handle outlived its node", NULL, ERB_NONE, 0u);
                return;
            }
        }
        /* An engine that already called back (a 0 ms animation) has moved the entry on. */
        if (entry->state == ANIM_RUNNING)
        {
            entry->engine = handle;
            if (handle == ER_ANIM_HANDLE_INVALID)
            {
                erb_rt_report("animation not started: no node, or the engine's pool is full", ERB_NONE, 0u);
                anim_complete(index, false);
            }
        }
        return;
    }

    if (kind == ERB_ANIM_SEQUENCE || kind == ERB_ANIM_LOOP)
    {
        const uint32_t first = anim_child(index, 0u);
        if (kind == ERB_ANIM_LOOP)
        {
            const uint8_t* child_descriptor = erb_anim(program, entries[first].descriptor);
            const uint16_t child_target = erb_u16(child_descriptor + ERB_ANIM_TARGET_AT);
            if (child_descriptor[ERB_ANIM_KIND_AT] <= ERB_ANIM_DECAY && child_target != ERB_NONE
                && erb_slot(program, child_target)[ERB_SLOT_TYPE_AT] == ERB_T_AVALUE)
            {
                uint32_t handle;
                memcpy(&handle, slot_data(child_target), sizeof(handle));
                entry->value = er_anim_value_get((ERAnimValueHandle)handle);
            }
        }
        anim_start_entry(first);
        return;
    }

    /* Parallel and stagger: every child at once, a stagger's each n ms after the one before. */
    entry->step = children;
    const uint32_t gap = erb_u32(descriptor + ERB_ANIM_AMOUNT_AT);
    for (uint32_t nth = 0; nth < children && entry->state == ANIM_RUNNING; nth++)
    {
        const uint32_t child = anim_child(index, nth);
        if (kind == ERB_ANIM_STAGGER && nth > 0u)
        {
            entries[child].state = ANIM_WAIT;
            entries[child].value = (float)(gap * nth);
        }
        else
        {
            anim_start_entry(child);
        }
    }
}

uint32_t erb_rt_anim_start(uint16_t descriptor, uint16_t done_function, const int32_t* to_cells)
{
    const ErbProgram* program = &g_erb.program;
    /* The tree, root first and each group's children after it, as the verifier walked it. */
    uint32_t size = 1;
    s_tree[0] = descriptor;
    s_tree_parent[0] = TREE_NO_PARENT;
    for (uint32_t index = 0; index < size; index++)
    {
        const uint8_t* anim = erb_anim(program, s_tree[index]);
        if (anim[ERB_ANIM_KIND_AT] <= ERB_ANIM_DECAY)
        {
            continue;
        }
        for (uint16_t child = 0; child < erb_u16(anim + ERB_ANIM_COUNT_AT); child++)
        {
            s_tree[size] = (uint16_t)(erb_u16(anim + ERB_ANIM_FIRST_AT) + child);
            s_tree_parent[size] = (uint8_t)index;
            size++;
        }
    }
    /* First fit: a free run of entries as long as the tree. */
    ErbAnimEntry* entries = anim_entries();
    uint32_t block = 0;
    uint32_t run_length = 0;
    for (uint32_t index = 0; index < anim_count() && run_length < size; index++)
    {
        run_length = entries[index].state == ANIM_FREE ? run_length + 1u : 0u;
        block = index + 1u - run_length;
    }
    if (run_length < size)
    {
        return 0u;
    }
    s_anim_generation = (uint16_t)(s_anim_generation == UINT16_MAX ? 1u : s_anim_generation + 1u);
    uint32_t to_index = 0;
    for (uint32_t index = 0; index < size; index++)
    {
        const uint8_t* anim = erb_anim(program, s_tree[index]);
        ErbAnimEntry* entry = &entries[block + index];
        memset(entry, 0, sizeof(*entry));
        entry->descriptor = s_tree[index];
        entry->parent = index ? (uint16_t)(block + s_tree_parent[index]) : ERB_NONE;
        entry->generation = s_anim_generation;
        entry->done_fn = index ? ERB_NONE : done_function;
        entry->first = index == 0u;
        entry->state = ANIM_DONE;
        uint32_t to_bits = erb_u32(anim + ERB_ANIM_TO_AT);
        if (anim[ERB_ANIM_KIND_AT] <= ERB_ANIM_DECAY && (anim[ERB_ANIM_FLAGS_AT] & ERB_ANIM_FLAG_TO_REGISTER)
            && to_cells)
        {
            to_bits = (uint32_t)to_cells[to_index++];
        }
        memcpy(&entry->value, &to_bits, sizeof(to_bits));
    }
    anim_start_entry(block);
    return ERB_PACK16(block, s_anim_generation);
}

void erb_rt_anim_stop(uint32_t handle)
{
    const uint32_t root = ERB_LOW16(handle);
    ErbAnimEntry* entries = anim_entries();
    if (handle == 0u || root >= anim_count() || !entries[root].first || entries[root].generation != ERB_HIGH16(handle)
        || entries[root].state == ANIM_FREE)
    {
        return;
    }
    /* Mark the tree stopped first, so the callbacks er_anim_stop makes find nothing to move on. */
    uint32_t end = root + 1u;
    while (end < anim_count() && !entries[end].first && entries[end].state != ANIM_FREE)
    {
        end++;
    }
    for (uint32_t index = root; index < end; index++)
    {
        const bool running = entries[index].state == ANIM_RUNNING;
        entries[index].state = ANIM_STOPPED;
        if (running && entries[index].engine != ER_ANIM_HANDLE_INVALID)
        {
            er_anim_stop(entries[index].engine);
        }
    }
    entries[root].parent = ERB_NONE;
    anim_complete(root, false);
}

static void tick_anims(uint32_t elapsed_ms)
{
    /* Every wait counts down before any that ran out moves on, so a wait one of them starts waits its full time,
       as the C output's completion waits do. */
    const uint32_t count = g_erb.program.regions[ERB_REGION_ANIMS].count;
    ErbAnimEntry* entries = anim_entries();
    for (uint32_t index = 0; index < count; index++)
    {
        ErbAnimEntry* entry = &entries[index];
        if (entry->state != ANIM_DELAY && entry->state != ANIM_WAIT)
        {
            continue;
        }
        entry->value -= (float)elapsed_ms;
        if (entry->value <= 0.0f)
        {
            entry->state = entry->state == ANIM_DELAY ? ANIM_DELAY_DUE : ANIM_WAIT_DUE;
        }
    }
    for (uint32_t index = 0; index < count && g_erb.loaded && !g_erb.faulted; index++)
    {
        ErbAnimEntry* entry = &entries[index];
        if (entry->state == ANIM_DELAY_DUE)
        {
            entry->state = ANIM_RUNNING;
            anim_complete(index, true);
        }
        else if (entry->state == ANIM_WAIT_DUE)
        {
            anim_start_entry(index);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Loading and teardown
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Hands the engine the package's keyboard, copied into its structs with the strings left in place. */
static void apply_keyboard(void)
{
#if ERUI_ONSCREEN_KEYBOARD
    const ErbProgram* program = &g_erb.program;
    const uint8_t* table = program->keyboard;
    if (!table)
    {
        return;
    }
    const uint16_t layers = erb_u16(table + ERB_KEYBOARD_LAYER_COUNT_AT);
    const uint16_t rows = erb_u16(table + ERB_KEYBOARD_ROW_COUNT_AT);
    const uint16_t keys = erb_u16(table + ERB_KEYBOARD_KEY_COUNT_AT);
    const uint8_t* layer_groups = table + ERB_KEYBOARD_HEADER_SIZE;
    const uint8_t* row_groups = layer_groups + layers * ERB_KEYBOARD_GROUP_SIZE;
    const uint8_t* key_records = row_groups + rows * ERB_KEYBOARD_GROUP_SIZE;
    for (uint32_t index = 0; index < keys; index++)
    {
        const uint8_t* key = key_records + index * ERB_KEYBOARD_KEY_SIZE;
        const uint16_t label = erb_u16(key + ERB_KEY_LABEL_AT);
        const uint16_t text = erb_u16(key + ERB_KEY_TEXT_AT);
        s_keyboard_keys[index] = (ERKeyboardKey){
            .label = label == ERB_NONE ? NULL : erb_string(program, label),
            .text = text == ERB_NONE ? NULL : erb_string(program, text),
            .type = key[ERB_KEY_TYPE_AT],
            .layer = key[ERB_KEY_LAYER_AT],
            .span = key[ERB_KEY_SPAN_AT],
            .highlight_layer = key[ERB_KEY_HIGHLIGHT_AT],
        };
    }
    for (uint32_t index = 0; index < rows; index++)
    {
        const uint8_t* group = row_groups + index * ERB_KEYBOARD_GROUP_SIZE;
        s_keyboard_rows[index].keys = &s_keyboard_keys[erb_u16(group + ERB_KEYBOARD_FIRST_AT)];
        s_keyboard_rows[index].count = (uint8_t)erb_u16(group + ERB_KEYBOARD_COUNT_AT);
    }
    for (uint32_t index = 0; index < layers; index++)
    {
        const uint8_t* group = layer_groups + index * ERB_KEYBOARD_GROUP_SIZE;
        s_keyboard_layers[index].rows = &s_keyboard_rows[erb_u16(group + ERB_KEYBOARD_FIRST_AT)];
        s_keyboard_layers[index].count = (uint8_t)erb_u16(group + ERB_KEYBOARD_COUNT_AT);
    }
    s_keyboard = (ERKeyboardConfig){
        .layers = layers ? s_keyboard_layers : NULL,
        .layer_count = (uint8_t)layers,
        .grid_cols = table[ERB_KEYBOARD_GRID_COLS_AT],
        .row_height_px = erb_u16(table + ERB_KEYBOARD_ROW_HEIGHT_AT),
        .key_gap_px = table[ERB_KEYBOARD_KEY_GAP_AT],
        .key_radius_px = table[ERB_KEYBOARD_KEY_RADIUS_AT],
        .font_size_px = table[ERB_KEYBOARD_FONT_SIZE_AT],
        .panel_color = erb_u32(table + ERB_KEYBOARD_PANEL_COLOR_AT),
        .key_color = erb_u32(table + ERB_KEYBOARD_KEY_COLOR_AT),
        .key_active_color = erb_u32(table + ERB_KEYBOARD_KEY_ACTIVE_COLOR_AT),
        .label_color = erb_u32(table + ERB_KEYBOARD_LABEL_COLOR_AT),
    };
    er_keyboard_set_config(&s_keyboard);
#endif
}

/** @brief Tears the app down: the scene, animations, registered assets, the keyboard, the arena and the queue. */
static void teardown(void)
{
    if (g_erb.loaded)
    {
        g_erb.loaded = false;
        er_reset(); /* nodes, animations, animated values, vectors and input */
        if (g_erb.program.keyboard)
        {
            er_keyboard_set_config(NULL);
        }
        er_image_clear();
        er_font_clear();
        memset(g_erb.arena, 0, g_erb.program.arena_bytes);
    }
    s_queue_head = 0;
    s_queue_count = 0;
    s_queue_effects = 0;
    g_erb.in_vm = false;
}

/** @brief Stops the app after a fault: tears it down and shows the error (or hands it to the firmware). */
static void handle_fault(void)
{
    s_fault_pending = false;
    teardown();
    if (g_erb.config.on_fault)
    {
        g_erb.config.on_fault(&g_erb.error, g_erb.config.context);
    }
    else
    {
        erb_show_error(&g_erb.error);
    }
}

/** @brief Step 8: the arena zeroed, each slot's initial value written, animated values created. */
static ErbStatus init_slots(void)
{
    const ErbProgram* program = &g_erb.program;
    memset(g_erb.arena, 0, program->arena_bytes);
    for (uint32_t index = 0; index < program->slot_count; index++)
    {
        const uint8_t* slot = erb_slot(program, index);
        uint8_t* data = slot_data((uint16_t)index);
        const uint32_t initial_bits = erb_u32(slot + ERB_SLOT_INIT_AT);
        switch (slot[ERB_SLOT_TYPE_AT])
        {
            case ERB_T_INT:
            case ERB_T_BOOL:
            case ERB_T_FLOAT:
            case ERB_T_ENUM:
                memcpy(data, &initial_bits, sizeof(initial_bits));
                break;
            case ERB_T_INT64:
            case ERB_T_DOUBLE:
                if (initial_bits != UINT32_MAX)
                {
                    memcpy(data, program->const64 + initial_bits * sizeof(uint64_t), 8u);
                }
                break;
            case ERB_T_STRING:
                if (initial_bits != UINT32_MAX)
                {
                    strcpy((char*)data, erb_string(program, initial_bits));
                }
                break;
            case ERB_T_AVALUE:
            {
                float initial_value;
                memcpy(&initial_value, &initial_bits, sizeof(initial_value));
                const uint32_t handle = er_anim_value_create(initial_value);
                if (handle == ER_ANIM_VALUE_INVALID)
                {
                    return erb_rt_fault(
                        ERB_ERR_TOO_BIG, "the engine's animated value pool is full", NULL, ERB_NONE, 0u);
                }
                memcpy(data, &handle, sizeof(handle));
                break;
            }
            default:
                break;
        }
    }
    return ERB_OK;
}

/** @brief Steps 7 to 12: everything after verification. */
static ErbStatus start(const ErbConfig* config)
{
    teardown();
    g_erb.program = s_candidate;
    g_erb.config = *config;
    g_erb.arena = (uint8_t*)config->arena;
    g_erb.budget = config->budget ? config->budget : ERB_EVENT_BUDGET;
    g_erb.faulted = false;
    s_fault_pending = false;
    s_reported_count = 0;
    memset(&g_erb.error, 0, sizeof(g_erb.error));
    g_erb.error.function = ERB_NONE;

    /* Step 7: an empty scene with no error screen up, then the asset pack, registered in place. It passed its
     * check-only pass, so this can't fail part way. */
    er_error_screen_hide();
    er_reset();
    er_image_clear();
    er_font_clear();
    const ErbProgram* program = &g_erb.program;
    if (program->assets)
    {
        /* A pack with fonts only verifies with storage for them, so no storage means no fonts to place. */
        ErAssetsStorage no_fonts;
        memset(&no_fonts, 0, sizeof(no_fonts));
        const bool loaded = er_assets_load_pack_into(
            program->assets, program->assets_length, config->fonts ? config->fonts : &no_fonts);
        if (!loaded)
        {
            return erb_rt_fault(ERB_ERR_ASSETS, "registering the checked pack failed", NULL, ERB_NONE, 0u);
        }
    }
    g_erb.loaded = true;

    /* Step 8 and 9: the arena, then the boot function, which works out values from the real screen. */
    ErbStatus status = init_slots();
    if (status == ERB_OK && program->boot_function != ERB_NONE)
    {
        status = run(program->boot_function, 0, NULL);
    }
    if (status != ERB_OK)
    {
        return status;
    }

    /* Step 10: the root template. */
    ERNode* root = NULL;
    status = erb_rt_build(program->root_template, NULL, NULL, 0, &root);
    if (status != ERB_OK)
    {
        return status;
    }
    apply_keyboard();

    /* Step 11 and 12: every state-driven prop's first value, the mount effects, then the root. */
    memset(dirty_words(), 0xFF, program->dirty_words * sizeof(uint32_t));
    status = settle();
    if (status != ERB_OK)
    {
        return status;
    }
    er_tree_set_root(root);
    while (s_queue_count && !g_erb.faulted)
    {
        ErbEvent next;
        queue_pop(&next);
        status = run_event(&next);
    }
    return g_erb.faulted ? g_erb.error.status : ERB_OK;
}

bool erb_load(const void* package, size_t package_bytes, const ErbConfig* config, ErbError* error)
{
    ErbError local;
    ErbError* out = error ? error : &local;
    if (config)
    {
        s_screen_w = config->screen_w;
        s_screen_h = config->screen_h;
    }
    if (!config || !erb_verify(package, package_bytes, config ? config : &g_erb.config, &s_candidate, out))
    {
        if (!config)
        {
            memset(out, 0, sizeof(*out));
            out->status = ERB_ERR_NO_PACKAGE;
            out->function = ERB_NONE;
        }
        /* The running app, if any, is untouched. */
        g_erb.error = *out;
        return false;
    }
    if (start(config) != ERB_OK)
    {
        *out = g_erb.error;
        handle_fault();
        return false;
    }
    return true;
}

void erb_unload(void)
{
    teardown();
    g_erb.faulted = false;
    s_fault_pending = false;
}

bool erb_running(void)
{
    return g_erb.loaded && !g_erb.faulted;
}

const ErbError* erb_last_error(void)
{
    return &g_erb.error;
}

void erb_tick(uint32_t elapsed_ms)
{
    if (g_erb.loaded && !g_erb.faulted)
    {
        /* Animation waits first, as the C output counts its completion waits down before its timers. */
        tick_anims(elapsed_ms);
        tick_timers(elapsed_ms);
    }
    if (s_fault_pending)
    {
        handle_fault();
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Host inputs and the wall clock
 ---------------------------------------------------------------------------------------------------------------------*/

int erb_input_find(const char* name)
{
    if (!g_erb.loaded || !name)
    {
        return -1;
    }
    /* Inputs are sorted by name (checked at load). */
    int low = 0;
    int high = (int)g_erb.program.input_count - 1;
    while (low <= high)
    {
        const int middle = (low + high) / 2;
        const int order =
            strcmp(name, erb_string(&g_erb.program, erb_u16(g_erb.program.inputs + middle * ERB_INPUT_SIZE)));
        if (order == 0)
        {
            return middle;
        }
        if (order < 0)
        {
            high = middle - 1;
        }
        else
        {
            low = middle + 1;
        }
    }
    return -1;
}

/** @brief Writes a host input's slot and runs the update. */
static bool input_set(int input, uint8_t type, const void* value, size_t bytes)
{
    if (!erb_running() || input < 0 || (uint32_t)input >= g_erb.program.input_count)
    {
        return false;
    }
    const uint16_t slot_index = erb_u16(g_erb.program.inputs + (uint32_t)input * ERB_INPUT_SIZE + 2u);
    const uint8_t* slot = erb_slot(&g_erb.program, slot_index);
    if (slot[ERB_SLOT_TYPE_AT] != type)
    {
        return false;
    }
    uint8_t* data = slot_data(slot_index);
    bool changed;
    if (type == ERB_T_STRING)
    {
        const uint16_t capacity = erb_u16(slot + ERB_SLOT_CAP_AT);
        changed = strcmp((const char*)data, (const char*)value) != 0;
        if (changed)
        {
            const size_t length = strlen((const char*)value);
            const size_t take = length < capacity ? length : capacity;
            memcpy(data, value, take);
            data[take] = '\0';
        }
    }
    else
    {
        changed = memcmp(data, value, bytes) != 0;
        memcpy(data, value, bytes);
    }
    if (changed && (slot[ERB_SLOT_FLAGS_AT] & ERB_SLOT_TRACKED))
    {
        erb_rt_set_dirty(erb_u16(slot + ERB_SLOT_DIRTY_BIT_AT));
    }
    ErbEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = ERB_EVENT_UPDATE;
    dispatch(&event);
    if (s_fault_pending && !g_erb.in_vm)
    {
        handle_fault();
    }
    return true;
}

bool erb_input_set_int(int input, int32_t value)
{
    return input_set(input, ERB_T_INT, &value, sizeof(value));
}

bool erb_input_set_bool(int input, bool value)
{
    const int32_t cell = value ? 1 : 0;
    return input_set(input, ERB_T_BOOL, &cell, sizeof(cell));
}

bool erb_input_set_float(int input, float value)
{
    return input_set(input, ERB_T_FLOAT, &value, sizeof(value));
}

bool erb_input_set_string(int input, const char* value)
{
    return value && input_set(input, ERB_T_STRING, value, 0u);
}

void erb_set_wall_clock(int64_t epoch_ms)
{
    g_erb.wall_clock_base = epoch_ms;
    g_erb.wall_clock_set_ms = er_now_ms64();
    if (!erb_running())
    {
        return;
    }
    /* Whatever reads the date may have changed, so every state-driven prop is worked out again. */
    memset(dirty_words(), 0xFF, g_erb.program.dirty_words * sizeof(uint32_t));
    ErbEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = ERB_EVENT_UPDATE;
    dispatch(&event);
    if (s_fault_pending && !g_erb.in_vm)
    {
        handle_fault();
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - The error screen
 ---------------------------------------------------------------------------------------------------------------------*/

void erb_show_error(const ErbError* error)
{
    char reason[ERROR_LINE_SIZE];
    char amounts[ERROR_LINE_SIZE];
    char location[ERROR_LINE_SIZE];
    error_reason(error, reason, sizeof(reason));
    error_amounts(error, amounts, sizeof(amounts));
    error_location(error, location, sizeof(location));
    const ERErrorScreen screen = {
        .title = error->status < ERB_ERR_BUDGET ? ERROR_TITLE_LOAD : ERROR_TITLE_RUN,
        .reason = reason,
        .amounts = amounts,
        .location = location,
        .detail = error->detail,
    };
    er_error_screen_show(s_screen_w, s_screen_h, &screen);
}
