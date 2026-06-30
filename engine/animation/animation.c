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

#include "er_node_internal.h"
#include "renderer_internal.h"
#include <math.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#ifndef ERUI_MAX_ANIMATIONS
#define ERUI_MAX_ANIMATIONS 64
#endif

#ifndef ERUI_MAX_ANIM_GROUPS
#define ERUI_MAX_ANIM_GROUPS 8
#endif

#ifndef ERUI_MAX_GROUP_ENTRIES
#define ERUI_MAX_GROUP_ENTRIES 8
#endif

#ifndef ERUI_MAX_ANIM_VALUES
#define ERUI_MAX_ANIM_VALUES 16
#endif

#ifndef ERUI_MAX_VALUE_BINDINGS
#define ERUI_MAX_VALUE_BINDINGS 4
#endif

/** @brief Spring state considered settled when both |disp| and |vel| are below these thresholds. */
#define SPRING_SETTLE_DISP 0.001f
#define SPRING_SETTLE_VEL 0.01f

/** @brief Decay stops when the velocity magnitude drops below this value (value/ms). */
#define DECAY_VEL_STOP 0.0001f

/** @brief Default spring parameters matching React Native Animated.spring() defaults. */
#define SPRING_DEFAULT_STIFFNESS 100.0f
#define SPRING_DEFAULT_DAMPING 10.0f
#define SPRING_DEFAULT_MASS 1.0f

/** @brief Default decay deceleration per millisecond (≈ RN 0.998 per 60fps frame). */
#define DECAY_DEFAULT_DECELERATION 0.998f

/** @brief Group type values stored in ERAnimGroup::type. */
#define GROUP_TYPE_SEQUENCE 0
#define GROUP_TYPE_PARALLEL 1
#define GROUP_TYPE_STAGGER 2

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Active animation slot.
 */
typedef struct
{
    bool active;
    bool in_delay; /**< true while waiting out the delay_ms countdown. */
    uint16_t node_tag;
    uint16_t handle; /**< Unique handle; 0 = not yet assigned. */
    ERAnimProp prop;
    ERAnimType type;
    ERAnimEasing easing;
    float bezier_x1;
    float bezier_y1;
    float bezier_x2;
    float bezier_y2;
    uint32_t delay_ms;
    uint32_t delay_elapsed_ms;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    bool loop;
    bool loop_reverse; /**< ping-pong (swap endpoints) each loop; else restart (continuous). */
    bool color_value;
    float from_value;
    float to_value;
    uint32_t from_color;
    uint32_t to_color;
    /* Spring-specific state */
    float spring_stiffness;
    float spring_damping;
    float spring_mass;
    float spring_pos; /**< Current normalized position [0 = from, 1 = to, may overshoot]. */
    float spring_vel; /**< Current normalized velocity (units of progress per second). */
    /* Decay-specific state */
    float decay_velocity;     /**< Current velocity in (to_value - from_value)/ms units. */
    float decay_pos;          /**< Accumulated decay offset (used to derive from_value). */
    float decay_deceleration; /**< Per-ms friction factor. */
    /* Completion */
    ERAnimCompleteFn on_complete;
    void* on_complete_user_data;
    /* Group back-reference */
    uint8_t group_slot; /**< Index into s_groups; UINT8_MAX = standalone animation. */
    uint8_t group_entry;
    /* Value binding */
    uint16_t value_handle; /**< Non-zero when this animation targets an ERAnimValue; node_tag unused. */
} ERAnimation;

/**
 * @brief Entry within an animation group (copy of ERAnimEntry).
 */
typedef struct
{
    uint16_t node_tag;
    ERAnimProp prop;
    float value;
    ERAnimConfig cfg;
} ERGroupEntry;

/**
 * @brief Active animation group (sequence, parallel, or stagger).
 */
typedef struct
{
    bool active;
    uint8_t type; /**< GROUP_TYPE_SEQUENCE / PARALLEL / STAGGER. */
    uint16_t handle;
    uint16_t count;
    uint16_t current; /**< Sequence: index of the currently running entry. */
    uint16_t done;    /**< Parallel/stagger: number of entries that have completed. */
    uint32_t stagger_ms;
    ERGroupEntry entries[ERUI_MAX_GROUP_ENTRIES];
    ERAnimCompleteFn on_complete;
    void* on_complete_user_data;
} ERAnimGroup;

/**
 * @brief One node-property pair bound to an ERAnimValue.
 */
typedef struct
{
    uint16_t node_tag;
    ERAnimProp prop;
    bool active;
    bool interpolated;      /**< When true, value is mapped through interp before being applied. */
    ERInterpolation interp; /**< Mapping applied when interpolated is true. */
} ERValueBinding;

/**
 * @brief Standalone animatable float with optional node-property bindings.
 *
 * Models the React Native Animated.Value concept: one float can be animated and its
 * current value pushed to multiple node properties without re-entering any higher-level
 * layer per frame (useNativeDriver = true equivalent).
 */
typedef struct
{
    bool in_use;
    uint16_t handle;
    float current;
    ERValueBinding bindings[ERUI_MAX_VALUE_BINDINGS];
    uint8_t binding_count;
} ERAnimValue;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERAnimation s_animations[ERUI_MAX_ANIMATIONS];
static ERAnimGroup s_groups[ERUI_MAX_ANIM_GROUPS];
static ERAnimValue s_anim_values[ERUI_MAX_ANIM_VALUES];
static uint16_t s_next_handle = 1; /**< Monotonically increasing handle counter; wraps avoiding 0. */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public — reset
 ---------------------------------------------------------------------------------------------------------------------*/

