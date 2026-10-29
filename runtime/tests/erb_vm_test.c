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
 * The Flow B VM and runtime, on the engine with a null backend: arithmetic with the C output's rules, loops,
 * calls, text, lists, props, templates, events, timers, effects, animations, inputs, reloads and faults.
 */

#include "er_node_internal.h"
#include "erb_internal.h"
#include "erb_package.h"
#include "native_renderer.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Harness
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_failed = 0;
static int s_checked = 0;

static uint32_t s_package[8192];
static uint32_t s_arena[1024];
static TPkg s_pkg;
static ErbConfig s_config;
static ErbError s_error;
static ErbStatus s_status;
static char s_log[512];
static bool s_native_fails;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        s_checked++;                                                                                                   \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, (message));                                                         \
            s_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool sensors_read(const int32_t* args, int32_t* result, void* context)
{
    (void)context;
    *result = args[0] + 1;
    return !s_native_fails;
}

static const ErbNative s_natives[] = {{"sensors.read", 1, "i:i", sensors_read}};

static void log_line(const char* line, void* context)
{
    (void)context;
    strncat(s_log, line, sizeof(s_log) - strlen(s_log) - 2u);
    strcat(s_log, "\n");
}

static TPkg* base(void)
{
    erb_unload();
    tpkg_base(&s_pkg);
    memset(&s_config, 0, sizeof(s_config));
    s_config.arena = s_arena;
    s_config.arena_bytes = sizeof(s_arena);
    s_config.screen_w = TPKG_SCREEN;
    s_config.screen_h = TPKG_SCREEN;
    s_config.natives = s_natives;
    s_config.native_count = 1;
    s_config.log = log_line;
    s_log[0] = '\0';
    s_native_fails = false;
    return &s_pkg;
}

static bool load(void)
{
    const uint32_t length = tpkg_build(&s_pkg, (uint8_t*)s_package);
    const bool ok = erb_load(s_package, length, &s_config, &s_error);
    if (!ok && s_error.status != ERB_ERR_UPDATE_LOOP)
    {
        printf("  (load: %s, %s)\n", erb_status_str(s_error.status), s_error.detail ? s_error.detail : "");
    }
    return ok;
}

static uint8_t* slot(uint16_t index)
{
    return (uint8_t*)s_arena + s_pkg.slots[index].offset;
}

static int32_t slot_int(uint16_t index)
{
    int32_t value;
    memcpy(&value, slot(index), sizeof(value));
    return value;
}

static ERNode* slot_node(uint16_t index, uint32_t element)
{
    int32_t cells[2];
    memcpy(cells, slot(index) + element * 8u, sizeof(cells));
    return erb_rt_node(cells);
}

static uint16_t
helper(const uint8_t* types, uint8_t registers, uint8_t params, uint8_t result, const uint32_t* code, uint32_t words)
{
    return tpkg_function(&s_pkg, ERB_FN_HELPER, params, result, types, registers, code, words);
}

/** @brief Runs a function directly, as the runtime would (the VM marked as running). */
static int32_t call(uint16_t function, int32_t first, int32_t second)
{
    int32_t args[2] = {first, second};
    int32_t result = 0;
    g_erb.in_vm = true;
    s_status = erb_vm_run(function, args, &result);
    g_erb.in_vm = false;
    return result;
}

static int64_t call64(uint16_t function, int64_t first, int64_t second)
{
    int32_t args[4] = {(int32_t)(uint32_t)first,
                       (int32_t)((uint64_t)first >> 32),
                       (int32_t)(uint32_t)second,
                       (int32_t)((uint64_t)second >> 32)};
    int32_t result[2] = {0, 0};
    g_erb.in_vm = true;
    s_status = erb_vm_run(function, args, result);
    g_erb.in_vm = false;
    return (int64_t)((uint64_t)(uint32_t)result[0] | ((uint64_t)(uint32_t)result[1] << 32));
}

static int32_t bits(float value)
{
    int32_t cell;
    memcpy(&cell, &value, sizeof(cell));
    return cell;
}

static float as_float(int32_t cell)
{
    float value;
    memcpy(&value, &cell, sizeof(value));
    return value;
}

static uint32_t add_const(float value)
{
    memcpy(&s_pkg.const32[s_pkg.const32_count], &value, sizeof(value));
    return s_pkg.const32_count++;
}

static uint16_t add_string(const char* text)
{
    s_pkg.strings[s_pkg.string_count] = text;
    return (uint16_t)s_pkg.string_count++;
}

