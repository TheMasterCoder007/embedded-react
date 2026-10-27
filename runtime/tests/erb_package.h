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
    float stroke_w, miter;
    uint8_t cap, join, fill_rule;
    int16_t fill_grad, stroke_grad;
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
    uint16_t function, file;
    uint32_t offset, line;
} TDebugEntry;

#define TPKG_MAX 64

/** @brief A whole package. Counts say how much of each array is used. */
typedef struct
{
    const char* tag;
    uint16_t major, minor, abi, flags;
    uint32_t build_id;
    TPanel panels[4];
    uint16_t panel_count;
    TNeed needs[8];
    uint16_t need_count;

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

    bool has_debug;
    uint32_t debug_build_id;
    TDebugEntry debug_entries[8];
    uint32_t debug_entry_count;
    uint16_t debug_names[24];
    const char* debug_strings[4];
    uint32_t debug_string_count;

    uint8_t assets[128];
    uint32_t assets_length;

    /* Ways to break the container and directory that the structs above can't express. */
    bool omit[ERB_TABLE_COUNT_]; /**< Leave a table out of the directory. */
    bool program_twice;          /**< A second program section. */
    bool misalign_tables;        /**< Start the tables 2 bytes off 4-aligned. */
    uint16_t extra_table_id;     /**< Also list a table with this id (0: none). */
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

static void tb_u8(TBuf* buf, uint32_t value)
{
    buf->data[buf->length++] = (uint8_t)value;
}

static void tb_u16(TBuf* buf, uint32_t value)
{
    tb_u8(buf, value & 0xFFu);
    tb_u8(buf, (value >> 8) & 0xFFu);
}

static void tb_u32(TBuf* buf, uint32_t value)
{
    tb_u16(buf, value & 0xFFFFu);
    tb_u16(buf, value >> 16);
}

static void tb_f32(TBuf* buf, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    tb_u32(buf, bits);
}

static void tb_pad4(TBuf* buf)
{
    while (buf->length & 3u)
    {
        tb_u8(buf, 0u);
    }
}

static void tb_put32(uint8_t* at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

/** @brief A strings table: count, offsets from the table's start, then the NUL-terminated bytes. */
static void tb_strings(TBuf* buf, const char* const* strings, uint32_t count)
{
    const uint32_t start = buf->length;
    tb_u32(buf, count);
    const uint32_t offsets = buf->length;
    for (uint32_t index = 0; index < count; index++)
    {
        tb_u32(buf, 0u);
    }
    for (uint32_t index = 0; index < count; index++)
    {
        tb_put32(buf->data + offsets + index * 4u, buf->length - start);
        const size_t length = strlen(strings[index]) + 1u;
        memcpy(buf->data + buf->length, strings[index], length);
        buf->length += (uint32_t)length;
    }
    tb_pad4(buf);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tables
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Writes table @p id of @p pkg; returns false when the package has nothing for it. */
static bool tpkg_table(const TPkg* pkg, uint32_t id, TBuf* buf)
{
    const uint32_t start = buf->length;
    switch (id)
    {
        case ERB_TABLE_STRINGS:
            if (!pkg->string_count)
                return false;
            tb_strings(buf, pkg->strings, pkg->string_count);
            break;
        case ERB_TABLE_CONST32:
            for (uint32_t index = 0; index < pkg->const32_count; index++)
                tb_u32(buf, pkg->const32[index]);
            break;
        case ERB_TABLE_CONST64:
            for (uint32_t index = 0; index < pkg->const64_count; index++)
            {
                tb_u32(buf, (uint32_t)pkg->const64[index]);
                tb_u32(buf, (uint32_t)(pkg->const64[index] >> 32));
            }
            break;
        case ERB_TABLE_ARENA:
            for (uint32_t index = 0; index < pkg->region_count; index++)
            {
                const TRegion* region = &pkg->regions[index];
                tb_u16(buf, region->kind);
                tb_u16(buf, region->count);
                tb_u32(buf, region->offset);
                tb_u32(buf, region->bytes);
            }
            return true;
        case ERB_TABLE_SLOTS:
            for (uint32_t index = 0; index < pkg->slot_count; index++)
            {
                const TSlot* slot = &pkg->slots[index];
                tb_u8(buf, slot->type);
                tb_u8(buf, slot->flags);
                tb_u16(buf, slot->dirty_bit);
                tb_u32(buf, slot->offset);
                tb_u32(buf, slot->init);
                tb_u16(buf, slot->cap);
                tb_u16(buf, slot->aux);
            }
            return true;
        case ERB_TABLE_ENUMS:
            if (!pkg->enum_set_count)
                return false;
            tb_u32(buf, pkg->enum_set_count);
            for (uint32_t index = 0; index < pkg->enum_set_count; index++)
            {
                tb_u16(buf, pkg->enum_sets[index].first);
                tb_u16(buf, pkg->enum_sets[index].count);
            }
            for (uint32_t index = 0; index < pkg->enum_value_count; index++)
                tb_u16(buf, pkg->enum_values[index]);
            tb_pad4(buf);
            break;
        case ERB_TABLE_ROWS:
            if (!pkg->row_count)
                return false;
            tb_u32(buf, pkg->row_count);
            for (uint32_t index = 0; index < pkg->row_count; index++)
            {
                tb_u16(buf, pkg->rows[index].size);
                tb_u16(buf, pkg->rows[index].first);
                tb_u16(buf, pkg->rows[index].field_count);
                tb_u16(buf, 0u);
            }
            for (uint32_t index = 0; index < pkg->field_count; index++)
            {
                tb_u8(buf, pkg->fields[index].type);
                tb_u8(buf, 0u);
                tb_u16(buf, pkg->fields[index].cap);
                tb_u32(buf, pkg->fields[index].offset);
            }
            break;
        case ERB_TABLE_PROPS:
            if (!pkg->prop_set_count)
                return false;
            tb_u32(buf, pkg->prop_set_count);
            for (uint32_t index = 0; index < pkg->prop_set_count; index++)
            {
                tb_u16(buf, pkg->prop_sets[index].first);
                tb_u16(buf, pkg->prop_sets[index].count);
            }
            for (uint32_t index = 0; index < pkg->prop_entry_count; index++)
            {
                tb_u16(buf, pkg->prop_entries[index].prop);
                tb_u8(buf, pkg->prop_entries[index].kind);
                tb_u8(buf, 0u);
                tb_u32(buf, pkg->prop_entries[index].value);
            }
            break;
        case ERB_TABLE_TEMPLATES:
            tb_u32(buf, pkg->template_count);
            for (uint32_t index = 0; index < pkg->template_count; index++)
            {
                tb_u16(buf, pkg->templates[index].first_node);
                tb_u16(buf, pkg->templates[index].node_count);
                tb_u16(buf, pkg->templates[index].kind);
                tb_u16(buf, 0u);
            }
            for (uint32_t index = 0; index < pkg->node_count; index++)
            {
                const TNode* node = &pkg->nodes[index];
                tb_u8(buf, node->type);
                tb_u8(buf, node->flags);
                tb_u16(buf, node->children);
                tb_u16(buf, node->props);
                tb_u16(buf, node->handle);
                tb_u16(buf, node->bindings);
                tb_u8(buf, node->binding_count);
                tb_u8(buf, 0u);
            }
            return true;
        case ERB_TABLE_BINDINGS:
            for (uint32_t index = 0; index < pkg->binding_count; index++)
            {
                tb_u8(buf, pkg->bindings[index].kind);
                tb_u8(buf, pkg->bindings[index].event);
                tb_u16(buf, pkg->bindings[index].target);
            }
            break;
        case ERB_TABLE_FUNCTIONS:
            for (uint32_t index = 0; index < pkg->function_count; index++)
            {
                const TFunction* function = &pkg->functions[index];
                tb_u32(buf, function->code);
                tb_u32(buf, function->words);
                tb_u32(buf, function->types);
                tb_u8(buf, function->registers);
                tb_u8(buf, function->params);
                tb_u8(buf, function->result);
                tb_u8(buf, function->kind);
            }
            return true;
        case ERB_TABLE_TYPES:
            for (uint32_t index = 0; index < pkg->types_count; index++)
                tb_u8(buf, pkg->types[index]);
            tb_pad4(buf);
            return true;
        case ERB_TABLE_CODE:
            for (uint32_t index = 0; index < pkg->code_count; index++)
                tb_u32(buf, pkg->code[index]);
            return true;
        case ERB_TABLE_ANIMS:
            for (uint32_t index = 0; index < pkg->anim_count; index++)
            {
                const TAnim* anim = &pkg->anims[index];
                tb_u8(buf, anim->kind);
                tb_u8(buf, anim->easing);
                tb_u8(buf, anim->flags);
                tb_u8(buf, 0u);
                tb_u16(buf, anim->target);
                tb_u16(buf, anim->prop);
                tb_u16(buf, anim->first);
                tb_u16(buf, anim->count);
                tb_u32(buf, anim->amount);
                tb_u32(buf, anim->delay_ms);
                tb_u32(buf, anim->to_bits);
                for (int param = 0; param < 4; param++)
                    tb_f32(buf, anim->params[param]);
            }
            break;
        case ERB_TABLE_TAPES:
        {
            if (!pkg->tape_count)
                return false;
            const uint32_t floats = ERB_TAPE_HEADER_SIZE + pkg->tape_count * ERB_TAPE_SIZE;
            const uint32_t paints = floats + pkg->tape_float_count * 4u;
            const uint32_t gradients = paints + pkg->paint_count * ERB_PAINT_SIZE;
            const uint32_t stops = gradients + pkg->gradient_count * ERB_GRADIENT_SIZE;
            tb_u32(buf, pkg->tape_count);
            tb_u32(buf, paints);
            tb_u32(buf, gradients);
            tb_u32(buf, stops);
            for (uint32_t index = 0; index < pkg->tape_count; index++)
            {
                const TTape* tape = &pkg->tapes[index];
                tb_u32(buf, floats + tape->first_float * 4u);
                tb_u16(buf, tape->float_count);
                tb_u16(buf, tape->paint_first);
                tb_u16(buf, tape->paint_count);
                tb_u16(buf, tape->gradient_first);
                tb_u16(buf, tape->gradient_count);
                tb_u16(buf, 0u);
            }
            for (uint32_t index = 0; index < pkg->tape_float_count; index++)
                tb_f32(buf, pkg->tape_floats[index]);
            for (uint32_t index = 0; index < pkg->paint_count; index++)
            {
                const TPaint* paint = &pkg->paints[index];
                tb_u32(buf, paint->fill);
                tb_u32(buf, paint->stroke);
                tb_f32(buf, paint->stroke_w);
                tb_f32(buf, paint->miter);
                tb_u8(buf, paint->cap);
                tb_u8(buf, paint->join);
                tb_u8(buf, paint->fill_rule);
                tb_u8(buf, 0u);
                tb_u16(buf, (uint16_t)paint->fill_grad);
                tb_u16(buf, (uint16_t)paint->stroke_grad);
            }
            for (uint32_t index = 0; index < pkg->gradient_count; index++)
            {
                const TGradient* gradient = &pkg->gradients[index];
                tb_u8(buf, gradient->type);
                tb_u8(buf, gradient->stop_count);
                tb_u16(buf, gradient->stop_first);
                tb_f32(buf, gradient->ax);
                tb_f32(buf, gradient->ay);
                tb_f32(buf, gradient->bx);
                tb_f32(buf, gradient->by);
                tb_f32(buf, gradient->radius);
            }
            for (uint32_t index = 0; index < pkg->stop_count; index++)
            {
                tb_u32(buf, pkg->stops[index].color);
                tb_f32(buf, pkg->stops[index].position);
            }
            break;
        }
        case ERB_TABLE_IMPORTS:
            for (uint32_t index = 0; index < pkg->import_count; index++)
            {
                tb_u16(buf, pkg->imports[index].name);
                tb_u16(buf, pkg->imports[index].version);
                tb_u16(buf, pkg->imports[index].signature);
                tb_u16(buf, 0u);
            }
            break;
        case ERB_TABLE_INPUTS:
            for (uint32_t index = 0; index < pkg->input_count; index++)
            {
                tb_u16(buf, pkg->inputs[index].name);
                tb_u16(buf, pkg->inputs[index].slot);
            }
            break;
        default:
            return false;
    }
    return buf->length > start;
}

/** @brief Writes the program section into @p buf. */
static void tpkg_program(const TPkg* pkg, TBuf* buf)
{
    static uint8_t tables[ERB_TABLE_COUNT_][4096];
    uint32_t lengths[ERB_TABLE_COUNT_] = {0};
    bool present[ERB_TABLE_COUNT_] = {false};
    uint32_t table_count = 0;
    for (uint32_t id = 1; id < ERB_TABLE_COUNT_; id++)
    {
        TBuf table = {tables[id], 0};
        present[id] = tpkg_table(pkg, id, &table) && !pkg->omit[id];
        lengths[id] = table.length;
        table_count += present[id];
    }
    table_count += pkg->extra_table_id ? 1u : 0u;

    const uint32_t start = buf->length;
    tb_u16(buf, pkg->major);
    tb_u16(buf, pkg->minor);
    tb_u16(buf, pkg->abi);
    tb_u16(buf, pkg->flags);
    tb_u32(buf, pkg->build_id);
    uint32_t arena_bytes = 0;
    uint16_t node_count = 0;
    for (uint32_t index = 0; index < pkg->panel_count; index++)
    {
        arena_bytes = pkg->panels[index].arena_bytes > arena_bytes ? pkg->panels[index].arena_bytes : arena_bytes;
        node_count = pkg->panels[index].node_count > node_count ? pkg->panels[index].node_count : node_count;
    }
    tb_u32(buf, arena_bytes);
    tb_u16(buf, node_count);
    tb_u16(buf, pkg->panel_count);
    tb_u16(buf, pkg->need_count);
    tb_u16(buf, table_count);
    for (uint32_t index = 0; index < pkg->panel_count; index++)
    {
        const TPanel* panel = &pkg->panels[index];
        tb_u16(buf, panel->width);
        tb_u16(buf, panel->height);
        tb_u32(buf, panel->arena_bytes);
        tb_u16(buf, panel->node_count);
        tb_u16(buf, panel->root_template);
        tb_u16(buf, panel->boot_function);
        tb_u16(buf, panel->update_function);
    }
    for (uint32_t index = 0; index < pkg->need_count; index++)
    {
        tb_u16(buf, pkg->needs[index].resource);
        tb_u16(buf, 0u);
        tb_u32(buf, pkg->needs[index].amount);
    }
    const uint32_t directory = buf->length;
    buf->length += table_count * ERB_DIRECTORY_SIZE;
    if (pkg->misalign_tables)
    {
        tb_u16(buf, 0u);
    }
    uint32_t entry = 0;
    uint32_t offsets[ERB_TABLE_COUNT_] = {0};
    for (uint32_t id = 1; id < ERB_TABLE_COUNT_; id++)
    {
        if (!present[id])
        {
            continue;
        }
        offsets[id] = buf->length - start;
        memcpy(buf->data + buf->length, tables[id], lengths[id]);
        buf->length += lengths[id];
    }
    for (uint32_t id = 1; id < ERB_TABLE_COUNT_; id++)
    {
        if (!present[id])
        {
            continue;
        }
        uint8_t* at = buf->data + directory + entry * ERB_DIRECTORY_SIZE;
        const uint32_t offset = (pkg->overlap_tables && id == ERB_TABLE_CODE) ? offsets[ERB_TABLE_TYPES] : offsets[id];
        at[0] = (uint8_t)id;
        at[1] = 0u;
        at[2] = at[3] = 0u;
        tb_put32(at + 4, offset);
        tb_put32(at + 8, lengths[id]);
        entry++;
    }
    if (pkg->extra_table_id)
    {
        uint8_t* at = buf->data + directory + entry * ERB_DIRECTORY_SIZE;
        at[0] = (uint8_t)pkg->extra_table_id;
        at[1] = at[2] = at[3] = 0u;
        tb_put32(at + 4, buf->length - start);
        tb_put32(at + 8, 0u);
    }
}

static void tpkg_debug(const TPkg* pkg, TBuf* buf)
{
    tb_u32(buf, pkg->debug_build_id);
    tb_u32(buf, pkg->debug_entry_count);
    tb_u32(buf, pkg->function_count);
    const uint32_t strings =
        ERB_DEBUG_HEADER_SIZE + pkg->debug_entry_count * ERB_DEBUG_ENTRY_SIZE + ((pkg->function_count * 2u + 3u) & ~3u);
    tb_u32(buf, strings);
    for (uint32_t index = 0; index < pkg->debug_entry_count; index++)
    {
        const TDebugEntry* entry = &pkg->debug_entries[index];
        tb_u16(buf, entry->function);
        tb_u16(buf, 0u);
        tb_u32(buf, entry->offset);
        tb_u16(buf, entry->file);
        tb_u16(buf, 0u);
        tb_u32(buf, entry->line);
    }
    for (uint32_t index = 0; index < pkg->function_count; index++)
    {
        tb_u16(buf, pkg->debug_names[index]);
    }
    tb_pad4(buf);
    tb_strings(buf, pkg->debug_strings, pkg->debug_string_count);
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
 * @brief Writes @p pkg as a sealed container.
 *
 * @param[out] out  At least 32 KB, 4-aligned.
 *
 * @return The container's length.
 */
static uint32_t tpkg_build(const TPkg* pkg, uint8_t* out)
{
    static uint8_t program[24576];
    static uint8_t debug[2048];
    TBuf program_buf = {program, 0};
    tpkg_program(pkg, &program_buf);
    TBuf debug_buf = {debug, 0};
    if (pkg->has_debug)
    {
        tpkg_debug(pkg, &debug_buf);
    }

    TBuf container = {out, 0};
    memcpy(out, "ERCF", 4);
    container.length = 4;
    tb_u32(&container, 1u);
    tb_u32(&container, 0u);
    const uint16_t tag_length = (uint16_t)strlen(pkg->tag);
    tb_u16(&container, tag_length);
    memcpy(out + container.length, pkg->tag, tag_length);
    container.length += tag_length;
    const uint32_t count_at = container.length;
    tb_u32(&container, 0u);
    uint32_t section_count = 0;
    if (pkg->assets_length)
    {
        tpkg_section(&container, ER_SECTION_ASSET_PACK, pkg->assets, pkg->assets_length, &section_count);
    }
    tpkg_section(&container, ERB_SECTION_PROGRAM, program, program_buf.length, &section_count);
    if (pkg->program_twice)
    {
        tpkg_section(&container, ERB_SECTION_PROGRAM, program, program_buf.length, &section_count);
    }
    if (pkg->has_debug)
    {
        tpkg_section(&container, ERB_SECTION_DEBUG, debug, debug_buf.length, &section_count);
    }
    tb_put32(out + count_at, section_count);
    tb_put32(out + 8, er_crc32(out + 12, container.length - 12u));
    return container.length;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Building the app
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Adds a function with its register types and code; returns its index. */
static uint16_t tpkg_function(TPkg* pkg,
                              uint8_t kind,
                              uint8_t params,
                              uint8_t result,
                              const uint8_t* types,
                              uint8_t registers,
                              const uint32_t* code,
                              uint32_t words)
{
    TFunction* function = &pkg->functions[pkg->function_count];
    function->code = pkg->code_count;
    function->words = words;
    function->types = pkg->types_count;
    function->registers = registers;
    function->params = params;
    function->result = result;
    function->kind = kind;
    memcpy(pkg->types + pkg->types_count, types, registers);
    pkg->types_count += registers;
    memcpy(pkg->code + pkg->code_count, code, words * 4u);
    pkg->code_count += words;
    return (uint16_t)pkg->function_count++;
}

/** @brief Replaces function @p index's code (its words may change) and register types. */
static void
tpkg_set_code(TPkg* pkg, uint16_t index, const uint8_t* types, uint8_t registers, const uint32_t* code, uint32_t words)
{
    TFunction* function = &pkg->functions[index];
    function->code = pkg->code_count;
    function->words = words;
    function->types = pkg->types_count;
    function->registers = registers;
    memcpy(pkg->types + pkg->types_count, types, registers);
    pkg->types_count += registers;
    memcpy(pkg->code + pkg->code_count, code, words * 4u);
    pkg->code_count += words;
}

#define OP_ABC(op, a, b, c) ERB_ABC(ERB_OP_##op, a, b, c)
#define OP_ABX(op, a, bx) ERB_ABX(ERB_OP_##op, a, bx)

/* The baseline app's pieces, by index. */
enum
{
    S_COUNT = 0, /**< int, tracked (bit 0), init 5. Input "steps". */
    S_SPEED = 1, /**< float, init 1.5. */
    S_BIG = 2,   /**< int64, const64[0]. */
    S_MODE = 3,  /**< enum of set 0 ("on", "off"), init 1. */
    S_READY = 4, /**< bool, boot only. */
    S_FADE = 5,  /**< animated value, init 0. */
    S_ANIM = 6,  /**< anim handle. */
    S_LABEL = 7, /**< string cap 15, init "hello", tracked (bit 1). */
    S_ITEMS = 8, /**< list of row 0 (int, float), cap 4. */
    S_ROOT = 9,  /**< node handle of the root view. */
    S_ROWS = 10, /**< node handle array, cap 4, for row template instances. */
};

enum
{
    F_BOOT = 0,
    F_UPDATE = 1,
    F_PRESS = 2,
    F_TIMER = 3,
    F_EFFECT = 4,
    F_DONE = 5,
    F_DOUBLE = 6, /**< helper: int -> int, doubles it */
};

enum
{
    STR_HELLO = 0,
    STR_IMG = 1,
    STR_ON = 2,
    STR_OFF = 3,
    STR_SENSORS = 4,
    STR_SIG = 5,
    STR_STEPS = 6,
    STR_BOOM = 7,
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

/** @brief Fills @p pkg with the baseline app: valid, and using every table. */
static void tpkg_base(TPkg* pkg)
{
    memset(pkg, 0, sizeof(*pkg));
    pkg->tag = "erb";
    pkg->major = ERB_FORMAT_MAJOR;
    pkg->minor = ERB_FORMAT_MINOR;
    pkg->abi = ER_ABI;
    pkg->flags = 0u;
    pkg->build_id = TPKG_BUILD_ID;
    pkg->panels[0] = (TPanel){TPKG_SCREEN, TPKG_SCREEN, TPKG_ARENA_BYTES, 8u, 0u, F_BOOT, F_UPDATE};
    pkg->panel_count = 1;
    pkg->needs[0] = (TNeed){ERB_RES_ANIM_VALUES, 1u};
    pkg->needs[1] = (TNeed){ERB_RES_IMAGES, 1u};
    pkg->need_count = 2;

    static const char* const strings[] = {"hello", "img", "on", "off", "sensors.read", "i:i", "steps", "boom"};
    memcpy(pkg->strings, strings, sizeof(strings));
    pkg->string_count = 8;
    pkg->const32[0] = 7u;
    pkg->const32[1] = ER_FLEX_COL;
    pkg->const32[2] = ER_FLEX_ROW;
    pkg->const32_count = 3;
    pkg->const64[0] = 1234567890123ull;
    pkg->const64_count = 1;

    const TRegion regions[] = {
        {ERB_REGION_SLOTS, 0, 0, 32},
        {ERB_REGION_DIRTY, 2, 32, 8},
        {ERB_REGION_STRINGS, 0, 40, 16},
        {ERB_REGION_LISTS, 0, 56, 36},
        {ERB_REGION_HANDLES, 0, 92, 40},
        {ERB_REGION_TIMERS, 2, 132, 32},
        {ERB_REGION_ANIMS, 4, 164, 80},
        {ERB_REGION_TAPE, 16, 244, 64},
        {ERB_REGION_EVENTS, 4, 308, 128},
        {ERB_REGION_STACK, 64, 436, 256},
    };
    memcpy(pkg->regions, regions, sizeof(regions));
    pkg->region_count = 10;

    float speed = 1.5f;
    uint32_t speed_bits;
    memcpy(&speed_bits, &speed, sizeof(speed_bits));
    const TSlot slots[] = {
        {ERB_T_INT, ERB_SLOT_TRACKED, 0, 0, 5, 0, ERB_NONE},
        {ERB_T_FLOAT, 0, ERB_NONE, 4, speed_bits, 0, ERB_NONE},
        {ERB_T_INT64, 0, ERB_NONE, 8, 0, 0, ERB_NONE},
        {ERB_T_ENUM, 0, ERB_NONE, 16, 1, 0, 0},
        {ERB_T_BOOL, ERB_SLOT_BOOT_ONLY, ERB_NONE, 20, 0, 0, ERB_NONE},
        {ERB_T_AVALUE, 0, ERB_NONE, 24, 0, 0, ERB_NONE},
        {ERB_T_ANIM, 0, ERB_NONE, 28, 0, 0, ERB_NONE},
        {ERB_T_STRING, ERB_SLOT_TRACKED, 1, 40, STR_HELLO, 15, ERB_NONE},
        {ERB_T_LIST, 0, ERB_NONE, 56, 0, 4, 0},
        {ERB_T_NODE, 0, ERB_NONE, 92, 0, 0, ERB_NONE},
        {ERB_T_NODE, 0, ERB_NONE, 100, 0, 4, ERB_NONE},
    };
    memcpy(pkg->slots, slots, sizeof(slots));
    pkg->slot_count = 11;

    pkg->enum_sets[0] = (TSet){0, 2};
    pkg->enum_set_count = 1;
    pkg->enum_values[0] = STR_ON;
    pkg->enum_values[1] = STR_OFF;
    pkg->enum_value_count = 2;

    pkg->rows[0] = (TRow){8, 0, 2};
    pkg->row_count = 1;
    pkg->fields[0] = (TField){ERB_T_INT, 0, 0};
    pkg->fields[1] = (TField){ERB_T_FLOAT, 0, 4};
    pkg->field_count = 2;

    pkg->prop_sets[0] = (TSet){0, 1};
    pkg->prop_sets[1] = (TSet){1, 2};
    pkg->prop_sets[2] = (TSet){3, 1};
    pkg->prop_set_count = 3;
    pkg->prop_entries[0] = (TPropEntry){PROP_TEXT, ERB_PK_STRING, STR_HELLO};
    pkg->prop_entries[1] = (TPropEntry){PROP_WIDTH, ERB_PK_I16, 100};
    pkg->prop_entries[2] = (TPropEntry){PROP_BACKGROUND_COLOR, ERB_PK_COLOR, COLOR_BLUE};
    pkg->prop_entries[3] = (TPropEntry){PROP_IMAGE_NAME, ERB_PK_IMAGE, STR_IMG};
    pkg->prop_entry_count = 4;

    pkg->templates[0] = (TTemplate){0, 4, ERB_TPL_ROOT};
    pkg->templates[1] = (TTemplate){4, 1, ERB_TPL_ROW};
    pkg->templates[2] = (TTemplate){5, 1, ERB_TPL_BRANCH};
    pkg->template_count = 3;
    const TNode nodes[] = {
        {ER_NODE_VIEW, 0, 3, 1, S_ROOT, 0, 1},
        {ER_NODE_TEXT, 0, 0, 0, ERB_NONE, 0, 0},
        {ER_NODE_IMAGE, 0, 0, 2, ERB_NONE, 0, 0},
        {ER_NODE_VECTOR, 0, 0, ERB_NONE, ERB_NONE, 1, 1},
        {ER_NODE_VIEW, 0, 0, ERB_NONE, S_ROWS, 2, 1},
        {ER_NODE_TEXT, 0, 0, 0, ERB_NONE, 0, 0},
    };
    memcpy(pkg->nodes, nodes, sizeof(nodes));
    pkg->node_count = 6;

    pkg->bindings[0] = (TBinding){ERB_BIND_EVENT, ER_EVENT_PRESS, F_PRESS};
    pkg->bindings[1] = (TBinding){ERB_BIND_TAPE, 0, 0};
    pkg->bindings[2] = (TBinding){ERB_BIND_ANIMATED, PROP_OPACITY, S_FADE};
    pkg->binding_count = 3;

    {
        const uint8_t types[] = {ERB_T_BOOL};
        const uint32_t code[] = {OP_ABX(LOADI, 0, 1), OP_ABX(SET32, 0, S_READY), OP_ABC(RET, 0, 0, 0)};
        tpkg_function(pkg, ERB_FN_BOOT, 0, 0, types, 1, code, 3);
    }
    {
        const uint32_t code[] = {OP_ABX(JDIRTY, 0, 0), 1u, OP_ABC(RET, 0, 0, 0)};
        tpkg_function(pkg, ERB_FN_UPDATE, 0, 0, NULL, 0, code, 3);
    }
    {
        const uint8_t types[] = {ERB_T_INT, ERB_T_INT};
        const uint32_t code[] = {
            OP_ABX(GET32, 0, S_COUNT),
            OP_ABX(LOADI, 1, 1),
            OP_ABC(ADD, 0, 0, 1),
            OP_ABX(CALL, 0, F_DOUBLE),
            OP_ABX(SET32, 0, S_COUNT),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_function(pkg, ER_EVENT_PRESS, 0, 0, types, 2, code, 6);
    }
    {
        const uint32_t code[] = {OP_ABC(RET, 0, 0, 0)};
        tpkg_function(pkg, ERB_FN_TIMER, 0, 0, NULL, 0, code, 1);
        tpkg_function(pkg, ERB_FN_EFFECT, 0, 0, NULL, 0, code, 1);
        tpkg_function(pkg, ERB_FN_ANIM_DONE, 0, 0, NULL, 0, code, 1);
    }
    {
        const uint8_t types[] = {ERB_T_INT, ERB_T_INT};
        const uint32_t code[] = {OP_ABX(LOADI, 1, 2), OP_ABC(MUL, 0, 0, 1), OP_ABC(RET, 0, 0, 0)};
        tpkg_function(pkg, ERB_FN_HELPER, 1, ERB_T_INT, types, 2, code, 3);
    }

    pkg->anims[0] = (TAnim){ERB_ANIM_SEQUENCE, 0, 0, ERB_NONE, 0, 1, 1, 0, 0, 0, {0, 0, 0, 0}};
    pkg->anims[1] = (TAnim){ERB_ANIM_TIMING, ER_EASE_LINEAR, 0, S_FADE, 0, 0, 0, 100, 0, FLOAT_ONE_BITS, {0, 0, 0, 0}};
    pkg->anim_count = 2;

    const float floats[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 10.0f, 10.0f, 6.0f};
    memcpy(pkg->tape_floats, floats, sizeof(floats));
    pkg->tape_float_count = 9;
    pkg->tapes[0] = (TTape){0, 9, 0, 1, 0, 0};
    pkg->tape_count = 1;
    pkg->paints[0] = (TPaint){COLOR_WHITE, 0u, 0.0f, 0.0f, 0, 0, 0, 0, 0};
    pkg->paint_count = 1;

    pkg->imports[0] = (TImport){STR_SENSORS, 1, STR_SIG};
    pkg->import_count = 1;
    pkg->inputs[0] = (TInput){STR_STEPS, S_COUNT};
    pkg->input_count = 1;

    pkg->has_debug = true;
    pkg->debug_build_id = pkg->build_id;
    pkg->debug_entries[0] = (TDebugEntry){F_PRESS, 0, 0, 41};
    pkg->debug_entry_count = 1;
    pkg->debug_strings[0] = "App.jsx";
    pkg->debug_string_count = 1;

    /* An ERPK v2 pack with one 1x1 ARGB image named "img": format byte, then a pad so the pixels sit at 32. */
    static const uint8_t assets[] = {
        'E', 'R', 'P', 'K', 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,    0,    3,    0,
        'i', 'm', 'g', 1,   0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    memcpy(pkg->assets, assets, sizeof(assets));
    pkg->assets_length = sizeof(assets);
}

#endif
