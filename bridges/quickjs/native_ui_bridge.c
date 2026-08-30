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

#include "er_scene.h"
#include "native_renderer.h"

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
 * can allocate. Handle 0 is reserved as the invalid sentinel; usable handles are [1, N).
 */
#ifndef ER_BRIDGE_MAX_HANDLES
#define ER_BRIDGE_MAX_HANDLES 1024
#endif

/** @brief Invalid handle sentinel returned by createNode on failure. */
#define ER_BRIDGE_HANDLE_INVALID 0

/**
 * @brief Maximum number of concurrently scheduled timers (setTimeout + setInterval).
 *
 * Fixed pool — no heap growth, matching the embedded ethos. The React scheduler uses at
 * most a couple at a time; app code rarely needs more than a handful.
 */
#ifndef ER_BRIDGE_MAX_TIMERS
#define ER_BRIDGE_MAX_TIMERS 64
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Slot -> node map. Index 0 is reserved (invalid handle); NULL means free. */
static ERNode* s_node_by_handle[ER_BRIDGE_MAX_HANDLES];

/** @brief Stack of freed slot indices available for reuse. */
static int32_t s_free_stack[ER_BRIDGE_MAX_HANDLES];

/** @brief Number of indices currently on the free stack. */
static int s_free_top = 0;

/** @brief Next never-yet-allocated slot index; starts at 1 (0 is the invalid sentinel). */
static int s_high_water = 1;

/** @brief Context the bridge was installed into; used by the event trampoline. */
static JSContext* s_bridge_ctx = NULL;

/**
 * @brief JS object mapping encoded (handle, eventType) keys to JS handler functions.
 *
 * Borrowed reference: the object is owned by the NativeUI global (so it lives for the
 * context lifetime and is freed at teardown). Keys are handle * ER_EVENT_TYPE_COUNT_ + type.
 */
static JSValue s_event_handlers;

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
    int nargs;            /**< Count of extra args forwarded to the callback. */
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

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — handle table
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Allocates a handle slot and binds it to a node.
 *
 * @param[in] node  Node to bind (must be non-NULL).
 *
 * @return Handle in [1, ER_BRIDGE_MAX_HANDLES), or ER_BRIDGE_HANDLE_INVALID if the table is full.
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
    return slot;
}

/**
 * @brief Releases a handle slot, returning it to the free stack.
 *
 * @param[in] handle  Handle previously returned by handle_alloc().
 */
static void handle_free(int32_t handle)
{
    if (handle <= 0 || handle >= ER_BRIDGE_MAX_HANDLES || s_node_by_handle[handle] == NULL)
    {
        return;
    }
    s_node_by_handle[handle] = NULL;
    s_free_stack[s_free_top++] = handle;
}

/**
 * @brief Resolves a handle to its bound node.
 *
 * @param[in] handle  Handle to resolve.
 *
 * @return Bound node, or NULL if the handle is out of range or unbound.
 */
static ERNode* handle_node(int32_t handle)
{
    if (handle <= 0 || handle >= ER_BRIDGE_MAX_HANDLES)
    {
        return NULL;
    }
    return s_node_by_handle[handle];
}

/**
 * @brief Resolves a JS argument to its bound node.
 *
 * @param[in] ctx  QuickJS context.
 * @param[in] v    JS value holding an integer handle.
 *
 * @return Bound node, or NULL if the value is not a valid handle.
 */
static ERNode* node_arg(JSContext* ctx, JSValueConst v)
{
    int32_t h = 0;
    if (JS_ToInt32(ctx, &h, v) != 0)
    {
        return NULL;
    }
    return handle_node(h);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — value coercers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Fetches a property, treating undefined/null/exception as "absent".
 *
 * On success the caller owns *out and must JS_FreeValue() it.
 *
 * @param[in]  ctx  QuickJS context.
 * @param[in]  obj  Object to read from.
 * @param[in]  key  Property name.
 * @param[out] out  Receives the property value when present.
 *
 * @return true when the property is present and meaningful; false otherwise.
 */
static bool prop_get(JSContext* ctx, JSValueConst obj, const char* key, JSValue* out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v) || JS_IsException(v))
    {
        JS_FreeValue(ctx, v);
        return false;
    }
    *out = v;
    return true;
}

/**
 * @brief Coerces a numeric JS value to a clamped int16 pixel dimension.
 *
 * @param[in]  ctx  QuickJS context.
 * @param[in]  v    JS value (expected numeric).
 * @param[out] out  Receives the clamped dimension.
 *
 * @return true on a numeric value; false for strings (e.g. percentages) handled elsewhere.
 */
static bool to_dim(JSContext* ctx, JSValueConst v, int16_t* out)
{
    if (!JS_IsNumber(v))
    {
        return false;
    }
    double d = 0.0;
    if (JS_ToFloat64(ctx, &d, v) != 0)
    {
        return false;
    }
    if (d < INT16_MIN)
    {
        d = INT16_MIN;
    }
    if (d > INT16_MAX)
    {
        d = INT16_MAX;
    }
    *out = (int16_t)d;
    return true;
}

/**
 * @brief Maps a single hex character to its 0–15 value.
 *
 * @param[in] c  ASCII hex digit.
 *
 * @return Nibble value, or -1 if c is not a hex digit.
 */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * @brief Clamps an int to the 0–255 channel range.
 *
 * @param[in] v  Value to clamp.
 *
 * @return v clamped to [0, 255].
 */
static int clamp_u8(int v)
{
    if (v < 0)
    {
        return 0;
    }
    if (v > 255)
    {
        return 255;
    }
    return v;
}

/**
 * @brief Parses a CSS hex color body (the text after '#') into straight-alpha ARGB8888.
 *
 * Accepts #rgb, #rrggbb, and #rrggbbaa forms. Three- and six-digit forms imply full alpha.
 *
 * @param[in]  s    Hex digits (without the leading '#').
 * @param[out] out  Receives the ARGB8888 color.
 *
 * @return true on a valid hex color; false otherwise.
 */
static bool parse_hex_color(const char* s, uint32_t* out)
{
    const size_t n = strlen(s);
    int r, g, b, a = 255;

    if (n == 3)
    {
        const int r1 = hex_nibble(s[0]);
        const int g1 = hex_nibble(s[1]);
        const int b1 = hex_nibble(s[2]);
        if (r1 < 0 || g1 < 0 || b1 < 0)
        {
            return false;
        }
        r = r1 * 17;
        g = g1 * 17;
        b = b1 * 17;
    }
    else if (n == 6 || n == 8)
    {
        int v[8];
        for (size_t i = 0; i < n; i++)
        {
            v[i] = hex_nibble(s[i]);
            if (v[i] < 0)
            {
                return false;
            }
        }
        r = v[0] * 16 + v[1];
        g = v[2] * 16 + v[3];
        b = v[4] * 16 + v[5];
        if (n == 8)
        {
            a = v[6] * 16 + v[7];
        }
    }
    else
    {
        return false;
    }

    *out = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
}

/**
 * @brief Parses a CSS rgb()/rgba() function string into straight-alpha ARGB8888.
 *
 * @param[in]  s    String beginning with "rgb" or "rgba".
 * @param[out] out  Receives the ARGB8888 color.
 *
 * @return true on a valid functional color; false otherwise.
 */
static bool parse_rgb_func(const char* s, uint32_t* out)
{
    const char* p = strchr(s, '(');
    if (!p)
    {
        return false;
    }
    p++;

    double vals[4];
    int count = 0;
    while (count < 4)
    {
        char* end = NULL;
        const double d = strtod(p, &end);
        if (end == p)
        {
            break;
        }
        vals[count++] = d;
        p = end;
        while (*p == ' ' || *p == ',')
        {
            p++;
        }
        if (*p == ')' || *p == '\0')
        {
            break;
        }
    }
    if (count < 3)
    {
        return false;
    }

    const int r = clamp_u8((int)(vals[0] + 0.5));
    const int g = clamp_u8((int)(vals[1] + 0.5));
    const int b = clamp_u8((int)(vals[2] + 0.5));
    const int a = count >= 4 ? clamp_u8((int)(vals[3] * 255.0 + 0.5)) : 255;
    *out = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
}

/**
 * @brief Parses a CSS color string (hex, rgb/rgba, or a small set of named colors).
 *
 * @param[in]  s    Color string.
 * @param[out] out  Receives straight-alpha ARGB8888.
 *
 * @return true when the string was recognised; false otherwise.
 */
