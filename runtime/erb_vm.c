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
 * erb_vm — the Flow B interpreter. It trusts what the verifier checked (opcodes, register indices and types,
 * branch targets, table indices from operands, call targets and the stack depth) and checks only what
 * depends on values: node handles, list rows from data, string caps, pool room and the budget.
 *
 * A call pushes a frame on the register stack in the arena instead of recursing in C, so the VM adds one
 * fixed C frame whatever the app does.
 */

#include "erb_internal.h"
#include "erb_props.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/** @brief Placement for the dispatch loop: an RP2040 port defines this to its RAM section. */
#ifndef ERB_VM_HOT
#define ERB_VM_HOT
#endif

/** @brief Room for an int64 or a shortest float as text, with its sign and NUL. */
#define NUMBER_TEXT_SIZE 24u

/** @brief Room for toFixed's output: the integer digits of a large float plus ERB_FIXED_DIGITS_MAX decimals. */
#define FIXED_TEXT_SIZE 64u

/* Cells of a call frame, before its registers. */
#define FRAME_CALLER 0u      /**< The calling function, or FRAME_NO_CALLER for the entry point. */
#define FRAME_RETURN_PC 1u   /**< Where the caller resumes. */
#define FRAME_CALLER_BASE 2u /**< The caller's frame. */
#define FRAME_NO_CALLER (-1)

/*----------------------------------------------------------------------------------------------------------------------
 - Arithmetic, with the C output's rules
 ---------------------------------------------------------------------------------------------------------------------*/

static int32_t saturate32(int64_t value)
{
    return value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : (int32_t)value;
}

static int32_t div_zero32(int32_t dividend)
{
    return dividend > 0 ? INT32_MAX : dividend < 0 ? INT32_MIN : 0;
}

static int32_t div32(int32_t dividend, int32_t divisor)
{
    if (divisor == 0)
    {
        return div_zero32(dividend);
    }
    return saturate32((int64_t)dividend / divisor);
}

static int32_t floordiv32(int32_t dividend, int32_t divisor)
{
    if (divisor == 0)
    {
        return div_zero32(dividend);
    }
    const int64_t quotient = (int64_t)dividend / divisor;
    return saturate32(((int64_t)dividend % divisor != 0 && (dividend < 0) != (divisor < 0)) ? quotient - 1 : quotient);
}

static int32_t ceildiv32(int32_t dividend, int32_t divisor)
{
    if (divisor == 0)
    {
        return div_zero32(dividend);
    }
    const int64_t quotient = (int64_t)dividend / divisor;
    return saturate32(((int64_t)dividend % divisor != 0 && (dividend < 0) == (divisor < 0)) ? quotient + 1 : quotient);
}

/** @brief Math.round(dividend / divisor): floor((2 * dividend + divisor) / (2 * divisor)) once divisor is made
 * positive. */
static int32_t rounddiv32(int32_t dividend, int32_t divisor)
{
    if (divisor == 0)
    {
        return div_zero32(dividend);
    }
    const int64_t flipped_dividend = divisor < 0 ? -(int64_t)dividend : dividend;
    const int64_t divisor_magnitude = divisor < 0 ? -(int64_t)divisor : divisor;
    const int64_t doubled = 2 * flipped_dividend + divisor_magnitude;
    const int64_t quotient = doubled / (2 * divisor_magnitude);
    return saturate32((doubled % (2 * divisor_magnitude) != 0 && doubled < 0) ? quotient - 1 : quotient);
}

static int32_t mod32(int32_t dividend, int32_t divisor)
{
    return (divisor == 0 || divisor == -1) ? 0 : dividend % divisor;
}

static int64_t add64(int64_t left, int64_t right)
{
    if (right > 0 ? left > INT64_MAX - right : left < INT64_MIN - right)
    {
        return right > 0 ? INT64_MAX : INT64_MIN;
    }
    return left + right;
}

static int64_t sub64(int64_t left, int64_t right)
{
    if (right < 0 ? left > INT64_MAX + right : left < INT64_MIN + right)
    {
        return right < 0 ? INT64_MAX : INT64_MIN;
    }
    return left - right;
}

static int64_t mul64(int64_t left, int64_t right)
{
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 5)
    int64_t product;
    if (!__builtin_mul_overflow(left, right, &product))
    {
        return product;
    }
#else
    if (left == 0 || right == 0
        || (left > 0 ? (right > 0 ? left <= INT64_MAX / right : right >= INT64_MIN / left)
                     : (right > 0 ? left >= INT64_MIN / right : left >= INT64_MAX / right)))
    {
        return left * right;
    }
#endif
    return (left < 0) == (right < 0) ? INT64_MAX : INT64_MIN;
}

static int64_t neg64(int64_t value)
{
    return value == INT64_MIN ? INT64_MAX : -value;
}

/** @brief The zero divisor and INT64_MIN / -1, shared by every 64-bit division. Returns true when handled. */
static bool div_edge64(int64_t dividend, int64_t divisor, int64_t* out)
{
    if (divisor == 0)
    {
        *out = dividend > 0 ? INT64_MAX : dividend < 0 ? INT64_MIN : 0;
        return true;
    }
    if (divisor == -1)
    {
        *out = neg64(dividend);
        return true;
    }
    return false;
}

static int64_t div64(int64_t dividend, int64_t divisor)
{
    int64_t out;
    return div_edge64(dividend, divisor, &out) ? out : dividend / divisor;
}

