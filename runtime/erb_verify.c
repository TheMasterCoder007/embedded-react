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
 * erb_verify — everything a package is checked for at load (the spec's "Verifier rules"). Reads only the
 * package, so a reload that fails here leaves the running app untouched. What passes needs no checking
 * again at run time, except the short list in the spec's "Run-time checks".
 *
 * Every walk is linear in the package's size: tables whose records must not overlap are required in
 * ascending order instead of being compared pairwise.
 */

#include "erb_internal.h"
#include "erb_props.h"

#include "er_assets.h"
#include "er_container.h"

#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The largest anim_count ASTART can address with a 16-bit entry index. */
#define MAX_ANIM_ENTRIES 0xFFFEu

/** @brief Most timers (an id is gen * count + index, which must stay a positive int). */
#define MAX_TIMERS 4096u

/*----------------------------------------------------------------------------------------------------------------------
 - Opcode table
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    uint8_t form;
    uint8_t words;
    uint8_t writes;
    uint8_t operand_a_kind;
    uint8_t operand_b_kind;
    uint8_t operand_c_kind;
} OpInfo;

static const OpInfo s_opcodes[ERB_OP_COUNT_] = {[ERB_OP_INVALID_] = {0u, 0u, 0u, 0u, 0u, 0u},
#define OP_INFO_(name, form, words, writes, operand_a_kind, operand_b_kind, operand_c_kind)                            \
    [ERB_OP_##name] = {form, words, writes, operand_a_kind, operand_b_kind, operand_c_kind},
                                                ERB_OPCODES(OP_INFO_)
#undef OP_INFO_
};

uint32_t erb_op_words(uint8_t opcode)
{
    return opcode < ERB_OP_COUNT_ ? s_opcodes[opcode].words : 0u;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Verification scratch (fixed; only used while verifying)
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    uint32_t depth; /**< Register-stack cells on its deepest call path, its own frame included. */
    uint32_t worst; /**< Most instructions one call can execute, saturated. */
} FunctionCost;

typedef struct
{
    uint32_t start; /**< First word of the body (just after its LOOPSET). */
    uint32_t end;   /**< The LOOP instruction. */
    uint32_t bound;
    uint8_t counter;
} Loop;

static FunctionCost s_costs[ERB_MAX_FUNCTIONS];
static uint32_t s_starts[ERB_BIT_WORDS(ERB_MAX_FUNCTION_WORDS)];
static Loop s_loops[ERB_MAX_LOOPS];
static uint32_t s_loop_count;
static uint16_t s_tree[ERB_MAX_ANIM_TREE];

/* The asset pack's image names, sorted, so each image prop is a binary search rather than a walk of the pack. */
static uint32_t s_image_names[ERB_MAX_IMAGES];
static uint32_t s_image_count;

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static bool fail(ErbError* error, ErbStatus status, const char* detail)
{
    error->status = status;
    error->detail = detail;
    return false;
}

/** @brief Entries a caller storage array holds: none when it isn't there, whatever its count says. */
static uint32_t storage_room(const void* array, uint32_t count)
{
    return array ? count : 0u;
}

static bool too_big(ErbError* error, uint16_t resource, uint32_t needed, uint32_t available)
{
    error->resource = resource;
    error->needed = needed;
    error->available = available;
    return fail(error, ERB_ERROR_TOO_BIG, "needs more than the firmware reserves");
}

static uint32_t round4(uint32_t bytes)
{
    return (bytes + 3u) & ~3u;
}

static uint32_t sat_add(uint32_t left, uint32_t right)
{
    return left > UINT32_MAX - right ? UINT32_MAX : left + right;
}

static uint32_t sat_mul(uint32_t left, uint32_t right)
{
    const uint64_t product = (uint64_t)left * right;
    return product > UINT32_MAX ? UINT32_MAX : (uint32_t)product;
}

/** @brief True when [offset, offset + length) lies inside [0, limit), without overflowing. */
static bool in_bounds(uint32_t offset, uint32_t length, uint32_t limit)
{
    return offset <= limit && length <= limit - offset;
}

static bool is_start(uint32_t program_counter)
{
    return erb_bit_get(s_starts, program_counter);
}

/** @brief Checks one string of UTF-8: no overlong forms, surrogates or code points past U+10FFFF. */
static bool valid_utf8(const uint8_t* bytes, size_t length)
{
    size_t index = 0;
    while (index < length)
    {
        const uint8_t lead = bytes[index];
        uint32_t follow;
        uint32_t code;
        uint32_t lowest;
        if (lead < 0x80u)
        {
            index++;
            continue;
        }
        if ((lead & 0xE0u) == 0xC0u)
        {
            follow = 1u;
            code = lead & 0x1Fu;
            lowest = 0x80u;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            follow = 2u;
            code = lead & 0x0Fu;
            lowest = 0x800u;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            follow = 3u;
            code = lead & 0x07u;
            lowest = 0x10000u;
        }
        else
        {
            return false;
        }
        if (length - index <= follow)
        {
            return false;
        }
        for (uint32_t byte_index = 1; byte_index <= follow; byte_index++)
        {
            const uint8_t next = bytes[index + byte_index];
            if ((next & 0xC0u) != 0x80u)
            {
                return false;
            }
            code = (code << 6) | (next & 0x3Fu);
        }
        if (code < lowest || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu))
        {
            return false;
        }
        index += follow + 1u;
    }
    return true;
}

/**
 * @brief Checks a strings table: `count u32`, `count` offsets, then NUL-terminated UTF-8. Strings must be in
 *        order, each starting after the previous one's NUL, so the walk is linear.
 *
 * @param[out] count  Receives the string count.
 */
static bool check_strings(const uint8_t* table, uint32_t length, uint32_t* count)
{
    if (length < 4u)
    {
        return false;
    }
    const uint32_t string_count = erb_u32(table + ERB_STRINGS_COUNT_AT);
    if (string_count > (length - 4u) / 4u)
    {
        return false;
    }
    uint32_t next = ERB_STRINGS_OFFSETS_AT + string_count * sizeof(uint32_t);
    for (uint32_t index = 0; index < string_count; index++)
    {
        const uint32_t offset = erb_u32(table + ERB_STRINGS_OFFSETS_AT + index * sizeof(uint32_t));
        if (offset < next || offset >= length)
        {
            return false;
        }
        const uint8_t* nul = (const uint8_t*)memchr(table + offset, 0, length - offset);
        if (!nul || !valid_utf8(table + offset, (size_t)(nul - (table + offset))))
        {
            return false;
        }
        next = (uint32_t)(nul - table) + 1u;
    }
    *count = string_count;
    return true;
}

static size_t string_length(const ErbProgram* program, uint32_t index)
{
    return strlen(erb_string(program, index));
}

uint32_t erb_field_bytes(uint8_t type, uint16_t capacity)
{
    switch (type)
    {
        case ERB_TYPE_INT64:
        case ERB_TYPE_DOUBLE:
            return 8u;
        case ERB_TYPE_STRING:
            return round4((uint32_t)capacity + 1u);
        default:
            return 4u;
    }
}

uint32_t erb_slot_bytes(const ErbProgram* program, const uint8_t* slot)
{
    const uint8_t type = slot[ERB_SLOT_TYPE_AT];
    const uint16_t capacity = erb_u16(slot + ERB_SLOT_CAP_AT);
    switch (type)
    {
        case ERB_TYPE_INT64:
        case ERB_TYPE_DOUBLE:
            return 8u;
        case ERB_TYPE_NODE:
            return 8u * (capacity ? capacity : 1u);
        case ERB_TYPE_STRING:
            return round4((uint32_t)capacity + 1u);
        case ERB_TYPE_LIST:
            return 4u
                   + (uint32_t)capacity * erb_u16(erb_row(program, erb_u16(slot + ERB_SLOT_AUX_AT)) + ERB_ROW_SIZE_AT);
        default:
            return 4u;
    }
}

/** @brief The region a slot of this type lives in. */
static uint8_t slot_region(uint8_t type)
{
    switch (type)
    {
        case ERB_TYPE_NODE:
            return ERB_REGION_HANDLES;
        case ERB_TYPE_STRING:
            return ERB_REGION_STRINGS;
        case ERB_TYPE_LIST:
            return ERB_REGION_LISTS;
        default:
            return ERB_REGION_SLOTS;
    }
}

