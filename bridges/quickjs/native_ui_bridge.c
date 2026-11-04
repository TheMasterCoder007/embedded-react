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

#include "native_ui_bridge.h"

#include "er_perf.h"
#include "er_scene.h"
#include "native_renderer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Capacity of the JS→ERNode* handle table.
 *
 * Should be >= the engine's ERUI_MAX_NODES so a handle exists for every node the engine
 * can allocate. Slot 0 is reserved as the invalid sentinel; usable slots are [1, N).
 */
#ifndef ER_BRIDGE_MAX_HANDLES
#define ER_BRIDGE_MAX_HANDLES 1024
#endif

#if ER_BRIDGE_MAX_HANDLES > 65536
#error "ER_BRIDGE_MAX_HANDLES must fit a handle (generation * ER_BRIDGE_MAX_HANDLES + slot) in a positive int32"
#endif

/** @brief Invalid handle sentinel returned by createNode on failure, and the slot no handle is bound to. */
#define ER_BRIDGE_HANDLE_INVALID 0

/**
 * @brief A slot's generations cycle 1 to this, so a handle (generation * ER_BRIDGE_MAX_HANDLES + slot) stays positive.
 */
#define ER_BRIDGE_HANDLE_GENERATIONS 0x7FFFu

/**
 * @brief Maximum number of concurrently scheduled timers (setTimeout + setInterval).
 *
 * Fixed pool — no heap growth, matching the embedded ethos. The React scheduler uses at
 * most a couple at a time; app code rarely needs more than a handful.
 */
#ifndef ER_BRIDGE_MAX_TIMERS
#define ER_BRIDGE_MAX_TIMERS 64
#endif

/**
 * @brief Declares a NativeUI method whose body is charged to ER_PERF_JS_MARSHAL.
 *
 * The prop/tape/tree calls are what "marshalling" costs, and they have too many early returns to
 * bracket by hand — so the macro renames the body and puts a timed shim in front of it. With the
 * instrumentation compiled out it expands to the plain signature and nothing is generated at all.
 */
#if ER_PERF_STATS
#define ER_BRIDGE_MARSHAL_FN(name)                                                                                     \
    static JSValue name##_body(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);                   \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)                           \
    {                                                                                                                  \
        er_perf_js_begin(ER_PERF_JS_MARSHAL);                                                                          \
        JSValue result = name##_body(ctx, this_val, argc, argv);                                                       \
        er_perf_js_end(ER_PERF_JS_MARSHAL);                                                                            \
        return result;                                                                                                 \
    }                                                                                                                  \
    static JSValue name##_body(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
#else
#define ER_BRIDGE_MARSHAL_FN(name)                                                                                     \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Slot -> node map. Index 0 is reserved (invalid handle); NULL means free. */
static ERNode* s_node_by_handle[ER_BRIDGE_MAX_HANDLES];

/** @brief Each slot's current generation, so a handle from before the slot was reused no longer matches. */
static uint16_t s_handle_generation[ER_BRIDGE_MAX_HANDLES];

/** @brief Stack of freed slot indices available for reuse. */
static int32_t s_free_stack[ER_BRIDGE_MAX_HANDLES];

/** @brief Number of indices currently on the free stack. */
static int s_free_top = 0;

/** @brief Next never-yet-allocated slot index; starts at 1 (0 is the invalid sentinel). */
static int s_high_water = 1;

/** @brief A createNode failure has been reported; the next success clears it, so a run of failures logs once. */
static bool s_create_failure_logged = false;

/** @brief A stale handle has been reported, so an app that keeps using one logs once. */
static bool s_stale_handle_logged = false;

/** @brief Context the bridge was installed into; used by the event trampoline. */
static JSContext* s_bridge_ctx = NULL;

/** @brief Receives JS exceptions nothing caught; NULL prints them to stderr. */
static ErBridgeUncaughtFn s_uncaught_handler = NULL;

/**
 * @brief JS object mapping encoded (handle, eventType) keys to JS handler functions.
 *
 * Borrowed reference: the object is owned by the NativeUI global (so it lives for the
 * context lifetime and is freed at teardown). Keys are handle * ER_EVENT_TYPE_COUNT_ + type.
 */
static JSValue s_event_handlers;

/**
 * @brief JS object mapping (handle * ER_RESPONDER_QUERY_COUNT + query) to that node's responder
 *        query callback. Borrowed reference, owned by the NativeUI global — same ownership trick
 *        as s_event_handlers (see below).
 */
static JSValue s_query_handlers;

/**
 * @brief One scheduled timer slot. Index into s_timers[] doubles as the registry key.
 *
 * The JS callback and any bound extra args live in s_timer_handlers[slot] (a JS array
 * [callback, ...args]); only the scheduling metadata is kept C-side.
 */
typedef struct
{
    bool active;          /**< Slot is in use. */
    int32_t id;           /**< User-facing id returned to JS (monotonic, > 0). */
    uint32_t due_ms;      /**< er_now_ms() value at which the callback should next fire. */
    uint32_t interval_ms; /**< Repeat period in ms; 0 = one-shot (setTimeout). */
    int extra_arg_count;  /**< Count of extra args forwarded to the callback. */
} ERBridgeTimer;

/** @brief Fixed timer pool. Slot index is the key into s_timer_handlers. */
static ERBridgeTimer s_timers[ER_BRIDGE_MAX_TIMERS];

/** @brief Next id handed out by setTimeout/setInterval; starts at 1 (0 = "no timer"). */
static int32_t s_next_timer_id = 1;

/**
 * @brief JS object mapping a slot index to that timer's [callback, ...args] array.
 *
 * Borrowed reference, owned by the NativeUI global (freed at context teardown), exactly like
 * s_event_handlers. Keeping the callbacks in the JS graph means no C-side ref leaks at shutdown.
 */
static JSValue s_timer_handlers;

/**
 * @brief Max concurrently-pending Animated `.start(cb)` completion callbacks.
 *
 * Should be >= the engine's concurrent-animation cap so every value animation can carry a JS
 * completion callback. Excess animations still run; they just don't notify JS on completion.
 */
#ifndef ER_BRIDGE_MAX_ANIM_COMPLETIONS
#define ER_BRIDGE_MAX_ANIM_COMPLETIONS 64
#endif

/** @brief Per-slot "in use" flags for pending animation-completion callbacks. */
static bool s_anim_complete_active[ER_BRIDGE_MAX_ANIM_COMPLETIONS];

/**
 * @brief JS object mapping a completion slot index to the `.start()` callback.
 *
 * Same ownership trick as the event/timer registries: owned by NativeUI, GC-rooted, freed at
 * teardown. The slot index is the engine animation's on_complete user_data.
 */
static JSValue s_anim_complete_handlers;

/**
 * @brief Slots in the batch registry (s_batch_slots).
 */
enum
{
    ER_BATCH_SLOT_WRAP = 0, /**< JS batcher installed by NativeUI.setBatcher, or undefined. */
    ER_BATCH_SLOT_RUN = 1,  /**< C function the batcher calls to run the pump body. */
};

/**
 * @brief JS object holding the batcher and the pump-body callable. Same ownership trick as the
 *        event/timer registries: owned by NativeUI, GC-rooted, freed at teardown. Borrowed, so it is
 *        only valid once er_bridge_install has run — like every other registry here.
 */
static JSValue s_batch_slots = JS_UNDEFINED;

/**
 * @brief Depth of the open batch scopes (frame pump, event dispatch); 0 = none.
 *
 * A commit asked for inside a scope is remembered rather than run, and the outermost scope runs one
 * at the end. React drives one commit per state update, so without this a frame with three timers
 * firing pays three full engine commits — each one a layout check plus two whole-node-pool walks —
 * to paint what one commit paints.
 */
static int s_batch_depth = 0;

/** @brief A commit was asked for while a batch scope was open, and still owes a run. */
static bool s_commit_pending = false;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — batch scopes
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Opens a batch scope: NativeUI.commit() is folded into the close instead of running now. */
static void batch_enter(void)
{
    s_batch_depth++;
}

