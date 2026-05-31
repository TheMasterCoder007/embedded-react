#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * @param[in] argb  Fill color.
 * @param[in] x     Destination X.
 * @param[in] y     Destination Y.
 * @param[in] w     Fill width.
 * @param[in] h     Fill height.
 * @param[in] ctx   Pointer to RenderCounts.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    RenderCounts* counts = ctx;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    counts->last_fill_color = argb;
}

/**
 * @brief Backend copy callback unused by this test.
 *
 * @param[in] src     Source buffer.
 * @param[in] stride  Source stride.
 * @param[in] x       Destination X.
 * @param[in] y       Destination Y.
 * @param[in] w       Width.
 * @param[in] h       Height.
 * @param[in] ctx     Opaque context.
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
 * @brief Backend blend callback unused by this test.
 *
 * @param[in] src     Source buffer.
 * @param[in] stride  Source stride.
 * @param[in] alpha   Global alpha.
 * @param[in] x       Destination X.
 * @param[in] y       Destination Y.
 * @param[in] w       Width.
 * @param[in] h       Height.
 * @param[in] ctx     Opaque context.
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
 *   - Decay: velocity decreases each tick.
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
    ERProps p = props_default();
    p.width = 100;
    p.height = 40;
    p.background_color = 0xFFFF0000U;
    p.opacity = 255U;
    er_node_set_props(card, &p);

    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 100U;

    er_anim_start(card, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF00FF00U), &cfg);
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
    er_anim_start(card, ER_PROP_OPACITY, 0.0f, &cfg);
    embedded_renderer_tick(25U);
    er_anim_cancel(card, ER_PROP_OPACITY);
    const uint8_t cancelled_opacity = card->props.view.opacity;
    embedded_renderer_tick(100U);
    if (card->props.view.opacity != cancelled_opacity)
        return fail("cancel: cancelled opacity animation continued ticking");

    /* -----------------------------------------------------------------------
     * TIMING: zero-duration snaps immediately
     * ---------------------------------------------------------------------- */
    cfg.duration_ms = 0U;
    er_anim_start(card, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF0000FFU), &cfg);
    if (card->props.view.background_color != 0xFF0000FFU)
        return fail("zero-duration: animation did not apply immediately");

    /* -----------------------------------------------------------------------
     * EASING: quad-in must produce a value below linear at the midpoint
     * ---------------------------------------------------------------------- */
    {
        ERNode* eq = er_node_create(ER_NODE_VIEW);
        ERProps ep = props_default();
        ep.width = 10;
        ep.height = 10;
        ep.opacity = 0U;
        er_node_set_props(eq, &ep);

        ERAnimConfig ec = {0};
        ec.type = ER_ANIM_TIMING;
        ec.duration_ms = 100U;
        ec.easing = ER_EASE_QUAD_IN;
        er_anim_start(eq, ER_PROP_OPACITY, 1.0f, &ec);
        embedded_renderer_tick(50U);

        /* Quad-in at t=0.5: f(0.5)=0.25  →  opacity ≈ 64.
         * Linear at t=0.5 would give opacity ≈ 128. */
        if (eq->props.view.opacity >= 100U)
            return fail("easing quad-in: midpoint value should be well below linear");

        er_node_destroy(eq);
    }

    /* -----------------------------------------------------------------------
     * EASING: ease-out must produce a value above linear at the midpoint
     * ---------------------------------------------------------------------- */
    {
        ERNode* eo = er_node_create(ER_NODE_VIEW);
        ERProps ep = props_default();
        ep.width = 10;
        ep.height = 10;
        ep.opacity = 0U;
        er_node_set_props(eo, &ep);

        ERAnimConfig ec = {0};
        ec.type = ER_ANIM_TIMING;
        ec.duration_ms = 100U;
        ec.easing = ER_EASE_EASE_OUT;
        er_anim_start(eo, ER_PROP_OPACITY, 1.0f, &ec);
        embedded_renderer_tick(50U);

        /* ease-out is faster at the beginning → midpoint value > linear (128). */
        if (eo->props.view.opacity <= 128U)
            return fail("easing ease-out: midpoint value should be above linear");

        er_node_destroy(eo);
    }

    /* -----------------------------------------------------------------------
     * SPRING: settles at the target within 3 seconds
     * ---------------------------------------------------------------------- */
    {
        ERNode* sn = er_node_create(ER_NODE_VIEW);
        ERProps sp = props_default();
        sp.width = 10;
        sp.height = 10;
        sp.opacity = 0U;
        er_node_set_props(sn, &sp);

        ERAnimConfig sc = {0};
        sc.type = ER_ANIM_SPRING;
        sc.stiffness = 100.0f;
        sc.damping = 10.0f;
        sc.mass = 1.0f;
        er_anim_start(sn, ER_PROP_OPACITY, 1.0f, &sc);

        /* Tick for up to 3 seconds; expect the spring to settle. */
        for (int tick = 0; tick < 300; tick++)
            embedded_renderer_tick(10U);

        /* Opacity should be 255 (fully opaque) after the spring settles. */
        if (sn->props.view.opacity < 250U)
            return fail("spring: did not settle at target opacity within 3 seconds");

        er_node_destroy(sn);
    }

    /* -----------------------------------------------------------------------
     * DECAY: velocity decreases over time
     * ---------------------------------------------------------------------- */
    {
        ERNode* dn = er_node_create(ER_NODE_VIEW);
        ERProps dp2 = props_default();
        dp2.width = 10;
        dp2.height = 10;
        dp2.opacity = 0U;
        er_node_set_props(dn, &dp2);

        ERAnimConfig dc = {0};
        dc.type = ER_ANIM_DECAY;
        dc.velocity = 0.1f;       /* 0.1 normalised/ms — moves toward to_value initially */
        dc.deceleration = 0.990f; /* aggressive deceleration for a short test */

        er_anim_start(dn, ER_PROP_OPACITY, 1.0f, &dc);

        embedded_renderer_tick(50U);
        const uint8_t mid_opacity = dn->props.view.opacity;
        embedded_renderer_tick(500U);
        const uint8_t late_opacity = dn->props.view.opacity;

        /* After 500ms extra the position must not have grown by much more than the first 50ms. */
        const int delta_mid = (int)mid_opacity;
        const int delta_late = (int)late_opacity - delta_mid;
        if (delta_late > delta_mid * 3)
            return fail("decay: velocity did not decelerate — position grew unexpectedly");

        er_node_destroy(dn);
    }

    /* -----------------------------------------------------------------------
     * DELAY: animation value must not change until delay expires
     * ---------------------------------------------------------------------- */
    {
        ERNode* ln = er_node_create(ER_NODE_VIEW);
        ERProps lp2 = props_default();
        lp2.width = 10;
        lp2.height = 10;
        lp2.opacity = 0U;
        er_node_set_props(ln, &lp2);

        ERAnimConfig lc = {0};
        lc.type = ER_ANIM_TIMING;
        lc.duration_ms = 100U;
        lc.delay_ms = 200U;

        er_anim_start(ln, ER_PROP_OPACITY, 1.0f, &lc);

        embedded_renderer_tick(100U); /* within delay window */
        if (ln->props.view.opacity != 0U)
            return fail("delay: opacity changed before delay expired");

        embedded_renderer_tick(200U); /* past the delay; animation should be running */
        embedded_renderer_tick(100U); /* complete the animation duration */
        if (ln->props.view.opacity < 240U)
            return fail("delay: animation did not complete after delay + duration");

        er_node_destroy(ln);
    }

    /* -----------------------------------------------------------------------
     * COMPLETION CALLBACK: fires with finished=true at end of timing animation
     * ---------------------------------------------------------------------- */
    {
        ERNode* cn = er_node_create(ER_NODE_VIEW);
        ERProps cp2 = props_default();
        cp2.width = 10;
        cp2.height = 10;
        cp2.opacity = 0U;
        er_node_set_props(cn, &cp2);

        s_complete_count = 0;
        s_complete_finished = false;

        ERAnimConfig cc = {0};
        cc.type = ER_ANIM_TIMING;
        cc.duration_ms = 100U;
        cc.on_complete = on_complete;

        er_anim_start(cn, ER_PROP_OPACITY, 1.0f, &cc);
        embedded_renderer_tick(100U);

        if (s_complete_count != 1)
            return fail("completion: on_complete not called exactly once at end");
        if (!s_complete_finished)
            return fail("completion: on_complete called with finished=false on natural end");

        er_node_destroy(cn);
    }

    /* -----------------------------------------------------------------------
     * COMPLETION CALLBACK: er_anim_stop fires finished=false
     * ---------------------------------------------------------------------- */
    {
        ERNode* sn2 = er_node_create(ER_NODE_VIEW);
        ERProps sp2 = props_default();
        sp2.width = 10;
        sp2.height = 10;
        sp2.opacity = 0U;
        er_node_set_props(sn2, &sp2);

        s_complete_count = 0;
        s_complete_finished = true; /* will be overwritten */

        ERAnimConfig sc2 = {0};
        sc2.type = ER_ANIM_TIMING;
        sc2.duration_ms = 500U;
        sc2.on_complete = on_complete;

        ERAnimHandle h = er_anim_start(sn2, ER_PROP_OPACITY, 1.0f, &sc2);
        embedded_renderer_tick(100U);
        er_anim_stop(h);

        if (s_complete_count != 1)
            return fail("er_anim_stop: on_complete not called after stop");
        if (s_complete_finished)
            return fail("er_anim_stop: on_complete should have finished=false");

        er_node_destroy(sn2);
    }

    /* -----------------------------------------------------------------------
     * SEQUENCE: three color animations fire one after another
     * ---------------------------------------------------------------------- */
    {
        ERNode* a = er_node_create(ER_NODE_VIEW);
        ERNode* b = er_node_create(ER_NODE_VIEW);
        ERNode* c = er_node_create(ER_NODE_VIEW);

        ERProps pp = props_default();
        pp.width = 10;
        pp.height = 10;
        pp.background_color = 0xFF000000U;
        er_node_set_props(a, &pp);
        er_node_set_props(b, &pp);
        er_node_set_props(c, &pp);

        ERAnimEntry entries[3];
        memset(entries, 0, sizeof(entries));

        entries[0].node = a;
        entries[0].prop = ER_PROP_BACKGROUND_COLOR;
        entries[0].value = float_from_bits(0xFFFF0000U);
        entries[0].cfg.type = ER_ANIM_TIMING;
        entries[0].cfg.duration_ms = 100U;

        entries[1].node = b;
        entries[1].prop = ER_PROP_BACKGROUND_COLOR;
        entries[1].value = float_from_bits(0xFF00FF00U);
        entries[1].cfg.type = ER_ANIM_TIMING;
        entries[1].cfg.duration_ms = 100U;

        entries[2].node = c;
        entries[2].prop = ER_PROP_BACKGROUND_COLOR;
        entries[2].value = float_from_bits(0xFF0000FFU);
        entries[2].cfg.type = ER_ANIM_TIMING;
        entries[2].cfg.duration_ms = 100U;

        s_complete_count = 0;
        s_complete_finished = false;

        er_anim_sequence(entries, 3, on_complete, NULL);

        /* After 100ms: first animation done, second should be running. */
        embedded_renderer_tick(100U);
        if (a->props.view.background_color != 0xFFFF0000U)
            return fail("sequence: entry 0 did not reach target at t=100ms");
        if (b->props.view.background_color == 0xFF00FF00U)
            return fail("sequence: entry 1 finished before entry 0 was done");
        if (s_complete_count != 0)
            return fail("sequence: group completed too early");

        /* After another 100ms: second done. */
        embedded_renderer_tick(100U);
        if (b->props.view.background_color != 0xFF00FF00U)
            return fail("sequence: entry 1 did not reach target at t=200ms");

        /* After another 100ms: third done, group completed. */
        embedded_renderer_tick(100U);
        if (c->props.view.background_color != 0xFF0000FFU)
            return fail("sequence: entry 2 did not reach target at t=300ms");
        if (s_complete_count != 1)
            return fail("sequence: group on_complete not called after last entry");
        if (!s_complete_finished)
            return fail("sequence: group on_complete called with finished=false");

        er_node_destroy(a);
        er_node_destroy(b);
        er_node_destroy(c);
    }

    /* -----------------------------------------------------------------------
     * PARALLEL: two animations start and finish simultaneously
     * ---------------------------------------------------------------------- */
    {
        ERNode* pa = er_node_create(ER_NODE_VIEW);
        ERNode* pb = er_node_create(ER_NODE_VIEW);

        ERProps pp = props_default();
        pp.width = 10;
        pp.height = 10;
        pp.opacity = 0U;
        er_node_set_props(pa, &pp);
        er_node_set_props(pb, &pp);

        ERAnimEntry entries[2];
        memset(entries, 0, sizeof(entries));

        entries[0].node = pa;
        entries[0].prop = ER_PROP_OPACITY;
        entries[0].value = 1.0f;
        entries[0].cfg.type = ER_ANIM_TIMING;
        entries[0].cfg.duration_ms = 100U;

        entries[1].node = pb;
        entries[1].prop = ER_PROP_OPACITY;
        entries[1].value = 1.0f;
        entries[1].cfg.type = ER_ANIM_TIMING;
        entries[1].cfg.duration_ms = 150U;

        s_complete_count = 0;

        er_anim_parallel(entries, 2, on_complete, NULL);

        embedded_renderer_tick(100U);
        if (pa->props.view.opacity < 250U)
            return fail("parallel: entry 0 (100ms) not done at t=100ms");
        if (s_complete_count != 0)
            return fail("parallel: group completed before longer entry finished");

        embedded_renderer_tick(50U);
        if (pb->props.view.opacity < 250U)
            return fail("parallel: entry 1 (150ms) not done at t=150ms");
        if (s_complete_count != 1)
            return fail("parallel: group on_complete not called after all entries done");

        er_node_destroy(pa);
        er_node_destroy(pb);
    }

    /* -----------------------------------------------------------------------
     * STAGGER: second animation starts after stagger_ms delay
     * ---------------------------------------------------------------------- */
    {
        ERNode* sa = er_node_create(ER_NODE_VIEW);
        ERNode* sb = er_node_create(ER_NODE_VIEW);

        ERProps sp = props_default();
        sp.width = 10;
        sp.height = 10;
        sp.opacity = 0U;
        er_node_set_props(sa, &sp);
        er_node_set_props(sb, &sp);

        ERAnimEntry entries[2];
        memset(entries, 0, sizeof(entries));

        entries[0].node = sa;
        entries[0].prop = ER_PROP_OPACITY;
        entries[0].value = 1.0f;
        entries[0].cfg.type = ER_ANIM_TIMING;
        entries[0].cfg.duration_ms = 100U;

        entries[1].node = sb;
        entries[1].prop = ER_PROP_OPACITY;
        entries[1].value = 1.0f;
        entries[1].cfg.type = ER_ANIM_TIMING;
        entries[1].cfg.duration_ms = 100U;

        /* stagger 50ms: entry 0 starts at 0ms, entry 1 at 50ms. */
        er_anim_stagger(entries, 2, 50U, NULL, NULL);

        /* At t=100ms: entry 0 done (0-100ms), entry 1 halfway (50-150ms). */
        embedded_renderer_tick(100U);
        if (sa->props.view.opacity < 250U)
            return fail("stagger: entry 0 not done at t=100ms");
        if (sb->props.view.opacity >= 250U)
            return fail("stagger: entry 1 should not be done at t=100ms (starts at 50ms)");
        if (sb->props.view.opacity == 0U)
            return fail("stagger: entry 1 should have started by t=100ms");

        er_node_destroy(sa);
        er_node_destroy(sb);
    }

    /* -----------------------------------------------------------------------
     * RENDER ORDER: animated lower-z node must not paint over higher-z sibling
     * ---------------------------------------------------------------------- */
    RenderCounts counts = {0};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &counts};
    embedded_renderer_set_backend(&be);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = 120;
    p.height = 80;
    p.background_color = 0xFF000000U;
    er_node_set_props(root, &p);
    er_tree_set_root(root);

    ERNode* low = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 80;
    p.height = 60;
    p.background_color = 0xFFFF0000U;
    p.z_index = 0;
    er_node_set_props(low, &p);

    ERNode* high = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 20;
    p.top = 10;
    p.width = 80;
    p.height = 60;
    p.background_color = 0xFF0000FFU;
    p.z_index = 10;
    er_node_set_props(high, &p);

    er_tree_append_child(root, low);
    er_tree_append_child(root, high);
    er_commit();

    cfg.duration_ms = 100U;
    cfg.easing = ER_EASE_LINEAR;
    er_anim_start(low, ER_PROP_BACKGROUND_COLOR, float_from_bits(0xFF00FF00U), &cfg);
    embedded_renderer_tick(50U);
    er_commit();

    if (counts.last_fill_color != 0xFF0000FFU)
        return fail("render order: animated lower zIndex node rendered over higher sibling");

    /* -----------------------------------------------------------------------
     * ANIMATED VALUE: one value drives multiple node-property bindings
     * ---------------------------------------------------------------------- */
    {
        /* Create two nodes; both will receive translateX updates from one value. */
        ERNode* va = er_node_create(ER_NODE_VIEW);
        ERNode* vb = er_node_create(ER_NODE_VIEW);

        ERProps vp = props_default();
        vp.width = 10;
        vp.height = 10;
        er_node_set_props(va, &vp);
        er_node_set_props(vb, &vp);

        /* Create the shared value at 0 and bind both nodes. */
        ERAnimValueHandle vh = er_anim_value_create(0.0f);
        if (vh == ER_ANIM_VALUE_INVALID)
            return fail("anim_value: er_anim_value_create returned INVALID");

        er_anim_value_bind(vh, va, ER_PROP_TRANSLATE_X);
        er_anim_value_bind(vh, vb, ER_PROP_TRANSLATE_X);

        /* Immediate snap to 50. */
        er_anim_value_set(vh, 50.0f);
        if (va->tp_translate_x < 49.9f || va->tp_translate_x > 50.1f)
            return fail("anim_value: er_anim_value_set did not propagate to first binding");
        if (vb->tp_translate_x < 49.9f || vb->tp_translate_x > 50.1f)
            return fail("anim_value: er_anim_value_set did not propagate to second binding");
        if (er_anim_value_get(vh) < 49.9f || er_anim_value_get(vh) > 50.1f)
            return fail("anim_value: er_anim_value_get did not return snapped value");

        /* Animate back to 0 over 100ms; verify midpoint drives both nodes. */
        ERAnimConfig vc = {0};
        vc.type = ER_ANIM_TIMING;
        vc.duration_ms = 100U;
        vc.easing = ER_EASE_LINEAR;
        er_anim_value_animate(vh, 0.0f, &vc);

        embedded_renderer_tick(50U);
        /* At the midpoint both nodes should be near 25. */
        if (va->tp_translate_x > 30.0f || va->tp_translate_x < 20.0f)
            return fail("anim_value: first binding not at midpoint during animation");
        if (vb->tp_translate_x > 30.0f || vb->tp_translate_x < 20.0f)
            return fail("anim_value: second binding not at midpoint during animation");

        embedded_renderer_tick(50U);
        /* Animation complete; both should be at the target. */
        if (va->tp_translate_x > 1.0f)
            return fail("anim_value: first binding did not reach target at end");
        if (vb->tp_translate_x > 1.0f)
            return fail("anim_value: second binding did not reach target at end");

        /* Duplicate bind is silently ignored — binding_count must not grow past 2. */
        er_anim_value_bind(vh, va, ER_PROP_TRANSLATE_X);
        er_anim_value_set(vh, 10.0f);
        if (va->tp_translate_x < 9.9f || va->tp_translate_x > 10.1f)
            return fail("anim_value: duplicate bind guard broke propagation");

        er_anim_value_destroy(vh);
        er_node_destroy(va);
        er_node_destroy(vb);
    }

    return EXIT_SUCCESS;
}