static bool is_wide(uint8_t type)
{
    return type == ERB_TYPE_INT64 || type == ERB_TYPE_DOUBLE || type == ERB_TYPE_NODE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Container, header, panel and needs (steps 1 to 5)
 ---------------------------------------------------------------------------------------------------------------------*/

static bool check_container(const void* package, size_t package_bytes, ErbProgram* program, ErbError* error)
{
    ErContainer container;
    if (er_container_read(package, package_bytes, &container) != ER_CONTAINER_OK)
    {
        return fail(error, ERB_ERROR_CORRUPT, "container magic, version, bounds or CRC");
    }
    if (!erb_aligned4((uintptr_t)package))
    {
        return fail(error, ERB_ERROR_NOT_FLOW_B, "container not 4-byte aligned");
    }
    if (container.tag_length != sizeof(ERB_TAG) - 1u || memcmp(container.tag, ERB_TAG, sizeof(ERB_TAG) - 1u) != 0)
    {
        return fail(error, ERB_ERROR_NOT_FLOW_B, "container tag is not erb");
    }
    uint32_t programs = 0;
    for (uint32_t index = 0; index < container.section_count; index++)
    {
        const ErContainerSection* section = &container.sections[index];
        const bool aligned = erb_aligned4((uintptr_t)(section->data - (const uint8_t*)package));
        switch (section->type)
        {
            case ER_SECTION_BYTECODE:
            case ER_SECTION_VENDOR_BYTECODE:
                return fail(error, ERB_ERROR_NOT_FLOW_B, "QuickJS bytecode section");
            case ER_SECTION_ASSET_PACK:
                if (program->assets || !aligned)
                {
                    return fail(error, ERB_ERROR_NOT_FLOW_B, "second or unaligned asset pack");
                }
                program->assets = section->data;
                program->assets_length = section->length;
                break;
            case ERB_SECTION_PROGRAM:
                if (programs++ || !aligned)
                {
                    return fail(error, ERB_ERROR_NOT_FLOW_B, "second or unaligned program section");
                }
                program->section = section->data;
                program->section_length = section->length;
                break;
            case ERB_SECTION_DEBUG:
                if (program->debug || !aligned)
                {
                    return fail(error, ERB_ERROR_NOT_FLOW_B, "second or unaligned debug section");
                }
                program->debug = section->data;
                program->debug_length = section->length;
                break;
            default:
                break;
        }
    }
    if (!programs)
    {
        return fail(error, ERB_ERROR_NOT_FLOW_B, "no program section");
    }
    return true;
}

/** @brief A check's stack effect: what each op pops and pushes. ERB_CHECK_OP_MATH pops two for a two-operand function.
 */
static const struct
{
    uint8_t pops;
    uint8_t pushes;
} s_check_opcodes[ERB_CHECK_OP_COUNT_] = {
    [ERB_CHECK_OP_END] = {1, 0},    [ERB_CHECK_OP_WIDTH] = {0, 1}, [ERB_CHECK_OP_HEIGHT] = {0, 1},
    [ERB_CHECK_OP_PUSHI] = {0, 1},  [ERB_CHECK_OP_PUSHK] = {0, 1}, [ERB_CHECK_OP_LOAD] = {0, 1},
    [ERB_CHECK_OP_STORE] = {1, 0},  [ERB_CHECK_OP_ADD] = {2, 1},   [ERB_CHECK_OP_SUB] = {2, 1},
    [ERB_CHECK_OP_MUL] = {2, 1},    [ERB_CHECK_OP_DIV] = {2, 1},   [ERB_CHECK_OP_NEG] = {1, 1},
    [ERB_CHECK_OP_MATH] = {1, 1},   [ERB_CHECK_OP_TOINT] = {1, 1}, [ERB_CHECK_OP_IDIV] = {2, 1},
    [ERB_CHECK_OP_LT] = {2, 1},     [ERB_CHECK_OP_GT] = {2, 1},    [ERB_CHECK_OP_LE] = {2, 1},
    [ERB_CHECK_OP_GE] = {2, 1},     [ERB_CHECK_OP_EQ] = {2, 1},    [ERB_CHECK_OP_NE] = {2, 1},
    [ERB_CHECK_OP_NOT] = {1, 1},    [ERB_CHECK_OP_AND] = {2, 1},   [ERB_CHECK_OP_OR] = {2, 1},
    [ERB_CHECK_OP_SELECT] = {3, 1},
};

static bool check_truth(double value)
{
    return value == value && value != 0.0;
}

/**
 * @brief Runs one panel's layout check. It has no jumps and every word is bounds-checked, so a hostile check can do no
 * more than fail, and the budget shared by all the panels' checks limits the time it takes.
 *
 * @param[in,out] budget  Words the load may still run.
 * @param[out] passes  Whether the panel's layout fits the screen.
 *
 * @return false when the check is malformed.
 */
static bool run_check(
    const uint8_t* table, uint32_t length, uint32_t start, const ErbConfig* config, uint32_t* budget, bool* passes)
{
    double stack[ERB_CHECK_STACK];
    double locals[ERB_CHECK_LOCALS];
    memset(locals, 0, sizeof(locals));
    uint32_t depth = 0u;
    const uint32_t words = length / sizeof(uint32_t);
    for (uint32_t program_counter = start; program_counter < words; program_counter++)
    {
        if (*budget == 0u)
        {
            return false;
        }
        (*budget)--;
        const uint32_t word = erb_u32(table + program_counter * sizeof(uint32_t));
        const uint8_t opcode = ERB_CHECK_OP(word);
        const int32_t argument = ERB_CHECK_ARGUMENT(word);
        if (opcode == 0u || opcode >= ERB_CHECK_OP_COUNT_)
        {
            return false;
        }
        const bool binary_math = opcode == ERB_CHECK_OP_MATH && argument >= ERB_MATH_MIN;
        const uint32_t pops = binary_math ? 2u : s_check_opcodes[opcode].pops;
        if (depth < pops || depth - pops + s_check_opcodes[opcode].pushes > ERB_CHECK_STACK)
        {
            return false;
        }
        depth -= pops;
        const double left = pops ? stack[depth] : 0.0;
        const double right = pops > 1u ? stack[depth + 1u] : 0.0;
        double result = 0.0;
        switch (opcode)
        {
            case ERB_CHECK_OP_END:
                *passes = check_truth(left);
                return true;
            case ERB_CHECK_OP_WIDTH:
                result = config->screen_width;
                break;
            case ERB_CHECK_OP_HEIGHT:
                result = config->screen_height;
                break;
            case ERB_CHECK_OP_PUSHI:
                result = argument;
                break;
            case ERB_CHECK_OP_PUSHK:
            {
                if (words - program_counter < 3u)
                {
                    return false;
                }
                const uint64_t bits = (uint64_t)erb_u32(table + (program_counter + 1u) * sizeof(uint32_t))
                                      | ((uint64_t)erb_u32(table + (program_counter + 2u) * sizeof(uint32_t)) << 32);
                memcpy(&result, &bits, sizeof(result));
                program_counter += 2u;
                break;
            }
            case ERB_CHECK_OP_LOAD:
                if (argument < 0 || (uint32_t)argument >= ERB_CHECK_LOCALS)
                {
                    return false;
                }
                result = locals[argument];
                break;
            case ERB_CHECK_OP_STORE:
                if (argument < 0 || (uint32_t)argument >= ERB_CHECK_LOCALS)
                {
                    return false;
                }
                locals[argument] = left;
                continue;
            case ERB_CHECK_OP_ADD:
                result = left + right;
                break;
            case ERB_CHECK_OP_SUB:
                result = left - right;
                break;
            case ERB_CHECK_OP_MUL:
                result = left * right;
                break;
            case ERB_CHECK_OP_DIV:
                result = left / right;
                break;
            case ERB_CHECK_OP_NEG:
                result = -left;
                break;
            case ERB_CHECK_OP_MATH:
                if (argument < 0 || argument >= ERB_MATH_COUNT_)
                {
                    return false;
                }
                result = erb_vm_dmath((uint32_t)argument, left, right);
                break;
            case ERB_CHECK_OP_TOINT:
                result = erb_vm_to_int(left);
                break;
            case ERB_CHECK_OP_IDIV:
                if (argument < 0 || argument >= ERB_CHECK_OP_IDIV_COUNT_)
                {
                    return false;
                }
                result = erb_vm_idiv((uint32_t)argument, left, right);
                break;
            case ERB_CHECK_OP_LT:
                result = left < right;
                break;
            case ERB_CHECK_OP_GT:
                result = left > right;
                break;
            case ERB_CHECK_OP_LE:
                result = left <= right;
                break;
            case ERB_CHECK_OP_GE:
                result = left >= right;
                break;
            case ERB_CHECK_OP_EQ:
                result = left == right;
                break;
            case ERB_CHECK_OP_NE:
                result = left != right;
                break;
            case ERB_CHECK_OP_NOT:
                result = !check_truth(left);
                break;
            case ERB_CHECK_OP_AND:
                result = check_truth(left) && check_truth(right);
                break;
            case ERB_CHECK_OP_OR:
                result = check_truth(left) || check_truth(right);
                break;
            default:
                result = check_truth(left) ? right : stack[depth + 2u];
                break;
        }
        stack[depth++] = result;
    }
    return false;
}

/**
 * @brief Finds the table of layout checks. It comes before the rest of the directory is checked, since it picks
 * the panel the directory is read for.
 *
 * @return false when it is malformed; a package with none leaves @p table NULL.
 */
static bool find_checks(
    const uint8_t* directory, uint16_t table_count, const ErbProgram* program, const uint8_t** table, uint32_t* length)
{
    *table = NULL;
    *length = 0u;
    for (uint16_t index = 0; index < table_count; index++)
    {
        const uint8_t* entry = directory + index * ERB_DIRECTORY_SIZE;
        if (erb_u16(entry + ERB_DIRECTORY_ID_AT) != ERB_TABLE_CHECKS)
        {
            continue;
        }
        const uint32_t offset = erb_u32(entry + ERB_DIRECTORY_OFFSET_AT);
        const uint32_t bytes = erb_u32(entry + ERB_DIRECTORY_LENGTH_AT);
        if (*table || erb_u16(entry + ERB_DIRECTORY_PANEL_AT) != ERB_NONE || !erb_aligned4(offset)
            || !erb_aligned4(bytes) || !in_bounds(offset, bytes, program->section_length))
        {
            return false;
        }
        *table = program->section + offset;
        *length = bytes;
    }
    return true;
}

/**
 * @brief Step 4: the first panel that takes this screen, by its check. Before minor 6, by its exact size.
 *
 * @return false, having filled @p error, when a check is malformed or no panel takes the screen.
 */
static bool pick_panel(const ErbConfig* config,
                       ErbProgram* program,
                       uint16_t panel_count,
                       uint32_t need_count,
                       uint16_t table_count,
                       bool has_checks,
                       ErbError* error)
{
    const uint8_t* section = program->section;
    const uint8_t* checks = NULL;
    uint32_t checks_length = 0u;
    if (has_checks
        && !find_checks(section + ERB_HEADER_SIZE + panel_count * ERB_PANEL_SIZE + need_count * ERB_NEED_SIZE,
                        table_count,
                        program,
                        &checks,
                        &checks_length))
    {
        return fail(error, ERB_ERROR_TABLE, "layout checks table");
    }
    uint32_t budget = ERB_CHECK_BUDGET;
    bool picked = false;
    /* Every check runs, even after one passes, so a malformed one fails the load on every screen. */
    for (uint16_t index = 0; index < panel_count; index++)
    {
        const uint8_t* candidate = section + ERB_HEADER_SIZE + index * ERB_PANEL_SIZE;
        bool passes = false;
        if (!has_checks)
        {
            passes = erb_u16(candidate + ERB_PANEL_WIDTH_AT) == config->screen_width
                     && erb_u16(candidate + ERB_PANEL_HEIGHT_AT) == config->screen_height;
        }
        else if (erb_u32(candidate + ERB_PANEL_CHECK_AT) == ERB_CHECK_ALWAYS)
        {
            passes = true;
        }
        else if (!run_check(checks, checks_length, erb_u32(candidate + ERB_PANEL_CHECK_AT), config, &budget, &passes))
        {
            return fail(error, ERB_ERROR_TABLE, "malformed layout check");
        }
        if (passes && !picked)
        {
            program->panel = index;
            picked = true;
        }
    }
    if (picked)
    {
        return true;
    }
    error->needed = config->screen_width;
    error->available = config->screen_height;
    return fail(error, ERB_ERROR_NO_PANEL, "no layout takes the screen size");
}

static bool check_header(const ErbConfig* config, ErbProgram* program, ErbError* error)
{
    const uint8_t* section = program->section;
    if (program->section_length < ERB_HEADER_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "program section shorter than its header");
    }
    const uint16_t major = erb_u16(section + ERB_HEADER_FORMAT_MAJOR_AT);
    const uint16_t minor = erb_u16(section + ERB_HEADER_FORMAT_MINOR_AT);
    program->engine_abi = erb_u16(section + ERB_HEADER_ENGINE_ABI_AT);
    program->flags = erb_u16(section + ERB_HEADER_FLAGS_AT);
    program->build_id = erb_u32(section + ERB_HEADER_BUILD_ID_AT);
    error->build_id = program->build_id;
    if (major != ERB_FORMAT_MAJOR || minor > ERB_FORMAT_MINOR || (program->flags & ~ERB_FLAGS_KNOWN) != 0u)
    {
        error->needed = major;
        error->available = minor;
        return fail(error, ERB_ERROR_NEWER_FORMAT, "format version or flags");
    }
    if (program->engine_abi < ER_ABI_MIN || program->engine_abi > ER_ABI)
    {
        error->needed = program->engine_abi;
        return fail(error, ERB_ERROR_ENGINE_ABI, "engine ABI level out of range");
    }

    const uint16_t panel_count = erb_u16(section + ERB_HEADER_PANEL_COUNT_AT);
    const uint16_t need_count = erb_u16(section + ERB_HEADER_NEED_COUNT_AT);
    const uint16_t table_count = erb_u16(section + ERB_HEADER_TABLE_COUNT_AT);
    const uint32_t fixed =
        ERB_HEADER_SIZE + panel_count * ERB_PANEL_SIZE + need_count * ERB_NEED_SIZE + table_count * ERB_DIRECTORY_SIZE;
    if (panel_count == 0u || fixed > program->section_length)
    {
        return fail(error, ERB_ERROR_TABLE, "panels, needs and directory past the section");
    }

    /* Step 4: the panel for this screen. */
    const bool has_checks = minor >= ERB_FORMAT_MINOR_CHECKS;
    if (!pick_panel(config, program, panel_count, need_count, table_count, has_checks, error))
    {
        return false;
    }
    const uint8_t* panel = section + ERB_HEADER_SIZE + program->panel * ERB_PANEL_SIZE;
    program->arena_bytes = erb_u32(panel + ERB_PANEL_ARENA_BYTES_AT);
    program->node_count = erb_u16(panel + ERB_PANEL_NODE_COUNT_AT);
    program->root_template = erb_u16(panel + ERB_PANEL_ROOT_TEMPLATE_AT);
    program->boot_function = erb_u16(panel + ERB_PANEL_BOOT_FUNCTION_AT);
    program->update_function = erb_u16(panel + ERB_PANEL_UPDATE_FUNCTION_AT);

    /* Step 5: what it needs against what the firmware reserves. */
    const uint32_t arena_available =
        (config->arena && erb_aligned4((uintptr_t)config->arena)) ? config->arena_bytes : 0u;
    if (program->arena_bytes > arena_available)
    {
        return too_big(error, ERB_RESOURCE_ARENA, program->arena_bytes, arena_available);
    }
    ERPoolLimits limits;
    er_pool_limits(&limits);
    const uint32_t nodes_available =
        (config->node_count && config->node_count < limits.nodes) ? config->node_count : limits.nodes;
    if (program->node_count > nodes_available)
    {
        return too_big(error, ERB_RESOURCE_NODES, program->node_count, nodes_available);
    }
    const ErAssetsStorage* fonts = config->fonts;
    uint32_t named = 0u; /* resources already named, one bit each */
    for (uint16_t index = 0; index < need_count; index++)
    {
        const uint8_t* need = section + ERB_HEADER_SIZE + panel_count * ERB_PANEL_SIZE + index * ERB_NEED_SIZE;
        const uint16_t resource = erb_u16(need + ERB_NEED_RESOURCE_AT);
        const uint32_t amount = erb_u32(need + ERB_NEED_AMOUNT_AT);
        /* Only the chosen layout's needs have to fit. */
        const uint16_t need_panel = has_checks ? erb_u16(need + ERB_NEED_PANEL_AT) : ERB_NONE;
        if (need_panel != ERB_NONE && need_panel >= panel_count)
        {
            return fail(error, ERB_ERROR_TABLE, "a need for a panel the package doesn't declare");
        }
        if (need_panel != ERB_NONE && need_panel != program->panel)
        {
            continue;
        }
        uint32_t available;
        switch (resource)
        {
            case ERB_RESOURCE_VECTOR_NODES:
                available = limits.vector_nodes;
                break;
            case ERB_RESOURCE_ANIMATION_VALUES:
                available = limits.animation_values;
                break;
            case ERB_RESOURCE_ANIMATIONS:
                available = limits.animations;
                break;
            case ERB_RESOURCE_ANIMATION_GROUPS:
                available = limits.animation_groups;
                break;
            case ERB_RESOURCE_IMAGES:
                available = limits.images;
                break;
            case ERB_RESOURCE_FONTS:
                available = fonts ? storage_room(fonts->fonts, fonts->font_count) : 0u;
                break;
            case ERB_RESOURCE_GLYPHS:
                available = fonts ? storage_room(fonts->glyphs, fonts->glyph_count) : 0u;
                break;
            case ERB_RESOURCE_EXTRA_GLYPHS:
                available = fonts ? storage_room(fonts->extras, fonts->extra_count) : 0u;
                break;
            default:
                error->resource = resource;
                error->needed = amount;
                error->available = 0u;
                return fail(error, ERB_ERROR_TOO_BIG, "unknown resource");
        }
        /* Named twice, each need could pass alone while the two together don't fit. */
        if (named & (1u << resource))
        {
            return fail(error, ERB_ERROR_TABLE, "a resource needed twice");
        }
        named |= 1u << resource;
        if (amount > available)
        {
            return too_big(error, resource, amount, available);
        }
        program->needs[resource] = amount;
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Table directory and the data tables
 ---------------------------------------------------------------------------------------------------------------------*/

static bool check_directory(ErbProgram* program, ErbError* error)
{
    const uint8_t* section = program->section;
    const uint16_t panel_count = erb_u16(section + ERB_HEADER_PANEL_COUNT_AT);
    const uint16_t need_count = erb_u16(section + ERB_HEADER_NEED_COUNT_AT);
    const uint16_t table_count = erb_u16(section + ERB_HEADER_TABLE_COUNT_AT);
    const uint8_t* directory = section + ERB_HEADER_SIZE + panel_count * ERB_PANEL_SIZE + need_count * ERB_NEED_SIZE;
    const uint32_t data_start = (uint32_t)(directory - section) + table_count * ERB_DIRECTORY_SIZE;
    const bool has_panels = erb_u16(section + ERB_HEADER_FORMAT_MINOR_AT) >= ERB_FORMAT_MINOR_PANELS;

    for (uint16_t index = 0; index < table_count; index++)
    {
        const uint8_t* entry = directory + index * ERB_DIRECTORY_SIZE;
        const uint16_t table_id = erb_u16(entry + ERB_DIRECTORY_ID_AT);
        const uint16_t panel = has_panels ? erb_u16(entry + ERB_DIRECTORY_PANEL_AT) : ERB_NONE;
        const uint32_t offset = erb_u32(entry + ERB_DIRECTORY_OFFSET_AT);
        const uint32_t length = erb_u32(entry + ERB_DIRECTORY_LENGTH_AT);
        if (table_id == 0u || table_id >= ERB_TABLE_COUNT_)
        {
            return fail(error, ERB_ERROR_TABLE, "unknown table id");
        }
        if (panel != ERB_NONE && panel >= panel_count)
        {
            return fail(error, ERB_ERROR_TABLE, "a table for a panel the package doesn't declare");
        }
        if (offset < data_start || !erb_aligned4(offset) || !erb_aligned4(length)
            || !in_bounds(offset, length, program->section_length))
        {
            return fail(error, ERB_ERROR_TABLE, "table out of bounds or unaligned");
        }
        /* A table is every panel's (ERB_NONE) or one panel's; only this panel's are used. */
        if (panel != ERB_NONE && panel != program->panel)
        {
            continue;
        }
        if (program->table[table_id])
        {
            return fail(error, ERB_ERROR_TABLE, "table listed twice");
        }
        program->table[table_id] = section + offset;
        program->table_length[table_id] = length;
    }
    /* No two of this panel's tables overlap. At most one of each id, so this is at most 20 x 20. */
    for (uint32_t first = 1; first < ERB_TABLE_COUNT_; first++)
    {
        for (uint32_t second = first + 1u; second < ERB_TABLE_COUNT_; second++)
        {
            if (!program->table[first] || !program->table[second] || !program->table_length[first]
                || !program->table_length[second])
            {
                continue;
            }
            const uint8_t* first_table = program->table[first];
            const uint8_t* second_table = program->table[second];
            if (first_table < second_table + program->table_length[second]
                && second_table < first_table + program->table_length[first])
            {
                return fail(error, ERB_ERROR_TABLE, "tables overlap");
            }
        }
    }
    static const uint8_t required[] = {
        ERB_TABLE_ARENA,
        ERB_TABLE_SLOTS,
        ERB_TABLE_TEMPLATES,
        ERB_TABLE_FUNCTIONS,
        ERB_TABLE_TYPES,
        ERB_TABLE_CODE,
    };
    for (uint32_t index = 0; index < sizeof(required); index++)
    {
        if (!program->table[required[index]])
        {
            return fail(error, ERB_ERROR_TABLE, "a required table is missing");
        }
    }
    return true;
}

/**
 * @brief Splits a table with a header array (`count u32`, `count` headers, then records) into its parts.
 *
 * @return false when the headers don't fit or the rest isn't a whole number of records.
 */
static bool split_table(const ErbProgram* program,
                        uint8_t table_id,
                        uint32_t header_size,
                        uint32_t record_size,
                        const uint8_t** headers,
                        uint32_t* header_count,
                        const uint8_t** records,
                        uint32_t* record_count)
{
    const uint8_t* table = program->table[table_id];
    const uint32_t length = program->table_length[table_id];
    *header_count = 0u;
    *record_count = 0u;
    if (!table)
    {
        return true;
    }
    if (length < 4u)
    {
        return false;
    }
    const uint32_t count = erb_u32(table + ERB_HEADERS_COUNT_AT);
    if (count > (length - 4u) / header_size)
    {
        return false;
    }
    const uint32_t rest = length - 4u - count * header_size;
    if (record_size > 1u && rest % record_size != 0u)
    {
        return false;
    }
    *headers = table + ERB_HEADERS_FIRST_AT;
    *header_count = count;
    *records = table + ERB_HEADERS_FIRST_AT + count * header_size;
    *record_count = rest / record_size;
    return true;
}

static bool check_data_tables(ErbProgram* program, ErbError* error)
{
    if (program->table[ERB_TABLE_STRINGS]
        && !check_strings(
            program->table[ERB_TABLE_STRINGS], program->table_length[ERB_TABLE_STRINGS], &program->string_count))
    {
        return fail(error, ERB_ERROR_STRING, "string offset, terminator, order or UTF-8");
    }
    program->const32 = (const uint32_t*)program->table[ERB_TABLE_CONST32];
    program->const32_count = program->table_length[ERB_TABLE_CONST32] / 4u;
    program->const64 = program->table[ERB_TABLE_CONST64];
    if (program->table_length[ERB_TABLE_CONST64] % 8u)
    {
        return fail(error, ERB_ERROR_TABLE, "const64 not whole records");
    }
    program->const64_count = program->table_length[ERB_TABLE_CONST64] / 8u;

    /* Enums: sets of string indices. */
    if (!split_table(program,
                     ERB_TABLE_ENUMS,
                     ERB_SET_SIZE,
                     2u,
                     &program->enum_sets,
                     &program->enum_set_count,
                     &program->enum_values,
                     &program->enum_value_count))
    {
        return fail(error, ERB_ERROR_TABLE, "enums table malformed");
    }
    for (uint32_t set = 0; set < program->enum_set_count; set++)
    {
        const uint16_t first = erb_u16(program->enum_sets + set * ERB_SET_SIZE);
        const uint16_t count = erb_u16(program->enum_sets + set * ERB_SET_SIZE + 2u);
        if (count == 0u || (uint32_t)first + count > program->enum_value_count)
        {
            return fail(error, ERB_ERROR_TABLE, "enum set out of range");
        }
        for (uint32_t value = 0; value < count; value++)
        {
            if (erb_enum_string(program, set, value) >= program->string_count)
            {
                return fail(error, ERB_ERROR_TABLE, "enum value names no string");
            }
        }
    }

    /* Rows: list row layouts. */
    if (!split_table(program,
                     ERB_TABLE_ROWS,
                     ERB_ROW_SIZE,
                     ERB_FIELD_SIZE,
                     &program->rows,
                     &program->row_count,
                     &program->fields,
                     &program->field_count))
    {
        return fail(error, ERB_ERROR_STATE, "rows table malformed");
    }
    for (uint32_t row = 0; row < program->row_count; row++)
    {
        const uint8_t* layout = erb_row(program, row);
        const uint16_t size = erb_u16(layout + ERB_ROW_SIZE_AT);
        const uint16_t first = erb_u16(layout + ERB_ROW_FIRST_AT);
        const uint16_t field_count = erb_u16(layout + ERB_ROW_FIELD_COUNT_AT);
        if (size == 0u || !erb_aligned4(size) || (uint32_t)first + field_count > program->field_count)
        {
            return fail(error, ERB_ERROR_STATE, "row layout size or fields out of range");
        }
        uint32_t next = 0;
        for (uint32_t index = 0; index < field_count; index++)
        {
            const uint8_t* field = program->fields + (first + index) * ERB_FIELD_SIZE;
            const uint8_t type = field[ERB_FIELD_TYPE_AT];
            const uint16_t capacity = erb_u16(field + ERB_FIELD_CAP_AT);
            const uint32_t offset = erb_u32(field + ERB_FIELD_OFFSET_AT);
            const bool known = type == ERB_TYPE_INT || type == ERB_TYPE_BOOL || type == ERB_TYPE_FLOAT
                               || type == ERB_TYPE_INT64 || type == ERB_TYPE_DOUBLE || type == ERB_TYPE_STRING;
            if (!known || (type == ERB_TYPE_STRING && capacity == 0u) || !erb_aligned4(offset) || offset < next
                || !in_bounds(offset, erb_field_bytes(type, capacity), size))
            {
                return fail(error, ERB_ERROR_STATE, "row field type, cap or offset");
            }
            next = offset + erb_field_bytes(type, capacity);
        }
    }
    return true;
}

static bool check_arena(ErbProgram* program, ErbError* error)
{
    const uint8_t* table = program->table[ERB_TABLE_ARENA];
    const uint32_t length = program->table_length[ERB_TABLE_ARENA];
    if (length % ERB_REGION_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "arena table not whole records");
    }
    uint32_t next = 0;
    uint32_t total = 0;
    for (uint32_t index = 0; index < length / ERB_REGION_SIZE; index++)
    {
        const uint8_t* record = table + index * ERB_REGION_SIZE;
        const uint16_t kind = erb_u16(record + ERB_REGION_KIND_AT);
        const uint16_t count = erb_u16(record + ERB_REGION_COUNT_AT);
        const uint32_t offset = erb_u32(record + ERB_REGION_OFFSET_AT);
        const uint32_t bytes = erb_u32(record + ERB_REGION_BYTES_AT);
        if (kind == 0u || kind >= ERB_REGION_COUNT_ || program->regions[kind].present)
        {
            return fail(error, ERB_ERROR_ARENA, "unknown or repeated region");
        }
        if (!erb_aligned4(offset) || !erb_aligned4(bytes) || offset < next
            || !in_bounds(offset, bytes, program->arena_bytes))
        {
            return fail(error, ERB_ERROR_ARENA, "region unaligned, overlapping or past arena_bytes");
        }
        uint32_t expected;
        switch (kind)
        {
            case ERB_REGION_DIRTY:
                expected = ERB_DIRTY_COPIES * ERB_BIT_WORDS(count) * sizeof(uint32_t);
                break;
            case ERB_REGION_TIMERS:
                expected = count <= MAX_TIMERS ? count * ERB_TIMER_SIZE : 0u;
                break;
            case ERB_REGION_ANIMATIONS:
                expected = count <= MAX_ANIM_ENTRIES ? count * ERB_ANIMATION_ENTRY_SIZE : 0u;
                break;
            case ERB_REGION_STACK:
            case ERB_REGION_TAPE:
                expected = count * sizeof(int32_t); /* a register cell or a tape float */
                break;
            case ERB_REGION_EVENTS:
                expected = count * ERB_EVENT_SIZE;
                break;
            default:
                expected = bytes;
                break;
        }
        if (bytes != expected
            || ((kind == ERB_REGION_TIMERS || kind == ERB_REGION_ANIMATIONS || kind == ERB_REGION_STACK
                 || kind == ERB_REGION_EVENTS)
                && count == 0u))
        {
            return fail(error, ERB_ERROR_ARENA, "region size doesn't match its count");
        }
        program->regions[kind].offset = offset;
        program->regions[kind].bytes = bytes;
        program->regions[kind].count = count;
        program->regions[kind].present = true;
        next = offset + bytes;
        total += bytes;
    }
    if (total != program->arena_bytes)
    {
        return fail(error, ERB_ERROR_ARENA, "regions don't add up to arena_bytes");
    }
    if (!program->regions[ERB_REGION_STACK].present)
    {
        return fail(error, ERB_ERROR_ARENA, "no register stack");
    }
    program->dirty_words = ERB_BIT_WORDS(program->regions[ERB_REGION_DIRTY].count);
    return true;
}

static bool check_slots(ErbProgram* program, ErbError* error)
{
    const uint32_t length = program->table_length[ERB_TABLE_SLOTS];
    if (length % ERB_SLOT_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "slots table not whole records");
    }
    program->slots = program->table[ERB_TABLE_SLOTS];
    program->slot_count = length / ERB_SLOT_SIZE;
    if (program->slot_count > ERB_NONE)
    {
        return fail(error, ERB_ERROR_STATE, "too many slots");
    }
    uint32_t next = 0;
    uint32_t animated_values = 0;
    for (uint32_t index = 0; index < program->slot_count; index++)
    {
        const uint8_t* slot = erb_slot(program, index);
        const uint8_t type = slot[ERB_SLOT_TYPE_AT];
        const uint8_t flags = slot[ERB_SLOT_FLAGS_AT];
        const uint16_t dirty_bit = erb_u16(slot + ERB_SLOT_DIRTY_BIT_AT);
        const uint32_t offset = erb_u32(slot + ERB_SLOT_OFFSET_AT);
        const uint32_t initial = erb_u32(slot + ERB_SLOT_INIT_AT);
        const uint16_t capacity = erb_u16(slot + ERB_SLOT_CAP_AT);
        const uint16_t enum_or_row = erb_u16(slot + ERB_SLOT_AUX_AT);
        if (type == ERB_TYPE_PAIR || type >= ERB_TYPE_COUNT_ || (flags & ~ERB_SLOT_FLAGS_KNOWN))
        {
            return fail(error, ERB_ERROR_STATE, "slot type or flags");
        }
        if ((flags & ERB_SLOT_TRACKED) && dirty_bit >= program->regions[ERB_REGION_DIRTY].count)
        {
            return fail(error, ERB_ERROR_STATE, "dirty bit out of range");
        }
        if ((type == ERB_TYPE_ENUM && enum_or_row >= program->enum_set_count)
            || (type == ERB_TYPE_LIST && enum_or_row >= program->row_count))
        {
            return fail(error, ERB_ERROR_STATE, "enum set or row layout missing");
        }
        const bool has_capacity = type == ERB_TYPE_STRING || type == ERB_TYPE_LIST || type == ERB_TYPE_NODE;
        if ((!has_capacity && capacity) || ((type == ERB_TYPE_STRING || type == ERB_TYPE_LIST) && capacity == 0u))
        {
            return fail(error, ERB_ERROR_STATE, "slot cap");
        }
        const ErbRegion* region = &program->regions[slot_region(type)];
        const uint32_t bytes = erb_slot_bytes(program, slot);
        if (!erb_aligned4(offset) || offset < next || !region->present || offset < region->offset
            || !in_bounds(offset - region->offset, bytes, region->bytes))
        {
            return fail(error, ERB_ERROR_STATE, "slot outside its region, unaligned or overlapping");
        }
        next = offset + bytes;

        bool init_ok;
        switch (type)
        {
            case ERB_TYPE_BOOL:
                init_ok = initial <= 1u;
                break;
            case ERB_TYPE_INT64:
            case ERB_TYPE_DOUBLE:
                init_ok = initial == UINT32_MAX || initial < program->const64_count;
                break;
            case ERB_TYPE_STRING:
                init_ok = initial == UINT32_MAX
                          || (initial < program->string_count && string_length(program, initial) <= capacity);
                break;
            case ERB_TYPE_ENUM:
                init_ok = initial < erb_enum_count(program, enum_or_row);
                break;
            case ERB_TYPE_NODE:
            case ERB_TYPE_ANIMATION:
            case ERB_TYPE_LIST:
                init_ok = initial == 0u;
                break;
            default:
                init_ok = true;
                break;
        }
        if (!init_ok)
        {
            return fail(error, ERB_ERROR_STATE, "slot initial value");
        }
        animated_values += type == ERB_TYPE_ANIMATED_VALUE ? 1u : 0u;
    }
    /* Each one is made at load, after the running app is gone; the need was held to the engine's pool. */
    if (animated_values > program->needs[ERB_RESOURCE_ANIMATION_VALUES])
    {
        return fail(error, ERB_ERROR_STATE, "more animated values than the package's needs declare");
    }
    return true;
}

