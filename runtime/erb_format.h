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

#ifndef ERB_FORMAT_H
#define ERB_FORMAT_H

/*
 * erb_format — the Flow B package format: section and table ids, record sizes, the codes stored in records,
 * and the instruction set. The compiler's package writer and this runtime both follow it; the spec is
 * website/docs/internals/flow-b-packages.md.
 *
 * Everything is little-endian. Every table starts 4-aligned and every record is a multiple of 4 bytes.
 */

#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Versions and container
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Package format and instruction set this runtime runs. A package must have this major. */
#define ERB_FORMAT_MAJOR 1u

/** @brief Additions within the major. A package's minor may be at most this. */
#define ERB_FORMAT_MINOR 15u

/** @brief The minor whose table directory names each table's panel. Before it, every table is every panel's. */
#define ERB_FORMAT_MINOR_PANELS 4u

/**
 * @brief The minor whose debug section holds a table for each panel, each naming its panel and length. Before it,
 * the section is one table for all panels, with a 16-byte header and a u32 function count.
 */
#define ERB_FORMAT_MINOR_DEBUG_PANELS 5u

/**
 * @brief The minor whose panels name a layout check instead of a width and height, and whose needs name their panel.
 * Before it, a panel is for one exact size and every need is every panel's.
 */
#define ERB_FORMAT_MINOR_CHECKS 6u

/**
 * @brief The minor whose imports and native events name their optional module, and whose native event records are
 * 12 bytes. Before it, every import is required and a native events table isn't read.
 */
#define ERB_FORMAT_MINOR_OPTIONAL 12u

/** @brief The container tag of a Flow B package. */
#define ERB_TAG "erb"

/** @brief Section types Flow B adds to ERCF (the asset pack stays ER_SECTION_ASSET_PACK). */
enum
{
    ERB_SECTION_PROGRAM = 4,
    ERB_SECTION_DEBUG = 5,
};

/** @brief Header flag bits. */
#define ERB_FLAG_DEBUG 0x0001u
#define ERB_FLAGS_KNOWN ERB_FLAG_DEBUG

/** @brief "None", for any u16 index. */
#define ERB_NONE 0xFFFFu

/*----------------------------------------------------------------------------------------------------------------------
 - Program section
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Fixed sizes, in bytes. */
#define ERB_HEADER_SIZE 24u
#define ERB_PANEL_SIZE 16u
#define ERB_NEED_SIZE 8u
#define ERB_DIRECTORY_SIZE 12u

/*
 * Field offsets, in bytes from the start of their record. Each record's layout is in the spec; the _AT
 * suffix marks an offset.
 */

/** @brief Program header. */
#define ERB_HEADER_FORMAT_MAJOR_AT 0u
#define ERB_HEADER_FORMAT_MINOR_AT 2u
#define ERB_HEADER_ENGINE_ABI_AT 4u
#define ERB_HEADER_FLAGS_AT 6u
#define ERB_HEADER_BUILD_ID_AT 8u
#define ERB_HEADER_ARENA_BYTES_AT 12u
#define ERB_HEADER_NODE_COUNT_AT 16u
#define ERB_HEADER_PANEL_COUNT_AT 18u
#define ERB_HEADER_NEED_COUNT_AT 20u
#define ERB_HEADER_TABLE_COUNT_AT 22u

/** @brief Panel. */
#define ERB_PANEL_CHECK_AT 0u /**< Since minor 6: the check's first word in the checks table. */
#define ERB_PANEL_WIDTH_AT 0u /**< Before minor 6. */
#define ERB_PANEL_HEIGHT_AT 2u
#define ERB_PANEL_ARENA_BYTES_AT 4u
#define ERB_PANEL_NODE_COUNT_AT 8u
#define ERB_PANEL_ROOT_TEMPLATE_AT 10u
#define ERB_PANEL_BOOT_FUNCTION_AT 12u
#define ERB_PANEL_UPDATE_FUNCTION_AT 14u

/** @brief Need. */
#define ERB_NEED_RESOURCE_AT 0u
#define ERB_NEED_PANEL_AT 2u /**< Since minor 6: the panel it is for, or ERB_NONE for every panel. */
#define ERB_NEED_AMOUNT_AT 4u

/** @brief Table directory entry. */
#define ERB_DIRECTORY_ID_AT 0u
#define ERB_DIRECTORY_PANEL_AT 2u
#define ERB_DIRECTORY_OFFSET_AT 4u
#define ERB_DIRECTORY_LENGTH_AT 8u

/** @brief Engine resources a package names in its needs, besides nodes. */
enum
{
    ERB_RESOURCE_VECTOR_NODES = 1,
    ERB_RESOURCE_ANIMATION_VALUES = 2,
    ERB_RESOURCE_ANIMATIONS = 3,
    ERB_RESOURCE_ANIMATION_GROUPS = 4,
    ERB_RESOURCE_IMAGES = 5,
    ERB_RESOURCE_FONTS = 6,
    ERB_RESOURCE_GLYPHS = 7,
    ERB_RESOURCE_EXTRA_GLYPHS = 8,
    ERB_RESOURCE_COUNT_ = 9,
};

/** @brief Table ids. */
enum
{
    ERB_TABLE_STRINGS = 1,
    ERB_TABLE_CONST32 = 2,
    ERB_TABLE_CONST64 = 3,
    ERB_TABLE_ARENA = 4,
    ERB_TABLE_SLOTS = 5,
    ERB_TABLE_ENUMS = 6,
    ERB_TABLE_ROWS = 7,
    ERB_TABLE_PROPS = 8,
    ERB_TABLE_TEMPLATES = 9,
    ERB_TABLE_BINDINGS = 10,
    ERB_TABLE_FUNCTIONS = 11,
    ERB_TABLE_TYPES = 12,
    ERB_TABLE_CODE = 13,
    ERB_TABLE_ANIMATIONS = 14,
    ERB_TABLE_TAPES = 15,
    ERB_TABLE_IMPORTS = 16,
    ERB_TABLE_INPUTS = 17,
    ERB_TABLE_INTERPOLATIONS = 18, /**< Since minor 1. */
    ERB_TABLE_KEYBOARD = 19,       /**< Since minor 3. */
    ERB_TABLE_CHECKS = 20,         /**< Since minor 6. */
    ERB_TABLE_NATIVE_EVENTS = 21,  /**< Since minor 11. */
    ERB_TABLE_COUNT_ = 22,
};