/** @brief Closes a batch scope, committing once if anything inside it asked to. */
static void batch_leave(void)
{
    if (s_batch_depth > 0 && --s_batch_depth == 0 && s_commit_pending)
    {
        s_commit_pending = false;
        ER_PERF_JS_BEGIN(ER_PERF_JS_COMMIT);
        er_commit();
        ER_PERF_JS_END(ER_PERF_JS_COMMIT);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — handle table
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Allocates a handle slot and binds it to a node.
 *
 * @param[in] node  Node to bind (must be non-NULL).
 *
 * @return Positive handle, or ER_BRIDGE_HANDLE_INVALID if the table is full.
 */
static int32_t handle_alloc(ERNode* node)
{
    int32_t slot;
    if (s_free_top > 0)
    {
        slot = s_free_stack[--s_free_top];
    }
    else if (s_high_water < ER_BRIDGE_MAX_HANDLES)
    {
        slot = s_high_water++;
    }
    else
    {
        return ER_BRIDGE_HANDLE_INVALID;
    }
    s_node_by_handle[slot] = node;
    s_handle_generation[slot] = (uint16_t)(s_handle_generation[slot] % ER_BRIDGE_HANDLE_GENERATIONS + 1u);
    return (int32_t)s_handle_generation[slot] * ER_BRIDGE_MAX_HANDLES + slot;
}

/**
 * @brief Releases a slot, returning it to the free stack.
 *
 * @param[in] slot  Slot to release.
 */
static void slot_free(int32_t slot)
{
    if (slot <= 0 || slot >= ER_BRIDGE_MAX_HANDLES || s_node_by_handle[slot] == NULL)
    {
        return;
    }
    s_node_by_handle[slot] = NULL;
    s_free_stack[s_free_top++] = slot;
}

/**
 * @brief Finds the slot a handle is bound to. A handle whose node was destroyed finds nothing, even once its slot
 *        holds another node, and the first such handle is logged.
 *
 * @param[in] handle  Handle to resolve. 0 (no node) and negative values (a create that failed) are not stale.
 *
 * @return The slot, or ER_BRIDGE_HANDLE_INVALID.
 */
static int32_t handle_slot(int32_t handle)
{
    if (handle <= 0)
    {
        return ER_BRIDGE_HANDLE_INVALID;
    }
    const int32_t slot = handle % ER_BRIDGE_MAX_HANDLES;
    if (s_node_by_handle[slot] && s_handle_generation[slot] == handle / ER_BRIDGE_MAX_HANDLES)
    {
        return slot;
    }
    if (!s_stale_handle_logged)
    {
        fprintf(stderr, "NativeUI: stale node handle; the call was skipped\n");
        s_stale_handle_logged = true;
    }
    return ER_BRIDGE_HANDLE_INVALID;
}

/**
 * @brief Resolves a handle to its bound node.
 *
 * @param[in] handle  Handle to resolve.
 *
 * @return Bound node, or NULL if the handle is invalid or stale.
 */
static ERNode* handle_node(int32_t handle)
{
    return s_node_by_handle[handle_slot(handle)];
}

/**
 * @brief Resolves a JS argument to its bound node.
 *
 * @param[in] ctx           QuickJS context.
 * @param[in] handle_value  JS value holding an integer handle.
 *
 * @return Bound node, or NULL if the value is not a valid handle.
 */
static ERNode* node_arg(JSContext* ctx, JSValueConst handle_value)
{
    int32_t handle = 0;
    if (JS_ToInt32(ctx, &handle, handle_value) != 0)
    {
        return NULL;
    }
    return handle_node(handle);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — value coercers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Fetches a property, treating undefined/null/exception as "absent".
 *
 * On success the caller owns *out and must JS_FreeValue() it.
 *
 * @param[in]  ctx     QuickJS context.
 * @param[in]  object  Object to read from.
 * @param[in]  key     Property name.
 * @param[out] out     Receives the property value when present.
 *
 * @return true when the property is present and meaningful; false otherwise.
 */
static bool prop_get(JSContext* ctx, JSValueConst object, const char* key, JSValue* out)
{
    JSValue value = JS_GetPropertyStr(ctx, object, key);
    if (JS_IsUndefined(value) || JS_IsNull(value) || JS_IsException(value))
    {
        JS_FreeValue(ctx, value);
        return false;
    }
    *out = value;
    return true;
}

/**
 * @brief Snaps a JS number to a whole int16 pixel the way JavaScript's `Math.round` does.
 *
 * Style values are JS doubles (a scale factor lands `top` on 58.575) but ERProps dimensions are int16, so
 * something has to snap them. Flow B's AOT compiler folds the very same expressions with `Math.round`, so
 * this must agree exactly or the two flows lay one app out a pixel apart. `Math.round` is floor(x + 0.5)
 * — halves go UP, not away from zero — which a plain C cast (truncate toward zero) gets wrong for every
 * fraction, in both directions.
 *
 * Out-of-range and NaN inputs are resolved HERE rather than at the cast: a float-to-int cast is undefined
 * when the value does not fit, and this runs on a spread of MCU toolchains that each pick their own answer.
 *
 * @param[in] value  Value to snap (any double, including NaN and out-of-range).
 *
 * @return value rounded to the nearest whole number (halves up), clamped to int16; 0 for NaN. Note that the
 *         low clamp lands on ER_LAYOUT_AUTO, so a wildly negative dimension reads as "not specified".
 */
static int16_t dim_from_double(double value)
{
    /* NaN compares false against everything, so it would slip past both clamps below and reach the cast.
       A NaN dimension is an app bug (a 0/0 in a computed layout); resolve it to 0 in both flows instead. */
    if (value != value)
    {
        return 0;
    }
    if (value < INT16_MIN)
    {
        return INT16_MIN;
    }
    if (value > INT16_MAX)
    {
        return INT16_MAX;
    }
    return (int16_t)floor(value + 0.5);
}

/**
 * @brief Coerces a numeric JS value to a clamped, rounded int16 pixel dimension.
 *
 * @param[in]  ctx    QuickJS context.
 * @param[in]  value  JS value (expected numeric).
 * @param[out] out    Receives the clamped dimension.
 *
 * @return true on a numeric value; false for strings (e.g. percentages) handled elsewhere.
 */
static bool to_dim(JSContext* ctx, JSValueConst value, int16_t* out)
{
    if (!JS_IsNumber(value))
    {
        return false;
    }
    double number = 0.0;
    if (JS_ToFloat64(ctx, &number, value) != 0)
    {
        return false;
    }
    *out = dim_from_double(number);
    return true;
}

/** @brief Value of the hex digit 'a' (and 'A'). */
#define ER_HEX_LETTER_BASE 10

/** @brief Base of a hex number. */
#define ER_HEX_RADIX 16

/** @brief Scales a #rgb digit to its #rrggbb channel (0xF becomes 0xFF). */
#define ER_HEX_SHORT_DIGIT_SCALE 17

/** @brief Digits in the #rgb, #rrggbb and #rrggbbaa forms. */
#define ER_HEX_SHORT_DIGITS 3
#define ER_HEX_RGB_DIGITS 6
#define ER_HEX_RGBA_DIGITS 8

/** @brief Numbers in rgb() and rgba(). */
#define ER_RGB_FUNCTION_MIN_COMPONENTS 3
#define ER_RGB_FUNCTION_MAX_COMPONENTS 4

/** @brief Alpha of a fully opaque color. */
#define ER_BRIDGE_ALPHA_OPAQUE 255

/** @brief Largest channel or opacity byte, for scaling a 0–1 fraction. */
#define ER_BRIDGE_CHANNEL_MAX_FLOAT 255.0

/**
 * @brief Maps a single hex character to its 0–15 value.
 *
 * @param[in] digit  ASCII hex digit.
 *
 * @return Nibble value, or -1 if digit is not a hex digit.
 */
static int hex_nibble(char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return digit - 'a' + ER_HEX_LETTER_BASE;
    }
    if (digit >= 'A' && digit <= 'F')
    {
        return digit - 'A' + ER_HEX_LETTER_BASE;
    }
    return -1;
}

/**
 * @brief Clamps an int to the 0–255 channel range.
 *
 * @param[in] value  Value to clamp.
 *
 * @return value clamped to [0, 255].
 */
static int clamp_u8(int value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > UINT8_MAX)
    {
        return UINT8_MAX;
    }
    return value;
}

/**
 * @brief Parses a CSS hex color body (the text after '#') into straight-alpha ARGB8888.
 *
 * Accepts #rgb, #rrggbb, and #rrggbbaa forms. Three- and six-digit forms imply full alpha.
 *
 * @param[in]  hex_digits  Hex digits (without the leading '#').
 * @param[out] out         Receives the ARGB8888 color.
 *
 * @return true on a valid hex color; false otherwise.
 */
static bool parse_hex_color(const char* hex_digits, uint32_t* out)
{
    const size_t digit_count = strlen(hex_digits);
    int red, green, blue, alpha = ER_BRIDGE_ALPHA_OPAQUE;

    if (digit_count == ER_HEX_SHORT_DIGITS)
    {
        const int red_digit = hex_nibble(hex_digits[0]);
        const int green_digit = hex_nibble(hex_digits[1]);
        const int blue_digit = hex_nibble(hex_digits[2]);
        if (red_digit < 0 || green_digit < 0 || blue_digit < 0)
        {
            return false;
        }
        red = red_digit * ER_HEX_SHORT_DIGIT_SCALE;
        green = green_digit * ER_HEX_SHORT_DIGIT_SCALE;
        blue = blue_digit * ER_HEX_SHORT_DIGIT_SCALE;
    }
    else if (digit_count == ER_HEX_RGB_DIGITS || digit_count == ER_HEX_RGBA_DIGITS)
    {
        int digits[ER_HEX_RGBA_DIGITS];
        for (size_t digit_index = 0; digit_index < digit_count; digit_index++)
        {
            digits[digit_index] = hex_nibble(hex_digits[digit_index]);
            if (digits[digit_index] < 0)
            {
                return false;
            }
        }
        red = digits[0] * ER_HEX_RADIX + digits[1];
        green = digits[2] * ER_HEX_RADIX + digits[3];
        blue = digits[4] * ER_HEX_RADIX + digits[5];
        if (digit_count == ER_HEX_RGBA_DIGITS)
        {
            alpha = digits[6] * ER_HEX_RADIX + digits[7];
        }
    }
    else
    {
        return false;
    }

    *out = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
    return true;
}

/**
 * @brief Parses a CSS rgb()/rgba() function string into straight-alpha ARGB8888.
 *
 * @param[in]  text  String beginning with "rgb" or "rgba".
 * @param[out] out   Receives the ARGB8888 color.
 *
 * @return true on a valid functional color; false otherwise.
 */
static bool parse_rgb_func(const char* text, uint32_t* out)
{
    const char* cursor = strchr(text, '(');
    if (!cursor)
    {
        return false;
    }
    cursor++;

    double components[ER_RGB_FUNCTION_MAX_COMPONENTS];
    int component_count = 0;
    while (component_count < ER_RGB_FUNCTION_MAX_COMPONENTS)
    {
        char* end = NULL;
        const double component = strtod(cursor, &end);
        if (end == cursor)
        {
            break;
        }
        components[component_count++] = component;
        cursor = end;
        while (*cursor == ' ' || *cursor == ',')
        {
            cursor++;
        }
        if (*cursor == ')' || *cursor == '\0')
        {
            break;
        }
    }
    if (component_count < ER_RGB_FUNCTION_MIN_COMPONENTS)
    {
        return false;
    }

    const int red = clamp_u8((int)(components[0] + 0.5));
    const int green = clamp_u8((int)(components[1] + 0.5));
    const int blue = clamp_u8((int)(components[2] + 0.5));
    const int alpha = component_count >= ER_RGB_FUNCTION_MAX_COMPONENTS
                          ? clamp_u8((int)(components[3] * ER_BRIDGE_CHANNEL_MAX_FLOAT + 0.5))
                          : ER_BRIDGE_ALPHA_OPAQUE;
    *out = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
    return true;
}

/** @brief True when @p text equals the lowercase @p lower, ignoring ASCII case in @p text. */
static bool ascii_ieq(const char* text, const char* lower)
{
    for (; *lower; text++, lower++)
    {
        const char folded = (*text >= 'A' && *text <= 'Z') ? (char)(*text - 'A' + 'a') : *text;
        if (folded != *lower)
        {
            return false;
        }
    }
    return *text == '\0';
}

/**
 * @brief Parses a CSS color string (hex, rgb/rgba, or a named color, in any case).
 *
 * @param[in]  text  Color string.
 * @param[out] out   Receives straight-alpha ARGB8888.
 *
 * @return true when the string was recognised; false otherwise.
 */
static bool parse_css_color(const char* text, uint32_t* out)
{
    while (*text == ' ')
    {
        text++;
    }
    if (*text == '#')
    {
        return parse_hex_color(text + 1, out);
    }
    if (strncmp(text, "rgb", 3) == 0)
    {
        return parse_rgb_func(text, out);
    }

    /* Must match NAMED_COLORS in packages/embedded-react/src/embedded-react/named-colors.js. */
    static const struct
    {
        const char* name;
        uint32_t argb;
    } k_named_colors[] = {
        {"transparent", 0x00000000U},
        {"black", 0xFF000000U},
        {"white", 0xFFFFFFFFU},
        {"red", 0xFFFF0000U},
        {"green", 0xFF008000U},
        {"blue", 0xFF0000FFU},
        {"gray", 0xFF808080U},
        {"grey", 0xFF808080U},
        {"yellow", 0xFFFFFF00U},
        {"cyan", 0xFF00FFFFU},
        {"magenta", 0xFFFF00FFU},
        {"orange", 0xFFFFA500U},
    };
    for (size_t color_index = 0; color_index < sizeof(k_named_colors) / sizeof(k_named_colors[0]); color_index++)
    {
        if (ascii_ieq(text, k_named_colors[color_index].name))
        {
            *out = k_named_colors[color_index].argb;
            return true;
        }
    }
    return false;
}

/** @brief True when two JS values are the same string object (identity, not content). */
static inline bool same_js_string(JSValueConst first, JSValueConst second)
{
    return JS_VALUE_GET_TAG(first) == JS_VALUE_GET_TAG(second) && JS_VALUE_GET_PTR(first) == JS_VALUE_GET_PTR(second);
}

/** @brief Entries in the parsed-color cache. Direct-mapped, so a power of two. */
#define ER_COLOR_CACHE_ENTRIES 16

/** @brief Low pointer bits the allocator keeps constant, dropped so the cache index uses bits that vary. */
#define ER_COLOR_CACHE_POINTER_SHIFT 4

/**
 * @brief Colors already parsed out of a CSS string, keyed by the string object itself.
 *
 * A style's colors are literals in the bundle, so the identical string comes back every commit while
 * rgb()/rgba() parsing runs strtod four times. Each entry owns a reference to its key, which is what
 * makes comparing identity sound: the string cannot be freed and its address reused while it is
 * cached. Emptied by prop_atoms_init() when the runtime changes.
 */
static struct
{
    JSValue key;
    uint32_t argb;
} s_color_cache[ER_COLOR_CACHE_ENTRIES];

/**
 * @brief Coerces a JS color value (number or CSS string) to straight-alpha ARGB8888.
 *
 * @param[in]  ctx    QuickJS context.
 * @param[in]  value  JS value (number is taken as a raw 0xAARRGGBB word).
 * @param[out] out    Receives the ARGB8888 color.
 *
 * @return true on success; false if the value could not be interpreted as a color.
 */
static bool to_color(JSContext* ctx, JSValueConst value, uint32_t* out)
{
    if (JS_IsNumber(value))
    {
        int64_t number = 0;
        if (JS_ToInt64(ctx, &number, value) != 0)
        {
            return false;
        }
        *out = (uint32_t)number;
        return true;
    }
    if (!JS_IsString(value))
    {
        return false;
    }

    const unsigned slot = ((unsigned)((uintptr_t)JS_VALUE_GET_PTR(value) >> ER_COLOR_CACHE_POINTER_SHIFT))
                          & (ER_COLOR_CACHE_ENTRIES - 1U);
    if (same_js_string(value, s_color_cache[slot].key))
    {
        *out = s_color_cache[slot].argb;
        return true;
    }

    const char* text = JS_ToCString(ctx, value);
    if (!text)
    {
        return false;
    }
    const bool ok = parse_css_color(text, out);
    JS_FreeCString(ctx, text);
    if (ok)
    {
        JS_FreeValue(ctx, s_color_cache[slot].key);
        s_color_cache[slot].key = JS_DupValue(ctx, value);
        s_color_cache[slot].argb = *out;
    }
    return ok;
}

/**
 * @brief Coerces a JS angle value (number in degrees, or a "Ndeg"/"Nrad" string) to degrees.
 *
 * @param[in]  ctx    QuickJS context.
 * @param[in]  value  JS value to coerce.
 * @param[out] out    Receives the angle in degrees.
 *
 * @return true on success; false if the value is not a recognisable angle.
 */
static bool to_angle(JSContext* ctx, JSValueConst value, float* out)
{
    if (JS_IsNumber(value))
    {
        double angle = 0.0;
        if (JS_ToFloat64(ctx, &angle, value) != 0)
        {
            return false;
        }
        *out = (float)angle;
        return true;
    }
    if (JS_IsString(value))
    {
        const char* text = JS_ToCString(ctx, value);
        if (!text)
        {
            return false;
        }
        char* end = NULL;
        double angle = strtod(text, &end);
        bool ok = end != text;
        if (ok && strstr(end, "rad") != NULL)
        {
            /* er_scene.h's constant, so this is the exact inverse of the deg->rad the engine applies. */
            angle = angle * (double)ER_RAD2DEG;
        }
        JS_FreeCString(ctx, text);
        if (ok)
        {
            *out = (float)angle;
        }
        return ok;
    }
    return false;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — enum string maps
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maps a flexDirection string to ERFlexDirection. @param[in] token String. @return Enum value. */
static uint8_t map_flex_direction(const char* token)
{
    if (strcmp(token, "row") == 0)
    {
        return ER_FLEX_ROW;
    }
    if (strcmp(token, "row-reverse") == 0)
    {
        return ER_FLEX_ROW_REVERSE;
    }
    if (strcmp(token, "column-reverse") == 0)
    {
        return ER_FLEX_COLUMN_REVERSE;
    }
    return ER_FLEX_COLUMN;
}

/** @brief Maps a justifyContent string to ERFlexJustify. @param[in] token String. @return Enum value. */
static uint8_t map_justify(const char* token)
{
    if (strcmp(token, "center") == 0)
    {
        return ER_JUSTIFY_CENTER;
    }
    if (strcmp(token, "flex-end") == 0)
    {
        return ER_JUSTIFY_FLEX_END;
    }
    if (strcmp(token, "space-between") == 0)
    {
        return ER_JUSTIFY_SPACE_BETWEEN;
    }
    if (strcmp(token, "space-around") == 0)
    {
        return ER_JUSTIFY_SPACE_AROUND;
    }
    if (strcmp(token, "space-evenly") == 0)
    {
        return ER_JUSTIFY_SPACE_EVENLY;
    }
    return ER_JUSTIFY_FLEX_START;
}

/** @brief Maps an align string to ERFlexAlign. @param[in] token String. @return Enum value. */
static uint8_t map_align(const char* token)
{
    if (strcmp(token, "stretch") == 0)
    {
        return ER_ALIGN_STRETCH;
    }
    if (strcmp(token, "flex-start") == 0)
    {
        return ER_ALIGN_FLEX_START;
    }
    if (strcmp(token, "center") == 0)
    {
        return ER_ALIGN_CENTER;
    }
    if (strcmp(token, "flex-end") == 0)
    {
        return ER_ALIGN_FLEX_END;
    }
    return ER_ALIGN_AUTO;
}

/** @brief Maps an alignContent string to ERAlignContent. @param[in] token String. @return Enum value. */
static uint8_t map_align_content(const char* token)
{
    if (strcmp(token, "flex-end") == 0)
    {
        return ER_ALIGN_CONTENT_FLEX_END;
    }
    if (strcmp(token, "center") == 0)
    {
        return ER_ALIGN_CONTENT_CENTER;
    }
    if (strcmp(token, "stretch") == 0)
    {
        return ER_ALIGN_CONTENT_STRETCH;
    }
    if (strcmp(token, "space-between") == 0)
    {
        return ER_ALIGN_CONTENT_SPACE_BETWEEN;
    }
    if (strcmp(token, "space-around") == 0)
    {
        return ER_ALIGN_CONTENT_SPACE_AROUND;
    }
    return ER_ALIGN_CONTENT_FLEX_START;
}

/** @brief Maps a position string to ERPositionType. @param[in] token String. @return Enum value. */
static uint8_t map_position(const char* token)
{
    return strcmp(token, "absolute") == 0 ? ER_POSITION_ABSOLUTE : ER_POSITION_RELATIVE;
}

/** @brief Maps a display string to ERDisplayMode. @param[in] token String. @return Enum value. */
static uint8_t map_display(const char* token)
{
    return strcmp(token, "none") == 0 ? ER_DISPLAY_NONE : ER_DISPLAY_FLEX;
}

/** @brief Maps an overflow string to EROverflow. @param[in] token String. @return Enum value. */
static uint8_t map_overflow(const char* token)
{
    if (strcmp(token, "hidden") == 0)
    {
        return ER_OVERFLOW_HIDDEN;
    }
    if (strcmp(token, "scroll") == 0)
    {
        return ER_OVERFLOW_SCROLL;
    }
    return ER_OVERFLOW_VISIBLE;
}

/** @brief Maps a textAlign string to ERTextAlign. @param[in] token String. @return Enum value. */
static uint8_t map_text_align(const char* token)
{
    if (strcmp(token, "center") == 0)
    {
        return ER_TEXT_ALIGN_CENTER;
    }
    if (strcmp(token, "right") == 0)
    {
        return ER_TEXT_ALIGN_RIGHT;
    }
    return ER_TEXT_ALIGN_LEFT;
}

/** @brief Maps a resizeMode string to ERResizeMode. @param[in] token String. @return Enum value. */
static uint8_t map_resize_mode(const char* token)
{
    if (strcmp(token, "contain") == 0)
    {
        return ER_RESIZE_CONTAIN;
    }
    if (strcmp(token, "stretch") == 0)
    {
        return ER_RESIZE_STRETCH;
    }
    if (strcmp(token, "repeat") == 0)
    {
        return ER_RESIZE_REPEAT;
    }
    if (strcmp(token, "center") == 0)
    {
        return ER_RESIZE_CENTER;
    }
    return ER_RESIZE_COVER;
}

/** @brief Maps a flexWrap string to ERFlexWrap. @param[in] token String. @return Enum value. */
static uint8_t map_flex_wrap(const char* token)
{
    if (strcmp(token, "wrap") == 0)
    {
        return ER_WRAP_WRAP;
    }
    if (strcmp(token, "wrap-reverse") == 0)
    {
        return ER_WRAP_WRAP_REVERSE;
    }
    return ER_WRAP_NOWRAP;
}

/** @brief Maps a fontStyle string to ERFontStyle. @param[in] token String. @return Enum value. */
static uint8_t map_font_style(const char* token)
{
    return strcmp(token, "italic") == 0 ? ER_FONT_STYLE_ITALIC : ER_FONT_STYLE_NORMAL;
}

/** @brief Maps a borderStyle string to ERBorderStyle. @param[in] token String. @return Enum value. */
static uint8_t map_border_style(const char* token)
{
    if (strcmp(token, "dashed") == 0)
    {
        return ER_BORDER_DASHED;
    }
    if (strcmp(token, "dotted") == 0)
    {
        return ER_BORDER_DOTTED;
    }
    return ER_BORDER_SOLID;
}

/** @brief Maps an ellipsizeMode string to ERTextEllipsize. @param[in] token String. @return Enum value. */
static uint8_t map_ellipsize(const char* token)
{
    if (strcmp(token, "head") == 0)
    {
        return ER_TEXT_ELLIPSIZE_HEAD;
    }
    if (strcmp(token, "middle") == 0)
    {
        return ER_TEXT_ELLIPSIZE_MIDDLE;
    }
    if (strcmp(token, "clip") == 0)
    {
        return ER_TEXT_ELLIPSIZE_CLIP;
    }
    return ER_TEXT_ELLIPSIZE_TAIL;
}

/** @brief Maps a textDecorationLine string to ERTextDecoration. @param[in] token String. @return Enum value. */
static uint8_t map_text_decoration(const char* token)
{
    if (strcmp(token, "underline") == 0)
    {
        return ER_TEXT_DECORATION_UNDERLINE;
    }
    if (strcmp(token, "line-through") == 0)
    {
        return ER_TEXT_DECORATION_LINE_THROUGH;
    }
    return ER_TEXT_DECORATION_NONE;
}

/** @brief Maps a pointerEvents string to ERPointerEvents. @param[in] token String. @return Enum value. */
static uint8_t map_pointer_events(const char* token)
{
    if (strcmp(token, "none") == 0)
    {
        return ER_POINTER_EVENTS_NONE;
    }
    if (strcmp(token, "box-only") == 0)
    {
        return ER_POINTER_EVENTS_BOX_ONLY;
    }
    if (strcmp(token, "box-none") == 0)
    {
        return ER_POINTER_EVENTS_BOX_NONE;
    }
    return ER_POINTER_EVENTS_AUTO;
}

/** @brief Maps a node-type string to ERNodeType. @param[in] token String. @return Enum value (View on miss). */
static ERNodeType map_node_type(const char* token)
{
    if (strcmp(token, "Text") == 0)
    {
        return ER_NODE_TEXT;
    }
    if (strcmp(token, "Image") == 0)
    {
        return ER_NODE_IMAGE;
    }
    if (strcmp(token, "ScrollView") == 0)
    {
        return ER_NODE_SCROLL_VIEW;
    }
    if (strcmp(token, "FlatList") == 0)
    {
        return ER_NODE_FLAT_LIST;
    }
    if (strcmp(token, "Pressable") == 0)
    {
        return ER_NODE_PRESSABLE;
    }
    if (strcmp(token, "TextInput") == 0)
    {
        return ER_NODE_TEXT_INPUT;
    }
    if (strcmp(token, "ActivityIndicator") == 0)
    {
        return ER_NODE_ACTIVITY_INDICATOR;
    }
    if (strcmp(token, "Switch") == 0)
    {
        return ER_NODE_SWITCH;
    }
    if (strcmp(token, "Modal") == 0)
    {
        return ER_NODE_MODAL;
    }
    if (strcmp(token, "Svg") == 0)
    {
        return ER_NODE_VECTOR;
    }
    if (strcmp(token, "Dial") == 0)
    {
        return ER_NODE_ARC;
    }
    return ER_NODE_VIEW;
}

/** @brief Maps a Dial `cap` token to ERArcCap. @param[in] token String. @return Enum value (default butt). */
static uint8_t map_arc_cap(const char* token)
{
    if (strcmp(token, "round") == 0)
    {
        return ER_ARC_CAP_ROUND;
    }
    return ER_ARC_CAP_BUTT;
}

/** @brief Maps a Dial `knob` token to ERArcKnob. @param[in] token String. @return Enum value (default none). */
static uint8_t map_arc_knob(const char* token)
{
    if (strcmp(token, "circle") == 0)
    {
        return ER_ARC_KNOB_CIRCLE;
    }
    if (strcmp(token, "image") == 0)
    {
        return ER_ARC_KNOB_IMAGE;
    }
    if (strcmp(token, "child") == 0)
    {
        return ER_ARC_KNOB_CHILD;
    }
    return ER_ARC_KNOB_NONE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — props marshalling
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initialises an ERProps to engine defaults (layout fields auto, fully opaque).
 *
 * A zero-initialised ERProps would set every layout field to 0 (a valid explicit value) and
 * opacity to 0 (fully transparent), so the layout-auto fields are reset to ER_LAYOUT_AUTO and
 * opacity to 255 to match a freshly created node.
 *
 * @param[out] props  Props to initialise.
 */
static void props_init_defaults(ERProps* props)
{
    /* The default seeding lives in the engine (er_props_default) so the Flow B AOT codegen produces
       byte-identical prop bags. This wrapper stays for the call sites + the doc comment above. */
    er_props_default(props);
}

/**
 * @brief Every JS prop name apply_props() understands, as an index into s_prop_slots.
 *
 * Order doesn't matter for correctness (lookup is by atom, not by enum value) — grouped here to
 * mirror apply_props()'s own layout/text/etc. sections for readability.
 */
typedef enum
{
    PROP_WIDTH,
    PROP_HEIGHT,
    PROP_MIN_WIDTH,
    PROP_MAX_WIDTH,
    PROP_MIN_HEIGHT,
    PROP_MAX_HEIGHT,
    PROP_TOP,
    PROP_LEFT,
    PROP_RIGHT,
    PROP_BOTTOM,
    PROP_MARGIN,
    PROP_MARGIN_TOP,
    PROP_MARGIN_RIGHT,
    PROP_MARGIN_BOTTOM,
    PROP_MARGIN_LEFT,
    PROP_MARGIN_HORIZONTAL,
    PROP_MARGIN_VERTICAL,
    PROP_PADDING,
    PROP_PADDING_TOP,
    PROP_PADDING_RIGHT,
    PROP_PADDING_BOTTOM,
    PROP_PADDING_LEFT,
    PROP_PADDING_HORIZONTAL,
    PROP_PADDING_VERTICAL,
    PROP_GAP,
    PROP_ROW_GAP,
    PROP_COLUMN_GAP,
    PROP_FLEX,
    PROP_FLEX_GROW,
    PROP_FLEX_SHRINK,
    PROP_FLEX_BASIS,
    PROP_FLEX_DIRECTION,
    PROP_FLEX_WRAP,
    PROP_JUSTIFY_CONTENT,
    PROP_ALIGN_ITEMS,
    PROP_ALIGN_SELF,
    PROP_ALIGN_CONTENT,
    PROP_POSITION,
    PROP_DISPLAY,
    PROP_OVERFLOW,
    PROP_POINTER_EVENTS,
    PROP_DELAY_LONG_PRESS,
    PROP_ASPECT_RATIO,
    PROP_BACKGROUND_COLOR,
    PROP_OPACITY,
    PROP_BORDER_RADIUS,
    PROP_BORDER_TOP_LEFT_RADIUS,
    PROP_BORDER_TOP_RIGHT_RADIUS,
    PROP_BORDER_BOTTOM_LEFT_RADIUS,
    PROP_BORDER_BOTTOM_RIGHT_RADIUS,
    PROP_BORDER_WIDTH,
    PROP_BORDER_LEFT_WIDTH,
    PROP_BORDER_TOP_WIDTH,
    PROP_BORDER_RIGHT_WIDTH,
    PROP_BORDER_BOTTOM_WIDTH,
    PROP_BORDER_COLOR,
    PROP_BORDER_LEFT_COLOR,
    PROP_BORDER_TOP_COLOR,
    PROP_BORDER_RIGHT_COLOR,
    PROP_BORDER_BOTTOM_COLOR,
    PROP_BORDER_STYLE,
    PROP_Z_INDEX,
    PROP_TEXT,
    PROP_FONT_FAMILY,
    PROP_COLOR,
    PROP_FONT_SIZE,
    PROP_FONT_WEIGHT,
    PROP_FONT_STYLE,
    PROP_TEXT_ALIGN,
    PROP_TEXT_DECORATION_LINE,
    PROP_NUMBER_OF_LINES,
    PROP_ELLIPSIZE_MODE,
    PROP_LINE_HEIGHT,
    PROP_LETTER_SPACING,
    PROP_IMAGE_NAME,
    PROP_RESIZE_MODE,
    PROP_TINT_COLOR,
    PROP_TRANSFORM,
    PROP_TRANSFORM_ORIGIN,
    PROP_SHADOW_COLOR,
    PROP_SHADOW_OFFSET,
    PROP_SHADOW_OPACITY,
    PROP_SHADOW_RADIUS,
    PROP_ELEVATION,
    PROP_ANIMATING,
    PROP_VALUE,
    PROP_TRACK_COLOR,
    PROP_THUMB_COLOR,
    PROP_PLACEHOLDER,
    PROP_PLACEHOLDER_TEXT_COLOR,
    PROP_CURSOR_COLOR,
    PROP_EDITABLE,
    PROP_SECURE_TEXT_ENTRY,
    PROP_VISIBLE,
    PROP_BACKDROP_COLOR,
    PROP_MIN,
    PROP_MAX,
    PROP_START_ANGLE,
    PROP_SWEEP_ANGLE,
    PROP_STEP,
    PROP_GAP_ANGLE,
    PROP_THICKNESS,
    PROP_BAND_THICKNESS,
    PROP_KNOB_SIZE,
    PROP_KNOB_BORDER_WIDTH,
    PROP_INDICATOR_COLOR,
    PROP_BAND_COLOR,
    PROP_KNOB_COLOR,
    PROP_KNOB_BORDER_COLOR,
    PROP_SEGMENTS,
    PROP_CAP,
    PROP_KNOB,
    PROP_KNOB_IMAGE,
    PROP_ADJUSTABLE,
    PROP_RANGE,
    PROP_VALUE_START,
    PROP_MIN_SPAN,
    PROP_INDICATOR_GRADIENT,
    PROP_COUNT_,
} PropId;

/** @brief JS prop names, indexed by PropId. Designated initializers keep this in sync with the enum. */
static const char* const k_prop_names[PROP_COUNT_] = {
    [PROP_WIDTH] = "width",
    [PROP_HEIGHT] = "height",
    [PROP_MIN_WIDTH] = "minWidth",
    [PROP_MAX_WIDTH] = "maxWidth",
    [PROP_MIN_HEIGHT] = "minHeight",
    [PROP_MAX_HEIGHT] = "maxHeight",
    [PROP_TOP] = "top",
    [PROP_LEFT] = "left",
    [PROP_RIGHT] = "right",
    [PROP_BOTTOM] = "bottom",
    [PROP_MARGIN] = "margin",
    [PROP_MARGIN_TOP] = "marginTop",
    [PROP_MARGIN_RIGHT] = "marginRight",
    [PROP_MARGIN_BOTTOM] = "marginBottom",
    [PROP_MARGIN_LEFT] = "marginLeft",
    [PROP_MARGIN_HORIZONTAL] = "marginHorizontal",
    [PROP_MARGIN_VERTICAL] = "marginVertical",
    [PROP_PADDING] = "padding",
    [PROP_PADDING_TOP] = "paddingTop",
    [PROP_PADDING_RIGHT] = "paddingRight",
    [PROP_PADDING_BOTTOM] = "paddingBottom",
    [PROP_PADDING_LEFT] = "paddingLeft",
    [PROP_PADDING_HORIZONTAL] = "paddingHorizontal",
    [PROP_PADDING_VERTICAL] = "paddingVertical",
    [PROP_GAP] = "gap",
    [PROP_ROW_GAP] = "rowGap",
    [PROP_COLUMN_GAP] = "columnGap",
    [PROP_FLEX] = "flex",
    [PROP_FLEX_GROW] = "flexGrow",
    [PROP_FLEX_SHRINK] = "flexShrink",
    [PROP_FLEX_BASIS] = "flexBasis",
    [PROP_FLEX_DIRECTION] = "flexDirection",
    [PROP_FLEX_WRAP] = "flexWrap",
    [PROP_JUSTIFY_CONTENT] = "justifyContent",
    [PROP_ALIGN_ITEMS] = "alignItems",
    [PROP_ALIGN_SELF] = "alignSelf",
    [PROP_ALIGN_CONTENT] = "alignContent",
    [PROP_POSITION] = "position",
    [PROP_DISPLAY] = "display",
    [PROP_OVERFLOW] = "overflow",
    [PROP_POINTER_EVENTS] = "pointerEvents",
    [PROP_DELAY_LONG_PRESS] = "delayLongPress",
    [PROP_ASPECT_RATIO] = "aspectRatio",
    [PROP_BACKGROUND_COLOR] = "backgroundColor",
    [PROP_OPACITY] = "opacity",
    [PROP_BORDER_RADIUS] = "borderRadius",
    [PROP_BORDER_TOP_LEFT_RADIUS] = "borderTopLeftRadius",
    [PROP_BORDER_TOP_RIGHT_RADIUS] = "borderTopRightRadius",
    [PROP_BORDER_BOTTOM_LEFT_RADIUS] = "borderBottomLeftRadius",
    [PROP_BORDER_BOTTOM_RIGHT_RADIUS] = "borderBottomRightRadius",
    [PROP_BORDER_WIDTH] = "borderWidth",
    [PROP_BORDER_LEFT_WIDTH] = "borderLeftWidth",
    [PROP_BORDER_TOP_WIDTH] = "borderTopWidth",
    [PROP_BORDER_RIGHT_WIDTH] = "borderRightWidth",
    [PROP_BORDER_BOTTOM_WIDTH] = "borderBottomWidth",
    [PROP_BORDER_COLOR] = "borderColor",
    [PROP_BORDER_LEFT_COLOR] = "borderLeftColor",
    [PROP_BORDER_TOP_COLOR] = "borderTopColor",
    [PROP_BORDER_RIGHT_COLOR] = "borderRightColor",
    [PROP_BORDER_BOTTOM_COLOR] = "borderBottomColor",
    [PROP_BORDER_STYLE] = "borderStyle",
    [PROP_Z_INDEX] = "zIndex",
    [PROP_TEXT] = "text",
    [PROP_FONT_FAMILY] = "fontFamily",
    [PROP_COLOR] = "color",
    [PROP_FONT_SIZE] = "fontSize",
    [PROP_FONT_WEIGHT] = "fontWeight",
    [PROP_FONT_STYLE] = "fontStyle",
    [PROP_TEXT_ALIGN] = "textAlign",
    [PROP_TEXT_DECORATION_LINE] = "textDecorationLine",
    [PROP_NUMBER_OF_LINES] = "numberOfLines",
    [PROP_ELLIPSIZE_MODE] = "ellipsizeMode",
    [PROP_LINE_HEIGHT] = "lineHeight",
    [PROP_LETTER_SPACING] = "letterSpacing",
    [PROP_IMAGE_NAME] = "imageName",
    [PROP_RESIZE_MODE] = "resizeMode",
    [PROP_TINT_COLOR] = "tintColor",
    [PROP_TRANSFORM] = "transform",
    [PROP_TRANSFORM_ORIGIN] = "transformOrigin",
    [PROP_SHADOW_COLOR] = "shadowColor",
    [PROP_SHADOW_OFFSET] = "shadowOffset",
    [PROP_SHADOW_OPACITY] = "shadowOpacity",
    [PROP_SHADOW_RADIUS] = "shadowRadius",
    [PROP_ELEVATION] = "elevation",
    [PROP_ANIMATING] = "animating",
    [PROP_VALUE] = "value",
    [PROP_TRACK_COLOR] = "trackColor",
    [PROP_THUMB_COLOR] = "thumbColor",
    [PROP_PLACEHOLDER] = "placeholder",
    [PROP_PLACEHOLDER_TEXT_COLOR] = "placeholderTextColor",
    [PROP_CURSOR_COLOR] = "cursorColor",
    [PROP_EDITABLE] = "editable",
    [PROP_SECURE_TEXT_ENTRY] = "secureTextEntry",
    [PROP_VISIBLE] = "visible",
    [PROP_BACKDROP_COLOR] = "backdropColor",
    [PROP_MIN] = "min",
    [PROP_MAX] = "max",
    [PROP_START_ANGLE] = "startAngle",
    [PROP_SWEEP_ANGLE] = "sweepAngle",
    [PROP_STEP] = "step",
    [PROP_GAP_ANGLE] = "gapAngle",
    [PROP_THICKNESS] = "thickness",
    [PROP_BAND_THICKNESS] = "bandThickness",
    [PROP_KNOB_SIZE] = "knobSize",
    [PROP_KNOB_BORDER_WIDTH] = "knobBorderWidth",
    [PROP_INDICATOR_COLOR] = "indicatorColor",
    [PROP_BAND_COLOR] = "bandColor",
    [PROP_KNOB_COLOR] = "knobColor",
    [PROP_KNOB_BORDER_COLOR] = "knobBorderColor",
    [PROP_SEGMENTS] = "segments",
    [PROP_CAP] = "cap",
    [PROP_KNOB] = "knob",
    [PROP_KNOB_IMAGE] = "knobImage",
    [PROP_ADJUSTABLE] = "adjustable",
    [PROP_RANGE] = "range",
    [PROP_VALUE_START] = "valueStart",
    [PROP_MIN_SPAN] = "minSpan",
    [PROP_INDICATOR_GRADIENT] = "indicatorGradient",
};

/** @brief (atom, id) pair; s_prop_atoms is sorted by atom value once, for prop_id_from_atom()'s bsearch. */
typedef struct
{
    JSAtom atom;
    PropId id;
} PropAtomEntry;

static PropAtomEntry s_prop_atoms[PROP_COUNT_];

/** @brief Runtime s_prop_atoms was built against; atoms are runtime-scoped, not context-scoped. */
static JSRuntime* s_prop_atoms_rt = NULL;

/**
 * @brief The last token seen for each enum-valued prop, and the enum it mapped to.
 *
 * These values are literals in the bundle (`flexDirection: 'row'`), so the identical string comes back
 * every commit and a pointer compare replaces JS_ToCString (which copies the string out) plus a strcmp
 * ladder. Like the color cache, the entry owns a reference to its key so identity stays meaningful.
 * Emptied by prop_atoms_init() when the runtime changes.
 */
static JSValue s_enum_memo_string[PROP_COUNT_];
static uint8_t s_enum_memo_value[PROP_COUNT_];

static int prop_atom_entry_cmp(const void* first, const void* second)
{
    const JSAtom first_atom = ((const PropAtomEntry*)first)->atom;
    const JSAtom second_atom = ((const PropAtomEntry*)second)->atom;
    return (first_atom > second_atom) - (first_atom < second_atom);
}

/**
 * @brief Interns every apply_props() key into s_prop_atoms, sorted for prop_id_from_atom()'s bsearch.
 *
 * Also empties the two marshalling caches (enum tokens, parsed colors), whose keys are strings from
 * the runtime that is going away.
 *
 * Atoms live in the JSRuntime, not the JSContext, and er_bridge_install() runs on every hot-reload
 * (see its doc comment: the simulator's live reload frees the JSContext but keeps the JSRuntime, so
 * a fresh context is installed on the same runtime). This only rebuilds the table when the runtime
 * actually changed, so a hot-reload doesn't re-intern (and leak a ref on) the same 90-odd atoms.
 *
 * @param[in] ctx  QuickJS context to intern the names into.
 */
static void prop_atoms_init(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (rt == s_prop_atoms_rt)
    {
        return;
    }
    for (int entry_index = 0; entry_index < PROP_COUNT_; entry_index++)
    {
        s_prop_atoms[entry_index].atom = JS_NewAtom(ctx, k_prop_names[entry_index]);
        s_prop_atoms[entry_index].id = (PropId)entry_index;
        /* Strings held against the old runtime died with it — drop the keys, don't unref them. */
        s_enum_memo_string[entry_index] = JS_UNDEFINED;
    }
    for (int entry_index = 0; entry_index < ER_COLOR_CACHE_ENTRIES; entry_index++)
    {
        s_color_cache[entry_index].key = JS_UNDEFINED;
    }
    qsort(s_prop_atoms, PROP_COUNT_, sizeof(s_prop_atoms[0]), prop_atom_entry_cmp);
    s_prop_atoms_rt = rt;
}

/**
 * @brief Drops the bridge state cached against the current JSRuntime.
 */
void er_bridge_set_uncaught_handler(ErBridgeUncaughtFn handler)
{
    s_uncaught_handler = handler;
}

void er_bridge_release_runtime(void)
{
    /* Forget, don't unref: every cached atom and string belongs to the runtime being torn down.
     * The next prop_atoms_init() re-interns and empties the caches. */
    s_prop_atoms_rt = NULL;
}

/**
 * @brief Maps a JS own-property atom to the PropId apply_props() knows it by.
 *
 * @param[in] atom  Atom from JS_GetOwnPropertyNames() on the props object.
 *
 * @return The matching PropId, or PROP_COUNT_ if apply_props() doesn't read this key.
 */
static PropId prop_id_from_atom(JSAtom atom)
{
    const PropAtomEntry key = {.atom = atom, .id = PROP_COUNT_};
    const PropAtomEntry* hit = bsearch(&key, s_prop_atoms, PROP_COUNT_, sizeof(s_prop_atoms[0]), prop_atom_entry_cmp);
    return hit ? hit->id : PROP_COUNT_;
}

/**
 * @brief Per-call scratch built by apply_props(): s_prop_slots[id] is the (owned) value read for
 * that key on the current call's object, or JS_UNDEFINED if the key was absent.
 *
 * NOTE: Static storage is safe because the bridge is single-threaded and apply_props() is not re-entrant.
 */
static JSValue s_prop_slots[PROP_COUNT_];

/**
 * @brief Maps an enum-valued prop's JS string to its engine enum, memoised per prop id.
 *
 * @param[in]  ctx     QuickJS context.
 * @param[in]  value   The slot value (already known to be present).
 * @param[in]  id      Prop this value belongs to — the memo key.
 * @param[in]  mapper  Mapper for this prop's token set.
 * @param[out] out     Receives the mapped enum.
 *
 * @return true when @p value produced a value; false leaves @p out alone.
 */
static bool enum_from_slot(JSContext* ctx, JSValueConst value, PropId id, uint8_t (*mapper)(const char*), uint8_t* out)
{
    const bool cacheable = JS_IsString(value);
    if (cacheable && same_js_string(value, s_enum_memo_string[id]))
    {
        *out = s_enum_memo_value[id];
        return true;
    }

    const char* token = JS_ToCString(ctx, value);
    if (!token)
    {
        return false;
    }
    *out = mapper(token);
    JS_FreeCString(ctx, token);
    if (cacheable)
    {
        JS_FreeValue(ctx, s_enum_memo_string[id]);
        s_enum_memo_string[id] = JS_DupValue(ctx, value);
        s_enum_memo_value[id] = *out;
    }
    return true;
}

/* Marshalling convenience macros — each reads one slot (populated from the object's own keys by
   apply_props()) into the ERProps `props`. They share the locals (ctx, props) of apply_props() and are
   #undef'd at its end. */

#define ER_DIM(id, field)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _value = s_prop_slots[id];                                                                        \
        if (!JS_IsUndefined(_value))                                                                                   \
        {                                                                                                              \
            int16_t _dimension;                                                                                        \
            if (to_dim(ctx, _value, &_dimension))                                                                      \
            {                                                                                                          \
                props.field = _dimension;                                                                              \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_COL(id, field)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _value = s_prop_slots[id];                                                                        \
        if (!JS_IsUndefined(_value))                                                                                   \
        {                                                                                                              \
            uint32_t _color;                                                                                           \
            if (to_color(ctx, _value, &_color))                                                                        \
            {                                                                                                          \
                props.field = _color;                                                                                  \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_ENUM(id, field, fn)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _value = s_prop_slots[id];                                                                        \
        if (!JS_IsUndefined(_value))                                                                                   \
        {                                                                                                              \
            enum_from_slot(ctx, _value, id, fn, &props.field);                                                         \
        }                                                                                                              \
    } while (0)

#define ER_U8(id, field)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _value = s_prop_slots[id];                                                                        \
        if (!JS_IsUndefined(_value))                                                                                   \
        {                                                                                                              \
            int32_t _number;                                                                                           \
            if (JS_ToInt32(ctx, &_number, _value) == 0)                                                                \
            {                                                                                                          \
                props.field = (uint8_t)clamp_u8(_number);                                                              \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_F32(id, field)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _value = s_prop_slots[id];                                                                        \
        if (!JS_IsUndefined(_value))                                                                                   \
        {                                                                                                              \
            double _number;                                                                                            \
            if (JS_ToFloat64(ctx, &_number, _value) == 0)                                                              \
            {                                                                                                          \
                props.field = (float)_number;                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_STR(id, field, maxlen)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _value = s_prop_slots[id];                                                                        \
        if (!JS_IsUndefined(_value))                                                                                   \
        {                                                                                                              \
            const char* _text = JS_ToCString(ctx, _value);                                                             \
            if (_text)                                                                                                 \
            {                                                                                                          \
                strncpy(props.field, _text, (maxlen));                                                                 \
                props.field[(maxlen)] = '\0';                                                                          \
                JS_FreeCString(ctx, _text);                                                                            \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

/**
 * @brief Reads the `flex` shorthand and expands it to grow/shrink/basis with RN semantics.
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched `flex` value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_flex_shorthand(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    double flex = 0.0;
    if (JS_ToFloat64(ctx, &flex, value) == 0)
    {
        if (flex > 0.0)
        {
            props->flex_grow = dim_from_double(flex);
            props->flex_shrink = 1;
            props->flex_basis = 0;
        }
        else if (flex == 0.0)
        {
            props->flex_grow = 0;
            props->flex_shrink = 0;
        }
        else
        {
            props->flex_grow = 0;
            props->flex_shrink = 1;
        }
    }
}

/**
 * @brief Reads `flexBasis`, supporting both numeric pixels and percentage strings.
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched `flexBasis` value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_flex_basis(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    if (JS_IsNumber(value))
    {
        int16_t pixels;
        if (to_dim(ctx, value, &pixels))
        {
            props->flex_basis = pixels;
        }
    }
    else if (JS_IsString(value))
    {
        const char* text = JS_ToCString(ctx, value);
        if (text)
        {
            char* end = NULL;
            const double percent = strtod(text, &end);
            if (end != text && *end == '%' && !isnan(percent))
            {
                /* `flexBasis: '0%'` is the CSS `flex: 1 1 0%` idiom, and 0.0 is the percentage field's
                 * "not set" sentinel — so it lowers to the pixel field, which means the same thing. */
                if (percent == 0.0)
                {
                    props->flex_basis = 0;
                }
                else
                {
                    props->flex_basis_percent = (float)percent;
                }
            }
            JS_FreeCString(ctx, text);
        }
    }
}

/**
 * @brief Reads a dimension value that may be a number (pixels) or a percentage string.
 *
 * A numeric value sets the px field; a `"N%"` string sets the percent field, which takes precedence
 * over the px field. The engine resolves the percentage against whichever box that prop measures
 * from — the parent's content size for a size, its containing block for an inset.
 *
 * @param[in]  ctx      QuickJS context.
 * @param[in]  value    Pre-fetched value (e.g. `width`), JS_UNDEFINED if absent.
 * @param[out] pixels   Receives the pixel value when the JS value is numeric.
 * @param[out] percent  Receives the percentage when the JS value is a `"N%"` string.
 */
static void apply_dim_pct(JSContext* ctx, JSValueConst value, int16_t* pixels, float* percent)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    if (JS_IsNumber(value))
    {
        int16_t dimension;
        if (to_dim(ctx, value, &dimension))
        {
            *pixels = dimension;
        }
    }
    else if (JS_IsString(value))
    {
        const char* text = JS_ToCString(ctx, value);
        if (text)
        {
            char* end = NULL;
            const double parsed_percent = strtod(text, &end);
            if (end != text && *end == '%' && !isnan(parsed_percent))
            {
                /* 0% of any box is 0 px, and 0.0 is the percentage field's "not set" sentinel — so an
                 * authored `'0%'` goes to the pixel field, where it still pins the edge. */
                if (parsed_percent == 0.0)
                {
                    *pixels = 0;
                }
                else
                {
                    *percent = (float)parsed_percent;
                }
            }
            JS_FreeCString(ctx, text);
        }
    }
}

/** @brief CSS weight at or above which text uses the engine's bold face — the "semibold" cutoff. */
#define ER_FONT_WEIGHT_BOLD_MIN 600

/**
 * @brief Resolves a JS `fontWeight` (number, "bold", or a numeric string) to the engine's 0/1 flag.
 *
 * Shared by the node-prop parser and the text-span parser, which sit ~1700 lines apart: a span has to
 * reach the same verdict as the Text node it lives in, or one line of a paragraph comes out bold and
 * the rest does not.
 *
 * @param[in] ctx       QuickJS context.
 * @param[in] value     A present (non-undefined) `fontWeight` value.
 * @param[in] fallback  Value to keep when the JS value cannot be read at all.
 *
 * @return 1 for bold, 0 otherwise.
 */
static uint8_t to_font_weight(JSContext* ctx, JSValueConst value, uint8_t fallback)
{
    if (JS_IsNumber(value))
    {
        int32_t weight = 0;
        if (JS_ToInt32(ctx, &weight, value) != 0)
        {
            return fallback;
        }
        return weight >= ER_FONT_WEIGHT_BOLD_MIN ? 1U : 0U;
    }
    const char* text = JS_ToCString(ctx, value);
    if (!text)
    {
        return fallback;
    }
    const uint8_t bold = (strcmp(text, "bold") == 0 || atoi(text) >= ER_FONT_WEIGHT_BOLD_MIN) ? 1U : 0U;
    JS_FreeCString(ctx, text);
    return bold;
}

/**
 * @brief Reads `fontWeight` (string keyword, numeric, or numeric string) into a 0/1 weight.
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched `fontWeight` value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_font_weight(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    props->font_weight = to_font_weight(ctx, value, props->font_weight);
}

/**
 * @brief Reads `opacity` (RN 0.0–1.0 float) into the engine's 0–255 byte opacity.
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched `opacity` value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_opacity(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    double opacity = 1.0;
    if (JS_ToFloat64(ctx, &opacity, value) == 0)
    {
        /* NaN fails every comparison, so `opacity < 0.0` would pass it on to the cast, where it is undefined. A NaN
           opacity (a 0/0 in app math) is fully transparent, as Flow B's app_opacity makes it. */
        if (!(opacity > 0.0))
        {
            opacity = 0.0;
        }
        if (opacity > 1.0)
        {
            opacity = 1.0;
        }
        props->opacity = (uint8_t)(opacity * ER_BRIDGE_CHANNEL_MAX_FLOAT + 0.5);
    }
}

/**
 * @brief Reads a float-valued key from a transform-list item.
 *
 * @param[in]  ctx   QuickJS context.
 * @param[in]  item  Transform list element (single-key object).
 * @param[in]  key   Key to read.
 * @param[out] out   Receives the float value when present.
 *
 * @return true when the key is present and numeric; false otherwise.
 */
static bool item_f32(JSContext* ctx, JSValueConst item, const char* key, float* out)
{
    JSValue value;
    if (!prop_get(ctx, item, key, &value))
    {
        return false;
    }
    double number = 0.0;
    const bool ok = JS_ToFloat64(ctx, &number, value) == 0;
    if (ok)
    {
        *out = (float)number;
    }
    JS_FreeValue(ctx, value);
    return ok;
}

/**
 * @brief Reads an angle-valued key (number or "Ndeg"/"Nrad") from a transform-list item.
 *
 * @param[in]  ctx   QuickJS context.
 * @param[in]  item  Transform list element (single-key object).
 * @param[in]  key   Key to read.
 * @param[out] out   Receives the angle in degrees when present.
 *
 * @return true when the key is present and a recognisable angle; false otherwise.
 */
static bool item_angle(JSContext* ctx, JSValueConst item, const char* key, float* out)
{
    JSValue value;
    if (!prop_get(ctx, item, key, &value))
    {
        return false;
    }
    const bool ok = to_angle(ctx, value, out);
    JS_FreeValue(ctx, value);
    return ok;
}

/**
 * @brief Flattens the RN `transform` array (list of single-key objects) into transform_* fields.
 *
 * @param[in]     ctx         QuickJS context.
 * @param[in]     transforms  Pre-fetched `transform` value (JS_UNDEFINED if absent).
 * @param[in,out] props       Props to update.
 */
static void apply_transform(JSContext* ctx, JSValueConst transforms, ERProps* props)
{
    if (JS_IsUndefined(transforms))
    {
        return;
    }
    if (JS_IsArray(transforms))
    {
        JSValue length_value = JS_GetPropertyStr(ctx, transforms, "length");
        int32_t transform_count = 0;
        JS_ToInt32(ctx, &transform_count, length_value);
        JS_FreeValue(ctx, length_value);

        for (int32_t transform_index = 0; transform_index < transform_count; transform_index++)
        {
            JSValue item = JS_GetPropertyUint32(ctx, transforms, (uint32_t)transform_index);
            if (JS_IsObject(item))
            {
                float amount;
                if (item_f32(ctx, item, "translateX", &amount))
                {
                    props->transform_translate_x = amount;
                }
                if (item_f32(ctx, item, "translateY", &amount))
                {
                    props->transform_translate_y = amount;
                }
                if (item_f32(ctx, item, "scale", &amount))
                {
                    props->transform_scale_x = amount;
                    props->transform_scale_y = amount;
                }
                if (item_f32(ctx, item, "scaleX", &amount))
                {
                    props->transform_scale_x = amount;
                }
                if (item_f32(ctx, item, "scaleY", &amount))
                {
                    props->transform_scale_y = amount;
                }
                if (item_angle(ctx, item, "rotate", &amount))
                {
                    props->transform_rotate_z = amount;
                }
                if (item_angle(ctx, item, "rotateZ", &amount))
                {
                    props->transform_rotate_z = amount;
                }
                if (item_angle(ctx, item, "rotateX", &amount))
                {
                    props->transform_rotate_x = amount;
                }
                if (item_angle(ctx, item, "rotateY", &amount))
                {
                    props->transform_rotate_y = amount;
                }
                if (item_f32(ctx, item, "perspective", &amount))
                {
                    props->transform_perspective = amount;
                }
            }
            JS_FreeValue(ctx, item);
        }
    }
}

/**
 * @brief Reads `transformOrigin` as a two-element [x, y] array of fractional pivots [0–1].
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched `transformOrigin` value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_transform_origin(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    if (JS_IsArray(value))
    {
        JSValue origin_x_value = JS_GetPropertyUint32(ctx, value, 0);
        JSValue origin_y_value = JS_GetPropertyUint32(ctx, value, 1);
        double origin_x = 0.0;
        double origin_y = 0.0;
        if (JS_ToFloat64(ctx, &origin_x, origin_x_value) == 0)
        {
            props->transform_origin_x = (float)origin_x;
        }
        if (JS_ToFloat64(ctx, &origin_y, origin_y_value) == 0)
        {
            props->transform_origin_y = (float)origin_y;
        }
        JS_FreeValue(ctx, origin_x_value);
        JS_FreeValue(ctx, origin_y_value);
    }
}

/**
 * @brief Reads `shadowOffset` ({width, height}) into the shadow_offset_x/y fields.
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched `shadowOffset` value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_shadow_offset(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    if (JS_IsObject(value))
    {
        JSValue width_value = JS_GetPropertyStr(ctx, value, "width");
        JSValue height_value = JS_GetPropertyStr(ctx, value, "height");
        double offset_x = 0.0;
        double offset_y = 0.0;
        if (JS_ToFloat64(ctx, &offset_x, width_value) == 0)
        {
            props->shadow_offset_x = (float)offset_x;
        }
        if (JS_ToFloat64(ctx, &offset_y, height_value) == 0)
        {
            props->shadow_offset_y = (float)offset_y;
        }
        JS_FreeValue(ctx, width_value);
        JS_FreeValue(ctx, height_value);
    }
}

/**
 * @brief Reads `trackColor` ({false, true}) into the Switch track_color_false/true fields.
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched `trackColor` value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_track_color(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value))
    {
        return;
    }
    if (!JS_IsObject(value))
    {
        /* A plain colour is the Dial's single-colour track (the Switch form is the {false, true} pair). */
        uint32_t color;
        if (to_color(ctx, value, &color))
        {
            props->arc_track_color = color;
        }
        return;
    }
    if (JS_IsObject(value))
    {
        JSValue false_value = JS_GetPropertyStr(ctx, value, "false");
        JSValue true_value = JS_GetPropertyStr(ctx, value, "true");
        uint32_t color;
        if (to_color(ctx, false_value, &color))
        {
            props->track_color_false = color;
        }
        if (to_color(ctx, true_value, &color))
        {
            props->track_color_true = color;
        }
        JS_FreeValue(ctx, false_value);
        JS_FreeValue(ctx, true_value);
    }
}

/**
 * @brief Reads a Dial `indicatorGradient` ({type: 'conic'|'radial', stops: [{color, offset}]}) into the
 *        View-gradient fields, which ER_NODE_ARC reads as its indicator paint.
 *
 * @param[in]     ctx    QuickJS context.
 * @param[in]     value  Pre-fetched value (JS_UNDEFINED if absent).
 * @param[in,out] props  Props to update.
 */
static void apply_indicator_gradient(JSContext* ctx, JSValueConst value, ERProps* props)
{
    if (JS_IsUndefined(value) || JS_IsNull(value) || !JS_IsObject(value))
    {
        return;
    }
    JSValue type_value = JS_GetPropertyStr(ctx, value, "type");
    const char* type_name = JS_ToCString(ctx, type_value);
    if (type_name)
    {
        props->gradient_type = (strcmp(type_name, "radial") == 0) ? ER_GRADIENT_RADIAL : ER_GRADIENT_CONIC;
        JS_FreeCString(ctx, type_name);
    }
    JS_FreeValue(ctx, type_value);

    JSValue stops = JS_GetPropertyStr(ctx, value, "stops");
    if (JS_IsArray(stops))
    {
        JSValue length_value = JS_GetPropertyStr(ctx, stops, "length");
        int32_t stop_total = 0;
        JS_ToInt32(ctx, &stop_total, length_value);
        JS_FreeValue(ctx, length_value);
        if (stop_total > ER_GRADIENT_MAX_STOPS)
        {
            stop_total = ER_GRADIENT_MAX_STOPS;
        }
        uint8_t stop_count = 0;
        for (int32_t stop_index = 0; stop_index < stop_total; stop_index++)
        {
            JSValue stop = JS_GetPropertyUint32(ctx, stops, (uint32_t)stop_index);
            JSValue color_value = JS_GetPropertyStr(ctx, stop, "color");
            JSValue offset_value = JS_GetPropertyStr(ctx, stop, "offset");
            uint32_t color;
            double offset = (stop_total > 1) ? (double)stop_index / (double)(stop_total - 1) : 0.0;
            if (!JS_IsUndefined(offset_value))
            {
                JS_ToFloat64(ctx, &offset, offset_value);
            }
            if (to_color(ctx, color_value, &color))
            {
                props->gradient_stops[stop_count].color = color;
                props->gradient_stops[stop_count].position = (float)offset;
                stop_count++;
            }
            JS_FreeValue(ctx, offset_value);
            JS_FreeValue(ctx, color_value);
            JS_FreeValue(ctx, stop);
        }
        props->gradient_stop_count = stop_count;
    }
    JS_FreeValue(ctx, stops);
}

/**
 * @brief Reads object's own enumerable string keys into s_prop_slots.
 *
 * Keys apply_props() doesn't recognise, and keys whose value is undefined/null/exception (same
 * filter prop_get() used to apply), are skipped — s_prop_slots[id] stays JS_UNDEFINED for those,
 * matching "absent" for every ER_* macro and custom applier.
 *
 * @param[in] ctx     QuickJS context.
 * @param[in] object  Flat props object passed to apply_props().
 */
static void gather_prop_slots(JSContext* ctx, JSValueConst object)
{
    for (int slot_index = 0; slot_index < PROP_COUNT_; slot_index++)
    {
        s_prop_slots[slot_index] = JS_UNDEFINED;
    }

    JSPropertyEnum* property_names = NULL;
    uint32_t property_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &property_names, &property_count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        != 0)
    {
        return;
    }
    for (uint32_t name_index = 0; name_index < property_count; name_index++)
    {
        const PropId id = prop_id_from_atom(property_names[name_index].atom);
        if (id == PROP_COUNT_)
        {
            continue;
        }
        JSValue value = JS_GetProperty(ctx, object, property_names[name_index].atom);
        if (JS_IsUndefined(value) || JS_IsNull(value) || JS_IsException(value))
        {
            JS_FreeValue(ctx, value);
            continue;
        }
        s_prop_slots[id] = value;
    }
    JS_FreePropertyEnum(ctx, property_names, property_count);
}

/** @brief Shortest long-press delay; 0 is the engine's "no preference". */
#define ER_LONG_PRESS_MIN_MS 1

/**
 * @brief Translates a flat JS props object into an ERProps and applies it to a node.
 *
 * Reads a single flat object (the JS layer merges resolved style + props before calling).
 * Unrecognised keys are ignored; unspecified props fall back to engine defaults.
 *
 * Rather than probing all ~90 known prop names against `object` (a JS_GetPropertyStr per name, every
 * call, regardless of how many keys `object` actually has), this walks object's own keys once via
 * JS_GetOwnPropertyNames and dispatches each into s_prop_slots by atom — see gather_prop_slots().
 * The statement order below is unchanged from the probing version and still matters: the `flex`
 * shorthand must run before the explicit flexGrow/flexShrink/flexBasis overrides, and `color`
 * legitimately feeds both the text `color` field and ActivityIndicator's `indicator_color`.
 *
 * @param[in] ctx     QuickJS context.
 * @param[in] node    Target node.
 * @param[in] object  Flat props object.
 */
static void apply_props(JSContext* ctx, ERNode* node, JSValueConst object)
{
    ERProps props;
    props_init_defaults(&props);

    gather_prop_slots(ctx, object);

    /* Layout — dimensions and box model. */
    apply_dim_pct(ctx, s_prop_slots[PROP_WIDTH], &props.width, &props.width_percent);
    apply_dim_pct(ctx, s_prop_slots[PROP_HEIGHT], &props.height, &props.height_percent);
    ER_DIM(PROP_MIN_WIDTH, min_width);
    ER_DIM(PROP_MAX_WIDTH, max_width);
    ER_DIM(PROP_MIN_HEIGHT, min_height);
    ER_DIM(PROP_MAX_HEIGHT, max_height);
    apply_dim_pct(ctx, s_prop_slots[PROP_TOP], &props.top, &props.top_percent);
    apply_dim_pct(ctx, s_prop_slots[PROP_LEFT], &props.left, &props.left_percent);
    apply_dim_pct(ctx, s_prop_slots[PROP_RIGHT], &props.right, &props.right_percent);
    apply_dim_pct(ctx, s_prop_slots[PROP_BOTTOM], &props.bottom, &props.bottom_percent);
    ER_DIM(PROP_MARGIN, margin);
    ER_DIM(PROP_MARGIN_TOP, margin_top);
    ER_DIM(PROP_MARGIN_RIGHT, margin_right);
    ER_DIM(PROP_MARGIN_BOTTOM, margin_bottom);
    ER_DIM(PROP_MARGIN_LEFT, margin_left);
    ER_DIM(PROP_MARGIN_HORIZONTAL, margin_horizontal);
    ER_DIM(PROP_MARGIN_VERTICAL, margin_vertical);
    ER_DIM(PROP_PADDING, padding);
    ER_DIM(PROP_PADDING_TOP, padding_top);
    ER_DIM(PROP_PADDING_RIGHT, padding_right);
    ER_DIM(PROP_PADDING_BOTTOM, padding_bottom);
    ER_DIM(PROP_PADDING_LEFT, padding_left);
    ER_DIM(PROP_PADDING_HORIZONTAL, padding_horizontal);
    ER_DIM(PROP_PADDING_VERTICAL, padding_vertical);
    ER_DIM(PROP_GAP, gap);
    ER_DIM(PROP_ROW_GAP, row_gap);
    ER_DIM(PROP_COLUMN_GAP, column_gap);

    /* Layout — flex. flex shorthand first; explicit grow/shrink/basis override it. */
    apply_flex_shorthand(ctx, s_prop_slots[PROP_FLEX], &props);
    ER_DIM(PROP_FLEX_GROW, flex_grow);
    ER_DIM(PROP_FLEX_SHRINK, flex_shrink);
    apply_flex_basis(ctx, s_prop_slots[PROP_FLEX_BASIS], &props);
    ER_ENUM(PROP_FLEX_DIRECTION, flex_direction, map_flex_direction);
    ER_ENUM(PROP_FLEX_WRAP, flex_wrap, map_flex_wrap);
    ER_ENUM(PROP_JUSTIFY_CONTENT, justify_content, map_justify);
    ER_ENUM(PROP_ALIGN_ITEMS, align_items, map_align);
    ER_ENUM(PROP_ALIGN_SELF, align_self, map_align);
    ER_ENUM(PROP_ALIGN_CONTENT, align_content, map_align_content);
    ER_ENUM(PROP_POSITION, position, map_position);
    ER_ENUM(PROP_DISPLAY, display, map_display);
    ER_ENUM(PROP_OVERFLOW, overflow, map_overflow);
    ER_ENUM(PROP_POINTER_EVENTS, pointer_events, map_pointer_events);

    /* delayLongPress: how long the finger has to rest before onLongPress fires. 0 is the engine's
       "no preference" sentinel, so an app asking for 0 (RN's "fire on the next tick") gets 1 ms. */
    {
        JSValueConst value = s_prop_slots[PROP_DELAY_LONG_PRESS];
        if (!JS_IsUndefined(value))
        {
            int32_t delay_ms = 0;
            if (JS_ToInt32(ctx, &delay_ms, value) == 0)
            {
                props.long_press_ms =
                    (uint16_t)(delay_ms < ER_LONG_PRESS_MIN_MS ? ER_LONG_PRESS_MIN_MS
                                                               : (delay_ms > UINT16_MAX ? UINT16_MAX : delay_ms));
            }
        }
    }

    {
        JSValueConst value = s_prop_slots[PROP_ASPECT_RATIO];
        if (!JS_IsUndefined(value))
        {
            double number = 0.0;
            if (JS_ToFloat64(ctx, &number, value) == 0)
            {
                props.aspect_ratio = (float)number;
            }
        }
    }

    /* View visual. */
    ER_COL(PROP_BACKGROUND_COLOR, background_color);
    apply_opacity(ctx, s_prop_slots[PROP_OPACITY], &props);
    ER_DIM(PROP_BORDER_RADIUS, border_radius);
    ER_DIM(PROP_BORDER_TOP_LEFT_RADIUS, border_top_left_radius);
    ER_DIM(PROP_BORDER_TOP_RIGHT_RADIUS, border_top_right_radius);
    ER_DIM(PROP_BORDER_BOTTOM_LEFT_RADIUS, border_bottom_left_radius);
    ER_DIM(PROP_BORDER_BOTTOM_RIGHT_RADIUS, border_bottom_right_radius);
    ER_DIM(PROP_BORDER_WIDTH, border_width);
    ER_DIM(PROP_BORDER_LEFT_WIDTH, border_left_width);
    ER_DIM(PROP_BORDER_TOP_WIDTH, border_top_width);
    ER_DIM(PROP_BORDER_RIGHT_WIDTH, border_right_width);
    ER_DIM(PROP_BORDER_BOTTOM_WIDTH, border_bottom_width);
    ER_COL(PROP_BORDER_COLOR, border_color);
    ER_COL(PROP_BORDER_LEFT_COLOR, border_left_color);
    ER_COL(PROP_BORDER_TOP_COLOR, border_top_color);
    ER_COL(PROP_BORDER_RIGHT_COLOR, border_right_color);
    ER_COL(PROP_BORDER_BOTTOM_COLOR, border_bottom_color);
    ER_ENUM(PROP_BORDER_STYLE, border_style, map_border_style);
    ER_DIM(PROP_Z_INDEX, z_index);

    /* Text. */
    ER_STR(PROP_TEXT, text, ER_TEXT_MAX);
    ER_STR(PROP_FONT_FAMILY, font_family, ER_FONT_FAMILY_MAX);
    ER_COL(PROP_COLOR, color);
    ER_U8(PROP_FONT_SIZE, font_size);
    apply_font_weight(ctx, s_prop_slots[PROP_FONT_WEIGHT], &props);
    ER_ENUM(PROP_FONT_STYLE, font_style, map_font_style);
    ER_ENUM(PROP_TEXT_ALIGN, text_align, map_text_align);
    ER_ENUM(PROP_TEXT_DECORATION_LINE, text_decoration, map_text_decoration);
    ER_U8(PROP_NUMBER_OF_LINES, number_of_lines);
    ER_ENUM(PROP_ELLIPSIZE_MODE, ellipsize_mode, map_ellipsize);
    ER_DIM(PROP_LINE_HEIGHT, line_height);
    ER_DIM(PROP_LETTER_SPACING, letter_spacing);

    /* Image. */
    ER_STR(PROP_IMAGE_NAME, image_name, ER_IMAGE_NAME_MAX);
    ER_ENUM(PROP_RESIZE_MODE, resize_mode, map_resize_mode);
    ER_COL(PROP_TINT_COLOR, tint_color);

    /* Transforms. */
    apply_transform(ctx, s_prop_slots[PROP_TRANSFORM], &props);
    apply_transform_origin(ctx, s_prop_slots[PROP_TRANSFORM_ORIGIN], &props);

    /* Shadow (View-family; rendered only when ERUI_SHADOWS and shadow_opacity > 0). */
    ER_COL(PROP_SHADOW_COLOR, shadow_color);
    apply_shadow_offset(ctx, s_prop_slots[PROP_SHADOW_OFFSET], &props);
    {
        JSValueConst value = s_prop_slots[PROP_SHADOW_OPACITY];
        if (!JS_IsUndefined(value))
        {
            double number = 0.0;
            if (JS_ToFloat64(ctx, &number, value) == 0)
            {
                props.shadow_opacity = (float)number;
            }
        }
    }
    ER_U8(PROP_SHADOW_RADIUS, shadow_radius);
    ER_U8(PROP_ELEVATION, elevation);

    /* ActivityIndicator (RN uses `color` for the spinner tint). */
    ER_COL(PROP_COLOR, indicator_color);
    ER_U8(PROP_ANIMATING, animating);

    /* Switch. */
    ER_U8(PROP_VALUE, switch_value);
    apply_track_color(ctx, s_prop_slots[PROP_TRACK_COLOR], &props);
    ER_COL(PROP_THUMB_COLOR, thumb_color);

    /* Dial (ER_NODE_ARC). `value` and `trackColor` are shared with Switch above; the engine reads the
     * fields that match the node type. */
    ER_F32(PROP_VALUE, arc_value);
    ER_F32(PROP_MIN, arc_min);
    ER_F32(PROP_MAX, arc_max);
    ER_F32(PROP_START_ANGLE, arc_start_angle);
    ER_F32(PROP_SWEEP_ANGLE, arc_sweep_angle);
    ER_F32(PROP_STEP, arc_step);
    ER_F32(PROP_GAP_ANGLE, arc_gap_angle);
    ER_DIM(PROP_THICKNESS, arc_width);
    ER_DIM(PROP_BAND_THICKNESS, arc_band_width);
    ER_DIM(PROP_KNOB_SIZE, arc_knob_size);
    ER_DIM(PROP_KNOB_BORDER_WIDTH, arc_knob_border_width);
    ER_COL(PROP_INDICATOR_COLOR, arc_indicator_color);
    ER_COL(PROP_BAND_COLOR, arc_band_color);
    ER_COL(PROP_KNOB_COLOR, arc_knob_color);
    ER_COL(PROP_KNOB_BORDER_COLOR, arc_knob_border_color);
    ER_U8(PROP_SEGMENTS, arc_segments);
    ER_ENUM(PROP_CAP, arc_cap, map_arc_cap);
    ER_ENUM(PROP_KNOB, arc_knob, map_arc_knob);
    ER_STR(PROP_KNOB_IMAGE, image_name, ER_IMAGE_NAME_MAX);
    ER_U8(PROP_ADJUSTABLE, arc_adjustable);
    ER_U8(PROP_RANGE, arc_range);
    ER_F32(PROP_VALUE_START, arc_value_start);
    ER_F32(PROP_MIN_SPAN, arc_min_span);
    apply_indicator_gradient(ctx, s_prop_slots[PROP_INDICATOR_GRADIENT], &props);

    /* TextInput. */
    ER_STR(PROP_PLACEHOLDER, placeholder, ER_PLACEHOLDER_MAX);
    ER_COL(PROP_PLACEHOLDER_TEXT_COLOR, placeholder_color);
    ER_COL(PROP_CURSOR_COLOR, cursor_color);
    ER_U8(PROP_EDITABLE, editable);
    ER_U8(PROP_SECURE_TEXT_ENTRY, secure_text_entry);

    /* Modal. */
    ER_U8(PROP_VISIBLE, modal_visible);
    ER_COL(PROP_BACKDROP_COLOR, backdrop_color);

    er_node_set_props(node, &props);

    for (int slot_index = 0; slot_index < PROP_COUNT_; slot_index++)
    {
        JS_FreeValue(ctx, s_prop_slots[slot_index]);
        s_prop_slots[slot_index] = JS_UNDEFINED;
    }
}

#undef ER_DIM
#undef ER_COL
#undef ER_ENUM
#undef ER_U8
#undef ER_STR

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — events
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Event-type names exposed on JS event objects, indexed by EREventType. */
static const char* const k_event_names[ER_EVENT_TYPE_COUNT_] = {
    "press",
    "longPress",
    "pressIn",
    "pressOut",
    "touchStart",
    "touchMove",
    "touchEnd",
    "touchCancel",
    "scroll",
    "responderGrant",
    "responderReject",
    "responderMove",
    "responderRelease",
    "responderTerminate",
    "layout",
    "changeText",
    "submitEditing",
    "focus",
    "blur",
    "valueChange",
};

/**
 * @brief Maps an RN handler prop name (onPress, onChangeText, …) to an EREventType.
 *
 * @param[in]  name  Handler prop name.
 * @param[out] out   Receives the event type when recognised.
 *
 * @return true when the name maps to a supported event; false otherwise.
 */
static bool event_type_from_name(const char* name, EREventType* out)
{
    static const struct
    {
        const char* name;
        EREventType type;
    } k_handler_props[] = {
        {"onPress", ER_EVENT_PRESS},
        {"onLongPress", ER_EVENT_LONG_PRESS},
        {"onPressIn", ER_EVENT_PRESS_IN},
        {"onPressOut", ER_EVENT_PRESS_OUT},
        {"onTouchStart", ER_EVENT_TOUCH_START},
        {"onTouchMove", ER_EVENT_TOUCH_MOVE},
        {"onTouchEnd", ER_EVENT_TOUCH_END},
        {"onTouchCancel", ER_EVENT_TOUCH_CANCEL},
        {"onResponderGrant", ER_EVENT_RESPONDER_GRANT},
        {"onResponderReject", ER_EVENT_RESPONDER_REJECT},
        {"onResponderMove", ER_EVENT_RESPONDER_MOVE},
        {"onResponderRelease", ER_EVENT_RESPONDER_RELEASE},
        {"onResponderTerminate", ER_EVENT_RESPONDER_TERMINATE},
        {"onScroll", ER_EVENT_SCROLL},
        {"onChangeText", ER_EVENT_CHANGE_TEXT},
        {"onSubmitEditing", ER_EVENT_SUBMIT_EDITING},
        {"onFocus", ER_EVENT_FOCUS},
        {"onBlur", ER_EVENT_BLUR},
        {"onLayout", ER_EVENT_LAYOUT},
        {"onChange", ER_EVENT_VALUE_CHANGE},      /* Dial: the new value */
        {"onValueChange", ER_EVENT_VALUE_CHANGE}, /* Switch: the new boolean; Dial: the new value */
    };
    for (size_t entry_index = 0; entry_index < sizeof(k_handler_props) / sizeof(k_handler_props[0]); entry_index++)
    {
        if (strcmp(name, k_handler_props[entry_index].name) == 0)
        {
            *out = k_handler_props[entry_index].type;
            return true;
        }
    }
    return false;
}

/**
 * @brief Maps a responder-negotiation prop name to an ERResponderQuery.
 *
 * These are the RN View responder props (onStartShouldSetResponder and friends). Unlike events they
 * are QUERIES: the engine calls them synchronously during gesture negotiation and the JS return value
 * (a boolean) decides whether the node claims the responder.
 *
 * @param[in]  name  Query prop name.
 * @param[out] out   Receives the query type when recognised.
 *
 * @return true when the name maps to a supported responder query; false otherwise.
 */
static bool query_type_from_name(const char* name, ERResponderQuery* out)
{
    static const struct
    {
        const char* name;
        ERResponderQuery query;
    } k_query_props[] = {
        {"onStartShouldSetResponder", ER_QUERY_START_SHOULD_SET},
        {"onStartShouldSetResponderCapture", ER_QUERY_START_SHOULD_SET_CAPTURE},
        {"onMoveShouldSetResponder", ER_QUERY_MOVE_SHOULD_SET},
        {"onMoveShouldSetResponderCapture", ER_QUERY_MOVE_SHOULD_SET_CAPTURE},
        {"onResponderTerminationRequest", ER_QUERY_TERMINATION_REQUEST},
    };
    for (size_t entry_index = 0; entry_index < sizeof(k_query_props) / sizeof(k_query_props[0]); entry_index++)
    {
        if (strcmp(name, k_query_props[entry_index].name) == 0)
        {
            *out = k_query_props[entry_index].query;
            return true;
        }
    }
    return false;
}

/**
 * @brief Builds the JS event object handed to a handler from an engine EREventData payload.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] type  Event type that fired.
 * @param[in] data  Engine event payload.
 *
 * @return A new JS object the caller owns (must JS_FreeValue).
 */
static JSValue build_event_object(JSContext* ctx, EREventType type, const EREventData* data)
{
    JSValue event = JS_NewObject(ctx);
    if (type < ER_EVENT_TYPE_COUNT_)
    {
        JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, k_event_names[type]));
    }
    JS_SetPropertyStr(ctx, event, "x", JS_NewInt32(ctx, data->x));
    JS_SetPropertyStr(ctx, event, "y", JS_NewInt32(ctx, data->y));
    JS_SetPropertyStr(ctx, event, "dx", JS_NewInt32(ctx, data->dx));
    JS_SetPropertyStr(ctx, event, "dy", JS_NewInt32(ctx, data->dy));
    JS_SetPropertyStr(ctx, event, "vx", JS_NewFloat64(ctx, (double)data->vx));
    JS_SetPropertyStr(ctx, event, "vy", JS_NewFloat64(ctx, (double)data->vy));

    if (type == ER_EVENT_SCROLL)
    {
        JS_SetPropertyStr(ctx, event, "scrollX", JS_NewFloat64(ctx, (double)data->scroll_x));
        JS_SetPropertyStr(ctx, event, "scrollY", JS_NewFloat64(ctx, (double)data->scroll_y));
    }
    else if (type == ER_EVENT_LAYOUT)
    {
        JSValue layout = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, layout, "x", JS_NewInt32(ctx, data->layout_rect.x));
        JS_SetPropertyStr(ctx, layout, "y", JS_NewInt32(ctx, data->layout_rect.y));
        JS_SetPropertyStr(ctx, layout, "width", JS_NewInt32(ctx, data->layout_rect.width));
        JS_SetPropertyStr(ctx, layout, "height", JS_NewInt32(ctx, data->layout_rect.height));
        // React Native reads it from e.nativeEvent.layout; e.layout is the same object.
        JSValue native_event = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, native_event, "layout", JS_DupValue(ctx, layout));
        JS_SetPropertyStr(ctx, event, "nativeEvent", native_event);
        JS_SetPropertyStr(ctx, event, "layout", layout);
    }
    else if (type == ER_EVENT_CHANGE_TEXT && data->changed_text)
    {
        JS_SetPropertyStr(ctx, event, "text", JS_NewString(ctx, data->changed_text));
    }
    else if (type == ER_EVENT_VALUE_CHANGE)
    {
        JS_SetPropertyStr(ctx, event, "value", JS_NewFloat64(ctx, (double)data->value));
        JS_SetPropertyStr(ctx, event, "valueStart", JS_NewFloat64(ctx, (double)data->value_start));
    }
    return event;
}