/** @brief Checks a constant prop entry's value against its prop. */
static bool check_prop_value(const ErbProgram* program, const ErbPropInfo* info, uint32_t value)
{
    switch (info->kind)
    {
        case ERB_PROP_KIND_I16:
            return (int32_t)value >= INT16_MIN && (int32_t)value <= INT16_MAX;
        case ERB_PROP_KIND_U8:
        case ERB_PROP_KIND_OPACITY:
            return value <= UINT8_MAX;
        case ERB_PROP_KIND_U16:
            return value <= UINT16_MAX;
        case ERB_PROP_KIND_ENUM:
            return value <= info->max;
        case ERB_PROP_KIND_BOOL:
            return value <= 1u;
        case ERB_PROP_KIND_STRING:
            return value < program->string_count && string_length(program, value) <= info->max;
        case ERB_PROP_KIND_IMAGE:
            return value < program->string_count && string_length(program, value) <= info->max && program->assets
                   && er_assets_find_image(program->assets, s_image_names, s_image_count, erb_string(program, value));
        default:
            return true;
    }
}

static bool check_props(ErbProgram* program, ErbError* error)
{
    if (!split_table(program,
                     ERB_TABLE_PROPS,
                     ERB_SET_SIZE,
                     ERB_PROP_SIZE,
                     &program->prop_sets,
                     &program->prop_set_count,
                     &program->prop_entries,
                     &program->prop_entry_count))
    {
        return fail(error, ERB_ERROR_TABLE, "props table malformed");
    }
    for (uint32_t set = 0; set < program->prop_set_count; set++)
    {
        const uint16_t first = erb_u16(program->prop_sets + set * ERB_SET_SIZE);
        const uint16_t count = erb_u16(program->prop_sets + set * ERB_SET_SIZE + 2u);
        if ((uint32_t)first + count > program->prop_entry_count)
        {
            return fail(error, ERB_ERROR_TABLE, "prop set out of range");
        }
    }
    for (uint32_t index = 0; index < program->prop_entry_count; index++)
    {
        const uint8_t* entry = program->prop_entries + index * ERB_PROP_SIZE;
        const ErbPropInfo* info = erb_prop_info(erb_u16(entry + ERB_PROP_ID_AT));
        if (!info)
        {
            return fail(error, ERB_ERROR_PROP, "prop id not in this firmware's ABI");
        }
        if (entry[ERB_PROP_KIND_AT] != info->kind
            || !check_prop_value(program, info, erb_u32(entry + ERB_PROP_VALUE_AT)))
        {
            return fail(error, ERB_ERROR_PROP, "prop value kind or range");
        }
    }
    return true;
}

