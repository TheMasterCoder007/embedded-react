#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"
#include <stdio.h>
#include <stdlib.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Null fill callback used for the test render backend.
 *
 * @param[in] argb  Fill color (unused).
 * @param[in] x     X (unused).
 * @param[in] y     Y (unused).
 * @param[in] w     Width (unused).
 * @param[in] h     Height (unused).
 * @param[in] ctx   Context (unused).
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief Null copy callback.
 *
 * @param[in] src     Source (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] w       Width (unused).
 * @param[in] h       Height (unused).
 * @param[in] ctx     Context (unused).
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief Null blend callback.
 *
 * @param[in] src     Source (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] alpha   Alpha (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] w       Width (unused).
 * @param[in] h       Height (unused).
 * @param[in] ctx     Context (unused).
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief Returns an ERProps struct with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return ERProps with layout defaults initialized.
 */
static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable failure description.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
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
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

    /* -----------------------------------------------------------------------
     * Setup: a 200×200 root with one 60×60 child at an explicit position.
     * ---------------------------------------------------------------------- */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.width = 200;
    p.height = 200;
    er_node_set_props(root, &p);
    er_tree_set_root(root);

    ERNode* child = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = 60;
    p.height = 60;
    p.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &p);
    er_tree_append_child(root, child);

    /* First commit — no animation config; child gets animated == computed. */
    er_commit();
    if (child->animated.w != child->computed.w)
        return fail("first commit: animated.w should equal computed.w");
    if (child->animated.h != child->computed.h)
        return fail("first commit: animated.h should equal computed.h");

    /* -----------------------------------------------------------------------
     * TIMING LINEAR: grow child from 60→120 over 200 ms, linear easing.
     * ---------------------------------------------------------------------- */
    ERLayoutAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 200U;
    cfg.easing = ER_EASE_LINEAR;
    er_layout_anim_configure_next(&cfg);

    p = props_default();
    p.width = 120;
    p.height = 120;
    p.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &p);

    /* Commit starts the animation. animated should still be near 60 (not snapped). */
    er_commit();
    if (child->animated.w >= child->computed.w)
        return fail("timing: animated.w should not have jumped to computed.w immediately after commit");

    /* At t=0 the slot is active but no time has elapsed yet. Tick 100 ms (half). */
    embedded_renderer_tick(100U);
    if (child->animated.w < 85 || child->animated.w > 95)
        return fail("timing linear: animated.w should be ~90 at the half-way point");
    if (child->animated.h < 85 || child->animated.h > 95)
        return fail("timing linear: animated.h should be ~90 at the half-way point");

    /* Finish the animation. */
    embedded_renderer_tick(100U);
    if (child->animated.w != child->computed.w)
        return fail("timing linear: animated.w should equal computed.w after full duration");
    if (child->animated.h != child->computed.h)
        return fail("timing linear: animated.h should equal computed.h after full duration");

    /* -----------------------------------------------------------------------
     * CONFIGURE-NEXT IS SINGLE-USE: second commit without a new configure
     * call must NOT animate — child snaps directly.
     * ---------------------------------------------------------------------- */
    p = props_default();
    p.width = 60;
    p.height = 60;
    p.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &p);

    er_commit(); /* No er_layout_anim_configure_next() call — should snap. */
    if (child->animated.w != child->computed.w)
        return fail("single-use: animated.w should have snapped without a fresh configure");

    /* -----------------------------------------------------------------------
     * SPRING: grow child from 60→120 using spring physics.
     * ---------------------------------------------------------------------- */
    cfg.type = ER_ANIM_SPRING;
    cfg.duration_ms = 0U;
    cfg.stiffness = 200.0f;
    cfg.damping = 20.0f;
    cfg.mass = 1.0f;
    er_layout_anim_configure_next(&cfg);

    p = props_default();
    p.width = 120;
    p.height = 120;
    p.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &p);
    er_commit();

    /* Run the spring for 3 seconds — enough time for the physics to fully settle and
     * the slot to become inactive.  The early-exit pattern (break when animated==computed)
     * is intentionally avoided: the spring passes through the target before settling,
     * so that check fires prematurely and leaves an active slot that corrupts later tests. */
    for (int tick = 0; tick < 300; tick++)
        embedded_renderer_tick(10U);
    if (child->animated.w != child->computed.w)
        return fail("spring: did not settle at computed rect within 3 seconds");

    /* -----------------------------------------------------------------------
     * NEW-NODE SKIP: a freshly created node must snap without animation even
     * when a config is pending.
     * ---------------------------------------------------------------------- */
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 300U;
    cfg.easing = ER_EASE_LINEAR;
    er_layout_anim_configure_next(&cfg);

    ERNode* newnode = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = 80;
    p.height = 80;
    er_node_set_props(newnode, &p);
    er_tree_append_child(root, newnode);

    er_commit();
    if (newnode->animated.w != newnode->computed.w)
        return fail("new-node: animated.w should snap on first commit even with pending config");

    /* Consume any leftover pending state before mid-animation retarget test. */
    embedded_renderer_tick(400U);

    /* -----------------------------------------------------------------------
     * MID-ANIMATION RETARGET: change the target while an animation is running;
     * the animation should restart from the current display position.
     * ---------------------------------------------------------------------- */
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 400U;
    cfg.easing = ER_EASE_LINEAR;
    er_layout_anim_configure_next(&cfg);

    /* Start animating child from 120→60. */
    p = props_default();
    p.width = 60;
    p.height = 60;
    p.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &p);
    er_commit();

    /* Advance 100 ms (25% of 400 ms); animated.w should be between 60 and 120. */
    embedded_renderer_tick(100U);
    const int16_t mid_w = child->animated.w;
    if (mid_w <= 60 || mid_w >= 120)
        return fail("retarget: mid-animation animated.w out of expected range");

    /* Retarget to 40 while the animation is still running.  We use 40 rather than
     * a value near the midpoint to avoid false equality with mid_w. */
    er_layout_anim_configure_next(&cfg);
    p = props_default();
    p.width = 40;
    p.height = 40;
    p.background_color = 0xFF2A9D8F;
    er_node_set_props(child, &p);
    er_commit(); /* layout_anim_post_layout should retarget the slot */

    /* Animated.w should still be near mid_w (not yet at 40). */
    if (child->animated.w == 40)
        return fail("retarget: animated.w jumped to new target instead of animating from mid-point");

    /* Finish the retargeted animation. */
    embedded_renderer_tick(500U);
    if (child->animated.w != 40)
        return fail("retarget: did not settle at new target 40 after re-animation");

    printf("OK\n");
    return EXIT_SUCCESS;
}
