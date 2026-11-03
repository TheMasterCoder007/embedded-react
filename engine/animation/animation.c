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

#include "arc_widget.h"
#include "er_node_internal.h"
#include "renderer_internal.h"
#include "spring.h"
#include "transform.h"
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

/** @brief ERAnimation::group_slot of an animation that belongs to no group. */
#define GROUP_SLOT_NONE 0xFFU

/** @brief ERAnimation::node_tag of a value animation, and of a group entry with no node. */
#define ANIM_NO_NODE_TAG 0xFFFFU

/** @brief The opacity byte at full opacity, and the largest value of a colour channel. */
#define OPACITY_BYTE_MAX 255.0f
#define CHANNEL_MAX 255U

/** @brief Stored for a Text colour animated to 0: the painter reads 0 as unset and draws white. */
#define TEXT_COLOR_TRANSPARENT 0x00FFFFFFU

/** @brief Newton's method on a bezier curve: iteration cap, and the error or slope too small to go on. */
#define BEZIER_NEWTON_ITERATIONS 8
#define BEZIER_NEWTON_EPSILON 0.00001f

/* Control points (x1, y1, x2, y2) of the standard CSS easing curves. */
#define CURVE_EASE 0.25f, 0.1f, 0.25f, 1.0f
#define CURVE_EASE_IN 0.42f, 0.0f, 1.0f, 1.0f
#define CURVE_EASE_OUT 0.0f, 0.0f, 0.58f, 1.0f
#define CURVE_EASE_IN_OUT 0.42f, 0.0f, 0.58f, 1.0f

/** @brief ER_EASE_ELASTIC_OUT: one full wave, in radians. */
#define ELASTIC_TWO_PI 6.283185f

/** @brief A spring's position is held to this range when applied, so a runaway cannot reach the node. */
#define SPRING_POSITION_MIN (-2.0f)
#define SPRING_POSITION_MAX 3.0f

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
    float spring_position; /**< Current normalized position [0 = from, 1 = to, may overshoot]. */
    float spring_velocity; /**< Current normalized velocity (units of progress per second). */
    /* Decay-specific state */
    float decay_velocity;     /**< Current velocity in (to_value - from_value)/ms units. */
    float decay_position;     /**< Accumulated decay offset (used to derive from_value). */
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
    ERAnimConfig config;
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
    bool interpolated; /**< When true, value is mapped through map before being applied. */
    bool color;        /**< map holds a color mapping (map.color) rather than a numeric one (map.numeric). */
    union
    {
        ERInterpolation numeric;
        ERColorInterpolation color;
    } map;
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

void er_anim_pool_limits(ERPoolLimits* limits)
{
    limits->animations = (uint16_t)ERUI_MAX_ANIMATIONS;
    limits->anim_groups = (uint16_t)ERUI_MAX_ANIM_GROUPS;
    limits->group_entries = (uint16_t)ERUI_MAX_GROUP_ENTRIES;
    limits->anim_values = (uint16_t)ERUI_MAX_ANIM_VALUES;
}

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
    uint16_t handle = s_next_handle++;
    if (s_next_handle == 0)
        s_next_handle = 1;
    return handle;
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
 * @brief Returns a node's opacity byte, wherever its type keeps it.
 *
 * @param[in] node  Node to inspect.
 *
 * @return Pointer to the opacity byte, or NULL when the node has none.
 */
static uint8_t* opacity_field(ERNode* node)
{
    if (is_view_node(node))
        return &node->props.view.opacity;
    switch (node->type)
    {
        case ER_NODE_TEXT:
            return &node->props.text.opacity;
        case ER_NODE_IMAGE:
            return &node->props.image.opacity;
        case ER_NODE_TEXT_INPUT:
            return &node->props.text_input.opacity;
        default:
            return NULL;
    }
}

/**
 * @brief Marks a node dirty after one of its animated properties changed.
 *
 * An opacity change on a node that composites its own subtree changes only its blend, so the node's fade
 * cache stays valid. A Text or Image fades at a draw alpha inside whatever subtree holds it, so it, like
 * every other prop, changes content.
 *
 * @param[in,out] node  Node that changed.
 * @param[in]     prop  Property that changed.
 */
