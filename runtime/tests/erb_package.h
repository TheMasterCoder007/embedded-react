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

#ifndef ERB_PACKAGE_H
#define ERB_PACKAGE_H

/*
 * A Flow B package written as C structs, and the writer that turns it into a sealed ERCF container: what the
 * runtime tests use in place of the compiler. tpkg_base() fills in a small app that uses every table; a test
 * changes one thing and checks what the runtime makes of it.
 */

#include "er_container.h"
#include "erb_format.h"
#include "erb_props.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Records
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    uint16_t width, height;
    uint32_t arena_bytes;
    uint16_t node_count, root_template, boot_function, update_function;
} TPanel;

/** @brief The check a panel gets unless TPkg::check_words is set: the panel's exact size. */
#define TPKG_SIZE_CHECK_WORDS 8u

typedef struct
{
    uint16_t resource;
    uint32_t amount;
} TNeed;

typedef struct
{
    uint16_t kind, count;
    uint32_t offset, bytes;
} TRegion;

typedef struct
{
    uint8_t type, flags;
    uint16_t dirty_bit;
    uint32_t offset, init;
    uint16_t cap, aux;
} TSlot;

typedef struct
{
    uint16_t first, count;
} TSet;

typedef struct
{
    uint16_t size, first, field_count;
} TRow;

typedef struct
{
    uint8_t type;
    uint16_t cap;
    uint32_t offset;
} TField;

typedef struct
{
    uint16_t prop;
    uint8_t kind;
    uint32_t value;
} TPropEntry;

typedef struct
{
    uint16_t first_node, node_count, kind;
} TTemplate;

typedef struct
{
    uint8_t type, flags;
    uint16_t children, props, handle, bindings;
    uint8_t binding_count;
} TNode;

typedef struct
{
    uint8_t kind, event;
    uint16_t target;
} TBinding;

typedef struct
{
    uint32_t code, words, types;
    uint8_t registers, params, result, kind;
} TFunction;

typedef struct
{
    uint8_t kind, easing, flags;
    uint16_t target, prop, first, count;
    uint32_t amount, delay_ms, to_bits;
    float params[4];
} TAnim;

typedef struct
{
    uint32_t first_float; /**< Index into tape_floats. */
    uint16_t float_count, paint_first, paint_count, gradient_first, gradient_count;
} TTape;

typedef struct
{
    uint32_t fill, stroke;
    float stroke_width, miter;
    uint8_t cap, join, fill_rule;
    int16_t fill_gradient, stroke_gradient;
} TPaint;

typedef struct
{
    uint8_t type, stop_count;
    uint16_t stop_first;
    float ax, ay, bx, by, radius;
} TGradient;

typedef struct
{
    uint32_t color;
    float position;
} TStop;

typedef struct
{
    uint16_t name, version, signature;
} TImport;

typedef struct
{
    uint16_t name, slot;
} TInput;

typedef struct
{
    uint16_t slot;
    uint8_t point_count, flags, extrapolate_left, extrapolate_right;
    float input[8];
    uint32_t output[8]; /**< f32 bits, or ARGB with ERB_INTERPOLATION_FLAG_COLOR. */
} TInterp;

typedef struct
{
    uint16_t first, count;
} TGroup;

typedef struct
{
    uint16_t label, text;
    uint8_t type, layer, span, highlight;
} TKey;

/** @brief A keyboard table: its look, then its layers, rows and keys. */
typedef struct
{
    uint16_t row_height;
    uint8_t grid_columns, key_gap, key_radius, font_size;
    uint32_t colors[4]; /**< Panel, key, active key and label. */
    TGroup layers[12];
    uint32_t layer_count;
    TGroup rows[12];
    uint32_t row_count;
    TKey keys[16];
    uint32_t key_count;
} TKeyboard;

typedef struct
{
    uint16_t function, file;
    uint32_t offset, line;
} TDebugEntry;

#define TPKG_MAX 64

/* Room the builder sets aside for a package's program and debug sections. */
#define TPKG_PROGRAM_BYTES 24576
#define TPKG_DEBUG_BYTES 2048

/* The package and arena buffers the tests load into. */
#define TEST_PACKAGE_WORDS 8192
#define TEST_ARENA_WORDS 1024

/** @brief A whole package. Counts say how much of each array is used. */
typedef struct
{
    const char* tag;
    uint16_t major, minor, abi, flags;
    uint32_t build_id;
    TPanel panels[4];
    uint16_t panel_count;
    TNeed needs[8];
    uint16_t need_panels[8]; /**< Since minor 6: the panel each need is for. tpkg_base sets them to ERB_NONE. */
    uint16_t need_count;

    const uint32_t* check_words; /**< The checks table in place of a size check per panel (NULL: those). */
    uint32_t check_word_count;
    uint32_t panel_checks[4]; /**< With check_words: each panel's first word, or ERB_CHECK_ALWAYS. */

    const char* strings[32];
    uint32_t string_count;
    uint32_t const32[16];
    uint32_t const32_count;
    uint64_t const64[8];
    uint32_t const64_count;
    TRegion regions[12];
    uint32_t region_count;
    TSlot slots[TPKG_MAX];
    uint32_t slot_count;
    TSet enum_sets[8];
    uint32_t enum_set_count;
    uint16_t enum_values[16];
    uint32_t enum_value_count;
    TRow rows[4];
    uint32_t row_count;
    TField fields[8];
    uint32_t field_count;
    TSet prop_sets[8];
    uint32_t prop_set_count;
    TPropEntry prop_entries[16];
    uint32_t prop_entry_count;
    TTemplate templates[8];
    uint32_t template_count;
    TNode nodes[TPKG_MAX];
    uint32_t node_count;
    TBinding bindings[16];
    uint32_t binding_count;
    TFunction functions[24];
    uint32_t function_count;
    uint8_t types[256];
    uint32_t types_count;
    uint32_t code[1024];
    uint32_t code_count;
    TAnim anims[16];
    uint32_t anim_count;
    float tape_floats[256];
    uint32_t tape_float_count;
    TTape tapes[4];
    uint32_t tape_count;
    TPaint paints[8];
    uint32_t paint_count;
    TGradient gradients[4];
    uint32_t gradient_count;
    TStop stops[8];
    uint32_t stop_count;
    TImport imports[4];
    uint32_t import_count;
    TInput inputs[4];
    uint32_t input_count;
    TInterp interps[4];
    uint32_t interp_count;
    bool has_keyboard;
    TKeyboard keyboard;

    bool has_debug;
    uint16_t debug_panels[2]; /**< The panel each debug table is for (ERB_NONE: every panel). */
    uint32_t debug_table_count;
    uint32_t debug_build_id;
    TDebugEntry debug_entries[8];
    uint32_t debug_entry_count;
    uint16_t debug_names[24];
    const char* debug_strings[4];
    uint32_t debug_string_count;

    uint8_t assets[1024];
    uint32_t assets_length;

    /* Ways to break the container and directory that the structs above can't express. */
    bool omit[ERB_TABLE_COUNT_]; /**< Leave a table out of the directory. */
    bool program_twice;          /**< A second program section. */
    bool misalign_tables;        /**< Start the tables 2 bytes off 4-aligned. */
    uint16_t extra_table_id;     /**< Also list a table with this id (0: none). */
    uint16_t extra_table_panel;  /**< The panel the extra table is for (ERB_NONE: every panel). */
    bool per_panel;              /**< List each table once per panel, every panel's at the same bytes. */
    bool panel_field_zero;       /**< Write 0 in each table's panel field, as a package before minor 4 did. */
    bool overlap_tables;         /**< Point the code table at the types table. */
} TPkg;

