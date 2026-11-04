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
#include "native_renderer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Records the last fill color emitted by the renderer.
 */
typedef struct
{
    uint32_t last_fill_color;
} RenderCounts;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill callback used to check render order.
 *
 * @param[in] argb    Fill color.
 * @param[in] x       Destination X.
 * @param[in] y       Destination Y.
 * @param[in] width   Fill width.
 * @param[in] handle  Fill height.
 * @param[in] ctx     Pointer to RenderCounts.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int handle, void* ctx)
{
    RenderCounts* counts = ctx;
    (void)x;
    (void)y;
    (void)width;
    (void)handle;
    counts->last_fill_color = argb;
}

/**
 * @brief Backend copy callback unused by this test.
 *
 * @param[in] source  Source buffer.
 * @param[in] stride  Source stride.
 * @param[in] x       Destination X.
 * @param[in] y       Destination Y.
 * @param[in] width   Width.
 * @param[in] handle  Height.
 * @param[in] ctx     Opaque context.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int handle, void* ctx)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)handle;
    (void)ctx;
}

/**
 * @brief Backend blend callback unused by this test.
 *
 * @param[in] source  Source buffer.
 * @param[in] stride  Source stride.
 * @param[in] alpha   Global alpha.
 * @param[in] x       Destination X.
 * @param[in] y       Destination Y.
 * @param[in] width   Width.
 * @param[in] handle  Height.
 * @param[in] ctx     Opaque context.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int handle, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)handle;
    (void)ctx;
}

/**
 * @brief Packs a uint32_t into a float without numeric conversion.
 *
 * @param[in] bits  Raw bits to store in the float.
 *
 * @return Float carrying bits.
 */
static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/**
 * @brief Returns an ERProps struct with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return ERProps with layout defaults initialized.
 */
static ERProps props_default(void)
{
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] message  Human-readable failure description.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — completion callback helper
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Completion flag set by the test completion callback.
 */
static int s_complete_count = 0;
static bool s_complete_finished = false;

/**
 * @brief Completion callback that records how many times it was called and whether it was finished.
 *
 * @param[in] finished   true if the animation completed normally.
 * @param[in] user_data  Unused.
 */
