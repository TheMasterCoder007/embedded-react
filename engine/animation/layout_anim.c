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
#include "er_scene.h"
#include "renderer_internal.h"
#include "spring.h"
#include <math.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum number of simultaneous layout animations across all nodes. */
#ifndef ERUI_MAX_LAYOUT_ANIMS
#define ERUI_MAX_LAYOUT_ANIMS 16
#endif

/** @brief Spring considered settled when |displacement from target| < this. */
#define LA_SPRING_SETTLE_DISP 0.005f

/** @brief Spring considered settled when |velocity| < this. */
#define LA_SPRING_SETTLE_VEL 0.05f

/** @brief Default spring stiffness k (matches React Native Animated.spring() default). */
#define LA_SPRING_DEFAULT_STIFFNESS 100.0f

/** @brief Default spring damping c (matches React Native Animated.spring() default). */
#define LA_SPRING_DEFAULT_DAMPING 10.0f

/** @brief Default spring mass m (matches React Native Animated.spring() default). */
#define LA_SPRING_DEFAULT_MASS 1.0f

/** @brief ERLayoutAnim::elapsed_ms holds at this instead of wrapping. */
#define LA_ELAPSED_MAX 0xFFFFU

/** @brief Newton's method on a bezier curve: iteration cap, and the error or slope too small to go on. */
#define LA_BEZIER_NEWTON_ITERATIONS 8
#define LA_BEZIER_NEWTON_EPSILON 0.00001f

/* Control points (x1, y1, x2, y2) of the standard CSS easing curves. */
#define LA_CURVE_EASE 0.25f, 0.1f, 0.25f, 1.0f
#define LA_CURVE_EASE_IN 0.42f, 0.0f, 1.0f, 1.0f
#define LA_CURVE_EASE_OUT 0.0f, 0.0f, 0.58f, 1.0f
#define LA_CURVE_EASE_IN_OUT 0.42f, 0.0f, 0.58f, 1.0f

/** @brief ER_EASE_ELASTIC_OUT: one full wave, in radians. */
#define LA_ELASTIC_TWO_PI 6.283185f

/** @brief A spring's position is held to this range when applied, so a runaway cannot reach the node. */
#define LA_SPRING_POSITION_MIN (-2.0f)
#define LA_SPRING_POSITION_MAX 3.0f

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Active layout animation slot.
 */
typedef struct
{
    bool active;
    uint16_t node_tag;
    ERLayoutRect from;    /**< Display rect when the animation started. */
    ERLayoutRect to;      /**< Target rect (== node->computed). */
    uint16_t elapsed_ms;  /**< Elapsed time for ER_ANIM_TIMING. */
    uint8_t type;         /**< ERAnimType value. */
    uint16_t duration_ms; /**< Total duration for ER_ANIM_TIMING. */
    uint8_t easing;       /**< ERAnimEasing value for ER_ANIM_TIMING. */
    float bezier_x1;      /**< Bezier CP1 X for ER_EASE_BEZIER. */
    float bezier_y1;      /**< Bezier CP1 Y for ER_EASE_BEZIER. */
    float bezier_x2;      /**< Bezier CP2 X for ER_EASE_BEZIER. */
    float bezier_y2;      /**< Bezier CP2 Y for ER_EASE_BEZIER. */
    float stiffness;      /**< Spring stiffness k. */
    float damping;        /**< Spring damping c. */
    float mass;           /**< Spring mass m. */
    float spring_position; /**< Normalised spring position: 0.0 = from, 1.0 = to. */
    float spring_velocity; /**< Spring velocity in normalised units per millisecond. */
} ERLayoutAnim;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERLayoutAnim s_layout_anims[ERUI_MAX_LAYOUT_ANIMS];
static ERLayoutAnimConfig s_pending_config;
static bool s_pending_active;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public — reset
 ---------------------------------------------------------------------------------------------------------------------*/

void er_layout_anim_reset(void)
{
    memset(s_layout_anims, 0, sizeof(s_layout_anims));
    memset(&s_pending_config, 0, sizeof(s_pending_config));
    s_pending_active = false;
}