/** @brief Record sizes, in bytes. */
#define ERB_REGION_SIZE 12u
#define ERB_SLOT_SIZE 16u
#define ERB_SET_SIZE 4u
#define ERB_ROW_SIZE 8u
#define ERB_FIELD_SIZE 8u
#define ERB_PROP_SIZE 8u
#define ERB_TEMPLATE_SIZE 8u
#define ERB_NODE_SIZE 12u
#define ERB_BINDING_SIZE 4u
#define ERB_FUNCTION_SIZE 16u
#define ERB_ANIMATION_SIZE 40u
#define ERB_TAPE_SIZE 16u
#define ERB_TAPE_HEADER_SIZE 16u
#define ERB_PAINT_SIZE 24u
#define ERB_GRADIENT_SIZE 24u
#define ERB_STOP_SIZE 8u
#define ERB_IMPORT_SIZE 8u
#define ERB_NATIVE_EVENT_SIZE 12u
#define ERB_INPUT_SIZE 4u
#define ERB_INTERPOLATION_SIZE 72u
#define ERB_KEYBOARD_HEADER_SIZE 28u
#define ERB_KEYBOARD_GROUP_SIZE 4u
#define ERB_KEYBOARD_KEY_SIZE 8u

/** @brief The strings table: a count, then one u32 byte offset per string. */
#define ERB_STRINGS_COUNT_AT 0u
#define ERB_STRINGS_OFFSETS_AT 4u

/** @brief Tables with a header array (enums, rows, props, templates) start with its u32 count. */
#define ERB_HEADERS_COUNT_AT 0u
#define ERB_HEADERS_FIRST_AT 4u

/** @brief Arena region. */
#define ERB_REGION_KIND_AT 0u
#define ERB_REGION_COUNT_AT 2u
#define ERB_REGION_OFFSET_AT 4u
#define ERB_REGION_BYTES_AT 8u

/** @brief Slot. */
#define ERB_SLOT_TYPE_AT 0u
#define ERB_SLOT_FLAGS_AT 1u
#define ERB_SLOT_DIRTY_BIT_AT 2u
#define ERB_SLOT_OFFSET_AT 4u
#define ERB_SLOT_INIT_AT 8u
#define ERB_SLOT_CAP_AT 12u
#define ERB_SLOT_AUX_AT 14u /**< The enum set of an enum slot, the row layout of a list slot. */

/** @brief Enum set and prop set headers. */
#define ERB_SET_FIRST_AT 0u
#define ERB_SET_COUNT_AT 2u

/** @brief Row layout, and the fields it lists. */
#define ERB_ROW_SIZE_AT 0u
#define ERB_ROW_FIRST_AT 2u
#define ERB_ROW_FIELD_COUNT_AT 4u
#define ERB_FIELD_TYPE_AT 0u
#define ERB_FIELD_CAP_AT 2u
#define ERB_FIELD_OFFSET_AT 4u

/** @brief Constant prop entry. */
#define ERB_PROP_ID_AT 0u
#define ERB_PROP_KIND_AT 2u
#define ERB_PROP_VALUE_AT 4u

/** @brief Template, and its node records. */
#define ERB_TEMPLATE_FIRST_NODE_AT 0u
#define ERB_TEMPLATE_NODE_COUNT_AT 2u
#define ERB_TEMPLATE_KIND_AT 4u
#define ERB_NODE_TYPE_AT 0u
#define ERB_NODE_FLAGS_AT 1u
#define ERB_NODE_CHILDREN_AT 2u
#define ERB_NODE_PROPS_AT 4u
#define ERB_NODE_HANDLE_AT 6u
#define ERB_NODE_BINDINGS_AT 8u
#define ERB_NODE_BINDING_COUNT_AT 10u

/** @brief Binding. */
#define ERB_BINDING_KIND_AT 0u
#define ERB_BINDING_EVENT_AT 1u
#define ERB_BINDING_TARGET_AT 2u

/** @brief Function. */
#define ERB_FUNCTION_CODE_AT 0u
#define ERB_FUNCTION_WORDS_AT 4u
#define ERB_FUNCTION_TYPES_AT 8u
#define ERB_FUNCTION_REGISTERS_AT 12u
#define ERB_FUNCTION_PARAMS_AT 13u
#define ERB_FUNCTION_RESULT_AT 14u
#define ERB_FUNCTION_KIND_AT 15u

/** @brief Animation descriptor. */
#define ERB_ANIMATION_KIND_AT 0u
#define ERB_ANIMATION_EASING_AT 1u
#define ERB_ANIMATION_FLAGS_AT 2u
#define ERB_ANIMATION_TARGET_AT 4u
#define ERB_ANIMATION_PROP_AT 6u
#define ERB_ANIMATION_FIRST_AT 8u
#define ERB_ANIMATION_COUNT_AT 10u
#define ERB_ANIMATION_AMOUNT_AT 12u /**< Duration ms (timing, delay), gap ms (stagger) or iterations (loop). */
#define ERB_ANIMATION_DELAY_MS_AT 16u
#define ERB_ANIMATION_TO_AT 20u
#define ERB_ANIMATION_PARAM0_AT 24u /**< Spring stiffness, decay velocity, or bezier x1. */
#define ERB_ANIMATION_PARAM1_AT 28u /**< Spring damping, decay deceleration, or bezier y1. */
#define ERB_ANIMATION_PARAM2_AT 32u /**< Spring mass, or bezier x2. */
#define ERB_ANIMATION_PARAM3_AT 36u /**< Spring velocity, or bezier y2. */

/** @brief Interpolation: the animated value, its breakpoints, and the inputs then the outputs (f32, or ARGB). */
#define ERB_INTERPOLATION_SLOT_AT 0u
#define ERB_INTERPOLATION_POINT_COUNT_AT 2u
#define ERB_INTERPOLATION_FLAGS_AT 3u
#define ERB_INTERPOLATION_EXTRAPOLATE_LEFT_AT 4u
#define ERB_INTERPOLATION_EXTRAPOLATE_RIGHT_AT 5u
#define ERB_INTERPOLATION_INPUT_AT 8u
#define ERB_INTERPOLATION_OUTPUT_AT 40u

/** @brief Interpolation flag bits. */
#define ERB_INTERPOLATION_FLAG_COLOR 0x01u /**< The outputs are ARGB8888 colors. */
#define ERB_INTERPOLATION_FLAGS_KNOWN ERB_INTERPOLATION_FLAG_COLOR