/** @brief Most arguments a batched callback takes. */
#define ER_BATCHED_MAX_ARGS 2

/**
 * @brief Calls a JS callback inside the installed batcher, or directly when there is none.
 *
 * The batcher (NativeUI.setBatcher) is React's batchedUpdates, so a callback that sets three pieces
 * of state renders once instead of three times. Takes at most two arguments — every callback the
 * bridge dispatches this way carries one (an event object, a changed string, a finished flag) or
 * two (a Dial's value and the low end of its band).
 *
 * @param[in] ctx       QuickJS context.
 * @param[in] callback  Callback to invoke.
 * @param[in] argc      Argument count (0..2).
 * @param[in] argv      Arguments.
 *
 * @return The callback's return value, or an exception (caller frees either way).
 */
static JSValue bridge_call_batched(JSContext* ctx, JSValueConst callback, int argc, JSValueConst* argv)
{
    if (!JS_IsObject(s_batch_slots))
    {
        return JS_Call(ctx, callback, JS_UNDEFINED, argc, argv); /* before er_bridge_install */
    }
    JSValue batcher = JS_GetPropertyUint32(ctx, s_batch_slots, ER_BATCH_SLOT_WRAP);
    JSValue result;
    if (JS_IsFunction(ctx, batcher))
    {
        JSValueConst batcher_arguments[ER_BATCHED_MAX_ARGS + 1] = {
            callback, (argc > 0) ? argv[0] : JS_UNDEFINED, (argc > 1) ? argv[1] : JS_UNDEFINED};
        result = JS_Call(ctx, batcher, JS_UNDEFINED, argc + 1, batcher_arguments);
    }
    else
    {
        result = JS_Call(ctx, callback, JS_UNDEFINED, argc, argv);
    }
    JS_FreeValue(ctx, batcher);
    return result;
}