void er_layout_anim_cancel_node(uint16_t node_tag)
{
    /* Same recycled-tag hazard er_anim_cancel_node() exists for, in this module's own slot table: the
     * tick below deactivates a slot whose node is gone, but only while the tag is still on the free
     * list. A node created before the next tick takes the tag and inherits the interpolation — the
     * destroyed node's position animation would drive it to coordinates it never asked for. */
    for (int slot_index = 0; slot_index < ERUI_MAX_LAYOUT_ANIMS; slot_index++)
    {
        if (s_layout_anims[slot_index].active && s_layout_anims[slot_index].node_tag == node_tag)
            s_layout_anims[slot_index].active = false;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants: Public (preset configs)
 ---------------------------------------------------------------------------------------------------------------------*/

const ERLayoutAnimConfig ER_LAYOUT_ANIM_EASE_IN_EASE_OUT = {
    .type = ER_ANIM_TIMING,
    .duration_ms = ER_ANIM_DEFAULT_DURATION_MS,
    .easing = ER_EASE_EASE_IN_OUT,
};

const ERLayoutAnimConfig ER_LAYOUT_ANIM_LINEAR = {
    .type = ER_ANIM_TIMING,
    .duration_ms = ER_ANIM_DEFAULT_DURATION_MS,
    .easing = ER_EASE_LINEAR,
};

const ERLayoutAnimConfig ER_LAYOUT_ANIM_SPRING = {
    .type = ER_ANIM_SPRING,
    .stiffness = LA_SPRING_DEFAULT_STIFFNESS,
    .damping = LA_SPRING_DEFAULT_DAMPING,
    .mass = LA_SPRING_DEFAULT_MASS,
};

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — easing curves
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Evaluates the X component of a unit cubic bezier at a curve parameter.
 *
 * @param[in] parameter  Curve parameter [0,1].
 * @param[in] x1         First control-point X.
 * @param[in] x2         Second control-point X.
 *
 * @return X on the curve.
 */
static float la_bezier_x(float parameter, float x1, float x2)
{
    const float linear = 3.0f * x1;
    const float quadratic = 3.0f * (x2 - x1) - linear;
    const float cubic = 1.0f - linear - quadratic;
    return ((cubic * parameter + quadratic) * parameter + linear) * parameter;
}

/**
 * @brief Evaluates the derivative dX/dt of a unit cubic bezier at a curve parameter.
 *
 * @param[in] parameter  Curve parameter [0,1].
 * @param[in] x1         First control-point X.
 * @param[in] x2         Second control-point X.
 *
 * @return dX/dt at that parameter.
 */
static float la_bezier_dx(float parameter, float x1, float x2)
{
    const float linear = 3.0f * x1;
    const float quadratic = 3.0f * (x2 - x1) - linear;
    const float cubic = 1.0f - linear - quadratic;
    return (3.0f * cubic * parameter + 2.0f * quadratic) * parameter + linear;
}

/**
 * @brief Solves for the bezier parameter given normalised time via Newton's method.
 *
 * @param[in] time  Input normalised time [0,1].
 * @param[in] x1    First control-point X.
 * @param[in] x2    Second control-point X.
 *
 * @return The curve parameter such that la_bezier_x(parameter, x1, x2) ≈ time.
 */
static float la_bezier_t_for_x(float time, float x1, float x2)
{
    float parameter = time;
    for (int iteration = 0; iteration < LA_BEZIER_NEWTON_ITERATIONS; iteration++)
    {
        const float error = la_bezier_x(parameter, x1, x2) - time;
        if (error > -LA_BEZIER_NEWTON_EPSILON && error < LA_BEZIER_NEWTON_EPSILON)
            break;
        const float slope = la_bezier_dx(parameter, x1, x2);
        if (slope < LA_BEZIER_NEWTON_EPSILON)
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
 * @brief Evaluates a cubic bezier easing curve for a given normalised time.
 *
 * @param[in] time  Normalised time [0,1].
 * @param[in] x1    First control-point X.
 * @param[in] y1    First control-point Y.
 * @param[in] x2    Second control-point X.
 * @param[in] y2    Second control-point Y.
 *
 * @return Easing output Y for the given X.
 */
static float la_cubic_bezier(float time, float x1, float y1, float x2, float y2)
{
    if (time <= 0.0f)
        return 0.0f;
    if (time >= 1.0f)
        return 1.0f;
    const float parameter = la_bezier_t_for_x(time, x1, x2);
    const float linear = 3.0f * y1;
    const float quadratic = 3.0f * (y2 - y1) - linear;
    const float cubic = 1.0f - linear - quadratic;
    return ((cubic * parameter + quadratic) * parameter + linear) * parameter;
}

/**
 * @brief Applies an easing curve to a linear normalised progress value.
 *
 * @param[in] progress   Linear progress [0,1].
 * @param[in] easing     ERAnimEasing enum value.
 * @param[in] bezier_x1  Bezier X1 (only for ER_EASE_BEZIER).
 * @param[in] bezier_y1  Bezier Y1 (only for ER_EASE_BEZIER).
 * @param[in] bezier_x2  Bezier X2 (only for ER_EASE_BEZIER).
 * @param[in] bezier_y2  Bezier Y2 (only for ER_EASE_BEZIER).
 *
 * @return Eased progress value.
 */
static float
la_apply_easing(float progress, ERAnimEasing easing, float bezier_x1, float bezier_y1, float bezier_x2, float bezier_y2)
{
    switch (easing)
    {
        case ER_EASE_LINEAR:
        default:
            return progress;
        case ER_EASE_EASE:
            return la_cubic_bezier(progress, LA_CURVE_EASE);
        case ER_EASE_EASE_IN:
            return la_cubic_bezier(progress, LA_CURVE_EASE_IN);
        case ER_EASE_EASE_OUT:
            return la_cubic_bezier(progress, LA_CURVE_EASE_OUT);
        case ER_EASE_EASE_IN_OUT:
            return la_cubic_bezier(progress, LA_CURVE_EASE_IN_OUT);
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
            return (float)pow(2.0, -10.0 * progress) * (float)sin((progress - phase_shift) * LA_ELASTIC_TWO_PI / period)
                   + 1.0f;
        }
        case ER_EASE_BEZIER:
            return la_cubic_bezier(progress, bezier_x1, bezier_y1, bezier_x2, bezier_y2);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — spring physics
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Integrates spring physics for delta_ms milliseconds and checks for settlement.
 *
 * @param[in,out] layout_anim  Layout animation slot whose spring state is updated.
 * @param[in]     delta_ms     Milliseconds to integrate.
 *
 * @return true when the spring has settled at the target.
 */
static bool la_tick_spring(ERLayoutAnim* layout_anim, uint32_t delta_ms)
{
    uint32_t steps = delta_ms;
    if (steps > ER_SPRING_MAX_STEPS)
        steps = ER_SPRING_MAX_STEPS;

    const float stiffness = layout_anim->stiffness > 0.0f ? layout_anim->stiffness : LA_SPRING_DEFAULT_STIFFNESS;
    const float damping = layout_anim->damping > 0.0f ? layout_anim->damping : LA_SPRING_DEFAULT_DAMPING;
    const float mass = layout_anim->mass > 0.0f ? layout_anim->mass : LA_SPRING_DEFAULT_MASS;

    for (uint32_t step = 0; step < steps; step++)
        er_spring_step(&layout_anim->spring_position, &layout_anim->spring_velocity, stiffness, damping, mass);

    const float displacement = layout_anim->spring_position - 1.0f;
    return (displacement > -LA_SPRING_SETTLE_DISP && displacement < LA_SPRING_SETTLE_DISP)
           && (layout_anim->spring_velocity > -LA_SPRING_SETTLE_VEL
               && layout_anim->spring_velocity < LA_SPRING_SETTLE_VEL);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — slot management
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Finds the active slot for a given node tag, or NULL if none exists.
 *
 * @param[in] tag  Node tag to search for.
 *
 * @return Pointer to the matching slot, or NULL.
 */
static ERLayoutAnim* la_find_slot(uint16_t tag)
{
    for (int slot_index = 0; slot_index < ERUI_MAX_LAYOUT_ANIMS; slot_index++)
    {
        if (s_layout_anims[slot_index].active && s_layout_anims[slot_index].node_tag == tag)
            return &s_layout_anims[slot_index];
    }
    return NULL;
}

/**
 * @brief Allocates a free slot for the given node tag.
 *
 * If an active slot already exists for this tag it is returned (retarget path).
 * Otherwise the first inactive slot is claimed.  Returns NULL when the pool is full.
 *
 * @param[in] tag  Node tag to assign to the slot.
 *
 * @return Pointer to the claimed slot, or NULL if the pool is exhausted.
 */
static ERLayoutAnim* la_alloc_slot(uint16_t tag)
{
    /* Reuse existing slot for the same node. */
    ERLayoutAnim* existing = la_find_slot(tag);
    if (existing)
        return existing;

    for (int slot_index = 0; slot_index < ERUI_MAX_LAYOUT_ANIMS; slot_index++)
    {
        if (!s_layout_anims[slot_index].active)
        {
            s_layout_anims[slot_index].active = true;
            s_layout_anims[slot_index].node_tag = tag;
            return &s_layout_anims[slot_index];
        }
    }
    return NULL;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — rect interpolation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Writes the linearly interpolated layout rect into node->animated.
 *
 * @param[in]     layout_anim  Animation slot providing from/to rects.
 * @param[in]     position     Normalised position [0 = from, 1 = to]; may slightly overshoot.
 * @param[in,out] node         Node whose animated rect is updated.
 */
static void la_apply_pos(const ERLayoutAnim* layout_anim, float position, ERNode* node)
{
    const float from_x = (float)layout_anim->from.x;
    const float from_y = (float)layout_anim->from.y;
    const float from_width = (float)layout_anim->from.w;
    const float from_height = (float)layout_anim->from.h;
    const float to_x = (float)layout_anim->to.x;
    const float to_y = (float)layout_anim->to.y;
    const float to_width = (float)layout_anim->to.w;
    const float to_height = (float)layout_anim->to.h;

    node->animated.x = (int16_t)(from_x + (to_x - from_x) * position);
    node->animated.y = (int16_t)(from_y + (to_y - from_y) * position);
    node->animated.w = (int16_t)(from_width + (to_width - from_width) * position);
    node->animated.h = (int16_t)(from_height + (to_height - from_height) * position);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — post-layout tree walk
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Processes a single node and recurses into its children.
 *
 * @param[in,out] node         Node to process.
 * @param[in]     has_pending  true when a pending layout animation config is active.
 * @param[in]     config       Pending config pointer (only dereferenced when has_pending is true).
 */
static void la_post_layout_node(ERNode* node, bool has_pending, const ERLayoutAnimConfig* config)
{
    if (!node || node->layout.display == ER_DISPLAY_NONE)
        return;

    const ERLayoutRect current = node->computed;
    const ERLayoutRect previous = node->prev_layout;

    /* A node whose last committed computed rect was zero-size is being created, not
     * updated.  Skip animation for new nodes — snap to computed immediately. */
    const bool is_new = (previous.w == 0 && previous.h == 0);

    const bool changed = (current.x != node->animated.x || current.y != node->animated.y
                          || current.w != node->animated.w || current.h != node->animated.h);

    ERLayoutAnim* layout_anim = la_find_slot(node->tag);

    if (layout_anim)
    {
        /* Retarget an in-progress animation: restart from current display position. */
        if (layout_anim->to.x != current.x || layout_anim->to.y != current.y || layout_anim->to.w != current.w
            || layout_anim->to.h != current.h)
        {
            layout_anim->from = node->animated;
            layout_anim->to = current;
            layout_anim->elapsed_ms = 0;
            layout_anim->spring_position = 0.0f;
            layout_anim->spring_velocity = 0.0f;
        }
    }
    else if (has_pending && changed && !is_new)
    {
        /* Start a new layout animation. */
        layout_anim = la_alloc_slot(node->tag);
        if (layout_anim)
        {
            layout_anim->from = node->animated;
            layout_anim->to = current;
            layout_anim->elapsed_ms = 0;
            layout_anim->type = (uint8_t)config->type;
            layout_anim->duration_ms = config->duration_ms;
            layout_anim->easing = (uint8_t)config->easing;
            layout_anim->bezier_x1 = config->bezier_x1;
            layout_anim->bezier_y1 = config->bezier_y1;
            layout_anim->bezier_x2 = config->bezier_x2;
            layout_anim->bezier_y2 = config->bezier_y2;
            layout_anim->stiffness = config->stiffness;
            layout_anim->damping = config->damping;
            layout_anim->mass = config->mass;
            layout_anim->spring_position = 0.0f;
            layout_anim->spring_velocity = 0.0f;
        }
        else
        {
            /* Pool exhausted — snap immediately. */
            node->animated = current;
        }
    }
    else
    {
        /* No animation for this node — snap animated to computed. */
        node->animated = current;
    }

    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        la_post_layout_node(child, has_pending, config);
        child_tag = child->next_sibling_tag;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_layout_anim_configure_next(const ERLayoutAnimConfig* config)
{
    if (!config)
        return;
    s_pending_config = *config;
    s_pending_active = true;
}

void er_layout_anim_post_layout(ERNode* root)
{
    const bool has_pending = s_pending_active;
    la_post_layout_node(root, has_pending, &s_pending_config);
    s_pending_active = false;
}

bool er_layout_anim_has_pending(void)
{
    return s_pending_active;
}

/**
 * @brief Reports whether any layout animation is currently interpolating a node's position.
 *
 * While true, one or more nodes have node->animated drifting away from node->computed, so any
 * cached computed-space geometry (e.g. the compositor's subtree paint bounds) is stale for the
 * animating nodes. Callers use this to fall back to a conservative full traversal.
 *
 * @return true if at least one layout animation slot is active, false otherwise.
 */
bool er_layout_anim_is_active(void)
{
    for (int slot_index = 0; slot_index < ERUI_MAX_LAYOUT_ANIMS; slot_index++)
        if (s_layout_anims[slot_index].active)
            return true;
    return false;
}

void er_layout_anim_tick(uint32_t delta_ms)
{
    for (int slot_index = 0; slot_index < ERUI_MAX_LAYOUT_ANIMS; slot_index++)
    {
        ERLayoutAnim* layout_anim = &s_layout_anims[slot_index];
        if (!layout_anim->active)
            continue;

        ERNode* node = er_get_node(layout_anim->node_tag);
        if (!node)
        {
            layout_anim->active = false;
            continue;
        }

        bool finished = false;

        if (layout_anim->type == (uint8_t)ER_ANIM_TIMING)
        {
            const uint32_t elapsed_new = (uint32_t)layout_anim->elapsed_ms + delta_ms;
            layout_anim->elapsed_ms = (elapsed_new > LA_ELAPSED_MAX) ? LA_ELAPSED_MAX : (uint16_t)elapsed_new;

            float progress = 1.0f;
            if (layout_anim->duration_ms > 0U && layout_anim->elapsed_ms < layout_anim->duration_ms)
                progress = (float)layout_anim->elapsed_ms / (float)layout_anim->duration_ms;

            const float eased = la_apply_easing(progress,
                                                (ERAnimEasing)layout_anim->easing,
                                                layout_anim->bezier_x1,
                                                layout_anim->bezier_y1,
                                                layout_anim->bezier_x2,
                                                layout_anim->bezier_y2);
            la_apply_pos(layout_anim, eased, node);

            if (progress >= 1.0f)
                finished = true;
        }
        else /* ER_ANIM_SPRING */
        {
            const bool settled = la_tick_spring(layout_anim, delta_ms);

            float position = layout_anim->spring_position;
            if (position < LA_SPRING_POSITION_MIN)
                position = LA_SPRING_POSITION_MIN;
            if (position > LA_SPRING_POSITION_MAX)
                position = LA_SPRING_POSITION_MAX;
            la_apply_pos(layout_anim, position, node);

            if (settled)
                finished = true;
        }

        er_mark_dirty_upward(node);

        if (finished)
        {
            node->animated = layout_anim->to;
            layout_anim->active = false;
        }
    }
}
