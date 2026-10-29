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
 * One test per verifier rule (website/docs/internals/flow-b-packages.md, "Verifier rules"): the baseline
 * package passes, and a package that breaks one rule is rejected with that rule's reason.
 */

#include "erb_internal.h"
#include "erb_package.h"
#include "native_renderer.h"

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
static ErbProgram s_program;

static bool sensors_read(const int32_t* args, int32_t* result, void* context)
{
    (void)context;
    *result = args[0] + 1;
    return true;
}

static const ErbNative s_natives[] = {{"sensors.read", 1, "i:i", sensors_read}};

/** @brief The baseline package and a config it fits. */
static TPkg* base(void)
{
    tpkg_base(&s_pkg);
    memset(&s_config, 0, sizeof(s_config));
    s_config.arena = s_arena;
    s_config.arena_bytes = sizeof(s_arena);
    s_config.screen_w = TPKG_SCREEN;
    s_config.screen_h = TPKG_SCREEN;
    s_config.natives = s_natives;
    s_config.native_count = 1;
    return &s_pkg;
}

static ErbError verify_bytes(const void* bytes, size_t length, bool* ok)
{
    ErbError error;
    *ok = erb_verify(bytes, length, &s_config, &s_program, &error);
    return error;
}

static void expect(ErbStatus expected, const char* what, int line)
{
    const uint32_t length = tpkg_build(&s_pkg, (uint8_t*)s_package);
    bool ok;
    const ErbError error = verify_bytes(s_package, length, &ok);
    s_checked++;
    const bool pass = expected == ERB_OK ? ok : (!ok && error.status == expected);
    if (!pass)
    {
        printf("FAIL line %d (%s): expected %s, got %s (%s)\n",
               line,
               what,
               erb_status_str(expected),
               ok ? "ok" : erb_status_str(error.status),
               ok ? "" : error.detail);
        s_failed = 1;
    }
}

#define EXPECT(status, what) expect(status, what, __LINE__)

/** @brief A header flag bit no format version defines. */
#define UNKNOWN_FLAG 0x8000u

/* The image and font counts in an ERPK pack's header. */
#define TEST_IMAGE_COUNT_AT 8u
#define TEST_FONT_COUNT_AT 12u

/** @brief A slot flag bit no format version defines. */
#define UNKNOWN_SLOT_FLAG 0x80u

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

/** @brief Replaces a function's code, keeping its register types. */
static void set_code(uint16_t function, const uint32_t* code, uint32_t words)
{
    TFunction* record = &s_pkg.functions[function];
    tpkg_set_code(&s_pkg, function, s_pkg.types + record->types, record->registers, code, words);
}