/**
 * @brief Hands a pending JS exception nothing caught to the uncaught handler, or prints it (message and
 *        stack) to stderr when there is none, and clears it.
 *
 * @param[in] ctx    Context whose current exception should be reported.
 * @param[in] where  Short label for the failure site (e.g. "timer callback").
 */
static void bridge_report_exception(JSContext* ctx, const char* where)
{
    JSValue exception = JS_GetException(ctx);
    if (s_uncaught_handler)
    {
        s_uncaught_handler(ctx, exception, where);
        JS_FreeValue(ctx, exception);
        return;
    }
    const char* message = JS_ToCString(ctx, exception);
    fprintf(stderr, "JS %s exception: %s\n", where, message ? message : "(unknown)");
    if (message)
    {
        JS_FreeCString(ctx, message);
    }
    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
    if (!JS_IsUndefined(stack))
    {
        const char* stack_text = JS_ToCString(ctx, stack);
        if (stack_text)
        {
            fprintf(stderr, "%s\n", stack_text);
            JS_FreeCString(ctx, stack_text);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception);
}

/**
 * @brief Engine event callback: dispatches to the JS handler registered for (node, type).
 *
 * The (handle, type) pair is encoded in user_data and the JS function is looked up in the
 * handler registry. Any exception thrown by the handler is reported to stderr and swallowed
 * so it does not unwind through the engine's C call stack.
 *
 * @param[in] node       Node that fired the event (unused; key carries the handle).
 * @param[in] data       Event payload.
 * @param[in] user_data  Encoded key: handle * ER_EVENT_TYPE_COUNT_ + type.
 */
static void bridge_event_trampoline(ERNode* node, const EREventData* data, void* user_data)
{
    if (!s_bridge_ctx)
    {
        return;
    }
    JSContext* ctx = s_bridge_ctx;
    const uint32_t key = (uint32_t)(uintptr_t)user_data;
    const EREventType type = (EREventType)(key % ER_EVENT_TYPE_COUNT_);

    JSValue handler = JS_GetPropertyUint32(ctx, s_event_handlers, key);
    if (JS_IsFunction(ctx, handler))
    {
        /* Native events arrive outside any React batch, so without this scope a handler that sets
           two pieces of state renders and paints twice. */
        batch_enter();
        JSValue second_argument = JS_UNDEFINED;
        int argument_count = 1;
        /* onChangeText receives the new TEXT directly (RN convention, matching Flow B's AOT lowering);
         * every other event gets the event object. */
        JSValue first_argument;
        if (type == ER_EVENT_CHANGE_TEXT)
        {
            first_argument = JS_NewString(ctx, data->changed_text ? data->changed_text : "");
        }
        else if (type == ER_EVENT_VALUE_CHANGE)
        {
            /* onValueChange / onChange receive the new VALUE directly: a boolean for Switch (RN's
             * onValueChange(boolean)), a number for Dial. A RANGE Dial passes the band's low end as a
             * SECOND argument — two plain numbers rather than an object, so the AOT can lower the same
             * handler shape to `data->value` / `data->value_start` with no allocation on device. */
            first_argument = (er_node_get_type(node) == ER_NODE_SWITCH) ? JS_NewBool(ctx, data->value != 0.0f)
                                                                        : JS_NewFloat64(ctx, (double)data->value);
            second_argument = JS_NewFloat64(ctx, (double)data->value_start);
            argument_count = ER_BATCHED_MAX_ARGS;
        }
        else
        {
            first_argument = build_event_object(ctx, type, data);
        }
        JSValue arguments[ER_BATCHED_MAX_ARGS] = {first_argument, second_argument};
        JSValue result = bridge_call_batched(ctx, handler, argument_count, arguments);
        if (JS_IsException(result))
        {
            bridge_report_exception(ctx, "event handler");
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, first_argument);
        JS_FreeValue(ctx, second_argument);
        batch_leave();
    }
    JS_FreeValue(ctx, handler);
}

/**
 * @brief Engine → JS responder-query trampoline: calls the registered predicate, returns its boolean.
 *
 * Runs synchronously inside the engine's gesture negotiation (hit-test dispatch), like every event
 * trampoline. The event object carries x/y/dx/dy/vx/vy but no `type` — a query is not one of the named
 * events (ER_EVENT_TYPE_COUNT_ makes build_event_object skip the type string). An exception in the
 * predicate is reported and answered with false, so a broken predicate never claims a gesture.
 *
 * @param[in] node       Node being queried (unused; key carries the handle).
 * @param[in] data       Current touch payload (x, y, dx, dy, vx, vy populated).
 * @param[in] user_data  Encoded key: handle * ER_RESPONDER_QUERY_COUNT + query.
 *
 * @return true when the JS predicate returned truthy (claim / keep the responder).
 */
static bool bridge_query_trampoline(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    if (!s_bridge_ctx)
    {
        return false;
    }
    JSContext* ctx = s_bridge_ctx;
    const uint32_t key = (uint32_t)(uintptr_t)user_data;

    bool claim = false;
    JSValue predicate = JS_GetPropertyUint32(ctx, s_query_handlers, key);
    if (JS_IsFunction(ctx, predicate))
    {
        JSValue event = build_event_object(ctx, ER_EVENT_TYPE_COUNT_, data);
        JSValue result = JS_Call(ctx, predicate, JS_UNDEFINED, 1, &event);
        if (JS_IsException(result))
        {
            bridge_report_exception(ctx, "responder query");
        }
        else
        {
            claim = JS_ToBool(ctx, result) > 0;
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, event);
    }
    JS_FreeValue(ctx, predicate);
    return claim;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — timers & job queue
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Deactivates a timer slot and releases its callback array from the registry.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] slot  Slot index in s_timers / s_timer_handlers.
 */
static void timer_clear_slot(JSContext* ctx, int slot)
{
    s_timers[slot].active = false;
    JS_SetPropertyUint32(ctx, s_timer_handlers, (uint32_t)slot, JS_UNDEFINED);
}

/** @brief setTimeout/setInterval's first argument after the callback and delay. */
#define ER_TIMER_FIRST_EXTRA_ARG 2

/** @brief Shortest setInterval period, so due_ms always advances. */
#define ER_TIMER_MIN_INTERVAL_MS 1

/**
 * @brief Shared implementation for setTimeout / setInterval.
 *
 * @param[in] ctx          QuickJS context.
 * @param[in] argc         Argument count (argv[0]=callback, argv[1]=delay ms, argv[2..]=extra args).
 * @param[in] argv         Argument values.
 * @param[in] is_interval  true for setInterval (repeating), false for setTimeout (one-shot).
 *
 * @return JS integer timer id (> 0), or 0 if the callback is not a function or the pool is full.
 */
static JSValue timer_register(JSContext* ctx, int argc, JSValueConst* argv, bool is_interval)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
    {
        return JS_NewInt32(ctx, 0);
    }

    int32_t delay = 0;
    if (argc > 1)
    {
        JS_ToInt32(ctx, &delay, argv[1]);
    }
    if (delay < 0)
    {
        delay = 0;
    }

    int slot = -1;
    for (int candidate = 0; candidate < ER_BRIDGE_MAX_TIMERS; candidate++)
    {
        if (!s_timers[candidate].active)
        {
            slot = candidate;
            break;
        }
    }
    if (slot < 0)
    {
        fprintf(stderr, "setTimeout/setInterval: timer pool full (%d)\n", ER_BRIDGE_MAX_TIMERS);
        return JS_NewInt32(ctx, 0);
    }

    /* Stash [callback, ...extraArgs] in the JS-owned registry so it is GC-rooted and freed at
       teardown. The extra args (argv[2..]) are forwarded to the callback per the web spec. */
    const int extra_arg_count = argc > ER_TIMER_FIRST_EXTRA_ARG ? argc - ER_TIMER_FIRST_EXTRA_ARG : 0;
    JSValue bundle = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, bundle, 0, JS_DupValue(ctx, argv[0]));
    for (int extra_index = 0; extra_index < extra_arg_count; extra_index++)
    {
        JS_SetPropertyUint32(
            ctx, bundle, (uint32_t)(extra_index + 1), JS_DupValue(ctx, argv[ER_TIMER_FIRST_EXTRA_ARG + extra_index]));
    }
    JS_SetPropertyUint32(ctx, s_timer_handlers, (uint32_t)slot, bundle);

    ERBridgeTimer* timer = &s_timers[slot];
    timer->active = true;
    timer->id = s_next_timer_id++;
    timer->extra_arg_count = extra_arg_count;
    /* setInterval clamps to a 1ms floor so due_ms strictly advances (no per-pump busy-fire). */
    timer->interval_ms =
        is_interval ? (uint32_t)(delay < ER_TIMER_MIN_INTERVAL_MS ? ER_TIMER_MIN_INTERVAL_MS : delay) : 0U;
    timer->due_ms = er_now_ms() + (uint32_t)delay;
    return JS_NewInt32(ctx, timer->id);
}