/** @brief The tapes table's header, a tape record, and the paints, gradients and stops it names. */
#define ERB_TAPES_COUNT_AT 0u
#define ERB_TAPES_PAINTS_AT 4u
#define ERB_TAPES_GRADIENTS_AT 8u
#define ERB_TAPES_STOPS_AT 12u
#define ERB_TAPE_FLOATS_AT 0u
#define ERB_TAPE_FLOAT_COUNT_AT 4u
#define ERB_TAPE_PAINT_FIRST_AT 6u
#define ERB_TAPE_PAINT_COUNT_AT 8u
#define ERB_TAPE_GRADIENT_FIRST_AT 10u
#define ERB_TAPE_GRADIENT_COUNT_AT 12u
#define ERB_PAINT_FILL_AT 0u
#define ERB_PAINT_STROKE_AT 4u
#define ERB_PAINT_STROKE_WIDTH_AT 8u
#define ERB_PAINT_MITER_AT 12u
#define ERB_PAINT_CAP_AT 16u
#define ERB_PAINT_JOIN_AT 17u
#define ERB_PAINT_FILL_RULE_AT 18u
#define ERB_PAINT_FILL_GRADIENT_AT 20u
#define ERB_PAINT_STROKE_GRADIENT_AT 22u
#define ERB_GRADIENT_TYPE_AT 0u
#define ERB_GRADIENT_STOP_COUNT_AT 1u
#define ERB_GRADIENT_STOP_FIRST_AT 2u
#define ERB_GRADIENT_AX_AT 4u
#define ERB_GRADIENT_AY_AT 8u
#define ERB_GRADIENT_BX_AT 12u
#define ERB_GRADIENT_BY_AT 16u
#define ERB_GRADIENT_RADIUS_AT 20u
#define ERB_STOP_COLOR_AT 0u
#define ERB_STOP_POSITION_AT 4u

/** @brief What VPAINT sets in a paint of the scratch (C), and the register type each takes. */
enum
{
    ERB_VECTOR_PAINT_FILL = 0,            /**< int: ARGB. */
    ERB_VECTOR_PAINT_STROKE = 1,          /**< int: ARGB. */
    ERB_VECTOR_PAINT_STROKE_WIDTH = 2,    /**< float. */
    ERB_VECTOR_PAINT_FILL_GRADIENT = 3,   /**< int: a 1-based gradient, or 0 for none. */
    ERB_VECTOR_PAINT_STROKE_GRADIENT = 4, /**< int: a 1-based gradient, or 0 for none. */
    ERB_VECTOR_PAINT_COUNT_ = 5,
};

/** @brief What VGRAD sets in a gradient of the scratch (C): its geometry (floats), then each stop's color (int) and
 *         position (float), stop k at ERB_VECTOR_GRADIENT_STOPS + 2k. */
enum
{
    ERB_VECTOR_GRADIENT_AX = 0,
    ERB_VECTOR_GRADIENT_AY = 1,
    ERB_VECTOR_GRADIENT_BX = 2,
    ERB_VECTOR_GRADIENT_BY = 3,
    ERB_VECTOR_GRADIENT_RADIUS = 4,
    ERB_VECTOR_GRADIENT_STOPS = 8,
};

/** @brief The keyboard table (since minor 3): a header, then its layers, its rows and its keys. A layer names its
 *         rows and a row its keys as `first u16`, `count u16`; each group follows the one before it. */
#define ERB_KEYBOARD_LAYER_COUNT_AT 0u /**< 0 keeps the engine's built-in layout. */
#define ERB_KEYBOARD_ROW_COUNT_AT 2u
#define ERB_KEYBOARD_KEY_COUNT_AT 4u
#define ERB_KEYBOARD_ROW_HEIGHT_AT 6u
#define ERB_KEYBOARD_GRID_COLUMNS_AT 8u
#define ERB_KEYBOARD_KEY_GAP_AT 9u
#define ERB_KEYBOARD_KEY_RADIUS_AT 10u
#define ERB_KEYBOARD_FONT_SIZE_AT 11u
#define ERB_KEYBOARD_PANEL_COLOR_AT 12u
#define ERB_KEYBOARD_KEY_COLOR_AT 16u
#define ERB_KEYBOARD_KEY_ACTIVE_COLOR_AT 20u
#define ERB_KEYBOARD_LABEL_COLOR_AT 24u
#define ERB_KEYBOARD_FIRST_AT 0u
#define ERB_KEYBOARD_COUNT_AT 2u
#define ERB_KEY_LABEL_AT 0u /**< A string, or ERB_NONE for none. */
#define ERB_KEY_TEXT_AT 2u  /**< A string, or ERB_NONE for none. */
#define ERB_KEY_TYPE_AT 4u  /**< An ERKeyboardKeyType. */
#define ERB_KEY_LAYER_AT 5u
#define ERB_KEY_SPAN_AT 6u
#define ERB_KEY_HIGHLIGHT_AT 7u

/** @brief Native import. */
#define ERB_IMPORT_NAME_AT 0u
#define ERB_IMPORT_VERSION_AT 2u
#define ERB_IMPORT_SIGNATURE_AT 4u
#define ERB_IMPORT_MODULE_AT 6u /**< Since minor 12: its optional module, or ERB_NONE for a required one. */

/** @brief Optional modules a package may have. A missing one doesn't refuse the package; AVAIL says which are. */
#define ERB_MAX_OPTIONAL_MODULES 32u

/** @brief Native event handler: the event, as the firmware registers it, and the function it runs. */
#define ERB_NATIVE_EVENT_NAME_AT 0u
#define ERB_NATIVE_EVENT_VERSION_AT 2u
#define ERB_NATIVE_EVENT_SIGNATURE_AT 4u
#define ERB_NATIVE_EVENT_FUNCTION_AT 6u /**< ERB_NONE for an optional module's event nothing subscribes to. */
#define ERB_NATIVE_EVENT_MODULE_AT 8u   /**< Since minor 12, as ERB_IMPORT_MODULE_AT. Then 2 reserved bytes. */

/** @brief Host input. */
#define ERB_INPUT_NAME_AT 0u
#define ERB_INPUT_SLOT_AT 2u

/** @brief Arena region kinds. Each appears at most once; `count` is what each one holds. */
enum
{
    ERB_REGION_SLOTS = 1,      /**< Value slots. count unused. */
    ERB_REGION_DIRTY = 2,      /**< count dirty bits, kept twice (live and the pass's copy). */
    ERB_REGION_STRINGS = 3,    /**< String slot buffers. count unused. */
    ERB_REGION_LISTS = 4,      /**< List pools. count unused. */
    ERB_REGION_HANDLES = 5,    /**< Node handle slots. count unused. */
    ERB_REGION_TIMERS = 6,     /**< count timers, ERB_TIMER_SIZE each. */
    ERB_REGION_ANIMATIONS = 7, /**< count running animation entries, ERB_ANIMATION_ENTRY_SIZE each. */
    ERB_REGION_STACK = 8,      /**< count register cells for VM frames. */
    ERB_REGION_TAPE = 9,       /**< count floats of vector tape scratch. */
    ERB_REGION_EVENTS = 10,    /**< count queued events, ERB_EVENT_SIZE each. */
    ERB_REGION_COUNT_ = 11,
};

/** @brief Fixed arena record sizes. */
#define ERB_TIMER_SIZE 16u
#define ERB_ANIMATION_ENTRY_SIZE 20u
#define ERB_EVENT_SIZE 32u