static void mark_prop_dirty(ERNode* node, ERAnimProp prop)
{
    if (prop == ER_PROP_OPACITY && (is_view_node(node) || node->type == ER_NODE_TEXT_INPUT))
        er_mark_dirty_upward_opacity(node);
    else
        er_mark_dirty_upward(node);
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
static bool read_numeric_value(ERNode* node, ERAnimProp prop, float* value)
{
    if (!value)
        return false;

    switch (prop)
    {
        case ER_PROP_OPACITY:
        {
            const uint8_t* opacity = opacity_field(node);
            if (!opacity)
                return false;
            *value = (float)*opacity / OPACITY_BYTE_MAX;
            return true;
        }
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
        case ER_PROP_ARC_VALUE:
            if (node->type != ER_NODE_ARC)
                return false;
            *value = node->arc_value;
            return true;
        case ER_PROP_ARC_VALUE_START:
            if (node->type != ER_NODE_ARC)
                return false;
            *value = node->arc_value_start;
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
    const bool had_transform = node->has_transform;
    node->has_transform = (node->tp_translate_x != 0.0f || node->tp_translate_y != 0.0f || node->tp_scale_x != 0.0f
                           || node->tp_scale_y != 0.0f || node->tp_rotate_z != 0.0f || node->tp_rotate_x != 0.0f
                           || node->tp_rotate_y != 0.0f || node->tp_perspective != 0.0f);
    /* The compositor caches prune and cull flags derived from this one, normally refreshed by a layout pass
     * an animation-only frame skips. Nothing moved, so ask for the bounds refresh alone: when a transform
     * appears or goes, and when one turns complex (a rotate or scale joining a translate) while the cached
     * subtree_complex_xf says it holds none. One that stops being complex only leaves that flag set, which
     * costs a cull, never a pixel, until the next layout pass clears it. */
    bool refresh = node->has_transform != had_transform;
#if ERUI_TRANSFORMS_FULL
    if (er_node_has_complex_transform(node) && !node->subtree_complex_xf)
        refresh = true;
#endif
    if (refresh)
        er_request_subtree_bounds_pass();
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
    /* NaN fails every comparison, so a clamp below would pass it on to an integer cast (the opacity byte,
     * or the compositor placing a node by (int)tp_translate_x), where it is undefined. A NaN value is 0,
     * which for a transform component means unset. */
    if (value != value)
        value = 0.0f;

    switch (prop)
    {
        case ER_PROP_OPACITY:
        {
            uint8_t* field = opacity_field(node);
            if (!field)
                return false;
            if (value < 0.0f)
                value = 0.0f;
            if (value > 1.0f)
                value = 1.0f;
            const uint8_t opacity = (uint8_t)(value * OPACITY_BYTE_MAX + 0.5f);
            if (*field == opacity)
                return false;
            *field = opacity;
            return true;
        }
        case ER_PROP_TRANSLATE_X:
            value = er_px_offset(value);
            if (node->tp_translate_x == value)
                return false;
            node->tp_translate_x = value;
            update_has_transform(node);
            return true;
        case ER_PROP_TRANSLATE_Y:
            value = er_px_offset(value);
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
        case ER_PROP_ARC_VALUE:
            return er_arc_apply_value(node, value);
        case ER_PROP_ARC_VALUE_START:
            return er_arc_apply_value_start(node, value);
        default:
            return false;
    }
}

/**
 * @brief Linearly interpolates a color channel.
 *
 * @param[in] from      Start channel value.
 * @param[in] to        Target channel value.
 * @param[in] fraction  Interpolation fraction; outside 0.0-1.0 extrapolates, clamped to the channel range.
 *
 * @return Interpolated channel value.
 */
static uint8_t lerp_channel(uint8_t from, uint8_t to, float fraction)
{
    const float value = (float)from + ((float)to - (float)from) * fraction;
    if (!(value > 0.0f)) /* NaN too: a package's or an app's maths can reach fraction */
        return 0U;
    if (value >= (float)CHANNEL_MAX)
        return CHANNEL_MAX;
    return (uint8_t)(value + 0.5f);
}

/**
 * @brief Linearly interpolates an ARGB8888 color.
 *
 * @param[in] from      Start ARGB8888 color.
 * @param[in] to        Target ARGB8888 color.
 * @param[in] fraction  Interpolation fraction (see lerp_channel).
 *
 * @return Interpolated ARGB8888 color.
 */
static uint32_t lerp_color(uint32_t from, uint32_t to, float fraction)
{
    const uint8_t alpha = lerp_channel((uint8_t)(from >> 24), (uint8_t)(to >> 24), fraction);
    const uint8_t red = lerp_channel((uint8_t)(from >> 16), (uint8_t)(to >> 16), fraction);
    const uint8_t green = lerp_channel((uint8_t)(from >> 8), (uint8_t)(to >> 8), fraction);
    const uint8_t blue = lerp_channel((uint8_t)from, (uint8_t)to, fraction);

    return ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
}

/**
 * @brief Applies a color value to a node property.
 *
 * @param[in,out] node   Target node.
 * @param[in]     prop   Property to update.
 * @param[in]     color  ARGB8888 color.
 *
 * @return true if the stored color actually changed (see apply_numeric_value).
 */
static bool apply_color_value(ERNode* node, ERAnimProp prop, uint32_t color)
{
    if (!node)
        return false;

    uint32_t* field;
    switch (prop)
    {
        case ER_PROP_BACKGROUND_COLOR:
            if (!is_view_node(node))
                return false;
            field = &node->props.view.background_color;
            break;
        case ER_PROP_COLOR:
            if (node->type != ER_NODE_TEXT)
                return false;
            /* The text painter reads 0 as unset and draws white. Any color at alpha 0 draws nothing alike. */
            if (color == 0U)
                color = TEXT_COLOR_TRANSPARENT;
            field = &node->props.text.color;
            break;
        default:
            return false;
    }
    if (*field == color)
        return false;
    *field = color;
    return true;
}

/**
 * @brief Applies an animated value's current float to one binding's node property.
 *
 * @param[in,out] node     Bound node.
 * @param[in]     binding  Binding, with its mapping.
 * @param[in]     current  The value's current float.
 *
 * @return true if the property changed.
 */
static bool apply_binding(ERNode* node, const ERValueBinding* binding, float current)
{
    if (binding->color)
        return apply_color_value(node, binding->prop, er_interpolate_color(current, &binding->map.color));
    if (binding->interpolated)
        current = er_interpolate(current,
                                 binding->map.numeric.input_range,
                                 binding->map.numeric.output_range,
                                 (int)binding->map.numeric.point_count,
                                 binding->map.numeric.extrapolate_left,
                                 binding->map.numeric.extrapolate_right);
    return apply_numeric_value(node, binding->prop, current);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — easing curves
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Evaluates the X component of a cubic bezier at a curve parameter.
 *
 * @param[in] parameter  Curve parameter [0,1].
 * @param[in] x1         First control-point X.
 * @param[in] x2         Second control-point X.
 *
 * @return X coordinate on the curve.
 */
static float bezier_x(float parameter, float x1, float x2)
{
    const float linear = 3.0f * x1;
    const float quadratic = 3.0f * (x2 - x1) - linear;
    const float cubic = 1.0f - linear - quadratic;
    return ((cubic * parameter + quadratic) * parameter + linear) * parameter;
}

/**
 * @brief Evaluates the derivative dX/dt of a cubic bezier at a curve parameter.
 *
 * @param[in] parameter  Curve parameter [0,1].
 * @param[in] x1         First control-point X.
 * @param[in] x2         Second control-point X.
 *
 * @return dX/dt at that parameter.
 */
static float bezier_dx(float parameter, float x1, float x2)
{
    const float linear = 3.0f * x1;
    const float quadratic = 3.0f * (x2 - x1) - linear;
    const float cubic = 1.0f - linear - quadratic;
    return (3.0f * cubic * parameter + 2.0f * quadratic) * parameter + linear;
}

/**
 * @brief Finds the bezier curve parameter for a given normalised time using Newton's method.
 *
 * @param[in] time  Input normalised time [0,1].
 * @param[in] x1    First control-point X.
 * @param[in] x2    Second control-point X.
 *
 * @return The curve parameter such that bezier_x(parameter, x1, x2) ≈ time.
 */
static float bezier_t_for_x(float time, float x1, float x2)
{
    float parameter = time;
    for (int iteration = 0; iteration < BEZIER_NEWTON_ITERATIONS; iteration++)
    {
        const float error = bezier_x(parameter, x1, x2) - time;
        if (error > -BEZIER_NEWTON_EPSILON && error < BEZIER_NEWTON_EPSILON)
            break;
        const float slope = bezier_dx(parameter, x1, x2);
        if (slope < BEZIER_NEWTON_EPSILON)
            break;
        parameter -= error / slope;
        if (parameter < 0.0f)
            parameter = 0.0f;
        if (parameter > 1.0f)
            parameter = 1.0f;
    }
    return parameter;
}

/**
 * @brief Evaluates a cubic bezier easing curve for a given time input.
 *
 * @param[in] time  Normalised time [0,1].
 * @param[in] x1    First control-point X.
 * @param[in] y1    First control-point Y.
 * @param[in] x2    Second control-point X.
 * @param[in] y2    Second control-point Y.
 *
 * @return Easing output Y for the given X.
 */
static float cubic_bezier(float time, float x1, float y1, float x2, float y2)
{
    if (time <= 0.0f)
        return 0.0f;
    if (time >= 1.0f)
        return 1.0f;
    const float parameter = bezier_t_for_x(time, x1, x2);
    const float linear = 3.0f * y1;
    const float quadratic = 3.0f * (y2 - y1) - linear;
    const float cubic = 1.0f - linear - quadratic;
    return ((cubic * parameter + quadratic) * parameter + linear) * parameter;
}

/**
 * @brief Applies an easing curve to a linear progress value.
 *
 * @param[in] progress   Linear progress [0,1].
 * @param[in] easing     Easing function to apply.
 * @param[in] bezier_x1  Bezier X1 (used only for ER_EASE_BEZIER).
 * @param[in] bezier_y1  Bezier Y1 (used only for ER_EASE_BEZIER).
 * @param[in] bezier_x2  Bezier X2 (used only for ER_EASE_BEZIER).
 * @param[in] bezier_y2  Bezier Y2 (used only for ER_EASE_BEZIER).
 *
 * @return Eased progress value.
 */
static float
apply_easing(float progress, ERAnimEasing easing, float bezier_x1, float bezier_y1, float bezier_x2, float bezier_y2)
{
    switch (easing)
    {
        case ER_EASE_LINEAR:
        default:
            return progress;

        case ER_EASE_EASE:
            return cubic_bezier(progress, CURVE_EASE);

        case ER_EASE_EASE_IN:
            return cubic_bezier(progress, CURVE_EASE_IN);

        case ER_EASE_EASE_OUT:
            return cubic_bezier(progress, CURVE_EASE_OUT);

        case ER_EASE_EASE_IN_OUT:
            return cubic_bezier(progress, CURVE_EASE_IN_OUT);

        case ER_EASE_QUAD_IN:
            return progress * progress;

        case ER_EASE_QUAD_OUT:
        {
            const float inverse = 1.0f - progress;
            return 1.0f - inverse * inverse;
        }

        case ER_EASE_QUAD_IN_OUT:
            if (progress < 0.5f)
                return 2.0f * progress * progress;
            else
            {
                const float inverse = 1.0f - progress;
                return 1.0f - 2.0f * inverse * inverse;
            }

        case ER_EASE_CUBIC_IN:
            return progress * progress * progress;

        case ER_EASE_CUBIC_OUT:
        {
            const float inverse = 1.0f - progress;
            return 1.0f - inverse * inverse * inverse;
        }

        case ER_EASE_CUBIC_IN_OUT:
            if (progress < 0.5f)
                return 4.0f * progress * progress * progress;
            else
            {
                const float inverse = progress - 1.0f;
                return 1.0f + 4.0f * inverse * inverse * inverse;
            }

        case ER_EASE_BOUNCE_OUT:
        {
            const float bounce_scale = 7.5625f;
            const float bounce_divisor = 2.75f;
            float bounce_x = progress;
            if (bounce_x < 1.0f / bounce_divisor)
                return bounce_scale * bounce_x * bounce_x;
            else if (bounce_x < 2.0f / bounce_divisor)
            {
                bounce_x -= 1.5f / bounce_divisor;
                return bounce_scale * bounce_x * bounce_x + 0.75f;
            }
            else if (bounce_x < 2.5f / bounce_divisor)
            {
                bounce_x -= 2.25f / bounce_divisor;
                return bounce_scale * bounce_x * bounce_x + 0.9375f;
            }
            else
            {
                bounce_x -= 2.625f / bounce_divisor;
                return bounce_scale * bounce_x * bounce_x + 0.984375f;
            }
        }

        case ER_EASE_ELASTIC_OUT:
        {
            if (progress <= 0.0f)
                return 0.0f;
            if (progress >= 1.0f)
                return 1.0f;
            const float period = 0.3f;
            const float phase_shift = period / 4.0f;
            return (float)pow(2.0, -10.0 * progress) * (float)sin((progress - phase_shift) * ELASTIC_TWO_PI / period)
                   + 1.0f;
        }

        case ER_EASE_BEZIER:
            return cubic_bezier(progress, bezier_x1, bezier_y1, bezier_x2, bezier_y2);
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
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIMATIONS; slot_index++)
    {
        ERAnimation* animation = &s_animations[slot_index];
        if (animation->active && animation->node_tag == node_tag && animation->prop == prop)
        {
            animation->active = false;
            if (animation->on_complete)
                animation->on_complete(false, animation->on_complete_user_data);
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
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIMATIONS; slot_index++)
    {
        if (!s_animations[slot_index].active)
            return &s_animations[slot_index];
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
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIM_GROUPS; slot_index++)
    {
        if (!s_groups[slot_index].active)
            return &s_groups[slot_index];
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
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIM_VALUES; slot_index++)
    {
        if (s_anim_values[slot_index].in_use && s_anim_values[slot_index].handle == handle)
            return &s_anim_values[slot_index];
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
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIMATIONS; slot_index++)
    {
        ERAnimation* animation = &s_animations[slot_index];
        if (animation->active && animation->value_handle == value_handle)
        {
            animation->active = false;
            if (animation->on_complete)
                animation->on_complete(false, animation->on_complete_user_data);
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
 * @param[in] anim_value  Value whose current float is pushed to every active binding.
 */
static void push_to_value_bindings(ERAnimValue* anim_value)
{
    for (int binding_index = 0; binding_index < anim_value->binding_count; binding_index++)
    {
        ERValueBinding* binding = &anim_value->bindings[binding_index];
        if (!binding->active)
            continue;
        ERNode* node = er_get_node(binding->node_tag);
        if (!node)
            continue;
        if (apply_binding(node, binding, anim_value->current))
            mark_prop_dirty(node, binding->prop);
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
    for (int value_index = 0; value_index < ERUI_MAX_ANIM_VALUES; value_index++)
    {
        ERAnimValue* anim_value = &s_anim_values[value_index];
        if (!anim_value->in_use)
            continue;
        for (int binding_index = 0; binding_index < anim_value->binding_count; binding_index++)
        {
            ERValueBinding* binding = &anim_value->bindings[binding_index];
            if (!binding->active || binding->node_tag != node->tag)
                continue;
            (void)apply_binding(node, binding, anim_value->current);
        }
    }
}

void er_anim_unbind_node(uint16_t node_tag)
{
    /*
     * Drop every animated-value binding that targets this node. Called from er_node_destroy so a destroyed
     * node leaves nothing behind: its tag is pushed to the free list and handed to a future node, and a
     * lingering binding would otherwise make the value push its float to that unrelated node — a
     * native-driver animation writing to the wrong element. Mirrors er_anim_reapply_bound's walk; deactivating
     * a binding is just a flag, no callback.
     */
    for (int value_index = 0; value_index < ERUI_MAX_ANIM_VALUES; value_index++)
    {
        ERAnimValue* anim_value = &s_anim_values[value_index];
        if (!anim_value->in_use)
            continue;
        for (int binding_index = 0; binding_index < anim_value->binding_count; binding_index++)
        {
            if (anim_value->bindings[binding_index].active && anim_value->bindings[binding_index].node_tag == node_tag)
                anim_value->bindings[binding_index].active = false;
        }
    }
}

void er_anim_unbind_prop(ERNode* node, ERAnimProp prop)
{
    /*
     * Release ONE property of one node, whatever value happens to drive it. The reconciler calls this when a
     * prop stops being animated (or changes to a different Animated.Value) — it knows the node and the prop,
     * but not which value owns the binding, and er_anim_value_unbind_all() would also drop that value's
     * bindings on every OTHER node it drives. Dropping the pair everywhere it appears is also what recovers a
     * node whose property ended up bound to two values at once: after this call nothing writes it but
     * er_node_set_props.
     */
    if (!node)
        return;
    bool released = false;
    for (int value_index = 0; value_index < ERUI_MAX_ANIM_VALUES; value_index++)
    {
        ERAnimValue* anim_value = &s_anim_values[value_index];
        if (!anim_value->in_use)
            continue;
        for (int binding_index = 0; binding_index < anim_value->binding_count; binding_index++)
        {
            if (anim_value->bindings[binding_index].active && anim_value->bindings[binding_index].node_tag == node->tag
                && anim_value->bindings[binding_index].prop == prop)
            {
                anim_value->bindings[binding_index].active = false;
                released = true;
            }
        }
    }
    /* The next setProps writes the static value back, but skips the repaint when its props are unchanged,
     * which they are when the prop simply stopped being animated. */
    if (released)
        er_mark_dirty_upward(node);
}

/**
 * @brief Deactivates a group and every animation running under it.
 *
 * The one place a group is torn down early, shared by the two callers that need it: er_anim_stop()
 * cancels on the caller's word and reports it, er_anim_cancel_node() cancels because the group's node
 * is being destroyed and must stay silent (see the reasoning there).
 *
 * @param[in,out] group   Group to tear down; must be active.
 * @param[in]     notify  Fire each member's on_complete and then the group's, all with
 *                        `finished = false`. The group is marked inactive before its own callback
 *                        runs, so a handler that starts something new does not see a dead group.
 */
static void deactivate_group(ERAnimGroup* group, bool notify)
{
    const uint8_t slot = (uint8_t)(group - s_groups);
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIMATIONS; slot_index++)
    {
        if (!s_animations[slot_index].active || s_animations[slot_index].group_slot != slot)
            continue;
        s_animations[slot_index].active = false;
        if (notify && s_animations[slot_index].on_complete)
            s_animations[slot_index].on_complete(false, s_animations[slot_index].on_complete_user_data);
    }
    group->active = false;
    if (notify && group->on_complete)
        group->on_complete(false, group->on_complete_user_data);
}

void er_anim_cancel_node(uint16_t node_tag)
{
    /*
     * Stop every animation aimed at this node. Called from er_node_destroy for the same reason as
     * er_anim_unbind_node next to it, and covering the half that one does not: er_anim_unbind_node drops
     * ERAnimValue BINDINGS, while these are the animations keyed directly by node_tag (er_anim_start).
     *
     * er_anim_tick already deactivates an animation whose node has gone (er_get_node returns NULL), but
     * that only holds while the tag is still on the free list. The destroyed tag is recycled by the very
     * next er_node_create, so a node created before the next tick resolves the SAME tag to itself and
     * inherits the animation: an <ActivityIndicator> unmounted mid-spin left its looping ER_PROP_ROTATE_Z
     * behind, and a plain View that took its tag started rotating on its own.
     *
     * No on_complete is fired, here or for a group. Two reasons:
     *   - er_node_destroy runs inside the bridge's subtree teardown, so a callback would re-enter JS
     *     partway through a walk that is deleting nodes. The trampoline's own safety note is about
     *     running inside er_anim_tick, where the tree is not being mutated underneath it.
     *   - An unmounted component does not want its .start() callback. Nothing leaks by staying quiet:
     *     the bridge allocates a completion slot only for VALUE animations (js_anim_value_animate), and
     *     those are skipped below.
     */

    /* Groups first, so a group losing one member does not leave its siblings running against a
     * half-dismantled group. A group choreographing a node that no longer exists cannot finish
     * correctly, so the whole group goes — including one that merely holds the tag in a PENDING entry:
     * a sequence would otherwise reach that entry later and start_anim_internal() would hand it to
     * whatever now owns the tag. Freeing the slot also beats today's outcome, where such a group stalls
     * at a step that never completes and is never reclaimed. */
    for (int group_index = 0; group_index < ERUI_MAX_ANIM_GROUPS; group_index++)
    {
        ERAnimGroup* group = &s_groups[group_index];
        if (!group->active)
            continue;
        for (uint16_t entry_index = 0; entry_index < group->count; entry_index++)
        {
            if (group->entries[entry_index].node_tag == node_tag)
            {
                deactivate_group(group, false);
                break;
            }
        }
    }

    for (int slot_index = 0; slot_index < ERUI_MAX_ANIMATIONS; slot_index++)
    {
        ERAnimation* animation = &s_animations[slot_index];
        /* value_handle guards the ERAnimValue animations, which do not target a node at all. They park
         * node_tag at 0xFFFF so no real tag can collide with them today; the test is here so that
         * sentinel is not the only thing standing between a destroyed tag 0 and every value animation. */
        if (animation->active && animation->value_handle == 0U && animation->node_tag == node_tag)
            animation->active = false;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — spring / decay tick helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Integrates spring physics for delta_ms milliseconds (max ER_SPRING_MAX_STEPS steps).
 *
 * @param[in,out] anim      Animation slot to update.
 * @param[in]     delta_ms  Elapsed time in milliseconds.
 *
 * @return true when the spring has settled.
 */
static bool tick_spring(ERAnimation* anim, uint32_t delta_ms)
{
    uint32_t steps = delta_ms;
    if (steps > ER_SPRING_MAX_STEPS)
        steps = ER_SPRING_MAX_STEPS;

    const float stiffness = anim->spring_stiffness > 0.0f ? anim->spring_stiffness : SPRING_DEFAULT_STIFFNESS;
    const float damping = anim->spring_damping > 0.0f ? anim->spring_damping : SPRING_DEFAULT_DAMPING;
    const float mass = anim->spring_mass > 0.0f ? anim->spring_mass : SPRING_DEFAULT_MASS;

    for (uint32_t step = 0; step < steps; step++)
        er_spring_step(&anim->spring_position, &anim->spring_velocity, stiffness, damping, mass);

    const float displacement = anim->spring_position - 1.0f;
    return (displacement > -SPRING_SETTLE_DISP && displacement < SPRING_SETTLE_DISP)
           && (anim->spring_velocity > -SPRING_SETTLE_VEL && anim->spring_velocity < SPRING_SETTLE_VEL);
}

/**
 * @brief Advances decay physics for delta_ms milliseconds (max ER_SPRING_MAX_STEPS steps).
 *
 * Capping the step count keeps a long frame cheap. A decay caught by one coasts for a few more ticks
 * instead of jumping to where it would have landed, which is how the spring integrator already behaves.
 *
 * @param[in,out] anim      Animation slot to update.
 * @param[in]     delta_ms  Elapsed time in milliseconds.
 *
 * @return true when the velocity has dropped below DECAY_VEL_STOP.
 */
static bool tick_decay(ERAnimation* anim, uint32_t delta_ms)
{
    uint32_t steps = delta_ms;
    if (steps > ER_SPRING_MAX_STEPS)
        steps = ER_SPRING_MAX_STEPS;

    const float deceleration = anim->decay_deceleration > 0.0f ? anim->decay_deceleration : DECAY_DEFAULT_DECELERATION;

    for (uint32_t step = 0; step < steps; step++)
    {
        anim->decay_position += anim->decay_velocity;
        anim->decay_velocity *= deceleration;
    }

    const float velocity = anim->decay_velocity;
    return (velocity > -DECAY_VEL_STOP && velocity < DECAY_VEL_STOP);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — group completion / progression
 ---------------------------------------------------------------------------------------------------------------------*/

/* Forward declaration needed because start_group_entry calls er_anim_start indirectly. */
static ERAnimHandle start_anim_internal(uint16_t node_tag,
                                        ERAnimProp prop,
                                        float value,
                                        const ERAnimConfig* config,
                                        uint8_t group_slot,
                                        uint8_t group_entry);

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
    ERAnimGroup* group = &s_groups[group_slot];
    if (!group->active)
        return;

    if (group->type == GROUP_TYPE_SEQUENCE)
    {
        (void)entry_index;
        group->current++;
        if (group->current < group->count)
        {
            ERGroupEntry* entry = &group->entries[group->current];
            (void)start_anim_internal(
                entry->node_tag, entry->prop, entry->value, &entry->config, group_slot, (uint8_t)group->current);
        }
        else
        {
            group->active = false;
            if (group->on_complete)
                group->on_complete(true, group->on_complete_user_data);
        }
    }
    else /* PARALLEL or STAGGER */
    {
        group->done++;
        if (group->done >= group->count)
        {
            group->active = false;
            if (group->on_complete)
                group->on_complete(true, group->on_complete_user_data);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — core animation start
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Internal animation start that allows group back-references.
 *
 * @param[in] node_tag     Tag of the node to animate.
 * @param[in] prop         Property to animate.
 * @param[in] value        Target value.
 * @param[in] config       Configuration (NULL = immediate snap).
 * @param[in] group_slot   Owner group slot; UINT8_MAX if standalone.
 * @param[in] group_entry  Entry index within the group.
 *
 * @return Handle, or ER_ANIM_HANDLE_INVALID on failure.
 */
static ERAnimHandle start_anim_internal(uint16_t node_tag,
                                        ERAnimProp prop,
                                        float value,
                                        const ERAnimConfig* config,
                                        uint8_t group_slot,
                                        uint8_t group_entry)
{
    ERNode* node = er_get_node(node_tag);
    if (!node || !node->in_use)
        return ER_ANIM_HANDLE_INVALID;

    const ERAnimType type = config ? config->type : ER_ANIM_TIMING;

    cancel_by_tag(node_tag, prop);

    const bool color = is_color_prop(prop);

    /* Immediate-snap path: zero duration timing with no delay. */
    const uint32_t duration_ms = config ? config->duration_ms : 0U;
    const uint32_t delay_ms = config ? config->delay_ms : 0U;

    if (type == ER_ANIM_TIMING && duration_ms == 0U && delay_ms == 0U)
    {
        if (color)
            (void)apply_color_value(node, prop, bits_from_float(value));
        else
            (void)apply_numeric_value(node, prop, value);
        er_mark_dirty_upward(node);

        if (config && config->on_complete)
            config->on_complete(true, config->on_complete_user_data);
        if (group_slot != GROUP_SLOT_NONE)
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
    anim->easing = config ? config->easing : ER_EASE_LINEAR;
    anim->bezier_x1 = config ? config->bezier_x1 : 0.0f;
    anim->bezier_y1 = config ? config->bezier_y1 : 0.0f;
    anim->bezier_x2 = config ? config->bezier_x2 : 1.0f;
    anim->bezier_y2 = config ? config->bezier_y2 : 1.0f;
    anim->delay_ms = delay_ms;
    anim->in_delay = delay_ms > 0U;
    anim->duration_ms = duration_ms;
    anim->loop = config ? config->loop : false;
    anim->loop_reverse = config ? config->loop_reverse : false;
    anim->color_value = color;
    anim->on_complete = config ? config->on_complete : NULL;
    anim->on_complete_user_data = config ? config->on_complete_user_data : NULL;
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
        anim->spring_stiffness = config ? config->stiffness : 0.0f;
        anim->spring_damping = config ? config->damping : 0.0f;
        anim->spring_mass = config ? config->mass : 0.0f;
        /* Initial velocity is supplied as value/s; convert to normalised/s. */
        const float range = anim->to_value - anim->from_value;
        anim->spring_position = 0.0f;
        anim->spring_velocity = (range != 0.0f && config) ? config->velocity / range : 0.0f;
    }
    else if (type == ER_ANIM_DECAY)
    {
        /* velocity is value/ms; convert to normalised/ms relative to the range. */
        const float range = anim->to_value - anim->from_value;
        anim->decay_velocity = (range != 0.0f && config) ? config->velocity / range : 0.0f;
        anim->decay_deceleration =
            (config && config->deceleration > 0.0f) ? config->deceleration : DECAY_DEFAULT_DECELERATION;
    }

    return anim->handle;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

ERAnimHandle er_anim_start(ERNode* node, ERAnimProp prop, float value, const ERAnimConfig* config)
{
    if (!node || !node->in_use)
        return ER_ANIM_HANDLE_INVALID;
    return start_anim_internal(node->tag, prop, value, config, GROUP_SLOT_NONE, 0U);
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

    for (int slot_index = 0; slot_index < ERUI_MAX_ANIMATIONS; slot_index++)
    {
        ERAnimation* animation = &s_animations[slot_index];
        if (animation->active && animation->handle == handle)
        {
            animation->active = false;
            if (animation->on_complete)
                animation->on_complete(false, animation->on_complete_user_data);
            return;
        }
    }

    for (int slot_index = 0; slot_index < ERUI_MAX_ANIM_GROUPS; slot_index++)
    {
        ERAnimGroup* group = &s_groups[slot_index];
        if (group->active && group->handle == handle)
        {
            deactivate_group(group, true); /* cancels every member and reports finished = false */
            return;
        }
    }
}

ERAnimHandle er_anim_sequence(const ERAnimEntry* entries, uint16_t count, ERAnimCompleteFn on_complete, void* user_data)
{
    if (!entries || count == 0)
        return ER_ANIM_HANDLE_INVALID;
    ERAnimGroup* group = alloc_group();
    if (!group)
        return ER_ANIM_HANDLE_INVALID;

    memset(group, 0, sizeof(*group));
    group->active = true;
    group->type = GROUP_TYPE_SEQUENCE;
    group->handle = alloc_handle();
    group->on_complete = on_complete;
    group->on_complete_user_data = user_data;
    group->count = count < ERUI_MAX_GROUP_ENTRIES ? count : ERUI_MAX_GROUP_ENTRIES;

    for (uint16_t entry_index = 0; entry_index < group->count; entry_index++)
    {
        const ERNode* node = entries[entry_index].node;
        group->entries[entry_index].node_tag = node ? node->tag : (uint16_t)ANIM_NO_NODE_TAG;
        group->entries[entry_index].prop = entries[entry_index].prop;
        group->entries[entry_index].value = entries[entry_index].value;
        group->entries[entry_index].config = entries[entry_index].cfg;
        group->entries[entry_index].config.on_complete = NULL;
    }

    const uint8_t slot = (uint8_t)(group - s_groups);

    /* Start the first entry. */
    ERGroupEntry* entry = &group->entries[0];
    (void)start_anim_internal(entry->node_tag, entry->prop, entry->value, &entry->config, slot, 0U);

    return group->handle;
}

ERAnimHandle er_anim_parallel(const ERAnimEntry* entries, uint16_t count, ERAnimCompleteFn on_complete, void* user_data)
{
    if (!entries || count == 0)
        return ER_ANIM_HANDLE_INVALID;
    ERAnimGroup* group = alloc_group();
    if (!group)
        return ER_ANIM_HANDLE_INVALID;

    memset(group, 0, sizeof(*group));
    group->active = true;
    group->type = GROUP_TYPE_PARALLEL;
    group->handle = alloc_handle();
    group->on_complete = on_complete;
    group->on_complete_user_data = user_data;
    group->count = count < ERUI_MAX_GROUP_ENTRIES ? count : ERUI_MAX_GROUP_ENTRIES;

    for (uint16_t entry_index = 0; entry_index < group->count; entry_index++)
    {
        const ERNode* node = entries[entry_index].node;
        group->entries[entry_index].node_tag = node ? node->tag : (uint16_t)ANIM_NO_NODE_TAG;
        group->entries[entry_index].prop = entries[entry_index].prop;
        group->entries[entry_index].value = entries[entry_index].value;
        group->entries[entry_index].config = entries[entry_index].cfg;
        group->entries[entry_index].config.on_complete = NULL;
    }

    const uint8_t slot = (uint8_t)(group - s_groups);

    for (uint16_t entry_index = 0; entry_index < group->count; entry_index++)
    {
        ERGroupEntry* entry = &group->entries[entry_index];
        (void)start_anim_internal(
            entry->node_tag, entry->prop, entry->value, &entry->config, slot, (uint8_t)entry_index);
    }

    return group->handle;
}

ERAnimHandle er_anim_stagger(
    const ERAnimEntry* entries, uint16_t count, uint32_t stagger_ms, ERAnimCompleteFn on_complete, void* user_data)
{
    if (!entries || count == 0)
        return ER_ANIM_HANDLE_INVALID;
    ERAnimGroup* group = alloc_group();
    if (!group)
        return ER_ANIM_HANDLE_INVALID;

    memset(group, 0, sizeof(*group));
    group->active = true;
    group->type = GROUP_TYPE_STAGGER;
    group->handle = alloc_handle();
    group->stagger_ms = stagger_ms;
    group->on_complete = on_complete;
    group->on_complete_user_data = user_data;
    group->count = count < ERUI_MAX_GROUP_ENTRIES ? count : ERUI_MAX_GROUP_ENTRIES;

    for (uint16_t entry_index = 0; entry_index < group->count; entry_index++)
    {
        const ERNode* node = entries[entry_index].node;
        group->entries[entry_index].node_tag = node ? node->tag : (uint16_t)ANIM_NO_NODE_TAG;
        group->entries[entry_index].prop = entries[entry_index].prop;
        group->entries[entry_index].value = entries[entry_index].value;
        group->entries[entry_index].config = entries[entry_index].cfg;
        group->entries[entry_index].config.on_complete = NULL;
        group->entries[entry_index].config.delay_ms += (uint32_t)entry_index * stagger_ms;
    }

    const uint8_t slot = (uint8_t)(group - s_groups);

    for (uint16_t entry_index = 0; entry_index < group->count; entry_index++)
    {
        ERGroupEntry* entry = &group->entries[entry_index];
        (void)start_anim_internal(
            entry->node_tag, entry->prop, entry->value, &entry->config, slot, (uint8_t)entry_index);
    }

    return group->handle;
}

ERAnimValueHandle er_anim_value_create(float initial_value)
{
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIM_VALUES; slot_index++)
    {
        ERAnimValue* anim_value = &s_anim_values[slot_index];
        if (!anim_value->in_use)
        {
            memset(anim_value, 0, sizeof(*anim_value));
            anim_value->in_use = true;
            anim_value->handle = alloc_handle();
            anim_value->current = initial_value;
            return anim_value->handle;
        }
    }
    return ER_ANIM_VALUE_INVALID;
}

/**
 * @brief Releases every binding of a value and marks each bound node dirty.
 *
 * Its next setProps writes the static value back but skips the repaint when the props are unchanged, so
 * the release has to ask for it (as er_anim_unbind_prop does).
 *
 * @param[in,out] anim_value  Value whose bindings to release.
 */
static void release_bindings(ERAnimValue* anim_value)
{
    for (int binding_index = 0; binding_index < anim_value->binding_count; binding_index++)
    {
        if (!anim_value->bindings[binding_index].active)
            continue;
        anim_value->bindings[binding_index].active = false;
        ERNode* node = er_get_node(anim_value->bindings[binding_index].node_tag);
        if (node)
            er_mark_dirty_upward(node);
    }
    anim_value->binding_count = 0;
}

void er_anim_value_destroy(ERAnimValueHandle handle)
{
    if (handle == ER_ANIM_VALUE_INVALID)
        return;
    cancel_value_anim(handle);
    ERAnimValue* anim_value = find_value_slot(handle);
    if (anim_value)
    {
        release_bindings(anim_value);
        anim_value->in_use = false;
    }
}

/**
 * @brief Claims a free binding slot on a value for a node+prop pair.
 *
 * @param[in,out] anim_value  Value to binding; NULL claims nothing.
 * @param[in]     node        Target node.
 * @param[in]     prop        Property to drive.
 *
 * @return The claimed binding, its mapping left for the caller to fill; NULL when there is no value,
 *         the pair is already bound (the reconciler may re-bind on every render, which would otherwise
 *         exhaust the pool), or every slot is taken.
 */
static ERValueBinding* claim_binding(ERAnimValue* anim_value, const ERNode* node, ERAnimProp prop)
{
    if (!anim_value)
        return NULL;
    for (int binding_index = 0; binding_index < anim_value->binding_count; binding_index++)
    {
        if (anim_value->bindings[binding_index].active && anim_value->bindings[binding_index].node_tag == node->tag
            && anim_value->bindings[binding_index].prop == prop)
            return NULL;
    }
    for (int binding_index = 0; binding_index < ERUI_MAX_VALUE_BINDINGS; binding_index++)
    {
        ERValueBinding* binding = &anim_value->bindings[binding_index];
        if (binding->active)
            continue;
        memset(binding, 0, sizeof(*binding));
        binding->node_tag = node->tag;
        binding->prop = prop;
        binding->active = true;
        if ((uint8_t)(binding_index + 1) > anim_value->binding_count)
            anim_value->binding_count = (uint8_t)(binding_index + 1);
        return binding;
    }
    return NULL;
}

void er_anim_value_bind(ERAnimValueHandle handle, ERNode* node, ERAnimProp prop)
{
    /* A plain float is not a color: a color prop is bound through er_anim_value_bind_color. */
    if (handle == ER_ANIM_VALUE_INVALID || !node || is_color_prop(prop))
        return;
    ERAnimValue* anim_value = find_value_slot(handle);
    ERValueBinding* binding = claim_binding(anim_value, node, prop);
    if (!binding)
        return;
    /* Apply at once so the node shows the current value, not its default until the value next changes. */
    (void)apply_binding(node, binding, anim_value->current);
    er_mark_dirty_upward(node);
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

    for (int point_index = 1; point_index <= last; ++point_index)
    {
        if (input <= input_range[point_index])
        {
            const float span = input_range[point_index] - input_range[point_index - 1];
            if (span == 0.0f)
                return output_range[point_index - 1];
            return output_range[point_index - 1]
                   + (output_range[point_index] - output_range[point_index - 1])
                         * ((input - input_range[point_index - 1]) / span);
        }
    }
    return output_range[last];
}

void er_anim_value_bind_interpolated(ERAnimValueHandle handle,
                                     ERNode* node,
                                     ERAnimProp prop,
                                     const ERInterpolation* interpolation)
{
    if (handle == ER_ANIM_VALUE_INVALID || !node || !interpolation || interpolation->point_count < 2u
        || is_color_prop(prop))
        return;
    ERAnimValue* anim_value = find_value_slot(handle);
    ERValueBinding* binding = claim_binding(anim_value, node, prop);
    if (!binding)
        return;
    binding->interpolated = true;
    binding->map.numeric = *interpolation;
    (void)apply_binding(node, binding, anim_value->current);
    er_mark_dirty_upward(node);
}

uint32_t er_interpolate_color(float input, const ERColorInterpolation* interpolation)
{
    if (!interpolation || interpolation->point_count < 2U)
        return 0U;

    const float* input_range = interpolation->input_range;
    const uint32_t* output_range = interpolation->output_range;
    const int last = (int)interpolation->point_count - 1;

    /* NaN maps to the last color, as er_interpolate maps it to the last output. */
    if (input != input)
        return output_range[last];

    /* The segment [segment - 1, segment] the input falls in, or extends past. Only EXTEND continues the slope
     * outside the range; IDENTITY has no color reading, so it holds the endpoint like CLAMP. */
    int segment;
    if (input < input_range[0])
    {
        if (interpolation->extrapolate_left != ER_EXTRAPOLATE_EXTEND)
            return output_range[0];
        segment = 1;
    }
    else if (input > input_range[last])
    {
        if (interpolation->extrapolate_right != ER_EXTRAPOLATE_EXTEND)
            return output_range[last];
        segment = last;
    }
    else
    {
        segment = 1;
        while (segment < last && input > input_range[segment])
            segment++;
    }

    const float span = input_range[segment] - input_range[segment - 1];
    if (span == 0.0f)
        return input > input_range[last] ? output_range[last] : output_range[segment - 1];
    return lerp_color(output_range[segment - 1], output_range[segment], (input - input_range[segment - 1]) / span);
}

void er_anim_value_bind_color(ERAnimValueHandle handle,
                              ERNode* node,
                              ERAnimProp prop,
                              const ERColorInterpolation* interpolation)
{
    if (handle == ER_ANIM_VALUE_INVALID || !node || !interpolation || interpolation->point_count < 2u
        || !is_color_prop(prop))
        return;
    ERAnimValue* anim_value = find_value_slot(handle);
    ERValueBinding* binding = claim_binding(anim_value, node, prop);
    if (!binding)
        return;
    binding->interpolated = true;
    binding->color = true;
    binding->map.color = *interpolation;
    (void)apply_binding(node, binding, anim_value->current);
    er_mark_dirty_upward(node);
}

void er_anim_value_unbind_all(ERAnimValueHandle handle)
{
    ERAnimValue* anim_value = find_value_slot(handle);
    if (anim_value)
        release_bindings(anim_value);
}

ERAnimHandle er_anim_value_animate(ERAnimValueHandle handle, float to_value, const ERAnimConfig* config)
{
    ERAnimValue* anim_value = find_value_slot(handle);
    if (!anim_value)
        return ER_ANIM_HANDLE_INVALID;

    cancel_value_anim(handle);

    const ERAnimType type = config ? config->type : ER_ANIM_TIMING;
    const uint32_t duration_ms = config ? config->duration_ms : 0U;
    const uint32_t delay_ms = config ? config->delay_ms : 0U;

    /* Immediate-snap path: zero-duration timing with no delay. */
    if (type == ER_ANIM_TIMING && duration_ms == 0U && delay_ms == 0U)
    {
        anim_value->current = to_value;
        push_to_value_bindings(anim_value);
        if (config && config->on_complete)
            config->on_complete(true, config->on_complete_user_data);
        return ER_ANIM_HANDLE_INVALID;
    }

    ERAnimation* anim = alloc_slot();
    if (!anim)
        return ER_ANIM_HANDLE_INVALID;

    memset(anim, 0, sizeof(*anim));
    anim->active = true;
    anim->value_handle = handle;
    anim->node_tag = ANIM_NO_NODE_TAG; /* unused sentinel for value animations */
    anim->handle = alloc_handle();
    anim->prop = ER_PROP_OPACITY; /* unused placeholder */
    anim->type = type;
    anim->easing = config ? config->easing : ER_EASE_LINEAR;
    anim->bezier_x1 = config ? config->bezier_x1 : 0.0f;
    anim->bezier_y1 = config ? config->bezier_y1 : 0.0f;
    anim->bezier_x2 = config ? config->bezier_x2 : 1.0f;
    anim->bezier_y2 = config ? config->bezier_y2 : 1.0f;
    anim->delay_ms = delay_ms;
    anim->in_delay = delay_ms > 0U;
    anim->duration_ms = duration_ms;
    anim->loop = config ? config->loop : false;
    anim->loop_reverse = config ? config->loop_reverse : false;
    anim->color_value = false;
    anim->from_value = anim_value->current;
    anim->to_value = to_value;
    anim->on_complete = config ? config->on_complete : NULL;
    anim->on_complete_user_data = config ? config->on_complete_user_data : NULL;
    anim->group_slot = GROUP_SLOT_NONE;
    anim->group_entry = 0U;

    if (type == ER_ANIM_SPRING)
    {
        anim->spring_stiffness = config ? config->stiffness : 0.0f;
        anim->spring_damping = config ? config->damping : 0.0f;
        anim->spring_mass = config ? config->mass : 0.0f;
        const float range = to_value - anim_value->current;
        anim->spring_position = 0.0f;
        anim->spring_velocity = (range != 0.0f && config) ? config->velocity / range : 0.0f;
    }
    else if (type == ER_ANIM_DECAY)
    {
        const float range = to_value - anim_value->current;
        anim->decay_velocity = (range != 0.0f && config) ? config->velocity / range : 0.0f;
        anim->decay_deceleration =
            (config && config->deceleration > 0.0f) ? config->deceleration : DECAY_DEFAULT_DECELERATION;
    }

    return anim->handle;
}

void er_anim_value_set(ERAnimValueHandle handle, float value)
{
    ERAnimValue* anim_value = find_value_slot(handle);
    if (!anim_value)
        return;
    cancel_value_anim(handle);
    anim_value->current = value;
    push_to_value_bindings(anim_value);
}

float er_anim_value_get(ERAnimValueHandle handle)
{
    const ERAnimValue* anim_value = find_value_slot(handle);
    return anim_value ? anim_value->current : 0.0f;
}

void er_anim_tick(uint32_t delta_ms)
{
    for (int slot_index = 0; slot_index < ERUI_MAX_ANIMATIONS; slot_index++)
    {
        ERAnimation* anim = &s_animations[slot_index];
        if (!anim->active)
            continue;

        /* ---- Value-target animation (ERAnimValue — no specific node) ---- */
        if (anim->value_handle != 0U)
        {
            ERAnimValue* anim_value = find_value_slot(anim->value_handle);
            if (!anim_value)
            {
                anim->active = false;
                continue;
            }

            uint32_t value_delta = delta_ms;
            if (anim->in_delay)
            {
                anim->delay_elapsed_ms += delta_ms;
                if (anim->delay_elapsed_ms < anim->delay_ms)
                    continue;
                value_delta = anim->delay_elapsed_ms - anim->delay_ms;
                anim->in_delay = false;
                anim->from_value = anim_value->current;
                if (anim->type == ER_ANIM_SPRING)
                    anim->spring_position = 0.0f;
                else if (anim->type == ER_ANIM_DECAY)
                    anim->decay_position = 0.0f;
            }

            bool value_finished = false;
            float result = anim->from_value;

            if (anim->type == ER_ANIM_TIMING)
            {
                anim->elapsed_ms += value_delta;
                float progress = 1.0f;
                if (anim->duration_ms > 0U && anim->elapsed_ms < anim->duration_ms)
                    progress = (float)anim->elapsed_ms / (float)anim->duration_ms;
                const float eased = apply_easing(
                    progress, anim->easing, anim->bezier_x1, anim->bezier_y1, anim->bezier_x2, anim->bezier_y2);
                result = anim->from_value + (anim->to_value - anim->from_value) * eased;
                if (progress >= 1.0f)
                {
                    if (anim->loop)
                    {
                        anim->elapsed_ms = 0U;
                        if (anim->loop_reverse) /* ping-pong; else restart from the start (continuous) */
                        {
                            const float swapped = anim->from_value;
                            anim->from_value = anim->to_value;
                            anim->to_value = swapped;
                        }
                    }
                    else
                    {
                        value_finished = true;
                    }
                }
            }
            else if (anim->type == ER_ANIM_SPRING)
            {
                const bool settled = tick_spring(anim, value_delta);
                float position = anim->spring_position;
                if (position < SPRING_POSITION_MIN)
                    position = SPRING_POSITION_MIN;
                if (position > SPRING_POSITION_MAX)
                    position = SPRING_POSITION_MAX;
                result = anim->from_value + (anim->to_value - anim->from_value) * position;
                if (settled)
                {
                    result = anim->to_value;
                    value_finished = true;
                }
            }
            else if (anim->type == ER_ANIM_DECAY)
            {
                const bool stopped = tick_decay(anim, value_delta);
                result = anim->from_value + (anim->to_value - anim->from_value) * anim->decay_position;
                if (stopped)
                    value_finished = true;
            }

            anim_value->current = result;
            push_to_value_bindings(anim_value);

            if (value_finished)
            {
                const ERAnimCompleteFn on_complete = anim->on_complete;
                void* user_data = anim->on_complete_user_data;
                const uint8_t group_slot = anim->group_slot;
                const uint8_t group_entry = anim->group_entry;
                anim->active = false;
                if (on_complete)
                    on_complete(true, user_data);
                if (group_slot != GROUP_SLOT_NONE)
                    on_group_entry_complete(group_slot, group_entry);
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
                    anim->spring_position = 0.0f;
                else if (anim->type == ER_ANIM_DECAY)
                    anim->decay_position = 0.0f;
            }
        }

        bool finished = false;

        if (anim->type == ER_ANIM_TIMING)
        {
            anim->elapsed_ms += effective_delta;
            float progress = 1.0f;
            if (anim->duration_ms > 0U && anim->elapsed_ms < anim->duration_ms)
                progress = (float)anim->elapsed_ms / (float)anim->duration_ms;

            const float eased = apply_easing(
                progress, anim->easing, anim->bezier_x1, anim->bezier_y1, anim->bezier_x2, anim->bezier_y2);

            if (anim->color_value)
                (void)apply_color_value(node, anim->prop, lerp_color(anim->from_color, anim->to_color, eased));
            else
                (void)apply_numeric_value(
                    node, anim->prop, anim->from_value + (anim->to_value - anim->from_value) * eased);

            if (progress >= 1.0f)
            {
                if (anim->loop)
                {
                    anim->elapsed_ms = 0U;
                    if (anim->loop_reverse) /* ping-pong; else restart from the start (continuous one-way) */
                    {
                        if (anim->color_value)
                        {
                            const uint32_t swapped = anim->from_color;
                            anim->from_color = anim->to_color;
                            anim->to_color = swapped;
                        }
                        else
                        {
                            const float swapped = anim->from_value;
                            anim->from_value = anim->to_value;
                            anim->to_value = swapped;
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

            /* Clamp spring_position to a reasonable range to prevent runaway. */
            float position = anim->spring_position;
            if (position < SPRING_POSITION_MIN)
                position = SPRING_POSITION_MIN;
            if (position > SPRING_POSITION_MAX)
                position = SPRING_POSITION_MAX;
            (void)apply_numeric_value(
                node, anim->prop, anim->from_value + (anim->to_value - anim->from_value) * position);

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
                node, anim->prop, anim->from_value + (anim->to_value - anim->from_value) * anim->decay_position);

            if (stopped)
                finished = true;
        }

        mark_prop_dirty(node, anim->prop);

        if (finished)
        {
            const ERAnimCompleteFn on_complete = anim->on_complete;
            void* user_data = anim->on_complete_user_data;
            const uint8_t group_slot = anim->group_slot;
            const uint8_t group_entry = anim->group_entry;

            anim->active = false;

            if (on_complete)
                on_complete(true, user_data);
            if (group_slot != GROUP_SLOT_NONE)
                on_group_entry_complete(group_slot, group_entry);
        }
    }
}