static bool is_entry_kind(uint8_t kind)
{
    return kind < ER_EVENT_TYPE_COUNT_
           || (kind >= ERB_FUNCTION_KIND_QUERY && kind < ERB_FUNCTION_KIND_QUERY + ER_RESPONDER_QUERY_COUNT)
           || (kind >= ERB_FUNCTION_KIND_TIMER && kind <= ERB_FUNCTION_KIND_ANIMATION_DONE
               && kind != ERB_FUNCTION_KIND_HELPER);
}

static bool check_functions(ErbProgram* program, ErbError* error)
{
    const uint32_t length = program->table_length[ERB_TABLE_FUNCTIONS];
    if (length % ERB_FUNCTION_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "functions table not whole records");
    }
    program->functions = program->table[ERB_TABLE_FUNCTIONS];
    program->function_count = length / ERB_FUNCTION_SIZE;
    program->types = program->table[ERB_TABLE_TYPES];
    program->types_length = program->table_length[ERB_TABLE_TYPES];
    program->code = (const uint32_t*)program->table[ERB_TABLE_CODE];
    program->code_words = program->table_length[ERB_TABLE_CODE] / 4u;
    if (program->function_count > ERB_MAX_FUNCTIONS)
    {
        return fail(error, ERB_ERROR_FUNCTION, "more functions than ERB_MAX_FUNCTIONS");
    }
    for (uint32_t index = 0; index < program->function_count; index++)
    {
        const uint8_t* function = erb_function(program, index);
        const uint32_t code = erb_u32(function + ERB_FUNCTION_CODE_AT);
        const uint32_t words = erb_u32(function + ERB_FUNCTION_WORDS_AT);
        const uint32_t types = erb_u32(function + ERB_FUNCTION_TYPES_AT);
        const uint8_t registers = function[ERB_FUNCTION_REGISTERS_AT];
        const uint8_t params = function[ERB_FUNCTION_PARAMS_AT];
        const uint8_t result = function[ERB_FUNCTION_RESULT_AT];
        const uint8_t kind = function[ERB_FUNCTION_KIND_AT];
        if (words == 0u || words > ERB_MAX_FUNCTION_WORDS || !in_bounds(code, words, program->code_words)
            || !in_bounds(types, registers, program->types_length))
        {
            return fail(error, ERB_ERROR_FUNCTION, "code or types out of bounds");
        }
        const uint8_t* type = program->types + types;
        for (uint32_t register_index = 0; register_index < registers; register_index++)
        {
            if (is_wide(type[register_index]))
            {
                if ((register_index & 1u) || register_index + 1u >= registers
                    || type[register_index + 1u] != ERB_TYPE_PAIR)
                {
                    return fail(error, ERB_ERROR_FUNCTION, "64-bit register not an even pair");
                }
                register_index++;
            }
            else if (type[register_index] == ERB_TYPE_PAIR || type[register_index] >= ERB_TYPE_REGISTER_COUNT_)
            {
                return fail(error, ERB_ERROR_FUNCTION, "register type");
            }
        }
        if (params > registers || (params && type[params - 1u] != ERB_TYPE_PAIR && is_wide(type[params - 1u]))
            || result >= ERB_TYPE_REGISTER_COUNT_)
        {
            return fail(error, ERB_ERROR_FUNCTION, "parameters or result");
        }
        if (!is_entry_kind(kind) && kind != ERB_FUNCTION_KIND_HELPER)
        {
            return fail(error, ERB_ERROR_FUNCTION, "function kind");
        }
        /* Entry points take nothing but a handler's instance index, and only queries return (a bool). */
        const bool handler = kind < ER_EVENT_TYPE_COUNT_;
        const bool query = kind >= ERB_FUNCTION_KIND_QUERY && kind < ERB_FUNCTION_KIND_QUERY + ER_RESPONDER_QUERY_COUNT;
        if (kind != ERB_FUNCTION_KIND_HELPER)
        {
            const bool params_ok = params == 0u || ((handler || query) && params == 1u && type[0] == ERB_TYPE_INT);
            const bool result_ok = query ? result == ERB_TYPE_BOOL : result == 0u;
            if (!params_ok || !result_ok)
            {
                return fail(error, ERB_ERROR_FUNCTION, "entry point parameters or result");
            }
        }
    }
    if ((program->boot_function != ERB_NONE
         && (program->boot_function >= program->function_count
             || erb_function(program, program->boot_function)[ERB_FUNCTION_KIND_AT] != ERB_FUNCTION_KIND_BOOT))
        || (program->update_function != ERB_NONE
            && (program->update_function >= program->function_count
                || erb_function(program, program->update_function)[ERB_FUNCTION_KIND_AT] != ERB_FUNCTION_KIND_UPDATE)))
    {
        return fail(error, ERB_ERROR_FUNCTION, "panel boot or update function");
    }
    return true;
}

static bool check_interps(ErbProgram* program, ErbError* error)
{
    const uint32_t length = program->table_length[ERB_TABLE_INTERPOLATIONS];
    if (length % ERB_INTERPOLATION_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "interps table not whole records");
    }
    program->interps = program->table[ERB_TABLE_INTERPOLATIONS];
    program->interp_count = length / ERB_INTERPOLATION_SIZE;
    if (program->interp_count > ERB_NONE)
    {
        return fail(error, ERB_ERROR_ANIMATION, "too many interpolations");
    }
    for (uint32_t index = 0; index < program->interp_count; index++)
    {
        const uint8_t* interp = erb_interp(program, index);
        const uint16_t slot = erb_u16(interp + ERB_INTERPOLATION_SLOT_AT);
        const uint8_t points = interp[ERB_INTERPOLATION_POINT_COUNT_AT];
        if (slot >= program->slot_count || erb_slot(program, slot)[ERB_SLOT_TYPE_AT] != ERB_TYPE_ANIMATED_VALUE
            || points < 2u || points > ER_INTERPOLATE_MAX_POINTS
            || (interp[ERB_INTERPOLATION_FLAGS_AT] & ~ERB_INTERPOLATION_FLAGS_KNOWN)
            || interp[ERB_INTERPOLATION_EXTRAPOLATE_LEFT_AT] > ER_EXTRAPOLATE_IDENTITY
            || interp[ERB_INTERPOLATION_EXTRAPOLATE_RIGHT_AT] > ER_EXTRAPOLATE_IDENTITY)
        {
            return fail(error, ERB_ERROR_ANIMATION, "interpolation value, points, flags or extrapolation");
        }
    }
    return true;
}

static bool check_bindings(ErbProgram* program, ErbError* error)
{
    const uint32_t length = program->table_length[ERB_TABLE_BINDINGS];
    program->bindings = program->table[ERB_TABLE_BINDINGS];
    program->binding_count = length / ERB_BINDING_SIZE;
    for (uint32_t index = 0; index < program->binding_count; index++)
    {
        const uint8_t* binding = program->bindings + index * ERB_BINDING_SIZE;
        const uint8_t kind = binding[ERB_BINDING_KIND_AT];
        const uint8_t event = binding[ERB_BINDING_EVENT_AT];
        const uint16_t target = erb_u16(binding + ERB_BINDING_TARGET_AT);
        bool ok;
        switch (kind)
        {
            case ERB_BIND_EVENT:
                ok = event < ER_EVENT_TYPE_COUNT_ && target < program->function_count
                     && erb_function(program, target)[ERB_FUNCTION_KIND_AT] == event;
                break;
            case ERB_BIND_QUERY:
                ok = event < ER_RESPONDER_QUERY_COUNT && target < program->function_count
                     && erb_function(program, target)[ERB_FUNCTION_KIND_AT] == ERB_FUNCTION_KIND_QUERY + event;
                break;
            case ERB_BIND_ANIMATED:
            {
                /* A color takes only an interpolation onto colors, as the engine binds one. */
                const ErbPropInfo* info = erb_prop_info(event);
                ok = info && info->animation_prop != ERB_NO_ANIMATION && info->kind != ERB_PROP_KIND_COLOR
                     && target < program->slot_count
                     && erb_slot(program, target)[ERB_SLOT_TYPE_AT] == ERB_TYPE_ANIMATED_VALUE;
                break;
            }
            case ERB_BIND_INTERPOLATED:
            {
                const ErbPropInfo* info = erb_prop_info(event);
                ok = info && info->animation_prop != ERB_NO_ANIMATION && target < program->interp_count
                     && ((erb_interp(program, target)[ERB_INTERPOLATION_FLAGS_AT] & ERB_INTERPOLATION_FLAG_COLOR) != 0u)
                            == (info->kind == ERB_PROP_KIND_COLOR);
                break;
            }
            case ERB_BIND_TAPE:
                ok = target < program->tape_count;
                break;
            default:
                ok = false;
                break;
        }
        if (!ok)
        {
            return fail(error, ERB_ERROR_BINDING, "binding kind, event or target");
        }
    }
    return true;
}

/** @brief True when a node of this type takes children. */
static bool takes_children(uint8_t type)
{
    switch (type)
    {
        case ER_NODE_TEXT:
        case ER_NODE_IMAGE:
        case ER_NODE_TEXT_INPUT:
        case ER_NODE_ACTIVITY_INDICATOR:
        case ER_NODE_SWITCH:
        case ER_NODE_VECTOR:
            return false;
        default:
            return true;
    }
}

static bool check_templates(ErbProgram* program, ErbError* error)
{
    if (!split_table(program,
                     ERB_TABLE_TEMPLATES,
                     ERB_TEMPLATE_SIZE,
                     ERB_NODE_SIZE,
                     &program->templates,
                     &program->template_count,
                     &program->nodes,
                     &program->node_record_count))
    {
        return fail(error, ERB_ERROR_TABLE, "templates table malformed");
    }
    for (uint32_t index = 0; index < program->template_count; index++)
    {
        const uint8_t* template_record = erb_template(program, index);
        const uint16_t first = erb_u16(template_record + ERB_TEMPLATE_FIRST_NODE_AT);
        const uint16_t count = erb_u16(template_record + ERB_TEMPLATE_NODE_COUNT_AT);
        const uint16_t kind = erb_u16(template_record + ERB_TEMPLATE_KIND_AT);
        if (count == 0u || (uint32_t)first + count > program->node_record_count || kind < ERB_TEMPLATE_KIND_ROOT
            || kind > ERB_TEMPLATE_KIND_ROW || count > program->node_count)
        {
            return fail(error, ERB_ERROR_TEMPLATE, "template range, kind or node count");
        }
        /* Pre-order with child counts: a stack of children still owed must empty exactly at the last node. */
        uint16_t owed[ERB_MAX_TEMPLATE_DEPTH];
        uint32_t depth = 0;
        for (uint32_t node_index = 0; node_index < count; node_index++)
        {
            const uint8_t* node = program->nodes + (first + node_index) * ERB_NODE_SIZE;
            const uint8_t type = node[ERB_NODE_TYPE_AT];
            const uint16_t children = erb_u16(node + ERB_NODE_CHILDREN_AT);
            const uint16_t props = erb_u16(node + ERB_NODE_PROPS_AT);
            const uint16_t handle = erb_u16(node + ERB_NODE_HANDLE_AT);
            const uint16_t bindings = erb_u16(node + ERB_NODE_BINDINGS_AT);
            const uint8_t binding_count = node[ERB_NODE_BINDING_COUNT_AT];
            if (node_index > 0u)
            {
                if (depth == 0u)
                {
                    return fail(error, ERB_ERROR_TEMPLATE, "nodes after the tree ends");
                }
                owed[depth - 1u]--; /* never 0 here: an emptied entry is popped below */
            }
            if (type > ER_NODE_ARC || node[ERB_NODE_FLAGS_AT] != 0u || (children && !takes_children(type)))
            {
                return fail(error, ERB_ERROR_TEMPLATE, "node type, flags or children");
            }
            if (props != ERB_NONE && props >= program->prop_set_count)
            {
                return fail(error, ERB_ERROR_TEMPLATE, "node prop set");
            }
            if (handle != ERB_NONE)
            {
                const uint8_t* slot = handle < program->slot_count ? erb_slot(program, handle) : NULL;
                const bool array = slot && erb_u16(slot + ERB_SLOT_CAP_AT) != 0u;
                if (!slot || slot[ERB_SLOT_TYPE_AT] != ERB_TYPE_NODE || array != (kind == ERB_TEMPLATE_KIND_ROW))
                {
                    return fail(error, ERB_ERROR_TEMPLATE, "node handle slot");
                }
            }
            if (binding_count && (uint32_t)bindings + binding_count > program->binding_count)
            {
                return fail(error, ERB_ERROR_BINDING, "node bindings out of range");
            }
            for (uint32_t binding = 0; binding < binding_count; binding++)
            {
                if (program->bindings[(bindings + binding) * ERB_BINDING_SIZE] == ERB_BIND_TAPE
                    && type != ER_NODE_VECTOR)
                {
                    return fail(error, ERB_ERROR_BINDING, "tape on a node that isn't a vector");
                }
            }
            if (children)
            {
                if (depth == ERB_MAX_TEMPLATE_DEPTH)
                {
                    return fail(error, ERB_ERROR_TEMPLATE, "template nests past ERB_MAX_TEMPLATE_DEPTH");
                }
                owed[depth++] = children;
            }
            while (depth && owed[depth - 1u] == 0u)
            {
                depth--;
            }
        }
        if (depth != 0u)
        {
            return fail(error, ERB_ERROR_TEMPLATE, "child counts don't make one tree");
        }
    }
    if (program->root_template >= program->template_count
        || erb_u16(erb_template(program, program->root_template) + ERB_TEMPLATE_KIND_AT) != ERB_TEMPLATE_KIND_ROOT)
    {
        return fail(error, ERB_ERROR_TEMPLATE, "panel root template");
    }
    return true;
}