/** @brief The dirty region keeps its bits twice: live, and the copy the update pass reads. */
#define ERB_DIRTY_COPIES 2u

/** @brief A list pool: a u32 row count, then the rows. */
#define ERB_LIST_HEADER_SIZE 4u

/** @brief Cells each VM frame keeps besides its registers: the caller, the return pc and the caller's base. */
#define ERB_FRAME_CELLS 3u

/** @brief Register and slot value types. A 64-bit type takes two cells, the first even. */
enum
{
    ERB_TYPE_PAIR = 0, /**< In the types table: the second cell of a 64-bit register. */
    ERB_TYPE_INT = 1,
    ERB_TYPE_BOOL = 2,
    ERB_TYPE_FLOAT = 3,
    ERB_TYPE_INT64 = 4,
    ERB_TYPE_DOUBLE = 5,
    ERB_TYPE_STRING = 6,
    ERB_TYPE_ENUM = 7,
    ERB_TYPE_NODE = 8,
    ERB_TYPE_ANIMATION = 9,
    ERB_TYPE_REGISTER_COUNT_ = 10,
    ERB_TYPE_ANIMATED_VALUE = 10, /**< Slot only: an animated value the app owns. */
    ERB_TYPE_LIST = 11,           /**< Slot only: a list pool. */
    ERB_TYPE_COUNT_ = 12,
};

/** @brief Slot flag bits. */
#define ERB_SLOT_TRACKED 0x01u   /**< Has a dirty bit, set when the value changes. */
#define ERB_SLOT_BOOT_ONLY 0x02u /**< Written only by the boot function. */
#define ERB_SLOT_FLAGS_KNOWN 0x03u

/** @brief Template kinds. */
enum
{
    ERB_TEMPLATE_KIND_ROOT = 1,
    ERB_TEMPLATE_KIND_BRANCH = 2,
    ERB_TEMPLATE_KIND_ROW = 3,
};

/** @brief Binding kinds. */
enum
{
    ERB_BIND_EVENT = 1,        /**< event: EREventType; target: a handler for it. */
    ERB_BIND_QUERY = 2,        /**< event: ERResponderQuery; target: a query function for it. */
    ERB_BIND_ANIMATED = 3,     /**< event: prop id; target: an animated-value slot. */
    ERB_BIND_TAPE = 4,         /**< target: a tape, committed when the node is built. */
    ERB_BIND_INTERPOLATED = 5, /**< event: prop id; target: an interpolation. Since minor 1. */
};

/**
 * @brief Function kinds. Below ERB_FUNCTION_KIND_QUERY a kind is the EREventType its handler is bound to; between
 *        ERB_FUNCTION_KIND_QUERY and ERB_FUNCTION_KIND_TIMER it is ERB_FUNCTION_KIND_QUERY + an ERResponderQuery. Only
 * a helper can be called; every other kind is an entry point the runtime runs.
 */
enum
{
    ERB_FUNCTION_KIND_QUERY = 0x40,
    ERB_FUNCTION_KIND_TIMER = 0x80,
    ERB_FUNCTION_KIND_EFFECT = 0x81,
    ERB_FUNCTION_KIND_UPDATE = 0x82,
    ERB_FUNCTION_KIND_BOOT = 0x83,
    ERB_FUNCTION_KIND_HELPER = 0x84,
    ERB_FUNCTION_KIND_ANIMATION_DONE = 0x85,
    ERB_FUNCTION_KIND_NATIVE_EVENT = 0x86, /**< Entered with the event's arguments. Since minor 11. */
};

/** @brief Constant prop value kinds (each ABI prop has one; see erb_props.c). */
enum
{
    ERB_PROP_KIND_I16 = 1,     /**< A whole pixel, int16; ER_LAYOUT_AUTO is auto. */
    ERB_PROP_KIND_U8 = 2,      /**< 0 to 255. */
    ERB_PROP_KIND_U16 = 3,     /**< 0 to 65535. */
    ERB_PROP_KIND_ENUM = 4,    /**< 0 to the prop's largest value. */
    ERB_PROP_KIND_BOOL = 5,    /**< 0 or 1. */
    ERB_PROP_KIND_COLOR = 6,   /**< ARGB8888. */
    ERB_PROP_KIND_FLOAT = 7,   /**< f32 bits. */
    ERB_PROP_KIND_OPACITY = 8, /**< The engine's byte, 0 to 255. Set from a float as app_opacity does. */
    ERB_PROP_KIND_STRING = 9,  /**< A string index; its length within the prop's cap. */
    ERB_PROP_KIND_IMAGE = 10,  /**< A string index naming an image in the asset pack. */
};

/** @brief Animation descriptor kinds. */
enum
{
    ERB_ANIMATION_TIMING = 1,
    ERB_ANIMATION_SPRING = 2,
    ERB_ANIMATION_DECAY = 3,
    ERB_ANIMATION_SEQUENCE = 4,
    ERB_ANIMATION_PARALLEL = 5,
    ERB_ANIMATION_STAGGER = 6,
    ERB_ANIMATION_LOOP = 7,
};

/** @brief Animation descriptor flag bits. */
#define ERB_ANIMATION_FLAG_LOOP 0x01u
#define ERB_ANIMATION_FLAG_LOOP_REVERSE 0x02u
#define ERB_ANIMATION_FLAG_TO_REGISTER 0x04u
#define ERB_ANIMATION_FLAGS_KNOWN 0x07u