/**
 * @brief setTimeout(callback, delayMs, ...args) — schedules a one-shot timer.
 *
 * @return Integer timer id for clearTimeout, or 0 on failure.
 */
static JSValue js_set_timeout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    return timer_register(ctx, argc, argv, false);
}

/**
 * @brief setInterval(callback, periodMs, ...args) — schedules a repeating timer.
 *
 * @return Integer timer id for clearInterval, or 0 on failure.
 */
static JSValue js_set_interval(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    return timer_register(ctx, argc, argv, true);
}

/**
 * @brief clearTimeout(id) / clearInterval(id) — cancels a scheduled timer by id.
 *
 * @return JS_UNDEFINED. Unknown / already-fired ids are ignored.
 */
static JSValue js_clear_timer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int32_t id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) != 0 || id <= 0)
    {
        return JS_UNDEFINED;
    }
    for (int slot = 0; slot < ER_BRIDGE_MAX_TIMERS; slot++)
    {
        if (s_timers[slot].active && s_timers[slot].id == id)
        {
            timer_clear_slot(ctx, slot);
            break;
        }
    }
    return JS_UNDEFINED;
}

/**
 * @brief Drains the QuickJS job queue (Promise reactions / microtasks) to completion.
 *
 * @param[in] ctx  Any context on the runtime; pending jobs across the runtime are executed.
 */
static void bridge_run_microtasks(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    for (;;)
    {
        JSContext* job_ctx = NULL;
        const int status = JS_ExecutePendingJob(rt, &job_ctx);
        if (status == 0)
        {
            break; /* no more pending jobs */
        }
        if (status < 0 && job_ctx)
        {
            bridge_report_exception(job_ctx, "promise job");
        }
    }
}

/**
 * @brief Fires every timer whose deadline has passed, in a single bounded pass.
 *
 * One-shot timers are removed before their callback runs; intervals are rescheduled to
 * now + period (so they cannot re-fire within this pass). The callback array is captured
 * (owned) before invocation, so a callback that cancels its own timer cannot free the
 * function out from under the in-flight call.
 *
 * @param[in] ctx  QuickJS context.
 */
static void bridge_fire_due_timers(JSContext* ctx)
{
    const uint32_t now_ms = er_now_ms();
    for (int slot = 0; slot < ER_BRIDGE_MAX_TIMERS; slot++)
    {
        ERBridgeTimer* timer = &s_timers[slot];
        if (!timer->active)
        {
            continue;
        }
        /* Wrap-safe "due_ms <= now" comparison on the monotonic engine clock. */
        if ((int32_t)(now_ms - timer->due_ms) < 0)
        {
            continue;
        }

        JSValue bundle = JS_GetPropertyUint32(ctx, s_timer_handlers, (uint32_t)slot); /* owned */
        const int extra_arg_count = timer->extra_arg_count;

        if (timer->interval_ms > 0U)
        {
            timer->due_ms = now_ms + timer->interval_ms;
        }
        else
        {
            timer_clear_slot(ctx, slot);
        }

        if (JS_IsObject(bundle))
        {
            JSValue callback = JS_GetPropertyUint32(ctx, bundle, 0);
            JSValue* extra_args = NULL;
            if (extra_arg_count > 0)
            {
                extra_args = (JSValue*)malloc(sizeof(JSValue) * (size_t)extra_arg_count);
                for (int extra_index = 0; extra_index < extra_arg_count; extra_index++)
                {
                    extra_args[extra_index] = JS_GetPropertyUint32(ctx, bundle, (uint32_t)(extra_index + 1));
                }
            }
            JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, extra_arg_count, extra_args);
            if (JS_IsException(result))
            {
                bridge_report_exception(ctx, "timer callback");
            }
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, callback);
            for (int extra_index = 0; extra_index < extra_arg_count; extra_index++)
            {
                JS_FreeValue(ctx, extra_args[extra_index]);
            }
            free(extra_args);
        }
        JS_FreeValue(ctx, bundle);
    }
}

/**
 * @brief The frame's JS work: pending touch-moves, then microtasks, timers and microtasks again.
 *
 * Split out of er_bridge_pump so the installed batcher can run it (see js_run_pump_body).
 *
 * @param[in] ctx  QuickJS context.
 */
static void bridge_pump_body(JSContext* ctx)
{
    /* The body is callback delivery, so it opens one DISPATCH span — but React's render is NOT part
     * of that. React schedules its synchronous flush as a MICROTASK, so the render happens inside the
     * job drain below rather than at any batch boundary C can bracket; measured on an S3 knob drag,
     * the touch flush is ~20 us against ~1 ms of drain. So the drains carry the RECONCILE span, and
     * DISPATCH keeps what is left: the touch flush, the timers, and the app callbacks they run. */
    ER_PERF_JS_BEGIN(ER_PERF_JS_DISPATCH);
    embedded_renderer_flush_touch();
    ER_PERF_JS_BEGIN(ER_PERF_JS_RECONCILE);
    bridge_run_microtasks(ctx);
    ER_PERF_JS_END(ER_PERF_JS_RECONCILE);
    bridge_fire_due_timers(ctx);
    ER_PERF_JS_BEGIN(ER_PERF_JS_RECONCILE);
    bridge_run_microtasks(ctx);
    ER_PERF_JS_END(ER_PERF_JS_RECONCILE);
    ER_PERF_JS_END(ER_PERF_JS_DISPATCH);
}

/**
 * @brief True when the pump will run JS: a parked touch-move, a queued job, or a due timer.
 *
 * The batcher only earns its keep when a frame runs callbacks — it is what makes their renders one
 * render. Entering it costs a JS call and a closure, which on an idle frame buys nothing, and an
 * idle frame is the common one on a static screen.
 *
 * The touch check has to be asked BEFORE the flush, not inferred from it: a move handler that
 * resolves a promise or arms a 0 ms timer adds work the flush itself creates, and a frame that had
 * already decided to skip the batcher would then render the handler and its continuation
 * separately.
 *
 * @param[in] ctx  QuickJS context.
 *
 * @return true if the frame has callbacks to coalesce.
 */
static bool bridge_pump_has_js_work(JSContext* ctx)
{
    if (embedded_renderer_has_pending_touch() || JS_IsJobPending(JS_GetRuntime(ctx)))
    {
        return true;
    }
    const uint32_t now_ms = er_now_ms();
    for (int slot = 0; slot < ER_BRIDGE_MAX_TIMERS; slot++)
    {
        /* Wrap-safe "due_ms <= now", as in bridge_fire_due_timers. */
        if (s_timers[slot].active && (int32_t)(now_ms - s_timers[slot].due_ms) >= 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief The callable handed to the batcher; runs one pump body.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Arguments (unused).
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_run_pump_body(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    bridge_pump_body(ctx);
    return JS_UNDEFINED;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — NativeUI methods
 ---------------------------------------------------------------------------------------------------------------------*/

#if ER_PERF_STATS
/*
 * Splitting the JS phase into "the callback" and "React's render" cannot be done from C: the render
 * runs whenever React decides to flush, which is not reliably the batcher call this code can see
 * around it. The renderer's batcher IS that boundary — `fn(a, b)` is the callback and everything
 * batchedUpdates does around it is React — so these four let it mark the split from where it sits.
 * Published only on an instrumented build, so the renderer installs a plain batcher otherwise and
 * an ordinary build carries no marking cost at all.
 */

/** @brief NativeUI.perfCallbackBegin() — the batcher is entering the callback itself. */
static JSValue js_perf_callback_begin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    er_perf_js_begin(ER_PERF_JS_DISPATCH);
    return JS_UNDEFINED;
}

/** @brief NativeUI.perfCallbackEnd() — the callback returned; what follows in the batch is React. */
static JSValue js_perf_callback_end(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    er_perf_js_end(ER_PERF_JS_DISPATCH);
    return JS_UNDEFINED;
}

/** @brief NativeUI.perfMarshalBegin() — React's mutation phase started (host config prepareForCommit). */
static JSValue js_perf_marshal_begin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    er_perf_js_begin(ER_PERF_JS_MARSHAL);
    return JS_UNDEFINED;
}

/** @brief NativeUI.perfMarshalEnd() — mutation phase finished (host config resetAfterCommit). */
static JSValue js_perf_marshal_end(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    er_perf_js_end(ER_PERF_JS_MARSHAL);
    return JS_UNDEFINED;
}

/** @brief NativeUI.perfRenderBegin() — opening a batch scope; React may render before it closes. */
static JSValue js_perf_render_begin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    er_perf_js_begin(ER_PERF_JS_RECONCILE);
    return JS_UNDEFINED;
}

/** @brief NativeUI.perfRenderEnd() — the batch scope closed. */
static JSValue js_perf_render_end(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    er_perf_js_end(ER_PERF_JS_RECONCILE);
    return JS_UNDEFINED;
}
#endif

/**
 * @brief NativeUI.createNode(typeString) — creates a node and returns its handle.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = node type string.
 *
 * @return Integer handle, or 0 on failure.
 */
ER_BRIDGE_MARSHAL_FN(js_create_node)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }

    const char* type_name = JS_ToCString(ctx, argv[0]);
    if (!type_name)
    {
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }
    ERNode* node = er_node_create(map_node_type(type_name));
    JS_FreeCString(ctx, type_name);

    if (!node)
    {
        if (!s_create_failure_logged)
        {
            fprintf(stderr, "createNode: node pool full, raise ERUI_MAX_NODES\n");
            s_create_failure_logged = true;
        }
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }
    const int32_t handle = handle_alloc(node);
    if (handle == ER_BRIDGE_HANDLE_INVALID)
    {
        er_node_destroy(node);
        if (!s_create_failure_logged)
        {
            fprintf(stderr, "createNode: handle table full (%d), raise ER_BRIDGE_MAX_HANDLES\n", ER_BRIDGE_MAX_HANDLES);
            s_create_failure_logged = true;
        }
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }
    s_create_failure_logged = false;
    return JS_NewInt32(ctx, handle);
}

/**
 * @brief Finds the slot currently mapped to a node, or ER_BRIDGE_HANDLE_INVALID.
 *
 * Scans only the live high-water range of the (small) handle table. Used to reclaim the handle of a node
 * reached by walking the scene tree — a deleted subtree's descendants never pass through js_destroy_node.
 */
static int32_t slot_of_node(const ERNode* node)
{
    if (!node)
    {
        return ER_BRIDGE_HANDLE_INVALID;
    }
    for (int32_t slot = 1; slot < s_high_water; slot++)
    {
        if (s_node_by_handle[slot] == node)
        {
            return slot;
        }
    }
    return ER_BRIDGE_HANDLE_INVALID;
}

/** @brief Releases a node's JS event-handler refs and frees its bridge handle slot. */
static void free_node_slot(JSContext* ctx, int32_t slot)
{
    if (slot == ER_BRIDGE_HANDLE_INVALID)
    {
        return;
    }
    if (s_bridge_ctx)
    {
        for (uint32_t event_type = 0; event_type < ER_EVENT_TYPE_COUNT_; event_type++)
        {
            JS_SetPropertyUint32(
                ctx, s_event_handlers, (uint32_t)slot * ER_EVENT_TYPE_COUNT_ + event_type, JS_UNDEFINED);
        }
        for (uint32_t query = 0; query < ER_RESPONDER_QUERY_COUNT; query++)
        {
            JS_SetPropertyUint32(
                ctx, s_query_handlers, (uint32_t)slot * ER_RESPONDER_QUERY_COUNT + query, JS_UNDEFINED);
        }
    }
    slot_free(slot);
}

/**
 * @brief Destroys a node and its ENTIRE subtree — engine node, bridge handle, and event-handler refs each.
 *
 * React calls removeChild only for the TOP node of a deleted subtree, never its descendants, so freeing
 * just that node leaks every descendant in the engine's node pool (plus their handles + handler refs). The
 * leak is invisible — the node pool is a fixed array, not heap — until the pool exhausts and er_node_create
 * starts returning NULL (a frozen UI). We walk the children here, capturing each next-sibling BEFORE its
 * slot is freed, and reclaim the whole tree.
 */
static void destroy_node_and_subtree(JSContext* ctx, ERNode* node)
{
    if (!node)
    {
        return;
    }
    ERNode* child = er_node_first_child(node);
    while (child)
    {
        ERNode* next = er_node_next_sibling(child);
        destroy_node_and_subtree(ctx, child);
        child = next;
    }
    free_node_slot(ctx, slot_of_node(node));
    er_node_destroy(node);
}