static int64_t floordiv64(int64_t dividend, int64_t divisor)
{
    int64_t out;
    if (div_edge64(dividend, divisor, &out))
    {
        return out;
    }
    const int64_t quotient = dividend / divisor;
    return (dividend % divisor != 0 && (dividend < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

static int64_t ceildiv64(int64_t dividend, int64_t divisor)
{
    int64_t out;
    if (div_edge64(dividend, divisor, &out))
    {
        return out;
    }
    const int64_t quotient = dividend / divisor;
    return (dividend % divisor != 0 && (dividend < 0) == (divisor < 0)) ? quotient + 1 : quotient;
}

/** @brief Math.round(dividend / divisor), halves up: the floor, plus one when the rest is at least half the divisor. */
static int64_t rounddiv64(int64_t dividend, int64_t divisor)
{
    int64_t out;
    if (div_edge64(dividend, divisor, &out))
    {
        return out;
    }
    int64_t quotient = dividend / divisor;
    int64_t rest = dividend % divisor;
    if (rest != 0 && (rest < 0) != (divisor < 0))
    {
        quotient -= 1;
        rest += divisor;
    }
    return (divisor > 0 ? rest >= divisor - rest : rest <= divisor - rest) ? quotient + 1 : quotient;
}

static int64_t mod64(int64_t dividend, int64_t divisor)
{
    return (divisor == 0 || divisor == -1) ? 0 : dividend % divisor;
}

static int32_t float_to_int32(float value)
{
    if (value != value)
    {
        return 0;
    }
    if (value >= -(float)INT32_MIN)
    {
        return INT32_MAX;
    }
    if (value <= (float)INT32_MIN)
    {
        return INT32_MIN;
    }
    return (int32_t)value;
}

static int32_t double_to_int32(double value)
{
    if (value != value)
    {
        return 0;
    }
    if (value >= (double)INT32_MAX)
    {
        return INT32_MAX;
    }
    if (value <= (double)INT32_MIN)
    {
        return INT32_MIN;
    }
    return (int32_t)value;
}

static int64_t double_to_int64(double value)
{
    if (value != value)
    {
        return 0;
    }
    if (value >= (double)INT64_MAX)
    {
        return INT64_MAX;
    }
    if (value <= (double)INT64_MIN)
    {
        return INT64_MIN;
    }
    return (int64_t)value;
}

/** @brief Math.round of a float: halves up, as JS rounds. */
static float roundf_js(float value)
{
    const float floor_value = floorf(value);
    return value - floor_value >= 0.5f ? floor_value + 1.0f : floor_value;
}

static double round_js(double value)
{
    const double floor_value = floor(value);
    return value - floor_value >= 0.5 ? floor_value + 1.0 : floor_value;
}

static float fmath(uint32_t function, float left, float right)
{
    switch (function)
    {
        case ERB_MATH_SIN:
            return sinf(left);
        case ERB_MATH_COS:
            return cosf(left);
        case ERB_MATH_TAN:
            return tanf(left);
        case ERB_MATH_SQRT:
            return sqrtf(left);
        case ERB_MATH_ABS:
            return fabsf(left);
        case ERB_MATH_ROUND:
            return roundf_js(left);
        case ERB_MATH_FLOOR:
            return floorf(left);
        case ERB_MATH_CEIL:
            return ceilf(left);
        case ERB_MATH_MIN:
            return (left != left || right != right) ? NAN : left < right ? left : right;
        case ERB_MATH_MAX:
            return (left != left || right != right) ? NAN : left > right ? left : right;
        case ERB_MATH_ATAN2:
            return atan2f(left, right);
        case ERB_MATH_POW:
            return powf(left, right);
        default:
            return fmodf(left, right);
    }
}

static double dmath(uint32_t function, double left, double right)
{
    switch (function)
    {
        case ERB_MATH_SIN:
            return sin(left);
        case ERB_MATH_COS:
            return cos(left);
        case ERB_MATH_TAN:
            return tan(left);
        case ERB_MATH_SQRT:
            return sqrt(left);
        case ERB_MATH_ABS:
            return fabs(left);
        case ERB_MATH_ROUND:
            return round_js(left);
        case ERB_MATH_FLOOR:
            return floor(left);
        case ERB_MATH_CEIL:
            return ceil(left);
        case ERB_MATH_MIN:
            return (left != left || right != right) ? NAN : left < right ? left : right;
        case ERB_MATH_MAX:
            return (left != left || right != right) ? NAN : left > right ? left : right;
        case ERB_MATH_ATAN2:
            return atan2(left, right);
        case ERB_MATH_POW:
            return pow(left, right);
        default:
            return fmod(left, right);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Register cells
 ---------------------------------------------------------------------------------------------------------------------*/

static float get_float(const int32_t* cell)
{
    float value;
    memcpy(&value, cell, sizeof(value));
    return value;
}

static void set_float(int32_t* cell, float value)
{
    memcpy(cell, &value, sizeof(value));
}

static int64_t get_int64(const int32_t* cell)
{
    return (int64_t)((uint64_t)(uint32_t)cell[0] | ((uint64_t)(uint32_t)cell[1] << 32));
}

static void set_int64(int32_t* cell, int64_t value)
{
    cell[0] = (int32_t)(uint32_t)(uint64_t)value;
    cell[1] = (int32_t)(uint32_t)((uint64_t)value >> 32);
}

static double get_double(const int32_t* cell)
{
    const uint64_t bits = (uint64_t)get_int64(cell);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void set_double(int32_t* cell, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    set_int64(cell, (int64_t)bits);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Strings and the format buffer
 ---------------------------------------------------------------------------------------------------------------------*/

static char s_format[ER_TEXT_MAX + 1];
static uint32_t s_format_length;
static uint32_t s_piece; /**< Where the last piece starts, for FMTPAD. */
static bool s_format_cut;

static const char* string_of(int32_t ref)
{
    const uint32_t value = (uint32_t)ref;
    if (value == 0u)
    {
        return "";
    }
    if (value & ERB_STRING_ARENA)
    {
        return (const char*)g_erb.arena + (value & ~ERB_STRING_ARENA);
    }
    return erb_string(&g_erb.program, value - 1u);
}

static void format_append(const char* text)
{
    s_piece = s_format_length;
    const size_t length = strlen(text);
    const size_t room = ER_TEXT_MAX - s_format_length;
    const size_t take = length < room ? length : room;
    memcpy(s_format + s_format_length, text, take);
    s_format_length += (uint32_t)take;
    s_format[s_format_length] = '\0';
    s_format_cut |= take < length;
}

/** @brief A float as String() spells it where %g doesn't: Infinity, -Infinity, NaN, and 0 for -0. */
static const char* float_text(char* buffer, size_t capacity, float value)
{
    if (value != value)
    {
        return "NaN";
    }
    if (isinf(value))
    {
        return value > 0.0f ? "Infinity" : "-Infinity";
    }
    if (value == 0.0f)
    {
        return "0";
    }
    snprintf(buffer, capacity, "%g", (double)value);
    return buffer;
}

/** @brief Copies @p text into a buffer of @p capacity characters. Returns true when it fitted. */
static bool copy_capped(char* destination, uint32_t capacity, const char* text)
{
    const size_t length = strlen(text);
    const size_t take = length < capacity ? length : capacity;
    memmove(destination, text, take);
    destination[take] = '\0';
    return take == length;
}

/*----------------------------------------------------------------------------------------------------------------------
 - The interpreter
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Where the running instruction is, for faults and reports. */
static uint16_t s_function;
static uint32_t s_pc;

static ErbStatus fault(ErbStatus status, const char* detail, const char* name)
{
    return erb_rt_fault(status, detail, name, s_function, s_pc);
}

static void report(const char* what)
{
    erb_rt_report(what, s_function, s_pc);
}

/** @brief A node handle that doesn't resolve: a fault in a debug package, a reported skip in a release one. */
static ErbStatus stale(void)
{
    if (g_erb.program.flags & ERB_FLAG_DEBUG)
    {
        return fault(ERB_ERR_STALE_NODE, "a node handle outlived its node", NULL);
    }
    report("stale node handle; the instruction was skipped");
    return ERB_OK;
}

static uint8_t* slot_data(const uint8_t* slot)
{
    return g_erb.arena + erb_u32(slot + ERB_SLOT_OFFSET_AT);
}

static void mark(const uint8_t* slot)
{
    if (slot[ERB_SLOT_FLAGS_AT] & ERB_SLOT_TRACKED)
    {
        erb_rt_set_dirty(erb_u16(slot + ERB_SLOT_DIRTY_BIT_AT));
    }
}

/** @brief Writes a string into a string buffer of @p capacity, marking @p slot dirty when it changed. */
static void store_string(const uint8_t* slot, char* buffer, uint32_t capacity, const char* text)
{
    if (strcmp(buffer, text) == 0)
    {
        return;
    }
    if (!copy_capped(buffer, capacity, text))
    {
        report("string cut short at its cap");
    }
    mark(slot);
}

/** @brief The row field an instruction's extension word names, in a list's pool. NULL when the row isn't there. */
static uint8_t* row_field(uint32_t extension, int32_t row, const uint8_t** slot_out, const uint8_t** field_out)
{
    const ErbProgram* program = &g_erb.program;
    const uint8_t* slot = erb_slot(program, ERB_LOW16(extension));
    const uint8_t* layout = erb_row(program, erb_u16(slot + ERB_SLOT_AUX_AT));
    const uint8_t* field =
        program->fields + (erb_u16(layout + ERB_ROW_FIRST_AT) + ERB_EXT_OPERAND(extension)) * ERB_FIELD_SIZE;
    uint8_t* pool = slot_data(slot);
    uint32_t count;
    memcpy(&count, pool, sizeof(count));
    *slot_out = slot;
    *field_out = field;
    if (row < 0 || (uint32_t)row >= count)
    {
        return NULL;
    }
    return pool + ERB_LIST_HEADER_SIZE + (uint32_t)row * erb_u16(layout + ERB_ROW_SIZE_AT)
           + erb_u32(field + ERB_FIELD_OFFSET_AT);
}

ERB_VM_HOT ErbStatus erb_vm_run(uint16_t function, const int32_t* args, int32_t* result)
{
    const ErbProgram* program = &g_erb.program;
    uint8_t* arena = g_erb.arena;
    int32_t* stack = (int32_t*)(arena + program->regions[ERB_REGION_STACK].offset);
    uint32_t* dirty = (uint32_t*)(arena + program->regions[ERB_REGION_DIRTY].offset);
    float* tape = (float*)(arena + program->regions[ERB_REGION_TAPE].offset);
    ERProps* props = &g_erb_props;

    const bool counting = erb_bit_get(program->counting, function);
    uint32_t budget_left = g_erb.budget;

    const uint8_t* record = erb_function(program, function);
    const uint32_t* code = program->code + erb_u32(record + ERB_FUNCTION_CODE_AT);
    uint32_t base = 0;
    int32_t* registers = stack + ERB_FRAME_CELLS;
    memset(registers, 0, record[ERB_FUNCTION_REGISTERS_AT] * sizeof(int32_t));
    if (record[ERB_FUNCTION_PARAMS_AT])
    {
        memcpy(registers, args, record[ERB_FUNCTION_PARAMS_AT] * sizeof(int32_t));
    }
    stack[FRAME_CALLER] = FRAME_NO_CALLER;
    s_function = function;
    s_format_length = 0;
    s_format[0] = '\0';
    s_format_cut = false;

    uint32_t pc = 0;
    ErbStatus status = ERB_OK;
    for (;;)
    {
        const uint32_t word = code[pc];
        const uint8_t operand_a = ERB_A(word);
        const uint8_t operand_b = ERB_B(word);
        const uint8_t operand_c = ERB_C(word);
        const uint16_t operand_bx = ERB_BX(word);
        s_pc = pc;
        if (counting)
        {
            if (budget_left == 0u)
            {
                return fault(ERB_ERR_BUDGET, "instruction budget exceeded", NULL);
            }
            budget_left--;
        }
        uint32_t next = pc + 1u;

        switch (ERB_OP(word))
        {
            /* Moves */
            case ERB_OP_MOVE:
                registers[operand_a] = registers[operand_b];
                if (erb_type_cells(program->types[erb_u32(record + ERB_FUNCTION_TYPES_AT) + operand_a]) == 2u)
                {
                    registers[operand_a + 1] = registers[operand_b + 1];
                }
                break;
            case ERB_OP_LOADI:
                registers[operand_a] = ERB_SBX(word);
                break;
            case ERB_OP_LOADK:
                registers[operand_a] = (int32_t)program->const32[operand_bx];
                break;
            case ERB_OP_LOADK64:
                memcpy(&registers[operand_a], program->const64 + operand_bx * 8u, 8u);
                break;
            case ERB_OP_LOADS:
                registers[operand_a] = (int32_t)(operand_bx + 1u);
                break;
            case ERB_OP_LOADE:
                registers[operand_a] = operand_bx;
                next = pc + 2u;
                break;

            /* Slots */
            case ERB_OP_GET32:
                memcpy(&registers[operand_a], slot_data(erb_slot(program, operand_bx)), 4u);
                break;
            case ERB_OP_GET64:
                memcpy(&registers[operand_a], slot_data(erb_slot(program, operand_bx)), 8u);
                break;
            case ERB_OP_GETS:
                registers[operand_a] =
                    (int32_t)(ERB_STRING_ARENA | erb_u32(erb_slot(program, operand_bx) + ERB_SLOT_OFFSET_AT));
                break;
            case ERB_OP_SET32:
            case ERB_OP_SET64:
            {
                const uint8_t* slot = erb_slot(program, operand_bx);
                const size_t bytes = ERB_OP(word) == ERB_OP_SET32 ? 4u : 8u;
                uint8_t* data = slot_data(slot);
                if (memcmp(data, &registers[operand_a], bytes) != 0)
                {
                    memcpy(data, &registers[operand_a], bytes);
                    mark(slot);
                }
                break;
            }
            case ERB_OP_SETS:
            {
                const uint8_t* slot = erb_slot(program, operand_bx);
                store_string(
                    slot, (char*)slot_data(slot), erb_u16(slot + ERB_SLOT_CAP_AT), string_of(registers[operand_a]));
                break;
            }
            case ERB_OP_GETNI:
            {
                const uint8_t* slot = erb_slot(program, ERB_LOW16(code[pc + 1u]));
                if (registers[operand_b] < 0 || (uint32_t)registers[operand_b] >= erb_u16(slot + ERB_SLOT_CAP_AT))
                {
                    return fault(ERB_ERR_LIST_INDEX, "handle index past its array", NULL);
                }
                memcpy(&registers[operand_a], slot_data(slot) + (uint32_t)registers[operand_b] * 8u, 8u);
                next = pc + 2u;
                break;
            }

            /* Lists */
            case ERB_OP_LEN:
                memcpy(&registers[operand_a], slot_data(erb_slot(program, operand_bx)), 4u);
                break;
            case ERB_OP_ROWGET:
            case ERB_OP_ROWSET:
            {
                const uint8_t* slot;
                const uint8_t* field;
                uint8_t* data = row_field(code[pc + 1u], registers[operand_b], &slot, &field);
                if (!data)
                {
                    return fault(ERB_ERR_LIST_INDEX, "list row past the list's end", NULL);
                }
                const uint8_t type = field[ERB_FIELD_TYPE_AT];
                if (ERB_OP(word) == ERB_OP_ROWGET)
                {
                    if (type == ERB_T_STRING)
                    {
                        registers[operand_a] = (int32_t)(ERB_STRING_ARENA | (uint32_t)(data - arena));
                    }
                    else
                    {
                        memcpy(&registers[operand_a], data, erb_type_cells(type) * sizeof(int32_t));
                    }
                }
                else if (type == ERB_T_STRING)
                {
                    store_string(slot, (char*)data, erb_u16(field + ERB_FIELD_CAP_AT), string_of(registers[operand_a]));
                }
                else if (memcmp(data, &registers[operand_a], erb_type_cells(type) * sizeof(int32_t)) != 0)
                {
                    memcpy(data, &registers[operand_a], erb_type_cells(type) * sizeof(int32_t));
                    mark(slot);
                }
                next = pc + 2u;
                break;
            }
            case ERB_OP_APPEND:
            {
                const uint8_t* slot = erb_slot(program, operand_bx);
                const uint8_t* layout = erb_row(program, erb_u16(slot + ERB_SLOT_AUX_AT));
                uint8_t* pool = slot_data(slot);
                uint32_t count;
                memcpy(&count, pool, sizeof(count));
                if (count >= erb_u16(slot + ERB_SLOT_CAP_AT))
                {
                    report("list full; the row was dropped (raise ER_AOT_LIST_CAP)");
                    break;
                }
                uint8_t* row = pool + ERB_LIST_HEADER_SIZE + count * erb_u16(layout + ERB_ROW_SIZE_AT);
                uint32_t cell = operand_a;
                for (uint32_t index = 0; index < erb_u16(layout + ERB_ROW_FIELD_COUNT_AT); index++)
                {
                    const uint8_t* field =
                        program->fields + (erb_u16(layout + ERB_ROW_FIRST_AT) + index) * ERB_FIELD_SIZE;
                    uint8_t* data = row + erb_u32(field + ERB_FIELD_OFFSET_AT);
                    if (field[ERB_FIELD_TYPE_AT] == ERB_T_STRING)
                    {
                        if (!copy_capped((char*)data, erb_u16(field + ERB_FIELD_CAP_AT), string_of(registers[cell])))
                        {
                            report("string cut short at its cap");
                        }
                    }
                    else
                    {
                        memcpy(data, &registers[cell], erb_type_cells(field[ERB_FIELD_TYPE_AT]) * sizeof(int32_t));
                    }
                    cell += erb_type_cells(field[ERB_FIELD_TYPE_AT]);
                }
                count++;
                memcpy(pool, &count, sizeof(count));
                mark(slot);
                break;
            }
            case ERB_OP_CLEAR:
            case ERB_OP_POP:
            case ERB_OP_KEEP:
            {
                const uint8_t* slot = erb_slot(program, operand_bx);
                uint8_t* pool = slot_data(slot);
                uint32_t count;
                memcpy(&count, pool, sizeof(count));
                uint32_t kept = 0;
                if (ERB_OP(word) == ERB_OP_POP)
                {
                    kept = count ? count - 1u : 0u;
                }
                else if (ERB_OP(word) == ERB_OP_KEEP)
                {
                    kept = registers[operand_a] < 0                 ? 0u
                           : (uint32_t)registers[operand_a] < count ? (uint32_t)registers[operand_a]
                                                                    : count;
                }
                if (kept != count)
                {
                    memcpy(pool, &kept, sizeof(kept));
                    mark(slot);
                }
                break;
            }
            case ERB_OP_SLICE:
            {
                const uint8_t* slot = erb_slot(program, ERB_LOW16(code[pc + 1u]));
                const uint32_t row_size = erb_u16(erb_row(program, erb_u16(slot + ERB_SLOT_AUX_AT)) + ERB_ROW_SIZE_AT);
                uint8_t* pool = slot_data(slot);
                uint32_t count;
                memcpy(&count, pool, sizeof(count));
                const int64_t length = count;
                int64_t start = registers[operand_a] < 0 ? length + registers[operand_a] : registers[operand_a];
                int64_t end = registers[operand_b] < 0 ? length + registers[operand_b] : registers[operand_b];
                start = start < 0 ? 0 : start > length ? length : start;
                end = end < 0 ? 0 : end > length ? length : end;
                const uint32_t kept = end > start ? (uint32_t)(end - start) : 0u;
                if (kept != count)
                {
                    memmove(pool + ERB_LIST_HEADER_SIZE,
                            pool + ERB_LIST_HEADER_SIZE + (uint32_t)start * row_size,
                            kept * row_size);
                    memcpy(pool, &kept, sizeof(kept));
                    mark(slot);
                }
                next = pc + 2u;
                break;
            }

            /* Whole numbers */
            case ERB_OP_ADD:
                registers[operand_a] = saturate32((int64_t)registers[operand_b] + registers[operand_c]);
                break;
            case ERB_OP_SUB:
                registers[operand_a] = saturate32((int64_t)registers[operand_b] - registers[operand_c]);
                break;
            case ERB_OP_MUL:
                registers[operand_a] = saturate32((int64_t)registers[operand_b] * registers[operand_c]);
                break;
            case ERB_OP_DIV:
                registers[operand_a] = div32(registers[operand_b], registers[operand_c]);
                break;
            case ERB_OP_FLOORDIV:
                registers[operand_a] = floordiv32(registers[operand_b], registers[operand_c]);
                break;
            case ERB_OP_CEILDIV:
                registers[operand_a] = ceildiv32(registers[operand_b], registers[operand_c]);
                break;
            case ERB_OP_ROUNDDIV:
                registers[operand_a] = rounddiv32(registers[operand_b], registers[operand_c]);
                break;
            case ERB_OP_MOD:
                registers[operand_a] = mod32(registers[operand_b], registers[operand_c]);
                break;
            case ERB_OP_MIN:
                registers[operand_a] =
                    registers[operand_b] < registers[operand_c] ? registers[operand_b] : registers[operand_c];
                break;
            case ERB_OP_MAX:
                registers[operand_a] =
                    registers[operand_b] > registers[operand_c] ? registers[operand_b] : registers[operand_c];
                break;
            case ERB_OP_NEG:
                registers[operand_a] = registers[operand_b] == INT32_MIN ? INT32_MAX : -registers[operand_b];
                break;
            case ERB_OP_ABS:
                registers[operand_a] = registers[operand_b] >= 0           ? registers[operand_b]
                                       : registers[operand_b] == INT32_MIN ? INT32_MAX
                                                                           : -registers[operand_b];
                break;
            case ERB_OP_ADDL:
                set_int64(&registers[operand_a],
                          add64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_SUBL:
                set_int64(&registers[operand_a],
                          sub64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_MULL:
                set_int64(&registers[operand_a],
                          mul64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_DIVL:
                set_int64(&registers[operand_a],
                          div64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_FLOORDIVL:
                set_int64(&registers[operand_a],
                          floordiv64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_CEILDIVL:
                set_int64(&registers[operand_a],
                          ceildiv64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_ROUNDDIVL:
                set_int64(&registers[operand_a],
                          rounddiv64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_MODL:
                set_int64(&registers[operand_a],
                          mod64(get_int64(&registers[operand_b]), get_int64(&registers[operand_c])));
                break;
            case ERB_OP_MINL:
                set_int64(&registers[operand_a],
                          get_int64(&registers[operand_b]) < get_int64(&registers[operand_c])
                              ? get_int64(&registers[operand_b])
                              : get_int64(&registers[operand_c]));
                break;
            case ERB_OP_MAXL:
                set_int64(&registers[operand_a],
                          get_int64(&registers[operand_b]) > get_int64(&registers[operand_c])
                              ? get_int64(&registers[operand_b])
                              : get_int64(&registers[operand_c]));
                break;
            case ERB_OP_NEGL:
                set_int64(&registers[operand_a], neg64(get_int64(&registers[operand_b])));
                break;
            case ERB_OP_ABSL:
                set_int64(&registers[operand_a],
                          get_int64(&registers[operand_b]) >= 0 ? get_int64(&registers[operand_b])
                                                                : neg64(get_int64(&registers[operand_b])));
                break;

            /* Floats and doubles */
            case ERB_OP_FADD:
                set_float(&registers[operand_a], get_float(&registers[operand_b]) + get_float(&registers[operand_c]));
                break;
            case ERB_OP_FSUB:
                set_float(&registers[operand_a], get_float(&registers[operand_b]) - get_float(&registers[operand_c]));
                break;
            case ERB_OP_FMUL:
                set_float(&registers[operand_a], get_float(&registers[operand_b]) * get_float(&registers[operand_c]));
                break;
            case ERB_OP_FDIV:
                set_float(&registers[operand_a], get_float(&registers[operand_b]) / get_float(&registers[operand_c]));
                break;
            case ERB_OP_FNEG:
                set_float(&registers[operand_a], -get_float(&registers[operand_b]));
                break;
            case ERB_OP_FMATH:
                set_float(&registers[operand_a],
                          fmath(code[pc + 1u], get_float(&registers[operand_b]), get_float(&registers[operand_c])));
                next = pc + 2u;
                break;
            case ERB_OP_DADD:
                set_double(&registers[operand_a],
                           get_double(&registers[operand_b]) + get_double(&registers[operand_c]));
                break;
            case ERB_OP_DSUB:
                set_double(&registers[operand_a],
                           get_double(&registers[operand_b]) - get_double(&registers[operand_c]));
                break;
            case ERB_OP_DMUL:
                set_double(&registers[operand_a],
                           get_double(&registers[operand_b]) * get_double(&registers[operand_c]));
                break;
            case ERB_OP_DDIV:
                set_double(&registers[operand_a],
                           get_double(&registers[operand_b]) / get_double(&registers[operand_c]));
                break;
            case ERB_OP_DNEG:
                set_double(&registers[operand_a], -get_double(&registers[operand_b]));
                break;
            case ERB_OP_DMATH:
                set_double(&registers[operand_a],
                           dmath(code[pc + 1u], get_double(&registers[operand_b]), get_double(&registers[operand_c])));
                next = pc + 2u;
                break;

            /* Conversions */
            case ERB_OP_ITOF:
                set_float(&registers[operand_a], (float)registers[operand_b]);
                break;
            case ERB_OP_FTOI:
                registers[operand_a] = float_to_int32(get_float(&registers[operand_b]));
                break;
            case ERB_OP_LTOF:
                set_float(&registers[operand_a], (float)get_int64(&registers[operand_b]));
                break;
            case ERB_OP_ITOL:
                set_int64(&registers[operand_a], registers[operand_b]);
                break;
            case ERB_OP_LTOI:
                registers[operand_a] = saturate32(get_int64(&registers[operand_b]));
                break;
            case ERB_OP_DTOI:
                registers[operand_a] = double_to_int32(get_double(&registers[operand_b]));
                break;
            case ERB_OP_FTOD:
                set_double(&registers[operand_a], (double)get_float(&registers[operand_b]));
                break;
            case ERB_OP_DTOF:
                set_float(&registers[operand_a], (float)get_double(&registers[operand_b]));
                break;
            case ERB_OP_ITOD:
                set_double(&registers[operand_a], (double)registers[operand_b]);
                break;
            case ERB_OP_LTOD:
                set_double(&registers[operand_a], (double)get_int64(&registers[operand_b]));
                break;
            case ERB_OP_DTOL:
                set_int64(&registers[operand_a], double_to_int64(get_double(&registers[operand_b])));
                break;
            case ERB_OP_FTOL:
                set_int64(&registers[operand_a], double_to_int64((double)get_float(&registers[operand_b])));
                break;

            /* Comparisons */
            case ERB_OP_EQ:
                registers[operand_a] = registers[operand_b] == registers[operand_c];
                break;
            case ERB_OP_LT:
                registers[operand_a] = registers[operand_b] < registers[operand_c];
                break;
            case ERB_OP_LE:
                registers[operand_a] = registers[operand_b] <= registers[operand_c];
                break;
            case ERB_OP_FEQ:
                registers[operand_a] = get_float(&registers[operand_b]) == get_float(&registers[operand_c]);
                break;
            case ERB_OP_FLT:
                registers[operand_a] = get_float(&registers[operand_b]) < get_float(&registers[operand_c]);
                break;
            case ERB_OP_FLE:
                registers[operand_a] = get_float(&registers[operand_b]) <= get_float(&registers[operand_c]);
                break;
            case ERB_OP_LEQ:
                registers[operand_a] = get_int64(&registers[operand_b]) == get_int64(&registers[operand_c]);
                break;
            case ERB_OP_LLT:
                registers[operand_a] = get_int64(&registers[operand_b]) < get_int64(&registers[operand_c]);
                break;
            case ERB_OP_LLE:
                registers[operand_a] = get_int64(&registers[operand_b]) <= get_int64(&registers[operand_c]);
                break;
            case ERB_OP_DEQ:
                registers[operand_a] = get_double(&registers[operand_b]) == get_double(&registers[operand_c]);
                break;
            case ERB_OP_DLT:
                registers[operand_a] = get_double(&registers[operand_b]) < get_double(&registers[operand_c]);
                break;
            case ERB_OP_DLE:
                registers[operand_a] = get_double(&registers[operand_b]) <= get_double(&registers[operand_c]);
                break;
            case ERB_OP_SEQ:
                registers[operand_a] = strcmp(string_of(registers[operand_b]), string_of(registers[operand_c])) == 0;
                break;
            case ERB_OP_NOT:
                registers[operand_a] = !registers[operand_b];
                break;

            /* Control */
            case ERB_OP_JMP:
                next = (uint32_t)((int32_t)next + ERB_SBX(word));
                break;
            case ERB_OP_JT:
                if (registers[operand_a])
                {
                    next = (uint32_t)((int32_t)next + ERB_SBX(word));
                }
                break;
            case ERB_OP_JF:
                if (!registers[operand_a])
                {
                    next = (uint32_t)((int32_t)next + ERB_SBX(word));
                }
                break;
            case ERB_OP_JDIRTY:
                next = pc + 2u;
                if (dirty[program->dirty_words + operand_a] & code[pc + 1u])
                {
                    next = (uint32_t)((int32_t)next + ERB_SBX(word));
                }
                break;
            case ERB_OP_LOOPSET:
                registers[operand_a] = operand_bx;
                break;
            case ERB_OP_LOOP:
                if (--registers[operand_a] > 0)
                {
                    next = (uint32_t)((int32_t)next + ERB_SBX(word));
                }
                break;
            case ERB_OP_RET:
            {
                const uint8_t result_type = record[ERB_FUNCTION_RESULT_AT];
                const uint32_t cells = result_type ? erb_type_cells(result_type) : 0u;
                if (stack[base + FRAME_CALLER] == FRAME_NO_CALLER)
                {
                    if (cells && result)
                    {
                        memcpy(result, &registers[operand_a], cells * sizeof(int32_t));
                    }
                    return ERB_OK;
                }
                /* Back to the caller: the result goes to the CALL's A. */
                const uint16_t caller = (uint16_t)stack[base + FRAME_CALLER];
                const uint32_t return_pc = (uint32_t)stack[base + FRAME_RETURN_PC];
                const uint32_t caller_base = (uint32_t)stack[base + FRAME_CALLER_BASE];
                int32_t* callee_registers = registers;
                record = erb_function(program, caller);
                code = program->code + erb_u32(record + ERB_FUNCTION_CODE_AT);
                base = caller_base;
                registers = stack + base + ERB_FRAME_CELLS;
                if (cells)
                {
                    memcpy(
                        &registers[ERB_A(code[return_pc - 1u])], &callee_registers[operand_a], cells * sizeof(int32_t));
                }
                s_function = caller;
                next = return_pc;
                break;
            }

            /* Calls */
            case ERB_OP_CALL:
            {
                const uint8_t* callee = erb_function(program, operand_bx);
                const uint32_t callee_base = base + ERB_FRAME_CELLS + record[ERB_FUNCTION_REGISTERS_AT];
                int32_t* callee_registers = stack + callee_base + ERB_FRAME_CELLS;
                stack[callee_base + FRAME_CALLER] = s_function;
                stack[callee_base + FRAME_RETURN_PC] = (int32_t)next;
                stack[callee_base + FRAME_CALLER_BASE] = (int32_t)base;
                memset(callee_registers, 0, callee[ERB_FUNCTION_REGISTERS_AT] * sizeof(int32_t));
                memcpy(callee_registers, &registers[operand_a], callee[ERB_FUNCTION_PARAMS_AT] * sizeof(int32_t));
                record = callee;
                code = program->code + erb_u32(record + ERB_FUNCTION_CODE_AT);
                base = callee_base;
                registers = callee_registers;
                s_function = operand_bx;
                next = 0;
                break;
            }
            case ERB_OP_CALLN:
            {
                const uint8_t* import = program->imports + operand_bx * ERB_IMPORT_SIZE;
                const ErbNative* native = &g_erb.config.natives[program->natives[operand_bx]];
                int32_t value = 0;
                if (!native->callback(&registers[operand_a], &value, g_erb.config.context))
                {
                    return fault(ERB_ERR_NATIVE,
                                 "a native import reported failure",
                                 erb_string(program, erb_u16(import + ERB_IMPORT_NAME_AT)));
                }
                if (erb_string(program, erb_u16(import + ERB_IMPORT_SIGNATURE_AT))[0] != 'v')
                {
                    registers[operand_a] = value;
                }
                break;
            }

            /* Text */
            case ERB_OP_FMTK:
                format_append(erb_string(program, operand_bx));
                break;
            case ERB_OP_FMTS:
                format_append(string_of(registers[operand_a]));
                break;
            case ERB_OP_FMTI:
            case ERB_OP_FMTL:
            {
                char text[NUMBER_TEXT_SIZE];
                snprintf(text,
                         sizeof(text),
                         "%" PRId64,
                         ERB_OP(word) == ERB_OP_FMTI ? (int64_t)registers[operand_a]
                                                     : get_int64(&registers[operand_a]));
                format_append(text);
                break;
            }
            case ERB_OP_FMTF:
            {
                char text[NUMBER_TEXT_SIZE];
                format_append(float_text(text, sizeof(text), get_float(&registers[operand_a])));
                break;
            }
            case ERB_OP_FMTB:
                format_append(registers[operand_a] ? "true" : "false");
                break;
            case ERB_OP_FMTE:
            {
                const uint32_t count = erb_enum_count(program, operand_bx);
                format_append(erb_string(
                    program,
                    erb_enum_string(program,
                                    operand_bx,
                                    (uint32_t)registers[operand_a] < count ? (uint32_t)registers[operand_a] : 0u)));
                break;
            }
            case ERB_OP_FMTFX:
            {
                char text[FIXED_TEXT_SIZE];
                const float value = get_float(&registers[operand_a]);
                if (value != value || isinf(value))
                {
                    format_append(float_text(text, sizeof(text), value));
                }
                else
                {
                    snprintf(text, sizeof(text), "%.*f", (int)operand_b, value == 0.0f ? 0.0 : (double)value);
                    format_append(text);
                }
                break;
            }
            case ERB_OP_FMTPAD:
            {
                const uint32_t length = s_format_length - s_piece;
                if (length < operand_a)
                {
                    uint32_t shift = operand_a - length;
                    if (s_format_length + shift > ER_TEXT_MAX)
                    {
                        shift = ER_TEXT_MAX - s_format_length;
                        s_format_cut = true;
                    }
                    memmove(s_format + s_piece + shift, s_format + s_piece, length);
                    memset(s_format + s_piece, operand_b, shift);
                    s_format_length += shift;
                    s_format[s_format_length] = '\0';
                }
                break;
            }
            case ERB_OP_FMTSET:
            case ERB_OP_FMTTEXT:
            case ERB_OP_FMTSPAN:
            {
                if (ERB_OP(word) == ERB_OP_FMTSET)
                {
                    const uint8_t* slot = erb_slot(program, operand_bx);
                    store_string(slot, (char*)slot_data(slot), erb_u16(slot + ERB_SLOT_CAP_AT), s_format);
                }
                else if (ERB_OP(word) == ERB_OP_FMTTEXT)
                {
                    s_format_cut |= !copy_capped(props->text, ER_TEXT_MAX, s_format);
                }
                else
                {
                    s_format_cut |= !copy_capped(props->spans[operand_a].text, ER_SPAN_TEXT_MAX, s_format);
                }
                if (s_format_cut)
                {
                    report("text cut short at its cap");
                }
                s_format_length = 0;
                s_format[0] = '\0';
                s_format_cut = false;
                break;
            }

            /* Props */
            case ERB_OP_PROPS:
                erb_rt_props(operand_bx);
                break;
            case ERB_OP_PSETI:
                erb_rt_pset_int(operand_bx, registers[operand_a]);
                break;
            case ERB_OP_PSETF:
                erb_rt_pset_float(operand_bx, get_float(&registers[operand_a]));
                break;
            case ERB_OP_PSETB:
                erb_rt_pset_int(operand_bx, registers[operand_a] != 0);
                break;
            case ERB_OP_PSETS:
                if (!erb_rt_pset_string(operand_bx, string_of(registers[operand_a])))
                {
                    report("prop text cut short, or an image that isn't registered");
                }
                break;
            case ERB_OP_PSETE:
            {
                const uint32_t extension = code[pc + 1u];
                const uint32_t count = erb_enum_count(program, ERB_HIGH16(extension));
                const uint32_t value = (uint32_t)registers[operand_a] < count ? (uint32_t)registers[operand_a] : 0u;
                erb_rt_pset_int(operand_bx, (int32_t)program->const32[ERB_LOW16(extension) + value]);
                next = pc + 2u;
                break;
            }
            case ERB_OP_SPANS:
                props->span_count = operand_a;
                break;
            case ERB_OP_COMMIT:
            {
                ERNode* node = erb_rt_node(&registers[operand_a]);
                if (!node)
                {
                    status = stale();
                    break;
                }
                er_node_set_props(node, props);
                break;
            }

            /* Nodes */
            case ERB_OP_BUILD:
            {
                const uint32_t extension = code[pc + 1u];
                const uint8_t index = ERB_EXT_OPERAND(extension);
                ERNode* parent = erb_rt_node(&registers[operand_b]);
                ERNode* before = operand_c == operand_b ? NULL : erb_rt_node(&registers[operand_c]);
                next = pc + 2u;
                if (!parent)
                {
                    erb_rt_set_node(&registers[operand_a], NULL);
                    status = stale();
                    break;
                }
                if (counting)
                {
                    const uint32_t nodes =
                        erb_u16(erb_template(program, ERB_LOW16(extension)) + ERB_TEMPLATE_NODE_COUNT_AT);
                    if (budget_left < nodes)
                    {
                        return fault(ERB_ERR_BUDGET, "instruction budget exceeded", NULL);
                    }
                    budget_left -= nodes;
                }
                ERNode* built = NULL;
                status = erb_rt_build(
                    ERB_LOW16(extension), parent, before, index == ERB_NO_REGISTER ? 0 : registers[index], &built);
                erb_rt_set_node(&registers[operand_a], built);
                break;
            }
            case ERB_OP_DESTROY:
            {
                ERNode* node = erb_rt_node(&registers[operand_a]);
                if (node)
                {
                    erb_rt_destroy(node);
                }
                else
                {
                    status = stale();
                }
                break;
            }
            case ERB_OP_SCROLL:
            case ERB_OP_FOCUS:
            case ERB_OP_INPUT:
            case ERB_OP_VCOMMIT:
            case ERB_OP_VDIRTY:
            {
                ERNode* node = erb_rt_node(&registers[operand_a]);
                if (!node)
                {
                    status = stale();
                    break;
                }
                if (ERB_OP(word) == ERB_OP_SCROLL)
                {
                    er_scroll_view_set_offset(node, get_float(&registers[operand_b]), get_float(&registers[operand_c]));
                }
                else if (ERB_OP(word) == ERB_OP_FOCUS)
                {
                    er_text_input_focus(node);
                }
                else if (ERB_OP(word) == ERB_OP_INPUT)
                {
                    const uint8_t* slot = erb_slot(program, operand_bx);
                    const char* text = er_text_input_get_text(node);
                    store_string(slot, (char*)slot_data(slot), erb_u16(slot + ERB_SLOT_CAP_AT), text ? text : "");
                }
                else if (ERB_OP(word) == ERB_OP_VCOMMIT)
                {
                    erb_rt_commit_tape(node, operand_bx, tape);
                }
                else
                {
                    er_node_set_vector_dirty_rect(node,
                                                  registers[operand_b],
                                                  registers[operand_b + 1],
                                                  registers[operand_b + 2],
                                                  registers[operand_b + 3]);
                }
                break;
            }
            case ERB_OP_BLUR:
                er_text_input_blur();
                break;

            /* Timers */
            case ERB_OP_TSTART:
                registers[operand_a] =
                    erb_rt_timer_start((uint16_t)code[pc + 1u], registers[operand_b], operand_c != 0u);
                if (registers[operand_a] < 0)
                {
                    report("timer table full; the timer didn't start (raise ER_AOT_MAX_TIMERS)");
                }
                next = pc + 2u;
                break;
            case ERB_OP_TCLEAR:
                erb_rt_timer_clear(registers[operand_a]);
                break;

            /* Animation */
            case ERB_OP_ASTART:
            {
                const uint32_t extension = code[pc + 1u];
                const uint8_t to_first = ERB_EXT_OPERAND(extension);
                registers[operand_a] = (int32_t)erb_rt_anim_start(
                    operand_bx, ERB_LOW16(extension), to_first == ERB_NO_REGISTER ? NULL : &registers[to_first]);
                if (registers[operand_a] == 0)
                {
                    report("too many running animations; one didn't start (raise the anims region)");
                }
                next = pc + 2u;
                break;
            }
            case ERB_OP_ASTOP:
                erb_rt_anim_stop((uint32_t)registers[operand_a]);
                break;
            case ERB_OP_AVSET:
            case ERB_OP_AVGET:
            {
                uint32_t handle;
                memcpy(&handle, slot_data(erb_slot(program, operand_bx)), sizeof(handle));
                if (ERB_OP(word) == ERB_OP_AVSET)
                {
                    er_anim_value_set((ERAnimValueHandle)handle, get_float(&registers[operand_a]));
                }
                else
                {
                    set_float(&registers[operand_a], er_anim_value_get((ERAnimValueHandle)handle));
                }
                break;
            }

            /* Vectors */
            case ERB_OP_VTAPE:
            {
                const uint8_t* record_tape = erb_tape(program, operand_bx);
                memcpy(tape,
                       program->tapes + erb_u32(record_tape + ERB_TAPE_FLOATS_AT),
                       erb_u16(record_tape + ERB_TAPE_FLOAT_COUNT_AT) * sizeof(float));
                break;
            }
            case ERB_OP_VSET:
                tape[operand_bx] = get_float(&registers[operand_a]);
                break;

            /* Event data and clocks */
            case ERB_OP_EVT:
            {
                const ErbEvent* event = &g_erb.event;
                switch (operand_b)
                {
                    case ERB_EVT_X:
                    case ERB_EVT_LAYOUT_X:
                        registers[operand_a] = event->x;
                        break;
                    case ERB_EVT_Y:
                    case ERB_EVT_LAYOUT_Y:
                        registers[operand_a] = event->y;
                        break;
                    case ERB_EVT_DX:
                    case ERB_EVT_LAYOUT_W:
                        registers[operand_a] = event->dx;
                        break;
                    case ERB_EVT_DY:
                    case ERB_EVT_LAYOUT_H:
                        registers[operand_a] = event->dy;
                        break;
                    case ERB_EVT_VX:
                    case ERB_EVT_SCROLL_X:
                        set_float(&registers[operand_a], event->motion_x);
                        break;
                    case ERB_EVT_VY:
                    case ERB_EVT_SCROLL_Y:
                        set_float(&registers[operand_a], event->motion_y);
                        break;
                    case ERB_EVT_VALUE:
                        set_float(&registers[operand_a], event->value);
                        break;
                    case ERB_EVT_VALUE_START:
                        set_float(&registers[operand_a], event->value_start);
                        break;
                    default:
                        registers[operand_a] = event->finished;
                        break;
                }
                break;
            }
            case ERB_OP_NOW:
                set_int64(&registers[operand_a], (int64_t)er_now_ms64());
                break;
            case ERB_OP_DATENOW:
                set_int64(&registers[operand_a],
                          g_erb.wall_clock_base + (int64_t)(er_now_ms64() - g_erb.wall_clock_set_ms));
                break;

            /* Faults and effects */
            case ERB_OP_TRAP:
                return fault(ERB_ERR_TRAP,
                             "the app stopped itself",
                             operand_bx == ERB_NONE ? NULL : erb_string(program, operand_bx));
            case ERB_OP_EFFECT:
                status = erb_rt_queue_effect(operand_bx);
                break;

            default:
                return fault(ERB_ERR_BYTECODE, "unverified opcode", NULL);
        }
        if (status != ERB_OK)
        {
            return status;
        }
        pc = next;
    }
}