/** @brief Fires a node's event handler the way the engine would. */
static void fire(ERNode* node, EREventType event)
{
    EREventData data;
    memset(&data, 0, sizeof(data));
    data.x = 12;
    node->events[event].fn(node, &data, node->events[event].user_data);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Arithmetic
 ---------------------------------------------------------------------------------------------------------------------*/

static uint16_t binop(uint8_t op, uint8_t type)
{
    const uint8_t types[] = {type, type};
    const uint32_t code[] = {ERB_ABC(op, 0, 0, 1), OP_ABC(RET, 0, 0, 0)};
    return helper(types, 2, 2, type, code, 2);
}

static void test_int_arithmetic(void)
{
    base();
    const uint16_t add = binop(ERB_OP_ADD, ERB_T_INT);
    const uint16_t sub = binop(ERB_OP_SUB, ERB_T_INT);
    const uint16_t mul = binop(ERB_OP_MUL, ERB_T_INT);
    const uint16_t div = binop(ERB_OP_DIV, ERB_T_INT);
    const uint16_t floordiv = binop(ERB_OP_FLOORDIV, ERB_T_INT);
    const uint16_t ceildiv = binop(ERB_OP_CEILDIV, ERB_T_INT);
    const uint16_t rounddiv = binop(ERB_OP_ROUNDDIV, ERB_T_INT);
    const uint16_t mod = binop(ERB_OP_MOD, ERB_T_INT);
    const uint16_t neg = binop(ERB_OP_NEG, ERB_T_INT);
    const uint16_t abs_op = binop(ERB_OP_ABS, ERB_T_INT);
    CHECK(load(), "loads");

    CHECK(call(add, INT_MAX, 1) == INT_MAX, "+ saturates up");
    CHECK(call(sub, INT_MIN, 1) == INT_MIN, "- saturates down");
    CHECK(call(mul, 1 << 20, 1 << 20) == INT_MAX, "* saturates");
    CHECK(call(mul, -(1 << 20), 1 << 20) == INT_MIN, "* saturates by sign");
    CHECK(call(div, 7, -2) == -3, "/ truncates toward zero");
    CHECK(call(div, 5, 0) == INT_MAX && call(div, -5, 0) == INT_MIN && call(div, 0, 0) == 0, "/ 0 by the sign");
    CHECK(call(div, INT_MIN, -1) == INT_MAX, "INT_MIN / -1 saturates");
    CHECK(call(floordiv, -7, 2) == -4 && call(floordiv, 7, 2) == 3, "floor of a quotient");
    CHECK(call(ceildiv, 7, 2) == 4 && call(ceildiv, -7, 2) == -3, "ceil of a quotient");
    CHECK(call(rounddiv, 5, 2) == 3 && call(rounddiv, -5, 2) == -2 && call(rounddiv, 7, -2) == -3,
          "round of a quotient, halves up");
    CHECK(call(floordiv, INT_MIN, -1) == INT_MAX, "floor of INT_MIN / -1 saturates");
    CHECK(call(mod, -7, 2) == -1 && call(mod, 7, 0) == 0 && call(mod, INT_MIN, -1) == 0, "% as JS keeps it");
    CHECK(call(neg, INT_MIN, 0) == INT_MAX && call(abs_op, INT_MIN, 0) == INT_MAX, "-INT_MIN and abs saturate");
}

static void test_int64_arithmetic(void)
{
    base();
    const uint8_t types[] = {ERB_T_INT64, ERB_T_PAIR, ERB_T_INT64, ERB_T_PAIR};
    const uint32_t add_code[] = {OP_ABC(ADDL, 0, 0, 2), OP_ABC(RET, 0, 0, 0)};
    const uint32_t round_code[] = {OP_ABC(ROUNDDIVL, 0, 0, 2), OP_ABC(RET, 0, 0, 0)};
    const uint32_t floor_code[] = {OP_ABC(FLOORDIVL, 0, 0, 2), OP_ABC(RET, 0, 0, 0)};
    const uint16_t add = helper(types, 4, 4, ERB_T_INT64, add_code, 2);
    const uint16_t round = helper(types, 4, 4, ERB_T_INT64, round_code, 2);
    const uint16_t floor_op = helper(types, 4, 4, ERB_T_INT64, floor_code, 2);
    CHECK(load(), "loads");
    CHECK(call64(add, INT64_MAX, 1) == INT64_MAX, "64-bit + saturates");
    CHECK(call64(add, 1700000000000LL, 1) == 1700000000001LL, "64-bit + of a timestamp");
    CHECK(call64(round, -5, 2) == -2 && call64(round, 5, 2) == 3, "64-bit round of a quotient");
    CHECK(call64(floor_op, INT64_MIN, -1) == INT64_MAX && call64(floor_op, -7, 2) == -4, "64-bit floor");
}

static void test_float_rules(void)
{
    base();
    const uint8_t to_int_types[] = {ERB_T_FLOAT, ERB_T_INT};
    const uint32_t to_int[] = {OP_ABC(FTOI, 1, 0, 0), OP_ABC(RET, 1, 0, 0)};
    const uint16_t ftoi = helper(to_int_types, 2, 1, ERB_T_INT, to_int, 2);
    const uint8_t math_types[] = {ERB_T_FLOAT, ERB_T_FLOAT};
    const uint32_t round_code[] = {OP_ABC(FMATH, 0, 0, 1), ERB_MATH_ROUND, OP_ABC(RET, 0, 0, 0)};
    const uint32_t min_code[] = {OP_ABC(FMATH, 0, 0, 1), ERB_MATH_MIN, OP_ABC(RET, 0, 0, 0)};
    const uint32_t div_code[] = {OP_ABC(FDIV, 0, 0, 1), OP_ABC(RET, 0, 0, 0)};
    const uint16_t round = helper(math_types, 2, 2, ERB_T_FLOAT, round_code, 3);
    const uint16_t min = helper(math_types, 2, 2, ERB_T_FLOAT, min_code, 3);
    const uint16_t fdiv = helper(math_types, 2, 2, ERB_T_FLOAT, div_code, 2);
    CHECK(load(), "loads");
    CHECK(call(ftoi, bits(NAN), 0) == 0, "NaN to int is 0");
    CHECK(call(ftoi, bits(3e10f), 0) == INT_MAX && call(ftoi, bits(-3e10f), 0) == INT_MIN, "float to int saturates");
    CHECK(call(ftoi, bits(-2.7f), 0) == -2, "float to int truncates");
    CHECK(as_float(call(round, bits(2.5f), 0)) == 3.0f && as_float(call(round, bits(-2.5f), 0)) == -2.0f,
          "Math.round rounds halves up");
    CHECK(isnan(as_float(call(min, bits(NAN), bits(1.0f)))), "Math.min with NaN is NaN");
    CHECK(isinf(as_float(call(fdiv, bits(1.0f), bits(0.0f)))), "float division by zero is infinite, not a fault");
    CHECK(s_status == ERB_OK, "arithmetic never faults");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Loops, calls and natives
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_loops_and_calls(void)
{
    base();
    const uint8_t types[] = {ERB_T_INT, ERB_T_INT, ERB_T_INT};
    const uint32_t loop_code[] = {
        OP_ABX(LOADI, 2, 1),
        OP_ABX(LOOPSET, 1, 10),
        OP_ABC(ADD, 0, 0, 2),
        OP_ABX(LOOP, 1, -2),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t loop = helper(types, 3, 1, ERB_T_INT, loop_code, 5);
    const uint32_t once_code[] = {
        OP_ABX(LOADI, 2, 1),
        OP_ABX(LOOPSET, 1, 0),
        OP_ABC(ADD, 0, 0, 2),
        OP_ABX(LOOP, 1, -2),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t once = helper(types, 3, 1, ERB_T_INT, once_code, 5);
    const uint32_t native_code[] = {OP_ABX(CALLN, 0, 0), OP_ABC(RET, 0, 0, 0)};
    const uint16_t native = helper(types, 1, 1, ERB_T_INT, native_code, 2);
    CHECK(load(), "loads");
    CHECK(call(loop, 0, 0) == 10, "a loop runs its bound");
    CHECK(call(once, 0, 0) == 1, "a loop with bound 0 runs its body once");
    CHECK(call(native, 41, 0) == 42, "a native import is called with its arguments");
    s_native_fails = true;
    call(native, 41, 0);
    CHECK(s_status == ERB_ERR_NATIVE && g_erb.error.name && strcmp(g_erb.error.name, "sensors.read") == 0,
          "a native that fails stops the app, naming the module");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Text
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_text(void)
{
    base();
    const uint8_t types[] = {ERB_T_INT, ERB_T_FLOAT, ERB_T_BOOL, ERB_T_ENUM};
    const uint32_t mixed_code[] = {
        OP_ABX(LOADI, 0, 7),
        OP_ABC(FMTI, 0, 0, 0),
        OP_ABC(FMTPAD, 3, '0', 0),
        OP_ABX(LOADE, 3, 1),
        0u,
        OP_ABX(FMTE, 3, 0),
        OP_ABX(LOADI, 2, 1),
        OP_ABC(FMTB, 2, 0, 0),
        OP_ABX(FMTSET, 0, S_LABEL),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t mixed = helper(types, 4, 0, 0, mixed_code, 10);
    const uint32_t pi_constant = add_const(3.14159f);
    const uint32_t nan_bits = add_const(NAN);
    const uint32_t neg_zero = add_const(-0.0f);
    const uint32_t floats_code[] = {
        OP_ABX(LOADK, 1, pi_constant),
        OP_ABC(FMTFX, 1, 2, 0),
        OP_ABX(LOADK, 1, nan_bits),
        OP_ABC(FMTF, 1, 0, 0),
        OP_ABX(LOADK, 1, neg_zero),
        OP_ABC(FMTF, 1, 0, 0),
        OP_ABX(GET32, 1, S_SPEED),
        OP_ABC(FMTF, 1, 0, 0),
        OP_ABX(FMTSET, 0, S_LABEL),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t floats = helper(types, 4, 0, 0, floats_code, 10);
    const uint16_t long_text = add_string("a string longer than its cap");
    const uint32_t long_code[] = {OP_ABX(FMTK, 0, long_text), OP_ABX(FMTSET, 0, S_LABEL), OP_ABC(RET, 0, 0, 0)};
    const uint16_t too_long = helper(types, 4, 0, 0, long_code, 3);
    CHECK(load(), "loads");
    CHECK(strcmp((const char*)slot(S_LABEL), "hello") == 0, "a string slot starts at its initial value");
    call(mixed, 0, 0);
    CHECK(strcmp((const char*)slot(S_LABEL), "007offtrue") == 0, "padStart, an enum's string and a boolean word");
    call(floats, 0, 0);
    CHECK(strcmp((const char*)slot(S_LABEL), "3.14NaN01.5") == 0, "toFixed, NaN, -0 and a float as JS prints them");
    call(too_long, 0, 0);
    CHECK(strcmp((const char*)slot(S_LABEL), "a string longer") == 0, "a string is cut at its slot's cap");
    CHECK(strstr(s_log, "cut short") != NULL, "and the cut is reported");
    const size_t logged = strlen(s_log);
    call(too_long, 0, 0);
    CHECK(strlen(s_log) == logged, "once per place");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Lists
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_lists(void)
{
    base();
    const uint8_t types[] = {ERB_T_INT, ERB_T_FLOAT, ERB_T_INT, ERB_T_INT};
    const uint32_t speed = add_const(2.5f);
    const uint32_t fill_code[] = {
        OP_ABX(LOADK, 1, speed),
        OP_ABX(LOADI, 0, 10),
        OP_ABX(APPEND, 0, S_ITEMS),
        OP_ABX(LOADI, 0, 20),
        OP_ABX(APPEND, 0, S_ITEMS),
        OP_ABX(LOADI, 0, 30),
        OP_ABX(APPEND, 0, S_ITEMS),
        OP_ABX(LOADI, 2, 1),
        OP_ABC(ROWGET, 3, 2, 0),
        S_ITEMS,
        OP_ABC(RET, 3, 0, 0),
    };
    const uint16_t fill = helper(types, 4, 0, ERB_T_INT, fill_code, 11);
    const uint32_t len_code[] = {OP_ABX(LEN, 0, S_ITEMS), OP_ABC(RET, 0, 0, 0)};
    const uint16_t len = helper(types, 4, 0, ERB_T_INT, len_code, 2);
    const uint32_t pop_code[] = {OP_ABX(POP, 0, S_ITEMS), OP_ABX(LEN, 0, S_ITEMS), OP_ABC(RET, 0, 0, 0)};
    const uint16_t pop = helper(types, 4, 0, ERB_T_INT, pop_code, 3);
    const uint32_t slice_code[] = {
        OP_ABX(LOADI, 2, -1),
        OP_ABX(LOADI, 3, 9),
        OP_ABC(SLICE, 2, 3, 0),
        S_ITEMS,
        OP_ABX(LOADI, 2, 0),
        OP_ABC(ROWGET, 0, 2, 0),
        S_ITEMS,
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t slice = helper(types, 4, 0, ERB_T_INT, slice_code, 8);
    const uint32_t past_code[] = {OP_ABX(LOADI, 2, 9), OP_ABC(ROWGET, 0, 2, 0), S_ITEMS, OP_ABC(RET, 0, 0, 0)};
    const uint16_t past = helper(types, 4, 0, ERB_T_INT, past_code, 4);
    CHECK(load(), "loads");
    CHECK(call(fill, 0, 0) == 20, "rows append and read back");
    CHECK(call(len, 0, 0) == 3, "LEN counts them");
    CHECK(call(pop, 0, 0) == 2, "POP drops the last");
    CHECK(call(slice, 0, 0) == 20 && call(len, 0, 0) == 1, "SLICE(-1, 9) keeps the last row, as Array.slice does");
    call(fill, 0, 0);
    call(fill, 0, 0);
    CHECK(call(len, 0, 0) == 4, "a full list keeps its cap");
    CHECK(strstr(s_log, "list full") != NULL, "and the dropped row is reported");
    call(past, 0, 0);
    CHECK(s_status == ERB_ERR_LIST_INDEX, "a read past the end stops the app");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Props and templates
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_props(void)
{
    base();
    const uint8_t types[] = {ERB_T_FLOAT, ERB_T_INT, ERB_T_NODE, ERB_T_PAIR};
    const uint32_t angle = add_const(45.0f);
    const uint32_t code[] = {
        OP_ABX(LOADK, 0, angle),
        OP_ABX(PROPS, 0, ERB_NONE),
        OP_ABX(PSETF, 0, PROP_ROTATE_Z),
        OP_ABX(GET64, 2, S_ROOT),
        OP_ABC(COMMIT, 2, 0, 0),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t rotate = helper(types, 4, 0, 0, code, 6);
    CHECK(load(), "loads");
    call(rotate, 0, 0);
    CHECK(slot_node(S_ROOT, 0)->tp_rotate_z == 45.0f, "PSETF and COMMIT reach the node");

    erb_rt_props(ERB_NONE);
    erb_rt_pset_float(PROP_WIDTH, 10.5f);
    CHECK(g_erb_props.width == 11, "a dimension rounds halves up");
    erb_rt_pset_float(PROP_WIDTH, -10.5f);
    CHECK(g_erb_props.width == -10, "a negative half rounds up too");
    erb_rt_pset_float(PROP_WIDTH, NAN);
    CHECK(g_erb_props.width == 0, "a NaN dimension is 0");
    erb_rt_pset_float(PROP_WIDTH, 1e9f);
    CHECK(g_erb_props.width == INT16_MAX, "a dimension clamps to int16");
    erb_rt_pset_float(PROP_OPACITY, 0.5f);
    CHECK(g_erb_props.opacity == 128, "an opacity becomes the engine's byte");
    erb_rt_pset_float(PROP_OPACITY, 1.5f);
    CHECK(g_erb_props.opacity == ERB_OPACITY_OPAQUE, "an opacity past 1 is opaque");
    erb_rt_pset_int(PROP_FLEX_DIRECTION, 9);
    CHECK(g_erb_props.flex_direction == ER_FLEX_COL_REVERSE, "an enum prop clamps to its range");
    CHECK(!erb_rt_pset_string(PROP_IMAGE_NAME, "missing"), "an image that isn't registered is reported");
    CHECK(erb_rt_pset_string(PROP_IMAGE_NAME, "img"), "one that is isn't");
}

static void test_templates(void)
{
    base();
    const uint8_t types[] = {ERB_T_NODE, ERB_T_PAIR, ERB_T_NODE, ERB_T_PAIR, ERB_T_INT};
    const uint32_t build_code[] = {
        OP_ABX(LOADI, 4, 2),
        OP_ABX(GET64, 2, S_ROOT),
        OP_ABC(BUILD, 0, 2, 2),
        ERB_PACK16(1u, 4u), /* template 1, instance index in r4 */
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t build = helper(types, 5, 0, 0, build_code, 5);
    const uint32_t destroy_code[] = {
        OP_ABX(LOADI, 4, 2),
        OP_ABC(GETNI, 0, 4, 0),
        S_ROWS,
        OP_ABC(DESTROY, 0, 0, 0),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t destroy = helper(types, 5, 0, 0, destroy_code, 5);
    const uint32_t commit_code[] = {
        OP_ABX(LOADI, 4, 2),
        OP_ABC(GETNI, 0, 4, 0),
        S_ROWS,
        OP_ABX(PROPS, 0, ERB_NONE),
        OP_ABC(COMMIT, 0, 0, 0),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t commit = helper(types, 5, 0, 0, commit_code, 6);
    const uint32_t past_code[] = {
        OP_ABX(LOADI, 4, 4),
        OP_ABX(GET64, 2, S_ROOT),
        OP_ABC(BUILD, 0, 2, 2),
        ERB_PACK16(1u, 4u), /* template 1, instance index in r4 */
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t build_past = helper(types, 5, 0, 0, past_code, 5);
    CHECK(load(), "loads");

    CHECK(er_node_in_use_count() == 4, "the root template's four nodes are built");
    ERNode* root = slot_node(S_ROOT, 0);
    CHECK(root && er_node_get_type(root) == ER_NODE_VIEW, "the root's handle is kept in its slot");
    CHECK(er_node_get_type(er_node_first_child(root)) == ER_NODE_TEXT, "children are built in order");
    CHECK(slot_int(S_READY) == 1, "the boot function ran");
    CHECK(slot_int(S_FADE) != 0, "an animated value was created for its slot");

    call(build, 0, 0);
    CHECK(s_status == ERB_OK && er_node_in_use_count() == 5, "BUILD adds a row instance");
    CHECK(slot_node(S_ROWS, 2) && er_node_parent(slot_node(S_ROWS, 2)) == root,
          "its handle goes in the row's element of the handle array, under the parent");
    call(destroy, 0, 0);
    CHECK(s_status == ERB_OK && er_node_in_use_count() == 4, "DESTROY removes it");
    call(commit, 0, 0);
    CHECK(s_status == ERB_OK && strstr(s_log, "stale node handle") != NULL,
          "a release package skips a stale handle and reports it");
    call(build_past, 0, 0);
    CHECK(s_status == ERB_ERR_LIST_INDEX, "a row index past the handle array stops the app");

    base()->flags = ERB_FLAG_DEBUG;
    const uint16_t debug_build = helper(types, 5, 0, 0, build_code, 5);
    const uint16_t debug_destroy = helper(types, 5, 0, 0, destroy_code, 5);
    const uint16_t debug_commit = helper(types, 5, 0, 0, commit_code, 6);
    CHECK(load(), "a debug package loads");
    call(debug_build, 0, 0);
    call(debug_destroy, 0, 0);
    call(debug_commit, 0, 0);
    CHECK(s_status == ERB_ERR_STALE_NODE, "a debug package stops on a stale handle");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Events, inputs and updates
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief An update that writes the count into the label whenever bit 0 is dirty. */
static void count_to_label_update(void)
{
    const uint8_t types[] = {ERB_T_INT};
    const uint32_t code[] = {
        OP_ABX(JDIRTY, 0, 1),
        1u,
        OP_ABC(RET, 0, 0, 0),
        OP_ABX(GET32, 0, S_COUNT),
        OP_ABC(FMTI, 0, 0, 0),
        OP_ABX(FMTSET, 0, S_LABEL),
        OP_ABC(RET, 0, 0, 0),
    };
    tpkg_set_code(&s_pkg, F_UPDATE, types, 1, code, 7);
}

static void test_events_and_inputs(void)
{
    base();
    count_to_label_update();
    CHECK(load(), "loads");
    CHECK(strcmp((const char*)slot(S_LABEL), "5") == 0, "the first update gives every state-driven value");

    fire(slot_node(S_ROOT, 0), ER_EVENT_PRESS);
    CHECK(slot_int(S_COUNT) == 12, "a press runs its handler, which calls a helper");
    CHECK(strcmp((const char*)slot(S_LABEL), "12") == 0, "then the update re-sets what read the changed slot");

    const int steps = erb_input_find("steps");
    CHECK(steps == 0 && erb_input_find("nope") == -1, "inputs are found by name");
    CHECK(erb_input_set_int(steps, 40) && slot_int(S_COUNT) == 40, "a host input sets its slot");
    CHECK(strcmp((const char*)slot(S_LABEL), "40") == 0, "and runs the update");
    CHECK(!erb_input_set_float(steps, 1.0f), "an input of another type is refused");

    const uint8_t types[] = {ERB_T_INT64, ERB_T_PAIR};
    const uint32_t code[] = {OP_ABC(DATENOW, 0, 0, 0), OP_ABC(RET, 0, 0, 0)};
    base();
    const uint16_t date = helper(types, 2, 0, ERB_T_INT64, code, 2);
    CHECK(load(), "loads");
    erb_set_wall_clock(1700000000000LL);
    CHECK(call64(date, 0, 0) >= 1700000000000LL, "DATENOW reads the wall clock");
}

static void test_queries(void)
{
    base();
    /* Says yes while no finger is down. */
    const uint8_t types[] = {ERB_T_BOOL, ERB_T_INT, ERB_T_INT};
    const uint32_t code[] = {
        OP_ABC(EVT, 1, ERB_EVT_TOUCHES, 0),
        OP_ABX(LOADI, 2, 0),
        OP_ABC(EQ, 0, 1, 2),
        OP_ABC(RET, 0, 0, 0),
    };
    const uint16_t query =
        tpkg_function(&s_pkg, ERB_FN_QUERY + ER_QUERY_START_SHOULD_SET, 0, ERB_T_BOOL, types, 3, code, 4);
    s_pkg.bindings[3] = s_pkg.bindings[0];
    s_pkg.bindings[4] = (TBinding){ERB_BIND_QUERY, ER_QUERY_START_SHOULD_SET, query};
    s_pkg.binding_count = 5;
    s_pkg.nodes[0].bindings = 3;
    s_pkg.nodes[0].binding_count = 2;
    CHECK(load(), "loads");
    ERNode* root = slot_node(S_ROOT, 0);
    EREventData data;
    memset(&data, 0, sizeof(data));
    const ERResponderQueryHandler* handler = &root->queries[ER_QUERY_START_SHOULD_SET];
    CHECK(handler->fn && handler->fn(root, &data, handler->user_data), "a responder query answers from its function");
}

static void test_timers_and_effects(void)
{
    base();
    {
        const uint8_t types[] = {ERB_T_BOOL, ERB_T_INT, ERB_T_INT};
        const uint32_t boot[] = {
            OP_ABX(LOADI, 0, 1),
            OP_ABX(SET32, 0, S_READY),
            OP_ABX(LOADI, 2, 100),
            OP_ABC(TSTART, 1, 2, 1),
            F_TIMER,
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_BOOT, types, 3, boot, 6);
        const uint8_t timer_types[] = {ERB_T_INT, ERB_T_INT};
        const uint32_t timer[] = {
            OP_ABX(GET32, 0, S_COUNT),
            OP_ABX(LOADI, 1, 1),
            OP_ABC(ADD, 0, 0, 1),
            OP_ABX(SET32, 0, S_COUNT),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_TIMER, timer_types, 2, timer, 5);
    }
    CHECK(load(), "loads");
    erb_tick(100);
    erb_tick(50);
    erb_tick(50);
    erb_tick(100);
    CHECK(slot_int(S_COUNT) == 8, "a repeating timer fires every interval");

    /* A mount effect: queued by the boot function, run after the first update. */
    base();
    {
        const uint32_t boot[] = {OP_ABX(EFFECT, 0, F_EFFECT), OP_ABC(RET, 0, 0, 0)};
        tpkg_set_code(&s_pkg, F_BOOT, NULL, 0, boot, 2);
        const uint8_t types[] = {ERB_T_STRING};
        const uint32_t effect[] = {OP_ABX(LOADS, 0, STR_ON), OP_ABX(SETS, 0, S_LABEL), OP_ABC(RET, 0, 0, 0)};
        tpkg_set_code(&s_pkg, F_EFFECT, types, 1, effect, 3);
    }
    CHECK(load(), "loads");
    CHECK(strcmp((const char*)slot(S_LABEL), "on") == 0, "a mount effect runs at load");

    /* An effect that keeps setting what its update depends on is an update loop. */
    base();
    {
        const uint32_t update[] = {
            OP_ABX(JDIRTY, 0, 1),
            1u,
            OP_ABC(RET, 0, 0, 0),
            OP_ABX(EFFECT, 0, F_EFFECT),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_UPDATE, NULL, 0, update, 5);
        const uint8_t types[] = {ERB_T_INT, ERB_T_INT};
        const uint32_t effect[] = {
            OP_ABX(GET32, 0, S_COUNT),
            OP_ABX(LOADI, 1, 1),
            OP_ABC(ADD, 0, 0, 1),
            OP_ABX(SET32, 0, S_COUNT),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_EFFECT, types, 2, effect, 5);
    }
    CHECK(!load() && s_error.status == ERB_ERR_UPDATE_LOOP, "more than 50 update passes is a fault");
    CHECK(!erb_running() && er_node_in_use_count() == 3, "and the error screen replaces the app");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Animations
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_animations(void)
{
    base();
    const uint16_t press = F_PRESS;
    {
        const uint8_t types[] = {ERB_T_ANIM};
        const uint32_t start[] = {
            OP_ABX(ASTART, 0, 0), ERB_PACK16(F_DONE, ERB_NO_REGISTER), OP_ABX(SET32, 0, S_ANIM), OP_ABC(RET, 0, 0, 0)};
        tpkg_set_code(&s_pkg, press, types, 1, start, 4);
        const uint8_t done_types[] = {ERB_T_BOOL, ERB_T_INT};
        const uint32_t done[] = {
            OP_ABC(EVT, 0, ERB_EVT_FINISHED, 0),
            OP_ABX(LOADI, 1, 100),
            OP_ABX(JF, 0, 1),
            OP_ABX(LOADI, 1, 200),
            OP_ABX(SET32, 1, S_COUNT),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_DONE, done_types, 2, done, 6);
        /* A long press stops it. */
        const uint8_t stop_types[] = {ERB_T_ANIM};
        const uint32_t stop[] = {OP_ABX(GET32, 0, S_ANIM), OP_ABC(ASTOP, 0, 0, 0), OP_ABC(RET, 0, 0, 0)};
        const uint16_t stop_fn = tpkg_function(&s_pkg, ER_EVENT_LONG_PRESS, 0, 0, stop_types, 1, stop, 3);
        s_pkg.bindings[3] = s_pkg.bindings[0];
        s_pkg.bindings[4] = (TBinding){ERB_BIND_EVENT, ER_EVENT_LONG_PRESS, stop_fn};
        s_pkg.binding_count = 5;
        s_pkg.nodes[0].bindings = 3;
        s_pkg.nodes[0].binding_count = 2;
    }
    s_pkg.has_debug = false;
    CHECK(load(), "loads");
    ERNode* root = slot_node(S_ROOT, 0);
    uint32_t fade;
    memcpy(&fade, slot(S_FADE), sizeof(fade));

    fire(root, ER_EVENT_PRESS);
    CHECK(slot_int(S_ANIM) != 0, "ASTART gives a handle");
    for (int step = 0; step < 4; step++)
    {
        embedded_renderer_tick(40);
    }
    CHECK(er_anim_value_get((ERAnimValueHandle)fade) == 1.0f, "a sequence's timing leaf drives the animated value");
    CHECK(slot_int(S_COUNT) == 200, "the completion function runs, finished");

    er_anim_value_set((ERAnimValueHandle)fade, 0.0f);
    fire(root, ER_EVENT_PRESS);
    embedded_renderer_tick(20);
    fire(root, ER_EVENT_LONG_PRESS);
    CHECK(slot_int(S_COUNT) == 100, "ASTOP runs the completion function, not finished");
    for (int step = 0; step < 4; step++)
    {
        embedded_renderer_tick(40);
    }
    CHECK(slot_int(S_COUNT) == 100, "and nothing of the stopped tree runs on");
    CHECK(erb_running(), "the app runs on");

    /* A parallel group of a delay and a timing: done when both are, and not finished once the timing stops. */
    base();
    {
        s_pkg.anims[2] = (TAnim){ERB_ANIM_PARALLEL, 0, 0, ERB_NONE, 0, 3, 2, 0, 0, 0, {0, 0, 0, 0}};
        s_pkg.anims[3] = (TAnim){ERB_ANIM_TIMING, 0, 0, ERB_NONE, 0, 0, 0, 200, 0, 0, {0, 0, 0, 0}};
        s_pkg.anims[4] =
            (TAnim){ERB_ANIM_TIMING, ER_EASE_LINEAR, 0, S_FADE, 0, 0, 0, 50, 0, FLOAT_ONE_BITS, {0, 0, 0, 0}};
        s_pkg.anim_count = 5;
        const uint8_t types[] = {ERB_T_ANIM};
        const uint32_t start[] = {OP_ABX(ASTART, 0, 2), ERB_PACK16(F_DONE, ERB_NO_REGISTER), OP_ABC(RET, 0, 0, 0)};
        tpkg_set_code(&s_pkg, press, types, 1, start, 3);
        const uint8_t done_types[] = {ERB_T_BOOL, ERB_T_INT};
        const uint32_t done[] = {
            OP_ABC(EVT, 0, ERB_EVT_FINISHED, 0),
            OP_ABX(LOADI, 1, 100),
            OP_ABX(JF, 0, 1),
            OP_ABX(LOADI, 1, 200),
            OP_ABX(SET32, 1, S_COUNT),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_DONE, done_types, 2, done, 6);
    }
    s_pkg.has_debug = false;
    CHECK(load(), "a parallel group loads");
    root = slot_node(S_ROOT, 0);
    memcpy(&fade, slot(S_FADE), sizeof(fade));
    fire(root, ER_EVENT_PRESS);
    for (int step = 0; step < 5; step++)
    {
        embedded_renderer_tick(20);
        erb_tick(20);
    }
    CHECK(slot_int(S_COUNT) == 5, "the group waits for its delay after the timing ends");
    for (int step = 0; step < 6; step++)
    {
        embedded_renderer_tick(20);
        erb_tick(20);
    }
    CHECK(slot_int(S_COUNT) == 200, "and is done, finished, once both are");

    er_anim_value_set((ERAnimValueHandle)fade, 0.0f);
    fire(root, ER_EVENT_PRESS);
    embedded_renderer_tick(20);
    erb_tick(20);
    er_anim_value_set((ERAnimValueHandle)fade, 0.5f);
    CHECK(slot_int(S_COUNT) == 100, "a stopped child ends it at once, not finished, before its delay is up");

    /* An interpolated binding maps the value onto the prop. */
    base();
    s_pkg.bindings[3] = s_pkg.bindings[0];
    s_pkg.bindings[4] = (TBinding){ERB_BIND_INTERPOLATED, PROP_ROTATE_Z, 0};
    s_pkg.binding_count = 5;
    s_pkg.nodes[0].bindings = 3;
    s_pkg.nodes[0].binding_count = 2;
    s_pkg.has_debug = false;
    CHECK(load(), "an interpolated binding loads");
    memcpy(&fade, slot(S_FADE), sizeof(fade));
    er_anim_value_set((ERAnimValueHandle)fade, 0.5f);
    CHECK(slot_node(S_ROOT, 0)->tp_rotate_z == 45.0f, "and drives its prop through the interpolation");
    er_anim_value_set((ERAnimValueHandle)fade, 3.0f);
    CHECK(slot_node(S_ROOT, 0)->tp_rotate_z == 90.0f, "clamped past its range");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Faults and reloads
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_faults(void)
{
    base();
    {
        const uint32_t trap[] = {OP_ABX(TRAP, 0, STR_BOOM)};
        const TFunction keep = s_pkg.functions[F_PRESS];
        tpkg_set_code(&s_pkg, F_PRESS, s_pkg.types + keep.types, keep.registers, trap, 1);
    }
    CHECK(load(), "loads");
    fire(slot_node(S_ROOT, 0), ER_EVENT_PRESS);
    CHECK(!erb_running(), "a TRAP stops the app");
    erb_tick(0);
    const ErbError* error = erb_last_error();
    CHECK(error->status == ERB_ERR_TRAP && error->name && strcmp(error->name, "boom") == 0, "with the trap's reason");
    CHECK(error->file && strcmp(error->file, "App.jsx") == 0 && error->line == 41, "and the source line");
    CHECK(er_node_in_use_count() == 3, "the next tick shows the error screen");

    /* A timer that can run past the budget is counted, and stopped when it does. */
    base();
    {
        const uint8_t types[] = {ERB_T_BOOL, ERB_T_INT, ERB_T_INT};
        const uint32_t boot[] = {
            OP_ABX(LOADI, 2, 10),
            OP_ABC(TSTART, 1, 2, 0),
            F_TIMER,
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_BOOT, types, 3, boot, 4);
        const uint8_t loop_types[] = {ERB_T_INT, ERB_T_INT, ERB_T_INT};
        const uint32_t loop[] = {
            OP_ABX(LOOPSET, 0, 1000),
            OP_ABX(LOOPSET, 1, 1000),
            OP_ABX(LOADI, 2, 1),
            OP_ABX(LOOP, 1, -2),
            OP_ABX(LOOP, 0, -4),
            OP_ABC(RET, 0, 0, 0),
        };
        tpkg_set_code(&s_pkg, F_TIMER, loop_types, 3, loop, 6);
    }
    s_config.budget = 5000;
    CHECK(load(), "loads");
    erb_tick(10);
    CHECK(!erb_running() && erb_last_error()->status == ERB_ERR_BUDGET, "running out of budget stops the app");
}

static void test_reload(void)
{
    base();
    CHECK(load(), "loads");
    CHECK(er_node_in_use_count() == 4, "four nodes");
    s_pkg.major = ERB_FORMAT_MAJOR + 1u;
    static uint32_t bad[8192];
    const uint32_t bad_length = tpkg_build(&s_pkg, (uint8_t*)bad);
    ErbError error;
    CHECK(!erb_load(bad, bad_length, &s_config, &error) && error.status == ERB_ERR_NEWER_FORMAT,
          "a package that fails verification is refused");
    CHECK(erb_running() && er_node_in_use_count() == 4, "and the running app is left as it was");
    s_pkg.major = ERB_FORMAT_MAJOR;
    CHECK(load(), "a good package replaces it");
    CHECK(er_node_in_use_count() == 4, "with no nodes left over");
    erb_unload();
    CHECK(!erb_running() && er_node_in_use_count() == 0, "unloading tears everything down");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Mutations
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Nothing a package holds can crash the host: bytes of the baseline are flipped (and the container
 *        re-sealed, so they get past the CRC), and every mutant that verifies is loaded and driven.
 */
static void test_mutations(void)
{
    base();
    const uint32_t length = tpkg_build(&s_pkg, (uint8_t*)s_package);
    static uint32_t mutant[8192];
    uint32_t seed = 1u;
    uint32_t verified = 0;
    for (int iteration = 0; iteration < 3000; iteration++)
    {
        memcpy(mutant, s_package, length);
        uint8_t* bytes = (uint8_t*)mutant;
        for (int flip = 0; flip < 3; flip++)
        {
            seed = seed * 1103515245u + 12345u;
            bytes[12u + (seed >> 8) % (length - 12u)] ^= (uint8_t)(1u << ((seed >> 4) & 7u));
        }
        tb_put32(bytes + 8, er_crc32(bytes + 12, length - 12u));
        ErbError error;
        if (!erb_load(mutant, length, &s_config, &error))
        {
            continue;
        }
        verified++;
        ERNode* root = slot_node(S_ROOT, 0);
        for (int event = 0; root && event < ER_EVENT_TYPE_COUNT_; event++)
        {
            if (root->events[event].fn)
            {
                fire(root, (EREventType)event);
            }
        }
        erb_tick(100);
        embedded_renderer_tick(100);
        erb_input_set_int(0, (int32_t)seed);
        erb_unload();
    }
    CHECK(verified > 0u, "some mutants verify, and run without crashing the host");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

static void null_fill(uint32_t argb, int x, int y, int width, int height, void* context)
{
    (void)argb, (void)x, (void)y, (void)width, (void)height, (void)context;
}

int main(void)
{
    EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = null_fill;
    embedded_renderer_set_backend(&backend);

    test_int_arithmetic();
    test_int64_arithmetic();
    test_float_rules();
    test_loops_and_calls();
    test_text();
    test_lists();
    test_props();
    test_templates();
    test_events_and_inputs();
    test_queries();
    test_timers_and_effects();
    test_animations();
    test_faults();
    test_reload();
    test_mutations();

    if (s_failed)
    {
        return 1;
    }
    printf("All %d Flow B VM checks passed\n", s_checked);
    return 0;
}