/*----------------------------------------------------------------------------------------------------------------------
 - Byte writer
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    uint8_t* data;
    uint32_t length;
} TBuf;

static void tb_u8(TBuf* buffer, uint32_t value)
{
    buffer->data[buffer->length++] = (uint8_t)value;
}

static void tb_u16(TBuf* buffer, uint32_t value)
{
    tb_u8(buffer, value & 0xFFu);
    tb_u8(buffer, (value >> 8) & 0xFFu);
}

static void tb_u32(TBuf* buffer, uint32_t value)
{
    tb_u16(buffer, value & 0xFFFFu);
    tb_u16(buffer, value >> 16);
}

static void tb_f32(TBuf* buffer, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    tb_u32(buffer, bits);
}

static void tb_pad4(TBuf* buffer)
{
    while (buffer->length & 3u)
    {
        tb_u8(buffer, 0u);
    }
}

static void tb_put32(uint8_t* destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

/** @brief A strings table: count, offsets from the table's start, then the NUL-terminated bytes. */
static void tb_strings(TBuf* buffer, const char* const* strings, uint32_t count)
{
    const uint32_t start = buffer->length;
    tb_u32(buffer, count);
    const uint32_t offsets = buffer->length;
    for (uint32_t index = 0; index < count; index++)
    {
        tb_u32(buffer, 0u);
    }
    for (uint32_t index = 0; index < count; index++)
    {
        tb_put32(buffer->data + offsets + index * 4u, buffer->length - start);
        const size_t length = strlen(strings[index]) + 1u;
        memcpy(buffer->data + buffer->length, strings[index], length);
        buffer->length += (uint32_t)length;
    }
    tb_pad4(buffer);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tables
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Writes table @p table_id of @p package; returns false when the package has nothing for it. */
static bool tpkg_table(const TPkg* package, uint32_t table_id, TBuf* buffer)
{
    const uint32_t start = buffer->length;
    switch (table_id)
    {
        case ERB_TABLE_CHECKS:
            if (package->minor < ERB_FORMAT_MINOR_CHECKS)
                return false;
            if (package->check_words)
            {
                for (uint32_t index = 0; index < package->check_word_count; index++)
                    tb_u32(buffer, package->check_words[index]);
                break;
            }
            for (uint32_t index = 0; index < package->panel_count; index++)
            {
                tb_u32(buffer, ERB_CHECK_OP_WIDTH);
                tb_u32(buffer, ERB_CHECK_OP_PUSHI | ((uint32_t)package->panels[index].width << 8));
                tb_u32(buffer, ERB_CHECK_OP_EQ);
                tb_u32(buffer, ERB_CHECK_OP_HEIGHT);
                tb_u32(buffer, ERB_CHECK_OP_PUSHI | ((uint32_t)package->panels[index].height << 8));
                tb_u32(buffer, ERB_CHECK_OP_EQ);
                tb_u32(buffer, ERB_CHECK_OP_AND);
                tb_u32(buffer, ERB_CHECK_OP_END);
            }
            break;
        case ERB_TABLE_STRINGS:
            if (!package->string_count)
                return false;
            tb_strings(buffer, package->strings, package->string_count);
            break;
        case ERB_TABLE_CONST32:
            for (uint32_t index = 0; index < package->const32_count; index++)
                tb_u32(buffer, package->const32[index]);
            break;
        case ERB_TABLE_CONST64:
            for (uint32_t index = 0; index < package->const64_count; index++)
            {
                tb_u32(buffer, (uint32_t)package->const64[index]);
                tb_u32(buffer, (uint32_t)(package->const64[index] >> 32));
            }
            break;
        case ERB_TABLE_ARENA:
            for (uint32_t index = 0; index < package->region_count; index++)
            {
                const TRegion* region = &package->regions[index];
                tb_u16(buffer, region->kind);
                tb_u16(buffer, region->count);
                tb_u32(buffer, region->offset);
                tb_u32(buffer, region->bytes);
            }
            return true;
        case ERB_TABLE_SLOTS:
            for (uint32_t index = 0; index < package->slot_count; index++)
            {
                const TSlot* slot = &package->slots[index];
                tb_u8(buffer, slot->type);
                tb_u8(buffer, slot->flags);
                tb_u16(buffer, slot->dirty_bit);
                tb_u32(buffer, slot->offset);
                tb_u32(buffer, slot->init);
                tb_u16(buffer, slot->cap);
                tb_u16(buffer, slot->aux);
            }
            return true;
        case ERB_TABLE_ENUMS:
            if (!package->enum_set_count)
                return false;
            tb_u32(buffer, package->enum_set_count);
            for (uint32_t index = 0; index < package->enum_set_count; index++)
            {
                tb_u16(buffer, package->enum_sets[index].first);
                tb_u16(buffer, package->enum_sets[index].count);
            }
            for (uint32_t index = 0; index < package->enum_value_count; index++)
                tb_u16(buffer, package->enum_values[index]);
            tb_pad4(buffer);
            break;
        case ERB_TABLE_ROWS:
            if (!package->row_count)
                return false;
            tb_u32(buffer, package->row_count);
            for (uint32_t index = 0; index < package->row_count; index++)
            {
                tb_u16(buffer, package->rows[index].size);
                tb_u16(buffer, package->rows[index].first);
                tb_u16(buffer, package->rows[index].field_count);
                tb_u16(buffer, 0u);
            }
            for (uint32_t index = 0; index < package->field_count; index++)
            {
                tb_u8(buffer, package->fields[index].type);
                tb_u8(buffer, 0u);
                tb_u16(buffer, package->fields[index].cap);
                tb_u32(buffer, package->fields[index].offset);
            }
            break;
        case ERB_TABLE_PROPS:
            if (!package->prop_set_count)
                return false;
            tb_u32(buffer, package->prop_set_count);
            for (uint32_t index = 0; index < package->prop_set_count; index++)
            {
                tb_u16(buffer, package->prop_sets[index].first);
                tb_u16(buffer, package->prop_sets[index].count);
            }
            for (uint32_t index = 0; index < package->prop_entry_count; index++)
            {
                tb_u16(buffer, package->prop_entries[index].prop);
                tb_u8(buffer, package->prop_entries[index].kind);
                tb_u8(buffer, 0u);
                tb_u32(buffer, package->prop_entries[index].value);
            }
            break;
        case ERB_TABLE_TEMPLATES:
            tb_u32(buffer, package->template_count);
            for (uint32_t index = 0; index < package->template_count; index++)
            {
                tb_u16(buffer, package->templates[index].first_node);
                tb_u16(buffer, package->templates[index].node_count);
                tb_u16(buffer, package->templates[index].kind);
                tb_u16(buffer, 0u);
            }
            for (uint32_t index = 0; index < package->node_count; index++)
            {
                const TNode* node = &package->nodes[index];
                tb_u8(buffer, node->type);
                tb_u8(buffer, node->flags);
                tb_u16(buffer, node->children);
                tb_u16(buffer, node->props);
                tb_u16(buffer, node->handle);
                tb_u16(buffer, node->bindings);
                tb_u8(buffer, node->binding_count);
                tb_u8(buffer, 0u);
            }
            return true;
        case ERB_TABLE_BINDINGS:
            for (uint32_t index = 0; index < package->binding_count; index++)
            {
                tb_u8(buffer, package->bindings[index].kind);
                tb_u8(buffer, package->bindings[index].event);
                tb_u16(buffer, package->bindings[index].target);
            }
            break;
        case ERB_TABLE_FUNCTIONS:
            for (uint32_t index = 0; index < package->function_count; index++)
            {
                const TFunction* function = &package->functions[index];
                tb_u32(buffer, function->code);
                tb_u32(buffer, function->words);
                tb_u32(buffer, function->types);
                tb_u8(buffer, function->registers);
                tb_u8(buffer, function->params);
                tb_u8(buffer, function->result);
                tb_u8(buffer, function->kind);
            }
            return true;
        case ERB_TABLE_TYPES:
            for (uint32_t index = 0; index < package->types_count; index++)
                tb_u8(buffer, package->types[index]);
            tb_pad4(buffer);
            return true;
        case ERB_TABLE_CODE:
            for (uint32_t index = 0; index < package->code_count; index++)
                tb_u32(buffer, package->code[index]);
            return true;
        case ERB_TABLE_ANIMATIONS:
            for (uint32_t index = 0; index < package->anim_count; index++)
            {
                const TAnim* animation = &package->anims[index];
                tb_u8(buffer, animation->kind);
                tb_u8(buffer, animation->easing);
                tb_u8(buffer, animation->flags);
                tb_u8(buffer, 0u);
                tb_u16(buffer, animation->target);
                tb_u16(buffer, animation->prop);
                tb_u16(buffer, animation->first);
                tb_u16(buffer, animation->count);
                tb_u32(buffer, animation->amount);
                tb_u32(buffer, animation->delay_ms);
                tb_u32(buffer, animation->to_bits);
                for (int param = 0; param < 4; param++)
                    tb_f32(buffer, animation->params[param]);
            }
            break;
        case ERB_TABLE_TAPES:
        {
            if (!package->tape_count)
                return false;
            const uint32_t floats = ERB_TAPE_HEADER_SIZE + package->tape_count * ERB_TAPE_SIZE;
            const uint32_t paints = floats + package->tape_float_count * 4u;
            const uint32_t gradients = paints + package->paint_count * ERB_PAINT_SIZE;
            const uint32_t stops = gradients + package->gradient_count * ERB_GRADIENT_SIZE;
            tb_u32(buffer, package->tape_count);
            tb_u32(buffer, paints);
            tb_u32(buffer, gradients);
            tb_u32(buffer, stops);
            for (uint32_t index = 0; index < package->tape_count; index++)
            {
                const TTape* tape = &package->tapes[index];
                tb_u32(buffer, floats + tape->first_float * 4u);
                tb_u16(buffer, tape->float_count);
                tb_u16(buffer, tape->paint_first);
                tb_u16(buffer, tape->paint_count);
                tb_u16(buffer, tape->gradient_first);
                tb_u16(buffer, tape->gradient_count);
                tb_u16(buffer, 0u);
            }
            for (uint32_t index = 0; index < package->tape_float_count; index++)
                tb_f32(buffer, package->tape_floats[index]);
            for (uint32_t index = 0; index < package->paint_count; index++)
            {
                const TPaint* paint = &package->paints[index];
                tb_u32(buffer, paint->fill);
                tb_u32(buffer, paint->stroke);
                tb_f32(buffer, paint->stroke_width);
                tb_f32(buffer, paint->miter);
                tb_u8(buffer, paint->cap);
                tb_u8(buffer, paint->join);
                tb_u8(buffer, paint->fill_rule);
                tb_u8(buffer, 0u);
                tb_u16(buffer, (uint16_t)paint->fill_gradient);
                tb_u16(buffer, (uint16_t)paint->stroke_gradient);
            }
            for (uint32_t index = 0; index < package->gradient_count; index++)
            {
                const TGradient* gradient = &package->gradients[index];
                tb_u8(buffer, gradient->type);
                tb_u8(buffer, gradient->stop_count);
                tb_u16(buffer, gradient->stop_first);
                tb_f32(buffer, gradient->ax);
                tb_f32(buffer, gradient->ay);
                tb_f32(buffer, gradient->bx);
                tb_f32(buffer, gradient->by);
                tb_f32(buffer, gradient->radius);
            }
            for (uint32_t index = 0; index < package->stop_count; index++)
            {
                tb_u32(buffer, package->stops[index].color);
                tb_f32(buffer, package->stops[index].position);
            }
            break;
        }
        case ERB_TABLE_IMPORTS:
            for (uint32_t index = 0; index < package->import_count; index++)
            {
                tb_u16(buffer, package->imports[index].name);
                tb_u16(buffer, package->imports[index].version);
                tb_u16(buffer, package->imports[index].signature);
                tb_u16(buffer, 0u);
            }
            break;
        case ERB_TABLE_INPUTS:
            for (uint32_t index = 0; index < package->input_count; index++)
            {
                tb_u16(buffer, package->inputs[index].name);
                tb_u16(buffer, package->inputs[index].slot);
            }
            break;
        case ERB_TABLE_INTERPOLATIONS:
            for (uint32_t index = 0; index < package->interp_count; index++)
            {
                const TInterp* interp = &package->interps[index];
                tb_u16(buffer, interp->slot);
                tb_u8(buffer, interp->point_count);
                tb_u8(buffer, interp->flags);
                tb_u8(buffer, interp->extrapolate_left);
                tb_u8(buffer, interp->extrapolate_right);
                tb_u16(buffer, 0u);
                for (int point = 0; point < 8; point++)
                    tb_f32(buffer, interp->input[point]);
                for (int point = 0; point < 8; point++)
                    tb_u32(buffer, interp->output[point]);
            }
            break;
        case ERB_TABLE_KEYBOARD:
        {
            const TKeyboard* keyboard = &package->keyboard;
            if (!package->has_keyboard)
                return false;
            tb_u16(buffer, (uint16_t)keyboard->layer_count);
            tb_u16(buffer, (uint16_t)keyboard->row_count);
            tb_u16(buffer, (uint16_t)keyboard->key_count);
            tb_u16(buffer, keyboard->row_height);
            tb_u8(buffer, keyboard->grid_columns);
            tb_u8(buffer, keyboard->key_gap);
            tb_u8(buffer, keyboard->key_radius);
            tb_u8(buffer, keyboard->font_size);
            for (int color = 0; color < 4; color++)
                tb_u32(buffer, keyboard->colors[color]);
            for (uint32_t index = 0; index < keyboard->layer_count; index++)
            {
                tb_u16(buffer, keyboard->layers[index].first);
                tb_u16(buffer, keyboard->layers[index].count);
            }
            for (uint32_t index = 0; index < keyboard->row_count; index++)
            {
                tb_u16(buffer, keyboard->rows[index].first);
                tb_u16(buffer, keyboard->rows[index].count);
            }
            for (uint32_t index = 0; index < keyboard->key_count; index++)
            {
                const TKey* key = &keyboard->keys[index];
                tb_u16(buffer, key->label);
                tb_u16(buffer, key->text);
                tb_u8(buffer, key->type);
                tb_u8(buffer, key->layer);
                tb_u8(buffer, key->span);
                tb_u8(buffer, key->highlight);
            }
            break;
        }
        default:
            return false;
    }
    return buffer->length > start;
}

/** @brief Writes the program section into @p buffer. */
static void tpkg_program(const TPkg* package, TBuf* buffer)
{
    static uint8_t tables[ERB_TABLE_COUNT_][4096];
    uint32_t lengths[ERB_TABLE_COUNT_] = {0};
    bool present[ERB_TABLE_COUNT_] = {false};
    uint32_t table_count = 0;
    for (uint32_t table_id = 1; table_id < ERB_TABLE_COUNT_; table_id++)
    {
        TBuf table = {tables[table_id], 0};
        present[table_id] = tpkg_table(package, table_id, &table) && !package->omit[table_id];
        lengths[table_id] = table.length;
        table_count += present[table_id];
    }
    const uint32_t listings = package->per_panel ? package->panel_count : 1u;
    /* The checks table is listed once, whatever the panels' tables do. */
    const uint32_t shared_checks = present[ERB_TABLE_CHECKS] ? 1u : 0u;
    table_count = (table_count - shared_checks) * listings + shared_checks + (package->extra_table_id ? 1u : 0u);

    const uint32_t start = buffer->length;
    tb_u16(buffer, package->major);
    tb_u16(buffer, package->minor);
    tb_u16(buffer, package->abi);
    tb_u16(buffer, package->flags);
    tb_u32(buffer, package->build_id);
    uint32_t arena_bytes = 0;
    uint16_t node_count = 0;
    for (uint32_t index = 0; index < package->panel_count; index++)
    {
        arena_bytes =
            package->panels[index].arena_bytes > arena_bytes ? package->panels[index].arena_bytes : arena_bytes;
        node_count = package->panels[index].node_count > node_count ? package->panels[index].node_count : node_count;
    }
    tb_u32(buffer, arena_bytes);
    tb_u16(buffer, node_count);
    tb_u16(buffer, package->panel_count);
    tb_u16(buffer, package->need_count);
    tb_u16(buffer, table_count);
    for (uint32_t index = 0; index < package->panel_count; index++)
    {
        const TPanel* panel = &package->panels[index];
        if (package->minor >= ERB_FORMAT_MINOR_CHECKS)
        {
            tb_u32(buffer, package->check_words ? package->panel_checks[index] : index * TPKG_SIZE_CHECK_WORDS);
        }
        else
        {
            tb_u16(buffer, panel->width);
            tb_u16(buffer, panel->height);
        }
        tb_u32(buffer, panel->arena_bytes);
        tb_u16(buffer, panel->node_count);
        tb_u16(buffer, panel->root_template);
        tb_u16(buffer, panel->boot_function);
        tb_u16(buffer, panel->update_function);
    }
    for (uint32_t index = 0; index < package->need_count; index++)
    {
        tb_u16(buffer, package->needs[index].resource);
        tb_u16(buffer, package->minor >= ERB_FORMAT_MINOR_CHECKS ? package->need_panels[index] : 0u);
        tb_u32(buffer, package->needs[index].amount);
    }
    const uint32_t directory = buffer->length;
    buffer->length += table_count * ERB_DIRECTORY_SIZE;
    if (package->misalign_tables)
    {
        tb_u16(buffer, 0u);
    }
    uint32_t entry = 0;
    uint32_t offsets[ERB_TABLE_COUNT_] = {0};
    for (uint32_t table_id = 1; table_id < ERB_TABLE_COUNT_; table_id++)
    {
        if (!present[table_id])
        {
            continue;
        }
        offsets[table_id] = buffer->length - start;
        memcpy(buffer->data + buffer->length, tables[table_id], lengths[table_id]);
        buffer->length += lengths[table_id];
    }
    for (uint32_t listing = 0; listing < listings; listing++)
    {
        const uint16_t panel = package->per_panel ? (uint16_t)listing : package->panel_field_zero ? 0u : ERB_NONE;
        for (uint32_t table_id = 1; table_id < ERB_TABLE_COUNT_; table_id++)
        {
            if (!present[table_id] || (table_id == ERB_TABLE_CHECKS && listing > 0u))
            {
                continue;
            }
            uint8_t* entry_bytes = buffer->data + directory + entry * ERB_DIRECTORY_SIZE;
            const uint32_t offset =
                (package->overlap_tables && table_id == ERB_TABLE_CODE) ? offsets[ERB_TABLE_TYPES] : offsets[table_id];
            entry_bytes[0] = (uint8_t)table_id;
            entry_bytes[1] = 0u;
            entry_bytes[2] = (uint8_t)(table_id == ERB_TABLE_CHECKS ? ERB_NONE : panel);
            entry_bytes[3] = (uint8_t)((table_id == ERB_TABLE_CHECKS ? ERB_NONE : panel) >> 8);
            tb_put32(entry_bytes + 4, offset);
            tb_put32(entry_bytes + 8, lengths[table_id]);
            entry++;
        }
    }
    if (package->extra_table_id)
    {
        uint8_t* entry_bytes = buffer->data + directory + entry * ERB_DIRECTORY_SIZE;
        entry_bytes[0] = (uint8_t)package->extra_table_id;
        entry_bytes[1] = 0u;
        entry_bytes[2] = (uint8_t)package->extra_table_panel;
        entry_bytes[3] = (uint8_t)(package->extra_table_panel >> 8);
        tb_put32(entry_bytes + 4, buffer->length - start);
        tb_put32(entry_bytes + 8, 0u);
    }
}

/** @brief Writes one debug table: before ERB_FORMAT_MINOR_DEBUG_PANELS, the whole section. */
static void tpkg_debug_table(const TPkg* package, uint16_t panel, TBuf* buffer)
{
    const bool legacy = package->minor < ERB_FORMAT_MINOR_DEBUG_PANELS;
    const uint32_t start = buffer->length;
    tb_u32(buffer, package->debug_build_id);
    tb_u32(buffer, package->debug_entry_count);
    if (legacy)
    {
        tb_u32(buffer, package->function_count);
    }
    else
    {
        tb_u16(buffer, (uint16_t)package->function_count);
        tb_u16(buffer, panel);
    }
    const uint32_t strings = (legacy ? ERB_DEBUG_OLD_HEADER_SIZE : ERB_DEBUG_HEADER_SIZE)
                             + package->debug_entry_count * ERB_DEBUG_ENTRY_SIZE
                             + ((package->function_count * 2u + 3u) & ~3u);
    tb_u32(buffer, strings);
    const uint32_t length_at = buffer->length;
    if (!legacy)
    {
        tb_u32(buffer, 0u);
    }
    for (uint32_t index = 0; index < package->debug_entry_count; index++)
    {
        const TDebugEntry* entry = &package->debug_entries[index];
        tb_u16(buffer, entry->function);
        tb_u16(buffer, 0u);
        tb_u32(buffer, entry->offset);
        tb_u16(buffer, entry->file);
        tb_u16(buffer, 0u);
        tb_u32(buffer, entry->line);
    }
    for (uint32_t index = 0; index < package->function_count; index++)
    {
        tb_u16(buffer, package->debug_names[index]);
    }
    tb_pad4(buffer);
    tb_strings(buffer, package->debug_strings, package->debug_string_count);
    if (!legacy)
    {
        tb_put32(buffer->data + length_at, buffer->length - start);
    }
}

static void tpkg_debug(const TPkg* package, TBuf* buffer)
{
    for (uint32_t index = 0; index < package->debug_table_count; index++)
    {
        tpkg_debug_table(package, package->debug_panels[index], buffer);
    }
}

/** @brief Adds a section, preceded by padding so its bytes start 4-aligned in the container. */
static void tpkg_section(TBuf* out, uint32_t type, const uint8_t* bytes, uint32_t length, uint32_t* section_count)
{
    if ((out->length + 8u) & 3u)
    {
        const uint32_t pad = (4u - ((out->length + 16u) & 3u)) & 3u;
        tb_u32(out, ER_SECTION_PADDING);
        tb_u32(out, pad);
        for (uint32_t index = 0; index < pad; index++)
            tb_u8(out, 0u);
        (*section_count)++;
    }
    tb_u32(out, type);
    tb_u32(out, length);
    memcpy(out->data + out->length, bytes, length);
    out->length += length;
    (*section_count)++;
}

/**
 * @brief Writes @p package as a sealed container.
 *
 * @param[out] out  At least 32 KB, 4-aligned.
 *
 * @return The container's length.
 */
static uint32_t tpkg_build(const TPkg* package, uint8_t* out)
{
    static uint8_t program[TPKG_PROGRAM_BYTES];
    static uint8_t debug[TPKG_DEBUG_BYTES];
    TBuf program_buffer = {program, 0};
    tpkg_program(package, &program_buffer);
    TBuf debug_buffer = {debug, 0};
    if (package->has_debug)
    {
        tpkg_debug(package, &debug_buffer);
    }

    TBuf container = {out, 0};
    memcpy(out, "ERCF", 4);
    container.length = 4;
    tb_u32(&container, 1u);
    tb_u32(&container, 0u);
    const uint16_t tag_length = (uint16_t)strlen(package->tag);
    tb_u16(&container, tag_length);
    memcpy(out + container.length, package->tag, tag_length);
    container.length += tag_length;
    const uint32_t count_at = container.length;
    tb_u32(&container, 0u);
    uint32_t section_count = 0;
    if (package->assets_length)
    {
        tpkg_section(&container, ER_SECTION_ASSET_PACK, package->assets, package->assets_length, &section_count);
    }
    tpkg_section(&container, ERB_SECTION_PROGRAM, program, program_buffer.length, &section_count);
    if (package->program_twice)
    {
        tpkg_section(&container, ERB_SECTION_PROGRAM, program, program_buffer.length, &section_count);
    }
    if (package->has_debug)
    {
        tpkg_section(&container, ERB_SECTION_DEBUG, debug, debug_buffer.length, &section_count);
    }
    tb_put32(out + count_at, section_count);
    tb_put32(out + 8, er_crc32(out + 12, container.length - 12u));
    return container.length;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Building the app
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Adds a function with its register types and code; returns its index. */
static uint16_t tpkg_function(TPkg* package,
                              uint8_t kind,
                              uint8_t params,
                              uint8_t result,
                              const uint8_t* types,
                              uint8_t registers,
                              const uint32_t* code,
                              uint32_t words)
{
    TFunction* function = &package->functions[package->function_count];
    function->code = package->code_count;
    function->words = words;
    function->types = package->types_count;
    function->registers = registers;
    function->params = params;
    function->result = result;
    function->kind = kind;
    memcpy(package->types + package->types_count, types, registers);
    package->types_count += registers;
    memcpy(package->code + package->code_count, code, words * 4u);
    package->code_count += words;
    return (uint16_t)package->function_count++;
}

/** @brief Replaces function @p index's code (its words may change) and register types. */
static void tpkg_set_code(
    TPkg* package, uint16_t index, const uint8_t* types, uint8_t registers, const uint32_t* code, uint32_t words)
{
    TFunction* function = &package->functions[index];
    function->code = package->code_count;
    function->words = words;
    function->types = package->types_count;
    function->registers = registers;
    memcpy(package->types + package->types_count, types, registers);
    package->types_count += registers;
    memcpy(package->code + package->code_count, code, words * 4u);
    package->code_count += words;
}

#define OP_ABC(opcode, operand_a, operand_b, operand_c) ERB_ABC(ERB_OP_##opcode, operand_a, operand_b, operand_c)
#define OP_ABX(opcode, operand_a, operand_bx) ERB_ABX(ERB_OP_##opcode, operand_a, operand_bx)

/* The baseline app's pieces, by index. */
enum
{
    SLOT_COUNT = 0,     /**< int, tracked (bit 0), init 5. Input "steps". */
    SLOT_SPEED = 1,     /**< float, init 1.5. */
    SLOT_BIG = 2,       /**< int64, const64[0]. */
    SLOT_MODE = 3,      /**< enum of set 0 ("on", "off"), init 1. */
    SLOT_READY = 4,     /**< bool, boot only. */
    SLOT_FADE = 5,      /**< animated value, init 0. */
    SLOT_ANIMATION = 6, /**< anim handle. */
    SLOT_LABEL = 7,     /**< string cap 15, init "hello", tracked (bit 1). */
    SLOT_ITEMS = 8,     /**< list of row 0 (int, float), cap 4. */
    SLOT_ROOT = 9,      /**< node handle of the root view. */
    SLOT_ROWS = 10,     /**< node handle array, cap 4, for row template instances. */
};

enum
{
    FUNCTION_BOOT = 0,
    FUNCTION_UPDATE = 1,
    FUNCTION_PRESS = 2,
    FUNCTION_TIMER = 3,
    FUNCTION_EFFECT = 4,
    FUNCTION_DONE = 5,
    FUNCTION_DOUBLE = 6, /**< helper: int -> int, doubles it */
};

enum
{
    STRING_HELLO = 0,
    STRING_IMG = 1,
    STRING_ON = 2,
    STRING_OFF = 3,
    STRING_SENSORS = 4,
    STRING_SIGNATURE = 5,
    STRING_STEPS = 6,
    STRING_BOOM = 7,
};

#define TPKG_ARENA_BYTES 692u
#define TPKG_BUILD_ID 0x1234ABCDu

/* Prop ids the tests use (see erb_props.c). */
#define PROP_WIDTH 5u
#define PROP_FLEX_DIRECTION 27u
#define PROP_BACKGROUND_COLOR 47u
#define PROP_OPACITY 65u
#define PROP_TEXT 73u
#define PROP_IMAGE_NAME 85u
#define PROP_ROTATE_Z 92u

/* ARGB colors. */
#define COLOR_BLUE 0xFF0000FFu
#define COLOR_WHITE 0xFFFFFFFFu
#define COLOR_BLACK 0xFF000000u

/** @brief The bits of 1.0f, for a record's f32 field. */
#define FLOAT_ONE_BITS 0x3F800000u
#define TPKG_SCREEN 240u

/** @brief Fills @p package with the baseline app: valid, and using every table. */
static void tpkg_base(TPkg* package)
{
    memset(package, 0, sizeof(*package));
    package->tag = "erb";
    package->major = ERB_FORMAT_MAJOR;
    package->minor = ERB_FORMAT_MINOR;
    package->abi = ER_ABI;
    package->flags = 0u;
    package->build_id = TPKG_BUILD_ID;
    package->panels[0] = (TPanel){TPKG_SCREEN, TPKG_SCREEN, TPKG_ARENA_BYTES, 8u, 0u, FUNCTION_BOOT, FUNCTION_UPDATE};
    package->panel_count = 1;
    package->needs[0] = (TNeed){ERB_RESOURCE_ANIMATION_VALUES, 1u};
    package->needs[1] = (TNeed){ERB_RESOURCE_IMAGES, 1u};
    package->need_count = 2;
    for (uint32_t index = 0; index < 8u; index++)
    {
        package->need_panels[index] = ERB_NONE;
    }

    static const char* const strings[] = {"hello", "img", "on", "off", "sensors.read", "i:i", "steps", "boom"};
    memcpy(package->strings, strings, sizeof(strings));
    package->string_count = 8;
    package->const32[0] = 7u;
    package->const32[1] = ER_FLEX_COLUMN;
    package->const32[2] = ER_FLEX_ROW;
    package->const32_count = 3;
    package->const64[0] = 1234567890123ull;
    package->const64_count = 1;

    const TRegion regions[] = {
        {ERB_REGION_SLOTS, 0, 0, 32},
        {ERB_REGION_DIRTY, 2, 32, 8},
        {ERB_REGION_STRINGS, 0, 40, 16},
        {ERB_REGION_LISTS, 0, 56, 36},
        {ERB_REGION_HANDLES, 0, 92, 40},
        {ERB_REGION_TIMERS, 2, 132, 32},
        {ERB_REGION_ANIMATIONS, 4, 164, 80},
        {ERB_REGION_TAPE, 16, 244, 64},
        {ERB_REGION_EVENTS, 4, 308, 128},
        {ERB_REGION_STACK, 64, 436, 256},
    };
    memcpy(package->regions, regions, sizeof(regions));
    package->region_count = 10;

    float speed = 1.5f;
    uint32_t speed_bits;
    memcpy(&speed_bits, &speed, sizeof(speed_bits));
    const TSlot slots[] = {
        {ERB_TYPE_INT, ERB_SLOT_TRACKED, 0, 0, 5, 0, ERB_NONE},
        {ERB_TYPE_FLOAT, 0, ERB_NONE, 4, speed_bits, 0, ERB_NONE},
        {ERB_TYPE_INT64, 0, ERB_NONE, 8, 0, 0, ERB_NONE},
        {ERB_TYPE_ENUM, 0, ERB_NONE, 16, 1, 0, 0},
        {ERB_TYPE_BOOL, ERB_SLOT_BOOT_ONLY, ERB_NONE, 20, 0, 0, ERB_NONE},
        {ERB_TYPE_ANIMATED_VALUE, 0, ERB_NONE, 24, 0, 0, ERB_NONE},
        {ERB_TYPE_ANIMATION, 0, ERB_NONE, 28, 0, 0, ERB_NONE},
        {ERB_TYPE_STRING, ERB_SLOT_TRACKED, 1, 40, STRING_HELLO, 15, ERB_NONE},
        {ERB_TYPE_LIST, 0, ERB_NONE, 56, 0, 4, 0},
        {ERB_TYPE_NODE, 0, ERB_NONE, 92, 0, 0, ERB_NONE},
        {ERB_TYPE_NODE, 0, ERB_NONE, 100, 0, 4, ERB_NONE},
    };
    memcpy(package->slots, slots, sizeof(slots));
    package->slot_count = 11;

    package->enum_sets[0] = (TSet){0, 2};
    package->enum_set_count = 1;
    package->enum_values[0] = STRING_ON;
    package->enum_values[1] = STRING_OFF;
    package->enum_value_count = 2;

    package->rows[0] = (TRow){8, 0, 2};
    package->row_count = 1;
    package->fields[0] = (TField){ERB_TYPE_INT, 0, 0};
    package->fields[1] = (TField){ERB_TYPE_FLOAT, 0, 4};
    package->field_count = 2;

    package->prop_sets[0] = (TSet){0, 1};
    package->prop_sets[1] = (TSet){1, 2};
    package->prop_sets[2] = (TSet){3, 1};
    package->prop_set_count = 3;
    package->prop_entries[0] = (TPropEntry){PROP_TEXT, ERB_PROP_KIND_STRING, STRING_HELLO};
    package->prop_entries[1] = (TPropEntry){PROP_WIDTH, ERB_PROP_KIND_I16, 100};
    package->prop_entries[2] = (TPropEntry){PROP_BACKGROUND_COLOR, ERB_PROP_KIND_COLOR, COLOR_BLUE};
    package->prop_entries[3] = (TPropEntry){PROP_IMAGE_NAME, ERB_PROP_KIND_IMAGE, STRING_IMG};
    package->prop_entry_count = 4;

    package->templates[0] = (TTemplate){0, 4, ERB_TEMPLATE_KIND_ROOT};
    package->templates[1] = (TTemplate){4, 1, ERB_TEMPLATE_KIND_ROW};
    package->templates[2] = (TTemplate){5, 1, ERB_TEMPLATE_KIND_BRANCH};
    package->template_count = 3;
    const TNode nodes[] = {
        {ER_NODE_VIEW, 0, 3, 1, SLOT_ROOT, 0, 1},
        {ER_NODE_TEXT, 0, 0, 0, ERB_NONE, 0, 0},
        {ER_NODE_IMAGE, 0, 0, 2, ERB_NONE, 0, 0},
        {ER_NODE_VECTOR, 0, 0, ERB_NONE, ERB_NONE, 1, 1},
        {ER_NODE_VIEW, 0, 0, ERB_NONE, SLOT_ROWS, 2, 1},
        {ER_NODE_TEXT, 0, 0, 0, ERB_NONE, 0, 0},
    };
    memcpy(package->nodes, nodes, sizeof(nodes));
    package->node_count = 6;

    package->bindings[0] = (TBinding){ERB_BIND_EVENT, ER_EVENT_PRESS, FUNCTION_PRESS};
    package->bindings[1] = (TBinding){ERB_BIND_TAPE, 0, 0};
    package->bindings[2] = (TBinding){ERB_BIND_ANIMATED, PROP_OPACITY, SLOT_FADE};
    package->binding_count = 3;

    {
        const uint8_t types[] = {ERB_TYPE_BOOL};
        const uint32_t code[] = {OP_ABX(LOADI, 0, 1), OP_ABX(SET32, 0, SLOT_READY), OP_ABC(RET, 0, 0, 0)};
        tpkg_function(package, ERB_FUNCTION_KIND_BOOT, 0, 0, types, 1, code, 3);
    }
    {
        const uint32_t code[] = {OP_ABX(JDIRTY, 0, 0), 1u, OP_ABC(RET, 0, 0, 0)};
        tpkg_function(package, ERB_FUNCTION_KIND_UPDATE, 0, 0, NULL, 0, code, 3);
    }
    {
        const uint8_t types[] = {ERB_TYPE_INT, ERB_TYPE_INT};
        const uint32_t code[] = {
            OP_ABX(GET32, 0, SLOT_COUNT),
            OP_ABX(LOADI, 1, 1),
            OP_ABC(ADD, 0, 0, 1),
            OP_ABX(CALL, 0, FUNCTION_DOUBLE),
            OP_ABX(SET32, 0, SLOT_COUNT),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_function(package, ER_EVENT_PRESS, 0, 0, types, 2, code, 6);
    }
    {
        const uint32_t code[] = {OP_ABC(RET, 0, 0, 0)};
        tpkg_function(package, ERB_FUNCTION_KIND_TIMER, 0, 0, NULL, 0, code, 1);
        tpkg_function(package, ERB_FUNCTION_KIND_EFFECT, 0, 0, NULL, 0, code, 1);
        tpkg_function(package, ERB_FUNCTION_KIND_ANIMATION_DONE, 0, 0, NULL, 0, code, 1);
    }
    {
        const uint8_t types[] = {ERB_TYPE_INT, ERB_TYPE_INT};
        const uint32_t code[] = {OP_ABX(LOADI, 1, 2), OP_ABC(MUL, 0, 0, 1), OP_ABC(RET, 0, 0, 0)};
        tpkg_function(package, ERB_FUNCTION_KIND_HELPER, 1, ERB_TYPE_INT, types, 2, code, 3);
    }

    package->anims[0] = (TAnim){ERB_ANIMATION_SEQUENCE, 0, 0, ERB_NONE, 0, 1, 1, 0, 0, 0, {0, 0, 0, 0}};
    package->anims[1] =
        (TAnim){ERB_ANIMATION_TIMING, ER_EASE_LINEAR, 0, SLOT_FADE, 0, 0, 0, 100, 0, FLOAT_ONE_BITS, {0, 0, 0, 0}};
    package->anim_count = 2;

    const float floats[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 10.0f, 10.0f, 6.0f};
    memcpy(package->tape_floats, floats, sizeof(floats));
    package->tape_float_count = 9;
    package->tapes[0] = (TTape){0, 9, 0, 1, 0, 0};
    package->tape_count = 1;
    package->paints[0] = (TPaint){COLOR_WHITE, 0u, 0.0f, 0.0f, 0, 0, 0, 0, 0};
    package->paint_count = 1;

    package->imports[0] = (TImport){STRING_SENSORS, 1, STRING_SIGNATURE};
    package->import_count = 1;
    package->inputs[0] = (TInput){STRING_STEPS, SLOT_COUNT};
    package->input_count = 1;
    /* The animated value 0 to 1 onto 0 to 90 degrees, then onto black to white. */
    package->interps[0] =
        (TInterp){SLOT_FADE, 2, 0, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP, {0.0f, 1.0f}, {0u, 0x42B40000u}};
    package->interps[1] = (TInterp){SLOT_FADE,
                                    2,
                                    ERB_INTERPOLATION_FLAG_COLOR,
                                    ER_EXTRAPOLATE_EXTEND,
                                    ER_EXTRAPOLATE_EXTEND,
                                    {0.0f, 1.0f},
                                    {COLOR_BLACK, COLOR_WHITE}};
    package->interp_count = 2;

    package->has_debug = true;
    package->debug_panels[0] = ERB_NONE;
    package->debug_table_count = 1;
    package->debug_build_id = package->build_id;
    package->debug_entries[0] = (TDebugEntry){FUNCTION_PRESS, 0, 0, 41};
    package->debug_entry_count = 1;
    package->debug_strings[0] = "App.jsx";
    package->debug_string_count = 1;

    /* An ERPK v2 pack with one 1x1 ARGB image named "img": format byte, then a pad so the pixels sit at 32. */
    static const uint8_t assets[] = {
        'E', 'R', 'P', 'K', 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,    0,    3,    0,
        'i', 'm', 'g', 1,   0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    memcpy(package->assets, assets, sizeof(assets));
    package->assets_length = sizeof(assets);
}

#endif