static bool check_anims(ErbProgram* program, ErbError* error)
{
    const uint32_t length = program->table_length[ERB_TABLE_ANIMATIONS];
    if (length % ERB_ANIMATION_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "anims table not whole records");
    }
    program->anims = program->table[ERB_TABLE_ANIMATIONS];
    program->anim_count = length / ERB_ANIMATION_SIZE;
    if (program->anim_count > ERB_NONE)
    {
        return fail(error, ERB_ERROR_ANIMATION, "too many animations");
    }
    ERPoolLimits limits;
    er_pool_limits(&limits);
    uint32_t children_end = 0;
    for (uint32_t index = 0; index < program->anim_count; index++)
    {
        const uint8_t* anim = erb_anim(program, index);
        const uint8_t kind = anim[ERB_ANIMATION_KIND_AT];
        const uint8_t easing = anim[ERB_ANIMATION_EASING_AT];
        const uint8_t flags = anim[ERB_ANIMATION_FLAGS_AT];
        const uint16_t target = erb_u16(anim + ERB_ANIMATION_TARGET_AT);
        const uint16_t prop = erb_u16(anim + ERB_ANIMATION_PROP_AT);
        const uint16_t first = erb_u16(anim + ERB_ANIMATION_FIRST_AT);
        const uint16_t count = erb_u16(anim + ERB_ANIMATION_COUNT_AT);
        if (kind < ERB_ANIMATION_TIMING || kind > ERB_ANIMATION_LOOP || easing > ER_EASE_BEZIER
            || (flags & ~ERB_ANIMATION_FLAGS_KNOWN))
        {
            return fail(error, ERB_ERROR_ANIMATION, "kind, easing or flags");
        }
        if (kind <= ERB_ANIMATION_DECAY)
        {
            const uint8_t* slot = target < program->slot_count ? erb_slot(program, target) : NULL;
            const ErbPropInfo* info = erb_prop_info(prop);
            const bool on_node = slot && slot[ERB_SLOT_TYPE_AT] == ERB_TYPE_NODE
                                 && erb_u16(slot + ERB_SLOT_CAP_AT) == 0u && info
                                 && info->animation_prop != ERB_NO_ANIMATION;
            const bool on_value = slot && slot[ERB_SLOT_TYPE_AT] == ERB_TYPE_ANIMATED_VALUE;
            const bool delay = target == ERB_NONE && kind == ERB_ANIMATION_TIMING;
            if (count != 0u || !(on_node || on_value || delay))
            {
                return fail(error, ERB_ERROR_ANIMATION, "leaf target or prop");
            }
            continue;
        }
        /* Groups list their children after themselves, and in ascending, disjoint ranges, so a child can't
         * have two parents and the descriptors form trees. */
        if (count == 0u || count > limits.group_entries || (kind == ERB_ANIMATION_LOOP && count != 1u) || first <= index
            || first < children_end || (uint32_t)first + count > program->anim_count)
        {
            return fail(error, ERB_ERROR_ANIMATION, "group children");
        }
        children_end = (uint32_t)first + count;
    }
    return true;
}

static bool check_tapes(ErbProgram* program, ErbError* error)
{
    const uint8_t* table = program->table[ERB_TABLE_TAPES];
    const uint32_t length = program->table_length[ERB_TABLE_TAPES];
    if (!table)
    {
        return true;
    }
    if (length < ERB_TAPE_HEADER_SIZE)
    {
        return fail(error, ERB_ERROR_TAPE, "tapes header");
    }
    const uint32_t count = erb_u32(table + ERB_TAPES_COUNT_AT);
    const uint32_t paints = erb_u32(table + ERB_TAPES_PAINTS_AT);
    const uint32_t gradients = erb_u32(table + ERB_TAPES_GRADIENTS_AT);
    const uint32_t stops = erb_u32(table + ERB_TAPES_STOPS_AT);
    const uint32_t floats_start = ERB_TAPE_HEADER_SIZE + count * ERB_TAPE_SIZE;
    if (count > (length - ERB_TAPE_HEADER_SIZE) / ERB_TAPE_SIZE || paints < floats_start || gradients < paints
        || stops < gradients || stops > length || !erb_aligned4(paints) || (gradients - paints) % ERB_PAINT_SIZE
        || (stops - gradients) % ERB_GRADIENT_SIZE || (length - stops) % ERB_STOP_SIZE)
    {
        return fail(error, ERB_ERROR_TAPE, "tapes parts out of order or not whole records");
    }
    program->tapes = table;
    program->tape_count = count;
    program->paints = table + paints;
    program->paint_count = (gradients - paints) / ERB_PAINT_SIZE;
    program->gradients = table + gradients;
    program->gradient_count = (stops - gradients) / ERB_GRADIENT_SIZE;
    program->stops = table + stops;
    program->stop_count = (length - stops) / ERB_STOP_SIZE;

    for (uint32_t index = 0; index < count; index++)
    {
        const uint8_t* tape = erb_tape(program, index);
        const uint32_t floats = erb_u32(tape + ERB_TAPE_FLOATS_AT);
        const uint16_t float_count = erb_u16(tape + ERB_TAPE_FLOAT_COUNT_AT);
        const uint16_t paint_first = erb_u16(tape + ERB_TAPE_PAINT_FIRST_AT);
        const uint16_t paint_count = erb_u16(tape + ERB_TAPE_PAINT_COUNT_AT);
        const uint16_t gradient_first = erb_u16(tape + ERB_TAPE_GRADIENT_FIRST_AT);
        const uint16_t gradient_count = erb_u16(tape + ERB_TAPE_GRADIENT_COUNT_AT);
        if (!erb_aligned4(floats) || floats < floats_start || !in_bounds(floats, float_count * sizeof(float), paints)
            || float_count > ERUI_VECTOR_TAPE_MAX || paint_count > ERUI_VECTOR_PAINTS_MAX
            || gradient_count > ERUI_VECTOR_GRADIENTS_MAX || (uint32_t)paint_first + paint_count > program->paint_count
            || (uint32_t)gradient_first + gradient_count > program->gradient_count)
        {
            return fail(error, ERB_ERROR_TAPE, "tape floats, paints or gradients out of range");
        }
        /* The ops parse with their argument counts. */
        static const uint8_t operand_counts[] = {1u, 2u, 2u, 4u, 6u, 6u, 0u};
        uint32_t float_index = 0;
        while (float_index < float_count)
        {
            const float opcode_value = erb_f32(table + floats + float_index * sizeof(float));
            if (!(opcode_value >= ER_VECTOR_OP_SHAPE && opcode_value <= ER_VECTOR_OP_CLOSE)
                || (float)(int)opcode_value != opcode_value
                || float_index + 1u + operand_counts[(int)opcode_value] > float_count)
            {
                return fail(error, ERB_ERROR_TAPE, "op-tape doesn't parse");
            }
            const int opcode = (int)opcode_value;
            if (opcode == 0)
            {
                const float paint = erb_f32(table + floats + (float_index + 1u) * sizeof(float));
                if (!(paint >= 0.0f && paint < (float)paint_count) || (float)(int)paint != paint)
                {
                    return fail(error, ERB_ERROR_TAPE, "shape names no paint");
                }
            }
            float_index += 1u + operand_counts[opcode];
        }
        for (uint32_t paint_index = 0; paint_index < paint_count; paint_index++)
        {
            const uint8_t* paint = program->paints + (paint_first + paint_index) * ERB_PAINT_SIZE;
            const int16_t fill_gradient = (int16_t)erb_u16(paint + ERB_PAINT_FILL_GRADIENT_AT);
            const int16_t stroke_gradient = (int16_t)erb_u16(paint + ERB_PAINT_STROKE_GRADIENT_AT);
            if (paint[ERB_PAINT_CAP_AT] > ER_VECTOR_CAP_SQUARE || paint[ERB_PAINT_JOIN_AT] > ER_VECTOR_JOIN_BEVEL
                || paint[ERB_PAINT_FILL_RULE_AT] > ER_VECTOR_FILL_EVENODD || fill_gradient < 0
                || fill_gradient > gradient_count || stroke_gradient < 0 || stroke_gradient > gradient_count)
            {
                return fail(error, ERB_ERROR_TAPE, "paint");
            }
        }
        for (uint32_t grad_index = 0; grad_index < gradient_count; grad_index++)
        {
            const uint8_t* gradient = program->gradients + (gradient_first + grad_index) * ERB_GRADIENT_SIZE;
            const uint8_t stop_count = gradient[ERB_GRADIENT_STOP_COUNT_AT];
            if (gradient[ERB_GRADIENT_TYPE_AT] < ER_GRADIENT_LINEAR
                || gradient[ERB_GRADIENT_TYPE_AT] > ER_GRADIENT_CONIC || stop_count < 2u
                || stop_count > ER_VECTOR_GRADIENT_MAX_STOPS
                || (uint32_t)erb_u16(gradient + ERB_GRADIENT_STOP_FIRST_AT) + stop_count > program->stop_count)
            {
                return fail(error, ERB_ERROR_TAPE, "gradient");
            }
        }
        if (float_count > program->largest_tape)
        {
            program->largest_tape = float_count;
        }
    }
    return true;
}

static bool check_imports(const ErbConfig* config, ErbProgram* program, ErbError* error)
{
    program->imports = program->table[ERB_TABLE_IMPORTS];
    program->import_count = program->table_length[ERB_TABLE_IMPORTS] / ERB_IMPORT_SIZE;
    if (program->table_length[ERB_TABLE_IMPORTS] % ERB_IMPORT_SIZE || program->import_count > ERB_MAX_IMPORTS)
    {
        return fail(error, ERB_ERROR_TABLE, "imports table");
    }
    for (uint32_t index = 0; index < program->import_count; index++)
    {
        const uint8_t* import = program->imports + index * ERB_IMPORT_SIZE;
        const uint16_t name = erb_u16(import + ERB_IMPORT_NAME_AT);
        const uint16_t version = erb_u16(import + ERB_IMPORT_VERSION_AT);
        const uint16_t signature = erb_u16(import + ERB_IMPORT_SIGNATURE_AT);
        if (name >= program->string_count || signature >= program->string_count)
        {
            return fail(error, ERB_ERROR_TABLE, "import names no string");
        }
        uint32_t native = 0;
        for (; native < config->native_count; native++)
        {
            const ErbNative* candidate = &config->natives[native];
            if (candidate->callback && candidate->name && candidate->signature && candidate->version == version
                && strcmp(candidate->name, erb_string(program, name)) == 0
                && strcmp(candidate->signature, erb_string(program, signature)) == 0)
            {
                break;
            }
        }
        if (native == config->native_count)
        {
            error->name = erb_string(program, name);
            return fail(error, ERB_ERROR_IMPORT, "no native function with this name, version and signature");
        }
        program->natives[index] = (uint16_t)native;
    }
    return true;
}

static bool check_inputs(ErbProgram* program, ErbError* error)
{
    program->inputs = program->table[ERB_TABLE_INPUTS];
    program->input_count = program->table_length[ERB_TABLE_INPUTS] / ERB_INPUT_SIZE;
    const char* previous = NULL;
    for (uint32_t index = 0; index < program->input_count; index++)
    {
        const uint8_t* input = program->inputs + index * ERB_INPUT_SIZE;
        const uint16_t name = erb_u16(input + ERB_INPUT_NAME_AT);
        const uint16_t slot = erb_u16(input + ERB_INPUT_SLOT_AT);
        if (name >= program->string_count || slot >= program->slot_count)
        {
            return fail(error, ERB_ERROR_INPUT, "input name or slot out of range");
        }
        const uint8_t type = erb_slot(program, slot)[ERB_SLOT_TYPE_AT];
        if (type != ERB_TYPE_INT && type != ERB_TYPE_BOOL && type != ERB_TYPE_FLOAT && type != ERB_TYPE_STRING)
        {
            return fail(error, ERB_ERROR_INPUT, "input slot of a type the host can't set");
        }
        /* Sorted by name, so names are unique and erb_input_find can search. */
        const char* current = erb_string(program, name);
        if (previous && strcmp(previous, current) >= 0)
        {
            return fail(error, ERB_ERROR_INPUT, "input names not unique and sorted");
        }
        previous = current;
    }
    return true;
}

/** @brief Whether a keyboard group array (layers or rows) names consecutive runs of the next level, in order. */
static bool keyboard_groups(const uint8_t* groups, uint32_t count, uint32_t next_count, uint32_t max_each)
{
    uint32_t next = 0;
    for (uint32_t index = 0; index < count; index++)
    {
        const uint8_t* group = groups + index * ERB_KEYBOARD_GROUP_SIZE;
        const uint16_t size = erb_u16(group + ERB_KEYBOARD_COUNT_AT);
        if (erb_u16(group + ERB_KEYBOARD_FIRST_AT) != next || size == 0u || size > max_each)
        {
            return false;
        }
        next += size;
    }
    return next == next_count;
}