/** @brief Event data fields EVT reads. */
enum
{
    ERB_EVENT_FIELD_X = 0,
    ERB_EVENT_FIELD_Y = 1,
    ERB_EVENT_FIELD_DX = 2,
    ERB_EVENT_FIELD_DY = 3,
    ERB_EVENT_FIELD_VX = 4,
    ERB_EVENT_FIELD_VY = 5,
    ERB_EVENT_FIELD_SCROLL_X = 6,
    ERB_EVENT_FIELD_SCROLL_Y = 7,
    ERB_EVENT_FIELD_LAYOUT_X = 8,
    ERB_EVENT_FIELD_LAYOUT_Y = 9,
    ERB_EVENT_FIELD_LAYOUT_WIDTH = 10,
    ERB_EVENT_FIELD_LAYOUT_HEIGHT = 11,
    ERB_EVENT_FIELD_VALUE = 12,
    ERB_EVENT_FIELD_VALUE_START = 13,
    ERB_EVENT_FIELD_FINISHED =
        14, /**< bool: an animation ran to its end (in an ERB_FUNCTION_KIND_ANIMATION_DONE function). */
    ERB_EVENT_FIELD_TOUCHES = 15, /**< int: fingers down when the touch arrived. Since minor 2. */
    ERB_EVENT_FIELD_COUNT_ = 16,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Layout checks
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A panel whose layout fits every screen names this instead of a check. */
#define ERB_CHECK_ALWAYS 0xFFFFFFFFu

/** @brief Values a check may hold on its stack, and in its locals. */
#define ERB_CHECK_STACK 16u
#define ERB_CHECK_LOCALS 48u

/** @brief Words one load may run across all the panels' checks. */
#define ERB_CHECK_BUDGET 65536u

/** @brief A check's word: the op in the low byte, its signed argument above it. */
#define ERB_CHECK_OP(word) ((uint8_t)(word))
#define ERB_CHECK_ARGUMENT(word) ((int32_t)(word) >> 8)

/**
 * @brief A check's ops: a stack machine over doubles with no jumps, run before anything else in the package is
 * checked. It ends at END, which passes when the value on top is true. 0 is never an op.
 */
enum
{
    ERB_CHECK_OP_END = 1,
    ERB_CHECK_OP_WIDTH,  /**< Push the screen's width. */
    ERB_CHECK_OP_HEIGHT, /**< Push the screen's height. */
    ERB_CHECK_OP_PUSHI,  /**< Push the argument. */
    ERB_CHECK_OP_PUSHK,  /**< Push the double in the next two words, low word first. */
    ERB_CHECK_OP_LOAD,   /**< Push local arg. */
    ERB_CHECK_OP_STORE,  /**< Pop into local arg. */
    ERB_CHECK_OP_ADD,
    ERB_CHECK_OP_SUB,
    ERB_CHECK_OP_MUL,
    ERB_CHECK_OP_DIV,
    ERB_CHECK_OP_NEG,
    ERB_CHECK_OP_MATH,  /**< An ERB_MATH_ function, by the argument. */
    ERB_CHECK_OP_TOINT, /**< To a whole number toward zero: NaN as 0, saturating at the int range. */
    ERB_CHECK_OP_IDIV,  /**< Whole-number division with the VM's rules, by an ERB_CHECK_OP_IDIV_ argument. */
    ERB_CHECK_OP_LT,
    ERB_CHECK_OP_GT,
    ERB_CHECK_OP_LE,
    ERB_CHECK_OP_GE,
    ERB_CHECK_OP_EQ,
    ERB_CHECK_OP_NE,
    ERB_CHECK_OP_NOT,
    ERB_CHECK_OP_AND,
    ERB_CHECK_OP_OR,
    ERB_CHECK_OP_SELECT, /**< Pop the alternate, the consequent and the test; push the one the test picks. */
    ERB_CHECK_OP_COUNT_,
};

/** @brief ERB_CHECK_OP_IDIV's arguments. */
enum
{
    ERB_CHECK_OP_IDIV_TRUNC = 0,
    ERB_CHECK_OP_IDIV_FLOOR = 1,
    ERB_CHECK_OP_IDIV_CEIL = 2,
    ERB_CHECK_OP_IDIV_ROUND = 3,
    ERB_CHECK_OP_IDIV_MOD = 4,
    ERB_CHECK_OP_IDIV_COUNT_ = 5,
};

/** @brief FMATH and DMATH functions. Those from ERB_MATH_MIN on take two operands. */
enum
{
    ERB_MATH_SIN = 0,
    ERB_MATH_COS = 1,
    ERB_MATH_TAN = 2,
    ERB_MATH_SQRT = 3,
    ERB_MATH_ABS = 4,
    ERB_MATH_ROUND = 5,
    ERB_MATH_FLOOR = 6,
    ERB_MATH_CEIL = 7,
    ERB_MATH_MIN = 8,
    ERB_MATH_MAX = 9,
    ERB_MATH_ATAN2 = 10,
    ERB_MATH_POW = 11,
    ERB_MATH_MOD = 12,
    ERB_MATH_COUNT_ = 13,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Debug section
 ---------------------------------------------------------------------------------------------------------------------*/

#define ERB_DEBUG_HEADER_SIZE 20u
#define ERB_DEBUG_OLD_HEADER_SIZE 16u
#define ERB_DEBUG_ENTRY_SIZE 16u

#define ERB_DEBUG_BUILD_ID_AT 0u
#define ERB_DEBUG_ENTRY_COUNT_AT 4u
#define ERB_DEBUG_FUNCTION_COUNT_AT 8u
#define ERB_DEBUG_PANEL_AT 10u
#define ERB_DEBUG_STRINGS_AT 12u
#define ERB_DEBUG_LENGTH_AT 16u

#define ERB_DEBUG_ENTRY_FUNCTION_AT 0u
#define ERB_DEBUG_ENTRY_OFFSET_AT 4u
#define ERB_DEBUG_ENTRY_FILE_AT 8u
#define ERB_DEBUG_ENTRY_COLUMN_AT 10u
#define ERB_DEBUG_ENTRY_LINE_AT 12u

/*----------------------------------------------------------------------------------------------------------------------
 - Instructions
 *
 * One 32-bit word: the opcode in bits 0-7, A in 8-15, then B in 16-23 and C in 24-31, or a 16-bit Bx in
 * 16-31. Bx is unsigned for an index and two's complement for a branch offset, which counts words from the
 * next instruction. An opcode of length 2 takes one extension word after it, never an instruction itself.
 *
 * Each entry: name, form, length in words, whether it writes A, and what A, B and C (or Bx) are. A register
 * operand names the type it must have; ERB_K_REG is a register whose type the opcode checks itself, and
 * ERB_K_IMM an immediate or index the opcode checks itself.
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Operand kinds, beyond the register types. */
#define ERB_K_NONE 0u
#define ERB_K_REG 20u
#define ERB_K_IMM 21u

/** @brief Instruction forms. */
#define ERB_F_ABC 0u
#define ERB_F_ABX 1u

#define ERBO_I ERB_TYPE_INT
#define ERBO_B ERB_TYPE_BOOL
#define ERBO_F ERB_TYPE_FLOAT
#define ERBO_L ERB_TYPE_INT64
#define ERBO_D ERB_TYPE_DOUBLE
#define ERBO_S ERB_TYPE_STRING
#define ERBO_E ERB_TYPE_ENUM
#define ERBO_N ERB_TYPE_NODE
#define ERBO_A ERB_TYPE_ANIMATION
#define ERBO_R ERB_K_REG
#define ERBO_K ERB_K_IMM
#define ERBO_X ERB_K_NONE

/* clang-format off */
#define ERB_OPCODES(X)                                                                                                 \
    /* Moves */                                                                                                        \
    X(MOVE,     ERB_F_ABC, 1, 1, ERBO_R, ERBO_R, ERBO_X)  /* A = B, same type */                                       \
    X(LOADI,    ERB_F_ABX, 1, 1, ERBO_R, ERBO_K, ERBO_X)  /* int or bool A = signed Bx */                              \
    X(LOADK,    ERB_F_ABX, 1, 1, ERBO_R, ERBO_K, ERBO_X)  /* int or float A = const32[Bx] */                           \
    X(LOADK64,  ERB_F_ABX, 1, 1, ERBO_R, ERBO_K, ERBO_X)  /* int64 or double A = const64[Bx] */                        \
    X(LOADS,    ERB_F_ABX, 1, 1, ERBO_S, ERBO_K, ERBO_X)  /* A = strings[Bx] */                                        \
    X(LOADE,    ERB_F_ABX, 2, 1, ERBO_E, ERBO_K, ERBO_X)  /* A = value Bx of enum set ext */                           \
    /* Slots */                                                                                                        \
    X(GET32,    ERB_F_ABX, 1, 1, ERBO_R, ERBO_K, ERBO_X)  /* A = 4-byte slot Bx */                                     \
    X(GET64,    ERB_F_ABX, 1, 1, ERBO_R, ERBO_K, ERBO_X)  /* A = 8-byte slot Bx (int64, double, node) */               \
    X(GETS,     ERB_F_ABX, 1, 1, ERBO_S, ERBO_K, ERBO_X)  /* A = string slot Bx's buffer */                            \
    X(SET32,    ERB_F_ABX, 1, 0, ERBO_R, ERBO_K, ERBO_X)  /* slot Bx = A */                                            \
    X(SET64,    ERB_F_ABX, 1, 0, ERBO_R, ERBO_K, ERBO_X)                                                               \
    X(SETS,     ERB_F_ABX, 1, 0, ERBO_S, ERBO_K, ERBO_X)  /* copy A into string slot Bx, cut at its cap */             \
    X(GETNI,    ERB_F_ABC, 2, 1, ERBO_N, ERBO_I, ERBO_X)  /* A = node handle B of handle array slot ext */             \
    /* Lists: ext = slot | field << 16 */                                                                              \
    X(LEN,      ERB_F_ABX, 1, 1, ERBO_I, ERBO_K, ERBO_X)  /* A = rows in list slot Bx */                               \
    X(ROWGET,   ERB_F_ABC, 2, 1, ERBO_R, ERBO_I, ERBO_X)  /* A = field of row B */                                     \
    X(ROWSET,   ERB_F_ABC, 2, 0, ERBO_R, ERBO_I, ERBO_X)  /* field of row B = A */                                     \
    X(APPEND,   ERB_F_ABX, 1, 0, ERBO_R, ERBO_K, ERBO_X)  /* add a row from the registers from A */                    \
    X(CLEAR,    ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)                                                               \
    X(POP,      ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)  /* drop the last row */                                      \
    X(KEEP,     ERB_F_ABX, 1, 0, ERBO_I, ERBO_K, ERBO_X)  /* keep the first A rows */                                  \
    X(SLICE,    ERB_F_ABC, 2, 0, ERBO_I, ERBO_I, ERBO_X)  /* keep rows [A, B), as Array.slice counts them */           \
    /* Whole numbers, saturating */                                                                                    \
    X(ADD,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(SUB,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(MUL,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(DIV,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(FLOORDIV, ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(CEILDIV,  ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(ROUNDDIV, ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(MOD,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(MIN,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(MAX,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_I)                                                               \
    X(NEG,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_X)                                                               \
    X(ABS,      ERB_F_ABC, 1, 1, ERBO_I, ERBO_I, ERBO_X)                                                               \
    X(ADDL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(SUBL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(MULL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(DIVL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(FLOORDIVL,ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(CEILDIVL, ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(ROUNDDIVL,ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(MODL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(MINL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(MAXL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_L)                                                               \
    X(NEGL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_X)                                                               \
    X(ABSL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_L, ERBO_X)                                                               \
    /* Floats and doubles: ext = an ERB_MATH_ function */                                                              \
    X(FADD,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_F, ERBO_F)                                                               \
    X(FSUB,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_F, ERBO_F)                                                               \
    X(FMUL,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_F, ERBO_F)                                                               \
    X(FDIV,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_F, ERBO_F)                                                               \
    X(FNEG,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_F, ERBO_X)                                                               \
    X(FMATH,    ERB_F_ABC, 2, 1, ERBO_F, ERBO_F, ERBO_F)                                                               \
    X(DADD,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_D, ERBO_D)                                                               \
    X(DSUB,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_D, ERBO_D)                                                               \
    X(DMUL,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_D, ERBO_D)                                                               \
    X(DDIV,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_D, ERBO_D)                                                               \
    X(DNEG,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_D, ERBO_X)                                                               \
    X(DMATH,    ERB_F_ABC, 2, 1, ERBO_D, ERBO_D, ERBO_D)                                                               \
    /* Conversions */                                                                                                  \
    X(ITOF,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_I, ERBO_X)                                                               \
    X(FTOI,     ERB_F_ABC, 1, 1, ERBO_I, ERBO_F, ERBO_X)                                                               \
    X(LTOF,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_L, ERBO_X)                                                               \
    X(ITOL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_I, ERBO_X)                                                               \
    X(LTOI,     ERB_F_ABC, 1, 1, ERBO_I, ERBO_L, ERBO_X)                                                               \
    X(DTOI,     ERB_F_ABC, 1, 1, ERBO_I, ERBO_D, ERBO_X)                                                               \
    X(FTOD,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_F, ERBO_X)                                                               \
    X(DTOF,     ERB_F_ABC, 1, 1, ERBO_F, ERBO_D, ERBO_X)                                                               \
    X(ITOD,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_I, ERBO_X)                                                               \
    X(LTOD,     ERB_F_ABC, 1, 1, ERBO_D, ERBO_L, ERBO_X)                                                               \
    X(DTOL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_D, ERBO_X)                                                               \
    X(FTOL,     ERB_F_ABC, 1, 1, ERBO_L, ERBO_F, ERBO_X)                                                               \
    /* Comparisons, into a bool */                                                                                     \
    X(EQ,       ERB_F_ABC, 1, 1, ERBO_B, ERBO_R, ERBO_R)  /* int, bool or enum, both the same type */                  \
    X(LT,       ERB_F_ABC, 1, 1, ERBO_B, ERBO_I, ERBO_I)                                                               \
    X(LE,       ERB_F_ABC, 1, 1, ERBO_B, ERBO_I, ERBO_I)                                                               \
    X(FEQ,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_F, ERBO_F)                                                               \
    X(FLT,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_F, ERBO_F)                                                               \
    X(FLE,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_F, ERBO_F)                                                               \
    X(LEQ,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_L, ERBO_L)                                                               \
    X(LLT,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_L, ERBO_L)                                                               \
    X(LLE,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_L, ERBO_L)                                                               \
    X(DEQ,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_D, ERBO_D)                                                               \
    X(DLT,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_D, ERBO_D)                                                               \
    X(DLE,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_D, ERBO_D)                                                               \
    X(SEQ,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_S, ERBO_S)                                                               \
    X(NOT,      ERB_F_ABC, 1, 1, ERBO_B, ERBO_B, ERBO_X)                                                               \
    /* Control: branches go forward, except LOOP */                                                                    \
    X(JMP,      ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)                                                               \
    X(JT,       ERB_F_ABX, 1, 0, ERBO_B, ERBO_K, ERBO_X)                                                               \
    X(JF,       ERB_F_ABX, 1, 0, ERBO_B, ERBO_K, ERBO_X)                                                               \
    X(JDIRTY,   ERB_F_ABX, 2, 0, ERBO_K, ERBO_K, ERBO_X)  /* jump if dirty word A of this pass & ext != 0 */           \
    X(LOOPSET,  ERB_F_ABX, 1, 1, ERBO_I, ERBO_K, ERBO_X)  /* counter A = Bx */                                         \
    X(LOOP,     ERB_F_ABX, 1, 1, ERBO_I, ERBO_K, ERBO_X)  /* --A; jump back if A > 0 */                                \
    X(RET,      ERB_F_ABC, 1, 0, ERBO_R, ERBO_X, ERBO_X)  /* return A (if the function has a result) */                \
    /* Calls */                                                                                                        \
    X(CALL,     ERB_F_ABX, 1, 0, ERBO_R, ERBO_K, ERBO_X)  /* function Bx, arguments and result from A */               \
    X(CALLN,    ERB_F_ABX, 1, 0, ERBO_R, ERBO_K, ERBO_X)  /* import Bx, arguments from A, the result after them */    \
    /* Text: build into the format buffer, then put it somewhere */                                                    \
    X(FMTK,     ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)  /* strings[Bx] */                                            \
    X(FMTS,     ERB_F_ABC, 1, 0, ERBO_S, ERBO_X, ERBO_X)                                                               \
    X(FMTI,     ERB_F_ABC, 1, 0, ERBO_I, ERBO_X, ERBO_X)                                                               \
    X(FMTL,     ERB_F_ABC, 1, 0, ERBO_L, ERBO_X, ERBO_X)                                                               \
    X(FMTF,     ERB_F_ABC, 1, 0, ERBO_F, ERBO_X, ERBO_X)  /* as String() prints a float */                             \
    X(FMTB,     ERB_F_ABC, 1, 0, ERBO_B, ERBO_X, ERBO_X)  /* true or false */                                          \
    X(FMTE,     ERB_F_ABX, 1, 0, ERBO_E, ERBO_K, ERBO_X)  /* the string of enum set Bx */                              \
    X(FMTFX,    ERB_F_ABC, 1, 0, ERBO_F, ERBO_K, ERBO_X)  /* toFixed(B) */                                             \
    X(FMTPAD,   ERB_F_ABC, 1, 0, ERBO_K, ERBO_K, ERBO_X)  /* padStart(A, char B) on the last piece */                  \
    X(FMTSET,   ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)  /* into string slot Bx */                                    \
    X(FMTTEXT,  ERB_F_ABC, 1, 0, ERBO_X, ERBO_X, ERBO_X)  /* into the props scratch's text */                          \
    X(FMTSPAN,  ERB_F_ABC, 1, 0, ERBO_K, ERBO_X, ERBO_X)  /* into span A's text */                                     \
    /* Props */                                                                                                        \
    X(PROPS,    ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)  /* reset the scratch, then apply prop set Bx (or none) */    \
    X(PSETI,    ERB_F_ABX, 1, 0, ERBO_I, ERBO_K, ERBO_X)  /* prop Bx = A */                                            \
    X(PSETF,    ERB_F_ABX, 1, 0, ERBO_F, ERBO_K, ERBO_X)                                                               \
    X(PSETB,    ERB_F_ABX, 1, 0, ERBO_B, ERBO_K, ERBO_X)                                                               \
    X(PSETS,    ERB_F_ABX, 1, 0, ERBO_S, ERBO_K, ERBO_X)                                                               \
    X(PSETE,    ERB_F_ABX, 2, 0, ERBO_E, ERBO_K, ERBO_X)  /* prop Bx = const32[base + A]; ext = base | set << 16 */    \
    X(SPANS,    ERB_F_ABC, 1, 0, ERBO_K, ERBO_X, ERBO_X)  /* the scratch has A spans */                                \
    X(COMMIT,   ERB_F_ABC, 1, 0, ERBO_N, ERBO_X, ERBO_X)  /* set the scratch on node A */                              \
    /* Nodes */                                                                                                        \
    X(BUILD,    ERB_F_ABC, 2, 1, ERBO_N, ERBO_N, ERBO_N)  /* A = template under B, before C (C == B: last) */   \
    X(DESTROY,  ERB_F_ABC, 1, 0, ERBO_N, ERBO_X, ERBO_X)  /* remove and destroy A and its subtree */                   \
    X(SCROLL,   ERB_F_ABC, 1, 0, ERBO_N, ERBO_F, ERBO_F)                                                               \
    X(FOCUS,    ERB_F_ABC, 1, 0, ERBO_N, ERBO_X, ERBO_X)                                                               \
    X(BLUR,     ERB_F_ABC, 1, 0, ERBO_X, ERBO_X, ERBO_X)                                                               \
    X(INPUT,    ERB_F_ABX, 1, 0, ERBO_N, ERBO_K, ERBO_X)  /* copy text input A's text into string slot Bx */           \
    /* Timers */                                                                                                       \
    X(TSTART,   ERB_F_ABC, 2, 1, ERBO_I, ERBO_I, ERBO_K)  /* A = id of timer ext, every B ms, repeating if C */        \
    X(TCLEAR,   ERB_F_ABC, 1, 0, ERBO_I, ERBO_X, ERBO_X)                                                               \
    /* Animation */                                                                                                    \
    X(ASTART,   ERB_F_ABX, 2, 1, ERBO_A, ERBO_K, ERBO_X)  /* A = anim Bx; ext = done fn | to regs << 16 */          \
    X(ASTOP,    ERB_F_ABC, 1, 0, ERBO_A, ERBO_X, ERBO_X)                                                               \
    X(AVSET,    ERB_F_ABX, 1, 0, ERBO_F, ERBO_K, ERBO_X)  /* animated value slot Bx = A */                             \
    X(AVGET,    ERB_F_ABX, 1, 1, ERBO_F, ERBO_K, ERBO_X)                                                               \
    /* Vectors */                                                                                                      \
    X(VTAPE,    ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)  /* copy tape Bx's floats and paints into the scratch */      \
    X(VSET,     ERB_F_ABX, 1, 0, ERBO_F, ERBO_K, ERBO_X)  /* scratch[Bx] = A */                                        \
    X(VCOMMIT,  ERB_F_ABX, 1, 0, ERBO_N, ERBO_K, ERBO_X)  /* the scratch, with tape Bx's paints, onto node A */        \
    X(VDIRTY,   ERB_F_ABC, 1, 0, ERBO_N, ERBO_I, ERBO_X)  /* node A's dirty rect from ints B .. B + 3 */               \
    /* Event data and clocks */                                                                                        \
    X(EVT,      ERB_F_ABC, 1, 1, ERBO_R, ERBO_K, ERBO_X)  /* A = event field B */                                      \
    X(NOW,      ERB_F_ABC, 1, 1, ERBO_L, ERBO_X, ERBO_X)                                                               \
    X(DATENOW,  ERB_F_ABC, 1, 1, ERBO_L, ERBO_X, ERBO_X)                                                               \
    /* Faults and effects */                                                                                           \
    X(TRAP,     ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)  /* stop with strings[Bx] (or none) */                        \
    X(EFFECT,   ERB_F_ABX, 1, 0, ERBO_X, ERBO_K, ERBO_X)  /* queue effect function Bx after this update pass */      \
    /* Vectors, since minor 3 */                                                                                       \
    X(VPAINT,   ERB_F_ABC, 1, 0, ERBO_R, ERBO_K, ERBO_K)  /* field C of the scratch's paint B = A */                   \
    X(VGRAD,    ERB_F_ABC, 1, 0, ERBO_R, ERBO_K, ERBO_K)  /* field C of the scratch's gradient B = A */                \
    X(VCOMMITP, ERB_F_ABX, 1, 0, ERBO_N, ERBO_K, ERBO_X)  /* the scratch, with its own paints, onto node A */       \
    /* The panel, since minor 6 */                                                                                     \
    X(SCREEN,   ERB_F_ABX, 1, 1, ERBO_I, ERBO_K, ERBO_X)  /* A = the panel's width (Bx 0) or height (Bx 1) */           \
    /* Strings, since minor 7 */                                                                                        \
    X(FMTUPPER, ERB_F_ABC, 1, 0, ERBO_X, ERBO_X, ERBO_X)  /* toUpperCase() on the last piece */                         \
    X(FMTSLICE, ERB_F_ABC, 1, 0, ERBO_I, ERBO_I, ERBO_X)  /* slice(A, B) on the last piece */                           \
    X(SHAS,     ERB_F_ABC, 1, 1, ERBO_B, ERBO_S, ERBO_S)  /* A = B.includes(C) */                                     \
    /* Lists, since minor 8 */                                                                                         \
    X(REMOVE,   ERB_F_ABX, 1, 0, ERBO_I, ERBO_K, ERBO_X)  /* drop row A, moving the rows after it up */               \
    /* Strings, since minor 9 */                                                                                      \
    X(SCMP,     ERB_F_ABC, 1, 1, ERBO_I, ERBO_S, ERBO_S)  /* A = -1, 0 or 1 as B sorts before, with or after C */      \
    /* Lists, since minor 10 */                                                                                        \
    X(SPLIT,    ERB_F_ABC, 2, 0, ERBO_S, ERBO_S, ERBO_I)  /* list slot ext = A.split(B, C), C < 0 for no limit */    \
    /* Native modules, since minor 12 */                                                                               \
    X(AVAIL,    ERB_F_ABX, 1, 1, ERBO_B, ERBO_K, ERBO_X)  /* A = whether optional module Bx is available */      \
    /* Native modules, since minor 13 */                                                                               \
    X(CALLNS,   ERB_F_ABX, 2, 0, ERBO_R, ERBO_K, ERBO_X)  /* CALLN with a string result, into string slot ext */  \
    /* Logging, since minor 15 */                                                                                      \
    X(FMTLOG,   ERB_F_ABC, 1, 0, ERBO_X, ERBO_X, ERBO_X)  /* to the host's log, as one line */
/* clang-format on */

/** @brief Opcodes. 0 is never an instruction. */
enum
{
    ERB_OP_INVALID_ = 0,
#define ERB_OP_ENUM_(name, form, words, writes, operand_a_kind, operand_b_kind, operand_c_kind) ERB_OP_##name,
    ERB_OPCODES(ERB_OP_ENUM_)
#undef ERB_OP_ENUM_
    ERB_OP_COUNT_,
};

/** @brief Registers a function may have: a register operand is one byte. */
#define ERB_MAX_REGISTERS 256u

/** @brief Most decimals FMTFX (toFixed) may ask for. */
#define ERB_FIXED_DIGITS_MAX 20u

/** @brief FMTPAD's pad character: printable ASCII. */
#define ERB_PAD_CHAR_FIRST ' '
#define ERB_PAD_CHAR_LAST '~'

/** @brief A register operand that names no register (BUILD's instance index, ASTART's to values). */
#define ERB_NO_REGISTER 0xFFu

/** @brief Instruction fields. */
#define ERB_OP(word) ((uint8_t)((word) & 0xFFu))
#define ERB_A(word) ((uint8_t)(((word) >> 8) & 0xFFu))
#define ERB_B(word) ((uint8_t)(((word) >> 16) & 0xFFu))
#define ERB_C(word) ((uint8_t)(((word) >> 24) & 0xFFu))
#define ERB_BX(word) ((uint16_t)((word) >> 16))
#define ERB_SBX(word) ((int16_t)((word) >> 16))

/** @brief 16-bit halves of a word: an extension word's index and operand, or a packed handle. */
#define ERB_LOW16(word) ((uint16_t)((uint32_t)(word) & 0xFFFFu))
#define ERB_HIGH16(word) ((uint16_t)((uint32_t)(word) >> 16))
#define ERB_PACK16(low, high) ((uint32_t)(uint16_t)(low) | ((uint32_t)(uint16_t)(high) << 16))

/** @brief An extension word's operand byte (bits 16-23) and top byte (bits 24-31, 0 when unused). */
#define ERB_EXTENSION_OPERAND(extension) ((uint8_t)(((extension) >> 16) & 0xFFu))
#define ERB_EXTENSION_TOP(extension) ((uint8_t)((extension) >> 24))

/** @brief Builds an instruction word. */
#define ERB_ABC(opcode, operand_a, operand_b, operand_c)                                                               \
    ((uint32_t)(opcode) | ((uint32_t)(operand_a) << 8) | ((uint32_t)(operand_b) << 16) | ((uint32_t)(operand_c) << 24))
#define ERB_ABX(opcode, operand_a, operand_bx)                                                                         \
    ((uint32_t)(opcode) | ((uint32_t)(operand_a) << 8) | ((uint32_t)(uint16_t)(operand_bx) << 16))

#endif