/**
 * @brief NativeUI.destroyNode(handle) — destroys a node and frees its handle.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = node handle.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_destroy_node)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int32_t handle = 0;
    if (JS_ToInt32(ctx, &handle, argv[0]) == 0)
    {
        ERNode* node = handle_node(handle);
        if (node)
        {
            /* Destroy the WHOLE subtree (engine nodes + handles + handler refs). React hands us only the
             * subtree's top in removeChild, so this reclaims the descendants it never mentions. */
            destroy_node_and_subtree(ctx, node);
        }
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.appendChild(parent, child) — appends child to parent.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = parent handle, argv[1] = child handle.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_append_child)
{
    (void)this_val;
    if (argc < 2)
    {
        return JS_UNDEFINED;
    }
    ERNode* parent = node_arg(ctx, argv[0]);
    ERNode* child = node_arg(ctx, argv[1]);
    if (parent && child)
    {
        er_tree_append_child(parent, child);
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.insertBefore(parent, child, beforeChild) — inserts/moves child before a sibling.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = parent, argv[1] = child, argv[2] = sibling to insert before.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_insert_before)
{
    (void)this_val;
    if (argc < 3)
    {
        return JS_UNDEFINED;
    }
    ERNode* parent = node_arg(ctx, argv[0]);
    ERNode* child = node_arg(ctx, argv[1]);
    int32_t before_handle = 0;
    if (JS_ToInt32(ctx, &before_handle, argv[2]) != 0)
    {
        return JS_UNDEFINED;
    }
    ERNode* before = handle_node(before_handle); /* NULL handle → append (handled by the engine). */
    /* A stale sibling is not "no sibling": skip, rather than move the child to the end. */
    if (parent && child && (before || before_handle <= 0))
    {
        er_tree_insert_before(parent, child, before);
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.removeChild(parent, child) — removes child from parent.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = parent handle, argv[1] = child handle.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_remove_child)
{
    (void)this_val;
    if (argc < 2)
    {
        return JS_UNDEFINED;
    }
    ERNode* parent = node_arg(ctx, argv[0]);
    ERNode* child = node_arg(ctx, argv[1]);
    if (parent && child)
    {
        er_tree_remove_child(parent, child);
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.setRoot(handle) — sets the scene root.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = root node handle.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_set_root)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[0]);
    if (node)
    {
        er_tree_set_root(node);
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.setProps(handle, propsObject) — marshals and applies props to a node.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = node handle, argv[1] = props object.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_set_props)
{
    (void)this_val;
    if (argc < 2)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[0]);
    if (node && JS_IsObject(argv[1]))
    {
        apply_props(ctx, node, argv[1]);
    }
    return JS_UNDEFINED;
}

/** @brief A span style field that inherits from its Text node. */
#define ER_SPAN_STYLE_INHERIT 0xFFU

/**
 * @brief NativeUI.setTextSpans(handle, spans) — sets inline styled text spans on a Text node.
 *
 * `spans` is an array of segment objects `{ text, color?, fontSize?, fontWeight?, fontStyle?,
 * textDecorationLine?, letterSpacing? }`. Omitted style fields inherit from the node's base style
 * (sentinel values). An empty/absent array reverts the node to single-string (`text`) rendering.
 * The array is clamped to ER_TEXT_MAX_SPANS and each fragment to ER_SPAN_TEXT_MAX bytes.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = node handle, argv[1] = array of span objects.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_set_text_spans)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[0]);
    if (!node)
    {
        return JS_UNDEFINED;
    }
    if (argc < 2 || !JS_IsArray(argv[1]))
    {
        er_node_set_text_spans(node, NULL, 0);
        return JS_UNDEFINED;
    }

    JSValue length_value = JS_GetPropertyStr(ctx, argv[1], "length");
    int32_t span_count = 0;
    JS_ToInt32(ctx, &span_count, length_value);
    JS_FreeValue(ctx, length_value);
    if (span_count <= 0)
    {
        er_node_set_text_spans(node, NULL, 0);
        return JS_UNDEFINED;
    }
    if (span_count > ER_TEXT_MAX_SPANS)
    {
        span_count = ER_TEXT_MAX_SPANS;
    }

    ERTextSpan spans[ER_TEXT_MAX_SPANS];
    for (int32_t span_index = 0; span_index < span_count; span_index++)
    {
        ERTextSpan* span = &spans[span_index];
        memset(span, 0, sizeof(*span));
        /* Sentinels: every style field inherits from the base node unless overridden below. */
        span->font_weight = ER_SPAN_STYLE_INHERIT;
        span->font_style = ER_SPAN_STYLE_INHERIT;
        span->text_decoration = ER_SPAN_STYLE_INHERIT;
        span->letter_spacing = ER_LAYOUT_AUTO;

        JSValue segment = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)span_index);
        JSValue value;

        if (prop_get(ctx, segment, "text", &value))
        {
            const char* text = JS_ToCString(ctx, value);
            if (text)
            {
                size_t text_length = strlen(text);
                if (text_length > ER_SPAN_TEXT_MAX)
                {
                    text_length = ER_SPAN_TEXT_MAX;
                }
                memcpy(span->text, text, text_length);
                span->text[text_length] = '\0';
                JS_FreeCString(ctx, text);
            }
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, segment, "color", &value))
        {
            uint32_t color = 0;
            if (to_color(ctx, value, &color))
            {
                span->color = color;
            }
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, segment, "fontSize", &value))
        {
            int32_t font_size = 0;
            if (JS_ToInt32(ctx, &font_size, value) == 0 && font_size > 0)
            {
                span->font_size = (uint8_t)(font_size > UINT8_MAX ? UINT8_MAX : font_size);
            }
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, segment, "fontWeight", &value))
        {
            span->font_weight = to_font_weight(ctx, value, span->font_weight);
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, segment, "fontStyle", &value))
        {
            const char* text = JS_ToCString(ctx, value);
            if (text)
            {
                span->font_style = map_font_style(text);
                JS_FreeCString(ctx, text);
            }
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, segment, "textDecorationLine", &value))
        {
            const char* text = JS_ToCString(ctx, value);
            if (text)
            {
                span->text_decoration = map_text_decoration(text);
                JS_FreeCString(ctx, text);
            }
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, segment, "letterSpacing", &value))
        {
            int32_t letter_spacing = 0;
            if (JS_ToInt32(ctx, &letter_spacing, value) == 0)
            {
                span->letter_spacing = (int16_t)letter_spacing;
            }
            JS_FreeValue(ctx, value);
        }
        JS_FreeValue(ctx, segment);
    }

    er_node_set_text_spans(node, spans, (uint8_t)span_count);
    return JS_UNDEFINED;
}

/*----------------------------------------------------------------------------------------------------------------------
 - On-screen keyboard config (Flow A): setKeyboardConfig({ ...colours/sizes, layers? }) → ERKeyboardConfig.
 - er_keyboard_set_config keeps the POINTER, so the parsed structs live in persistent static pools (the JS
 - objects/strings are transient). Re-config overwrites the pools. No-op unless ERUI_ONSCREEN_KEYBOARD=1.
 ---------------------------------------------------------------------------------------------------------------------*/
#define KBD_MAX_KEYS 160
#define KBD_MAX_ROWS 28
#define KBD_MAX_LAYERS 6
#define KBD_STRING_POOL_BYTES 1536
#define KBD_NO_HIGHLIGHT 0xFFU
static ERKeyboardKey s_kbd_keys[KBD_MAX_KEYS];
static ERKeyboardRow s_kbd_rows[KBD_MAX_ROWS];
static ERKeyboardLayer s_kbd_layers[KBD_MAX_LAYERS];
static ERKeyboardConfig s_kbd_config;
static char s_kbd_strings[KBD_STRING_POOL_BYTES];
static int s_kbd_strings_used;

/** Length of a JS array (0 when not an array). */
static int arr_len(JSContext* ctx, JSValueConst array)
{
    if (!JS_IsArray(array))
        return 0;
    JSValue length_value = JS_GetPropertyStr(ctx, array, "length");
    int32_t length = 0;
    JS_ToInt32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    return length < 0 ? 0 : length;
}

/** Bump-copies a JS string into the persistent pool; returns a stable pointer or NULL (empty / pool full). */
static const char* kbd_intern(JSContext* ctx, JSValueConst value)
{
    const char* text = JS_ToCString(ctx, value);
    if (!text)
        return NULL;
    const size_t length = strlen(text);
    if (length == 0 || (size_t)s_kbd_strings_used + length + 1U > KBD_STRING_POOL_BYTES)
    {
        JS_FreeCString(ctx, text);
        return NULL;
    }
    char* copy = &s_kbd_strings[s_kbd_strings_used];
    memcpy(copy, text, length + 1);
    s_kbd_strings_used += (int)(length + 1);
    JS_FreeCString(ctx, text);
    return copy;
}

/** Interns object[name] (a string) into the pool, or NULL when absent. */
static const char* kbd_intern_prop(JSContext* ctx, JSValueConst object, const char* name)
{
    JSValue value;
    const char* interned = NULL;
    if (prop_get(ctx, object, name, &value))
    {
        interned = kbd_intern(ctx, value);
        JS_FreeValue(ctx, value);
    }
    return interned;
}

/** Reads object[name] as an int, returning whether it was present. */
static bool kbd_int(JSContext* ctx, JSValueConst object, const char* name, int* out)
{
    JSValue value;
    if (prop_get(ctx, object, name, &value))
    {
        int32_t number = 0;
        const bool ok = JS_ToInt32(ctx, &number, value) == 0;
        JS_FreeValue(ctx, value);
        if (ok)
        {
            *out = number;
            return true;
        }
    }
    return false;
}

/** True when object[name] is a present, truthy boolean. */
static bool kbd_bool(JSContext* ctx, JSValueConst object, const char* name)
{
    JSValue value;
    bool truthy = false;
    if (prop_get(ctx, object, name, &value))
    {
        truthy = JS_ToBool(ctx, value) > 0;
        JS_FreeValue(ctx, value);
    }
    return truthy;
}

/** Reads object[name] as a color into *out, returning whether present + valid. */
static bool kbd_color(JSContext* ctx, JSValueConst object, const char* name, uint32_t* out)
{
    JSValue value;
    bool valid = false;
    if (prop_get(ctx, object, name, &value))
    {
        valid = to_color(ctx, value, out);
        JS_FreeValue(ctx, value);
    }
    return valid;
}

/** @brief JS `setKeyboardConfig(config)` — applies a custom on-screen-keyboard layout/appearance. */
static JSValue js_set_keyboard_config(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
    {
        er_keyboard_set_config(NULL); /* restore the built-in default */
        return JS_UNDEFINED;
    }
    JSValueConst config_object = argv[0];
    ERKeyboardConfig* config = &s_kbd_config;
    memset(config, 0, sizeof(*config));
    s_kbd_strings_used = 0;
    int key_count = 0, row_count = 0, layer_count = 0, number = 0;

    kbd_color(ctx, config_object, "panelColor", &config->panel_color);
    kbd_color(ctx, config_object, "keyColor", &config->key_color);
    kbd_color(ctx, config_object, "keyActiveColor", &config->key_active_color);
    kbd_color(ctx, config_object, "labelColor", &config->label_color);
    if (kbd_int(ctx, config_object, "fontSize", &number))
        config->font_size_px = (uint8_t)number;
    if (kbd_int(ctx, config_object, "rowHeight", &number))
        config->row_height_px = (uint16_t)number;
    if (kbd_int(ctx, config_object, "keyGap", &number))
        config->key_gap_px = (uint8_t)number;
    if (kbd_int(ctx, config_object, "keyRadius", &number))
        config->key_radius_px = (uint8_t)number;
    if (kbd_int(ctx, config_object, "gridColumns", &number))
        config->grid_columns = (uint8_t)number;

    JSValue layers = JS_GetPropertyStr(ctx, config_object, "layers");
    if (JS_IsArray(layers))
    {
        const int layer_total = arr_len(ctx, layers);
        for (int layer_index = 0; layer_index < layer_total && layer_count < KBD_MAX_LAYERS; layer_index++)
        {
            JSValue layer = JS_GetPropertyUint32(ctx, layers, (uint32_t)layer_index);
            const int row_start = row_count;
            const int row_total = arr_len(ctx, layer);
            for (int row_index = 0; row_index < row_total && row_count < KBD_MAX_ROWS; row_index++)
            {
                JSValue row = JS_GetPropertyUint32(ctx, layer, (uint32_t)row_index);
                const int key_start = key_count;
                const int key_total = arr_len(ctx, row);
                for (int key_index = 0; key_index < key_total && key_count < KBD_MAX_KEYS; key_index++)
                {
                    JSValue key = JS_GetPropertyUint32(ctx, row, (uint32_t)key_index);
                    ERKeyboardKey* keyboard_key = &s_kbd_keys[key_count++];
                    memset(keyboard_key, 0, sizeof(*keyboard_key));
                    keyboard_key->span = 1;
                    keyboard_key->highlight_layer = KBD_NO_HIGHLIGHT;
                    const char* label = kbd_intern_prop(ctx, key, "label");
                    int layer_target = 0;
                    if (kbd_bool(ctx, key, "backspace"))
                    {
                        keyboard_key->type = ER_KEYBOARD_KEY_BACKSPACE;
                        keyboard_key->label = label ? label : "<";
                    }
                    else if (kbd_bool(ctx, key, "done"))
                    {
                        keyboard_key->type = ER_KEYBOARD_KEY_DONE;
                        keyboard_key->label = label ? label : "OK";
                    }
                    else if (kbd_int(ctx, key, "layer", &layer_target))
                    {
                        keyboard_key->type = ER_KEYBOARD_KEY_LAYER;
                        keyboard_key->layer = (uint8_t)layer_target;
                        keyboard_key->label = label;
                    }
                    else
                    {
                        const char* character = kbd_intern_prop(ctx, key, "char");
                        keyboard_key->type = ER_KEYBOARD_KEY_CHAR;
                        keyboard_key->text = character;
                        keyboard_key->label =
                            label ? label
                                  : ((character && character[0] == ' ') ? NULL : character); /* blank space bar */
                    }
                    if (kbd_int(ctx, key, "span", &number))
                        keyboard_key->span = (uint8_t)(number > 0 ? number : 1);
                    if (kbd_bool(ctx, key, "highlight"))
                        keyboard_key->highlight_layer = (uint8_t)layer_index;
                    JS_FreeValue(ctx, key);
                }
                ERKeyboardRow* keyboard_row = &s_kbd_rows[row_count++];
                keyboard_row->keys = &s_kbd_keys[key_start];
                keyboard_row->count = (uint8_t)(key_count - key_start);
                JS_FreeValue(ctx, row);
            }
            ERKeyboardLayer* keyboard_layer = &s_kbd_layers[layer_count++];
            keyboard_layer->rows = &s_kbd_rows[row_start];
            keyboard_layer->count = (uint8_t)(row_count - row_start);
            JS_FreeValue(ctx, layer);
        }
        config->layers = s_kbd_layers;
        config->layer_count = (uint8_t)layer_count;
    }
    JS_FreeValue(ctx, layers);

    er_keyboard_set_config(config);
    return JS_UNDEFINED;
}

/* Bridge-side staging caps; the engine clamps again to its own storage limits. They default TO the engine's
 * caps rather than repeating the numbers, because a bridge that stages less truncates the op-tape before the
 * engine ever sees it — the shape just goes missing, with no pool ever reporting an overflow. Overridable
 * (e.g. to stage a large imported SVG on a build whose engine caps were raised separately); the asserts hold
 * the "at least the engine's" rule either way. */
#ifndef VEC_BRIDGE_MAX_OPS
#define VEC_BRIDGE_MAX_OPS ERUI_VECTOR_TAPE_MAX
#endif
#ifndef VEC_BRIDGE_MAX_PAINTS
#define VEC_BRIDGE_MAX_PAINTS ERUI_VECTOR_PAINTS_MAX
#endif
#ifndef VEC_BRIDGE_MAX_GRADS
#define VEC_BRIDGE_MAX_GRADS ERUI_VECTOR_GRADIENTS_MAX
#endif
_Static_assert(VEC_BRIDGE_MAX_OPS >= ERUI_VECTOR_TAPE_MAX, "VEC_BRIDGE_MAX_OPS must be >= ERUI_VECTOR_TAPE_MAX");
_Static_assert(VEC_BRIDGE_MAX_PAINTS >= ERUI_VECTOR_PAINTS_MAX,
               "VEC_BRIDGE_MAX_PAINTS must be >= ERUI_VECTOR_PAINTS_MAX");
_Static_assert(VEC_BRIDGE_MAX_GRADS >= ERUI_VECTOR_GRADIENTS_MAX,
               "VEC_BRIDGE_MAX_GRADS must be >= ERUI_VECTOR_GRADIENTS_MAX");
/* JS paint record (kept in sync with svg-ops.js PAINT_STRIDE): the 7 base fields + fill_gradient + stroke_gradient. */
#define VEC_PAINT_FILL 0
#define VEC_PAINT_STROKE 1
#define VEC_PAINT_STROKE_WIDTH 2
#define VEC_PAINT_MITER_LIMIT 3
#define VEC_PAINT_CAP 4
#define VEC_PAINT_JOIN 5
#define VEC_PAINT_FILL_RULE 6
#define VEC_PAINT_FILL_GRADIENT 7
#define VEC_PAINT_STROKE_GRADIENT 8
#define VEC_PAINT_STRIDE 9
/* JS gradient record (kept in sync with svg-ops.js encodeVectorGradients):
 * [type, stop_count, (color, position) × ER_VECTOR_GRADIENT_MAX_STOPS, ax, ay, bx, by, r]. */
#define VEC_GRADIENT_TYPE 0
#define VEC_GRADIENT_STOP_COUNT 1
#define VEC_GRADIENT_STOPS 2
#define VEC_GRADIENT_STOP_COLOR 0
#define VEC_GRADIENT_STOP_POSITION 1
#define VEC_GRADIENT_STOP_FIELDS 2
#define VEC_GRADIENT_GEOMETRY (VEC_GRADIENT_STOPS + ER_VECTOR_GRADIENT_MAX_STOPS * VEC_GRADIENT_STOP_FIELDS)
#define VEC_GRADIENT_AX 0
#define VEC_GRADIENT_AY 1
#define VEC_GRADIENT_BX 2
#define VEC_GRADIENT_BY 3
#define VEC_GRADIENT_RADIUS 4
#define VEC_GRADIENT_GEOMETRY_FIELDS 5
#define VEC_GRADIENT_STRIDE (VEC_GRADIENT_GEOMETRY + VEC_GRADIENT_GEOMETRY_FIELDS)
/* The optional dirty rect: [x, y, w, h], with its edges bounded well inside the int range. */
#define VEC_DIRTY_RECT_FIELDS 4
#define VEC_DIRTY_RECT_BOUND 1e9

/** @brief Reads an array-like's `length` as an int (0 when it has none). */
static int vec_array_len(JSContext* ctx, JSValueConst array_like)
{
    int64_t length = 0;
    if (JS_GetLength(ctx, array_like, &length) != 0 || length < 0)
    {
        return 0;
    }
    return length > INT32_MAX ? INT32_MAX : (int)length;
}

/**
 * @brief Reads array[element_index] as a double.
 *
 * Decodes the two number tags inline: the general JS_ToFloat64 is an out-of-line call, and these
 * arrays are read element by element on every geometry upload.
 */
static double vec_num_at(JSContext* ctx, JSValueConst array, uint32_t element_index)
{
    JSValue element = JS_GetPropertyUint32(ctx, array, element_index);
    const int tag = JS_VALUE_GET_TAG(element);
    if (tag == JS_TAG_INT)
    {
        return (double)JS_VALUE_GET_INT(element); /* No refcount on a number, so nothing to free. */
    }
    if (JS_TAG_IS_FLOAT64(tag))
    {
        return JS_VALUE_GET_FLOAT64(element);
    }
    double number = 0.0;
    JS_ToFloat64(ctx, &number, element);
    JS_FreeValue(ctx, element);
    return number;
}

/**
 * @brief Reads the op-tape argument into @p out.
 *
 * A Float32Array arrives as one memcpy — the tape is already float on both sides, so nothing is lost.
 * Builds without the typed-array intrinsic pass a plain Array and it is read element by element.
 *
 * @return Floats written (clamped to @p capacity), or -1 when @p tape is neither array kind.
 */
static int vec_read_tape(JSContext* ctx, JSValueConst tape, float* out, int capacity)
{
    if (JS_GetTypedArrayType(tape) == JS_TYPED_ARRAY_FLOAT32)
    {
        size_t byte_offset = 0, bytes = 0, element_size = 0;
        JSValue buffer_value = JS_GetTypedArrayBuffer(ctx, tape, &byte_offset, &bytes, &element_size);
        size_t buffer_size = 0;
        const uint8_t* base = JS_IsException(buffer_value) ? NULL : JS_GetArrayBuffer(ctx, &buffer_size, buffer_value);
        JS_FreeValue(ctx, buffer_value);
        if (!base)
        {
            return 0; /* Detached buffer — treat as an empty tape. */
        }
        /* Bound in size_t first: narrowing a huge element count to int could go negative, skip the
         * clamp, and turn the memcpy size back into an enormous size_t. */
        size_t float_count = bytes / sizeof(float);
        if (float_count > (size_t)capacity)
        {
            float_count = (size_t)capacity;
        }
        memcpy(out, base + byte_offset, float_count * sizeof(float));
        return (int)float_count;
    }
    if (!JS_IsArray(tape))
    {
        return -1;
    }
    int float_count = vec_array_len(ctx, tape);
    if (float_count > capacity)
    {
        float_count = capacity;
    }
    for (int float_index = 0; float_index < float_count; float_index++)
    {
        out[float_index] = (float)vec_num_at(ctx, tape, (uint32_t)float_index);
    }
    return float_count;
}