static bool check_keyboard(ErbProgram* program, ErbError* error)
{
    const uint8_t* table = program->table[ERB_TABLE_KEYBOARD];
    const uint32_t length = program->table_length[ERB_TABLE_KEYBOARD];
    if (!table)
    {
        return true;
    }
    if (length < ERB_KEYBOARD_HEADER_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "keyboard header");
    }
    const uint16_t layers = erb_u16(table + ERB_KEYBOARD_LAYER_COUNT_AT);
    const uint16_t rows = erb_u16(table + ERB_KEYBOARD_ROW_COUNT_AT);
    const uint16_t keys = erb_u16(table + ERB_KEYBOARD_KEY_COUNT_AT);
    if (length
        != ERB_KEYBOARD_HEADER_SIZE + ((uint32_t)layers + rows) * ERB_KEYBOARD_GROUP_SIZE
               + (uint32_t)keys * ERB_KEYBOARD_KEY_SIZE)
    {
        return fail(error, ERB_ERROR_TABLE, "keyboard table not whole records");
    }
    if (layers > ERB_MAX_KEYBOARD_LAYERS || rows > ERB_MAX_KEYBOARD_ROWS || keys > ERB_MAX_KEYBOARD_KEYS)
    {
        return fail(error, ERB_ERROR_TOO_BIG, "keyboard layout larger than the firmware holds");
    }
    /* The engine counts a layer's rows and a row's keys in a byte. */
    const uint8_t* layer_groups = table + ERB_KEYBOARD_HEADER_SIZE;
    const uint8_t* row_groups = layer_groups + layers * ERB_KEYBOARD_GROUP_SIZE;
    if (!keyboard_groups(layer_groups, layers, rows, UINT8_MAX) || !keyboard_groups(row_groups, rows, keys, UINT8_MAX))
    {
        return fail(error, ERB_ERROR_TABLE, "keyboard layers or rows");
    }
    const uint8_t* key_records = row_groups + rows * ERB_KEYBOARD_GROUP_SIZE;
    for (uint32_t index = 0; index < keys; index++)
    {
        const uint8_t* key = key_records + index * ERB_KEYBOARD_KEY_SIZE;
        const uint16_t label = erb_u16(key + ERB_KEY_LABEL_AT);
        const uint16_t text = erb_u16(key + ERB_KEY_TEXT_AT);
        const uint8_t type = key[ERB_KEY_TYPE_AT];
        if ((label != ERB_NONE && label >= program->string_count) || (text != ERB_NONE && text >= program->string_count)
            || type > ER_KEYBOARD_KEY_LAYER || (type == ER_KEYBOARD_KEY_CHAR && text == ERB_NONE)
            || (type == ER_KEYBOARD_KEY_LAYER && key[ERB_KEY_LAYER_AT] >= layers))
        {
            return fail(error, ERB_ERROR_TABLE, "keyboard key");
        }
    }
    program->keyboard = table;
    return true;
}