static void on_complete(bool finished, void* user_data)
{
    (void)user_data;
    s_complete_count++;
    s_complete_finished = finished;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point exercising the animation engine.
 *
 * Tests covered:
 *   - Timing: linear interpolation, dirty flag, cancellation, zero-duration snap.
 *   - Easing: quad-in produces a value lower than linear at t=0.5.
 *   - Spring: settles at the target within a reasonable time.
 *   - Decay: velocity decreases each tick; an over-long frame integrates a bounded number of steps.
 *   - Delay: animation value unchanged until delay expires.
 *   - Completion callback: fires with finished=true on natural end.
 *   - Sequence: three animations run in order.
 *   - Parallel: two animations run simultaneously.
 *   - Stagger: animations start with the configured inter-entry delay.
 *   - er_anim_stop: cancels an active animation and fires finished=false.
 *   - zIndex-order render: animated lower-z node does not paint over higher-z sibling.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    embedded_renderer_set_backend(NULL);

    /* -----------------------------------------------------------------------
     * TIMING: basic color interpolation and dirty flag
     * ---------------------------------------------------------------------- */
    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps props = props_default();
    props.width = 100;
    props.height = 40;
    props.background_color = 0xFFFF0000U;
    props.opacity = OPACITY_OPAQUE;
    er_node_set_props(card, &props);

    ERAnimConfig config = {0};
    config.type = ER_ANIM_TIMING;
    config.duration_ms = 100U;

    er_anim_start(card, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF00FF00U), &config);
    embedded_renderer_tick(50U);
    if (card->props.view.background_color != 0xFF808000U)
        return fail("timing: background color did not interpolate at half duration");
    if (!card->dirty)
        return fail("timing: animated node was not marked dirty");

    embedded_renderer_tick(50U);
    if (card->props.view.background_color != 0xFF00FF00U)
        return fail("timing: background color did not finish at target");

    /* -----------------------------------------------------------------------
     * TIMING: cancellation stops the animation
     * ---------------------------------------------------------------------- */
    er_anim_start(card, ER_PROP_OPACITY, 0.0f, &config);
    embedded_renderer_tick(25U);
    er_anim_cancel(card, ER_PROP_OPACITY);
    const uint8_t cancelled_opacity = card->props.view.opacity;
    embedded_renderer_tick(100U);
    if (card->props.view.opacity != cancelled_opacity)
        return fail("cancel: cancelled opacity animation continued ticking");

    /* -----------------------------------------------------------------------
     * TIMING: zero-duration snaps immediately
     * ---------------------------------------------------------------------- */
    config.duration_ms = 0U;
    er_anim_start(card, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF0000FFU), &config);
    if (card->props.view.background_color != 0xFF0000FFU)
        return fail("zero-duration: animation did not apply immediately");

    /* -----------------------------------------------------------------------
     * EASING: quad-in must produce a value below linear at the midpoint
     * ---------------------------------------------------------------------- */
    {
        ERNode* quad_node = er_node_create(ER_NODE_VIEW);
        ERProps quad_props = props_default();
        quad_props.width = 10;
        quad_props.height = 10;
        quad_props.opacity = 0U;
        er_node_set_props(quad_node, &quad_props);

        ERAnimConfig quad_config = {0};
        quad_config.type = ER_ANIM_TIMING;
        quad_config.duration_ms = 100U;
        quad_config.easing = ER_EASE_QUAD_IN;
        er_anim_start(quad_node, ER_PROP_OPACITY, 1.0f, &quad_config);
        embedded_renderer_tick(50U);

        /* Quad-in at t=0.5: f(0.5)=0.25  →  opacity ≈ 64.
         * Linear at t=0.5 would give opacity ≈ 128. */
        if (quad_node->props.view.opacity >= 100U)
            return fail("easing quad-in: midpoint value should be well below linear");

        er_node_destroy(quad_node);
    }

    /* -----------------------------------------------------------------------
     * EASING: ease-out must produce a value above linear at the midpoint
     * ---------------------------------------------------------------------- */
    {
        ERNode* ease_out_node = er_node_create(ER_NODE_VIEW);
        ERProps ease_out_props = props_default();
        ease_out_props.width = 10;
        ease_out_props.height = 10;
        ease_out_props.opacity = 0U;
        er_node_set_props(ease_out_node, &ease_out_props);

        ERAnimConfig ease_out_config = {0};
        ease_out_config.type = ER_ANIM_TIMING;
        ease_out_config.duration_ms = 100U;
        ease_out_config.easing = ER_EASE_EASE_OUT;
        er_anim_start(ease_out_node, ER_PROP_OPACITY, 1.0f, &ease_out_config);
        embedded_renderer_tick(50U);

        /* ease-out is faster at the beginning → midpoint value > linear (128). */
        if (ease_out_node->props.view.opacity <= 128U)
            return fail("easing ease-out: midpoint value should be above linear");

        er_node_destroy(ease_out_node);
    }

    /* -----------------------------------------------------------------------
     * SPRING: settles at the target within 3 seconds
     * ---------------------------------------------------------------------- */
    {
        ERNode* spring_node = er_node_create(ER_NODE_VIEW);
        ERProps spring_props = props_default();
        spring_props.width = 10;
        spring_props.height = 10;
        spring_props.opacity = 0U;
        er_node_set_props(spring_node, &spring_props);

        ERAnimConfig spring_config = {0};
        spring_config.type = ER_ANIM_SPRING;
        spring_config.stiffness = 100.0f;
        spring_config.damping = 10.0f;
        spring_config.mass = 1.0f;
        er_anim_start(spring_node, ER_PROP_OPACITY, 1.0f, &spring_config);

        /* Tick for up to 3 seconds; expect the spring to settle. */
        for (int tick = 0; tick < 300; tick++)
            embedded_renderer_tick(10U);

        /* Opacity should be 255 (fully opaque) after the spring settles. */
        if (spring_node->props.view.opacity < 250U)
            return fail("spring: did not settle at target opacity within 3 seconds");

        er_node_destroy(spring_node);
    }

    /* -----------------------------------------------------------------------
     * DECAY: velocity decreases over time
     * ---------------------------------------------------------------------- */
    {
        ERNode* decay_node = er_node_create(ER_NODE_VIEW);
        ERProps decay_props = props_default();
        decay_props.width = 10;
        decay_props.height = 10;
        decay_props.opacity = 0U;
        er_node_set_props(decay_node, &decay_props);

        ERAnimConfig decay_config = {0};
        decay_config.type = ER_ANIM_DECAY;
        decay_config.velocity = 0.1f;       /* 0.1 normalised/ms — moves toward to_value initially */
        decay_config.deceleration = 0.990f; /* aggressive deceleration for a short test */

        er_anim_start(decay_node, ER_PROP_OPACITY, 1.0f, &decay_config);

        embedded_renderer_tick(50U);
        const uint8_t mid_opacity = decay_node->props.view.opacity;
        embedded_renderer_tick(500U);
        const uint8_t late_opacity = decay_node->props.view.opacity;

        /* After 500ms extra the position must not have grown by much more than the first 50ms. */
        const int delta_mid = (int)mid_opacity;
        const int delta_late = (int)late_opacity - delta_mid;
        if (delta_late > delta_mid * 3)
            return fail("decay: velocity did not decelerate — position grew unexpectedly");

        er_node_destroy(decay_node);
    }

    /* -----------------------------------------------------------------------
     * DECAY: an over-long frame integrates a bounded number of steps
     * ---------------------------------------------------------------------- */
    {
        ERNode* bounded_node = er_node_create(ER_NODE_VIEW);
        ERProps bounded_props = props_default();
        bounded_props.width = 10;
        bounded_props.height = 10;
        bounded_props.opacity = 0U;
        er_node_set_props(bounded_node, &bounded_props);

        s_complete_count = 0;
        s_complete_finished = false;

        ERAnimConfig bounded_config = {0};
        bounded_config.type = ER_ANIM_DECAY;
        bounded_config.velocity =
            0.1f; /* ~3.4s at the default deceleration before it falls below DECAY_VELOCITY_STOP */
        bounded_config.on_complete = on_complete;

        er_anim_start(bounded_node, ER_PROP_OPACITY, 1.0f, &bounded_config);

        /* An hour in one frame must not buy an hour of physics — the decay keeps coasting instead. */
        embedded_renderer_tick(3600000U);
        if (s_complete_count != 0)
            return fail("decay: an over-long frame ran the decay to completion instead of capping its steps");

        /* The cap must not stall it either: it still settles over the frames that follow. */
        for (int frame_index = 0; frame_index < 40 && s_complete_count == 0; frame_index++)
            embedded_renderer_tick(200U);
        if (s_complete_count != 1)
            return fail("decay: did not settle in the frames after a capped one");

        er_node_destroy(bounded_node);
    }

    /* -----------------------------------------------------------------------
     * DELAY: animation value must not change until delay expires
     * ---------------------------------------------------------------------- */
    {
        ERNode* delay_node = er_node_create(ER_NODE_VIEW);
        ERProps delay_props = props_default();
        delay_props.width = 10;
        delay_props.height = 10;
        delay_props.opacity = 0U;
        er_node_set_props(delay_node, &delay_props);

        ERAnimConfig delay_config = {0};
        delay_config.type = ER_ANIM_TIMING;
        delay_config.duration_ms = 100U;
        delay_config.delay_ms = 200U;

        er_anim_start(delay_node, ER_PROP_OPACITY, 1.0f, &delay_config);

        embedded_renderer_tick(100U); /* within delay window */
        if (delay_node->props.view.opacity != 0U)
            return fail("delay: opacity changed before delay expired");

        embedded_renderer_tick(200U); /* past the delay; animation should be running */
        embedded_renderer_tick(100U); /* complete the animation duration */
        if (delay_node->props.view.opacity < 240U)
            return fail("delay: animation did not complete after delay + duration");

        er_node_destroy(delay_node);
    }

    /* -----------------------------------------------------------------------
     * LOOP DIRECTION: a looping timing animation RESTARTS by default (a continuous
     * one-way loop, e.g. a spinner's 0→360° rotation) and only ping-pongs when
     * loop_reverse is set.
     * ---------------------------------------------------------------------- */
    {
        ERNode* spin = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = 10;
        root_props.height = 10;
        er_node_set_props(spin, &root_props);

        ERAnimConfig restart_config = {0};
        restart_config.type = ER_ANIM_TIMING;
        restart_config.duration_ms = 100U;
        restart_config.loop = true; /* loop_reverse defaults false → continuous restart */
        er_anim_start(spin, ER_PROP_ROTATE_Z, 360.0f, &restart_config);

        embedded_renderer_tick(100U); /* end of cycle 1 (reaches 360, then restarts at 0) */
        embedded_renderer_tick(25U);  /* into cycle 2: continuous → ~90, reversed would be ~270 */
        if (spin->transform_rotate_z > 180.0f)
            return fail("loop: default loop ping-ponged instead of restarting continuously");
        er_node_destroy(spin);

        ERNode* yoyo_node = er_node_create(ER_NODE_VIEW);
        er_node_set_props(yoyo_node, &root_props);
        ERAnimConfig yoyo_config = {0};
        yoyo_config.type = ER_ANIM_TIMING;
        yoyo_config.duration_ms = 100U;
        yoyo_config.loop = true;
        yoyo_config.loop_reverse = true; /* ping-pong */
        er_anim_start(yoyo_node, ER_PROP_ROTATE_Z, 360.0f, &yoyo_config);

        embedded_renderer_tick(100U); /* peak ~360 */
        embedded_renderer_tick(25U);  /* reversing → ~270 (heading back down), not ~90 */
        if (yoyo_node->transform_rotate_z < 180.0f)
            return fail("loop_reverse: did not ping-pong back down");
        er_node_destroy(yoyo_node);
    }

    /* -----------------------------------------------------------------------
     * COMPLETION CALLBACK: fires with finished=true at end of timing animation
     * ---------------------------------------------------------------------- */
    {
        ERNode* complete_node = er_node_create(ER_NODE_VIEW);
        ERProps complete_props = props_default();
        complete_props.width = 10;
        complete_props.height = 10;
        complete_props.opacity = 0U;
        er_node_set_props(complete_node, &complete_props);

        s_complete_count = 0;
        s_complete_finished = false;

        ERAnimConfig complete_config = {0};
        complete_config.type = ER_ANIM_TIMING;
        complete_config.duration_ms = 100U;
        complete_config.on_complete = on_complete;

        er_anim_start(complete_node, ER_PROP_OPACITY, 1.0f, &complete_config);
        embedded_renderer_tick(100U);

        if (s_complete_count != 1)
            return fail("completion: on_complete not called exactly once at end");
        if (!s_complete_finished)
            return fail("completion: on_complete called with finished=false on natural end");

        er_node_destroy(complete_node);
    }

    /* -----------------------------------------------------------------------
     * COMPLETION CALLBACK: er_anim_stop fires finished=false
     * ---------------------------------------------------------------------- */
    {
        ERNode* stop_node = er_node_create(ER_NODE_VIEW);
        ERProps stop_props = props_default();
        stop_props.width = 10;
        stop_props.height = 10;
        stop_props.opacity = 0U;
        er_node_set_props(stop_node, &stop_props);

        s_complete_count = 0;
        s_complete_finished = true; /* will be overwritten */

        ERAnimConfig stop_config = {0};
        stop_config.type = ER_ANIM_TIMING;
        stop_config.duration_ms = 500U;
        stop_config.on_complete = on_complete;

        ERAnimHandle handle = er_anim_start(stop_node, ER_PROP_OPACITY, 1.0f, &stop_config);
        embedded_renderer_tick(100U);
        er_anim_stop(handle);

        if (s_complete_count != 1)
            return fail("er_anim_stop: on_complete not called after stop");
        if (s_complete_finished)
            return fail("er_anim_stop: on_complete should have finished=false");

        er_node_destroy(stop_node);
    }

    /* -----------------------------------------------------------------------
     * SEQUENCE: three color animations fire one after another
     * ---------------------------------------------------------------------- */
    {
        ERNode* first_node = er_node_create(ER_NODE_VIEW);
        ERNode* second_node = er_node_create(ER_NODE_VIEW);
        ERNode* third_node = er_node_create(ER_NODE_VIEW);

        ERProps sequence_props = props_default();
        sequence_props.width = 10;
        sequence_props.height = 10;
        sequence_props.background_color = 0xFF000000U;
        er_node_set_props(first_node, &sequence_props);
        er_node_set_props(second_node, &sequence_props);
        er_node_set_props(third_node, &sequence_props);

        ERAnimEntry entries[3];
        memset(entries, 0, sizeof(entries));

        entries[0].node = first_node;
        entries[0].prop = ER_PROP_BACKGROUND_COLOR;
        entries[0].value = float_from_bits(0xFFFF0000U);
        entries[0].cfg.type = ER_ANIM_TIMING;
        entries[0].cfg.duration_ms = 100U;

        entries[1].node = second_node;
        entries[1].prop = ER_PROP_BACKGROUND_COLOR;
        entries[1].value = float_from_bits(0xFF00FF00U);
        entries[1].cfg.type = ER_ANIM_TIMING;
        entries[1].cfg.duration_ms = 100U;

        entries[2].node = third_node;
        entries[2].prop = ER_PROP_BACKGROUND_COLOR;
        entries[2].value = float_from_bits(0xFF0000FFU);
        entries[2].cfg.type = ER_ANIM_TIMING;
        entries[2].cfg.duration_ms = 100U;

        s_complete_count = 0;
        s_complete_finished = false;

        er_anim_sequence(entries, 3, on_complete, NULL);

        /* After 100ms: first animation done, second should be running. */
        embedded_renderer_tick(100U);
        if (first_node->props.view.background_color != 0xFFFF0000U)
            return fail("sequence: entry 0 did not reach target at t=100ms");
        if (second_node->props.view.background_color == 0xFF00FF00U)
            return fail("sequence: entry 1 finished before entry 0 was done");
        if (s_complete_count != 0)
            return fail("sequence: group completed too early");

        /* After another 100ms: second done. */
        embedded_renderer_tick(100U);
        if (second_node->props.view.background_color != 0xFF00FF00U)
            return fail("sequence: entry 1 did not reach target at t=200ms");

        /* After another 100ms: third done, group completed. */
        embedded_renderer_tick(100U);
        if (third_node->props.view.background_color != 0xFF0000FFU)
            return fail("sequence: entry 2 did not reach target at t=300ms");
        if (s_complete_count != 1)
            return fail("sequence: group on_complete not called after last entry");
        if (!s_complete_finished)
            return fail("sequence: group on_complete called with finished=false");

        er_node_destroy(first_node);
        er_node_destroy(second_node);
        er_node_destroy(third_node);
    }

    /* -----------------------------------------------------------------------
     * PARALLEL: two animations start and finish simultaneously
     * ---------------------------------------------------------------------- */
    {
        ERNode* parallel_a = er_node_create(ER_NODE_VIEW);
        ERNode* parallel_b = er_node_create(ER_NODE_VIEW);

        ERProps parallel_props = props_default();
        parallel_props.width = 10;
        parallel_props.height = 10;
        parallel_props.opacity = 0U;
        er_node_set_props(parallel_a, &parallel_props);
        er_node_set_props(parallel_b, &parallel_props);

        ERAnimEntry entries[2];
        memset(entries, 0, sizeof(entries));

        entries[0].node = parallel_a;
        entries[0].prop = ER_PROP_OPACITY;
        entries[0].value = 1.0f;
        entries[0].cfg.type = ER_ANIM_TIMING;
        entries[0].cfg.duration_ms = 100U;

        entries[1].node = parallel_b;
        entries[1].prop = ER_PROP_OPACITY;
        entries[1].value = 1.0f;
        entries[1].cfg.type = ER_ANIM_TIMING;
        entries[1].cfg.duration_ms = 150U;

        s_complete_count = 0;

        er_anim_parallel(entries, 2, on_complete, NULL);

        embedded_renderer_tick(100U);
        if (parallel_a->props.view.opacity < 250U)
            return fail("parallel: entry 0 (100ms) not done at t=100ms");
        if (s_complete_count != 0)
            return fail("parallel: group completed before longer entry finished");

        embedded_renderer_tick(50U);
        if (parallel_b->props.view.opacity < 250U)
            return fail("parallel: entry 1 (150ms) not done at t=150ms");
        if (s_complete_count != 1)
            return fail("parallel: group on_complete not called after all entries done");

        er_node_destroy(parallel_a);
        er_node_destroy(parallel_b);
    }

    /* -----------------------------------------------------------------------
     * STAGGER: second animation starts after stagger_ms delay
     * ---------------------------------------------------------------------- */
    {
        ERNode* stagger_a = er_node_create(ER_NODE_VIEW);
        ERNode* stagger_b = er_node_create(ER_NODE_VIEW);

        ERProps stagger_props = props_default();
        stagger_props.width = 10;
        stagger_props.height = 10;
        stagger_props.opacity = 0U;
        er_node_set_props(stagger_a, &stagger_props);
        er_node_set_props(stagger_b, &stagger_props);

        ERAnimEntry entries[2];
        memset(entries, 0, sizeof(entries));

        entries[0].node = stagger_a;
        entries[0].prop = ER_PROP_OPACITY;
        entries[0].value = 1.0f;
        entries[0].cfg.type = ER_ANIM_TIMING;
        entries[0].cfg.duration_ms = 100U;

        entries[1].node = stagger_b;
        entries[1].prop = ER_PROP_OPACITY;
        entries[1].value = 1.0f;
        entries[1].cfg.type = ER_ANIM_TIMING;
        entries[1].cfg.duration_ms = 100U;

        /* stagger 50ms: entry 0 starts at 0ms, entry 1 at 50ms. */
        er_anim_stagger(entries, 2, 50U, NULL, NULL);

        /* At t=100ms: entry 0 done (0-100ms), entry 1 halfway (50-150ms). */
        embedded_renderer_tick(100U);
        if (stagger_a->props.view.opacity < 250U)
            return fail("stagger: entry 0 not done at t=100ms");
        if (stagger_b->props.view.opacity >= 250U)
            return fail("stagger: entry 1 should not be done at t=100ms (starts at 50ms)");
        if (stagger_b->props.view.opacity == 0U)
            return fail("stagger: entry 1 should have started by t=100ms");

        er_node_destroy(stagger_a);
        er_node_destroy(stagger_b);
    }

    /* -----------------------------------------------------------------------
     * RENDER ORDER: animated lower-z node must not paint over higher-z sibling
     * ---------------------------------------------------------------------- */
    RenderCounts counts = {0};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &counts};
    embedded_renderer_set_backend(&backend);

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    props = props_default();
    props.width = 120;
    props.height = 80;
    props.background_color = 0xFF000000U;
    er_node_set_props(root, &props);
    er_tree_set_root(root);

    ERNode* low = er_node_create(ER_NODE_VIEW);
    props = props_default();
    props.position = ER_POS_ABSOLUTE;
    props.left = 0;
    props.top = 0;
    props.width = 80;
    props.height = 60;
    props.background_color = 0xFFFF0000U;
    props.z_index = 0;
    er_node_set_props(low, &props);

    ERNode* high = er_node_create(ER_NODE_VIEW);
    props = props_default();
    props.position = ER_POS_ABSOLUTE;
    props.left = 20;
    props.top = 10;
    props.width = 80;
    props.height = 60;
    props.background_color = 0xFF0000FFU;
    props.z_index = 10;
    er_node_set_props(high, &props);

    er_tree_append_child(root, low);
    er_tree_append_child(root, high);
    er_commit();

    config.duration_ms = 100U;
    config.easing = ER_EASE_LINEAR;
    er_anim_start(low, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF00FF00U), &config);
    embedded_renderer_tick(50U);
    er_commit();

    if (counts.last_fill_color != 0xFF0000FFU)
        return fail("render order: animated lower zIndex node rendered over higher sibling");

    /* -----------------------------------------------------------------------
     * ANIMATED VALUE: one value drives multiple node-property bindings
     * ---------------------------------------------------------------------- */
    {
        /* Create two nodes; both will receive translateX updates from one value. */
        ERNode* value_node_a = er_node_create(ER_NODE_VIEW);
        ERNode* value_node_b = er_node_create(ER_NODE_VIEW);

        ERProps value_props = props_default();
        value_props.width = 10;
        value_props.height = 10;
        er_node_set_props(value_node_a, &value_props);
        er_node_set_props(value_node_b, &value_props);

        /* Create the shared value at 0 and bind both nodes. */
        ERAnimValueHandle value = er_anim_value_create(0.0f);
        if (value == ER_ANIM_VALUE_INVALID)
            return fail("anim_value: er_anim_value_create returned INVALID");

        er_anim_value_bind(value, value_node_a, ER_PROP_TRANSLATE_X);
        er_anim_value_bind(value, value_node_b, ER_PROP_TRANSLATE_X);

        /* Immediate snap to 50. */
        er_anim_value_set(value, 50.0f);
        if (value_node_a->transform_translate_x < 49.9f || value_node_a->transform_translate_x > 50.1f)
            return fail("anim_value: er_anim_value_set did not propagate to first binding");
        if (value_node_b->transform_translate_x < 49.9f || value_node_b->transform_translate_x > 50.1f)
            return fail("anim_value: er_anim_value_set did not propagate to second binding");
        if (er_anim_value_get(value) < 49.9f || er_anim_value_get(value) > 50.1f)
            return fail("anim_value: er_anim_value_get did not return snapped value");

        /* Animate back to 0 over 100ms; verify midpoint drives both nodes. */
        ERAnimConfig value_config = {0};
        value_config.type = ER_ANIM_TIMING;
        value_config.duration_ms = 100U;
        value_config.easing = ER_EASE_LINEAR;
        er_anim_value_animate(value, 0.0f, &value_config);

        embedded_renderer_tick(50U);
        /* At the midpoint both nodes should be near 25. */
        if (value_node_a->transform_translate_x > 30.0f || value_node_a->transform_translate_x < 20.0f)
            return fail("anim_value: first binding not at midpoint during animation");
        if (value_node_b->transform_translate_x > 30.0f || value_node_b->transform_translate_x < 20.0f)
            return fail("anim_value: second binding not at midpoint during animation");

        embedded_renderer_tick(50U);
        /* Animation complete; both should be at the target. */
        if (value_node_a->transform_translate_x > 1.0f)
            return fail("anim_value: first binding did not reach target at end");
        if (value_node_b->transform_translate_x > 1.0f)
            return fail("anim_value: second binding did not reach target at end");

        /* Duplicate bind is silently ignored — binding_count must not grow past 2. */
        er_anim_value_bind(value, value_node_a, ER_PROP_TRANSLATE_X);
        er_anim_value_set(value, 10.0f);
        if (value_node_a->transform_translate_x < 9.9f || value_node_a->transform_translate_x > 10.1f)
            return fail("anim_value: duplicate bind guard broke propagation");

        er_anim_value_destroy(value);
        er_node_destroy(value_node_a);
        er_node_destroy(value_node_b);
    }

    /* -----------------------------------------------------------------------
     * COLOR BINDINGS: a value drives backgroundColor and color only through a color mapping; a plain
     * float bound to either is refused rather than written as a color.
     * ---------------------------------------------------------------------- */
    {
        ERNode* color_view = er_node_create(ER_NODE_VIEW);
        ERNode* color_text = er_node_create(ER_NODE_TEXT);
        ERProps color_props = props_default();
        color_props.background_color = 0xFF123456U;
        er_node_set_props(color_view, &color_props);
        color_props.color = 0xFF123456U;
        er_node_set_props(color_text, &color_props);

        ERAnimValueHandle color_value = er_anim_value_create(0.0f);
        er_anim_value_bind(color_value, color_view, ER_PROP_BACKGROUND_COLOR);
        const ERInterpolation numeric_mapping = {
            {0.0f, 1.0f}, {0.0f, 1.0f}, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP};
        er_anim_value_bind_interpolated(color_value, color_text, ER_PROP_COLOR, &numeric_mapping);
        er_anim_value_set(color_value, 1.0f);
        if (color_view->props.view.background_color != 0xFF123456U || color_text->props.text.color != 0xFF123456U)
            return fail("color bind: a plain float bound to a color prop should be refused");

        const ERColorInterpolation color_interpolation = {
            {0.0f, 1.0f}, {0xFF000000U, 0xFFFFFFFFU}, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP};
        er_anim_value_bind_color(color_value, color_view, ER_PROP_BACKGROUND_COLOR, &color_interpolation);
        er_anim_value_bind_color(color_value, color_text, ER_PROP_COLOR, &color_interpolation);
        if (color_view->props.view.background_color != 0xFFFFFFFFU || color_text->props.text.color != 0xFFFFFFFFU)
            return fail("color bind: binding should apply the current value at once");

        ERAnimConfig color_config = {0};
        color_config.type = ER_ANIM_TIMING;
        color_config.duration_ms = 100U;
        er_anim_value_animate(color_value, 0.0f, &color_config);
        embedded_renderer_tick(50U);
        if (color_view->props.view.background_color != 0xFF808080U || color_text->props.text.color != 0xFF808080U)
            return fail("color bind: animating the value should interpolate both colors");

        /* setProps writes the static color; the binding puts the animated one back. */
        er_node_set_props(color_view, &color_props);
        if (color_view->props.view.background_color != 0xFF808080U)
            return fail("color bind: setProps should not clobber an animated color");

        er_anim_value_destroy(color_value);
        er_node_destroy(color_view);
        er_node_destroy(color_text);
    }

    /* -----------------------------------------------------------------------
     * OPACITY ON TEXT AND IMAGE: both keep an opacity of their own, static and animated.
     * ---------------------------------------------------------------------- */
    {
        ERNode* opacity_text = er_node_create(ER_NODE_TEXT);
        ERNode* opacity_image = er_node_create(ER_NODE_IMAGE);
        if (opacity_text->props.text.opacity != 255U || opacity_image->props.image.opacity != 255U)
            return fail("text/image opacity: a fresh node should be opaque");
        ERProps opacity_props = props_default();
        opacity_props.opacity = 64U;
        er_node_set_props(opacity_text, &opacity_props);
        er_node_set_props(opacity_image, &opacity_props);
        if (opacity_text->props.text.opacity != 64U || opacity_image->props.image.opacity != 64U)
            return fail("text/image opacity: setProps should store a static opacity");

        ERAnimValueHandle opacity_value = er_anim_value_create(1.0f);
        er_anim_value_bind(opacity_value, opacity_text, ER_PROP_OPACITY);
        er_anim_value_bind(opacity_value, opacity_image, ER_PROP_OPACITY);
        if (opacity_text->props.text.opacity != 255U || opacity_image->props.image.opacity != 255U)
            return fail("text/image opacity: binding should apply the current value at once");
        er_anim_value_set(opacity_value, 0.5f);
        if (opacity_text->props.text.opacity != 128U || opacity_image->props.image.opacity != 128U)
            return fail("text/image opacity: an animated value should drive the opacity");

        er_anim_value_destroy(opacity_value);
        er_node_destroy(opacity_text);
        er_node_destroy(opacity_image);

        /* A TextInput keeps an explicit 0, so releasing an animated opacity can restore a hidden one. */
        ERNode* opacity_text_input = er_node_create(ER_NODE_TEXT_INPUT);
        if (opacity_text_input->props.text_input.opacity != 255U)
            return fail("text input opacity: a fresh node should be opaque");
        ERAnimValueHandle text_input_opacity = er_anim_value_create(1.0f);
        er_anim_value_bind(text_input_opacity, opacity_text_input, ER_PROP_OPACITY);
        er_anim_unbind_prop(opacity_text_input, ER_PROP_OPACITY);
        opacity_props.opacity = 0U;
        er_node_set_props(opacity_text_input, &opacity_props);
        if (opacity_text_input->props.text_input.opacity != 0U)
            return fail("text input opacity: a static opacity of 0 should stay 0");
        er_anim_value_destroy(text_input_opacity);
        er_node_destroy(opacity_text_input);
    }

    /* -----------------------------------------------------------------------
     * NAN AND OUT-OF-RANGE VALUES: NaN fails every comparison, so a clamp alone would pass it on to the
     * opacity byte's cast, or to the compositor's (int)tp_translate_x, both undefined in C. A NaN value is
     * 0 (for a transform component, unset), and a translate is clamped to what an int holds.
     * ---------------------------------------------------------------------- */
    {
        ERNode* nan_node = er_node_create(ER_NODE_VIEW);
        ERProps nan_props = props_default();
        nan_props.width = 10;
        nan_props.height = 10;
        er_node_set_props(nan_node, &nan_props);

        ERAnimValueHandle translate_value = er_anim_value_create(5.0f);
        ERAnimValueHandle scale_value = er_anim_value_create(2.0f);
        ERAnimValueHandle opacity_value = er_anim_value_create(1.0f);
        if (translate_value == ER_ANIM_VALUE_INVALID || scale_value == ER_ANIM_VALUE_INVALID
            || opacity_value == ER_ANIM_VALUE_INVALID)
            return fail("nan values: er_anim_value_create returned INVALID");
        er_anim_value_bind(translate_value, nan_node, ER_PROP_TRANSLATE_X);
        er_anim_value_bind(scale_value, nan_node, ER_PROP_SCALE_X);
        er_anim_value_bind(opacity_value, nan_node, ER_PROP_OPACITY);

        er_anim_value_set(translate_value, NAN);
        er_anim_value_set(scale_value, NAN);
        er_anim_value_set(opacity_value, NAN);
        if (nan_node->transform_translate_x != 0.0f)
            return fail("nan values: a NaN translate should be 0");
        if (nan_node->transform_scale_x != 0.0f)
            return fail("nan values: a NaN scale should be 0 (unset)");
        if (nan_node->props.view.opacity != 0U)
            return fail("nan values: a NaN opacity should be fully transparent");

        er_anim_value_set(translate_value, 1e10f);
        if (nan_node->transform_translate_x != 32767.0f)
            return fail("nan values: a translate past the int range should clamp to 32767");
        er_anim_value_set(translate_value, -1e10f);
        if (nan_node->transform_translate_x != -32767.0f)
            return fail("nan values: a translate below the int range should clamp to -32767");

        er_anim_value_destroy(translate_value);
        er_anim_value_destroy(scale_value);
        er_anim_value_destroy(opacity_value);
        er_node_destroy(nan_node);
    }

    /* -----------------------------------------------------------------------
     * STALE BINDING ON DESTROY: a binding to a destroyed node must not survive
     * to drive a DIFFERENT node that later reuses the freed tag. The node pool
     * recycles tags via a free list, so without clearing bindings in
     * er_node_destroy a value would push its float onto whatever new node lands
     * on the old tag — the corruption a hot-reload soft swap (tear down + rebuild
     * the tree in a live context) would otherwise hit. (Regression.)
     * ---------------------------------------------------------------------- */
    {
        ERAnimValueHandle binding_value = er_anim_value_create(0.0f);
        if (binding_value == ER_ANIM_VALUE_INVALID)
            return fail("stale_bind: er_anim_value_create returned INVALID");

        ERNode* gone = er_node_create(ER_NODE_VIEW);
        ERProps gone_props = props_default();
        gone_props.width = 10;
        gone_props.height = 10;
        er_node_set_props(gone, &gone_props);
        const uint16_t recycled_tag = gone->tag;

        er_anim_value_bind(binding_value, gone, ER_PROP_TRANSLATE_X);
        er_node_destroy(gone); /* must drop the binding to `gone` */

        /* The pool hands the freed tag back to the next node (LIFO free list). */
        ERNode* fresh = er_node_create(ER_NODE_VIEW);
        if (fresh->tag != recycled_tag)
            return fail("stale_bind: precondition — freed tag was not recycled");
        ERProps fresh_props = props_default();
        fresh_props.width = 10;
        fresh_props.height = 10;
        er_node_set_props(fresh, &fresh_props);

        /* Drive the value. A surviving binding would push 99 onto `fresh`. */
        er_anim_value_set(binding_value, 99.0f);
        if (fresh->transform_translate_x > 0.1f || fresh->transform_translate_x < -0.1f)
            return fail("stale_bind: binding to a destroyed node drove the node that reused its tag");

        er_anim_value_destroy(binding_value);
        er_node_destroy(fresh);
    }

    /* -----------------------------------------------------------------------
     * STALE ANIM: the other half of STALE_BIND above. er_anim_unbind_node()
     * covered ERAnimValue bindings; animations started against a node tag
     * (er_anim_start) went uncancelled, so a destroyed node handed its
     * animation to whatever took its tag.
     *
     * er_anim_tick() does drop an animation whose node has gone, but only
     * while the tag is still on the free list. The next er_node_create()
     * recycles it, so a node built before the next tick resolves the SAME tag
     * to itself and inherits the animation. (Regression.)
     * ---------------------------------------------------------------------- */
    {
        ERNode* doomed = er_node_create(ER_NODE_VIEW);
        ERProps doomed_props = props_default();
        doomed_props.width = 10;
        doomed_props.height = 10;
        er_node_set_props(doomed, &doomed_props);
        const uint16_t recycled_tag = doomed->tag;

        ERAnimConfig animation_config = {0};
        animation_config.type = ER_ANIM_TIMING;
        animation_config.duration_ms = 1000U;
        er_anim_start(doomed, ER_PROP_TRANSLATE_X, 500.0f, &animation_config);

        /* Destroyed mid-flight, and — the point of the case — with no tick in
         * between, which is the only window where the tag is still claimable. */
        er_node_destroy(doomed);

        ERNode* heir = er_node_create(ER_NODE_VIEW);
        if (heir->tag != recycled_tag)
            return fail("stale_anim: precondition — freed tag was not recycled");
        ERProps heir_props = props_default();
        heir_props.width = 10;
        heir_props.height = 10;
        er_node_set_props(heir, &heir_props);

        embedded_renderer_tick(200U);
        if (heir->transform_translate_x > 0.1f || heir->transform_translate_x < -0.1f)
            return fail("stale_anim: an animation on a destroyed node drove the node that reused its tag");

        er_node_destroy(heir);
    }

    /* -----------------------------------------------------------------------
     * STALE SPIN: the case that surfaced the bug above, and the one that hits
     * it without anybody calling er_anim_start at all.
     *
     * er_node_create() starts a looping ER_PROP_ROTATE_Z for EVERY
     * ActivityIndicator, so an indicator that unmounts while spinning always
     * leaves one behind. Recycled onto a plain View, that loop turned it into
     * a rotating View nobody asked for: tp_rotate_z climbed and has_transform
     * flipped to true, which then dragged the view onto the transform paths.
     * (Regression.)
     * ---------------------------------------------------------------------- */
    {
        ERNode* spinner = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
        ERProps spinner_props = props_default();
        spinner_props.width = 40;
        spinner_props.height = 40;
        /* props_default() zeroes `animating`, which er_node_set_props reads as
         * "stop" and cancels the spin — the case would then pass vacuously. */
        spinner_props.animating = 1U;
        er_node_set_props(spinner, &spinner_props);
        const uint16_t spin_tag = spinner->tag;

        embedded_renderer_tick(100U);
        if (spinner->transform_rotate_z <= 0.0f)
            return fail("stale_spin: precondition — the indicator is not actually spinning");

        er_node_destroy(spinner);

        ERNode* heir = er_node_create(ER_NODE_VIEW);
        if (heir->tag != spin_tag)
            return fail("stale_spin: precondition — freed tag was not recycled");
        ERProps heir_props = props_default();
        heir_props.width = 10;
        heir_props.height = 10;
        er_node_set_props(heir, &heir_props);

        embedded_renderer_tick(100U);
        if (heir->transform_rotate_z != 0.0f)
            return fail("stale_spin: a destroyed indicator's spin drove the node that reused its tag");
        if (heir->has_transform)
            return fail("stale_spin: the inherited spin gave a plain view a transform");

        er_node_destroy(heir);
    }

    /* -----------------------------------------------------------------------
     * STALE GROUP: a group is torn down when one of its nodes is destroyed —
     * including a SEQUENCE holding the tag in an entry that has not started.
     *
     * Cancelling only the running animation would leave the sequence to reach
     * that entry later and start it against whatever now owns the tag. The
     * whole group goes instead: it cannot finish correctly without the node,
     * and freeing the slot beats stalling forever at a step that never
     * completes. Callbacks stay silent — er_node_destroy runs inside subtree
     * teardown, which is no place to re-enter a completion handler.
     * ---------------------------------------------------------------------- */
    {
        ERNode* first = er_node_create(ER_NODE_VIEW);
        ERNode* later = er_node_create(ER_NODE_VIEW);
        ERProps group_props = props_default();
        group_props.width = 10;
        group_props.height = 10;
        er_node_set_props(first, &group_props);
        er_node_set_props(later, &group_props);
        const uint16_t later_tag = later->tag;

        ERAnimEntry entries[2] = {0};
        entries[0].node = first;
        entries[0].prop = ER_PROP_TRANSLATE_X;
        entries[0].value = 50.0f;
        entries[0].cfg.type = ER_ANIM_TIMING;
        entries[0].cfg.duration_ms = 100U;
        entries[1].node = later; /* never starts: destroyed while entry 0 runs */
        entries[1].prop = ER_PROP_TRANSLATE_Y;
        entries[1].value = 500.0f;
        entries[1].cfg.type = ER_ANIM_TIMING;
        entries[1].cfg.duration_ms = 100U;

        s_complete_count = 0;
        if (er_anim_sequence(entries, 2U, on_complete, NULL) == ER_ANIM_HANDLE_INVALID)
            return fail("stale_group: er_anim_sequence returned INVALID");

        embedded_renderer_tick(50U); /* entry 0 mid-flight, entry 1 still pending */
        er_node_destroy(later);

        ERNode* heir = er_node_create(ER_NODE_VIEW);
        if (heir->tag != later_tag)
            return fail("stale_group: precondition — freed tag was not recycled");
        er_node_set_props(heir, &group_props);

        const float first_x = first->transform_translate_x;
        embedded_renderer_tick(200U); /* long enough for entry 0 to have ended and entry 1 to start */

        if (heir->transform_translate_y > 0.1f || heir->transform_translate_y < -0.1f)
            return fail("stale_group: a pending sequence entry ran against the node that reused its tag");
        if (first->transform_translate_x != first_x)
            return fail("stale_group: the group kept animating after one of its nodes was destroyed");
        if (s_complete_count != 0)
            return fail("stale_group: tearing the group down fired its completion callback");

        er_node_destroy(heir);
        er_node_destroy(first);
    }

    /* -----------------------------------------------------------------------
     * CLOBBER GUARD: a declarative er_node_set_props() must not reset a prop
     * that is currently owned by an ERAnimValue binding.
     *
     * React re-renders call commitUpdate → er_node_set_props with the static
     * style (translateX absent → 0), which used to clobber the bound animated
     * translateX until the next tick re-pushed it. er_node_set_props now
     * re-applies bound values, so the animated transform survives the
     * declarative update within the same commit.
     * ---------------------------------------------------------------------- */
    {
        ERNode* clobber = er_node_create(ER_NODE_VIEW);
        ERProps clobber_props = props_default();
        clobber_props.width = 10;
        clobber_props.height = 10;
        er_node_set_props(clobber, &clobber_props);

        ERAnimValueHandle clobber_value = er_anim_value_create(0.0f);
        if (clobber_value == ER_ANIM_VALUE_INVALID)
            return fail("clobber: er_anim_value_create returned INVALID");

        er_anim_value_bind(clobber_value, clobber, ER_PROP_TRANSLATE_X);
        er_anim_value_set(clobber_value, 80.0f);
        if (clobber->transform_translate_x < 79.9f || clobber->transform_translate_x > 80.1f)
            return fail("clobber: bound value did not apply before re-render");

        /* Simulate a React re-render: the static props carry no transform (0). */
        ERProps rerender_props = props_default();
        rerender_props.width = 10;
        rerender_props.height = 10;
        er_node_set_props(clobber, &rerender_props);

        /* Without the re-apply guard this would now be 0 (the jump-to-middle). */
        if (clobber->transform_translate_x < 79.9f || clobber->transform_translate_x > 80.1f)
            return fail("clobber: er_node_set_props reset the animated translateX");

        er_anim_value_destroy(clobber_value);
        er_node_destroy(clobber);
    }

    /* -----------------------------------------------------------------------
     * UNBIND ONE PROP: the counterpart to the clobber guard above. Re-applying
     * bound values is what keeps a re-render from jumping a running animation,
     * but it also means a binding writes its prop forever — a prop that STOPS
     * being animated has its static value overwritten on the way out of every
     * er_node_set_props. er_anim_unbind_prop() is how the reconciler hands the
     * property back, and it must release the pair from every value that holds
     * it while leaving the node's other bindings (and the value's bindings to
     * other nodes) running. (Regression: issue #124.)
     * ---------------------------------------------------------------------- */
    {
        ERNode* owned = er_node_create(ER_NODE_VIEW);
        ERNode* other = er_node_create(ER_NODE_VIEW);
        ERProps owned_props = props_default();
        owned_props.width = 10;
        owned_props.height = 10;
        er_node_set_props(owned, &owned_props);
        er_node_set_props(other, &owned_props);

        ERAnimValueHandle first_value = er_anim_value_create(0.0f);
        ERAnimValueHandle second_value = er_anim_value_create(0.0f);
        if (first_value == ER_ANIM_VALUE_INVALID || second_value == ER_ANIM_VALUE_INVALID)
            return fail("unbind_prop: er_anim_value_create returned INVALID");

        /* translateX is driven by BOTH values (what a re-render that swapped Animated.Values without
         * releasing the old one leaves behind); translateY and the second node are bystanders. */
        er_anim_value_bind(first_value, owned, ER_PROP_TRANSLATE_X);
        er_anim_value_bind(second_value, owned, ER_PROP_TRANSLATE_X);
        er_anim_value_bind(first_value, owned, ER_PROP_TRANSLATE_Y);
        er_anim_value_bind(first_value, other, ER_PROP_TRANSLATE_X);

        er_anim_unbind_prop(owned, ER_PROP_TRANSLATE_X);

        /* Nothing drives owned->translateX now, from either value... */
        er_anim_value_set(first_value, 70.0f);
        er_anim_value_set(second_value, 40.0f);
        if (owned->transform_translate_x > 0.1f || owned->transform_translate_x < -0.1f)
            return fail("unbind_prop: a released property still followed its animated value");
        /* ...while the same node's other property and the same value's other node keep running. */
        if (owned->transform_translate_y < 69.9f || owned->transform_translate_y > 70.1f)
            return fail("unbind_prop: releasing one property dropped the node's other binding");
        if (other->transform_translate_x < 69.9f || other->transform_translate_x > 70.1f)
            return fail("unbind_prop: releasing one node dropped the value's binding to another node");

        /* And the static value a declarative update writes now sticks: er_node_set_props re-applies
         * only what is still bound, so with nothing bound translateX stays where it was put. */
        ERProps update_props = props_default();
        update_props.width = 10;
        update_props.height = 10;
        update_props.transform_translate_x = 25.0f;
        er_node_set_props(owned, &update_props);
        er_anim_value_set(first_value, 90.0f);
        if (owned->transform_translate_x < 24.9f || owned->transform_translate_x > 25.1f)
            return fail("unbind_prop: the static value did not survive after the release");

        /* A freed binding slot is reusable, and must not carry the previous tenant's interpolation. */
        ERInterpolation interpolation = {0};
        interpolation.point_count = 2U;
        interpolation.input_range[0] = 0.0f;
        interpolation.input_range[1] = 1.0f;
        interpolation.output_range[0] = 0.0f;
        interpolation.output_range[1] = 100.0f;
        interpolation.extrapolate_left = ER_EXTRAPOLATE_EXTEND;
        interpolation.extrapolate_right = ER_EXTRAPOLATE_EXTEND;
        er_anim_value_bind_interpolated(first_value, owned, ER_PROP_TRANSLATE_X, &interpolation);
        er_anim_value_set(first_value, 0.5f);
        if (owned->transform_translate_x < 49.9f || owned->transform_translate_x > 50.1f)
            return fail("unbind_prop: re-binding a released property did not take effect");

        er_anim_unbind_prop(owned, ER_PROP_TRANSLATE_X);
        er_anim_value_bind(first_value, owned, ER_PROP_TRANSLATE_X);
        er_anim_value_set(first_value, 0.25f);
        if (owned->transform_translate_x < 0.24f || owned->transform_translate_x > 0.26f)
            return fail("unbind_prop: a reused binding slot inherited the previous interpolation");

        er_anim_value_destroy(first_value);
        er_anim_value_destroy(second_value);
        er_node_destroy(owned);
        er_node_destroy(other);
    }

    return EXIT_SUCCESS;
}