/** @brief Replaces a function's code and register types. */
static void
set_code_types(uint16_t function, const uint8_t* types, uint8_t registers, const uint32_t* code, uint32_t words)
{
    tpkg_set_code(&s_pkg, function, types, registers, code, words);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Baseline
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_baseline_passes(void)
{
    base();
    EXPECT(ERB_OK, "the baseline package verifies");
    CHECK(s_program.function_count == 7u && s_program.slot_count == 11u, "the program's tables are found");
    CHECK(s_program.natives[0] == 0u, "the import resolves to the firmware's native");
    CHECK(s_program.counting[0] == 0u, "nothing in the baseline can exceed the budget");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Container
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_container(void)
{
    base();
    uint32_t length = tpkg_build(&s_pkg, (uint8_t*)s_package);
    ((uint8_t*)s_package)[length - 1u] ^= 0xFFu;
    bool ok;
    ErbError error = verify_bytes(s_package, length, &ok);
    CHECK(!ok && error.status == ERB_ERR_CORRUPT, "a CRC mismatch is corrupt");

    length = tpkg_build(&s_pkg, (uint8_t*)s_package);
    ((uint8_t*)s_package)[0] = 'X';
    error = verify_bytes(s_package, length, &ok);
    CHECK(!ok && error.status == ERB_ERR_CORRUPT, "a bad magic is not a package");

    length = tpkg_build(&s_pkg, (uint8_t*)s_package);
    static uint32_t shifted[8200];
    memcpy((uint8_t*)shifted + 2, s_package, length);
    error = verify_bytes((uint8_t*)shifted + 2, length, &ok);
    CHECK(!ok && error.status == ERB_ERR_NOT_FLOW_B, "a container off 4-byte alignment is rejected");

    base()->tag = "v0.15.0";
    EXPECT(ERB_ERR_NOT_FLOW_B, "a Flow A tag");
    base()->program_twice = true;
    EXPECT(ERB_ERR_NOT_FLOW_B, "two program sections");

    bool ok_null;
    error = verify_bytes(NULL, 0, &ok_null);
    CHECK(!ok_null && error.status == ERB_ERR_NO_PACKAGE, "no package");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Header, panel and needs
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_header(void)
{
    base()->major = ERB_FORMAT_MAJOR + 1u;
    EXPECT(ERB_ERR_NEWER_FORMAT, "a newer format major");
    base()->minor = ERB_FORMAT_MINOR + 1u;
    EXPECT(ERB_ERR_NEWER_FORMAT, "a newer format minor");
    base()->flags = UNKNOWN_FLAG;
    EXPECT(ERB_ERR_NEWER_FORMAT, "an unknown flag bit");
    base()->abi = ER_ABI + 1u;
    EXPECT(ERB_ERR_ENGINE_ABI, "an engine ABI past the firmware's");
    base()->abi = ER_ABI_MIN - 1u;
    EXPECT(ERB_ERR_ENGINE_ABI, "an engine ABI older than the firmware runs");
}

static void test_panel_and_needs(void)
{
    base();
    s_config.screen_w = 320;
    EXPECT(ERB_ERR_NO_PANEL, "no panel for the screen size");

    base();
    s_config.arena_bytes = 600;
    const uint32_t length = tpkg_build(&s_pkg, (uint8_t*)s_package);
    bool ok;
    const ErbError error = verify_bytes(s_package, length, &ok);
    CHECK(!ok && error.status == ERB_ERR_TOO_BIG && error.resource == ERB_RESOURCE_ARENA && error.needed == 692u
              && error.available == 600u,
          "an arena that doesn't fit, with the amounts");

    base()->panels[0].node_count = ERUI_MAX_NODES + 1u;
    EXPECT(ERB_ERR_TOO_BIG, "more nodes than the pool");
    base();
    s_config.node_count = 4;
    EXPECT(ERB_ERR_TOO_BIG, "more nodes than the firmware allows");
    base()->needs[0].amount = 10000u;
    EXPECT(ERB_ERR_TOO_BIG, "more animated values than the engine has");
    base()->needs[0].resource = 99u;
    EXPECT(ERB_ERR_TOO_BIG, "an unknown resource");
    base()->needs[0].resource = ERB_RES_FONTS;
    EXPECT(ERB_ERR_TOO_BIG, "fonts with no font storage");
    static GlyphInfo glyphs[1];
    static ErAssetsStorage no_array = {NULL, 1, glyphs, 1, NULL, 0, 0, 0, 0};
    s_config.fonts = &no_array;
    EXPECT(ERB_ERR_TOO_BIG, "font storage with a count but no array");
    base()->needs[1].resource = ERB_RES_ANIM_VALUES;
    EXPECT(ERB_ERR_TABLE, "a resource needed twice");
    base()->needs[0].amount = 0u;
    EXPECT(ERB_ERR_STATE, "more animated value slots than the needs declare");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tables
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_directory(void)
{
    base()->misalign_tables = true;
    EXPECT(ERB_ERR_TABLE, "tables off 4-byte alignment");
    base()->overlap_tables = true;
    EXPECT(ERB_ERR_TABLE, "two tables overlapping");
    base()->omit[ERB_TABLE_CODE] = true;
    EXPECT(ERB_ERR_TABLE, "a required table missing");
    base()->extra_table_id = 99u;
    EXPECT(ERB_ERR_TABLE, "an unknown table id");
    base()->extra_table_id = ERB_TABLE_STRINGS;
    EXPECT(ERB_ERR_TABLE, "a table listed twice");
}

static void test_strings(void)
{
    base()->strings[STR_BOOM] = "\xC3\x28";
    EXPECT(ERB_ERR_STRING, "invalid UTF-8");
    base()->strings[STR_BOOM] = "\xC0\xAF";
    EXPECT(ERB_ERR_STRING, "an overlong UTF-8 form");
    base()->strings[STR_BOOM] = "\xED\xA0\x80";
    EXPECT(ERB_ERR_STRING, "a UTF-16 surrogate");
    base()->strings[STR_BOOM] = "caf\xC3\xA9";
    EXPECT(ERB_OK, "valid UTF-8 passes");
}

static void test_arena(void)
{
    base()->regions[1].offset = 28u;
    EXPECT(ERB_ERR_ARENA, "regions overlapping");
    base();
    s_pkg.panels[0].arena_bytes = 700u;
    EXPECT(ERB_ERR_ARENA, "regions that don't add up to arena_bytes");
    base()->regions[2].kind = 99u;
    EXPECT(ERB_ERR_ARENA, "an unknown region kind");
    base()->regions[2].kind = ERB_REGION_SLOTS;
    EXPECT(ERB_ERR_ARENA, "a region kind twice");
    base()->regions[1].count = 100u;
    EXPECT(ERB_ERR_ARENA, "a region whose size doesn't match its count");
    base()->regions[0].offset = 2u;
    EXPECT(ERB_ERR_ARENA, "an unaligned region");
    base()->region_count = 9u;
    s_pkg.panels[0].arena_bytes = 436u;
    EXPECT(ERB_ERR_ARENA, "no register stack");
}

static void test_slots(void)
{
    base()->slots[S_COUNT].type = 99u;
    EXPECT(ERB_ERR_STATE, "an unknown slot type");
    base()->slots[S_SPEED].offset = 0u;
    EXPECT(ERB_ERR_STATE, "slots overlapping");
    base()->slots[S_COUNT].offset = 2u;
    EXPECT(ERB_ERR_STATE, "an unaligned slot");
    base()->slots[S_ROOT].type = ERB_T_INT64;
    EXPECT(ERB_ERR_STATE, "a slot outside its type's region");
    base()->slots[S_COUNT].dirty_bit = 5u;
    EXPECT(ERB_ERR_STATE, "a dirty bit past the region");
    base()->slots[S_READY].init = 2u;
    EXPECT(ERB_ERR_STATE, "a bool initialised to 2");
    base()->slots[S_MODE].init = 2u;
    EXPECT(ERB_ERR_STATE, "an enum initialised past its set");
    base()->slots[S_MODE].aux = 5u;
    EXPECT(ERB_ERR_STATE, "an enum naming no set");
    base()->slots[S_LABEL].cap = 3u;
    EXPECT(ERB_ERR_STATE, "a string initialised past its cap");
    base()->slots[S_COUNT].cap = 3u;
    EXPECT(ERB_ERR_STATE, "a cap on an int");
    base()->slots[S_BIG].init = 9u;
    EXPECT(ERB_ERR_STATE, "an int64 naming no constant");
    base()->slots[S_COUNT].flags = UNKNOWN_SLOT_FLAG;
    EXPECT(ERB_ERR_STATE, "an unknown slot flag");
}

static void test_lists(void)
{
    base()->fields[1].offset = 8u;
    EXPECT(ERB_ERR_STATE, "a row field past the row");
    base()->fields[1].type = ERB_T_STRING;
    EXPECT(ERB_ERR_STATE, "a string field with no cap");
    base()->rows[0].size = 6u;
    EXPECT(ERB_ERR_STATE, "a row size that isn't a multiple of 4");
    base()->slots[S_ITEMS].aux = 3u;
    EXPECT(ERB_ERR_STATE, "a list naming no row layout");
}

static void test_props(void)
{
    base()->prop_entries[1].prop = 9999u;
    EXPECT(ERB_ERR_PROP, "a prop id the firmware doesn't have");
    base()->prop_entries[1].kind = ERB_PK_COLOR;
    EXPECT(ERB_ERR_PROP, "a value kind that isn't the prop's");
    base()->prop_entries[1].value = 40000u;
    EXPECT(ERB_ERR_PROP, "a dimension past int16");
    base()->prop_entries[1] = (TPropEntry){27, ERB_PK_ENUM, 7u}; /* flex_direction */
    EXPECT(ERB_ERR_PROP, "an enum value past the prop's range");
    static char long_text[300];
    memset(long_text, 'a', sizeof(long_text) - 1u);
    base()->strings[STR_BOOM] = long_text;
    s_pkg.prop_entries[0].value = STR_BOOM;
    EXPECT(ERB_ERR_PROP, "text past the firmware's cap");
    base()->prop_entries[3].value = STR_HELLO;
    EXPECT(ERB_ERR_PROP, "an image the asset pack doesn't have");
    base()->prop_entries[0].value = 99u;
    EXPECT(ERB_ERR_PROP, "a string index past the table");
}

static void test_templates(void)
{
    base()->nodes[1].type = 99u;
    EXPECT(ERB_ERR_TEMPLATE, "an unknown node type");
    base()->nodes[0].children = 5u;
    EXPECT(ERB_ERR_TEMPLATE, "child counts past the template's nodes");
    base()->nodes[0].children = 2u;
    EXPECT(ERB_ERR_TEMPLATE, "nodes left over after the tree");
    base()->nodes[0].handle = S_COUNT;
    EXPECT(ERB_ERR_TEMPLATE, "a handle slot that isn't a node");
    base()->nodes[1].children = 1u;
    s_pkg.nodes[0].children = 2u;
    EXPECT(ERB_ERR_TEMPLATE, "a text node with a child");
    base()->panels[0].node_count = 3u;
    EXPECT(ERB_ERR_TEMPLATE, "a template with more nodes than the panel");
    base()->panels[0].root_template = 2u;
    EXPECT(ERB_ERR_TEMPLATE, "a root that isn't a root template");
    base()->nodes[4].handle = S_ROOT;
    EXPECT(ERB_ERR_TEMPLATE, "a row template keeping handles in a single slot");
    base()->nodes[2].props = 9u;
    EXPECT(ERB_ERR_TEMPLATE, "a prop set that doesn't exist");
    base()->nodes[0].flags = 1u;
    EXPECT(ERB_ERR_TEMPLATE, "an unknown node flag");
}

static void test_bindings(void)
{
    base()->bindings[0].target = F_TIMER;
    EXPECT(ERB_ERR_BINDING, "an event bound to a function of another kind");
    base()->bindings[2].event = 5u; /* width */
    EXPECT(ERB_ERR_BINDING, "an animated value bound to a prop that can't animate");
    base()->bindings[2].target = S_COUNT;
    EXPECT(ERB_ERR_BINDING, "an animated binding to a slot that isn't an animated value");
    base()->nodes[1].bindings = 1u;
    s_pkg.nodes[1].binding_count = 1u;
    EXPECT(ERB_ERR_BINDING, "a tape on a text node");
    base()->bindings[1].target = 4u;
    EXPECT(ERB_ERR_BINDING, "a tape that doesn't exist");
    base()->bindings[1].kind = 9u;
    EXPECT(ERB_ERR_BINDING, "an unknown binding kind");
    base()->nodes[0].binding_count = 9u;
    EXPECT(ERB_ERR_BINDING, "node bindings past the table");
    base()->bindings[2] = (TBinding){ERB_BIND_ANIMATED, PROP_BACKGROUND_COLOR, S_FADE};
    EXPECT(ERB_ERR_BINDING, "a plain animated binding to a color");

    base()->bindings[2] = (TBinding){ERB_BIND_INTERPOLATED, PROP_ROTATE_Z, 0};
    EXPECT(ERB_OK, "an interpolated binding");
    base()->bindings[2] = (TBinding){ERB_BIND_INTERPOLATED, PROP_BACKGROUND_COLOR, 1};
    EXPECT(ERB_OK, "an interpolation onto colors, bound to a color");
    base()->bindings[2] = (TBinding){ERB_BIND_INTERPOLATED, PROP_BACKGROUND_COLOR, 0};
    EXPECT(ERB_ERR_BINDING, "an interpolation onto numbers, bound to a color");
    base()->bindings[2] = (TBinding){ERB_BIND_INTERPOLATED, PROP_ROTATE_Z, 1};
    EXPECT(ERB_ERR_BINDING, "an interpolation onto colors, bound to a number");
    base()->bindings[2] = (TBinding){ERB_BIND_INTERPOLATED, PROP_WIDTH, 0};
    EXPECT(ERB_ERR_BINDING, "an interpolation bound to a prop that can't animate");
    base()->bindings[2] = (TBinding){ERB_BIND_INTERPOLATED, PROP_ROTATE_Z, 2};
    EXPECT(ERB_ERR_BINDING, "an interpolation that doesn't exist");
}

static void test_interps(void)
{
    base()->interps[0].point_count = 1u;
    EXPECT(ERB_ERR_ANIMATION, "an interpolation of one point");
    base()->interps[0].point_count = ER_INTERPOLATE_MAX_POINTS + 1u;
    EXPECT(ERB_ERR_ANIMATION, "more points than the engine holds");
    base()->interps[0].slot = S_COUNT;
    EXPECT(ERB_ERR_ANIMATION, "an interpolation of a slot that isn't an animated value");
    base()->interps[0].extrapolate_right = ER_EXTRAPOLATE_IDENTITY + 1u;
    EXPECT(ERB_ERR_ANIMATION, "an unknown extrapolation");
    base()->interps[0].flags = 0x02u;
    EXPECT(ERB_ERR_ANIMATION, "an unknown interpolation flag");
}

static void test_anims(void)
{
    base()->anims[1].kind = 99u;
    EXPECT(ERB_ERR_ANIMATION, "an unknown animation kind");
    base()->anims[1].easing = 99u;
    EXPECT(ERB_ERR_ANIMATION, "an unknown easing");
    base()->anims[0].first = 0u;
    EXPECT(ERB_ERR_ANIMATION, "a group whose child comes before it");
    base()->anims[0].count = 3u;
    EXPECT(ERB_ERR_ANIMATION, "a group's children past the table");
    base()->anims[1].target = S_COUNT;
    EXPECT(ERB_ERR_ANIMATION, "a leaf animating an int slot");
    base()->anims[2] = (TAnim){ERB_ANIM_PARALLEL, 0, 0, ERB_NONE, 0, 1, 1, 0, 0, 0, {0, 0, 0, 0}};
    s_pkg.anim_count = 3u;
    EXPECT(ERB_ERR_ANIMATION, "a child with two parents");
    base()->anims[0].kind = ERB_ANIM_LOOP;
    s_pkg.anims[0].count = 2u;
    s_pkg.anims[2] = s_pkg.anims[1];
    s_pkg.anim_count = 3u;
    EXPECT(ERB_ERR_ANIMATION, "a loop of two children");
    base()->anims[1].flags = 0x80u;
    EXPECT(ERB_ERR_ANIMATION, "an unknown animation flag");
}

static void test_tapes(void)
{
    base()->tape_floats[0] = 9.0f;
    EXPECT(ERB_ERR_TAPE, "an unknown vector op");
    base()->tape_floats[1] = 3.0f;
    EXPECT(ERB_ERR_TAPE, "a shape naming no paint");
    base()->tapes[0].float_count = 7u;
    EXPECT(ERB_ERR_TAPE, "an op cut off from its arguments");
    base()->paints[0].cap = 9u;
    EXPECT(ERB_ERR_TAPE, "an unknown stroke cap");
    base()->paints[0].fill_grad = 1;
    EXPECT(ERB_ERR_TAPE, "a paint naming a gradient the tape doesn't have");
    base()->gradients[0] = (TGradient){ER_GRADIENT_LINEAR, 1, 0, 0, 0, 1, 1, 0};
    s_pkg.gradient_count = 1u;
    s_pkg.stops[0] = (TStop){COLOR_WHITE, 0.0f};
    s_pkg.stop_count = 1u;
    s_pkg.tapes[0].gradient_count = 1u;
    EXPECT(ERB_ERR_TAPE, "a gradient with one stop");
    s_pkg.gradients[0].stop_count = 2u;
    s_pkg.stops[1] = (TStop){COLOR_BLACK, 1.0f};
    s_pkg.stop_count = 2u;
    EXPECT(ERB_OK, "a gradient with two stops passes");
    base()->tapes[0].paint_count = ERUI_VECTOR_PAINTS_MAX + 1u;
    EXPECT(ERB_ERR_TAPE, "more paints than the firmware keeps");
}

/** @brief Checks one timer function of an int and a float register against the verifier. */
static void expect_vector_code(ErbStatus expected, const char* what, uint32_t instruction, int line)
{
    const uint8_t types[] = {ERB_T_INT, ERB_T_FLOAT};
    const uint32_t code[] = {OP_ABX(LOADI, 0, 1), OP_ABX(LOADK, 1, 0), instruction, OP_ABC(RET, 0, 0, 0)};
    base();
    set_code_types(F_TIMER, types, 2, code, 4);
    expect(expected, what, line);
}
#define EXPECT_VECTOR_CODE(expected, what, instruction) expect_vector_code((expected), (what), (instruction), __LINE__)

static void test_vector_code(void)
{
    EXPECT_VECTOR_CODE(ERB_OK, "a fill from an int", OP_ABC(VPAINT, 0, 0, ERB_VPAINT_FILL));
    EXPECT_VECTOR_CODE(ERB_OK, "a stroke width from a float", OP_ABC(VPAINT, 1, 0, ERB_VPAINT_STROKE_WIDTH));
    EXPECT_VECTOR_CODE(ERB_ERR_BYTECODE, "a fill from a float", OP_ABC(VPAINT, 1, 0, ERB_VPAINT_FILL));
    EXPECT_VECTOR_CODE(ERB_ERR_BYTECODE, "an unknown paint field", OP_ABC(VPAINT, 0, 0, ERB_VPAINT_COUNT_));
    EXPECT_VECTOR_CODE(
        ERB_ERR_BYTECODE, "a paint past the firmware's", OP_ABC(VPAINT, 0, ERUI_VECTOR_PAINTS_MAX, ERB_VPAINT_FILL));
    EXPECT_VECTOR_CODE(ERB_OK, "a gradient's radius from a float", OP_ABC(VGRAD, 1, 0, ERB_VGRAD_RADIUS));
    EXPECT_VECTOR_CODE(ERB_OK, "a stop's color from an int", OP_ABC(VGRAD, 0, 0, ERB_VGRAD_STOPS + 2u));
    EXPECT_VECTOR_CODE(ERB_OK, "a stop's position from a float", OP_ABC(VGRAD, 1, 0, ERB_VGRAD_STOPS + 3u));
    EXPECT_VECTOR_CODE(ERB_ERR_BYTECODE, "a stop's position from an int", OP_ABC(VGRAD, 0, 0, ERB_VGRAD_STOPS + 1u));
    EXPECT_VECTOR_CODE(ERB_ERR_BYTECODE, "a field between the geometry and the stops", OP_ABC(VGRAD, 1, 0, 5u));
    EXPECT_VECTOR_CODE(
        ERB_ERR_BYTECODE, "a stop past the gradient's", OP_ABC(VGRAD, 0, 0, ERB_VGRAD_STOPS + 2u * ER_VGRAD_MAX_STOPS));
    EXPECT_VECTOR_CODE(
        ERB_ERR_BYTECODE, "a gradient past the firmware's", OP_ABC(VGRAD, 1, ERUI_VECTOR_GRADS_MAX, ERB_VGRAD_AX));
    EXPECT_VECTOR_CODE(ERB_ERR_BYTECODE, "a commit of a tape that doesn't exist", OP_ABX(VCOMMITP, 0, 1));
}

/** @brief The baseline package with a keyboard of one layer: a row of a key that types and a backspace. */
static TKeyboard* keyboard(void)
{
    base()->has_keyboard = true;
    TKeyboard* layout = &s_pkg.keyboard;
    layout->layers[0] = (TGroup){0, 1};
    layout->layer_count = 1;
    layout->rows[0] = (TGroup){0, 2};
    layout->row_count = 1;
    layout->keys[0] = (TKey){ERB_NONE, STR_ON, ER_KBD_KEY_CHAR, 0, 2, 0xFFu};
    layout->keys[1] = (TKey){STR_OFF, ERB_NONE, ER_KBD_KEY_BACKSPACE, 0, 1, 0xFFu};
    layout->key_count = 2;
    return layout;
}

static void test_keyboard(void)
{
    keyboard();
    EXPECT(ERB_OK, "a keyboard layout");
    base()->has_keyboard = true;
    s_pkg.keyboard.colors[1] = COLOR_BLUE;
    EXPECT(ERB_OK, "a keyboard's look over the built-in layout");
    keyboard()->key_count = 3;
    EXPECT(ERB_ERR_TABLE, "a keyboard table that isn't whole records");
    keyboard()->rows[0].first = 1;
    EXPECT(ERB_ERR_TABLE, "a row that doesn't start after the one before it");
    keyboard()->rows[0].count = 1;
    EXPECT(ERB_ERR_TABLE, "rows that leave a key out");
    TKeyboard* layout = keyboard();
    layout->layers[1] = (TGroup){1, 0};
    layout->layer_count = 2;
    EXPECT(ERB_ERR_TABLE, "a layer with no rows");
    keyboard()->keys[0].text = ERB_NONE;
    EXPECT(ERB_ERR_TABLE, "a key that types nothing");
    keyboard()->keys[1].label = 40;
    EXPECT(ERB_ERR_TABLE, "a key label that isn't a string");
    keyboard()->keys[1].type = ER_KBD_KEY_LAYER + 1u;
    EXPECT(ERB_ERR_TABLE, "an unknown key type");
    keyboard()->keys[1] = (TKey){STR_OFF, ERB_NONE, ER_KBD_KEY_LAYER, 0, 1, 0xFFu};
    EXPECT(ERB_OK, "a layer key");
    keyboard()->keys[1] = (TKey){STR_OFF, ERB_NONE, ER_KBD_KEY_LAYER, 1, 1, 0xFFu};
    EXPECT(ERB_ERR_TABLE, "a layer key switching to a layer that isn't there");
    layout = keyboard();
    for (uint32_t index = 0; index <= ERB_MAX_KEYBOARD_LAYERS; index++)
    {
        layout->layers[index] = (TGroup){(uint16_t)index, 1};
        layout->rows[index] = (TGroup){(uint16_t)index, 1};
        layout->keys[index] = layout->keys[0];
    }
    layout->layer_count = layout->row_count = layout->key_count = ERB_MAX_KEYBOARD_LAYERS + 1u;
    EXPECT(ERB_ERR_TOO_BIG, "more keyboard layers than the firmware holds");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions and code
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_functions(void)
{
    base()->functions[F_BOOT].words = 999u;
    EXPECT(ERB_ERR_FUNCTION, "code past the table");
    {
        const uint8_t types[] = {99u};
        const uint32_t code[] = {OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 1, code, 1);
        EXPECT(ERB_ERR_FUNCTION, "an unknown register type");
    }
    {
        const uint8_t types[] = {ERB_T_INT, ERB_T_INT64, ERB_T_PAIR};
        const uint32_t code[] = {OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 3, code, 1);
        EXPECT(ERB_ERR_FUNCTION, "a 64-bit register on an odd cell");
    }
    {
        const uint32_t code[] = {OP_ABX(LOADI, 0, 1)};
        base();
        set_code(F_BOOT, code, 1);
        EXPECT(ERB_ERR_FUNCTION, "code that runs off its end");
    }
    base()->functions[F_BOOT].params = 1u;
    EXPECT(ERB_ERR_FUNCTION, "a boot function with a parameter");
    base()->functions[F_TIMER].kind = ERB_FN_QUERY;
    EXPECT(ERB_ERR_FUNCTION, "a query that returns nothing");
    base()->functions[F_TIMER].kind = 0x70u;
    EXPECT(ERB_ERR_FUNCTION, "an unknown function kind");
    base()->panels[0].boot_function = F_TIMER;
    EXPECT(ERB_ERR_FUNCTION, "a panel's boot function that isn't one");
    base()->panels[0].update_function = 40u;
    EXPECT(ERB_ERR_FUNCTION, "a panel's update function past the table");
}

static void test_bytecode(void)
{
    {
        const uint32_t code[] = {250u, OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_TIMER, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "an unknown opcode");
    }
    {
        const uint32_t code[] = {OP_ABX(LOADI, 5, 1), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_BOOT, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "a register past the frame");
    }
    {
        const uint32_t code[] = {OP_ABX(LOADI, 0, 5), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_BOOT, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "a bool loaded with 5");
    }
    {
        const uint8_t types[] = {ERB_T_FLOAT, ERB_T_FLOAT};
        const uint32_t code[] = {OP_ABC(ADD, 0, 0, 1), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 2, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "integer ADD on float registers");
    }
    {
        const uint32_t code[] = {OP_ABX(GET32, 0, 99), OP_ABX(SET32, 0, S_COUNT), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_PRESS, code, 3);
        EXPECT(ERB_ERR_BYTECODE, "a slot past the table");
    }
    {
        const uint32_t code[] = {OP_ABX(GET32, 0, S_SPEED), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_PRESS, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "a float slot read into an int");
    }
    {
        const uint8_t types[] = {ERB_T_BOOL};
        const uint32_t code[] = {OP_ABX(LOADI, 0, 1), OP_ABX(SET32, 0, S_READY), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 1, code, 3);
        EXPECT(ERB_ERR_BYTECODE, "a boot-only slot written outside boot");
    }
    {
        const uint8_t types[] = {ERB_T_ENUM};
        const uint32_t code[] = {OP_ABX(LOADE, 0, 2), 0u, OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 1, code, 3);
        EXPECT(ERB_ERR_BYTECODE, "an enum value past its set");
    }
    base()->functions[F_UPDATE].words = 1u;
    EXPECT(ERB_ERR_BYTECODE, "an instruction cut off from its extension word");
    {
        const uint32_t code[] = {OP_ABX(GET32, 0, S_COUNT), OP_ABX(CALL, 0, F_DONE), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_PRESS, code, 3);
        EXPECT(ERB_ERR_BYTECODE, "a CALL to an entry point");
    }
    {
        const uint8_t types[] = {ERB_T_FLOAT, ERB_T_INT};
        const uint32_t code[] = {OP_ABX(CALL, 0, F_DOUBLE), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_PRESS, types, 2, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "a CALL with a float for an int parameter");
    }
    {
        const uint8_t types[] = {ERB_T_INT};
        const uint32_t code[] = {OP_ABC(EVT, 0, ERB_EVT_X, 0), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 1, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "EVT in a timer");
        base();
        set_code_types(F_PRESS, types, 1, code, 2);
        EXPECT(ERB_OK, "EVT x in a press handler");
    }
    {
        const uint8_t types[] = {ERB_T_INT};
        const uint32_t code[] = {OP_ABC(EVT, 0, ERB_EVT_TOUCHES, 0), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 1, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "EVT touches in a timer");
        base();
        set_code_types(F_PRESS, types, 1, code, 2);
        EXPECT(ERB_OK, "EVT touches in a press handler");
    }
    {
        const uint8_t types[] = {ERB_T_INT};
        const uint32_t code[] = {OP_ABX(PSETI, 0, 73), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 1, code, 2);
        EXPECT(ERB_ERR_PROP, "an int set on a text prop");
    }
    {
        const uint8_t types[] = {ERB_T_FLOAT};
        const uint32_t code[] = {OP_ABX(CALLN, 0, 0), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 1, code, 2);
        EXPECT(ERB_ERR_BYTECODE, "a native called with a float for an int");
    }
    {
        const uint8_t types[] = {ERB_T_NODE, ERB_T_PAIR};
        const uint32_t code[] = {OP_ABC(BUILD, 0, 0, 0), 0u, OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 2, code, 3);
        EXPECT(ERB_ERR_BYTECODE, "BUILD of the root template");
    }
}

static void test_control_flow(void)
{
    {
        const uint32_t code[] = {OP_ABC(RET, 0, 0, 0), OP_ABX(JMP, 0, -2), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_TIMER, code, 3);
        EXPECT(ERB_ERR_CONTROL_FLOW, "a backward JMP");
    }
    {
        const uint32_t code[] = {OP_ABX(JMP, 0, 1), OP_ABX(JDIRTY, 0, 0), 1u, OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_UPDATE, code, 4);
        EXPECT(ERB_ERR_CONTROL_FLOW, "a branch into an extension word");
    }
    {
        const uint32_t code[] = {OP_ABX(JMP, 0, 5), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code(F_TIMER, code, 2);
        EXPECT(ERB_ERR_CONTROL_FLOW, "a branch past the end");
    }
    const uint8_t types[] = {ERB_T_INT, ERB_T_INT};
    {
        const uint32_t code[] = {OP_ABX(LOADI, 1, 0), OP_ABX(LOOP, 0, -2), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_TIMER, types, 2, code, 3);
        EXPECT(ERB_ERR_CONTROL_FLOW, "a LOOP with no LOOPSET before its body");
    }
    {
        const uint32_t code[] = {
            OP_ABX(LOOPSET, 0, 10),
            OP_ABC(ADD, 0, 0, 1),
            OP_ABX(LOOP, 0, -2),
            OP_ABC(RET, 0, 0, 0),
        };
        base();
        set_code_types(F_TIMER, types, 2, code, 4);
        EXPECT(ERB_ERR_CONTROL_FLOW, "a loop counter written in the body");
    }
    {
        const uint32_t code[] = {
            OP_ABX(JMP, 0, 1),
            OP_ABX(LOOPSET, 0, 10),
            OP_ABX(LOADI, 1, 1),
            OP_ABX(LOOP, 0, -2),
            OP_ABC(RET, 0, 0, 0),
        };
        base();
        set_code_types(F_TIMER, types, 2, code, 5);
        EXPECT(ERB_ERR_CONTROL_FLOW, "a jump into a loop's body");
    }
    {
        const uint32_t code[] = {
            OP_ABX(LOOPSET, 0, 10),
            OP_ABX(LOADI, 1, 1),
            OP_ABX(LOOP, 0, -2),
            OP_ABC(RET, 0, 0, 0),
        };
        base();
        set_code_types(F_TIMER, types, 2, code, 4);
        EXPECT(ERB_OK, "a bounded loop passes");
    }
}

static void test_calls(void)
{
    {
        const uint8_t types[] = {ERB_T_INT, ERB_T_INT};
        const uint32_t code[] = {OP_ABX(CALL, 0, F_DOUBLE), OP_ABC(RET, 0, 0, 0)};
        base();
        set_code_types(F_DOUBLE, types, 2, code, 2);
        EXPECT(ERB_ERR_RECURSION, "a helper calling itself");
    }
    {
        /* A second helper, before the first in the table, that the first calls: a call back up the table. */
        const uint8_t types[] = {ERB_T_INT, ERB_T_INT};
        const uint32_t helper[] = {OP_ABC(RET, 0, 0, 0)};
        base();
        const uint16_t later = tpkg_function(&s_pkg, ERB_FN_HELPER, 1, ERB_T_INT, types, 2, helper, 1);
        const uint32_t code[] = {OP_ABX(CALL, 0, F_DOUBLE), OP_ABC(RET, 0, 0, 0)};
        set_code_types(later, types, 2, code, 2);
        s_pkg.functions[later].params = 1;
        EXPECT(ERB_ERR_RECURSION, "a call to a function earlier in the table");
    }
    base()->regions[9].count = 4u;
    s_pkg.regions[9].bytes = 16u;
    s_pkg.panels[0].arena_bytes = TPKG_ARENA_BYTES - 240u;
    {
        const uint32_t length = tpkg_build(&s_pkg, (uint8_t*)s_package);
        bool ok;
        const ErbError error = verify_bytes(s_package, length, &ok);
        CHECK(!ok && error.status == ERB_ERR_STACK && error.needed == 10u && error.available == 4u,
              "a call path deeper than the register stack, with the amounts");
    }
}

static void test_budget(void)
{
    /* Two nested loops of 1000 run past the default budget, so the timer is run counting. */
    const uint8_t types[] = {ERB_T_INT, ERB_T_INT, ERB_T_INT};
    const uint32_t code[] = {
        OP_ABX(LOOPSET, 0, 1000),
        OP_ABX(LOOPSET, 1, 1000),
        OP_ABX(LOADI, 2, 1),
        OP_ABX(LOOP, 1, -2),
        OP_ABX(LOOP, 0, -4),
        OP_ABC(RET, 0, 0, 0),
    };
    base();
    set_code_types(F_TIMER, types, 3, code, 6);
    EXPECT(ERB_OK, "nested bounded loops pass");
    CHECK((s_program.counting[0] >> F_TIMER) & 1u, "an entry whose worst case passes the budget is counted");
    CHECK(!((s_program.counting[0] >> F_PRESS) & 1u), "an entry well inside the budget isn't");
}

static void test_imports_and_inputs(void)
{
    base();
    s_config.native_count = 0;
    {
        const uint32_t length = tpkg_build(&s_pkg, (uint8_t*)s_package);
        bool ok;
        const ErbError error = verify_bytes(s_package, length, &ok);
        CHECK(!ok && error.status == ERB_ERR_IMPORT && error.name && strcmp(error.name, "sensors.read") == 0,
              "a missing native, named");
    }
    base()->imports[0].version = 2u;
    EXPECT(ERB_ERR_IMPORT, "a native of another version");
    base()->imports[0].signature = STR_HELLO;
    EXPECT(ERB_ERR_IMPORT, "a native with another signature");

    base()->inputs[0].slot = S_ITEMS;
    EXPECT(ERB_ERR_INPUT, "an input on a list");
    base()->inputs[1] = s_pkg.inputs[0];
    s_pkg.input_count = 2u;
    EXPECT(ERB_ERR_INPUT, "two inputs with one name");
    base()->inputs[0].slot = 99u;
    EXPECT(ERB_ERR_INPUT, "an input on a slot past the table");
}

static void test_debug(void)
{
    base()->debug_build_id = 1u;
    EXPECT(ERB_ERR_DEBUG, "a debug table for another build");
    base()->debug_entries[0] = (TDebugEntry){F_UPDATE, 0, 1, 1};
    EXPECT(ERB_ERR_DEBUG, "an entry on an extension word");
    base()->debug_entries[1] = (TDebugEntry){F_BOOT, 0, 0, 1};
    s_pkg.debug_entry_count = 2u;
    EXPECT(ERB_ERR_DEBUG, "entries out of order");
    base()->debug_entries[0].file = 3u;
    EXPECT(ERB_ERR_DEBUG, "an entry naming no file");
    base()->debug_names[0] = 7u;
    EXPECT(ERB_ERR_DEBUG, "a function name past the strings");
    base()->debug_strings[0] = "\xFF";
    EXPECT(ERB_ERR_DEBUG, "a malformed debug string");
    base()->has_debug = false;
    EXPECT(ERB_OK, "no debug table is fine");
}

static void test_assets(void)
{
    base()->assets_length = 20u;
    EXPECT(ERB_ERR_ASSETS, "a truncated asset pack");
    base()->assets[4] = 9u;
    EXPECT(ERB_ERR_ASSETS, "an asset pack of an unknown version");
    /* The same image as a v1 pack: no pad, so its pixels land at 29. */
    static const uint8_t version1_pack[] = {
        'E', 'R', 'P', 'K', 1, 0, 0, 0, 1, 0, 0, 0, 0,    0,    0,    0,    3,
        0,   'i', 'm', 'g', 1, 0, 0, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    base();
    memcpy(s_pkg.assets, version1_pack, sizeof(version1_pack));
    s_pkg.assets_length = sizeof(version1_pack);
    EXPECT(ERB_ERR_ASSETS, "image pixels off their word");
    /* A font, with no storage for its tables. */
    static const uint8_t font[] = {
        1, 0, 'f', 16, 18, 14, 1, 'A', 0, 'A', 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0xFF,
    };
    base();
    s_pkg.assets[TEST_FONT_COUNT_AT] = 1u;
    memcpy(s_pkg.assets + s_pkg.assets_length, font, sizeof(font));
    s_pkg.assets_length += sizeof(font);
    EXPECT(ERB_ERR_ASSETS, "a font that doesn't fit the firmware's font storage");
    static BitmapFont fonts[1];
    static GlyphInfo glyphs[1];
    static ErAssetsStorage storage = {fonts, 1, glyphs, 1, NULL, 0, 0, 0, 0};
    s_config.fonts = &storage;
    EXPECT(ERB_OK, "the same font with room for it");

    base();
    s_pkg.assets[21] = 0u; /* the image's width */
    EXPECT(ERB_ERR_ASSETS, "an image with no width");

    base()->assets[TEST_FONT_COUNT_AT] = 1u;
    memcpy(s_pkg.assets + s_pkg.assets_length, font, sizeof(font));
    s_pkg.assets_length += sizeof(font);
    static ErAssetsStorage no_fonts_array = {NULL, 1, glyphs, 1, NULL, 0, 0, 0, 0};
    s_config.fonts = &no_fonts_array;
    EXPECT(ERB_ERR_ASSETS, "a font with font storage that has a count but no array");
}

/** @brief A font for the test pack: each glyph is glyph_width by 1 pixel at bitmap_offset. */
typedef struct
{
    const char* family;
    uint8_t pixel_size;
    uint8_t format;
    uint16_t first;
    uint16_t last;
    uint16_t glyph_count;
    uint8_t glyph_width;
    uint32_t bitmap_offset;
    uint32_t bitmap_length;
    uint16_t extras_count;
    uint32_t extra_codepoints[2];
} TestFont;

/** @brief A font that passes every check: 'A' only, one 1-bit pixel. */
static TestFont good_font(const char* family, uint8_t pixel_size)
{
    return (TestFont){family, pixel_size, ERUI_FONT_FMT_1BIT, 'A', 'A', 1u, 1u, 0u, 1u, 0u, {0u, 0u}};
}

static void put_u8(uint8_t value)
{
    s_pkg.assets[s_pkg.assets_length++] = value;
}

static void put_u16(uint32_t value)
{
    put_u8(value & 0xFFu);
    put_u8((value >> 8) & 0xFFu);
}

static void put_u32(uint32_t value)
{
    put_u16(value & 0xFFFFu);
    put_u16(value >> 16);
}

static void put_glyph(const TestFont* font)
{
    put_u32(font->bitmap_offset);
    put_u8(font->glyph_width);
    put_u8(1u); /* height */
    put_u8(0u);
    put_u8(0u);
    put_u8(font->glyph_width); /* advance */
}

/** @brief Appends @p font to the baseline pack and counts it in the pack's header. */
static void add_font(const TestFont* font)
{
    s_pkg.assets[TEST_FONT_COUNT_AT]++;
    const size_t name_length = strlen(font->family);
    put_u16((uint32_t)name_length);
    memcpy(s_pkg.assets + s_pkg.assets_length, font->family, name_length);
    s_pkg.assets_length += (uint32_t)name_length;
    put_u8(font->pixel_size);
    put_u8(font->pixel_size + 2u); /* line height */
    put_u8(font->pixel_size - 2u); /* baseline */
    put_u8(font->format);
    put_u16(font->first);
    put_u16(font->last);
    put_u16(font->glyph_count);
    put_u16(font->extras_count);
    put_u32(font->bitmap_length);
    for (uint16_t glyph_index = 0; glyph_index < font->glyph_count; glyph_index++)
    {
        put_glyph(font);
    }
    for (uint16_t extra_index = 0; extra_index < font->extras_count; extra_index++)
    {
        put_u32(font->extra_codepoints[extra_index]);
        put_glyph(font);
    }
    for (uint32_t byte_index = 0; byte_index < font->bitmap_length; byte_index++)
    {
        put_u8(0xFFu);
    }
}

/** @brief Appends a 1x1 ARGB image to the baseline pack (before any font) and counts it in the pack's header. */
static void add_image(const char* name)
{
    s_pkg.assets[TEST_IMAGE_COUNT_AT]++;
    const size_t name_length = strlen(name);
    put_u16((uint32_t)name_length);
    memcpy(s_pkg.assets + s_pkg.assets_length, name, name_length);
    s_pkg.assets_length += (uint32_t)name_length;
    put_u32(1u);
    put_u32(1u);
    put_u8(0u); /* ARGB8888 */
    while (s_pkg.assets_length % 4u)
    {
        put_u8(0u);
    }
    put_u32(COLOR_WHITE);
}

static void test_image_index(void)
{
    base();
    add_image("c");
    add_image("a");
    add_image("imh");
    add_image("b");
    uint32_t names[8];
    ErAssetsInfo info;
    CHECK(er_assets_check_pack_images(s_pkg.assets, s_pkg.assets_length, &info, names, 8u) && info.images == 5u,
          "a pack's images are listed");
    static const char* const present[] = {"a", "b", "c", "img", "imh"};
    for (uint32_t index = 0; index < 5u; index++)
    {
        CHECK(er_assets_find_image(s_pkg.assets, names, info.images, present[index]), "a listed image is found");
    }
    static const char* const absent[] = {"", "d", "im", "imgs", "A"};
    for (uint32_t index = 0; index < 5u; index++)
    {
        CHECK(!er_assets_find_image(s_pkg.assets, names, info.images, absent[index]), "an image it lacks isn't");
    }
    ERPoolLimits limits;
    er_pool_limits(&limits);
    if (limits.images >= info.images)
    {
        EXPECT(ERB_OK, "an image prop is found among several images");
    }
    else
    {
        EXPECT(ERB_ERR_ASSETS, "more images than the image registry holds");
    }
    CHECK(er_assets_check_pack_images(s_pkg.assets, s_pkg.assets_length, &info, names, 2u) && info.images == 5u,
          "images past the list's capacity are still counted");
}

/** @brief Storage for as many one-glyph fonts as the font registry can be asked to hold, and more. */
#define FONT_STORAGE_FONTS 32u

static void give_font_storage(void)
{
    static BitmapFont fonts[FONT_STORAGE_FONTS];
    static GlyphInfo glyphs[FONT_STORAGE_FONTS];
    static ExtraGlyph extras[FONT_STORAGE_FONTS];
    static ErAssetsStorage storage;
    storage =
        (ErAssetsStorage){fonts, FONT_STORAGE_FONTS, glyphs, FONT_STORAGE_FONTS, extras, FONT_STORAGE_FONTS, 0, 0, 0};
    s_config.fonts = &storage;
}

/** @brief The baseline package with one font, and room for its tables. */
static void expect_font(const TestFont* font, ErbStatus status, const char* what, int line)
{
    base();
    add_font(font);
    give_font_storage();
    expect(status, what, line);
}

static void test_fonts(void)
{
    TestFont font = good_font("f", 16u);
    expect_font(&font, ERB_OK, "a font that passes every check", __LINE__);
    font.format = 3u;
    expect_font(&font, ERB_ERR_ASSETS, "a font format the engine can't draw", __LINE__);
    font = good_font("f", 16u);
    font.first = 'B';
    expect_font(&font, ERB_ERR_ASSETS, "a font whose range ends before it starts", __LINE__);
    font = good_font("f", 16u);
    font.last = 'B';
    expect_font(&font, ERB_ERR_ASSETS, "a dense glyph array shorter than its range", __LINE__);
    font = good_font("f", 16u);
    font.bitmap_offset = 1u;
    expect_font(&font, ERB_ERR_ASSETS, "a glyph that starts past its font's bitmap", __LINE__);
    font = good_font("f", 16u);
    font.glyph_width = 9u; /* two bytes a row at 1 bit */
    expect_font(&font, ERB_ERR_ASSETS, "a glyph that runs past its font's bitmap", __LINE__);
    font = good_font("f", 16u);
    font.extras_count = 2u;
    font.extra_codepoints[0] = 300u;
    font.extra_codepoints[1] = 200u;
    expect_font(&font, ERB_ERR_ASSETS, "extra glyphs out of codepoint order", __LINE__);
    font.extra_codepoints[1] = 400u;
    expect_font(&font, ERB_OK, "extra glyphs in order", __LINE__);

    ERPoolLimits limits;
    er_pool_limits(&limits);
    static const char* const families[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n"};
    if (limits.font_families + 1u > sizeof(families) / sizeof(families[0])
        || limits.font_sizes + 1u > FONT_STORAGE_FONTS)
    {
        CHECK(false, "the test has enough family names and font storage for the registry's limits");
        return;
    }
    base();
    give_font_storage();
    for (uint32_t family_index = 0; family_index < limits.font_families; family_index++)
    {
        font = good_font(families[family_index], 16u);
        add_font(&font);
    }
    EXPECT(ERB_OK, "as many font families as the registry holds");
    font = good_font(families[limits.font_families], 16u);
    add_font(&font);
    EXPECT(ERB_ERR_ASSETS, "one family more than the registry holds");

    /* The registry compares ER_FONT_FAMILY_MAX bytes, so these two are one family. */
    base();
    give_font_storage();
    for (uint32_t family_index = 0; family_index + 1u < limits.font_families; family_index++)
    {
        font = good_font(families[family_index], 16u);
        add_font(&font);
    }
    font = good_font("a family name longer than the registry keeps: one", 16u);
    add_font(&font);
    font = good_font("a family name longer than the registry keeps: two", 18u);
    add_font(&font);
    EXPECT(ERB_OK, "names that differ past ER_FONT_FAMILY_MAX are one family");

    base();
    give_font_storage();
    for (uint32_t size_index = 0; size_index < limits.font_sizes; size_index++)
    {
        font = good_font("f", (uint8_t)(8u + size_index));
        add_font(&font);
    }
    EXPECT(ERB_OK, "as many sizes of one family as the registry holds");
    font = good_font("f", (uint8_t)(8u + limits.font_sizes));
    add_font(&font);
    EXPECT(ERB_ERR_ASSETS, "one size more than the registry holds");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Messages
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_messages(void)
{
    ErbError error;
    memset(&error, 0, sizeof(error));
    error.function = ERB_NONE;
    error.status = ERB_ERR_TOO_BIG;
    error.resource = ERB_RESOURCE_ARENA;
    error.needed = 30000;
    error.available = 24576;
    char text[128];
    erb_error_message(&error, text, sizeof(text));
    CHECK(strcmp(text, "needs 30000 arena bytes, firmware reserves 24576") == 0, "the arena message has the amounts");

    error.status = ERB_ERR_ENGINE_ABI;
    error.needed = 9;
    erb_error_message(&error, text, sizeof(text));
    CHECK(strstr(text, "built for engine ABI 9") != NULL, "the ABI message names the package's level");

    error.status = ERB_ERR_IMPORT;
    error.name = "sensors.read";
    erb_error_message(&error, text, sizeof(text));
    CHECK(strcmp(text, "missing native module: sensors.read") == 0, "the import message names the module");

    error.status = ERB_ERR_TRAP;
    error.name = NULL;
    error.file = "App.jsx";
    error.line = 41;
    erb_error_message(&error, text, sizeof(text));
    CHECK(strcmp(text, "app stopped at App.jsx:41") == 0, "a fault with a debug table names the source line");

    error.file = NULL;
    error.function = 3;
    error.offset = 12;
    error.build_id = 0xABCD;
    erb_error_message(&error, text, sizeof(text));
    CHECK(strcmp(text, "app stopped in function 3 at 12 (build 0000abcd)") == 0,
          "a fault without one names the function, offset and build");

    erb_error_message(&error, text, 8);
    CHECK(strlen(text) == 7u, "a message is cut to the buffer");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    test_baseline_passes();
    test_container();
    test_header();
    test_panel_and_needs();
    test_directory();
    test_strings();
    test_arena();
    test_slots();
    test_lists();
    test_props();
    test_templates();
    test_bindings();
    test_interps();
    test_anims();
    test_tapes();
    test_vector_code();
    test_keyboard();
    test_functions();
    test_bytecode();
    test_control_flow();
    test_calls();
    test_budget();
    test_imports_and_inputs();
    test_debug();
    test_assets();
    test_fonts();
    test_image_index();
    test_messages();

    if (s_failed)
    {
        return 1;
    }
    printf("All %d Flow B verifier checks passed\n", s_checked);
    return 0;
}