/**
 * @brief NativeUI.setVectorOps(handle, ops, paints, gradients, dirtyRect) — sets the path geometry on an Svg.
 *
 * @p ops is a flat number array (the op-tape; see the ER_VECTOR_OP_* contract). @p paints is a flat number array
 * of VEC_PAINT_STRIDE (9) fields per shape: [fill, stroke, strokeWidth, miterLimit, cap, join, fillRule,
 * fillGradient, strokeGradient] where fill/stroke are uint32 ARGB8888 (JS numbers hold them exactly) and fillGradient /
 * strokeGradient are 1-based indices into @p gradients (0 = solid). @p gradients is an optional flat array of
 * VEC_GRADIENT_STRIDE fields per gradient. A missing/empty ops array clears the node's geometry.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_set_vector_ops)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[0]);
    if (!node)
    {
        return JS_UNDEFINED;
    }
    /* Op-tape. Static (the bridge is single-threaded) to keep it off the stack. */
    static float ops[VEC_BRIDGE_MAX_OPS];
    const int op_count = argc < 2 ? -1 : vec_read_tape(ctx, argv[1], ops, VEC_BRIDGE_MAX_OPS);
    if (op_count <= 0)
    {
        er_node_set_vector_ops(node, NULL, 0, NULL, 0, NULL, 0);
        return JS_UNDEFINED;
    }

    /* Paint table: VEC_PAINT_STRIDE numbers per entry. */
    ERVectorPaint paints[VEC_BRIDGE_MAX_PAINTS];
    int paint_count = 0;
    if (argc >= 3 && JS_IsArray(argv[2]))
    {
        paint_count = vec_array_len(ctx, argv[2]) / VEC_PAINT_STRIDE;
        if (paint_count > VEC_BRIDGE_MAX_PAINTS)
        {
            paint_count = VEC_BRIDGE_MAX_PAINTS;
        }
        for (int entry_index = 0; entry_index < paint_count; entry_index++)
        {
            double paint_fields[VEC_PAINT_STRIDE];
            for (int field_index = 0; field_index < VEC_PAINT_STRIDE; field_index++)
            {
                paint_fields[field_index] =
                    vec_num_at(ctx, argv[2], (uint32_t)(entry_index * VEC_PAINT_STRIDE + field_index));
            }
            /* Colors fit a double exactly, so (uint32_t)double is bit-accurate ARGB8888. */
            paints[entry_index].fill = (uint32_t)paint_fields[VEC_PAINT_FILL];
            paints[entry_index].stroke = (uint32_t)paint_fields[VEC_PAINT_STROKE];
            paints[entry_index].stroke_width = (float)paint_fields[VEC_PAINT_STROKE_WIDTH];
            paints[entry_index].miter = (float)paint_fields[VEC_PAINT_MITER_LIMIT];
            paints[entry_index].cap = (uint8_t)paint_fields[VEC_PAINT_CAP];
            paints[entry_index].join = (uint8_t)paint_fields[VEC_PAINT_JOIN];
            paints[entry_index].fill_rule = (uint8_t)paint_fields[VEC_PAINT_FILL_RULE];
            paints[entry_index].fill_gradient =
                (int16_t)paint_fields[VEC_PAINT_FILL_GRADIENT]; /* 1-based fill gradient index (0 = solid) */
            paints[entry_index].stroke_gradient =
                (int16_t)paint_fields[VEC_PAINT_STROKE_GRADIENT]; /* 1-based stroke gradient index (0 = solid) */
        }
    }

    /* Gradient table (optional 4th arg): VEC_GRADIENT_STRIDE numbers per gradient. */
    ERVectorGradient gradients[VEC_BRIDGE_MAX_GRADS];
    int gradient_count = 0;
    if (argc >= 4 && JS_IsArray(argv[3]))
    {
        gradient_count = vec_array_len(ctx, argv[3]) / VEC_GRADIENT_STRIDE;
        if (gradient_count > VEC_BRIDGE_MAX_GRADS)
        {
            gradient_count = VEC_BRIDGE_MAX_GRADS;
        }
        for (int entry_index = 0; entry_index < gradient_count; entry_index++)
        {
            double gradient_fields[VEC_GRADIENT_STRIDE];
            for (int field_index = 0; field_index < VEC_GRADIENT_STRIDE; field_index++)
            {
                gradient_fields[field_index] =
                    vec_num_at(ctx, argv[3], (uint32_t)(entry_index * VEC_GRADIENT_STRIDE + field_index));
            }
            gradients[entry_index].type = (uint8_t)gradient_fields[VEC_GRADIENT_TYPE];
            gradients[entry_index].stop_count = (uint8_t)gradient_fields[VEC_GRADIENT_STOP_COUNT];
            for (int stop_index = 0; stop_index < ER_VECTOR_GRADIENT_MAX_STOPS; stop_index++)
            {
                gradients[entry_index].stops[stop_index].color =
                    (uint32_t)gradient_fields[VEC_GRADIENT_STOPS + VEC_GRADIENT_STOP_COLOR
                                              + stop_index * VEC_GRADIENT_STOP_FIELDS];
                gradients[entry_index].stops[stop_index].position =
                    (float)gradient_fields[VEC_GRADIENT_STOPS + VEC_GRADIENT_STOP_POSITION
                                           + stop_index * VEC_GRADIENT_STOP_FIELDS];
            }
            const int geometry_offset = VEC_GRADIENT_GEOMETRY;
            gradients[entry_index].ax = (float)gradient_fields[geometry_offset + VEC_GRADIENT_AX];
            gradients[entry_index].ay = (float)gradient_fields[geometry_offset + VEC_GRADIENT_AY];
            gradients[entry_index].bx = (float)gradient_fields[geometry_offset + VEC_GRADIENT_BX];
            gradients[entry_index].by = (float)gradient_fields[geometry_offset + VEC_GRADIENT_BY];
            gradients[entry_index].r = (float)gradient_fields[geometry_offset + VEC_GRADIENT_RADIUS];
        }
    }

    er_node_set_vector_ops(node, ops, op_count, paints, paint_count, gradients, gradient_count);

    /* Optional 5th arg: a [x, y, w, h] node-local sub-rect to restrict this commit's damage to. */
    if (argc >= 5 && JS_IsArray(argv[4]) && vec_array_len(ctx, argv[4]) >= VEC_DIRTY_RECT_FIELDS)
    {
        double rect[VEC_DIRTY_RECT_FIELDS];
        bool finite = true;
        for (int edge_index = 0; edge_index < VEC_DIRTY_RECT_FIELDS; edge_index++)
        {
            rect[edge_index] = vec_num_at(ctx, argv[4], (uint32_t)edge_index);
            finite = finite && isfinite(rect[edge_index]);
        }
        /* The rect is app math, so it can be NaN (a 0/0) or past the int range, where a cast is undefined. A
           non-finite edge gives no hint, so the engine's own damage applies, as it would without one: what
           changed in the tape, or the whole node. The rest is rounded out to whole pixels and its edges are
           bounded well inside the int range (bounding its lengths would cut short a rect that reaches past
           the bound), and the engine clips the rect's corners from there. */
        if (finite)
        {
            const double edges[VEC_DIRTY_RECT_FIELDS] = {
                floor(rect[0]), floor(rect[1]), ceil(rect[0] + rect[2]), ceil(rect[1] + rect[3])};
            int pixel_edges[VEC_DIRTY_RECT_FIELDS];
            for (int edge_index = 0; edge_index < VEC_DIRTY_RECT_FIELDS; edge_index++)
            {
                pixel_edges[edge_index] =
                    (int)(edges[edge_index] < -VEC_DIRTY_RECT_BOUND
                              ? -VEC_DIRTY_RECT_BOUND
                              : (edges[edge_index] > VEC_DIRTY_RECT_BOUND ? VEC_DIRTY_RECT_BOUND : edges[edge_index]));
            }
            er_node_set_vector_dirty_rect(
                node, pixel_edges[0], pixel_edges[1], pixel_edges[2] - pixel_edges[0], pixel_edges[3] - pixel_edges[1]);
        }
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.commit() — runs layout + paint for all pending mutations.
 *
 * Inside an open batch scope (a frame pump, an event dispatch) the commit is only remembered; the
 * scope runs a single one as it closes. The reconciler commits per state update, so this is what
 * keeps a frame that changed three things to one engine commit instead of three.
 *
 * @param[in] ctx   QuickJS context (unused).
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Arguments (unused).
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_commit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (s_batch_depth > 0)
    {
        s_commit_pending = true;
        return JS_UNDEFINED;
    }
    ER_PERF_JS_BEGIN(ER_PERF_JS_COMMIT);
    er_commit();
    ER_PERF_JS_END(ER_PERF_JS_COMMIT);
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.setBatcher(wrap) — installs the function the bridge runs host-driven JS inside.
 *
 * The contract is `wrap(fn, a, b)`: call `fn(a, b)` exactly once, synchronously, and forward both
 * arguments — they are the callback's payload (an event object, a changed string, a finished flag,
 * or a Dial's value and the low end of its band), so a wrapper that drops them delivers events with
 * no payload. Two is the maximum; nothing the bridge batches this way passes more. The return value
 * is discarded. The renderer installs `(fn, a, b) => batchedUpdates(() => fn(a, b))`, which is what
 * makes every state update a frame produces — a coalesced touch move, a timer, a promise
 * continuation — land in one render pass and one commit rather than one of each per callback.
 *
 * What runs inside it: the frame pump's whole body (passed as a zero-argument `fn`), each native
 * event dispatch, and each Animated completion callback. Pass null to remove it — the bridge then
 * calls those directly, which is also what a host with no React on it gets, having installed none.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = wrapper function, or null/undefined to clear.
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_set_batcher(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    const bool has_batcher = argc >= 1 && JS_IsFunction(ctx, argv[0]);
    JS_SetPropertyUint32(
        ctx, s_batch_slots, ER_BATCH_SLOT_WRAP, has_batcher ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED);
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.now() — returns the engine's monotonic millisecond clock.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count (unused).
 * @param[in] argv  Arguments (unused).
 *
 * @return Milliseconds since the backend was set, as a JS number.
 */
static JSValue js_now(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt64(ctx, (int64_t)er_bridge_now_ms());
}

/**
 * @brief NativeUI.setEvent(node, eventName, fn) — registers or clears a JS event handler.
 *
 * eventName is an RN handler prop name (e.g. "onPress"). A function registers a handler; a
 * non-function (null/undefined) clears it. The handler is stored in the registry and invoked
 * by the engine via the bridge trampoline when the event fires.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = node handle, argv[1] = event name, argv[2] = handler or null.
 *
 * @return JS_UNDEFINED.
 */
ER_BRIDGE_MARSHAL_FN(js_set_event)
{
    (void)this_val;
    if (argc < 3)
    {
        return JS_UNDEFINED;
    }
    int32_t handle = 0;
    if (JS_ToInt32(ctx, &handle, argv[0]) != 0)
    {
        return JS_UNDEFINED;
    }
    const int32_t slot = handle_slot(handle);
    ERNode* node = s_node_by_handle[slot];
    if (!node)
    {
        return JS_UNDEFINED;
    }
    const char* name = JS_ToCString(ctx, argv[1]);
    if (!name)
    {
        return JS_UNDEFINED;
    }
    EREventType type;
    ERResponderQuery query;
    const bool is_event = event_type_from_name(name, &type);
    const bool is_query = !is_event && query_type_from_name(name, &query);
    JS_FreeCString(ctx, name);
    if (!is_event && !is_query)
    {
        return JS_UNDEFINED;
    }

    if (is_event)
    {
        const uint32_t key = (uint32_t)slot * ER_EVENT_TYPE_COUNT_ + (uint32_t)type;
        if (JS_IsFunction(ctx, argv[2]))
        {
            JS_SetPropertyUint32(ctx, s_event_handlers, key, JS_DupValue(ctx, argv[2]));
            er_event_set(node, type, bridge_event_trampoline, (void*)(uintptr_t)key);
        }
        else
        {
            /* Storing undefined releases the previously held handler reference. */
            JS_SetPropertyUint32(ctx, s_event_handlers, key, JS_UNDEFINED);
            er_event_set(node, type, NULL, NULL);
        }
    }
    else
    {
        /* Responder queries ride the same prop path as events (host-config routes every on* prop
           here), but land in the engine's query table and their own registry. */
        const uint32_t key = (uint32_t)slot * ER_RESPONDER_QUERY_COUNT + (uint32_t)query;
        if (JS_IsFunction(ctx, argv[2]))
        {
            JS_SetPropertyUint32(ctx, s_query_handlers, key, JS_DupValue(ctx, argv[2]));
            er_responder_query_set(node, query, bridge_query_trampoline, (void*)(uintptr_t)key);
        }
        else
        {
            JS_SetPropertyUint32(ctx, s_query_handlers, key, JS_UNDEFINED);
            er_responder_query_set(node, query, NULL, NULL);
        }
    }
    return JS_UNDEFINED;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Animated (er_animation_value_*)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads a numeric property from an object into a float.
 *
 * @param[in]  ctx     QuickJS context.
 * @param[in]  object  Object to read from.
 * @param[in]  key     Property name.
 * @param[out] out     Receives the value when present and numeric.
 *
 * @return true when the property was present and read.
 */
static bool obj_f32(JSContext* ctx, JSValueConst object, const char* key, float* out)
{
    JSValue value;
    if (!prop_get(ctx, object, key, &value))
    {
        return false;
    }
    double number = 0.0;
    const bool ok = JS_ToFloat64(ctx, &number, value) == 0;
    if (ok)
    {
        *out = (float)number;
    }
    JS_FreeValue(ctx, value);
    return ok;
}

/**
 * @brief Maps an animatable prop name (opacity, translateX, rotate, …) to an ERAnimationProp.
 *
 * @param[in]  name  Prop name.
 * @param[out] out   Receives the ERAnimationProp when recognised.
 *
 * @return true when the name maps to a supported animatable prop.
 */
static bool anim_prop_from_name(const char* name, ERAnimationProp* out)
{
    static const struct
    {
        const char* name;
        ERAnimationProp prop;
    } k_animatable_props[] = {
        {"opacity", ER_PROP_OPACITY},
        {"translateX", ER_PROP_TRANSLATE_X},
        {"translateY", ER_PROP_TRANSLATE_Y},
        {"scaleX", ER_PROP_SCALE_X},
        {"scaleY", ER_PROP_SCALE_Y},
        {"rotate", ER_PROP_ROTATE_Z},
        {"rotateZ", ER_PROP_ROTATE_Z},
        {"rotateX", ER_PROP_ROTATE_X},
        {"rotateY", ER_PROP_ROTATE_Y},
        {"backgroundColor", ER_PROP_BACKGROUND_COLOR},
        {"color", ER_PROP_COLOR},
        {"value", ER_PROP_ARC_VALUE},            /* Dial value (ER_NODE_ARC) */
        {"valueStart", ER_PROP_ARC_VALUE_START}, /* Dial RANGE low end */
    };
    for (size_t entry_index = 0; entry_index < sizeof(k_animatable_props) / sizeof(k_animatable_props[0]);
         entry_index++)
    {
        if (strcmp(name, k_animatable_props[entry_index].name) == 0)
        {
            *out = k_animatable_props[entry_index].prop;
            return true;
        }
    }
    return false;
}

/** @brief Maps an easing token to ERAnimationEasing. @param[in] token String. @return Enum value (default ease). */
static ERAnimationEasing easing_from_name(const char* token)
{
    if (strcmp(token, "linear") == 0)
    {
        return ER_EASE_LINEAR;
    }
    if (strcmp(token, "easeIn") == 0)
    {
        return ER_EASE_EASE_IN;
    }
    if (strcmp(token, "easeOut") == 0)
    {
        return ER_EASE_EASE_OUT;
    }
    if (strcmp(token, "easeInOut") == 0)
    {
        return ER_EASE_EASE_IN_OUT;
    }
    if (strcmp(token, "quadIn") == 0)
    {
        return ER_EASE_QUAD_IN;
    }
    if (strcmp(token, "quadOut") == 0)
    {
        return ER_EASE_QUAD_OUT;
    }
    if (strcmp(token, "quadInOut") == 0)
    {
        return ER_EASE_QUAD_IN_OUT;
    }
    if (strcmp(token, "cubicIn") == 0)
    {
        return ER_EASE_CUBIC_IN;
    }
    if (strcmp(token, "cubicOut") == 0)
    {
        return ER_EASE_CUBIC_OUT;
    }
    if (strcmp(token, "cubicInOut") == 0)
    {
        return ER_EASE_CUBIC_IN_OUT;
    }
    if (strcmp(token, "bounceOut") == 0)
    {
        return ER_EASE_BOUNCE_OUT;
    }
    if (strcmp(token, "elasticOut") == 0)
    {
        return ER_EASE_ELASTIC_OUT;
    }
    return ER_EASE_EASE;
}

/**
 * @brief Marshals a JS animation config object into an ERAnimationConfig.
 *
 * Recognised keys: type ("timing"|"spring"|"decay"), duration, delay, loop, easing (string token or
 * a {x1,y1,x2,y2} bezier object), spring stiffness/damping/mass/velocity, decay deceleration/velocity.
 *
 * @param[in]  ctx     QuickJS context.
 * @param[in]  object  Config object (may be undefined).
 * @param[out] config  Zero-initialised then populated.
 */
static void marshal_anim_config(JSContext* ctx, JSValueConst object, ERAnimationConfig* config)
{
    memset(config, 0, sizeof(*config));
    config->type = ER_ANIMATION_TIMING;
    config->easing = ER_EASE_EASE;
    config->duration_ms = ER_ANIMATION_DEFAULT_DURATION_MS;

    if (!JS_IsObject(object))
    {
        return;
    }

    JSValue value;
    if (prop_get(ctx, object, "type", &value))
    {
        const char* token = JS_ToCString(ctx, value);
        if (token)
        {
            if (strcmp(token, "spring") == 0)
            {
                config->type = ER_ANIMATION_SPRING;
            }
            else if (strcmp(token, "decay") == 0)
            {
                config->type = ER_ANIMATION_DECAY;
            }
            JS_FreeCString(ctx, token);
        }
        JS_FreeValue(ctx, value);
    }
    if (prop_get(ctx, object, "duration", &value))
    {
        int32_t milliseconds = 0;
        if (JS_ToInt32(ctx, &milliseconds, value) == 0 && milliseconds >= 0)
        {
            config->duration_ms = (uint32_t)milliseconds;
        }
        JS_FreeValue(ctx, value);
    }
    if (prop_get(ctx, object, "delay", &value))
    {
        int32_t milliseconds = 0;
        if (JS_ToInt32(ctx, &milliseconds, value) == 0 && milliseconds >= 0)
        {
            config->delay_ms = (uint32_t)milliseconds;
        }
        JS_FreeValue(ctx, value);
    }
    if (prop_get(ctx, object, "easing", &value))
    {
        if (JS_IsString(value))
        {
            const char* token = JS_ToCString(ctx, value);
            if (token)
            {
                config->easing = easing_from_name(token);
                JS_FreeCString(ctx, token);
            }
        }
        else if (JS_IsObject(value))
        {
            config->easing = ER_EASE_BEZIER;
            obj_f32(ctx, value, "x1", &config->bezier_x1);
            obj_f32(ctx, value, "y1", &config->bezier_y1);
            obj_f32(ctx, value, "x2", &config->bezier_x2);
            obj_f32(ctx, value, "y2", &config->bezier_y2);
        }
        JS_FreeValue(ctx, value);
    }

    obj_f32(ctx, object, "stiffness", &config->stiffness);
    obj_f32(ctx, object, "damping", &config->damping);
    obj_f32(ctx, object, "mass", &config->mass);
    obj_f32(ctx, object, "velocity", &config->velocity);
    obj_f32(ctx, object, "deceleration", &config->deceleration);

    if (prop_get(ctx, object, "loop", &value))
    {
        config->loop = JS_ToBool(ctx, value) != 0;
        JS_FreeValue(ctx, value);
    }
}

/**
 * @brief NativeUI.configureNextLayoutAnimation(config) — arms a layout animation for the next commit.
 *
 * On the next er_commit() (driven by the reconciler's resetAfterCommit), every node whose computed
 * rect changed animates from its current display position to the new one. Recognized config keys:
 * type ("timing"|"spring"), duration, easing (token string or {x1,y1,x2,y2} bezier), and spring
 * stiffness/damping/mass (0 = engine default). The config is consumed by that single commit.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = config object (optional; defaults to a 300ms ease-in-out).
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_configure_next_layout_animation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;

    ERLayoutAnimationConfig config;
    memset(&config, 0, sizeof(config));
    config.type = ER_ANIMATION_TIMING;
    config.easing = ER_EASE_EASE_IN_OUT;
    config.duration_ms = ER_ANIMATION_DEFAULT_DURATION_MS;

    JSValueConst object = (argc > 0) ? argv[0] : JS_UNDEFINED;
    if (JS_IsObject(object))
    {
        JSValue value;
        if (prop_get(ctx, object, "type", &value))
        {
            const char* token = JS_ToCString(ctx, value);
            if (token)
            {
                if (strcmp(token, "spring") == 0)
                {
                    config.type = ER_ANIMATION_SPRING;
                }
                JS_FreeCString(ctx, token);
            }
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, object, "duration", &value))
        {
            int32_t milliseconds = 0;
            if (JS_ToInt32(ctx, &milliseconds, value) == 0 && milliseconds > 0)
            {
                config.duration_ms = (uint16_t)(milliseconds > UINT16_MAX ? UINT16_MAX : milliseconds);
            }
            JS_FreeValue(ctx, value);
        }
        if (prop_get(ctx, object, "easing", &value))
        {
            if (JS_IsString(value))
            {
                const char* token = JS_ToCString(ctx, value);
                if (token)
                {
                    config.easing = easing_from_name(token);
                    JS_FreeCString(ctx, token);
                }
            }
            else if (JS_IsObject(value))
            {
                config.easing = ER_EASE_BEZIER;
                obj_f32(ctx, value, "x1", &config.bezier_x1);
                obj_f32(ctx, value, "y1", &config.bezier_y1);
                obj_f32(ctx, value, "x2", &config.bezier_x2);
                obj_f32(ctx, value, "y2", &config.bezier_y2);
            }
            JS_FreeValue(ctx, value);
        }
        obj_f32(ctx, object, "stiffness", &config.stiffness);
        obj_f32(ctx, object, "damping", &config.damping);
        obj_f32(ctx, object, "mass", &config.mass);
    }

    er_layout_animation_configure_next(&config);
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.hasPendingLayoutAnimation() — true while a layout animation is in flight.
 *
 * Lets an app (or a test) tell whether the engine is mid-transition, e.g. to defer expensive work.
 *
 * @return JS boolean.
 */
static JSValue js_has_pending_layout_animation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, er_layout_animation_has_pending());
}

/**
 * @brief NativeUI.animValueCreate(initial) — creates a standalone animatable value.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] this  JS this (unused).
 * @param[in] argc  Argument count.
 * @param[in] argv  argv[0] = initial float value.
 *
 * @return The value handle as a JS integer.
 */
static JSValue js_anim_value_create(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    double initial = 0.0;
    if (argc > 0)
    {
        JS_ToFloat64(ctx, &initial, argv[0]);
    }
    return JS_NewInt32(ctx, (int32_t)er_animation_value_create((float)initial));
}

/** @brief NativeUI.animValueDestroy(handle). @return JS_UNDEFINED. */
static JSValue js_anim_value_destroy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int32_t value_handle = 0;
    if (JS_ToInt32(ctx, &value_handle, argv[0]) == 0)
    {
        er_animation_value_destroy((ERAnimationValueHandle)value_handle);
    }
    return JS_UNDEFINED;
}

/** @brief NativeUI.animValueSet(handle, value). @return JS_UNDEFINED. */
static JSValue js_anim_value_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 2)
    {
        return JS_UNDEFINED;
    }
    int32_t value_handle = 0;
    double value = 0.0;
    if (JS_ToInt32(ctx, &value_handle, argv[0]) == 0 && JS_ToFloat64(ctx, &value, argv[1]) == 0)
    {
        er_animation_value_set((ERAnimationValueHandle)value_handle, (float)value);
    }
    return JS_UNDEFINED;
}

/** @brief NativeUI.animValueGet(handle) → current float. @return JS number. */
static JSValue js_anim_value_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_NewFloat64(ctx, 0.0);
    }
    int32_t value_handle = 0;
    if (JS_ToInt32(ctx, &value_handle, argv[0]) != 0)
    {
        return JS_NewFloat64(ctx, 0.0);
    }
    return JS_NewFloat64(ctx, (double)er_animation_value_get((ERAnimationValueHandle)value_handle));
}

/**
 * @brief NativeUI.animValueBind(valueHandle, nodeHandle, propName) — binds a value to a node prop.
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_anim_value_bind(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 3)
    {
        return JS_UNDEFINED;
    }
    int32_t value_handle = 0;
    if (JS_ToInt32(ctx, &value_handle, argv[0]) != 0)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[1]);
    const char* name = JS_ToCString(ctx, argv[2]);
    if (node && name)
    {
        ERAnimationProp prop;
        if (anim_prop_from_name(name, &prop))
        {
            er_animation_value_bind((ERAnimationValueHandle)value_handle, node, prop);
        }
    }
    if (name)
    {
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.animUnbind(nodeHandle, propName) — releases the animated binding on a node prop.
 *
 * Node+prop keyed, not value keyed: the reconciler knows the property it is taking back (a style prop that
 * stopped being animated, or that swapped to a different Animated.Value) but not which value drives it.
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_anim_unbind(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 2)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[0]);
    const char* name = JS_ToCString(ctx, argv[1]);
    if (node && name)
    {
        ERAnimationProp prop;
        if (anim_prop_from_name(name, &prop))
        {
            er_animation_unbind_prop(node, prop);
        }
    }
    if (name)
    {
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}

/** @brief Maps an extrapolate token to ERExtrapolate. @param[in] token String. @return Enum (default extend). */
static ERExtrapolate extrapolate_from_name(const char* token)
{
    if (strcmp(token, "clamp") == 0)
    {
        return ER_EXTRAPOLATE_CLAMP;
    }
    if (strcmp(token, "identity") == 0)
    {
        return ER_EXTRAPOLATE_IDENTITY;
    }
    return ER_EXTRAPOLATE_EXTEND;
}

