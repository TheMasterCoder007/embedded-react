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

/** @brief Maximum spring integration steps per tick to bound CPU cost. */
#define LA_SPRING_MAX_STEPS 200

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
    float spring_pos;     /**< Normalised spring position: 0.0 = from, 1.0 = to. */
    float spring_vel;     /**< Spring velocity in normalised units per millisecond. */
} ERLayoutAnim;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERLayoutAnim s_la[ERUI_MAX_LAYOUT_ANIMS];
static ERLayoutAnimConfig s_pending_cfg;
static bool s_pending_active;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public — reset
 ---------------------------------------------------------------------------------------------------------------------*/

void er_layout_anim_reset(void)
{
    memset(s_la, 0, sizeof(s_la));
    memset(&s_pending_cfg, 0, sizeof(s_pending_cfg));
    s_pending_active = false;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants: Public (preset configs)
 ---------------------------------------------------------------------------------------------------------------------*/

const ERLayoutAnimConfig ER_LAYOUT_ANIM_EASE_IN_EASE_OUT = {
    .type = ER_ANIM_TIMING,
    .duration_ms = 300U,
    .easing = ER_EASE_EASE_IN_OUT,
};

const ERLayoutAnimConfig ER_LAYOUT_ANIM_LINEAR = {
    .type = ER_ANIM_TIMING,
    .duration_ms = 300U,
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
 * @brief Evaluates the X component of a unit cubic bezier at parameter t.
 *
 * @param[in] t   Curve parameter [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] x2  Second control-point X.
 *
 * @return X on the curve.
 */
static float la_bezier_x(float t, float x1, float x2)
{
    const float c = 3.0f * x1;
    const float b = 3.0f * (x2 - x1) - c;
    const float a = 1.0f - c - b;
    return ((a * t + b) * t + c) * t;
}

/**
 * @brief Evaluates the derivative dX/dt of a unit cubic bezier at parameter t.
 *
 * @param[in] t   Curve parameter [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] x2  Second control-point X.
 *
 * @return dX/dt at t.
 */
static float la_bezier_dx(float t, float x1, float x2)
{
    const float c = 3.0f * x1;
    const float b = 3.0f * (x2 - x1) - c;
    const float a = 1.0f - c - b;
    return (3.0f * a * t + 2.0f * b) * t + c;
}

/**
 * @brief Solves for the bezier parameter t given normalised time x via Newton's method.
 *
 * @param[in] x   Input normalised time [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] x2  Second control-point X.
 *
 * @return Bezier parameter t such that la_bezier_x(t, x1, x2) ≈ x.
 */
static float la_bezier_t_for_x(float x, float x1, float x2)
{
    float t = x;
    for (int i = 0; i < 8; i++)
    {
        const float dx = la_bezier_x(t, x1, x2) - x;
        if (dx > -0.00001f && dx < 0.00001f)
            break;
        const float d = la_bezier_dx(t, x1, x2);
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
 * @brief Evaluates a cubic bezier easing curve for a given normalised time.
 *
 * @param[in] x   Normalised time [0,1].
 * @param[in] x1  First control-point X.
 * @param[in] y1  First control-point Y.
 * @param[in] x2  Second control-point X.
 * @param[in] y2  Second control-point Y.
 *
 * @return Easing output Y for the given X.
 */
static float la_cubic_bezier(float x, float x1, float y1, float x2, float y2)
{
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        return 1.0f;
    const float t = la_bezier_t_for_x(x, x1, x2);
    const float cy = 3.0f * y1;
    const float by = 3.0f * (y2 - y1) - cy;
    const float ay = 1.0f - cy - by;
    return ((ay * t + by) * t + cy) * t;
}

/**
 * @brief Applies an easing curve to a linear normalised progress value.
 *
 * @param[in] t     Linear progress [0,1].
 * @param[in] ease  ERAnimEasing enum value.
 * @param[in] bx1   Bezier X1 (only for ER_EASE_BEZIER).
 * @param[in] by1   Bezier Y1 (only for ER_EASE_BEZIER).
 * @param[in] bx2   Bezier X2 (only for ER_EASE_BEZIER).
 * @param[in] by2   Bezier Y2 (only for ER_EASE_BEZIER).
 *
 * @return Eased progress value.
 */
static float la_apply_easing(float t, ERAnimEasing ease, float bx1, float by1, float bx2, float by2)
{
    switch (ease)
    {
        case ER_EASE_LINEAR:
        default:
            return t;
        case ER_EASE_EASE:
            return la_cubic_bezier(t, 0.25f, 0.1f, 0.25f, 1.0f);
        case ER_EASE_EASE_IN:
            return la_cubic_bezier(t, 0.42f, 0.0f, 1.0f, 1.0f);
        case ER_EASE_EASE_OUT:
            return la_cubic_bezier(t, 0.0f, 0.0f, 0.58f, 1.0f);
        case ER_EASE_EASE_IN_OUT:
            return la_cubic_bezier(t, 0.42f, 0.0f, 0.58f, 1.0f);
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
            return la_cubic_bezier(t, bx1, by1, bx2, by2);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — spring physics
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Integrates one millisecond of spring physics toward the normalised target 1.0.
 *
 * @param[in,out] pos        Current normalised spring position.
 * @param[in,out] vel        Current normalised velocity.
 * @param[in]     stiffness  Spring constant k.
 * @param[in]     damping    Damping coefficient c.
 * @param[in]     mass       Spring mass m.
 */
static void la_spring_step(float* pos, float* vel, float stiffness, float damping, float mass)
{
    const float dt = 0.001f;
    const float disp = *pos - 1.0f;
    const float accel = (-stiffness * disp - damping * (*vel)) / mass;
    *vel += accel * dt;
    *pos += (*vel) * dt;
}

/**
 * @brief Integrates spring physics for delta_ms milliseconds and checks for settlement.
 *
 * @param[in,out] la        Layout animation slot whose spring state is updated.
 * @param[in]     delta_ms  Milliseconds to integrate.
 *
 * @return true when the spring has settled at the target.
 */
static bool la_tick_spring(ERLayoutAnim* la, uint32_t delta_ms)
{
    uint32_t steps = delta_ms;
    if (steps > (uint32_t)LA_SPRING_MAX_STEPS)
        steps = (uint32_t)LA_SPRING_MAX_STEPS;

    const float k = la->stiffness > 0.0f ? la->stiffness : LA_SPRING_DEFAULT_STIFFNESS;
    const float c = la->damping > 0.0f ? la->damping : LA_SPRING_DEFAULT_DAMPING;
    const float m = la->mass > 0.0f ? la->mass : LA_SPRING_DEFAULT_MASS;

    for (uint32_t i = 0; i < steps; i++)
        la_spring_step(&la->spring_pos, &la->spring_vel, k, c, m);

    const float disp = la->spring_pos - 1.0f;
    return (disp > -LA_SPRING_SETTLE_DISP && disp < LA_SPRING_SETTLE_DISP)
           && (la->spring_vel > -LA_SPRING_SETTLE_VEL && la->spring_vel < LA_SPRING_SETTLE_VEL);
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
    for (int i = 0; i < ERUI_MAX_LAYOUT_ANIMS; i++)
    {
        if (s_la[i].active && s_la[i].node_tag == tag)
            return &s_la[i];
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

    for (int i = 0; i < ERUI_MAX_LAYOUT_ANIMS; i++)
    {
        if (!s_la[i].active)
        {
            s_la[i].active = true;
            s_la[i].node_tag = tag;
            return &s_la[i];
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
 * @param[in]     la   Animation slot providing from/to rects.
 * @param[in]     pos  Normalised position [0 = from, 1 = to]; may slightly overshoot.
 * @param[in,out] n    Node whose animated rect is updated.
 */
static void la_apply_pos(const ERLayoutAnim* la, float pos, ERNode* n)
{
    const float fx = (float)la->from.x;
    const float fy = (float)la->from.y;
    const float fw = (float)la->from.w;
    const float fh = (float)la->from.h;
    const float tx = (float)la->to.x;
    const float ty = (float)la->to.y;
    const float tw = (float)la->to.w;
    const float th = (float)la->to.h;

    n->animated.x = (int16_t)(fx + (tx - fx) * pos);
    n->animated.y = (int16_t)(fy + (ty - fy) * pos);
    n->animated.w = (int16_t)(fw + (tw - fw) * pos);
    n->animated.h = (int16_t)(fh + (th - fh) * pos);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — post-layout tree walk
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Processes a single node and recurses into its children.
 *
 * @param[in,out] n           Node to process.
 * @param[in]     has_pending true when a pending layout animation config is active.
 * @param[in]     cfg         Pending config pointer (only dereferenced when has_pending is true).
 */
static void la_post_layout_node(ERNode* n, bool has_pending, const ERLayoutAnimConfig* cfg)
{
    if (!n || n->layout.display == ER_DISPLAY_NONE)
        return;

    const ERLayoutRect cur = n->computed;
    const ERLayoutRect prev = n->prev_computed;

    /* A node whose last committed computed rect was zero-size is being created, not
     * updated.  Skip animation for new nodes — snap to computed immediately. */
    const bool is_new = (prev.w == 0 && prev.h == 0);

    const bool changed =
        (cur.x != n->animated.x || cur.y != n->animated.y || cur.w != n->animated.w || cur.h != n->animated.h);

    ERLayoutAnim* la = la_find_slot(n->tag);

    if (la)
    {
        /* Retarget an in-progress animation: restart from current display position. */
        if (la->to.x != cur.x || la->to.y != cur.y || la->to.w != cur.w || la->to.h != cur.h)
        {
            la->from = n->animated;
            la->to = cur;
            la->elapsed_ms = 0;
            la->spring_pos = 0.0f;
            la->spring_vel = 0.0f;
        }
    }
    else if (has_pending && changed && !is_new)
    {
        /* Start a new layout animation. */
        la = la_alloc_slot(n->tag);
        if (la)
        {
            la->from = n->animated;
            la->to = cur;
            la->elapsed_ms = 0;
            la->type = (uint8_t)cfg->type;
            la->duration_ms = cfg->duration_ms;
            la->easing = (uint8_t)cfg->easing;
            la->bezier_x1 = cfg->bezier_x1;
            la->bezier_y1 = cfg->bezier_y1;
            la->bezier_x2 = cfg->bezier_x2;
            la->bezier_y2 = cfg->bezier_y2;
            la->stiffness = cfg->stiffness;
            la->damping = cfg->damping;
            la->mass = cfg->mass;
            la->spring_pos = 0.0f;
            la->spring_vel = 0.0f;
        }
        else
        {
            /* Pool exhausted — snap immediately. */
            n->animated = cur;
        }
    }
    else
    {
        /* No animation for this node — snap animated to computed. */
        n->animated = cur;
    }

    uint16_t child_tag = n->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        la_post_layout_node(child, has_pending, cfg);
        child_tag = child->next_sibling_tag;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_layout_anim_configure_next(const ERLayoutAnimConfig* cfg)
{
    if (!cfg)
        return;
    s_pending_cfg = *cfg;
    s_pending_active = true;
}

void er_layout_anim_post_layout(ERNode* root)
{
    const bool has_pending = s_pending_active;
    la_post_layout_node(root, has_pending, &s_pending_cfg);
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
    for (int i = 0; i < ERUI_MAX_LAYOUT_ANIMS; i++)
        if (s_la[i].active)
            return true;
    return false;
}

void er_layout_anim_tick(uint32_t delta_ms)
{
    for (int i = 0; i < ERUI_MAX_LAYOUT_ANIMS; i++)
    {
        ERLayoutAnim* la = &s_la[i];
        if (!la->active)
            continue;

        ERNode* n = er_get_node(la->node_tag);
        if (!n)
        {
            la->active = false;
            continue;
        }

        bool finished = false;

        if (la->type == (uint8_t)ER_ANIM_TIMING)
        {
            const uint32_t elapsed_new = (uint32_t)la->elapsed_ms + delta_ms;
            la->elapsed_ms = (elapsed_new > 0xFFFFU) ? 0xFFFFU : (uint16_t)elapsed_new;

            float t = 1.0f;
            if (la->duration_ms > 0U && la->elapsed_ms < la->duration_ms)
                t = (float)la->elapsed_ms / (float)la->duration_ms;

            const float et = la_apply_easing(
                t, (ERAnimEasing)la->easing, la->bezier_x1, la->bezier_y1, la->bezier_x2, la->bezier_y2);
            la_apply_pos(la, et, n);

            if (t >= 1.0f)
                finished = true;
        }
        else /* ER_ANIM_SPRING */
        {
            const bool settled = la_tick_spring(la, delta_ms);

            float pos = la->spring_pos;
            if (pos < -2.0f)
                pos = -2.0f;
            if (pos > 3.0f)
                pos = 3.0f;
            la_apply_pos(la, pos, n);

            if (settled)
                finished = true;
        }

        er_mark_dirty_upward(n);

        if (finished)
        {
            n->animated = la->to;
            la->active = false;
        }
    }
}