static bool check_assets(const ErbConfig* config, ErbProgram* program, ErbError* error)
{
    if (!program->assets)
    {
        return true;
    }
    ErAssetsInfo info;
    if (!er_assets_check_pack_images(program->assets, program->assets_length, &info, s_image_names, ERB_MAX_IMAGES))
    {
        return fail(error, ERB_ERROR_ASSETS, "asset pack malformed");
    }
    /* The engine reads pixels in place as words, which faults on a core with no unaligned loads. */
    if (info.misaligned)
    {
        return fail(error, ERB_ERROR_ASSETS, "image pixels not word-aligned (a v1 pack)");
    }
    if (info.empty_images)
    {
        return fail(error, ERB_ERROR_ASSETS, "an image with no width or height");
    }
    ERPoolLimits limits;
    er_pool_limits(&limits);
    if (info.images > ERB_MAX_IMAGES)
    {
        return fail(error, ERB_ERROR_ASSETS, "more images than the image registry holds");
    }
    s_image_count = info.images;
    const ErAssetsStorage* fonts = config->fonts;
    if (info.images > limits.images || info.fonts > (fonts ? storage_room(fonts->fonts, fonts->font_count) : 0u)
        || info.glyphs > (fonts ? storage_room(fonts->glyphs, fonts->glyph_count) : 0u)
        || info.extra_glyphs > (fonts ? storage_room(fonts->extras, fonts->extra_count) : 0u))
    {
        return fail(
            error, ERB_ERROR_ASSETS, "asset pack doesn't fit the image registry or the firmware's font storage");
    }
    if (info.font_families > limits.font_families || info.largest_family > limits.font_sizes)
    {
        return fail(error, ERB_ERROR_ASSETS, "asset pack has more font families or sizes than the font registry holds");
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Code
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The context one function is checked in. */
typedef struct
{
    const ErbProgram* program;
    uint32_t index;
    const uint8_t* types;
    uint8_t registers;
    uint8_t kind;
    uint8_t result;
    uint32_t words;
    const uint32_t* code;
} FunctionContext;

static bool reg_is(const FunctionContext* context, uint32_t register_index, uint8_t type)
{
    return register_index < context->registers && context->types[register_index] == type;
}

static bool reg_any(const FunctionContext* context, uint32_t register_index)
{
    return register_index < context->registers && context->types[register_index] != ERB_TYPE_PAIR;
}

/** @brief A slot of this type, or NULL. */
static const uint8_t* slot_of(const FunctionContext* context, uint32_t index, uint8_t type)
{
    if (index >= context->program->slot_count)
    {
        return NULL;
    }
    const uint8_t* slot = erb_slot(context->program, index);
    return slot[ERB_SLOT_TYPE_AT] == type ? slot : NULL;
}

/** @brief A slot code may write: not boot-only, unless this is the boot function. */
static bool writable(const FunctionContext* context, const uint8_t* slot)
{
    return slot && (!(slot[ERB_SLOT_FLAGS_AT] & ERB_SLOT_BOOT_ONLY) || context->kind == ERB_FUNCTION_KIND_BOOT);
}

/** @brief The type EVT gives for a field, or 0 when this kind of function isn't entered with it. */
static uint8_t event_field_type(uint8_t kind, uint8_t field)
{
    const bool touch = (kind >= ERB_FUNCTION_KIND_QUERY && kind < ERB_FUNCTION_KIND_QUERY + ER_RESPONDER_QUERY_COUNT)
                       || (kind <= ER_EVENT_RESPONDER_TERMINATE && kind != ER_EVENT_SCROLL);
    if (touch && (field <= ERB_EVENT_FIELD_DY || field == ERB_EVENT_FIELD_TOUCHES))
    {
        return ERB_TYPE_INT;
    }
    if (touch && (field == ERB_EVENT_FIELD_VX || field == ERB_EVENT_FIELD_VY))
    {
        return ERB_TYPE_FLOAT;
    }
    if (kind == ER_EVENT_SCROLL && (field == ERB_EVENT_FIELD_SCROLL_X || field == ERB_EVENT_FIELD_SCROLL_Y))
    {
        return ERB_TYPE_FLOAT;
    }
    if (kind == ER_EVENT_LAYOUT && field >= ERB_EVENT_FIELD_LAYOUT_X && field <= ERB_EVENT_FIELD_LAYOUT_HEIGHT)
    {
        return ERB_TYPE_INT;
    }
    if (kind == ER_EVENT_VALUE_CHANGE && (field == ERB_EVENT_FIELD_VALUE || field == ERB_EVENT_FIELD_VALUE_START))
    {
        return ERB_TYPE_FLOAT;
    }
    if (kind == ERB_FUNCTION_KIND_ANIMATION_DONE && field == ERB_EVENT_FIELD_FINISHED)
    {
        return ERB_TYPE_BOOL;
    }
    return 0u;
}

/** @brief Checks a prop a PSET writes, against the value kinds that opcode can convert. */
static bool pset_ok(uint8_t opcode, uint16_t prop_id)
{
    const ErbPropInfo* info = erb_prop_info(prop_id);
    if (!info)
    {
        return false;
    }
    switch (opcode)
    {
        case ERB_OP_PSETI:
        case ERB_OP_PSETE:
            return info->kind != ERB_PROP_KIND_STRING && info->kind != ERB_PROP_KIND_IMAGE;
        case ERB_OP_PSETF:
            return info->kind == ERB_PROP_KIND_FLOAT || info->kind == ERB_PROP_KIND_I16
                   || info->kind == ERB_PROP_KIND_OPACITY || info->kind == ERB_PROP_KIND_U8
                   || info->kind == ERB_PROP_KIND_U16;
        case ERB_OP_PSETB:
            return info->kind == ERB_PROP_KIND_BOOL || info->kind == ERB_PROP_KIND_U8;
        default:
            return info->kind == ERB_PROP_KIND_STRING || info->kind == ERB_PROP_KIND_IMAGE;
    }
}

/** @brief Checks the registers from @p first against a native signature ("r:ppp"). */
static bool signature_ok(const FunctionContext* context, const char* signature, uint32_t first)
{
    if (!signature[0] || signature[1] != ':')
    {
        return false;
    }
    uint32_t register_index = first;
    for (const char* param = signature + 2; *param; param++, register_index++)
    {
        const uint8_t type = *param == 'i'   ? ERB_TYPE_INT
                             : *param == 'b' ? ERB_TYPE_BOOL
                             : *param == 'f' ? ERB_TYPE_FLOAT
                                             : 0u;
        if (!type || !reg_is(context, register_index, type))
        {
            return false;
        }
    }
    switch (signature[0])
    {
        case 'v':
            return true;
        case 'i':
            return reg_is(context, first, ERB_TYPE_INT);
        case 'b':
            return reg_is(context, first, ERB_TYPE_BOOL);
        case 'f':
            return reg_is(context, first, ERB_TYPE_FLOAT);
        default:
            return false;
    }
}

/**
 * @brief Walks an animation tree from @p root the way ASTART starts it, into s_tree.
 *
 * @param[out] size       Descriptors in the tree.
 * @param[out] to_values  Leaves that take their to value from a register.
 *
 * @return false when the tree has more than ERB_MAX_ANIM_TREE descriptors.
 */
static bool walk_anim_tree(const ErbProgram* program, uint16_t root, uint32_t* size, uint32_t* to_values)
{
    uint32_t count = 1;
    s_tree[0] = root;
    *to_values = 0;
    for (uint32_t index = 0; index < count; index++)
    {
        const uint8_t* anim = erb_anim(program, s_tree[index]);
        if (anim[ERB_ANIMATION_KIND_AT] <= ERB_ANIMATION_DECAY)
        {
            *to_values += (anim[ERB_ANIMATION_FLAGS_AT] & ERB_ANIMATION_FLAG_TO_REGISTER) ? 1u : 0u;
            continue;
        }
        const uint16_t first = erb_u16(anim + ERB_ANIMATION_FIRST_AT);
        const uint16_t children = erb_u16(anim + ERB_ANIMATION_COUNT_AT);
        if (count + children > ERB_MAX_ANIM_TREE)
        {
            return false;
        }
        for (uint16_t child = 0; child < children; child++)
        {
            s_tree[count++] = (uint16_t)(first + child);
        }
    }
    *size = count;
    return true;
}

/** @brief Checks the operands one instruction names beyond their register types. */
static bool check_operands(const FunctionContext* context, uint32_t program_counter, uint32_t word, ErbError* error)
{
    const ErbProgram* program = context->program;
    const uint8_t opcode = ERB_OP(word);
    const uint8_t operand_a = ERB_A(word);
    const uint8_t operand_b = ERB_B(word);
    const uint8_t operand_c = ERB_C(word);
    const uint16_t operand_bx = ERB_BX(word);
    const uint32_t extension = s_opcodes[opcode].words == 2u ? context->code[program_counter + 1u] : 0u;
    const uint16_t extension_index = ERB_LOW16(extension);
    const uint8_t* slot;

#define BAD(text) return fail(error, ERB_ERROR_BYTECODE, text)
    switch (opcode)
    {
        case ERB_OP_MOVE:
            if (context->types[operand_a] != context->types[operand_b])
                BAD("MOVE between types");
            break;
        case ERB_OP_LOADI:
            if (!(context->types[operand_a] == ERB_TYPE_INT
                  || (context->types[operand_a] == ERB_TYPE_BOOL && operand_bx <= 1u)))
                BAD("LOADI register or value");
            break;
        case ERB_OP_SCREEN:
            if (operand_bx > 1u)
                BAD("SCREEN dimension");
            break;
        case ERB_OP_LOADK:
            if ((context->types[operand_a] != ERB_TYPE_INT && context->types[operand_a] != ERB_TYPE_FLOAT)
                || operand_bx >= program->const32_count)
                BAD("LOADK register or constant");
            break;
        case ERB_OP_LOADK64:
            if ((context->types[operand_a] != ERB_TYPE_INT64 && context->types[operand_a] != ERB_TYPE_DOUBLE)
                || operand_bx >= program->const64_count)
                BAD("LOADK64 register or constant");
            break;
        case ERB_OP_LOADS:
        case ERB_OP_FMTK:
            if (operand_bx >= program->string_count)
                BAD("string index");
            break;
        case ERB_OP_LOADE:
            if (ERB_HIGH16(extension) || extension_index >= program->enum_set_count
                || operand_bx >= erb_enum_count(program, extension_index))
                BAD("LOADE set or value");
            break;
        case ERB_OP_GET32:
        case ERB_OP_SET32:
        {
            slot = operand_bx < program->slot_count ? erb_slot(program, operand_bx) : NULL;
            const uint8_t type = slot ? slot[ERB_SLOT_TYPE_AT] : 0u;
            const bool narrow = type == ERB_TYPE_INT || type == ERB_TYPE_BOOL || type == ERB_TYPE_FLOAT
                                || type == ERB_TYPE_ENUM || type == ERB_TYPE_ANIMATION;
            if (!narrow || context->types[operand_a] != type || (opcode == ERB_OP_SET32 && !writable(context, slot)))
                BAD("32-bit slot type or write");
            break;
        }
        case ERB_OP_GET64:
        case ERB_OP_SET64:
        {
            slot = operand_bx < program->slot_count ? erb_slot(program, operand_bx) : NULL;
            const uint8_t type = slot ? slot[ERB_SLOT_TYPE_AT] : 0u;
            if (!is_wide(type) || erb_u16(slot + ERB_SLOT_CAP_AT) != 0u || context->types[operand_a] != type
                || (opcode == ERB_OP_SET64 && !writable(context, slot)))
                BAD("64-bit slot type or write");
            break;
        }
        case ERB_OP_GETS:
        case ERB_OP_SETS:
        case ERB_OP_FMTSET:
        case ERB_OP_INPUT:
            slot = slot_of(context, operand_bx, ERB_TYPE_STRING);
            if (!slot || (opcode != ERB_OP_GETS && !writable(context, slot)))
                BAD("string slot");
            break;
        case ERB_OP_GETNI:
            slot = slot_of(context, extension_index, ERB_TYPE_NODE);
            if (ERB_HIGH16(extension) || !slot || erb_u16(slot + ERB_SLOT_CAP_AT) == 0u)
                BAD("GETNI handle array");
            break;
        case ERB_OP_LEN:
        case ERB_OP_CLEAR:
        case ERB_OP_POP:
        case ERB_OP_KEEP:
            if (!slot_of(context, operand_bx, ERB_TYPE_LIST)
                || (opcode != ERB_OP_LEN && !writable(context, slot_of(context, operand_bx, ERB_TYPE_LIST))))
                BAD("list slot");
            break;
        case ERB_OP_SLICE:
            if (ERB_HIGH16(extension) || !writable(context, slot_of(context, extension_index, ERB_TYPE_LIST)))
                BAD("list slot");
            break;
        case ERB_OP_ROWGET:
        case ERB_OP_ROWSET:
        {
            slot = slot_of(context, extension_index, ERB_TYPE_LIST);
            const uint8_t field = ERB_EXTENSION_OPERAND(extension);
            if (!slot || ERB_EXTENSION_TOP(extension) || (opcode == ERB_OP_ROWSET && !writable(context, slot)))
                BAD("row field");
            const uint8_t* layout = erb_row(program, erb_u16(slot + ERB_SLOT_AUX_AT));
            if (field >= erb_u16(layout + ERB_ROW_FIELD_COUNT_AT)
                || program->fields[(erb_u16(layout + ERB_ROW_FIRST_AT) + field) * ERB_FIELD_SIZE + ERB_FIELD_TYPE_AT]
                       != context->types[operand_a])
                BAD("row field");
            break;
        }
        case ERB_OP_APPEND:
        {
            slot = slot_of(context, operand_bx, ERB_TYPE_LIST);
            if (!writable(context, slot))
                BAD("list slot");
            const uint8_t* layout = erb_row(program, erb_u16(slot + ERB_SLOT_AUX_AT));
            uint32_t register_index = operand_a;
            for (uint32_t field = 0; field < erb_u16(layout + ERB_ROW_FIELD_COUNT_AT); field++)
            {
                const uint8_t type =
                    program->fields[(erb_u16(layout + ERB_ROW_FIRST_AT) + field) * ERB_FIELD_SIZE + ERB_FIELD_TYPE_AT];
                if (!reg_is(context, register_index, type))
                    BAD("APPEND registers don't match the row");
                register_index += erb_type_cells(type);
            }
            break;
        }
        case ERB_OP_FMATH:
        case ERB_OP_DMATH:
            if (extension >= ERB_MATH_COUNT_)
                BAD("math function");
            break;
        case ERB_OP_EQ:
            if (context->types[operand_b] != context->types[operand_c]
                || !(context->types[operand_b] == ERB_TYPE_INT || context->types[operand_b] == ERB_TYPE_BOOL
                     || context->types[operand_b] == ERB_TYPE_ENUM))
                BAD("EQ operand types");
            break;
        case ERB_OP_JDIRTY:
            if (operand_a >= program->dirty_words)
                BAD("JDIRTY dirty word");
            break;
        case ERB_OP_RET:
            if (context->result && !reg_is(context, operand_a, context->result))
                BAD("RET register isn't the result type");
            break;
        case ERB_OP_CALL:
        {
            if (operand_bx >= program->function_count)
                BAD("CALL target");
            if (operand_bx <= context->index)
                return fail(error, ERB_ERROR_RECURSION, "CALL to a function that isn't after the caller");
            const uint8_t* callee = erb_function(program, operand_bx);
            if (callee[ERB_FUNCTION_KIND_AT] != ERB_FUNCTION_KIND_HELPER)
                BAD("CALL target isn't a helper");
            const uint8_t* callee_types = program->types + erb_u32(callee + ERB_FUNCTION_TYPES_AT);
            for (uint32_t param = 0; param < callee[ERB_FUNCTION_PARAMS_AT]; param++)
            {
                if (operand_a + param >= context->registers || context->types[operand_a + param] != callee_types[param])
                    BAD("CALL arguments don't match the parameters");
            }
            if (callee[ERB_FUNCTION_RESULT_AT] && !reg_is(context, operand_a, callee[ERB_FUNCTION_RESULT_AT]))
                BAD("CALL result register");
            break;
        }
        case ERB_OP_CALLN:
            if (operand_bx >= program->import_count
                || !signature_ok(
                    context,
                    erb_string(program,
                               erb_u16(program->imports + operand_bx * ERB_IMPORT_SIZE + ERB_IMPORT_SIGNATURE_AT)),
                    operand_a))
                BAD("CALLN import or registers");
            break;
        case ERB_OP_FMTE:
            if (operand_bx >= program->enum_set_count)
                BAD("FMTE set");
            break;
        case ERB_OP_FMTFX:
            if (operand_b > ERB_FIXED_DIGITS_MAX)
                BAD("toFixed digits");
            break;
        case ERB_OP_FMTPAD:
            if (operand_a > ER_TEXT_MAX || operand_b < (uint8_t)ERB_PAD_CHAR_FIRST
                || operand_b > (uint8_t)ERB_PAD_CHAR_LAST)
                BAD("padStart width or character");
            break;
        case ERB_OP_FMTSPAN:
        case ERB_OP_SPANS:
            if (operand_a > ER_TEXT_MAX_SPANS || (opcode == ERB_OP_FMTSPAN && operand_a == ER_TEXT_MAX_SPANS))
                BAD("span index");
            break;
        case ERB_OP_PROPS:
            if (operand_bx != ERB_NONE && operand_bx >= program->prop_set_count)
                BAD("prop set");
            break;
        case ERB_OP_PSETI:
        case ERB_OP_PSETF:
        case ERB_OP_PSETB:
        case ERB_OP_PSETS:
            if (!pset_ok(opcode, operand_bx))
                return fail(error, ERB_ERROR_PROP, "PSET prop unknown or of another kind");
            break;
        case ERB_OP_PSETE:
        {
            const uint16_t set = ERB_HIGH16(extension);
            if (!pset_ok(opcode, operand_bx))
                return fail(error, ERB_ERROR_PROP, "PSETE prop unknown or of another kind");
            if (set >= program->enum_set_count
                || (uint32_t)extension_index + erb_enum_count(program, set) > program->const32_count)
                BAD("PSETE lookup table");
            break;
        }
        case ERB_OP_BUILD:
        {
            const uint8_t index = ERB_EXTENSION_OPERAND(extension);
            if (ERB_EXTENSION_TOP(extension) || extension_index >= program->template_count)
                BAD("BUILD template");
            const uint16_t kind = erb_u16(erb_template(program, extension_index) + ERB_TEMPLATE_KIND_AT);
            if (kind == ERB_TEMPLATE_KIND_ROOT || (kind == ERB_TEMPLATE_KIND_ROW && index == ERB_NO_REGISTER)
                || (index != ERB_NO_REGISTER && !reg_is(context, index, ERB_TYPE_INT)))
                BAD("BUILD template kind or index register");
            break;
        }
        case ERB_OP_TSTART:
            if (!program->regions[ERB_REGION_TIMERS].present || operand_c > 1u || extension >= program->function_count
                || erb_function(program, extension)[ERB_FUNCTION_KIND_AT] != ERB_FUNCTION_KIND_TIMER)
                BAD("TSTART function, repeat or timer table");
            break;
        case ERB_OP_ASTART:
        {
            uint32_t size;
            uint32_t to_values;
            const uint16_t done = extension_index;
            const uint8_t to_first = ERB_EXTENSION_OPERAND(extension);
            if (ERB_EXTENSION_TOP(extension) || operand_bx >= program->anim_count
                || !walk_anim_tree(program, operand_bx, &size, &to_values)
                || !program->regions[ERB_REGION_ANIMATIONS].present
                || size > program->regions[ERB_REGION_ANIMATIONS].count)
                BAD("ASTART animation or entries");
            if (done != ERB_NONE
                && (done >= program->function_count
                    || erb_function(program, done)[ERB_FUNCTION_KIND_AT] != ERB_FUNCTION_KIND_ANIMATION_DONE))
                BAD("ASTART completion function");
            for (uint32_t value = 0; value < to_values; value++)
            {
                if (!reg_is(context, (uint32_t)to_first + value, ERB_TYPE_FLOAT))
                    BAD("ASTART to registers");
            }
            break;
        }
        case ERB_OP_AVSET:
        case ERB_OP_AVGET:
            if (!slot_of(context, operand_bx, ERB_TYPE_ANIMATED_VALUE))
                BAD("animated value slot");
            break;
        case ERB_OP_VTAPE:
        case ERB_OP_VCOMMIT:
        case ERB_OP_VCOMMITP:
            if (operand_bx >= program->tape_count || !program->regions[ERB_REGION_TAPE].present
                || erb_u16(erb_tape(program, operand_bx) + ERB_TAPE_FLOAT_COUNT_AT)
                       > program->regions[ERB_REGION_TAPE].count)
                BAD("tape, or a scratch too small for it");
            break;
        case ERB_OP_VSET:
            if (!program->regions[ERB_REGION_TAPE].present || operand_bx >= program->regions[ERB_REGION_TAPE].count)
                BAD("tape scratch index");
            break;
        case ERB_OP_VPAINT:
        {
            const uint8_t type = operand_c == ERB_VECTOR_PAINT_STROKE_WIDTH ? ERB_TYPE_FLOAT : ERB_TYPE_INT;
            if (operand_b >= ERUI_VECTOR_PAINTS_MAX || operand_c >= ERB_VECTOR_PAINT_COUNT_
                || !reg_is(context, operand_a, type))
                BAD("VPAINT paint, field or register");
            break;
        }
        case ERB_OP_VGRAD:
        {
            /* Geometry, then a color and a position for each stop. */
            const bool geometry = operand_c <= ERB_VECTOR_GRADIENT_RADIUS;
            const bool stop = operand_c >= ERB_VECTOR_GRADIENT_STOPS
                              && operand_c < ERB_VECTOR_GRADIENT_STOPS + 2u * ER_VECTOR_GRADIENT_MAX_STOPS;
            const uint8_t type =
                geometry || (stop && (operand_c - ERB_VECTOR_GRADIENT_STOPS) % 2u) ? ERB_TYPE_FLOAT : ERB_TYPE_INT;
            if (operand_b >= ERUI_VECTOR_GRADIENTS_MAX || !(geometry || stop) || !reg_is(context, operand_a, type))
                BAD("VGRAD gradient, field or register");
            break;
        }
        case ERB_OP_VDIRTY:
            for (uint32_t register_index = operand_b; register_index < (uint32_t)operand_b + 4u; register_index++)
            {
                if (!reg_is(context, register_index, ERB_TYPE_INT))
                    BAD("VDIRTY registers");
            }
            break;
        case ERB_OP_EVT:
        {
            const uint8_t type = operand_b < ERB_EVENT_FIELD_COUNT_ ? event_field_type(context->kind, operand_b) : 0u;
            if (!type || context->types[operand_a] != type)
                BAD("EVT field isn't one this function is entered with");
            break;
        }
        case ERB_OP_TRAP:
            if (operand_bx != ERB_NONE && operand_bx >= program->string_count)
                BAD("TRAP message");
            break;
        case ERB_OP_EFFECT:
            if (!program->regions[ERB_REGION_EVENTS].present || operand_bx >= program->function_count
                || erb_function(program, operand_bx)[ERB_FUNCTION_KIND_AT] != ERB_FUNCTION_KIND_EFFECT)
                BAD("EFFECT function, or no event queue to hold it");
            break;
        default:
            break;
    }
#undef BAD
    return true;
}

/** @brief Checks the register operands the opcode table types. */
static bool check_register_kinds(const FunctionContext* context, uint32_t word)
{
    const OpInfo* info = &s_opcodes[ERB_OP(word)];
    const uint8_t fields[3] = {ERB_A(word), ERB_B(word), ERB_C(word)};
    const uint8_t kinds[3] = {info->operand_a_kind,
                              info->operand_b_kind,
                              info->form == ERB_F_ABC ? info->operand_c_kind : (uint8_t)ERB_K_NONE};
    for (uint32_t index = 0; index < 3u; index++)
    {
        const uint8_t kind = kinds[index];
        if (kind >= ERB_TYPE_INT && kind < ERB_TYPE_REGISTER_COUNT_ && !reg_is(context, fields[index], kind))
        {
            return false;
        }
        /* RET, CALL, CALLN and APPEND check their own, since whether A is used depends on the callee or row. */
        const uint8_t opcode = ERB_OP(word);
        const bool own =
            opcode == ERB_OP_RET || opcode == ERB_OP_CALL || opcode == ERB_OP_CALLN || opcode == ERB_OP_APPEND;
        if (kind == ERB_K_REG && !own && !reg_any(context, fields[index]))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Whether an instruction branches, and its target if it does.
 *
 * A branch to -1 wraps to UINT32_MAX, so the target can't double as a "doesn't branch" sentinel.
 */
static bool branch_target(uint32_t program_counter, uint32_t word, uint32_t* target)
{
    switch (ERB_OP(word))
    {
        case ERB_OP_JMP:
        case ERB_OP_JT:
        case ERB_OP_JF:
        case ERB_OP_JDIRTY:
        case ERB_OP_LOOP:
            *target = (uint32_t)((int32_t)(program_counter + erb_op_words(ERB_OP(word))) + ERB_SBX(word));
            return true;
        default:
            return false;
    }
}

/** @brief Checks one function's code and works out its cost (its callees are already done). */
static bool check_function(const ErbProgram* program, uint32_t index, ErbError* error)
{
    const uint8_t* record = erb_function(program, index);
    FunctionContext context = {
        program,
        index,
        program->types + erb_u32(record + ERB_FUNCTION_TYPES_AT),
        record[ERB_FUNCTION_REGISTERS_AT],
        record[ERB_FUNCTION_KIND_AT],
        record[ERB_FUNCTION_RESULT_AT],
        erb_u32(record + ERB_FUNCTION_WORDS_AT),
        program->code + erb_u32(record + ERB_FUNCTION_CODE_AT),
    };
    error->function = (uint16_t)index;

    /* Pass 1: instruction starts, loops, and loop counters. */
    memset(s_starts, 0, ERB_BIT_WORDS(context.words) * sizeof(uint32_t));
    s_loop_count = 0;
    uint32_t counters[ERB_BIT_WORDS(ERB_MAX_REGISTERS)] = {0u};
    uint32_t last = 0;
    for (uint32_t program_counter = 0; program_counter < context.words;)
    {
        const uint32_t word = context.code[program_counter];
        const uint8_t opcode = ERB_OP(word);
        const uint32_t length = erb_op_words(opcode);
        error->offset = program_counter;
        if (length == 0u || program_counter + length > context.words)
        {
            return fail(error, ERB_ERROR_BYTECODE, "unknown opcode or a truncated instruction");
        }
        erb_bit_set(s_starts, program_counter);
        if (opcode == ERB_OP_LOOPSET)
        {
            erb_bit_set(counters, ERB_A(word));
        }
        last = program_counter;
        program_counter += length;
    }
    const uint8_t last_opcode = ERB_OP(context.code[last]);
    if (last_opcode != ERB_OP_RET && last_opcode != ERB_OP_TRAP)
    {
        error->offset = last;
        return fail(error, ERB_ERROR_FUNCTION, "doesn't end in RET or TRAP");
    }
    for (uint32_t program_counter = 0; program_counter < context.words;
         program_counter += erb_op_words(ERB_OP(context.code[program_counter])))
    {
        const uint32_t word = context.code[program_counter];
        if (ERB_OP(word) != ERB_OP_LOOP)
        {
            continue;
        }
        error->offset = program_counter;
        uint32_t target = 0;
        branch_target(program_counter, word, &target);
        const uint32_t loopset = target - 1u;
        if (target > program_counter || target == 0u || !is_start(loopset)
            || ERB_OP(context.code[loopset]) != ERB_OP_LOOPSET || ERB_A(context.code[loopset]) != ERB_A(word))
        {
            return fail(error, ERB_ERROR_CONTROL_FLOW, "LOOP doesn't branch to just after its LOOPSET");
        }
        if (s_loop_count == ERB_MAX_LOOPS)
        {
            return fail(error, ERB_ERROR_CONTROL_FLOW, "more loops than ERB_MAX_LOOPS");
        }
        const uint32_t bound = ERB_BX(context.code[loopset]);
        s_loops[s_loop_count].start = target;
        s_loops[s_loop_count].end = program_counter;
        s_loops[s_loop_count].bound = bound ? bound : 1u;
        s_loops[s_loop_count].counter = ERB_A(word);
        s_loop_count++;
    }
    /* Loops nest or are apart. */
    for (uint32_t first = 0; first < s_loop_count; first++)
    {
        for (uint32_t second = first + 1u; second < s_loop_count; second++)
        {
            const Loop* first_loop = &s_loops[first];
            const Loop* second_loop = &s_loops[second];
            const bool apart = first_loop->end < second_loop->start || second_loop->end < first_loop->start;
            const bool nested = (first_loop->start <= second_loop->start && second_loop->end <= first_loop->end)
                                || (second_loop->start <= first_loop->start && first_loop->end <= second_loop->end);
            if (!apart && !nested)
            {
                return fail(error, ERB_ERROR_CONTROL_FLOW, "loops overlap");
            }
        }
    }

    /* Pass 2: operands, branches and cost. */
    uint32_t cost = 0;
    uint32_t depth = 0;
    for (uint32_t program_counter = 0; program_counter < context.words;)
    {
        const uint32_t word = context.code[program_counter];
        const uint8_t opcode = ERB_OP(word);
        const OpInfo* info = &s_opcodes[opcode];
        error->offset = program_counter;
        if (!check_register_kinds(&context, word))
        {
            return fail(error, ERB_ERROR_BYTECODE, "register operand out of the frame or of the wrong type");
        }
        if (!check_operands(&context, program_counter, word, error))
        {
            return false;
        }
        /* A loop counter is written by its LOOPSET and LOOP only. */
        const bool writes_operand_a =
            info->writes || (opcode == ERB_OP_CALL && erb_function(program, ERB_BX(word))[ERB_FUNCTION_RESULT_AT])
            || (opcode == ERB_OP_CALLN
                && erb_string(program,
                              erb_u16(program->imports + ERB_BX(word) * ERB_IMPORT_SIZE + ERB_IMPORT_SIGNATURE_AT))[0]
                       != 'v');
        if (writes_operand_a && opcode != ERB_OP_LOOPSET && opcode != ERB_OP_LOOP && erb_bit_get(counters, ERB_A(word)))
        {
            return fail(error, ERB_ERROR_CONTROL_FLOW, "a loop counter is written outside its LOOPSET and LOOP");
        }
        if (opcode == ERB_OP_LOOP && !erb_bit_get(counters, ERB_A(word)))
        {
            return fail(error, ERB_ERROR_CONTROL_FLOW, "LOOP counter has no LOOPSET");
        }
        uint32_t target = 0;
        if (branch_target(program_counter, word, &target))
        {
            if (target >= context.words || !is_start(target) || (opcode != ERB_OP_LOOP && target <= program_counter))
            {
                return fail(error, ERB_ERROR_CONTROL_FLOW, "branch backward, past the end or into an instruction");
            }
            /* Nothing jumps into a loop from outside it. */
            for (uint32_t loop = 0; loop < s_loop_count; loop++)
            {
                const Loop* body = &s_loops[loop];
                if (target >= body->start && target <= body->end
                    && (program_counter < body->start || program_counter > body->end))
                {
                    return fail(error, ERB_ERROR_CONTROL_FLOW, "branch into a loop");
                }
            }
        }

        /* Cost: every instruction runs once per pass of each loop around it. */
        uint32_t times = 1;
        for (uint32_t loop = 0; loop < s_loop_count; loop++)
        {
            if (program_counter >= s_loops[loop].start && program_counter <= s_loops[loop].end)
            {
                times = sat_mul(times, s_loops[loop].bound);
            }
        }
        uint32_t step = 1;
        if (opcode == ERB_OP_CALL)
        {
            step = sat_add(step, s_costs[ERB_BX(word)].worst);
            if (s_costs[ERB_BX(word)].depth > depth)
            {
                depth = s_costs[ERB_BX(word)].depth;
            }
        }
        else if (opcode == ERB_OP_BUILD)
        {
            step = sat_add(step,
                           erb_u16(erb_template(program, ERB_LOW16(context.code[program_counter + 1u]))
                                   + ERB_TEMPLATE_NODE_COUNT_AT));
        }
        cost = sat_add(cost, sat_mul(step, times));
        program_counter += info->words;
    }
    s_costs[index].worst = cost;
    s_costs[index].depth = ERB_FRAME_CELLS + context.registers + depth;
    return true;
}

static bool check_code(const ErbConfig* config, ErbProgram* program, ErbError* error)
{
    /* Callees come after their callers, so checking from the last function back has every callee's cost ready. */
    for (uint32_t index = program->function_count; index-- > 0u;)
    {
        if (!check_function(program, index, error))
        {
            return false;
        }
    }
    error->function = ERB_NONE;
    error->offset = 0;

    const uint32_t budget = config->budget ? config->budget : ERB_EVENT_BUDGET;
    const uint32_t stack = program->regions[ERB_REGION_STACK].count;
    for (uint32_t index = 0; index < program->function_count; index++)
    {
        if (s_costs[index].depth > stack)
        {
            error->function = (uint16_t)index;
            error->needed = s_costs[index].depth;
            error->available = stack;
            return fail(error, ERB_ERROR_STACK, "deepest call path doesn't fit the register stack");
        }
        if (s_costs[index].worst > budget && is_entry_kind(erb_function(program, index)[ERB_FUNCTION_KIND_AT]))
        {
            erb_bit_set(program->counting, index);
        }
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Debug section
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Marks one (already verified) function's instruction starts in s_starts. */
static void mark_starts(const ErbProgram* program, uint32_t index)
{
    const uint8_t* record = erb_function(program, index);
    const uint32_t words = erb_u32(record + ERB_FUNCTION_WORDS_AT);
    const uint32_t* code = program->code + erb_u32(record + ERB_FUNCTION_CODE_AT);
    memset(s_starts, 0, ERB_BIT_WORDS(words) * sizeof(uint32_t));
    for (uint32_t program_counter = 0; program_counter < words;
         program_counter += erb_op_words(ERB_OP(code[program_counter])))
    {
        erb_bit_set(s_starts, program_counter);
    }
}

/** @brief Checks one debug table, whose header is @p header_size bytes and holds @p function_count. */
static bool check_debug_table(ErbProgram* program,
                              const uint8_t* table,
                              uint32_t length,
                              uint32_t header_size,
                              uint32_t function_count,
                              ErbError* error)
{
    if (erb_u32(table + ERB_DEBUG_BUILD_ID_AT) != program->build_id || function_count != program->function_count)
    {
        return fail(error, ERB_ERROR_DEBUG, "build id or function count");
    }
    const uint32_t entry_count = erb_u32(table + ERB_DEBUG_ENTRY_COUNT_AT);
    const uint32_t strings = erb_u32(table + ERB_DEBUG_STRINGS_AT);
    if (entry_count > (length - header_size) / ERB_DEBUG_ENTRY_SIZE)
    {
        return fail(error, ERB_ERROR_DEBUG, "entries past the section");
    }
    const uint32_t names = header_size + entry_count * ERB_DEBUG_ENTRY_SIZE;
    const uint32_t names_end = names + round4(program->function_count * sizeof(uint16_t));
    uint32_t string_count = 0;
    if (names_end > length || strings != names_end || !erb_aligned4(strings)
        || !check_strings(table + strings, length - strings, &string_count))
    {
        return fail(error, ERB_ERROR_DEBUG, "parts out of order, or a malformed string table");
    }
    for (uint32_t index = 0; index < program->function_count; index++)
    {
        if (erb_u16(table + names + index * sizeof(uint16_t)) >= string_count)
        {
            return fail(error, ERB_ERROR_DEBUG, "function name index");
        }
    }
    uint32_t marked = UINT32_MAX;
    uint64_t previous = 0;
    for (uint32_t index = 0; index < entry_count; index++)
    {
        const uint8_t* entry = table + header_size + index * ERB_DEBUG_ENTRY_SIZE;
        const uint16_t function = erb_u16(entry + ERB_DEBUG_ENTRY_FUNCTION_AT);
        const uint32_t offset = erb_u32(entry + ERB_DEBUG_ENTRY_OFFSET_AT);
        const uint64_t key = ((uint64_t)function << 32) | offset;
        if (function >= program->function_count || (index && key <= previous)
            || erb_u16(entry + ERB_DEBUG_ENTRY_FILE_AT) >= string_count)
        {
            return fail(error, ERB_ERROR_DEBUG, "entry function, order or file");
        }
        previous = key;
        if (function != marked)
        {
            mark_starts(program, function);
            marked = function;
        }
        if (offset >= erb_u32(erb_function(program, function) + ERB_FUNCTION_WORDS_AT) || !is_start(offset))
        {
            return fail(error, ERB_ERROR_DEBUG, "entry not on an instruction");
        }
    }
    program->debug_entries = table + header_size;
    program->debug_entry_count = entry_count;
    program->debug_strings = table + strings;
    return true;
}

static bool check_debug(ErbProgram* program, ErbError* error)
{
    const uint8_t* debug = program->debug;
    const uint32_t length = program->debug_length;
    if (!debug)
    {
        return true;
    }
    /* Before the minor that names panels, the section is one table for all panels. */
    if (erb_u16(program->section + ERB_HEADER_FORMAT_MINOR_AT) < ERB_FORMAT_MINOR_DEBUG_PANELS)
    {
        if (length < ERB_DEBUG_OLD_HEADER_SIZE)
        {
            return fail(error, ERB_ERROR_DEBUG, "build id or function count");
        }
        return check_debug_table(
            program, debug, length, ERB_DEBUG_OLD_HEADER_SIZE, erb_u32(debug + ERB_DEBUG_FUNCTION_COUNT_AT), error);
    }

    /* A table for every panel (ERB_NONE) or one panel's; only this panel's is checked and used. */
    if (!length)
    {
        return fail(error, ERB_ERROR_DEBUG, "an empty debug section");
    }
    const uint16_t panel_count = erb_u16(program->section + ERB_HEADER_PANEL_COUNT_AT);
    const uint8_t* mine = NULL;
    uint32_t mine_length = 0;
    for (uint32_t table_offset = 0; table_offset < length;)
    {
        const uint8_t* table = debug + table_offset;
        if (length - table_offset < ERB_DEBUG_HEADER_SIZE)
        {
            return fail(error, ERB_ERROR_DEBUG, "a table past the section");
        }
        const uint32_t table_length = erb_u32(table + ERB_DEBUG_LENGTH_AT);
        if (table_length < ERB_DEBUG_HEADER_SIZE || table_length > length - table_offset || !erb_aligned4(table_length))
        {
            return fail(error, ERB_ERROR_DEBUG, "a table past the section");
        }
        /* Read only once the whole header is known to be in the section. */
        const uint16_t panel = erb_u16(table + ERB_DEBUG_PANEL_AT);
        if (panel != ERB_NONE && panel >= panel_count)
        {
            return fail(error, ERB_ERROR_DEBUG, "a table for a panel the package doesn't declare");
        }
        if (panel == ERB_NONE || panel == program->panel)
        {
            if (mine)
            {
                return fail(error, ERB_ERROR_DEBUG, "two tables for one panel");
            }
            mine = table;
            mine_length = table_length;
        }
        table_offset += table_length;
    }
    return !mine
           || check_debug_table(
               program, mine, mine_length, ERB_DEBUG_HEADER_SIZE, erb_u16(mine + ERB_DEBUG_FUNCTION_COUNT_AT), error);
}

void erb_debug_lookup(const ErbProgram* program, uint16_t function, uint32_t offset, const char** file, uint32_t* line)
{
    *file = NULL;
    *line = 0;
    if (!program->debug_entries)
    {
        return;
    }
    const uint8_t* strings = program->debug_strings;
    /* The last entry at or before the offset, in this function. */
    for (uint32_t index = 0; index < program->debug_entry_count; index++)
    {
        const uint8_t* entry = program->debug_entries + index * ERB_DEBUG_ENTRY_SIZE;
        if (erb_u16(entry + ERB_DEBUG_ENTRY_FUNCTION_AT) > function
            || (erb_u16(entry + ERB_DEBUG_ENTRY_FUNCTION_AT) == function
                && erb_u32(entry + ERB_DEBUG_ENTRY_OFFSET_AT) > offset))
        {
            break;
        }
        if (erb_u16(entry + ERB_DEBUG_ENTRY_FUNCTION_AT) == function)
        {
            *file = (const char*)strings
                    + erb_u32(strings + ERB_STRINGS_OFFSETS_AT
                              + erb_u16(entry + ERB_DEBUG_ENTRY_FILE_AT) * sizeof(uint32_t));
            *line = erb_u32(entry + ERB_DEBUG_ENTRY_LINE_AT);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Entry
 ---------------------------------------------------------------------------------------------------------------------*/

bool erb_verify(
    const void* package, size_t package_bytes, const ErbConfig* config, ErbProgram* program, ErbError* error)
{
    memset(program, 0, sizeof(*program));
    memset(error, 0, sizeof(*error));
    error->function = ERB_NONE;
    if (!package)
    {
        return fail(error, ERB_ERROR_NO_PACKAGE, "no package");
    }
    /* Tapes and interpolations come before bindings, which name them, so the tables are checked in this order. */
    return check_container(package, package_bytes, program, error) && check_header(config, program, error)
           && check_directory(program, error) && check_data_tables(program, error) && check_arena(program, error)
           && check_slots(program, error) && check_assets(config, program, error) && check_props(program, error)
           && check_functions(program, error) && check_tapes(program, error) && check_interps(program, error)
           && check_bindings(program, error) && check_templates(program, error) && check_anims(program, error)
           && check_imports(config, program, error) && check_inputs(program, error) && check_keyboard(program, error)
           && check_code(config, program, error) && check_debug(program, error);
}