/**
 * @brief Reads an extrapolate token from object[key] into *out (left unchanged if absent/invalid).
 *
 * @param[in]     ctx     QuickJS context.
 * @param[in]     object  Config object.
 * @param[in]     key     Property name (e.g. "extrapolate").
 * @param[in,out] out     Receives the mapped ERExtrapolate when the key is a string.
 */
static void read_extrapolate(JSContext* ctx, JSValueConst object, const char* key, ERExtrapolate* out)
{
    JSValue value;
    if (!prop_get(ctx, object, key, &value))
    {
        return;
    }
    const char* token = JS_ToCString(ctx, value);
    if (token)
    {
        *out = extrapolate_from_name(token);
        JS_FreeCString(ctx, token);
    }
    JS_FreeValue(ctx, value);
}

/**
 * @brief NativeUI.animValueBindInterpolated(valueHandle, nodeHandle, propName, interp) —
 *        binds a value to a node prop through a piecewise-linear interpolation.
 *
 * interp = { inputRange:[…], outputRange:[…], extrapolate?, extrapolateLeft?, extrapolateRight? }.
 * `extrapolate` sets both ends; the per-end keys override it. Tokens: "extend" (default), "clamp",
 * "identity". On `backgroundColor` and `color` the outputRange holds colors, any form a style color
 * takes, and on every other prop numbers; an entry that is neither leaves the prop unbound.
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_anim_value_bind_interpolated(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 4)
    {
        return JS_UNDEFINED;
    }
    int32_t value_handle = 0;
    if (JS_ToInt32(ctx, &value_handle, argv[0]) != 0)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[1]);
    const char* name = JS_ToCString(ctx, argv[2]);
    if (!node || !name || !JS_IsObject(argv[3]))
    {
        if (name)
        {
            JS_FreeCString(ctx, name);
        }
        return JS_UNDEFINED;
    }

    ERAnimationProp prop;
    const bool prop_known = anim_prop_from_name(name, &prop);
    JS_FreeCString(ctx, name);
    if (!prop_known)
    {
        return JS_UNDEFINED;
    }
    const bool color = prop == ER_PROP_BACKGROUND_COLOR || prop == ER_PROP_COLOR;

    ERInterpolation interpolation;
    ERColorInterpolation color_interpolation;
    memset(&interpolation, 0, sizeof(interpolation));
    memset(&color_interpolation, 0, sizeof(color_interpolation));
    interpolation.extrapolate_left = ER_EXTRAPOLATE_EXTEND;
    interpolation.extrapolate_right = ER_EXTRAPOLATE_EXTEND;

    /* `extrapolate` applies to both ends; the per-end keys override it (RN semantics). */
    read_extrapolate(ctx, argv[3], "extrapolate", &interpolation.extrapolate_left);
    interpolation.extrapolate_right = interpolation.extrapolate_left;
    read_extrapolate(ctx, argv[3], "extrapolateLeft", &interpolation.extrapolate_left);
    read_extrapolate(ctx, argv[3], "extrapolateRight", &interpolation.extrapolate_right);

    /* Read inputRange / outputRange arrays (clamped to ER_INTERPOLATE_MAX_POINTS). */
    bool outputs_ok = true;
    JSValue inputs = JS_GetPropertyStr(ctx, argv[3], "inputRange");
    JSValue outputs = JS_GetPropertyStr(ctx, argv[3], "outputRange");
    if (JS_IsArray(inputs) && JS_IsArray(outputs))
    {
        JSValue length_value = JS_GetPropertyStr(ctx, inputs, "length");
        int32_t point_count = 0;
        JS_ToInt32(ctx, &point_count, length_value);
        JS_FreeValue(ctx, length_value);
        if (point_count > ER_INTERPOLATE_MAX_POINTS)
        {
            point_count = ER_INTERPOLATE_MAX_POINTS;
        }
        for (int32_t point_index = 0; point_index < point_count; point_index++)
        {
            JSValue input_value = JS_GetPropertyUint32(ctx, inputs, (uint32_t)point_index);
            JSValue output_value = JS_GetPropertyUint32(ctx, outputs, (uint32_t)point_index);
            double input = 0.0;
            JS_ToFloat64(ctx, &input, input_value);
            interpolation.input_range[point_index] = (float)input;
            if (color)
            {
                outputs_ok = outputs_ok && to_color(ctx, output_value, &color_interpolation.output_range[point_index]);
            }
            else
            {
                double output = 0.0;
                outputs_ok = outputs_ok && JS_IsNumber(output_value) && JS_ToFloat64(ctx, &output, output_value) == 0;
                interpolation.output_range[point_index] = (float)output;
            }
            JS_FreeValue(ctx, input_value);
            JS_FreeValue(ctx, output_value);
        }
        interpolation.point_count = (uint8_t)point_count;
    }
    JS_FreeValue(ctx, inputs);
    JS_FreeValue(ctx, outputs);

    if (interpolation.point_count < 2 || !outputs_ok)
    {
        return JS_UNDEFINED;
    }
    if (color)
    {
        memcpy(color_interpolation.input_range, interpolation.input_range, sizeof(color_interpolation.input_range));
        color_interpolation.point_count = interpolation.point_count;
        color_interpolation.extrapolate_left = interpolation.extrapolate_left;
        color_interpolation.extrapolate_right = interpolation.extrapolate_right;
        er_animation_value_bind_color((ERAnimationValueHandle)value_handle, node, prop, &color_interpolation);
    }
    else
    {
        er_animation_value_bind_interpolated((ERAnimationValueHandle)value_handle, node, prop, &interpolation);
    }
    return JS_UNDEFINED;
}

/**
 * @brief Engine completion callback for a value animation: dispatches to the registered JS handler.
 *
 * user_data is the completion slot index. The handler is one-shot: it is captured (owned) and the
 * slot freed before the JS call, so a handler that chains the next animation can safely reuse this
 * slot. Runs inside er_animation_tick (the engine deactivates the finished animation before calling
 * this), so starting a new animation from the handler is safe. Handler exceptions are swallowed.
 *
 * @param[in] finished   true if the animation ran to completion; false if interrupted/stopped.
 * @param[in] user_data  Completion slot index (as uintptr_t).
 */
static void bridge_anim_complete_trampoline(bool finished, void* user_data)
{
    if (!s_bridge_ctx)
    {
        return;
    }
    JSContext* ctx = s_bridge_ctx;
    const int slot = (int)(uintptr_t)user_data;
    if (slot < 0 || slot >= ER_BRIDGE_MAX_ANIM_COMPLETIONS)
    {
        return;
    }

    JSValue callback = JS_GetPropertyUint32(ctx, s_anim_complete_handlers, (uint32_t)slot); /* owned */
    s_anim_complete_active[slot] = false;
    JS_SetPropertyUint32(ctx, s_anim_complete_handlers, (uint32_t)slot, JS_UNDEFINED);

    if (JS_IsFunction(ctx, callback))
    {
        batch_enter();
        JSValue finished_value = JS_NewBool(ctx, finished);
        JSValue result = bridge_call_batched(ctx, callback, 1, &finished_value);
        if (JS_IsException(result))
        {
            bridge_report_exception(ctx, "animation completion callback");
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, finished_value);
        batch_leave();
    }
    JS_FreeValue(ctx, callback);
}

/**
 * @brief NativeUI.animValueAnimate(valueHandle, toValue, config, onComplete?) — animates to toValue.
 *
 * If onComplete is a function it is invoked with a single boolean `finished` when the animation
 * ends (true = ran to completion, false = interrupted by a new animation or animStop).
 *
 * @return The animation handle as a JS integer.
 */
static JSValue js_anim_value_animate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 2)
    {
        return JS_NewInt32(ctx, 0);
    }
    int32_t value_handle = 0;
    double to_value = 0.0;
    if (JS_ToInt32(ctx, &value_handle, argv[0]) != 0 || JS_ToFloat64(ctx, &to_value, argv[1]) != 0)
    {
        return JS_NewInt32(ctx, 0);
    }
    ERAnimationConfig config;
    marshal_anim_config(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &config);

    /* Optional completion callback (argv[3]): grab a slot, root the callback, and route the
       engine's on_complete through the trampoline. Must be registered BEFORE er_animation_value_animate,
       which may synchronously cancel a superseded animation (firing that one's callback). */
    if (argc > 3 && JS_IsFunction(ctx, argv[3]))
    {
        int slot = -1;
        for (int candidate = 0; candidate < ER_BRIDGE_MAX_ANIM_COMPLETIONS; candidate++)
        {
            if (!s_anim_complete_active[candidate])
            {
                slot = candidate;
                break;
            }
        }
        if (slot >= 0)
        {
            s_anim_complete_active[slot] = true;
            JS_SetPropertyUint32(ctx, s_anim_complete_handlers, (uint32_t)slot, JS_DupValue(ctx, argv[3]));
            config.on_complete = bridge_anim_complete_trampoline;
            config.on_complete_user_data = (void*)(uintptr_t)slot;
        }
        else
        {
            fprintf(stderr, "animValueAnimate: completion pool full (%d)\n", ER_BRIDGE_MAX_ANIM_COMPLETIONS);
        }
    }

    const ERAnimationHandle animation_handle =
        er_animation_value_animate((ERAnimationValueHandle)value_handle, (float)to_value, &config);
    return JS_NewInt32(ctx, (int32_t)animation_handle);
}

/** @brief NativeUI.animStop(animHandle) — stops a running animation. @return JS_UNDEFINED. */
static JSValue js_anim_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int32_t animation_handle = 0;
    if (JS_ToInt32(ctx, &animation_handle, argv[0]) == 0)
    {
        er_animation_stop((ERAnimationHandle)animation_handle);
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.tick(deltaMs) — advances animations and input by deltaMs.
 *
 * The device host normally drives this from its frame loop; exposing it lets a JS-driven loop (or a
 * test) advance native-driver animations without re-entering JS per frame.
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_tick(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    int32_t delta = 0;
    if (argc > 0)
    {
        JS_ToInt32(ctx, &delta, argv[0]);
    }
    if (delta < 0)
    {
        delta = 0;
    }
    embedded_renderer_tick((uint32_t)delta);
    /* Advancing the engine clock can make timers due; pump so a JS-driven loop or test that
       only calls tick() still runs Promises and setTimeout/setInterval callbacks. */
    er_bridge_pump(ctx);
    return JS_UNDEFINED;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief The engine clock as 64 bits (see header).
 *
 * @return Milliseconds of engine-clock time since the backend was set.
 */
uint64_t er_bridge_now_ms(void)
{
    return er_now_ms64();
}

/**
 * @brief Runs one host pump: dispatches pending touch-moves, drains microtasks, fires due timers, then drains again.
 *
 * Drains before and after firing so a Promise that schedules a timer, and a timer callback
 * that resolves a Promise, both settle within the same pump. Call once per frame from the
 * host loop (after advancing the engine clock).
 *
 * The touch-move flush leads because it is the frame's first piece of JS: the engine coalesces a
 * drag's moves to the newest one per frame (see embedded_renderer_touch), and running it here rather
 * than at er_commit() lets whatever the handler schedules — a Promise continuation, a timer — settle
 * inside the same pump, so a drag never pays a frame of latency for being coalesced.
 *
 * The whole pump is one batch scope, and runs inside the batcher when one is installed
 * (NativeUI.setBatcher), so a frame's state updates cost one render and one commit however many
 * callbacks produced them. That commit runs as the pump returns: a callback part-way through the
 * frame still sees the rects the last commit computed, not the ones its own setState implies.
 *
 * @param[in] ctx  Context the bridge was installed into (NULL is a no-op).
 */
void er_bridge_pump(JSContext* ctx)
{
    if (!ctx)
    {
        return;
    }
    /* One batch scope over the whole frame's JS, so the commits every callback in it asks for
       collapse into the single one batch_leave() runs. */
    batch_enter();
    if (JS_IsObject(s_batch_slots) && bridge_pump_has_js_work(ctx))
    {
        JSValue pump_body = JS_GetPropertyUint32(ctx, s_batch_slots, ER_BATCH_SLOT_RUN);
        JSValue result = bridge_call_batched(ctx, pump_body, 0, NULL);
        if (JS_IsException(result))
        {
            /* The frame's callbacks report their own throws; what reaches here is the batched
               render React runs as the batch closes. */
            bridge_report_exception(ctx, "batched render");
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, pump_body);
    }
    else
    {
        /* Nothing to coalesce (or pumped before er_bridge_install): run the body directly and keep
           an idle frame free. Any event it dispatches still batches itself. */
        bridge_pump_body(ctx);
    }
    batch_leave();
}

/**
 * @brief Loads and runs a precompiled QuickJS bytecode blob (see header).
 *
 * @param[in] ctx       QuickJS context.
 * @param[in] bytecode  Bytecode bytes.
 * @param[in] length    Byte count.
 *
 * @return Evaluation result or a JS exception value (caller frees).
 */
JSValue er_bridge_run_bytecode(JSContext* ctx, const uint8_t* bytecode, size_t length)
{
    JSValue function_object = JS_ReadObject(ctx, bytecode, length, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(function_object))
    {
        return function_object;
    }
    /* JS_EvalFunction consumes function_object and returns the program result (or an exception). */
    return JS_EvalFunction(ctx, function_object);
}

/**
 * @brief Installs the NativeUI bridge object into a QuickJS global scope.
 *
 * @param[in] ctx  QuickJS context to install the bridge into.
 */
void er_bridge_install(JSContext* ctx)
{
    s_bridge_ctx = ctx;

    /* Builds (or, on a hot-reload reusing the same JSRuntime, confirms) the apply_props() atom
     * table used to dispatch a setProps object's own keys — see prop_atoms_init()'s doc comment. */
    prop_atoms_init(ctx);

    /* Start with an empty node-handle table so a re-install on a fresh context (the simulator's live
     * reload — see tools/simulator/README.md) doesn't alias stale handles onto newly created nodes. Pair this with
     * er_reset() on the engine side, which empties the node pool the handles point into. */
    memset(s_node_by_handle, 0, sizeof(s_node_by_handle));
    s_free_top = 0;
    s_high_water = 1;
    s_create_failure_logged = false;
    s_stale_handle_logged = false;

    /* Start every context with an empty timer pool (statics persist across re-install in tests). */
    for (int slot = 0; slot < ER_BRIDGE_MAX_TIMERS; slot++)
    {
        s_timers[slot].active = false;
    }
    s_next_timer_id = 1;
    for (int slot = 0; slot < ER_BRIDGE_MAX_ANIM_COMPLETIONS; slot++)
    {
        s_anim_complete_active[slot] = false;
    }
    /* No scope is open on a fresh context — an unbalanced enter from the old one must not
       swallow the new app's first commit. */
    s_batch_depth = 0;
    s_commit_pending = false;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue native_ui = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, native_ui, "createNode", JS_NewCFunction(ctx, js_create_node, "createNode", 1));
    JS_SetPropertyStr(ctx, native_ui, "destroyNode", JS_NewCFunction(ctx, js_destroy_node, "destroyNode", 1));
    JS_SetPropertyStr(ctx, native_ui, "appendChild", JS_NewCFunction(ctx, js_append_child, "appendChild", 2));
    JS_SetPropertyStr(ctx, native_ui, "insertBefore", JS_NewCFunction(ctx, js_insert_before, "insertBefore", 3));
    JS_SetPropertyStr(ctx, native_ui, "removeChild", JS_NewCFunction(ctx, js_remove_child, "removeChild", 2));
    JS_SetPropertyStr(ctx, native_ui, "setRoot", JS_NewCFunction(ctx, js_set_root, "setRoot", 1));
    JS_SetPropertyStr(ctx, native_ui, "setProps", JS_NewCFunction(ctx, js_set_props, "setProps", 2));
    JS_SetPropertyStr(ctx, native_ui, "setTextSpans", JS_NewCFunction(ctx, js_set_text_spans, "setTextSpans", 2));
    JS_SetPropertyStr(
        ctx, native_ui, "setKeyboardConfig", JS_NewCFunction(ctx, js_set_keyboard_config, "setKeyboardConfig", 1));
    JS_SetPropertyStr(ctx, native_ui, "setVectorOps", JS_NewCFunction(ctx, js_set_vector_ops, "setVectorOps", 5));
    /* Expose the bridge-side vector caps so the JS layer can warn when an <Svg> exceeds them — the bridge
     * silently truncates the op-tape / paint table otherwise (see warnVectorCaps in svg-ops.js). */
    JS_SetPropertyStr(ctx, native_ui, "maxVectorOps", JS_NewInt32(ctx, VEC_BRIDGE_MAX_OPS));
    JS_SetPropertyStr(ctx, native_ui, "maxVectorPaints", JS_NewInt32(ctx, VEC_BRIDGE_MAX_PAINTS));
    JS_SetPropertyStr(ctx, native_ui, "maxVectorGrads", JS_NewInt32(ctx, VEC_BRIDGE_MAX_GRADS));
    JS_SetPropertyStr(ctx, native_ui, "setEvent", JS_NewCFunction(ctx, js_set_event, "setEvent", 3));
    JS_SetPropertyStr(ctx, native_ui, "commit", JS_NewCFunction(ctx, js_commit, "commit", 0));
    JS_SetPropertyStr(ctx, native_ui, "setBatcher", JS_NewCFunction(ctx, js_set_batcher, "setBatcher", 1));
#if ER_PERF_STATS
    JS_SetPropertyStr(
        ctx, native_ui, "perfCallbackBegin", JS_NewCFunction(ctx, js_perf_callback_begin, "perfCallbackBegin", 0));
    JS_SetPropertyStr(
        ctx, native_ui, "perfCallbackEnd", JS_NewCFunction(ctx, js_perf_callback_end, "perfCallbackEnd", 0));
    JS_SetPropertyStr(
        ctx, native_ui, "perfRenderBegin", JS_NewCFunction(ctx, js_perf_render_begin, "perfRenderBegin", 0));
    JS_SetPropertyStr(ctx, native_ui, "perfRenderEnd", JS_NewCFunction(ctx, js_perf_render_end, "perfRenderEnd", 0));
    JS_SetPropertyStr(
        ctx, native_ui, "perfMarshalBegin", JS_NewCFunction(ctx, js_perf_marshal_begin, "perfMarshalBegin", 0));
    JS_SetPropertyStr(ctx, native_ui, "perfMarshalEnd", JS_NewCFunction(ctx, js_perf_marshal_end, "perfMarshalEnd", 0));
#endif
    JS_SetPropertyStr(ctx, native_ui, "now", JS_NewCFunction(ctx, js_now, "now", 0));
    JS_SetPropertyStr(ctx, native_ui, "tick", JS_NewCFunction(ctx, js_tick, "tick", 1));
    JS_SetPropertyStr(ctx,
                      native_ui,
                      "configureNextLayoutAnimation",
                      JS_NewCFunction(ctx, js_configure_next_layout_animation, "configureNextLayoutAnimation", 1));
    JS_SetPropertyStr(ctx,
                      native_ui,
                      "hasPendingLayoutAnimation",
                      JS_NewCFunction(ctx, js_has_pending_layout_animation, "hasPendingLayoutAnimation", 0));

    /* Animated (er_animation_value_* — native-driver animation). */
    JS_SetPropertyStr(
        ctx, native_ui, "animValueCreate", JS_NewCFunction(ctx, js_anim_value_create, "animValueCreate", 1));
    JS_SetPropertyStr(
        ctx, native_ui, "animValueDestroy", JS_NewCFunction(ctx, js_anim_value_destroy, "animValueDestroy", 1));
    JS_SetPropertyStr(ctx, native_ui, "animValueSet", JS_NewCFunction(ctx, js_anim_value_set, "animValueSet", 2));
    JS_SetPropertyStr(ctx, native_ui, "animValueGet", JS_NewCFunction(ctx, js_anim_value_get, "animValueGet", 1));
    JS_SetPropertyStr(ctx, native_ui, "animValueBind", JS_NewCFunction(ctx, js_anim_value_bind, "animValueBind", 3));
    JS_SetPropertyStr(ctx, native_ui, "animUnbind", JS_NewCFunction(ctx, js_anim_unbind, "animUnbind", 2));
    JS_SetPropertyStr(ctx,
                      native_ui,
                      "animValueBindInterpolated",
                      JS_NewCFunction(ctx, js_anim_value_bind_interpolated, "animValueBindInterpolated", 4));
    JS_SetPropertyStr(
        ctx, native_ui, "animValueAnimate", JS_NewCFunction(ctx, js_anim_value_animate, "animValueAnimate", 3));
    JS_SetPropertyStr(ctx, native_ui, "animStop", JS_NewCFunction(ctx, js_anim_stop, "animStop", 1));

    /* Event-handler registry: a plain object owned by NativeUI, so it lives for the context
       lifetime and is freed at teardown. We keep a borrowed reference for C-side lookup. */
    JSValue handlers = JS_NewObject(ctx);
    s_event_handlers = handlers;
    JS_SetPropertyStr(ctx, native_ui, "__er_event_handlers", handlers);

    /* Responder-query registry: same ownership trick. */
    JSValue queries = JS_NewObject(ctx);
    s_query_handlers = queries;
    JS_SetPropertyStr(ctx, native_ui, "__er_query_handlers", queries);

    /* Timer-callback registry: same ownership trick as the event handlers, so each timer's
       [callback, ...args] array is GC-rooted and reclaimed with the context. */
    JSValue timer_handlers = JS_NewObject(ctx);
    s_timer_handlers = timer_handlers;
    JS_SetPropertyStr(ctx, native_ui, "__er_timer_handlers", timer_handlers);

    /* Animation completion-callback registry (Animated `.start(cb)`); same ownership trick. */
    JSValue anim_complete_handlers = JS_NewObject(ctx);
    s_anim_complete_handlers = anim_complete_handlers;
    JS_SetPropertyStr(ctx, native_ui, "__er_animation_complete_handlers", anim_complete_handlers);

    /* Frame-batching slots (the installed batcher + the pump body it runs); same ownership trick. */
    JSValue batch_slots = JS_NewObject(ctx);
    s_batch_slots = batch_slots;
    JS_SetPropertyUint32(ctx, batch_slots, ER_BATCH_SLOT_WRAP, JS_UNDEFINED);
    JS_SetPropertyUint32(ctx, batch_slots, ER_BATCH_SLOT_RUN, JS_NewCFunction(ctx, js_run_pump_body, "runPumpBody", 0));
    JS_SetPropertyStr(ctx, native_ui, "__er_batch_slots", batch_slots);

    /* JS_SetPropertyStr takes ownership of native_ui; no separate free needed. */
    JS_SetPropertyStr(ctx, global, "NativeUI", native_ui);

    /* Standard web timer globals, driven by the host pump (er_bridge_pump) off the engine clock.
       Promises use QuickJS's native job queue; the pump drains it each frame. */
    JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval", JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunction(ctx, js_clear_timer, "clearInterval", 1));

    JS_FreeValue(ctx, global);
}
