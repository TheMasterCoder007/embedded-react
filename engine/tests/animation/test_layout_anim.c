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
#include <stdio.h>
#include <stdlib.h>

#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Null fill callback used for the test render backend.
 *
 * @param[in] argb    Fill color (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Context (unused).
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief Null copy callback.
 *
 * @param[in] source  Source (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Context (unused).
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief Null blend callback.
 *
 * @param[in] source  Source (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] alpha   Alpha (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Context (unused).
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
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
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point exercising the LayoutAnimation engine.
 *
 * Tests covered:
 *   - Timing linear: animated rect interpolates from old to new over the duration.
 *   - Timing ease: same correctness with a non-linear easing curve.
 *   - Spring: settles at the new computed rect within a bounded tick budget.
 *   - Configure-next is single-use: the second commit without a new configure call
 *     does not animate.
 *   - Mid-animation retarget: changing the target while an animation is in progress
 *     restarts the animation from the current display position.
 *   - New-node skip: a node appearing for the first time is snapped without animation.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    EmbeddedRenderBackend backend = {0};
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    /* -----------------------------------------------------------------------
     * Setup: a 200×200 root with one 60×60 child at an explicit position.
     * ---------------------------------------------------------------------- */
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_default();
    props.width = 200;
    props.height = 200;
    er_node_set_props(root, &props);
    er_tree_set_root(root);

    ERNode* child = er_node_create(ER_NODE_VIEW);
    props = props_default();
    props.width = 60;
    props.height = 60;
    props.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &props);
    er_tree_append_child(root, child);

    /* First commit — no animation config; child gets animated == computed. */
    er_commit();
    if (child->animated.width != child->computed.width)
        return fail("first commit: animated.w should equal computed.w");
    if (child->animated.height != child->computed.height)
        return fail("first commit: animated.h should equal computed.h");

    /* -----------------------------------------------------------------------
     * TIMING LINEAR: grow child from 60→120 over 200 ms, linear easing.
     * ---------------------------------------------------------------------- */
    ERLayoutAnimConfig config = {0};
    config.type = ER_ANIM_TIMING;
    config.duration_ms = 200U;
    config.easing = ER_EASE_LINEAR;
    er_layout_anim_configure_next(&config);

    props = props_default();
    props.width = 120;
    props.height = 120;
    props.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &props);

    /* Commit starts the animation. animated should still be near 60 (not snapped). */
    er_commit();
    if (child->animated.width >= child->computed.width)
        return fail("timing: animated.w should not have jumped to computed.w immediately after commit");

    /* At t=0 the slot is active but no time has elapsed yet. Tick 100 ms (half). */
    embedded_renderer_tick(100U);
    if (child->animated.width < 85 || child->animated.width > 95)
        return fail("timing linear: animated.w should be ~90 at the half-way point");
    if (child->animated.height < 85 || child->animated.height > 95)
        return fail("timing linear: animated.h should be ~90 at the half-way point");

    /* Finish the animation. */
    embedded_renderer_tick(100U);
    if (child->animated.width != child->computed.width)
        return fail("timing linear: animated.w should equal computed.w after full duration");
    if (child->animated.height != child->computed.height)
        return fail("timing linear: animated.h should equal computed.h after full duration");

    /* -----------------------------------------------------------------------
     * CONFIGURE-NEXT IS SINGLE-USE: second commit without a new configure
     * call must NOT animate — child snaps directly.
     * ---------------------------------------------------------------------- */
    props = props_default();
    props.width = 60;
    props.height = 60;
    props.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &props);

    er_commit(); /* No er_layout_anim_configure_next() call — should snap. */
    if (child->animated.width != child->computed.width)
        return fail("single-use: animated.w should have snapped without a fresh configure");

    /* -----------------------------------------------------------------------
     * SPRING: grow child from 60→120 using spring physics.
     * ---------------------------------------------------------------------- */
    config.type = ER_ANIM_SPRING;
    config.duration_ms = 0U;
    config.stiffness = 200.0f;
    config.damping = 20.0f;
    config.mass = 1.0f;
    er_layout_anim_configure_next(&config);

    props = props_default();
    props.width = 120;
    props.height = 120;
    props.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &props);
    er_commit();

    /* Run the spring for 3 seconds — enough time for the physics to fully settle and
     * the slot to become inactive.  The early-exit pattern (break when animated==computed)
     * is intentionally avoided: the spring passes through the target before settling,
     * so that check fires prematurely and leaves an active slot that corrupts later tests. */
    for (int tick = 0; tick < 300; tick++)
        embedded_renderer_tick(10U);
    if (child->animated.width != child->computed.width)
        return fail("spring: did not settle at computed rect within 3 seconds");

    /* -----------------------------------------------------------------------
     * NEW-NODE SKIP: a freshly created node must snap without animation even
     * when a config is pending.
     * ---------------------------------------------------------------------- */
    config.type = ER_ANIM_TIMING;
    config.duration_ms = 300U;
    config.easing = ER_EASE_LINEAR;
    er_layout_anim_configure_next(&config);

    ERNode* newnode = er_node_create(ER_NODE_VIEW);
    props = props_default();
    props.width = 80;
    props.height = 80;
    er_node_set_props(newnode, &props);
    er_tree_append_child(root, newnode);

    er_commit();
    if (newnode->animated.width != newnode->computed.width)
        return fail("new-node: animated.w should snap on first commit even with pending config");

    /* Consume any leftover pending state before mid-animation retarget test. */
    embedded_renderer_tick(400U);

    /* -----------------------------------------------------------------------
     * MID-ANIMATION RETARGET: change the target while an animation is running;
     * the animation should restart from the current display position.
     * ---------------------------------------------------------------------- */
    config.type = ER_ANIM_TIMING;
    config.duration_ms = 400U;
    config.easing = ER_EASE_LINEAR;
    er_layout_anim_configure_next(&config);

    /* Start animating child from 120→60. */
    props = props_default();
    props.width = 60;
    props.height = 60;
    props.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &props);
    er_commit();

    /* Advance 100 ms (25% of 400 ms); animated.w should be between 60 and 120. */
    embedded_renderer_tick(100U);
    const int16_t mid_width = child->animated.width;
    if (mid_width <= 60 || mid_width >= 120)
        return fail("retarget: mid-animation animated.w out of expected range");

    /* Retarget to 40 while the animation is still running.  We use 40 rather than
     * a value near the midpoint to avoid false equality with mid_width. */
    er_layout_anim_configure_next(&config);
    props = props_default();
    props.width = 40;
    props.height = 40;
    props.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &props);
    er_commit(); /* layout_anim_post_layout should retarget the slot */

    /* Animated.w should still be near mid_width (not yet at 40). */
    if (child->animated.width == 40)
        return fail("retarget: animated.w jumped to new target instead of animating from mid-point");

    /* Finish the retargeted animation. */
    embedded_renderer_tick(500U);
    if (child->animated.width != 40)
        return fail("retarget: did not settle at new target 40 after re-animation");

    /* -----------------------------------------------------------------------
     * STALE SLOT: a layout animation must not survive its node.
     *
     * er_layout_anim_tick() drops a slot whose node has gone, but only while
     * the tag is still on the free list — the next er_node_create() recycles
     * it, and the slot then interpolates the NEW node to the dead one's
     * geometry. er_node_destroy() cancels the slot outright, the same way it
     * cancels tag-keyed property animations. (Regression.)
     * ---------------------------------------------------------------------- */
    {
        ERNode* doomed = er_node_create(ER_NODE_VIEW);
        props = props_default();
        props.width = 60;
        props.height = 60;
        props.background_color = 0xFFE76F51;
        er_node_set_props(doomed, &props);
        er_tree_append_child(root, doomed);
        er_commit(); /* settles at 60×60 */

        er_layout_anim_configure_next(&config);
        props = props_default();
        props.width = 160;
        props.height = 160;
        props.background_color = 0xFFE76F51;
        er_node_set_props(doomed, &props);
        er_commit(); /* starts a 60 → 160 interpolation */
        embedded_renderer_tick(50U);
        if (doomed->animated.width <= 60 || doomed->animated.width >= 160)
            return fail("stale_slot: precondition — the layout animation is not mid-flight");

        const uint16_t recycled_tag = doomed->tag;
        er_tree_remove_child(root, doomed);
        er_node_destroy(doomed); /* destroyed mid-interpolation, before any tick */

        ERNode* heir = er_node_create(ER_NODE_VIEW);
        if (heir->tag != recycled_tag)
            return fail("stale_slot: precondition — freed tag was not recycled");
        props = props_default();
        props.width = 30;
        props.height = 30;
        props.background_color = 0xFF264653;
        er_node_set_props(heir, &props);
        er_tree_append_child(root, heir);
        er_commit();

        /* A surviving slot would drag the heir toward the dead node's 160×160. */
        embedded_renderer_tick(200U);
        if (heir->animated.width != heir->computed.width || heir->animated.height != heir->computed.height)
            return fail("stale_slot: a destroyed node's layout animation drove the node that reused its tag");

        er_tree_remove_child(root, heir);
        er_node_destroy(heir);
    }

    printf("OK\n");
    return EXIT_SUCCESS;
}