void er_anim_reset(void)
{
    /* All three pools are plain value arrays whose "free" state is all-zero (active/in_use = false),
     * with no heap to release — a zero-fill is a complete reset. */
    memset(s_animations, 0, sizeof(s_animations));
    memset(s_groups, 0, sizeof(s_groups));
    memset(s_anim_values, 0, sizeof(s_anim_values));
    s_next_handle = 1;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — handle allocation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Allocates a unique animation handle.
 *
 * @return Non-zero handle.
 */
static uint16_t alloc_handle(void)
{
    uint16_t h = s_next_handle++;
    if (s_next_handle == 0)
        s_next_handle = 1;
    return h;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — node inspection helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Converts a float carrying raw uint32_t bits back into an integer.
 *
 * @param[in] value  Float whose bytes contain a uint32_t.
 *
 * @return Recovered uint32_t value.
 */
static uint32_t bits_from_float(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/**
 * @brief Returns true when an animation property is packed ARGB8888 color.
 *
 * @param[in] prop  Animation property.
 *
 * @return true for color properties.
 */
static bool is_color_prop(ERAnimProp prop)
{
    return prop == ER_PROP_BACKGROUND_COLOR || prop == ER_PROP_COLOR;
}

/**
 * @brief Returns true when a node has view visual properties.
 *
 * @param[in] node  Node to inspect.
 *
 * @return true if the node stores ERViewProps.
 */
static bool is_view_node(const ERNode* node)
{
    return node->type == ER_NODE_VIEW || node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_PRESSABLE
           || node->type == ER_NODE_MODAL;
}

/**
 * @brief Reads the current numeric value for an animatable property.
 *
 * @param[in]  node   Node to inspect.
 * @param[in]  prop   Property to read.
 * @param[out] value  Output numeric value.
 *
 * @return true if the property is supported on the node.
 */
static bool read_numeric_value(const ERNode* node, ERAnimProp prop, float* value)
{
    if (!value)
        return false;

    switch (prop)
    {
        case ER_PROP_OPACITY:
            if (!is_view_node(node))
                return false;
            *value = (float)node->props.view.opacity / 255.0f;
            return true;
        case ER_PROP_TRANSLATE_X:
            *value = node->tp_translate_x;
            return true;
        case ER_PROP_TRANSLATE_Y:
            *value = node->tp_translate_y;
            return true;
        case ER_PROP_SCALE_X:
            *value = node->tp_scale_x;
            return true;
        case ER_PROP_SCALE_Y:
            *value = node->tp_scale_y;
            return true;
        case ER_PROP_ROTATE_Z:
            *value = node->tp_rotate_z;
            return true;
        case ER_PROP_ROTATE_X:
            *value = node->tp_rotate_x;
            return true;
        case ER_PROP_ROTATE_Y:
            *value = node->tp_rotate_y;
            return true;
        case ER_PROP_SWITCH_THUMB:
            if (node->type != ER_NODE_SWITCH)
                return false;
            *value = node->switch_thumb_t;
            return true;
        default:
            return false;
    }
}

/**
 * @brief Reads the current color value for an animatable property.
 *
 * @param[in]  node   Node to inspect.
 * @param[in]  prop   Property to read.
 * @param[out] color  Output ARGB8888 color.
 *
 * @return true if the property is supported on the node.
 */
static bool read_color_value(const ERNode* node, ERAnimProp prop, uint32_t* color)
{
    if (!color)
        return false;

    switch (prop)
    {
        case ER_PROP_BACKGROUND_COLOR:
            if (!is_view_node(node))
                return false;
            *color = node->props.view.background_color;
            return true;
        case ER_PROP_COLOR:
            if (node->type != ER_NODE_TEXT)
                return false;
            *color = node->props.text.color;
            return true;
        default:
            return false;
    }
}

/**
 * @brief Recomputes node->has_transform from the current tp_* fields.
 *
 * @param[in,out] node  Node to update.
 */
static void update_has_transform(ERNode* node)
{
    node->has_transform = (node->tp_translate_x != 0.0f || node->tp_translate_y != 0.0f || node->tp_scale_x != 0.0f
                           || node->tp_scale_y != 0.0f || node->tp_rotate_z != 0.0f || node->tp_rotate_x != 0.0f
                           || node->tp_rotate_y != 0.0f || node->tp_perspective != 0.0f);
}

/**
 * @brief Applies a numeric value to a node property.
 *
 * @param[in,out] node   Target node.
 * @param[in]     prop   Property to update.
 * @param[in]     value  Numeric value.
 *
 * @return true if the stored value actually CHANGED (so the caller should mark the node dirty);
 *         false if the property was invalid for this node or the value was already current. Callers
 *         that repaint per frame gate er_mark_dirty_upward() on this so a plateaued native-driver
 *         animation (e.g. a wave box resting at translateY 0) does not force a needless repaint.
 */
static bool apply_numeric_value(ERNode* node, ERAnimProp prop, float value)
{
    if (!node)
        return false;

    switch (prop)
    {
        case ER_PROP_OPACITY:
        {
            if (!is_view_node(node))
                return false;
            if (value < 0.0f)
                value = 0.0f;
            if (value > 1.0f)
                value = 1.0f;
            const uint8_t o = (uint8_t)(value * 255.0f + 0.5f);
            if (node->props.view.opacity == o)
                return false;
            node->props.view.opacity = o;
            return true;
        }
        case ER_PROP_TRANSLATE_X:
            if (node->tp_translate_x == value)
                return false;
            node->tp_translate_x = value;
            update_has_transform(node);
            return true;
        case ER_PROP_TRANSLATE_Y:
            if (node->tp_translate_y == value)
                return false;
            node->tp_translate_y = value;
            update_has_transform(node);
            return true;
        case ER_PROP_SCALE_X:
            if (node->tp_scale_x == value)
                return false;
            node->tp_scale_x = value;
            update_has_transform(node);
            return true;
        case ER_PROP_SCALE_Y:
            if (node->tp_scale_y == value)
                return false;
            node->tp_scale_y = value;
            update_has_transform(node);
            return true;
        case ER_PROP_ROTATE_Z:
            if (node->tp_rotate_z == value)
                return false;
            node->tp_rotate_z = value;
            /* ActivityIndicator uses rotate_z as its internal spin angle; skip has_transform
             * so the affine-transform render path does not try to rasterize it into scratch. */
            if (node->type != ER_NODE_ACTIVITY_INDICATOR)
                update_has_transform(node);
            return true;
        case ER_PROP_ROTATE_X:
            if (node->tp_rotate_x == value)
                return false;
            node->tp_rotate_x = value;
            update_has_transform(node);
            return true;
        case ER_PROP_ROTATE_Y:
            if (node->tp_rotate_y == value)
                return false;
            node->tp_rotate_y = value;
            update_has_transform(node);
            return true;
        case ER_PROP_SWITCH_THUMB:
        {
            if (node->type != ER_NODE_SWITCH)
                return false;
            if (value < 0.0f)
                value = 0.0f;
            if (value > 1.0f)
                value = 1.0f;
            if (node->switch_thumb_t == value)
                return false;
            node->switch_thumb_t = value;
            return true;
        }
        default:
            return false;
    }
}

/**
 * @brief Linearly interpolates a color channel.
 *
 * @param[in] from  Start channel value.
 * @param[in] to    Target channel value.
 * @param[in] t     Interpolation fraction in range 0.0-1.0.
 *
 * @return Interpolated channel value.
 */
static uint8_t lerp_channel(uint8_t from, uint8_t to, float t)
{
    const float value = (float)from + ((float)to - (float)from) * t;
    return (uint8_t)(value + 0.5f);
}

/**
 * @brief Linearly interpolates an ARGB8888 color.
 *
 * @param[in] from  Start ARGB8888 color.
 * @param[in] to    Target ARGB8888 color.
 * @param[in] t     Interpolation fraction in range 0.0-1.0.
 *
 * @return Interpolated ARGB8888 color.
 */
static uint32_t lerp_color(uint32_t from, uint32_t to, float t)
{
    const uint8_t a = lerp_channel((uint8_t)(from >> 24), (uint8_t)(to >> 24), t);
    const uint8_t r = lerp_channel((uint8_t)(from >> 16), (uint8_t)(to >> 16), t);
    const uint8_t g = lerp_channel((uint8_t)(from >> 8), (uint8_t)(to >> 8), t);
    const uint8_t b = lerp_channel((uint8_t)from, (uint8_t)to, t);

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/**
 * @brief Applies a color value to a node property.
 *
 * @param[in,out] node   Target node.
 * @param[in]     prop   Property to update.
 * @param[in]     color  ARGB8888 color.
 *
 * @return true if the property was applied.
 */
static bool apply_color_value(ERNode* node, ERAnimProp prop, uint32_t color)
{
    if (!node)
        return false;

    switch (prop)
    {
        case ER_PROP_BACKGROUND_COLOR:
            if (!is_view_node(node))
                return false;
            node->props.view.background_color = color;
            return true;
        case ER_PROP_COLOR:
            if (node->type != ER_NODE_TEXT)
                return false;
            node->props.text.color = color;
            return true;
        default:
            return false;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — easing curves
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Evaluates the X component of a cubic bezier at parameter t.
 *
 * @param[in] t   Curve parameter [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] x2  Second control-point X.
 *
 * @return X coordinate on the curve.
 */
static float bezier_x(float t, float x1, float x2)
{
    const float c = 3.0f * x1;
    const float b = 3.0f * (x2 - x1) - c;
    const float a = 1.0f - c - b;
    return ((a * t + b) * t + c) * t;
}

/**
 * @brief Evaluates the derivative dX/dt of a cubic bezier at parameter t.
 *
 * @param[in] t   Curve parameter [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] x2  Second control-point X.
 *
 * @return dX/dt at t.
 */
static float bezier_dx(float t, float x1, float x2)
{
    const float c = 3.0f * x1;
    const float b = 3.0f * (x2 - x1) - c;
    const float a = 1.0f - c - b;
    return (3.0f * a * t + 2.0f * b) * t + c;
}

/**
 * @brief Finds the bezier curve parameter t for a given normalised time x using Newton's method.
 *
 * @param[in] x   Input normalised time [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] x2  Second control-point X.
 *
 * @return Bezier parameter t such that bezier_x(t, x1, x2) ≈ x.
 */
static float bezier_t_for_x(float x, float x1, float x2)
{
    float t = x;
    for (int i = 0; i < 8; i++)
    {
        const float dx = bezier_x(t, x1, x2) - x;
        if (dx > -0.00001f && dx < 0.00001f)
            break;
        const float d = bezier_dx(t, x1, x2);
        if (d < 0.00001f)
            break;
        t -= dx / d;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
    }
    return t;
}

/**
 * @brief Evaluates a cubic bezier easing curve for a given time input.
 *
 * @param[in] x   Normalised time [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] y1  First control-point Y.
 * @param[in] x2  Second control-point X.
 * @param[in] y2  Second control-point Y.
 *
 * @return Easing output Y for the given X.
 */
static float cubic_bezier(float x, float x1, float y1, float x2, float y2)
{
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    const float t = bezier_t_for_x(x, x1, x2);
    const float c = 3.0f * y1;
    const float b = 3.0f * (y2 - y1) - c;
    const float a = 1.0f - c - b;
    return ((a * t + b) * t + c) * t;
}

/**
 * @brief Applies an easing curve to a linear progress value.
 *
 * @param[in] t     Linear progress [0,1].
 * @param[in] ease  Easing function to apply.
 * @param[in] bx1   Bezier X1 (used only for ER_EASE_BEZIER).
 * @param[in] by1   Bezier Y1 (used only for ER_EASE_BEZIER).
 * @param[in] bx2   Bezier X2 (used only for ER_EASE_BEZIER).
 * @param[in] by2   Bezier Y2 (used only for ER_EASE_BEZIER).
 *
 * @return Eased progress value.
 */
static float apply_easing(float t, ERAnimEasing ease, float bx1, float by1, float bx2, float by2)
{
    switch (ease)
    {
        case ER_EASE_LINEAR:
        default:
            return t;

        case ER_EASE_EASE:
            return cubic_bezier(t, 0.25f, 0.1f, 0.25f, 1.0f);

        case ER_EASE_EASE_IN:
            return cubic_bezier(t, 0.42f, 0.0f, 1.0f, 1.0f);

        case ER_EASE_EASE_OUT:
            return cubic_bezier(t, 0.0f, 0.0f, 0.58f, 1.0f);

        case ER_EASE_EASE_IN_OUT:
            return cubic_bezier(t, 0.42f, 0.0f, 0.58f, 1.0f);

        case ER_EASE_QUAD_IN:
            return t * t;

        case ER_EASE_QUAD_OUT:
        {
            const float u = 1.0f - t;
            return 1.0f - u * u;
        }

        case ER_EASE_QUAD_IN_OUT:
            if (t < 0.5f)
                return 2.0f * t * t;
            else
            {
                const float u = 1.0f - t;
                return 1.0f - 2.0f * u * u;
            }

        case ER_EASE_CUBIC_IN:
            return t * t * t;

        case ER_EASE_CUBIC_OUT:
        {
            const float u = 1.0f - t;
            return 1.0f - u * u * u;
        }

        case ER_EASE_CUBIC_IN_OUT:
            if (t < 0.5f)
                return 4.0f * t * t * t;
            else
            {
                const float u = t - 1.0f;
                return 1.0f + 4.0f * u * u * u;
            }

        case ER_EASE_BOUNCE_OUT:
        {
            const float n = 7.5625f;
            const float d = 2.75f;
            float x = t;
            if (x < 1.0f / d)
                return n * x * x;
            else if (x < 2.0f / d)
            {
                x -= 1.5f / d;
                return n * x * x + 0.75f;
            }
            else if (x < 2.5f / d)
            {
                x -= 2.25f / d;
                return n * x * x + 0.9375f;
            }
            else
            {
                x -= 2.625f / d;
                return n * x * x + 0.984375f;
            }
        }

        case ER_EASE_ELASTIC_OUT:
        {
            if (t <= 0.0f)
                return 0.0f;
            if (t >= 1.0f)
                return 1.0f;
            const float p = 0.3f;
            const float s = p / 4.0f;
            return (float)pow(2.0, -10.0 * t) * (float)sin((t - s) * 6.283185f / p) + 1.0f;
        }

        case ER_EASE_BEZIER:
            return cubic_bezier(t, bx1, by1, bx2, by2);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — slot allocation / cancellation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Cancels any active animation matching node and prop and fires its on_complete(false).
 *
 * @param[in] node_tag  Node tag to match.
 * @param[in] prop      Property to match.
 */
static void cancel_by_tag(uint16_t node_tag, ERAnimProp prop)
{
    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        ERAnimation* a = &s_animations[i];
        if (a->active && a->node_tag == node_tag && a->prop == prop)
        {
            a->active = false;
            if (a->on_complete)
                a->on_complete(false, a->on_complete_user_data);
        }
    }
}

/**
 * @brief Returns a free animation slot, or NULL if the pool is full.
 *
 * @return Pointer to a free ERAnimation slot, or NULL.
 */
static ERAnimation* alloc_slot(void)
{
    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        if (!s_animations[i].active)
            return &s_animations[i];
    }
    return NULL;
}

/**
 * @brief Returns a free group slot, or NULL if the pool is full.
 *
 * @return Pointer to a free ERAnimGroup slot, or NULL.
 */
static ERAnimGroup* alloc_group(void)
{
    for (int i = 0; i < ERUI_MAX_ANIM_GROUPS; i++)
    {
        if (!s_groups[i].active)
            return &s_groups[i];
    }
    return NULL;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — animatable values
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Finds an ERAnimValue slot by handle.
 *
 * @param[in] handle  Handle to look up.
 *
 * @return Pointer to the matching in-use slot, or NULL if not found.
 */
static ERAnimValue* find_value_slot(uint16_t handle)
{
    if (handle == 0U)
        return NULL;
    for (int i = 0; i < ERUI_MAX_ANIM_VALUES; i++)
    {
        if (s_anim_values[i].in_use && s_anim_values[i].handle == handle)
            return &s_anim_values[i];
    }
    return NULL;
}

/**
 * @brief Cancels any active animation slot that targets the given ERAnimValue handle.
 *
 * @param[in] value_handle  ERAnimValue handle whose animation should be cancelled.
 */
static void cancel_value_anim(uint16_t value_handle)
{
    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        ERAnimation* a = &s_animations[i];
        if (a->active && a->value_handle == value_handle)
        {
            a->active = false;
            if (a->on_complete)
                a->on_complete(false, a->on_complete_user_data);
        }
    }
}

/**
 * @brief Propagates an ERAnimValue's current float to all its bound node-property pairs.
 *
 * A bound node is marked dirty ONLY when the applied value actually changed it. This keeps a value
 * with many bindings cheap when most of them are momentarily unchanged (e.g. a "ripple" wave where
 * only the boxes under the moving band are displaced; the resting boxes apply the same value and are
 * not repainted), so the per-frame damage stays localized instead of covering every bound node.
 *
 * @param[in] val  Value whose current float is pushed to every active binding.
 */
static void push_to_value_bindings(ERAnimValue* val)
{
    for (int b = 0; b < val->binding_count; b++)
    {
        ERValueBinding* bind = &val->bindings[b];
        if (!bind->active)
            continue;
        ERNode* n = er_get_node(bind->node_tag);
        if (!n)
            continue;
        float v = val->current;
        if (bind->interpolated)
            v = er_interpolate(v,
                               bind->interp.input_range,
                               bind->interp.output_range,
                               (int)bind->interp.point_count,
                               bind->interp.extrapolate_left,
                               bind->interp.extrapolate_right);
        if (apply_numeric_value(n, bind->prop, v))
            er_mark_dirty_upward(n);
    }
}

void er_anim_reapply_bound(ERNode* node)
{
    if (!node)
        return;
    /* Re-push the current animated value to every prop of this node that is bound to an
     * ERAnimValue. Called after er_node_set_props so a declarative prop update (React commitUpdate)
     * does not clobber a native-driver animation: setProps writes the static value, then this
     * restores the animated value — within the same commit, before the frame is painted. */
    for (int s = 0; s < ERUI_MAX_ANIM_VALUES; s++)
    {
        ERAnimValue* val = &s_anim_values[s];
        if (!val->in_use)
            continue;
        for (int b = 0; b < val->binding_count; b++)
        {
            ERValueBinding* bind = &val->bindings[b];
            if (!bind->active || bind->node_tag != node->tag)
                continue;
            float v = val->current;
            if (bind->interpolated)
                v = er_interpolate(v,
                                   bind->interp.input_range,
                                   bind->interp.output_range,
                                   (int)bind->interp.point_count,
                                   bind->interp.extrapolate_left,
                                   bind->interp.extrapolate_right);
            (void)apply_numeric_value(node, bind->prop, v);
        }
    }
}

void er_anim_unbind_node(uint16_t node_tag)
{
    /*
     * Drop every animated-value binding that targets this node. Called from er_node_destroy so a destroyed
     * node leaves nothing behind: its tag is pushed to the free list and handed to a future node, and a
     * lingering binding would otherwise make the value push its float to that unrelated node — a
     * native-driver animation writing to the wrong element.Mirrors er_anim_reapply_bound's walk; deactivating
     * a binding is just a flag, no callback.
     */
    for (int s = 0; s < ERUI_MAX_ANIM_VALUES; s++)
    {
        ERAnimValue* val = &s_anim_values[s];
        if (!val->in_use)
            continue;
        for (int b = 0; b < val->binding_count; b++)
        {
            if (val->bindings[b].active && val->bindings[b].node_tag == node_tag)
                val->bindings[b].active = false;
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — spring / decay tick helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Integrates one millisecond of spring physics.
 *
 * @param[in,out] pos        Current spring position.
 * @param[in,out] vel        Current spring velocity.
 * @param[in]     stiffness  Spring constant k.
 * @param[in]     damping    Damping coefficient c.
 * @param[in]     mass       Spring mass m.
 */
static void spring_step(float* pos, float* vel, float stiffness, float damping, float mass)
{
    const float dt = 0.001f;
    const float disp = *pos - 1.0f;
    const float accel = (-stiffness * disp - damping * (*vel)) / mass;
    *vel += accel * dt;
    *pos += (*vel) * dt;
}

/**
 * @brief Integrates spring physics for delta_ms milliseconds (max 200 steps).
 *
 * @param[in,out] anim       Animation slot to update.
 * @param[in]     delta_ms   Elapsed time in milliseconds.
 *
 * @return true when the spring has settled.
 */
static bool tick_spring(ERAnimation* anim, uint32_t delta_ms)
{
    uint32_t steps = delta_ms;
    if (steps > 200)
        steps = 200;

    const float k = anim->spring_stiffness > 0.0f ? anim->spring_stiffness : SPRING_DEFAULT_STIFFNESS;
    const float c = anim->spring_damping > 0.0f ? anim->spring_damping : SPRING_DEFAULT_DAMPING;
    const float m = anim->spring_mass > 0.0f ? anim->spring_mass : SPRING_DEFAULT_MASS;

    for (uint32_t i = 0; i < steps; i++)
        spring_step(&anim->spring_pos, &anim->spring_vel, k, c, m);

    const float disp = anim->spring_pos - 1.0f;
    return (disp > -SPRING_SETTLE_DISP && disp < SPRING_SETTLE_DISP)
           && (anim->spring_vel > -SPRING_SETTLE_VEL && anim->spring_vel < SPRING_SETTLE_VEL);
}

/**
 * @brief Advances decay physics for delta_ms milliseconds.
 *
 * @param[in,out] anim       Animation slot to update.
 * @param[in]     delta_ms   Elapsed time in milliseconds.
 *
 * @return true when the velocity has dropped below DECAY_VEL_STOP.
 */
static bool tick_decay(ERAnimation* anim, uint32_t delta_ms)
{
    const float decel = anim->decay_deceleration > 0.0f ? anim->decay_deceleration : DECAY_DEFAULT_DECELERATION;

    for (uint32_t i = 0; i < delta_ms; i++)
    {
        anim->decay_pos += anim->decay_velocity;
        anim->decay_velocity *= decel;
    }

    const float v = anim->decay_velocity;
    return (v > -DECAY_VEL_STOP && v < DECAY_VEL_STOP);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — group completion / progression
 ---------------------------------------------------------------------------------------------------------------------*/

/* Forward declaration needed because start_group_entry calls er_anim_start indirectly. */
static ERAnimHandle start_anim_internal(
    uint16_t node_tag, ERAnimProp prop, float value, const ERAnimConfig* cfg, uint8_t group_slot, uint8_t group_entry);

/**
 * @brief Called when an animation belonging to a group completes naturally.
 *
 * @param[in] group_slot   Index of the owning group in s_groups.
 * @param[in] entry_index  Index of the completed entry within the group.
 */
static void on_group_entry_complete(uint8_t group_slot, uint8_t entry_index)
{
    if (group_slot >= ERUI_MAX_ANIM_GROUPS)
        return;
    ERAnimGroup* grp = &s_groups[group_slot];
    if (!grp->active)
        return;

    if (grp->type == GROUP_TYPE_SEQUENCE)
    {
        (void)entry_index;
        grp->current++;
        if (grp->current < grp->count)
        {
            ERGroupEntry* e = &grp->entries[grp->current];
            (void)start_anim_internal(e->node_tag, e->prop, e->value, &e->cfg, group_slot, (uint8_t)grp->current);
        }
        else
        {
            grp->active = false;
            if (grp->on_complete)
                grp->on_complete(true, grp->on_complete_user_data);
        }
    }
    else /* PARALLEL or STAGGER */
    {
        grp->done++;
        if (grp->done >= grp->count)
        {
            grp->active = false;
            if (grp->on_complete)
                grp->on_complete(true, grp->on_complete_user_data);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — core animation start
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Internal animation start that allows group back-references.
 *
 * @param[in] node_tag    Tag of the node to animate.
 * @param[in] prop        Property to animate.
 * @param[in] value       Target value.
 * @param[in] cfg         Configuration (NULL = immediate snap).
 * @param[in] group_slot  Owner group slot; UINT8_MAX if standalone.
 * @param[in] group_entry Entry index within the group.
 *
 * @return Handle, or ER_ANIM_HANDLE_INVALID on failure.
 */
static ERAnimHandle start_anim_internal(
    uint16_t node_tag, ERAnimProp prop, float value, const ERAnimConfig* cfg, uint8_t group_slot, uint8_t group_entry)
{
    ERNode* node = er_get_node(node_tag);
    if (!node || !node->in_use)
        return ER_ANIM_HANDLE_INVALID;

    const ERAnimType type = cfg ? cfg->type : ER_ANIM_TIMING;

    cancel_by_tag(node_tag, prop);

    const bool color = is_color_prop(prop);

    /* Immediate-snap path: zero duration timing with no delay. */
    const uint32_t dur = cfg ? cfg->duration_ms : 0U;
    const uint32_t dly = cfg ? cfg->delay_ms : 0U;

    if (type == ER_ANIM_TIMING && dur == 0U && dly == 0U)
    {
        if (color)
            (void)apply_color_value(node, prop, bits_from_float(value));
        else
            (void)apply_numeric_value(node, prop, value);
        er_mark_dirty_upward(node);

        if (cfg && cfg->on_complete)
            cfg->on_complete(true, cfg->on_complete_user_data);
        if (group_slot != 0xFFU)
            on_group_entry_complete(group_slot, group_entry);
        return ER_ANIM_HANDLE_INVALID;
    }

    ERAnimation* anim = alloc_slot();
    if (!anim)
        return ER_ANIM_HANDLE_INVALID;

    memset(anim, 0, sizeof(*anim));
    anim->active = true;
    anim->node_tag = node_tag;
    anim->handle = alloc_handle();
    anim->prop = prop;
    anim->type = type;
    anim->easing = cfg ? cfg->easing : ER_EASE_LINEAR;
    anim->bezier_x1 = cfg ? cfg->bezier_x1 : 0.0f;
    anim->bezier_y1 = cfg ? cfg->bezier_y1 : 0.0f;
    anim->bezier_x2 = cfg ? cfg->bezier_x2 : 1.0f;
    anim->bezier_y2 = cfg ? cfg->bezier_y2 : 1.0f;
    anim->delay_ms = dly;
    anim->in_delay = dly > 0U;
    anim->duration_ms = dur;
    anim->loop = cfg ? cfg->loop : false;
    anim->loop_reverse = cfg ? cfg->loop_reverse : false;
    anim->color_value = color;
    anim->on_complete = cfg ? cfg->on_complete : NULL;
    anim->on_complete_user_data = cfg ? cfg->on_complete_user_data : NULL;
    anim->group_slot = group_slot;
    anim->group_entry = group_entry;

    if (color)
    {
        if (!read_color_value(node, prop, &anim->from_color))
        {
            anim->active = false;
            return ER_ANIM_HANDLE_INVALID;
        }
        anim->to_color = bits_from_float(value);
    }
    else
    {
        if (!read_numeric_value(node, prop, &anim->from_value))
        {
            anim->active = false;
            return ER_ANIM_HANDLE_INVALID;
        }
        anim->to_value = value;
    }

    if (type == ER_ANIM_SPRING)
    {
        anim->spring_stiffness = cfg ? cfg->stiffness : 0.0f;
        anim->spring_damping = cfg ? cfg->damping : 0.0f;
        anim->spring_mass = cfg ? cfg->mass : 0.0f;
        /* Initial velocity is supplied as value/s; convert to normalised/s. */
        const float range = anim->to_value - anim->from_value;
        anim->spring_pos = 0.0f;
        anim->spring_vel = (range != 0.0f && cfg) ? cfg->velocity / range : 0.0f;
    }
    else if (type == ER_ANIM_DECAY)
    {
        /* velocity is value/ms; convert to normalised/ms relative to the range. */
        const float range = anim->to_value - anim->from_value;
        anim->decay_velocity = (range != 0.0f && cfg) ? cfg->velocity / range : 0.0f;
        anim->decay_deceleration = (cfg && cfg->deceleration > 0.0f) ? cfg->deceleration : DECAY_DEFAULT_DECELERATION;
    }

    return anim->handle;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

ERAnimHandle er_anim_start(ERNode* node, ERAnimProp prop, float value, const ERAnimConfig* cfg)
{
    if (!node || !node->in_use)
        return ER_ANIM_HANDLE_INVALID;
    return start_anim_internal(node->tag, prop, value, cfg, 0xFFU, 0U);
}

void er_anim_cancel(ERNode* node, ERAnimProp prop)
{
    if (!node)
        return;
    cancel_by_tag(node->tag, prop);
}

void er_anim_stop(ERAnimHandle handle)
{
    if (handle == ER_ANIM_HANDLE_INVALID)
        return;

    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        ERAnimation* a = &s_animations[i];
        if (a->active && a->handle == handle)
        {
            a->active = false;
            if (a->on_complete)
                a->on_complete(false, a->on_complete_user_data);
            return;
        }
    }

    for (int i = 0; i < ERUI_MAX_ANIM_GROUPS; i++)
    {
        ERAnimGroup* grp = &s_groups[i];
        if (grp->active && grp->handle == handle)
        {
            /* Cancel every running animation that belongs to this group. */
            for (int j = 0; j < ERUI_MAX_ANIMATIONS; j++)
            {
                if (s_animations[j].active && s_animations[j].group_slot == (uint8_t)i)
                {
                    s_animations[j].active = false;
                    if (s_animations[j].on_complete)
                        s_animations[j].on_complete(false, s_animations[j].on_complete_user_data);
                }
            }
            grp->active = false;
            if (grp->on_complete)
                grp->on_complete(false, grp->on_complete_user_data);
            return;
        }
    }
}

ERAnimHandle er_anim_sequence(const ERAnimEntry* entries, uint16_t count, ERAnimCompleteFn on_complete, void* user_data)
{
    if (!entries || count == 0)
        return ER_ANIM_HANDLE_INVALID;
    ERAnimGroup* grp = alloc_group();
    if (!grp)
        return ER_ANIM_HANDLE_INVALID;

    memset(grp, 0, sizeof(*grp));
    grp->active = true;
    grp->type = GROUP_TYPE_SEQUENCE;
    grp->handle = alloc_handle();
    grp->on_complete = on_complete;
    grp->on_complete_user_data = user_data;
    grp->count = count < ERUI_MAX_GROUP_ENTRIES ? count : ERUI_MAX_GROUP_ENTRIES;

    for (uint16_t i = 0; i < grp->count; i++)
    {
        const ERNode* node = entries[i].node;
        grp->entries[i].node_tag = node ? node->tag : (uint16_t)0xFFFFU;
        grp->entries[i].prop = entries[i].prop;
        grp->entries[i].value = entries[i].value;
        grp->entries[i].cfg = entries[i].cfg;
        grp->entries[i].cfg.on_complete = NULL;
    }

    const uint8_t slot = (uint8_t)(grp - s_groups);

    /* Start the first entry. */
    ERGroupEntry* e = &grp->entries[0];
    (void)start_anim_internal(e->node_tag, e->prop, e->value, &e->cfg, slot, 0U);

    return grp->handle;
}

ERAnimHandle er_anim_parallel(const ERAnimEntry* entries, uint16_t count, ERAnimCompleteFn on_complete, void* user_data)
{
    if (!entries || count == 0)
        return ER_ANIM_HANDLE_INVALID;
    ERAnimGroup* grp = alloc_group();
    if (!grp)
        return ER_ANIM_HANDLE_INVALID;

    memset(grp, 0, sizeof(*grp));
    grp->active = true;
    grp->type = GROUP_TYPE_PARALLEL;
    grp->handle = alloc_handle();
    grp->on_complete = on_complete;
    grp->on_complete_user_data = user_data;
    grp->count = count < ERUI_MAX_GROUP_ENTRIES ? count : ERUI_MAX_GROUP_ENTRIES;

    for (uint16_t i = 0; i < grp->count; i++)
    {
        const ERNode* node = entries[i].node;
        grp->entries[i].node_tag = node ? node->tag : (uint16_t)0xFFFFU;
        grp->entries[i].prop = entries[i].prop;
        grp->entries[i].value = entries[i].value;
        grp->entries[i].cfg = entries[i].cfg;
        grp->entries[i].cfg.on_complete = NULL;
    }

    const uint8_t slot = (uint8_t)(grp - s_groups);

    for (uint16_t i = 0; i < grp->count; i++)
    {
        ERGroupEntry* e = &grp->entries[i];
        (void)start_anim_internal(e->node_tag, e->prop, e->value, &e->cfg, slot, (uint8_t)i);
    }

    return grp->handle;
}

ERAnimHandle er_anim_stagger(
    const ERAnimEntry* entries, uint16_t count, uint32_t stagger_ms, ERAnimCompleteFn on_complete, void* user_data)
{
    if (!entries || count == 0)
        return ER_ANIM_HANDLE_INVALID;
    ERAnimGroup* grp = alloc_group();
    if (!grp)
        return ER_ANIM_HANDLE_INVALID;

    memset(grp, 0, sizeof(*grp));
    grp->active = true;
    grp->type = GROUP_TYPE_STAGGER;
    grp->handle = alloc_handle();
    grp->stagger_ms = stagger_ms;
    grp->on_complete = on_complete;
    grp->on_complete_user_data = user_data;
    grp->count = count < ERUI_MAX_GROUP_ENTRIES ? count : ERUI_MAX_GROUP_ENTRIES;

    for (uint16_t i = 0; i < grp->count; i++)
    {
        const ERNode* node = entries[i].node;
        grp->entries[i].node_tag = node ? node->tag : (uint16_t)0xFFFFU;
        grp->entries[i].prop = entries[i].prop;
        grp->entries[i].value = entries[i].value;
        grp->entries[i].cfg = entries[i].cfg;
        grp->entries[i].cfg.on_complete = NULL;
        grp->entries[i].cfg.delay_ms += (uint32_t)i * stagger_ms;
    }

    const uint8_t slot = (uint8_t)(grp - s_groups);

    for (uint16_t i = 0; i < grp->count; i++)
    {
        ERGroupEntry* e = &grp->entries[i];
        (void)start_anim_internal(e->node_tag, e->prop, e->value, &e->cfg, slot, (uint8_t)i);
    }

    return grp->handle;
}

ERAnimValueHandle er_anim_value_create(float initial_value)
{
    for (int i = 0; i < ERUI_MAX_ANIM_VALUES; i++)
    {
        ERAnimValue* v = &s_anim_values[i];
        if (!v->in_use)
        {
            memset(v, 0, sizeof(*v));
            v->in_use = true;
            v->handle = alloc_handle();
            v->current = initial_value;
            return v->handle;
        }
    }
    return ER_ANIM_VALUE_INVALID;
}

void er_anim_value_destroy(ERAnimValueHandle handle)
{
    if (handle == ER_ANIM_VALUE_INVALID)
        return;
    cancel_value_anim(handle);
    ERAnimValue* val = find_value_slot(handle);
    if (val)
        val->in_use = false;
}

void er_anim_value_bind(ERAnimValueHandle handle, ERNode* node, ERAnimProp prop)
{
    if (handle == ER_ANIM_VALUE_INVALID || !node)
        return;
    ERAnimValue* val = find_value_slot(handle);
    if (!val)
        return;
    /* Refuse duplicate bindings for the same node+prop pair. */
    for (int b = 0; b < val->binding_count; b++)
    {
        if (val->bindings[b].active && val->bindings[b].node_tag == node->tag && val->bindings[b].prop == prop)
            return;
    }
    for (int b = 0; b < ERUI_MAX_VALUE_BINDINGS; b++)
    {
        if (!val->bindings[b].active)
        {
            val->bindings[b].node_tag = node->tag;
            val->bindings[b].prop = prop;
            val->bindings[b].active = true;
            if ((uint8_t)(b + 1) > val->binding_count)
                val->binding_count = (uint8_t)(b + 1);
            /* Apply immediately so the node reflects the current value on bind (matches the
             * interpolated path) — keeps a freshly-bound node from showing its default until the
             * value next changes. */
            (void)apply_numeric_value(node, prop, val->current);
            er_mark_dirty_upward(node);
            return;
        }
    }
}

float er_interpolate(float input,
                     const float* input_range,
                     const float* output_range,
                     int point_count,
                     ERExtrapolate extrapolate_left,
                     ERExtrapolate extrapolate_right)
{
    if (!input_range || !output_range || point_count < 2)
        return input;

    const int last = point_count - 1;

    if (input < input_range[0])
    {
        if (extrapolate_left == ER_EXTRAPOLATE_CLAMP)
            return output_range[0];
        if (extrapolate_left == ER_EXTRAPOLATE_IDENTITY)
            return input;
        /* EXTEND: continue the slope of the first segment. */
        {
            const float span = input_range[1] - input_range[0];
            if (span == 0.0f)
                return output_range[0];
            return output_range[0] + (output_range[1] - output_range[0]) * ((input - input_range[0]) / span);
        }
    }

    if (input > input_range[last])
    {
        if (extrapolate_right == ER_EXTRAPOLATE_CLAMP)
            return output_range[last];
        if (extrapolate_right == ER_EXTRAPOLATE_IDENTITY)
            return input;
        /* EXTEND: continue the slope of the last segment. */
        {
            const float span = input_range[last] - input_range[last - 1];
            if (span == 0.0f)
                return output_range[last];
            return output_range[last - 1]
                   + (output_range[last] - output_range[last - 1]) * ((input - input_range[last - 1]) / span);
        }
    }

    for (int i = 1; i <= last; ++i)
    {
        if (input <= input_range[i])
        {
            const float span = input_range[i] - input_range[i - 1];
            if (span == 0.0f)
                return output_range[i - 1];
            return output_range[i - 1]
                   + (output_range[i] - output_range[i - 1]) * ((input - input_range[i - 1]) / span);
        }
    }
    return output_range[last];
}

void er_anim_value_bind_interpolated(ERAnimValueHandle handle,
                                     ERNode* node,
                                     ERAnimProp prop,
                                     const ERInterpolation* interp)
{
    if (handle == ER_ANIM_VALUE_INVALID || !node || !interp || interp->point_count < 2u)
        return;
    ERAnimValue* val = find_value_slot(handle);
    if (!val)
        return;
    /* Refuse duplicate bindings for the same node+prop pair (the reconciler may re-bind on every
     * render). Without this, re-renders would exhaust the binding pool. */
    for (int b = 0; b < val->binding_count; b++)
    {
        if (val->bindings[b].active && val->bindings[b].node_tag == node->tag && val->bindings[b].prop == prop)
            return;
    }
    for (int b = 0; b < ERUI_MAX_VALUE_BINDINGS; b++)
    {
        if (!val->bindings[b].active)
        {
            val->bindings[b].node_tag = node->tag;
            val->bindings[b].prop = prop;
            val->bindings[b].active = true;
            val->bindings[b].interpolated = true;
            val->bindings[b].interp = *interp;
            if ((uint8_t)(b + 1) > val->binding_count)
                val->binding_count = (uint8_t)(b + 1);
            /* Apply immediately so the node reflects the current value on bind. */
            {
                const float v = er_interpolate(val->current,
                                               interp->input_range,
                                               interp->output_range,
                                               (int)interp->point_count,
                                               interp->extrapolate_left,
                                               interp->extrapolate_right);
                (void)apply_numeric_value(node, prop, v);
                er_mark_dirty_upward(node);
            }
            return;
        }
    }
}

void er_anim_value_unbind_all(ERAnimValueHandle handle)
{
    ERAnimValue* val = find_value_slot(handle);
    if (!val)
        return;
    for (int b = 0; b < ERUI_MAX_VALUE_BINDINGS; b++)
        val->bindings[b].active = false;
    val->binding_count = 0;
}

ERAnimHandle er_anim_value_animate(ERAnimValueHandle handle, float to_value, const ERAnimConfig* cfg)
{
    ERAnimValue* val = find_value_slot(handle);
    if (!val)
        return ER_ANIM_HANDLE_INVALID;

    cancel_value_anim(handle);

    const ERAnimType type = cfg ? cfg->type : ER_ANIM_TIMING;
    const uint32_t dur = cfg ? cfg->duration_ms : 0U;
    const uint32_t dly = cfg ? cfg->delay_ms : 0U;

    /* Immediate-snap path: zero-duration timing with no delay. */
    if (type == ER_ANIM_TIMING && dur == 0U && dly == 0U)
    {
        val->current = to_value;
        push_to_value_bindings(val);
        if (cfg && cfg->on_complete)
            cfg->on_complete(true, cfg->on_complete_user_data);
        return ER_ANIM_HANDLE_INVALID;
    }

    ERAnimation* anim = alloc_slot();
    if (!anim)
        return ER_ANIM_HANDLE_INVALID;

    memset(anim, 0, sizeof(*anim));
    anim->active = true;
    anim->value_handle = handle;
    anim->node_tag = 0xFFFFU; /* unused sentinel for value animations */
    anim->handle = alloc_handle();
    anim->prop = ER_PROP_OPACITY; /* unused placeholder */
    anim->type = type;
    anim->easing = cfg ? cfg->easing : ER_EASE_LINEAR;
    anim->bezier_x1 = cfg ? cfg->bezier_x1 : 0.0f;
    anim->bezier_y1 = cfg ? cfg->bezier_y1 : 0.0f;
    anim->bezier_x2 = cfg ? cfg->bezier_x2 : 1.0f;
    anim->bezier_y2 = cfg ? cfg->bezier_y2 : 1.0f;
    anim->delay_ms = dly;
    anim->in_delay = dly > 0U;
    anim->duration_ms = dur;
    anim->loop = cfg ? cfg->loop : false;
    anim->loop_reverse = cfg ? cfg->loop_reverse : false;
    anim->color_value = false;
    anim->from_value = val->current;
    anim->to_value = to_value;
    anim->on_complete = cfg ? cfg->on_complete : NULL;
    anim->on_complete_user_data = cfg ? cfg->on_complete_user_data : NULL;
    anim->group_slot = 0xFFU;
    anim->group_entry = 0U;

    if (type == ER_ANIM_SPRING)
    {
        anim->spring_stiffness = cfg ? cfg->stiffness : 0.0f;
        anim->spring_damping = cfg ? cfg->damping : 0.0f;
        anim->spring_mass = cfg ? cfg->mass : 0.0f;
        const float range = to_value - val->current;
        anim->spring_pos = 0.0f;
        anim->spring_vel = (range != 0.0f && cfg) ? cfg->velocity / range : 0.0f;
    }
    else if (type == ER_ANIM_DECAY)
    {
        const float range = to_value - val->current;
        anim->decay_velocity = (range != 0.0f && cfg) ? cfg->velocity / range : 0.0f;
        anim->decay_deceleration = (cfg && cfg->deceleration > 0.0f) ? cfg->deceleration : DECAY_DEFAULT_DECELERATION;
    }

    return anim->handle;
}

void er_anim_value_set(ERAnimValueHandle handle, float value)
{
    ERAnimValue* val = find_value_slot(handle);
    if (!val)
        return;
    cancel_value_anim(handle);
    val->current = value;
    push_to_value_bindings(val);
}

float er_anim_value_get(ERAnimValueHandle handle)
{
    const ERAnimValue* val = find_value_slot(handle);
    return val ? val->current : 0.0f;
}

void er_anim_tick(uint32_t delta_ms)
{
    for (int i = 0; i < ERUI_MAX_ANIMATIONS; i++)
    {
        ERAnimation* anim = &s_animations[i];
        if (!anim->active)
            continue;

        /* ---- Value-target animation (ERAnimValue — no specific node) ---- */
        if (anim->value_handle != 0U)
        {
            ERAnimValue* val = find_value_slot(anim->value_handle);
            if (!val)
            {
                anim->active = false;
                continue;
            }

            uint32_t eff = delta_ms;
            if (anim->in_delay)
            {
                anim->delay_elapsed_ms += delta_ms;
                if (anim->delay_elapsed_ms < anim->delay_ms)
                    continue;
                eff = anim->delay_elapsed_ms - anim->delay_ms;
                anim->in_delay = false;
                anim->from_value = val->current;
                if (anim->type == ER_ANIM_SPRING)
                    anim->spring_pos = 0.0f;
                else if (anim->type == ER_ANIM_DECAY)
                    anim->decay_pos = 0.0f;
            }

            bool vfinished = false;
            float result = anim->from_value;

            if (anim->type == ER_ANIM_TIMING)
            {
                anim->elapsed_ms += eff;
                float t = 1.0f;
                if (anim->duration_ms > 0U && anim->elapsed_ms < anim->duration_ms)
                    t = (float)anim->elapsed_ms / (float)anim->duration_ms;
                const float et =
                    apply_easing(t, anim->easing, anim->bezier_x1, anim->bezier_y1, anim->bezier_x2, anim->bezier_y2);
                result = anim->from_value + (anim->to_value - anim->from_value) * et;
                if (t >= 1.0f)
                {
                    if (anim->loop)
                    {
                        anim->elapsed_ms = 0U;
                        if (anim->loop_reverse) /* ping-pong; else restart from the start (continuous) */
                        {
                            const float tmp = anim->from_value;
                            anim->from_value = anim->to_value;
                            anim->to_value = tmp;
                        }
                    }
                    else
                    {
                        vfinished = true;
                    }
                }
            }
            else if (anim->type == ER_ANIM_SPRING)
            {
                const bool settled = tick_spring(anim, eff);
                float pos = anim->spring_pos;
                if (pos < -2.0f)
                    pos = -2.0f;
                if (pos > 3.0f)
                    pos = 3.0f;
                result = anim->from_value + (anim->to_value - anim->from_value) * pos;
                if (settled)
                {
                    result = anim->to_value;
                    vfinished = true;
                }
            }
            else if (anim->type == ER_ANIM_DECAY)
            {
                const bool stopped = tick_decay(anim, eff);
                result = anim->from_value + (anim->to_value - anim->from_value) * anim->decay_pos;
                if (stopped)
                    vfinished = true;
            }

            val->current = result;
            push_to_value_bindings(val);

            if (vfinished)
            {
                const ERAnimCompleteFn cb = anim->on_complete;
                void* ud = anim->on_complete_user_data;
                const uint8_t gs = anim->group_slot;
                const uint8_t ge = anim->group_entry;
                anim->active = false;
                if (cb)
                    cb(true, ud);
                if (gs != 0xFFU)
                    on_group_entry_complete(gs, ge);
            }
            continue;
        }

        ERNode* node = er_get_node(anim->node_tag);
        if (!node)
        {
            anim->active = false;
            continue;
        }

        /* --- Delay countdown --- */
        uint32_t effective_delta = delta_ms;
        if (anim->in_delay)
        {
            anim->delay_elapsed_ms += delta_ms;
            if (anim->delay_elapsed_ms < anim->delay_ms)
                continue;
            /* Delay expired partway through this tick; only the remainder counts. */
            effective_delta = anim->delay_elapsed_ms - anim->delay_ms;
            anim->in_delay = false;
            /* Latch the "from" value at the moment the animation actually begins. */
            if (anim->color_value)
            {
                (void)read_color_value(node, anim->prop, &anim->from_color);
            }
            else
            {
                (void)read_numeric_value(node, anim->prop, &anim->from_value);
                if (anim->type == ER_ANIM_SPRING)
                    anim->spring_pos = 0.0f;
                else if (anim->type == ER_ANIM_DECAY)
                    anim->decay_pos = 0.0f;
            }
        }

        bool finished = false;

        if (anim->type == ER_ANIM_TIMING)
        {
            anim->elapsed_ms += effective_delta;
            float t = 1.0f;
            if (anim->duration_ms > 0U && anim->elapsed_ms < anim->duration_ms)
                t = (float)anim->elapsed_ms / (float)anim->duration_ms;

            const float et =
                apply_easing(t, anim->easing, anim->bezier_x1, anim->bezier_y1, anim->bezier_x2, anim->bezier_y2);

            if (anim->color_value)
                (void)apply_color_value(node, anim->prop, lerp_color(anim->from_color, anim->to_color, et));
            else
                (void)apply_numeric_value(
                    node, anim->prop, anim->from_value + (anim->to_value - anim->from_value) * et);

            if (t >= 1.0f)
            {
                if (anim->loop)
                {
                    anim->elapsed_ms = 0U;
                    if (anim->loop_reverse) /* ping-pong; else restart from the start (continuous one-way) */
                    {
                        if (anim->color_value)
                        {
                            const uint32_t tmp = anim->from_color;
                            anim->from_color = anim->to_color;
                            anim->to_color = tmp;
                        }
                        else
                        {
                            const float tmp = anim->from_value;
                            anim->from_value = anim->to_value;
                            anim->to_value = tmp;
                        }
                    }
                }
                else
                {
                    finished = true;
                }
            }
        }
        else if (anim->type == ER_ANIM_SPRING)
        {
            const bool settled = tick_spring(anim, effective_delta);

            /* Clamp spring_pos to a reasonable range to prevent runaway. */
            float pos = anim->spring_pos;
            if (pos < -2.0f)
                pos = -2.0f;
            if (pos > 3.0f)
                pos = 3.0f;
            (void)apply_numeric_value(node, anim->prop, anim->from_value + (anim->to_value - anim->from_value) * pos);

            if (settled)
            {
                (void)apply_numeric_value(node, anim->prop, anim->to_value);
                finished = true;
            }
        }
        else if (anim->type == ER_ANIM_DECAY)
        {
            const bool stopped = tick_decay(anim, effective_delta);
            (void)apply_numeric_value(
                node, anim->prop, anim->from_value + (anim->to_value - anim->from_value) * anim->decay_pos);

            if (stopped)
                finished = true;
        }

        er_mark_dirty_upward(node);

        if (finished)
        {
            const ERAnimCompleteFn cb = anim->on_complete;
            void* ud = anim->on_complete_user_data;
            const uint8_t gs = anim->group_slot;
            const uint8_t ge = anim->group_entry;

            anim->active = false;

            if (cb)
                cb(true, ud);
            if (gs != 0xFFU)
                on_group_entry_complete(gs, ge);
        }
    }
}