static bool parse_css_color(const char* s, uint32_t* out)
{
    while (*s == ' ')
    {
        s++;
    }
    if (*s == '#')
    {
        return parse_hex_color(s + 1, out);
    }
    if (strncmp(s, "rgb", 3) == 0)
    {
        return parse_rgb_func(s, out);
    }

    /* Minimal named-color set; extend as needed. */
    static const struct
    {
        const char* name;
        uint32_t argb;
    } k_named[] = {
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
    for (size_t i = 0; i < sizeof(k_named) / sizeof(k_named[0]); i++)
    {
        if (strcmp(s, k_named[i].name) == 0)
        {
            *out = k_named[i].argb;
            return true;
        }
    }
    return false;
}

/**
 * @brief Coerces a JS color value (number or CSS string) to straight-alpha ARGB8888.
 *
 * @param[in]  ctx  QuickJS context.
 * @param[in]  v    JS value (number is taken as a raw 0xAARRGGBB word).
 * @param[out] out  Receives the ARGB8888 color.
 *
 * @return true on success; false if the value could not be interpreted as a color.
 */
static bool to_color(JSContext* ctx, JSValueConst v, uint32_t* out)
{
    if (JS_IsNumber(v))
    {
        int64_t n = 0;
        if (JS_ToInt64(ctx, &n, v) != 0)
        {
            return false;
        }
        *out = (uint32_t)n;
        return true;
    }
    if (JS_IsString(v))
    {
        const char* s = JS_ToCString(ctx, v);
        if (!s)
        {
            return false;
        }
        const bool ok = parse_css_color(s, out);
        JS_FreeCString(ctx, s);
        return ok;
    }
    return false;
}

/**
 * @brief Coerces a JS angle value (number in degrees, or a "Ndeg"/"Nrad" string) to degrees.
 *
 * @param[in]  ctx  QuickJS context.
 * @param[in]  v    JS value to coerce.
 * @param[out] out  Receives the angle in degrees.
 *
 * @return true on success; false if the value is not a recognisable angle.
 */
static bool to_angle(JSContext* ctx, JSValueConst v, float* out)
{
    if (JS_IsNumber(v))
    {
        double d = 0.0;
        if (JS_ToFloat64(ctx, &d, v) != 0)
        {
            return false;
        }
        *out = (float)d;
        return true;
    }
    if (JS_IsString(v))
    {
        const char* s = JS_ToCString(ctx, v);
        if (!s)
        {
            return false;
        }
        char* end = NULL;
        double d = strtod(s, &end);
        bool ok = end != s;
        if (ok && strstr(end, "rad") != NULL)
        {
            d = d * 180.0 / 3.14159265358979323846;
        }
        JS_FreeCString(ctx, s);
        if (ok)
        {
            *out = (float)d;
        }
        return ok;
    }
    return false;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — enum string maps
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maps a flexDirection string to ERFlexDirection. @param[in] s String. @return Enum value. */
static uint8_t map_flex_direction(const char* s)
{
    if (strcmp(s, "row") == 0)
    {
        return ER_FLEX_ROW;
    }
    if (strcmp(s, "row-reverse") == 0)
    {
        return ER_FLEX_ROW_REVERSE;
    }
    if (strcmp(s, "column-reverse") == 0)
    {
        return ER_FLEX_COL_REVERSE;
    }
    return ER_FLEX_COL;
}

/** @brief Maps a justifyContent string to ERFlexJustify. @param[in] s String. @return Enum value. */
static uint8_t map_justify(const char* s)
{
    if (strcmp(s, "center") == 0)
    {
        return ER_JUSTIFY_CENTER;
    }
    if (strcmp(s, "flex-end") == 0)
    {
        return ER_JUSTIFY_FLEX_END;
    }
    if (strcmp(s, "space-between") == 0)
    {
        return ER_JUSTIFY_SPACE_BETWEEN;
    }
    if (strcmp(s, "space-around") == 0)
    {
        return ER_JUSTIFY_SPACE_AROUND;
    }
    if (strcmp(s, "space-evenly") == 0)
    {
        return ER_JUSTIFY_SPACE_EVENLY;
    }
    return ER_JUSTIFY_FLEX_START;
}

/** @brief Maps an align string to ERFlexAlign. @param[in] s String. @return Enum value. */
static uint8_t map_align(const char* s)
{
    if (strcmp(s, "stretch") == 0)
    {
        return ER_ALIGN_STRETCH;
    }
    if (strcmp(s, "flex-start") == 0)
    {
        return ER_ALIGN_FLEX_START;
    }
    if (strcmp(s, "center") == 0)
    {
        return ER_ALIGN_CENTER;
    }
    if (strcmp(s, "flex-end") == 0)
    {
        return ER_ALIGN_FLEX_END;
    }
    return ER_ALIGN_AUTO;
}

/** @brief Maps an alignContent string to ERAlignContent. @param[in] s String. @return Enum value. */
static uint8_t map_align_content(const char* s)
{
    if (strcmp(s, "flex-end") == 0)
    {
        return ER_ALIGN_CONTENT_FLEX_END;
    }
    if (strcmp(s, "center") == 0)
    {
        return ER_ALIGN_CONTENT_CENTER;
    }
    if (strcmp(s, "stretch") == 0)
    {
        return ER_ALIGN_CONTENT_STRETCH;
    }
    if (strcmp(s, "space-between") == 0)
    {
        return ER_ALIGN_CONTENT_SPACE_BETWEEN;
    }
    if (strcmp(s, "space-around") == 0)
    {
        return ER_ALIGN_CONTENT_SPACE_AROUND;
    }
    return ER_ALIGN_CONTENT_FLEX_START;
}

/** @brief Maps a position string to ERPositionType. @param[in] s String. @return Enum value. */
static uint8_t map_position(const char* s)
{
    return strcmp(s, "absolute") == 0 ? ER_POS_ABSOLUTE : ER_POS_RELATIVE;
}

/** @brief Maps a display string to ERDisplayMode. @param[in] s String. @return Enum value. */
static uint8_t map_display(const char* s)
{
    return strcmp(s, "none") == 0 ? ER_DISPLAY_NONE : ER_DISPLAY_FLEX;
}

/** @brief Maps an overflow string to EROverflow. @param[in] s String. @return Enum value. */
static uint8_t map_overflow(const char* s)
{
    if (strcmp(s, "hidden") == 0)
    {
        return ER_OVERFLOW_HIDDEN;
    }
    if (strcmp(s, "scroll") == 0)
    {
        return ER_OVERFLOW_SCROLL;
    }
    return ER_OVERFLOW_VISIBLE;
}

/** @brief Maps a textAlign string to ERTextAlign. @param[in] s String. @return Enum value. */
static uint8_t map_text_align(const char* s)
{
    if (strcmp(s, "center") == 0)
    {
        return ER_TEXT_ALIGN_CENTER;
    }
    if (strcmp(s, "right") == 0)
    {
        return ER_TEXT_ALIGN_RIGHT;
    }
    return ER_TEXT_ALIGN_LEFT;
}

/** @brief Maps a resizeMode string to ERResizeMode. @param[in] s String. @return Enum value. */
static uint8_t map_resize_mode(const char* s)
{
    if (strcmp(s, "contain") == 0)
    {
        return ER_RESIZE_CONTAIN;
    }
    if (strcmp(s, "stretch") == 0)
    {
        return ER_RESIZE_STRETCH;
    }
    if (strcmp(s, "repeat") == 0)
    {
        return ER_RESIZE_REPEAT;
    }
    if (strcmp(s, "center") == 0)
    {
        return ER_RESIZE_CENTER;
    }
    return ER_RESIZE_COVER;
}

/** @brief Maps a flexWrap string to ERFlexWrap. @param[in] s String. @return Enum value. */
static uint8_t map_flex_wrap(const char* s)
{
    if (strcmp(s, "wrap") == 0)
    {
        return ER_WRAP_WRAP;
    }
    if (strcmp(s, "wrap-reverse") == 0)
    {
        return ER_WRAP_WRAP_REVERSE;
    }
    return ER_WRAP_NOWRAP;
}

/** @brief Maps a fontStyle string to ERFontStyle. @param[in] s String. @return Enum value. */
static uint8_t map_font_style(const char* s)
{
    return strcmp(s, "italic") == 0 ? ER_FONT_STYLE_ITALIC : ER_FONT_STYLE_NORMAL;
}

/** @brief Maps a borderStyle string to ERBorderStyle. @param[in] s String. @return Enum value. */
static uint8_t map_border_style(const char* s)
{
    if (strcmp(s, "dashed") == 0)
    {
        return ER_BORDER_DASHED;
    }
    if (strcmp(s, "dotted") == 0)
    {
        return ER_BORDER_DOTTED;
    }
    return ER_BORDER_SOLID;
}

/** @brief Maps an ellipsizeMode string to ERTextEllipsize. @param[in] s String. @return Enum value. */
static uint8_t map_ellipsize(const char* s)
{
    if (strcmp(s, "head") == 0)
    {
        return ER_TEXT_ELLIPSIZE_HEAD;
    }
    if (strcmp(s, "middle") == 0)
    {
        return ER_TEXT_ELLIPSIZE_MIDDLE;
    }
    if (strcmp(s, "clip") == 0)
    {
        return ER_TEXT_ELLIPSIZE_CLIP;
    }
    return ER_TEXT_ELLIPSIZE_TAIL;
}

/** @brief Maps a textDecorationLine string to ERTextDecoration. @param[in] s String. @return Enum value. */
static uint8_t map_text_decoration(const char* s)
{
    if (strcmp(s, "underline") == 0)
    {
        return ER_TEXT_DECORATION_UNDERLINE;
    }
    if (strcmp(s, "line-through") == 0)
    {
        return ER_TEXT_DECORATION_LINE_THROUGH;
    }
    return ER_TEXT_DECORATION_NONE;
}

/** @brief Maps a pointerEvents string to ERPointerEvents. @param[in] s String. @return Enum value. */
static uint8_t map_pointer_events(const char* s)
{
    if (strcmp(s, "none") == 0)
    {
        return ER_POINTER_EVENTS_NONE;
    }
    if (strcmp(s, "box-only") == 0)
    {
        return ER_POINTER_EVENTS_BOX_ONLY;
    }
    if (strcmp(s, "box-none") == 0)
    {
        return ER_POINTER_EVENTS_BOX_NONE;
    }
    return ER_POINTER_EVENTS_AUTO;
}

/** @brief Maps a node-type string to ERNodeType. @param[in] s String. @return Enum value (View on miss). */
static ERNodeType map_node_type(const char* s)
{
    if (strcmp(s, "Text") == 0)
    {
        return ER_NODE_TEXT;
    }
    if (strcmp(s, "Image") == 0)
    {
        return ER_NODE_IMAGE;
    }
    if (strcmp(s, "ScrollView") == 0)
    {
        return ER_NODE_SCROLL_VIEW;
    }
    if (strcmp(s, "FlatList") == 0)
    {
        return ER_NODE_FLAT_LIST;
    }
    if (strcmp(s, "Pressable") == 0)
    {
        return ER_NODE_PRESSABLE;
    }
    if (strcmp(s, "TextInput") == 0)
    {
        return ER_NODE_TEXT_INPUT;
    }
    if (strcmp(s, "ActivityIndicator") == 0)
    {
        return ER_NODE_ACTIVITY_INDICATOR;
    }
    if (strcmp(s, "Switch") == 0)
    {
        return ER_NODE_SWITCH;
    }
    if (strcmp(s, "Modal") == 0)
    {
        return ER_NODE_MODAL;
    }
    if (strcmp(s, "Svg") == 0)
    {
        return ER_NODE_VECTOR;
    }
    if (strcmp(s, "Dial") == 0)
    {
        return ER_NODE_ARC;
    }
    return ER_NODE_VIEW;
}

/** @brief Maps a Dial `cap` token to ERArcCap. @param[in] s String. @return Enum value (default butt). */
static uint8_t map_arc_cap(const char* s)
{
    if (strcmp(s, "round") == 0)
    {
        return ER_ARC_CAP_ROUND;
    }
    return ER_ARC_CAP_BUTT;
}

/** @brief Maps a Dial `knob` token to ERArcKnob. @param[in] s String. @return Enum value (default none). */
static uint8_t map_arc_knob(const char* s)
{
    if (strcmp(s, "circle") == 0)
    {
        return ER_ARC_KNOB_CIRCLE;
    }
    if (strcmp(s, "image") == 0)
    {
        return ER_ARC_KNOB_IMAGE;
    }
    if (strcmp(s, "child") == 0)
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
 * @param[out] p  Props to initialise.
 */
static void props_init_defaults(ERProps* p)
{
    /* The default seeding lives in the engine (er_props_default) so the Flow B AOT codegen produces
       byte-identical prop bags. This wrapper stays for the call sites + the doc comment above. */
    er_props_default(p);
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

static int prop_atom_entry_cmp(const void* a, const void* b)
{
    const JSAtom aa = ((const PropAtomEntry*)a)->atom;
    const JSAtom ab = ((const PropAtomEntry*)b)->atom;
    return (aa > ab) - (aa < ab);
}

/**
 * @brief Interns every apply_props() key into s_prop_atoms, sorted for prop_id_from_atom()'s bsearch.
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
    for (int i = 0; i < PROP_COUNT_; i++)
    {
        s_prop_atoms[i].atom = JS_NewAtom(ctx, k_prop_names[i]);
        s_prop_atoms[i].id = (PropId)i;
    }
    qsort(s_prop_atoms, PROP_COUNT_, sizeof(s_prop_atoms[0]), prop_atom_entry_cmp);
    s_prop_atoms_rt = rt;
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

/* Marshalling convenience macros — each reads one slot (populated from the object's own keys by
   apply_props()) into the ERProps `p`. They share the locals (ctx, p) of apply_props() and are
   #undef'd at its end. */

#define ER_DIM(id, field)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _v = s_prop_slots[id];                                                                            \
        if (!JS_IsUndefined(_v))                                                                                       \
        {                                                                                                              \
            int16_t _d;                                                                                                \
            if (to_dim(ctx, _v, &_d))                                                                                  \
            {                                                                                                          \
                p.field = _d;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_COL(id, field)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _v = s_prop_slots[id];                                                                            \
        if (!JS_IsUndefined(_v))                                                                                       \
        {                                                                                                              \
            uint32_t _c;                                                                                               \
            if (to_color(ctx, _v, &_c))                                                                                \
            {                                                                                                          \
                p.field = _c;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_ENUM(id, field, fn)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _v = s_prop_slots[id];                                                                            \
        if (!JS_IsUndefined(_v))                                                                                       \
        {                                                                                                              \
            const char* _s = JS_ToCString(ctx, _v);                                                                    \
            if (_s)                                                                                                    \
            {                                                                                                          \
                p.field = fn(_s);                                                                                      \
                JS_FreeCString(ctx, _s);                                                                               \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_U8(id, field)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _v = s_prop_slots[id];                                                                            \
        if (!JS_IsUndefined(_v))                                                                                       \
        {                                                                                                              \
            int32_t _n;                                                                                                \
            if (JS_ToInt32(ctx, &_n, _v) == 0)                                                                         \
            {                                                                                                          \
                p.field = (uint8_t)clamp_u8(_n);                                                                       \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_F32(id, field)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _v = s_prop_slots[id];                                                                            \
        if (!JS_IsUndefined(_v))                                                                                       \
        {                                                                                                              \
            double _d;                                                                                                 \
            if (JS_ToFloat64(ctx, &_d, _v) == 0)                                                                       \
            {                                                                                                          \
                p.field = (float)_d;                                                                                   \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define ER_STR(id, field, maxlen)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        JSValueConst _v = s_prop_slots[id];                                                                            \
        if (!JS_IsUndefined(_v))                                                                                       \
        {                                                                                                              \
            const char* _s = JS_ToCString(ctx, _v);                                                                    \
            if (_s)                                                                                                    \
            {                                                                                                          \
                strncpy(p.field, _s, (maxlen));                                                                        \
                p.field[(maxlen)] = '\0';                                                                              \
                JS_FreeCString(ctx, _s);                                                                               \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

/**
 * @brief Reads the `flex` shorthand and expands it to grow/shrink/basis with RN semantics.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     obj  Source props object.
 * @param[in,out] p    Props to update.
 */
static void apply_flex_shorthand(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    double d = 0.0;
    if (JS_ToFloat64(ctx, &d, v) == 0)
    {
        if (d > 0.0)
        {
            p->flex_grow = (int16_t)d;
            p->flex_shrink = 1;
            p->flex_basis = 0;
        }
        else if (d == 0.0)
        {
            p->flex_grow = 0;
            p->flex_shrink = 0;
        }
        else
        {
            p->flex_grow = 0;
            p->flex_shrink = 1;
        }
    }
}

/**
 * @brief Reads `flexBasis`, supporting both numeric pixels and percentage strings.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     v    Pre-fetched `flexBasis` value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_flex_basis(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    if (JS_IsNumber(v))
    {
        int16_t d;
        if (to_dim(ctx, v, &d))
        {
            p->flex_basis = d;
        }
    }
    else if (JS_IsString(v))
    {
        const char* s = JS_ToCString(ctx, v);
        if (s)
        {
            char* end = NULL;
            const double pct = strtod(s, &end);
            if (end != s && *end == '%')
            {
                p->flex_basis_pct = (float)pct;
            }
            JS_FreeCString(ctx, s);
        }
    }
}

/**
 * @brief Reads a dimension value that may be a number (pixels) or a percentage string.
 *
 * A numeric value sets the px field; a `"N%"` string sets the percent field (which the engine
 * resolves against the parent's content size and which takes precedence over the px field).
 *
 * @param[in]  ctx  QuickJS context.
 * @param[in]  v    Pre-fetched value (e.g. `width`), JS_UNDEFINED if absent.
 * @param[out] px   Receives the pixel value when the JS value is numeric.
 * @param[out] pct  Receives the percentage when the JS value is a `"N%"` string.
 */
static void apply_dim_pct(JSContext* ctx, JSValueConst v, int16_t* px, float* pct)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    if (JS_IsNumber(v))
    {
        int16_t d;
        if (to_dim(ctx, v, &d))
        {
            *px = d;
        }
    }
    else if (JS_IsString(v))
    {
        const char* s = JS_ToCString(ctx, v);
        if (s)
        {
            char* end = NULL;
            const double p = strtod(s, &end);
            if (end != s && *end == '%')
            {
                *pct = (float)p;
            }
            JS_FreeCString(ctx, s);
        }
    }
}

/**
 * @brief Reads `fontWeight` (string keyword, numeric, or numeric string) into a 0/1 weight.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     v    Pre-fetched `fontWeight` value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_font_weight(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    if (JS_IsNumber(v))
    {
        int32_t n = 0;
        if (JS_ToInt32(ctx, &n, v) == 0)
        {
            p->font_weight = n >= 600 ? 1 : 0;
        }
    }
    else
    {
        const char* s = JS_ToCString(ctx, v);
        if (s)
        {
            p->font_weight = (strcmp(s, "bold") == 0 || atoi(s) >= 600) ? 1 : 0;
            JS_FreeCString(ctx, s);
        }
    }
}

/**
 * @brief Reads `opacity` (RN 0.0–1.0 float) into the engine's 0–255 byte opacity.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     v    Pre-fetched `opacity` value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_opacity(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    double d = 1.0;
    if (JS_ToFloat64(ctx, &d, v) == 0)
    {
        if (d < 0.0)
        {
            d = 0.0;
        }
        if (d > 1.0)
        {
            d = 1.0;
        }
        p->opacity = (uint8_t)(d * 255.0 + 0.5);
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
    JSValue v;
    if (!prop_get(ctx, item, key, &v))
    {
        return false;
    }
    double d = 0.0;
    const bool ok = JS_ToFloat64(ctx, &d, v) == 0;
    if (ok)
    {
        *out = (float)d;
    }
    JS_FreeValue(ctx, v);
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
    JSValue v;
    if (!prop_get(ctx, item, key, &v))
    {
        return false;
    }
    const bool ok = to_angle(ctx, v, out);
    JS_FreeValue(ctx, v);
    return ok;
}

/**
 * @brief Flattens the RN `transform` array (list of single-key objects) into transform_* fields.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     arr  Pre-fetched `transform` value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_transform(JSContext* ctx, JSValueConst arr, ERProps* p)
{
    if (JS_IsUndefined(arr))
    {
        return;
    }
    if (JS_IsArray(arr))
    {
        JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);

        for (int32_t i = 0; i < len; i++)
        {
            JSValue item = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            if (JS_IsObject(item))
            {
                float f;
                if (item_f32(ctx, item, "translateX", &f))
                {
                    p->transform_translate_x = f;
                }
                if (item_f32(ctx, item, "translateY", &f))
                {
                    p->transform_translate_y = f;
                }
                if (item_f32(ctx, item, "scale", &f))
                {
                    p->transform_scale_x = f;
                    p->transform_scale_y = f;
                }
                if (item_f32(ctx, item, "scaleX", &f))
                {
                    p->transform_scale_x = f;
                }
                if (item_f32(ctx, item, "scaleY", &f))
                {
                    p->transform_scale_y = f;
                }
                if (item_angle(ctx, item, "rotate", &f))
                {
                    p->transform_rotate_z = f;
                }
                if (item_angle(ctx, item, "rotateZ", &f))
                {
                    p->transform_rotate_z = f;
                }
                if (item_angle(ctx, item, "rotateX", &f))
                {
                    p->transform_rotate_x = f;
                }
                if (item_angle(ctx, item, "rotateY", &f))
                {
                    p->transform_rotate_y = f;
                }
                if (item_f32(ctx, item, "perspective", &f))
                {
                    p->transform_perspective = f;
                }
            }
            JS_FreeValue(ctx, item);
        }
    }
}

/**
 * @brief Reads `transformOrigin` as a two-element [x, y] array of fractional pivots [0–1].
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     v    Pre-fetched `transformOrigin` value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_transform_origin(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    if (JS_IsArray(v))
    {
        JSValue e0 = JS_GetPropertyUint32(ctx, v, 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, v, 1);
        double ox = 0.0;
        double oy = 0.0;
        if (JS_ToFloat64(ctx, &ox, e0) == 0)
        {
            p->transform_origin_x = (float)ox;
        }
        if (JS_ToFloat64(ctx, &oy, e1) == 0)
        {
            p->transform_origin_y = (float)oy;
        }
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
    }
}

/**
 * @brief Reads `shadowOffset` ({width, height}) into the shadow_offset_x/y fields.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     v    Pre-fetched `shadowOffset` value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_shadow_offset(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    if (JS_IsObject(v))
    {
        JSValue w = JS_GetPropertyStr(ctx, v, "width");
        JSValue h = JS_GetPropertyStr(ctx, v, "height");
        double dw = 0.0;
        double dh = 0.0;
        if (JS_ToFloat64(ctx, &dw, w) == 0)
        {
            p->shadow_offset_x = (float)dw;
        }
        if (JS_ToFloat64(ctx, &dh, h) == 0)
        {
            p->shadow_offset_y = (float)dh;
        }
        JS_FreeValue(ctx, w);
        JS_FreeValue(ctx, h);
    }
}

/**
 * @brief Reads `trackColor` ({false, true}) into the Switch track_color_false/true fields.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     v    Pre-fetched `trackColor` value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_track_color(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v))
    {
        return;
    }
    if (!JS_IsObject(v))
    {
        /* A plain colour is the Dial's single-colour track (the Switch form is the {false, true} pair). */
        uint32_t c;
        if (to_color(ctx, v, &c))
        {
            p->arc_track_color = c;
        }
        return;
    }
    if (JS_IsObject(v))
    {
        JSValue f = JS_GetPropertyStr(ctx, v, "false");
        JSValue t = JS_GetPropertyStr(ctx, v, "true");
        uint32_t c;
        if (to_color(ctx, f, &c))
        {
            p->track_color_false = c;
        }
        if (to_color(ctx, t, &c))
        {
            p->track_color_true = c;
        }
        JS_FreeValue(ctx, f);
        JS_FreeValue(ctx, t);
    }
}

/**
 * @brief Reads a Dial `indicatorGradient` ({type: 'conic'|'radial', stops: [{color, offset}]}) into the
 *        View-gradient fields, which ER_NODE_ARC reads as its indicator paint.
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     v    Pre-fetched value (JS_UNDEFINED if absent).
 * @param[in,out] p    Props to update.
 */
static void apply_indicator_gradient(JSContext* ctx, JSValueConst v, ERProps* p)
{
    if (JS_IsUndefined(v) || JS_IsNull(v) || !JS_IsObject(v))
    {
        return;
    }
    JSValue t = JS_GetPropertyStr(ctx, v, "type");
    const char* ts = JS_ToCString(ctx, t);
    if (ts)
    {
        p->gradient_type = (strcmp(ts, "radial") == 0) ? ER_GRADIENT_RADIAL : ER_GRADIENT_CONIC;
        JS_FreeCString(ctx, ts);
    }
    JS_FreeValue(ctx, t);

    JSValue stops = JS_GetPropertyStr(ctx, v, "stops");
    if (JS_IsArray(stops))
    {
        JSValue lenv = JS_GetPropertyStr(ctx, stops, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenv);
        JS_FreeValue(ctx, lenv);
        if (len > ER_GRADIENT_MAX_STOPS)
        {
            len = ER_GRADIENT_MAX_STOPS;
        }
        uint8_t n = 0;
        for (int32_t i = 0; i < len; i++)
        {
            JSValue st = JS_GetPropertyUint32(ctx, stops, (uint32_t)i);
            JSValue cv = JS_GetPropertyStr(ctx, st, "color");
            JSValue ov = JS_GetPropertyStr(ctx, st, "offset");
            uint32_t c;
            double o = (len > 1) ? (double)i / (double)(len - 1) : 0.0;
            if (!JS_IsUndefined(ov))
            {
                JS_ToFloat64(ctx, &o, ov);
            }
            if (to_color(ctx, cv, &c))
            {
                p->gradient_stops[n].color = c;
                p->gradient_stops[n].position = (float)o;
                n++;
            }
            JS_FreeValue(ctx, ov);
            JS_FreeValue(ctx, cv);
            JS_FreeValue(ctx, st);
        }
        p->gradient_stop_count = n;
    }
    JS_FreeValue(ctx, stops);
}

/**
 * @brief Reads obj's own enumerable string keys into s_prop_slots.
 *
 * Keys apply_props() doesn't recognise, and keys whose value is undefined/null/exception (same
 * filter prop_get() used to apply), are skipped — s_prop_slots[id] stays JS_UNDEFINED for those,
 * matching "absent" for every ER_* macro and custom applier.
 *
 * @param[in] ctx  QuickJS context.
 * @param[in] obj  Flat props object passed to apply_props().
 */
static void gather_prop_slots(JSContext* ctx, JSValueConst obj)
{
    for (int i = 0; i < PROP_COUNT_; i++)
    {
        s_prop_slots[i] = JS_UNDEFINED;
    }

    JSPropertyEnum* tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
    {
        return;
    }
    for (uint32_t i = 0; i < len; i++)
    {
        const PropId id = prop_id_from_atom(tab[i].atom);
        if (id == PROP_COUNT_)
        {
            continue;
        }
        JSValue v = JS_GetProperty(ctx, obj, tab[i].atom);
        if (JS_IsUndefined(v) || JS_IsNull(v) || JS_IsException(v))
        {
            JS_FreeValue(ctx, v);
            continue;
        }
        s_prop_slots[id] = v;
    }
    JS_FreePropertyEnum(ctx, tab, len);
}

/**
 * @brief Translates a flat JS props object into an ERProps and applies it to a node.
 *
 * Reads a single flat object (the JS layer merges resolved style + props before calling).
 * Unrecognised keys are ignored; unspecified props fall back to engine defaults.
 *
 * Rather than probing all ~90 known prop names against `obj` (a JS_GetPropertyStr per name, every
 * call, regardless of how many keys `obj` actually has), this walks obj's own keys once via
 * JS_GetOwnPropertyNames and dispatches each into s_prop_slots by atom — see gather_prop_slots().
 * The statement order below is unchanged from the probing version and still matters: the `flex`
 * shorthand must run before the explicit flexGrow/flexShrink/flexBasis overrides, and `color`
 * legitimately feeds both the text `color` field and ActivityIndicator's `indicator_color`.
 *
 * @param[in] ctx   QuickJS context.
 * @param[in] node  Target node.
 * @param[in] obj   Flat props object.
 */
static void apply_props(JSContext* ctx, ERNode* node, JSValueConst obj)
{
    ERProps p;
    props_init_defaults(&p);

    gather_prop_slots(ctx, obj);

    /* Layout — dimensions and box model. */
    apply_dim_pct(ctx, s_prop_slots[PROP_WIDTH], &p.width, &p.width_pct);
    apply_dim_pct(ctx, s_prop_slots[PROP_HEIGHT], &p.height, &p.height_pct);
    ER_DIM(PROP_MIN_WIDTH, min_width);
    ER_DIM(PROP_MAX_WIDTH, max_width);
    ER_DIM(PROP_MIN_HEIGHT, min_height);
    ER_DIM(PROP_MAX_HEIGHT, max_height);
    ER_DIM(PROP_TOP, top);
    ER_DIM(PROP_LEFT, left);
    ER_DIM(PROP_RIGHT, right);
    ER_DIM(PROP_BOTTOM, bottom);
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
    apply_flex_shorthand(ctx, s_prop_slots[PROP_FLEX], &p);
    ER_DIM(PROP_FLEX_GROW, flex_grow);
    ER_DIM(PROP_FLEX_SHRINK, flex_shrink);
    apply_flex_basis(ctx, s_prop_slots[PROP_FLEX_BASIS], &p);
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

    {
        JSValueConst v = s_prop_slots[PROP_ASPECT_RATIO];
        if (!JS_IsUndefined(v))
        {
            double d = 0.0;
            if (JS_ToFloat64(ctx, &d, v) == 0)
            {
                p.aspect_ratio = (float)d;
            }
        }
    }

    /* View visual. */
    ER_COL(PROP_BACKGROUND_COLOR, background_color);
    apply_opacity(ctx, s_prop_slots[PROP_OPACITY], &p);
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
    apply_font_weight(ctx, s_prop_slots[PROP_FONT_WEIGHT], &p);
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
    apply_transform(ctx, s_prop_slots[PROP_TRANSFORM], &p);
    apply_transform_origin(ctx, s_prop_slots[PROP_TRANSFORM_ORIGIN], &p);

    /* Shadow (View-family; rendered only when ERUI_SHADOWS and shadow_opacity > 0). */
    ER_COL(PROP_SHADOW_COLOR, shadow_color);
    apply_shadow_offset(ctx, s_prop_slots[PROP_SHADOW_OFFSET], &p);
    {
        JSValueConst v = s_prop_slots[PROP_SHADOW_OPACITY];
        if (!JS_IsUndefined(v))
        {
            double d = 0.0;
            if (JS_ToFloat64(ctx, &d, v) == 0)
            {
                p.shadow_opacity = (float)d;
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
    apply_track_color(ctx, s_prop_slots[PROP_TRACK_COLOR], &p);
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
    apply_indicator_gradient(ctx, s_prop_slots[PROP_INDICATOR_GRADIENT], &p);

    /* TextInput. */
    ER_STR(PROP_PLACEHOLDER, placeholder, ER_PLACEHOLDER_MAX);
    ER_COL(PROP_PLACEHOLDER_TEXT_COLOR, placeholder_color);
    ER_COL(PROP_CURSOR_COLOR, cursor_color);
    ER_U8(PROP_EDITABLE, editable);

    /* Modal. */
    ER_U8(PROP_VISIBLE, modal_visible);
    ER_COL(PROP_BACKDROP_COLOR, backdrop_color);

    er_node_set_props(node, &p);

    for (int i = 0; i < PROP_COUNT_; i++)
    {
        JS_FreeValue(ctx, s_prop_slots[i]);
        s_prop_slots[i] = JS_UNDEFINED;
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
    } k_map[] = {
        {"onPress", ER_EVENT_PRESS},
        {"onLongPress", ER_EVENT_LONG_PRESS},
        {"onPressIn", ER_EVENT_PRESS_IN},
        {"onPressOut", ER_EVENT_PRESS_OUT},
        {"onTouchStart", ER_EVENT_TOUCH_START},
        {"onTouchMove", ER_EVENT_TOUCH_MOVE},
        {"onTouchEnd", ER_EVENT_TOUCH_END},
        {"onTouchCancel", ER_EVENT_TOUCH_CANCEL},
        {"onScroll", ER_EVENT_SCROLL},
        {"onChangeText", ER_EVENT_CHANGE_TEXT},
        {"onSubmitEditing", ER_EVENT_SUBMIT_EDITING},
        {"onFocus", ER_EVENT_FOCUS},
        {"onBlur", ER_EVENT_BLUR},
        {"onLayout", ER_EVENT_LAYOUT},
        {"onChange", ER_EVENT_VALUE_CHANGE},      /* Dial: the new value */
        {"onValueChange", ER_EVENT_VALUE_CHANGE}, /* Switch: the new boolean; Dial: the new value */
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++)
    {
        if (strcmp(name, k_map[i].name) == 0)
        {
            *out = k_map[i].type;
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
    JSValue ev = JS_NewObject(ctx);
    if (type < ER_EVENT_TYPE_COUNT_)
    {
        JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, k_event_names[type]));
    }
    JS_SetPropertyStr(ctx, ev, "x", JS_NewInt32(ctx, data->x));
    JS_SetPropertyStr(ctx, ev, "y", JS_NewInt32(ctx, data->y));
    JS_SetPropertyStr(ctx, ev, "dx", JS_NewInt32(ctx, data->dx));
    JS_SetPropertyStr(ctx, ev, "dy", JS_NewInt32(ctx, data->dy));

    if (type == ER_EVENT_SCROLL)
    {
        JS_SetPropertyStr(ctx, ev, "scrollX", JS_NewFloat64(ctx, (double)data->scroll_x));
        JS_SetPropertyStr(ctx, ev, "scrollY", JS_NewFloat64(ctx, (double)data->scroll_y));
    }
    else if (type == ER_EVENT_LAYOUT)
    {
        JSValue r = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, r, "x", JS_NewInt32(ctx, data->layout_rect.x));
        JS_SetPropertyStr(ctx, r, "y", JS_NewInt32(ctx, data->layout_rect.y));
        JS_SetPropertyStr(ctx, r, "width", JS_NewInt32(ctx, data->layout_rect.w));
        JS_SetPropertyStr(ctx, r, "height", JS_NewInt32(ctx, data->layout_rect.h));
        JS_SetPropertyStr(ctx, ev, "layout", r);
    }
    else if (type == ER_EVENT_CHANGE_TEXT && data->changed_text)
    {
        JS_SetPropertyStr(ctx, ev, "text", JS_NewString(ctx, data->changed_text));
    }
    else if (type == ER_EVENT_VALUE_CHANGE)
    {
        JS_SetPropertyStr(ctx, ev, "value", JS_NewFloat64(ctx, (double)data->value));
        JS_SetPropertyStr(ctx, ev, "valueStart", JS_NewFloat64(ctx, (double)data->value_start));
    }
    return ev;
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

    JSValue cb = JS_GetPropertyUint32(ctx, s_event_handlers, key);
    if (JS_IsFunction(ctx, cb))
    {
        JSValue ev2 = JS_UNDEFINED;
        int argc2 = 1;
        /* onChangeText receives the new TEXT directly (RN convention, matching Flow B's AOT lowering);
         * every other event gets the event object. */
        JSValue ev;
        if (type == ER_EVENT_CHANGE_TEXT)
        {
            ev = JS_NewString(ctx, data->changed_text ? data->changed_text : "");
        }
        else if (type == ER_EVENT_VALUE_CHANGE)
        {
            /* onValueChange / onChange receive the new VALUE directly: a boolean for Switch (RN's
             * onValueChange(boolean)), a number for Dial. A RANGE Dial passes the band's low end as a
             * SECOND argument — two plain numbers rather than an object, so the AOT can lower the same
             * handler shape to `data->value` / `data->value_start` with no allocation on device. */
            ev = (er_node_get_type(node) == ER_NODE_SWITCH) ? JS_NewBool(ctx, data->value != 0.0f)
                                                            : JS_NewFloat64(ctx, (double)data->value);
            ev2 = JS_NewFloat64(ctx, (double)data->value_start);
            argc2 = 2;
        }
        else
        {
            ev = build_event_object(ctx, type, data);
        }
        JSValue argv2[2] = {ev, ev2};
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, argc2, argv2);
        if (JS_IsException(ret))
        {
            JSValue exc = JS_GetException(ctx);
            const char* msg = JS_ToCString(ctx, exc);
            fprintf(stderr, "JS event handler exception: %s\n", msg ? msg : "(unknown)");
            if (msg)
            {
                JS_FreeCString(ctx, msg);
            }
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, ev);
        JS_FreeValue(ctx, ev2);
    }
    JS_FreeValue(ctx, cb);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — timers & job queue
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a pending JS exception (message + stack) to stderr and clears it.
 *
 * @param[in] ctx    Context whose current exception should be reported.
 * @param[in] where  Short label for the failure site (e.g. "timer callback").
 */
static void bridge_report_exception(JSContext* ctx, const char* where)
{
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    fprintf(stderr, "JS %s exception: %s\n", where, msg ? msg : "(unknown)");
    if (msg)
    {
        JS_FreeCString(ctx, msg);
    }
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack))
    {
        const char* st = JS_ToCString(ctx, stack);
        if (st)
        {
            fprintf(stderr, "%s\n", st);
            JS_FreeCString(ctx, st);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

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
    for (int i = 0; i < ER_BRIDGE_MAX_TIMERS; i++)
    {
        if (!s_timers[i].active)
        {
            slot = i;
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
    const int nargs = argc > 2 ? argc - 2 : 0;
    JSValue bundle = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, bundle, 0, JS_DupValue(ctx, argv[0]));
    for (int k = 0; k < nargs; k++)
    {
        JS_SetPropertyUint32(ctx, bundle, (uint32_t)(k + 1), JS_DupValue(ctx, argv[2 + k]));
    }
    JS_SetPropertyUint32(ctx, s_timer_handlers, (uint32_t)slot, bundle);

    ERBridgeTimer* t = &s_timers[slot];
    t->active = true;
    t->id = s_next_timer_id++;
    t->nargs = nargs;
    /* setInterval clamps to a 1ms floor so due_ms strictly advances (no per-pump busy-fire). */
    t->interval_ms = is_interval ? (uint32_t)(delay < 1 ? 1 : delay) : 0U;
    t->due_ms = er_now_ms() + (uint32_t)delay;
    return JS_NewInt32(ctx, t->id);
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
    for (int i = 0; i < ER_BRIDGE_MAX_TIMERS; i++)
    {
        if (s_timers[i].active && s_timers[i].id == id)
        {
            timer_clear_slot(ctx, i);
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
        const int rc = JS_ExecutePendingJob(rt, &job_ctx);
        if (rc == 0)
        {
            break; /* no more pending jobs */
        }
        if (rc < 0 && job_ctx)
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
    const uint32_t now = er_now_ms();
    for (int slot = 0; slot < ER_BRIDGE_MAX_TIMERS; slot++)
    {
        ERBridgeTimer* t = &s_timers[slot];
        if (!t->active)
        {
            continue;
        }
        /* Wrap-safe "due_ms <= now" comparison on the monotonic engine clock. */
        if ((int32_t)(now - t->due_ms) < 0)
        {
            continue;
        }

        JSValue bundle = JS_GetPropertyUint32(ctx, s_timer_handlers, (uint32_t)slot); /* owned */
        const int nargs = t->nargs;

        if (t->interval_ms > 0U)
        {
            t->due_ms = now + t->interval_ms;
        }
        else
        {
            timer_clear_slot(ctx, slot);
        }

        if (JS_IsObject(bundle))
        {
            JSValue fn = JS_GetPropertyUint32(ctx, bundle, 0);
            JSValue* callv = NULL;
            if (nargs > 0)
            {
                callv = (JSValue*)malloc(sizeof(JSValue) * (size_t)nargs);
                for (int k = 0; k < nargs; k++)
                {
                    callv[k] = JS_GetPropertyUint32(ctx, bundle, (uint32_t)(k + 1));
                }
            }
            JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, nargs, callv);
            if (JS_IsException(ret))
            {
                bridge_report_exception(ctx, "timer callback");
            }
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, fn);
            for (int k = 0; k < nargs; k++)
            {
                JS_FreeValue(ctx, callv[k]);
            }
            free(callv);
        }
        JS_FreeValue(ctx, bundle);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — NativeUI methods
 ---------------------------------------------------------------------------------------------------------------------*/

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
static JSValue js_create_node(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }

    const char* type_str = JS_ToCString(ctx, argv[0]);
    if (!type_str)
    {
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }
    ERNode* node = er_node_create(map_node_type(type_str));
    JS_FreeCString(ctx, type_str);

    if (!node)
    {
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }
    const int32_t handle = handle_alloc(node);
    if (handle == ER_BRIDGE_HANDLE_INVALID)
    {
        er_node_destroy(node);
        return JS_NewInt32(ctx, ER_BRIDGE_HANDLE_INVALID);
    }
    return JS_NewInt32(ctx, handle);
}

/**
 * @brief Finds the bridge handle currently mapped to a node, or ER_BRIDGE_HANDLE_INVALID.
 *
 * Scans only the live high-water range of the (small) handle table. Used to reclaim the handle of a node
 * reached by walking the scene tree — a deleted subtree's descendants never pass through js_destroy_node.
 */
static int32_t handle_of_node(const ERNode* node)
{
    if (!node)
    {
        return ER_BRIDGE_HANDLE_INVALID;
    }
    for (int32_t h = 1; h < s_high_water; h++)
    {
        if (s_node_by_handle[h] == node)
        {
            return h;
        }
    }
    return ER_BRIDGE_HANDLE_INVALID;
}

/** @brief Releases a node's JS event-handler refs and frees its bridge handle slot. */
static void free_node_handle(JSContext* ctx, int32_t h)
{
    if (h == ER_BRIDGE_HANDLE_INVALID)
    {
        return;
    }
    if (s_bridge_ctx)
    {
        for (uint32_t t = 0; t < ER_EVENT_TYPE_COUNT_; t++)
        {
            JS_SetPropertyUint32(ctx, s_event_handlers, (uint32_t)h * ER_EVENT_TYPE_COUNT_ + t, JS_UNDEFINED);
        }
    }
    handle_free(h);
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
    free_node_handle(ctx, handle_of_node(node));
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
static JSValue js_destroy_node(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int32_t h = 0;
    if (JS_ToInt32(ctx, &h, argv[0]) == 0)
    {
        ERNode* node = handle_node(h);
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
static JSValue js_append_child(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
static JSValue js_insert_before(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 3)
    {
        return JS_UNDEFINED;
    }
    ERNode* parent = node_arg(ctx, argv[0]);
    ERNode* child = node_arg(ctx, argv[1]);
    ERNode* before = node_arg(ctx, argv[2]); /* NULL handle → append (handled by the engine). */
    if (parent && child)
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
static JSValue js_remove_child(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
static JSValue js_set_root(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
static JSValue js_set_props(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
static JSValue js_set_text_spans(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

    JSValue len_val = JS_GetPropertyStr(ctx, argv[1], "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);
    if (len <= 0)
    {
        er_node_set_text_spans(node, NULL, 0);
        return JS_UNDEFINED;
    }
    if (len > ER_TEXT_MAX_SPANS)
    {
        len = ER_TEXT_MAX_SPANS;
    }

    ERTextSpan spans[ER_TEXT_MAX_SPANS];
    for (int32_t i = 0; i < len; i++)
    {
        ERTextSpan* sp = &spans[i];
        memset(sp, 0, sizeof(*sp));
        /* Sentinels: every style field inherits from the base node unless overridden below. */
        sp->font_weight = 0xFFU;
        sp->font_style = 0xFFU;
        sp->text_decoration = 0xFFU;
        sp->letter_spacing = ER_LAYOUT_AUTO;

        JSValue seg = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        JSValue v;

        if (prop_get(ctx, seg, "text", &v))
        {
            const char* s = JS_ToCString(ctx, v);
            if (s)
            {
                size_t n = strlen(s);
                if (n > ER_SPAN_TEXT_MAX)
                {
                    n = ER_SPAN_TEXT_MAX;
                }
                memcpy(sp->text, s, n);
                sp->text[n] = '\0';
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, seg, "color", &v))
        {
            uint32_t c = 0;
            if (to_color(ctx, v, &c))
            {
                sp->color = c;
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, seg, "fontSize", &v))
        {
            int32_t fs = 0;
            if (JS_ToInt32(ctx, &fs, v) == 0 && fs > 0)
            {
                sp->font_size = (uint8_t)(fs > 255 ? 255 : fs);
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, seg, "fontWeight", &v))
        {
            if (JS_IsNumber(v))
            {
                int32_t w = 0;
                JS_ToInt32(ctx, &w, v);
                sp->font_weight = w >= 600 ? 1U : 0U;
            }
            else
            {
                const char* s = JS_ToCString(ctx, v);
                if (s)
                {
                    sp->font_weight = (strcmp(s, "bold") == 0 || atoi(s) >= 600) ? 1U : 0U;
                    JS_FreeCString(ctx, s);
                }
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, seg, "fontStyle", &v))
        {
            const char* s = JS_ToCString(ctx, v);
            if (s)
            {
                sp->font_style = map_font_style(s);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, seg, "textDecorationLine", &v))
        {
            const char* s = JS_ToCString(ctx, v);
            if (s)
            {
                sp->text_decoration = map_text_decoration(s);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, seg, "letterSpacing", &v))
        {
            int32_t ls = 0;
            if (JS_ToInt32(ctx, &ls, v) == 0)
            {
                sp->letter_spacing = (int16_t)ls;
            }
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, seg);
    }

    er_node_set_text_spans(node, spans, (uint8_t)len);
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
#define KBD_STR_POOL 1536
static ERKeyboardKey s_kbd_keys[KBD_MAX_KEYS];
static ERKeyboardRow s_kbd_rows[KBD_MAX_ROWS];
static ERKeyboardLayer s_kbd_layers[KBD_MAX_LAYERS];
static ERKeyboardConfig s_kbd_cfg;
static char s_kbd_strs[KBD_STR_POOL];
static int s_kbd_str_used;

/** Length of a JS array (0 when not an array). */
static int arr_len(JSContext* ctx, JSValueConst arr)
{
    if (!JS_IsArray(arr))
        return 0;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    int32_t n = 0;
    JS_ToInt32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n < 0 ? 0 : n;
}

/** Bump-copies a JS string into the persistent pool; returns a stable pointer or NULL (empty / pool full). */
static const char* kbd_intern(JSContext* ctx, JSValueConst v)
{
    const char* s = JS_ToCString(ctx, v);
    if (!s)
        return NULL;
    const size_t n = strlen(s);
    if (n == 0 || (size_t)s_kbd_str_used + n + 1U > KBD_STR_POOL)
    {
        JS_FreeCString(ctx, s);
        return NULL;
    }
    char* dst = &s_kbd_strs[s_kbd_str_used];
    memcpy(dst, s, n + 1);
    s_kbd_str_used += (int)(n + 1);
    JS_FreeCString(ctx, s);
    return dst;
}

/** Interns object[name] (a string) into the pool, or NULL when absent. */
static const char* kbd_intern_prop(JSContext* ctx, JSValueConst obj, const char* name)
{
    JSValue v;
    const char* r = NULL;
    if (prop_get(ctx, obj, name, &v))
    {
        r = kbd_intern(ctx, v);
        JS_FreeValue(ctx, v);
    }
    return r;
}

/** Reads object[name] as an int, returning whether it was present. */
static bool kbd_int(JSContext* ctx, JSValueConst obj, const char* name, int* out)
{
    JSValue v;
    if (prop_get(ctx, obj, name, &v))
    {
        int32_t n = 0;
        const bool ok = JS_ToInt32(ctx, &n, v) == 0;
        JS_FreeValue(ctx, v);
        if (ok)
        {
            *out = n;
            return true;
        }
    }
    return false;
}

/** True when object[name] is a present, truthy boolean. */
static bool kbd_bool(JSContext* ctx, JSValueConst obj, const char* name)
{
    JSValue v;
    bool r = false;
    if (prop_get(ctx, obj, name, &v))
    {
        r = JS_ToBool(ctx, v) > 0;
        JS_FreeValue(ctx, v);
    }
    return r;
}

/** Reads object[name] as a color into *out, returning whether present + valid. */
static bool kbd_color(JSContext* ctx, JSValueConst obj, const char* name, uint32_t* out)
{
    JSValue v;
    bool r = false;
    if (prop_get(ctx, obj, name, &v))
    {
        r = to_color(ctx, v, out);
        JS_FreeValue(ctx, v);
    }
    return r;
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
    JSValueConst cfgv = argv[0];
    ERKeyboardConfig* cfg = &s_kbd_cfg;
    memset(cfg, 0, sizeof(*cfg));
    s_kbd_str_used = 0;
    int nkeys = 0, nrows = 0, nlayers = 0, tmp = 0;

    kbd_color(ctx, cfgv, "panelColor", &cfg->panel_color);
    kbd_color(ctx, cfgv, "keyColor", &cfg->key_color);
    kbd_color(ctx, cfgv, "keyActiveColor", &cfg->key_active_color);
    kbd_color(ctx, cfgv, "labelColor", &cfg->label_color);
    if (kbd_int(ctx, cfgv, "fontSize", &tmp))
        cfg->font_size_px = (uint8_t)tmp;
    if (kbd_int(ctx, cfgv, "rowHeight", &tmp))
        cfg->row_height_px = (uint16_t)tmp;
    if (kbd_int(ctx, cfgv, "keyGap", &tmp))
        cfg->key_gap_px = (uint8_t)tmp;
    if (kbd_int(ctx, cfgv, "keyRadius", &tmp))
        cfg->key_radius_px = (uint8_t)tmp;
    if (kbd_int(ctx, cfgv, "gridCols", &tmp))
        cfg->grid_cols = (uint8_t)tmp;

    JSValue layers = JS_GetPropertyStr(ctx, cfgv, "layers");
    if (JS_IsArray(layers))
    {
        const int nl = arr_len(ctx, layers);
        for (int li = 0; li < nl && nlayers < KBD_MAX_LAYERS; li++)
        {
            JSValue layer = JS_GetPropertyUint32(ctx, layers, (uint32_t)li);
            const int row_start = nrows;
            const int nr = arr_len(ctx, layer);
            for (int ri = 0; ri < nr && nrows < KBD_MAX_ROWS; ri++)
            {
                JSValue row = JS_GetPropertyUint32(ctx, layer, (uint32_t)ri);
                const int key_start = nkeys;
                const int nk = arr_len(ctx, row);
                for (int ki = 0; ki < nk && nkeys < KBD_MAX_KEYS; ki++)
                {
                    JSValue key = JS_GetPropertyUint32(ctx, row, (uint32_t)ki);
                    ERKeyboardKey* k = &s_kbd_keys[nkeys++];
                    memset(k, 0, sizeof(*k));
                    k->span = 1;
                    k->highlight_layer = 0xFFU;
                    const char* jlabel = kbd_intern_prop(ctx, key, "label");
                    int layer_target = 0;
                    if (kbd_bool(ctx, key, "backspace"))
                    {
                        k->type = ER_KBD_KEY_BACKSPACE;
                        k->label = jlabel ? jlabel : "<";
                    }
                    else if (kbd_bool(ctx, key, "done"))
                    {
                        k->type = ER_KBD_KEY_DONE;
                        k->label = jlabel ? jlabel : "OK";
                    }
                    else if (kbd_int(ctx, key, "layer", &layer_target))
                    {
                        k->type = ER_KBD_KEY_LAYER;
                        k->layer = (uint8_t)layer_target;
                        k->label = jlabel;
                    }
                    else
                    {
                        const char* ch = kbd_intern_prop(ctx, key, "char");
                        k->type = ER_KBD_KEY_CHAR;
                        k->text = ch;
                        k->label = jlabel ? jlabel : ((ch && ch[0] == ' ') ? NULL : ch); /* blank space bar */
                    }
                    if (kbd_int(ctx, key, "span", &tmp))
                        k->span = (uint8_t)(tmp > 0 ? tmp : 1);
                    if (kbd_bool(ctx, key, "highlight"))
                        k->highlight_layer = (uint8_t)li;
                    JS_FreeValue(ctx, key);
                }
                ERKeyboardRow* r = &s_kbd_rows[nrows++];
                r->keys = &s_kbd_keys[key_start];
                r->count = (uint8_t)(nkeys - key_start);
                JS_FreeValue(ctx, row);
            }
            ERKeyboardLayer* L = &s_kbd_layers[nlayers++];
            L->rows = &s_kbd_rows[row_start];
            L->count = (uint8_t)(nrows - row_start);
            JS_FreeValue(ctx, layer);
        }
        cfg->layers = s_kbd_layers;
        cfg->layer_count = (uint8_t)nlayers;
    }
    JS_FreeValue(ctx, layers);

    er_keyboard_set_config(cfg);
    return JS_UNDEFINED;
}

/* Bridge-side caps; the engine clamps again to its own storage limits. Overridable (e.g. for large
 * imported SVGs) — keep these >= the engine's ERUI_VECTOR_TAPE_MAX / ERUI_VECTOR_PAINTS_MAX, or the bridge
 * truncates the op-tape before the engine ever sees it. */
#ifndef VEC_BRIDGE_MAX_OPS
#define VEC_BRIDGE_MAX_OPS 1024
#endif
#ifndef VEC_BRIDGE_MAX_PAINTS
#define VEC_BRIDGE_MAX_PAINTS 16
#endif
#ifndef VEC_BRIDGE_MAX_GRADS
#define VEC_BRIDGE_MAX_GRADS 16
#endif
/* JS paint record width (kept in sync with svg-ops.js PAINT_STRIDE): the 7 base fields + fill_grad + stroke_grad. */
#define VEC_PAINT_STRIDE 9
/* JS gradient record width (kept in sync with svg-ops.js encodeVectorGradients):
 * [type, stop_count, (color, position) × ER_VGRAD_MAX_STOPS, ax, ay, bx, by, r]. */
#define VEC_GRAD_STRIDE (2 + ER_VGRAD_MAX_STOPS * 2 + 5)

/**
 * @brief NativeUI.setVectorOps(handle, ops, paints, gradients, dirtyRect) — sets the path geometry on an Svg.
 *
 * @p ops is a flat number array (the op-tape; see the ER_VOP_* contract). @p paints is a flat number array
 * of VEC_PAINT_STRIDE (9) fields per shape: [fill, stroke, strokeWidth, miterLimit, cap, join, fillRule,
 * fillGrad, strokeGrad] where fill/stroke are uint32 ARGB8888 (JS numbers hold them exactly) and fillGrad /
 * strokeGrad are 1-based indices into @p gradients (0 = solid). @p gradients is an optional flat array of
 * VEC_GRAD_STRIDE fields per gradient. A missing/empty ops array clears the node's geometry.
 *
 * @return JS_UNDEFINED.
 */
static JSValue js_set_vector_ops(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
        er_node_set_vector_ops(node, NULL, 0, NULL, 0, NULL, 0);
        return JS_UNDEFINED;
    }

    /* Op-tape. Static (the bridge is single-threaded) to keep it off the stack. */
    static float ops[VEC_BRIDGE_MAX_OPS];
    JSValue olv = JS_GetPropertyStr(ctx, argv[1], "length");
    int32_t olen = 0;
    JS_ToInt32(ctx, &olen, olv);
    JS_FreeValue(ctx, olv);
    if (olen <= 0)
    {
        er_node_set_vector_ops(node, NULL, 0, NULL, 0, NULL, 0);
        return JS_UNDEFINED;
    }
    if (olen > VEC_BRIDGE_MAX_OPS)
    {
        olen = VEC_BRIDGE_MAX_OPS;
    }
    for (int32_t i = 0; i < olen; i++)
    {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        double d = 0.0;
        JS_ToFloat64(ctx, &d, e);
        ops[i] = (float)d;
        JS_FreeValue(ctx, e);
    }

    /* Paint table: 7 numbers per entry. */
    ERVectorPaint paints[VEC_BRIDGE_MAX_PAINTS];
    int np = 0;
    if (argc >= 3 && JS_IsArray(argv[2]))
    {
        JSValue plv = JS_GetPropertyStr(ctx, argv[2], "length");
        int32_t plen = 0;
        JS_ToInt32(ctx, &plen, plv);
        JS_FreeValue(ctx, plv);
        np = plen / VEC_PAINT_STRIDE;
        if (np > VEC_BRIDGE_MAX_PAINTS)
        {
            np = VEC_BRIDGE_MAX_PAINTS;
        }
        for (int i = 0; i < np; i++)
        {
            double f[VEC_PAINT_STRIDE];
            for (int j = 0; j < VEC_PAINT_STRIDE; j++)
            {
                JSValue e = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)(i * VEC_PAINT_STRIDE + j));
                double d = 0.0;
                JS_ToFloat64(ctx, &d, e);
                f[j] = d;
                JS_FreeValue(ctx, e);
            }
            /* Colors fit a double exactly, so (uint32_t)double is bit-accurate ARGB8888. */
            paints[i].fill = (uint32_t)f[0];
            paints[i].stroke = (uint32_t)f[1];
            paints[i].stroke_w = (float)f[2];
            paints[i].miter = (float)f[3];
            paints[i].cap = (uint8_t)f[4];
            paints[i].join = (uint8_t)f[5];
            paints[i].fill_rule = (uint8_t)f[6];
            paints[i].fill_grad = (int16_t)f[7];   /* 1-based fill gradient index (0 = solid) */
            paints[i].stroke_grad = (int16_t)f[8]; /* 1-based stroke gradient index (0 = solid) */
        }
    }

    /* Gradient table (optional 4th arg): VEC_GRAD_STRIDE numbers per gradient. */
    ERVectorGradient grads[VEC_BRIDGE_MAX_GRADS];
    int ngr = 0;
    if (argc >= 4 && JS_IsArray(argv[3]))
    {
        JSValue glv = JS_GetPropertyStr(ctx, argv[3], "length");
        int32_t glen = 0;
        JS_ToInt32(ctx, &glen, glv);
        JS_FreeValue(ctx, glv);
        ngr = glen / VEC_GRAD_STRIDE;
        if (ngr > VEC_BRIDGE_MAX_GRADS)
        {
            ngr = VEC_BRIDGE_MAX_GRADS;
        }
        for (int i = 0; i < ngr; i++)
        {
            double g[VEC_GRAD_STRIDE];
            for (int j = 0; j < VEC_GRAD_STRIDE; j++)
            {
                JSValue e = JS_GetPropertyUint32(ctx, argv[3], (uint32_t)(i * VEC_GRAD_STRIDE + j));
                double d = 0.0;
                JS_ToFloat64(ctx, &d, e);
                g[j] = d;
                JS_FreeValue(ctx, e);
            }
            grads[i].type = (uint8_t)g[0];
            grads[i].stop_count = (uint8_t)g[1];
            for (int s = 0; s < ER_VGRAD_MAX_STOPS; s++)
            {
                grads[i].stops[s].color = (uint32_t)g[2 + s * 2];
                grads[i].stops[s].position = (float)g[3 + s * 2];
            }
            const int geo = 2 + ER_VGRAD_MAX_STOPS * 2;
            grads[i].ax = (float)g[geo + 0];
            grads[i].ay = (float)g[geo + 1];
            grads[i].bx = (float)g[geo + 2];
            grads[i].by = (float)g[geo + 3];
            grads[i].r = (float)g[geo + 4];
        }
    }

    er_node_set_vector_ops(node, ops, olen, paints, np, grads, ngr);

    /* Optional 5th arg: a [x, y, w, h] node-local sub-rect to restrict this commit's damage to. */
    if (argc >= 5 && JS_IsArray(argv[4]))
    {
        JSValue dlv = JS_GetPropertyStr(ctx, argv[4], "length");
        int32_t dlen = 0;
        JS_ToInt32(ctx, &dlen, dlv);
        JS_FreeValue(ctx, dlv);
        if (dlen >= 4)
        {
            double d[4];
            for (int k = 0; k < 4; k++)
            {
                JSValue e = JS_GetPropertyUint32(ctx, argv[4], (uint32_t)k);
                d[k] = 0.0;
                JS_ToFloat64(ctx, &d[k], e);
                JS_FreeValue(ctx, e);
            }
            er_node_set_vector_dirty_rect(node, (int)d[0], (int)d[1], (int)d[2], (int)d[3]);
        }
    }
    return JS_UNDEFINED;
}

/**
 * @brief NativeUI.commit() — runs layout + paint for all pending mutations.
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
    er_commit();
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
    return JS_NewInt64(ctx, (int64_t)er_now_ms());
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
static JSValue js_set_event(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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
    ERNode* node = handle_node(handle);
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
    const bool ok = event_type_from_name(name, &type);
    JS_FreeCString(ctx, name);
    if (!ok)
    {
        return JS_UNDEFINED;
    }

    const uint32_t key = (uint32_t)handle * ER_EVENT_TYPE_COUNT_ + (uint32_t)type;
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
    return JS_UNDEFINED;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Animated (er_anim_value_*)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads a numeric property from an object into a float.
 *
 * @param[in]  ctx  QuickJS context.
 * @param[in]  obj  Object to read from.
 * @param[in]  key  Property name.
 * @param[out] out  Receives the value when present and numeric.
 *
 * @return true when the property was present and read.
 */
static bool obj_f32(JSContext* ctx, JSValueConst obj, const char* key, float* out)
{
    JSValue v;
    if (!prop_get(ctx, obj, key, &v))
    {
        return false;
    }
    double d = 0.0;
    const bool ok = JS_ToFloat64(ctx, &d, v) == 0;
    if (ok)
    {
        *out = (float)d;
    }
    JS_FreeValue(ctx, v);
    return ok;
}

/**
 * @brief Maps an animatable prop name (opacity, translateX, rotate, …) to an ERAnimProp.
 *
 * @param[in]  s    Prop name.
 * @param[out] out  Receives the ERAnimProp when recognised.
 *
 * @return true when the name maps to a supported animatable prop.
 */
static bool anim_prop_from_name(const char* s, ERAnimProp* out)
{
    static const struct
    {
        const char* name;
        ERAnimProp prop;
    } k[] = {
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
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
    {
        if (strcmp(s, k[i].name) == 0)
        {
            *out = k[i].prop;
            return true;
        }
    }
    return false;
}

/** @brief Maps an easing token to ERAnimEasing. @param[in] s String. @return Enum value (default ease). */
static ERAnimEasing easing_from_name(const char* s)
{
    if (strcmp(s, "linear") == 0)
    {
        return ER_EASE_LINEAR;
    }
    if (strcmp(s, "easeIn") == 0)
    {
        return ER_EASE_EASE_IN;
    }
    if (strcmp(s, "easeOut") == 0)
    {
        return ER_EASE_EASE_OUT;
    }
    if (strcmp(s, "easeInOut") == 0)
    {
        return ER_EASE_EASE_IN_OUT;
    }
    if (strcmp(s, "quadIn") == 0)
    {
        return ER_EASE_QUAD_IN;
    }
    if (strcmp(s, "quadOut") == 0)
    {
        return ER_EASE_QUAD_OUT;
    }
    if (strcmp(s, "quadInOut") == 0)
    {
        return ER_EASE_QUAD_IN_OUT;
    }
    if (strcmp(s, "cubicIn") == 0)
    {
        return ER_EASE_CUBIC_IN;
    }
    if (strcmp(s, "cubicOut") == 0)
    {
        return ER_EASE_CUBIC_OUT;
    }
    if (strcmp(s, "cubicInOut") == 0)
    {
        return ER_EASE_CUBIC_IN_OUT;
    }
    if (strcmp(s, "bounceOut") == 0)
    {
        return ER_EASE_BOUNCE_OUT;
    }
    if (strcmp(s, "elasticOut") == 0)
    {
        return ER_EASE_ELASTIC_OUT;
    }
    return ER_EASE_EASE;
}

/**
 * @brief Marshals a JS animation config object into an ERAnimConfig.
 *
 * Recognised keys: type ("timing"|"spring"|"decay"), duration, delay, loop, easing (string token or
 * a {x1,y1,x2,y2} bezier object), spring stiffness/damping/mass/velocity, decay deceleration/velocity.
 *
 * @param[in]  ctx  QuickJS context.
 * @param[in]  obj  Config object (may be undefined).
 * @param[out] cfg  Zero-initialised then populated.
 */
static void marshal_anim_config(JSContext* ctx, JSValueConst obj, ERAnimConfig* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->type = ER_ANIM_TIMING;
    cfg->easing = ER_EASE_EASE;
    cfg->duration_ms = 300U;

    if (!JS_IsObject(obj))
    {
        return;
    }

    JSValue v;
    if (prop_get(ctx, obj, "type", &v))
    {
        const char* s = JS_ToCString(ctx, v);
        if (s)
        {
            if (strcmp(s, "spring") == 0)
            {
                cfg->type = ER_ANIM_SPRING;
            }
            else if (strcmp(s, "decay") == 0)
            {
                cfg->type = ER_ANIM_DECAY;
            }
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, v);
    }
    if (prop_get(ctx, obj, "duration", &v))
    {
        int32_t n = 0;
        if (JS_ToInt32(ctx, &n, v) == 0 && n >= 0)
        {
            cfg->duration_ms = (uint32_t)n;
        }
        JS_FreeValue(ctx, v);
    }
    if (prop_get(ctx, obj, "delay", &v))
    {
        int32_t n = 0;
        if (JS_ToInt32(ctx, &n, v) == 0 && n >= 0)
        {
            cfg->delay_ms = (uint32_t)n;
        }
        JS_FreeValue(ctx, v);
    }
    if (prop_get(ctx, obj, "easing", &v))
    {
        if (JS_IsString(v))
        {
            const char* s = JS_ToCString(ctx, v);
            if (s)
            {
                cfg->easing = easing_from_name(s);
                JS_FreeCString(ctx, s);
            }
        }
        else if (JS_IsObject(v))
        {
            cfg->easing = ER_EASE_BEZIER;
            obj_f32(ctx, v, "x1", &cfg->bezier_x1);
            obj_f32(ctx, v, "y1", &cfg->bezier_y1);
            obj_f32(ctx, v, "x2", &cfg->bezier_x2);
            obj_f32(ctx, v, "y2", &cfg->bezier_y2);
        }
        JS_FreeValue(ctx, v);
    }

    obj_f32(ctx, obj, "stiffness", &cfg->stiffness);
    obj_f32(ctx, obj, "damping", &cfg->damping);
    obj_f32(ctx, obj, "mass", &cfg->mass);
    obj_f32(ctx, obj, "velocity", &cfg->velocity);
    obj_f32(ctx, obj, "deceleration", &cfg->deceleration);

    if (prop_get(ctx, obj, "loop", &v))
    {
        cfg->loop = JS_ToBool(ctx, v) != 0;
        JS_FreeValue(ctx, v);
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

    ERLayoutAnimConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type = ER_ANIM_TIMING;
    cfg.easing = ER_EASE_EASE_IN_OUT;
    cfg.duration_ms = 300U;

    JSValueConst obj = (argc > 0) ? argv[0] : JS_UNDEFINED;
    if (JS_IsObject(obj))
    {
        JSValue v;
        if (prop_get(ctx, obj, "type", &v))
        {
            const char* s = JS_ToCString(ctx, v);
            if (s)
            {
                if (strcmp(s, "spring") == 0)
                {
                    cfg.type = ER_ANIM_SPRING;
                }
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, obj, "duration", &v))
        {
            int32_t n = 0;
            if (JS_ToInt32(ctx, &n, v) == 0 && n > 0)
            {
                cfg.duration_ms = (uint16_t)(n > 65535 ? 65535 : n);
            }
            JS_FreeValue(ctx, v);
        }
        if (prop_get(ctx, obj, "easing", &v))
        {
            if (JS_IsString(v))
            {
                const char* s = JS_ToCString(ctx, v);
                if (s)
                {
                    cfg.easing = easing_from_name(s);
                    JS_FreeCString(ctx, s);
                }
            }
            else if (JS_IsObject(v))
            {
                cfg.easing = ER_EASE_BEZIER;
                obj_f32(ctx, v, "x1", &cfg.bezier_x1);
                obj_f32(ctx, v, "y1", &cfg.bezier_y1);
                obj_f32(ctx, v, "x2", &cfg.bezier_x2);
                obj_f32(ctx, v, "y2", &cfg.bezier_y2);
            }
            JS_FreeValue(ctx, v);
        }
        obj_f32(ctx, obj, "stiffness", &cfg.stiffness);
        obj_f32(ctx, obj, "damping", &cfg.damping);
        obj_f32(ctx, obj, "mass", &cfg.mass);
    }

    er_layout_anim_configure_next(&cfg);
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
    return JS_NewBool(ctx, er_layout_anim_has_pending());
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
    return JS_NewInt32(ctx, (int32_t)er_anim_value_create((float)initial));
}

/** @brief NativeUI.animValueDestroy(handle). @return JS_UNDEFINED. */
static JSValue js_anim_value_destroy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int32_t h = 0;
    if (JS_ToInt32(ctx, &h, argv[0]) == 0)
    {
        er_anim_value_destroy((ERAnimValueHandle)h);
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
    int32_t h = 0;
    double value = 0.0;
    if (JS_ToInt32(ctx, &h, argv[0]) == 0 && JS_ToFloat64(ctx, &value, argv[1]) == 0)
    {
        er_anim_value_set((ERAnimValueHandle)h, (float)value);
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
    int32_t h = 0;
    if (JS_ToInt32(ctx, &h, argv[0]) != 0)
    {
        return JS_NewFloat64(ctx, 0.0);
    }
    return JS_NewFloat64(ctx, (double)er_anim_value_get((ERAnimValueHandle)h));
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
    int32_t vh = 0;
    if (JS_ToInt32(ctx, &vh, argv[0]) != 0)
    {
        return JS_UNDEFINED;
    }
    ERNode* node = node_arg(ctx, argv[1]);
    const char* name = JS_ToCString(ctx, argv[2]);
    if (node && name)
    {
        ERAnimProp prop;
        if (anim_prop_from_name(name, &prop))
        {
            er_anim_value_bind((ERAnimValueHandle)vh, node, prop);
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
        ERAnimProp prop;
        if (anim_prop_from_name(name, &prop))
        {
            er_anim_unbind_prop(node, prop);
        }
    }
    if (name)
    {
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}

/** @brief Maps an extrapolate token to ERExtrapolate. @param[in] s String. @return Enum (default extend). */
static ERExtrapolate extrapolate_from_name(const char* s)
{
    if (strcmp(s, "clamp") == 0)
    {
        return ER_EXTRAPOLATE_CLAMP;
    }
    if (strcmp(s, "identity") == 0)
    {
        return ER_EXTRAPOLATE_IDENTITY;
    }
    return ER_EXTRAPOLATE_EXTEND;
}

/**
 * @brief Reads an extrapolate token from obj[key] into *out (left unchanged if absent/invalid).
 *
 * @param[in]     ctx  QuickJS context.
 * @param[in]     obj  Config object.
 * @param[in]     key  Property name (e.g. "extrapolate").
 * @param[in,out] out  Receives the mapped ERExtrapolate when the key is a string.
 */
static void read_extrapolate(JSContext* ctx, JSValueConst obj, const char* key, ERExtrapolate* out)
{
    JSValue v;
    if (!prop_get(ctx, obj, key, &v))
    {
        return;
    }
    const char* s = JS_ToCString(ctx, v);
    if (s)
    {
        *out = extrapolate_from_name(s);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
}

/**
 * @brief NativeUI.animValueBindInterpolated(valueHandle, nodeHandle, propName, interp) —
 *        binds a value to a node prop through a piecewise-linear interpolation.
 *
 * interp = { inputRange:[…], outputRange:[…], extrapolate?, extrapolateLeft?, extrapolateRight? }.
 * `extrapolate` sets both ends; the per-end keys override it. Tokens: "extend" (default), "clamp",
 * "identity".
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
    int32_t vh = 0;
    if (JS_ToInt32(ctx, &vh, argv[0]) != 0)
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

    ERAnimProp prop;
    const bool prop_ok = anim_prop_from_name(name, &prop);
    JS_FreeCString(ctx, name);
    if (!prop_ok)
    {
        return JS_UNDEFINED;
    }

    ERInterpolation interp;
    memset(&interp, 0, sizeof(interp));
    interp.extrapolate_left = ER_EXTRAPOLATE_EXTEND;
    interp.extrapolate_right = ER_EXTRAPOLATE_EXTEND;

    /* `extrapolate` applies to both ends; the per-end keys override it (RN semantics). */
    read_extrapolate(ctx, argv[3], "extrapolate", &interp.extrapolate_left);
    interp.extrapolate_right = interp.extrapolate_left;
    read_extrapolate(ctx, argv[3], "extrapolateLeft", &interp.extrapolate_left);
    read_extrapolate(ctx, argv[3], "extrapolateRight", &interp.extrapolate_right);

    /* Read inputRange / outputRange arrays (clamped to ER_INTERPOLATE_MAX_POINTS). */
    JSValue in = JS_GetPropertyStr(ctx, argv[3], "inputRange");
    JSValue out = JS_GetPropertyStr(ctx, argv[3], "outputRange");
    if (JS_IsArray(in) && JS_IsArray(out))
    {
        JSValue len_val = JS_GetPropertyStr(ctx, in, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        if (len > ER_INTERPOLATE_MAX_POINTS)
        {
            len = ER_INTERPOLATE_MAX_POINTS;
        }
        for (int32_t i = 0; i < len; i++)
        {
            JSValue iv = JS_GetPropertyUint32(ctx, in, (uint32_t)i);
            JSValue ov = JS_GetPropertyUint32(ctx, out, (uint32_t)i);
            double di = 0.0;
            double dovv = 0.0;
            JS_ToFloat64(ctx, &di, iv);
            JS_ToFloat64(ctx, &dovv, ov);
            interp.input_range[i] = (float)di;
            interp.output_range[i] = (float)dovv;
            JS_FreeValue(ctx, iv);
            JS_FreeValue(ctx, ov);
        }
        interp.point_count = (uint8_t)len;
    }
    JS_FreeValue(ctx, in);
    JS_FreeValue(ctx, out);

    if (interp.point_count >= 2)
    {
        er_anim_value_bind_interpolated((ERAnimValueHandle)vh, node, prop, &interp);
    }
    return JS_UNDEFINED;
}

/**
 * @brief Engine completion callback for a value animation: dispatches to the registered JS handler.
 *
 * user_data is the completion slot index. The handler is one-shot: it is captured (owned) and the
 * slot freed before the JS call, so a handler that chains the next animation can safely reuse this
 * slot. Runs inside er_anim_tick (the engine deactivates the finished animation before calling
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

    JSValue cb = JS_GetPropertyUint32(ctx, s_anim_complete_handlers, (uint32_t)slot); /* owned */
    s_anim_complete_active[slot] = false;
    JS_SetPropertyUint32(ctx, s_anim_complete_handlers, (uint32_t)slot, JS_UNDEFINED);

    if (JS_IsFunction(ctx, cb))
    {
        JSValue arg = JS_NewBool(ctx, finished);
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 1, &arg);
        if (JS_IsException(ret))
        {
            bridge_report_exception(ctx, "animation completion callback");
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, arg);
    }
    JS_FreeValue(ctx, cb);
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
    int32_t vh = 0;
    double to_value = 0.0;
    if (JS_ToInt32(ctx, &vh, argv[0]) != 0 || JS_ToFloat64(ctx, &to_value, argv[1]) != 0)
    {
        return JS_NewInt32(ctx, 0);
    }
    ERAnimConfig cfg;
    marshal_anim_config(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &cfg);

    /* Optional completion callback (argv[3]): grab a slot, root the callback, and route the
       engine's on_complete through the trampoline. Must be registered BEFORE er_anim_value_animate,
       which may synchronously cancel a superseded animation (firing that one's callback). */
    if (argc > 3 && JS_IsFunction(ctx, argv[3]))
    {
        int slot = -1;
        for (int i = 0; i < ER_BRIDGE_MAX_ANIM_COMPLETIONS; i++)
        {
            if (!s_anim_complete_active[i])
            {
                slot = i;
                break;
            }
        }
        if (slot >= 0)
        {
            s_anim_complete_active[slot] = true;
            JS_SetPropertyUint32(ctx, s_anim_complete_handlers, (uint32_t)slot, JS_DupValue(ctx, argv[3]));
            cfg.on_complete = bridge_anim_complete_trampoline;
            cfg.on_complete_user_data = (void*)(uintptr_t)slot;
        }
        else
        {
            fprintf(stderr, "animValueAnimate: completion pool full (%d)\n", ER_BRIDGE_MAX_ANIM_COMPLETIONS);
        }
    }

    const ERAnimHandle ah = er_anim_value_animate((ERAnimValueHandle)vh, (float)to_value, &cfg);
    return JS_NewInt32(ctx, (int32_t)ah);
}

/** @brief NativeUI.animStop(animHandle) — stops a running animation. @return JS_UNDEFINED. */
static JSValue js_anim_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int32_t ah = 0;
    if (JS_ToInt32(ctx, &ah, argv[0]) == 0)
    {
        er_anim_stop((ERAnimHandle)ah);
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
 * @param[in] ctx  Context the bridge was installed into (NULL is a no-op).
 */
void er_bridge_pump(JSContext* ctx)
{
    if (!ctx)
    {
        return;
    }
    embedded_renderer_flush_touch();
    bridge_run_microtasks(ctx);
    bridge_fire_due_timers(ctx);
    bridge_run_microtasks(ctx);
}

/**
 * @brief Loads and runs a precompiled QuickJS bytecode blob (see header).
 *
 * @param[in] ctx  QuickJS context.
 * @param[in] buf  Bytecode bytes.
 * @param[in] len  Byte count.
 *
 * @return Evaluation result or a JS exception value (caller frees).
 */
JSValue er_bridge_run_bytecode(JSContext* ctx, const uint8_t* buf, size_t len)
{
    JSValue obj = JS_ReadObject(ctx, buf, len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj))
    {
        return obj;
    }
    /* JS_EvalFunction consumes obj and returns the program result (or an exception). */
    return JS_EvalFunction(ctx, obj);
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

    /* Start every context with an empty timer pool (statics persist across re-install in tests). */
    for (int i = 0; i < ER_BRIDGE_MAX_TIMERS; i++)
    {
        s_timers[i].active = false;
    }
    s_next_timer_id = 1;
    for (int i = 0; i < ER_BRIDGE_MAX_ANIM_COMPLETIONS; i++)
    {
        s_anim_complete_active[i] = false;
    }

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

    /* Animated (er_anim_value_* — native-driver animation). */
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

    /* Timer-callback registry: same ownership trick as the event handlers, so each timer's
       [callback, ...args] array is GC-rooted and reclaimed with the context. */
    JSValue timer_handlers = JS_NewObject(ctx);
    s_timer_handlers = timer_handlers;
    JS_SetPropertyStr(ctx, native_ui, "__er_timer_handlers", timer_handlers);

    /* Animation completion-callback registry (Animated `.start(cb)`); same ownership trick. */
    JSValue anim_complete_handlers = JS_NewObject(ctx);
    s_anim_complete_handlers = anim_complete_handlers;
    JS_SetPropertyStr(ctx, native_ui, "__er_anim_complete_handlers", anim_complete_handlers);

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
