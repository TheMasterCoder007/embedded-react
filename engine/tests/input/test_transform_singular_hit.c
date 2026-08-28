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

/*
 * Issue #159: a transform that goes SINGULAR drops touches for one commit.
 *
 * node_map_point() builds a node's matrix from its CURRENT props, but the decision to USE one was gated
 * on last_paint_untransformed, which answers from the PREVIOUS paint. For exactly one commit after a
 * transform collapses the two disagree: the flag still says "this node is painted transformed" — it
 * was — while the fresh matrix is singular, so er_transform_invert() refuses and node_map_point()
 * returns false, which the hit test reads as "not this node". Nothing else answers either, so the touch
 * falls through to the background while the node's pixels sit unchanged on the panel.
 *
 * It misses rather than mis-targets, and it does not latch: the next paint records the raw box, the two
 * agree again, and the node is hittable. This test is about the one commit in between.
 *
 * Each scenario settles a node on a transform that really captures, moves it across the invert
 * threshold WITHOUT committing, and taps the middle of the pixels still on screen. Both halves of the
 * setup are pinned — er_transform_is_invertible() on each state, and last_paint_untransformed on the
 * settled paint — so a scenario that stopped straddling the threshold (a changed epsilon, a changed
 * matrix) fails as a broken setup rather than quietly passing on a case that no longer tests anything.
 *
 * Two things are asserted next to the dropped tap, because the obvious over-fix — treat every complex
 * transform as its raw box — passes without them: a tap outside the raw box still misses, and a
 * transform that stays INVERTIBLE across the same kind of change still maps touches through its matrix
 * rather than through its layout box.
 *
 * The Arc case is the same hole on the native-drag path: er_arc_hit() and er_arc_value_at() reach the
 * matrix through arc_local_point() -> node_map_point(), so a dial whose scale animates through the
 * singular band drops drag samples rather than just a press.
 *
 * Requires ERUI_TRANSFORMS=FULL — without the capture path there is no inverse to fail, and every
 * scenario would pass on a gate the build compiled out.
 */

#include "er_node_internal.h" /* last_paint_untransformed / arc_drag_finger — asserts the stale window */
#include "er_scene.h"
#include "native_renderer.h"
#include "transform.h" /* er_transform_is_invertible / _source_fits — asserts the path each state takes */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 240

/* The issue's own geometry: a 100x100 node at 60,60, so the box is 60..159 and its centre is 110,110.
 * Small enough to fit the transform scratch in every configuration CI builds, which is what puts the
 * settled state on the capture path at all. */
#define NODE_X 60
#define NODE_Y 60
#define NODE_W 100
#define NODE_H 100
#define NODE_CX (NODE_X + NODE_W / 2)
#define NODE_CY (NODE_Y + NODE_H / 2)

/* Well outside the raw box, and outside every transformed footprint here. */
#define OUTSIDE_X (NODE_X + NODE_W + 20)
#define OUTSIDE_Y (NODE_Y + NODE_H + 20)

/*----------------------------------------------------------------------------------------------------------------------
 - Backend
 ---------------------------------------------------------------------------------------------------------------------*/

/* Hit-testing reads no pixels, but the paint has to actually run: last_paint_untransformed is what the
 * gate under test consults, and it is written by render_tree. The callbacks discard their input. */

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

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

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_press_count = 0;
static int s_change_count = 0;

/** @brief Counts presses on the node under test. */
static void on_press(ERNode* n, const EREventData* d, void* ud)
{
    (void)n;
    (void)d;
    (void)ud;
    s_press_count++;
}

/** @brief Counts native Arc value changes, which is how a drag reports itself. */
static void on_change(ERNode* n, const EREventData* d, void* ud)
{
    (void)n;
    (void)d;
    (void)ud;
    s_change_count++;
}

/**
 * @brief Reports one failed assertion.
 *
 * @param[in] msg  What was expected.
 *
 * @return EXIT_FAILURE, for the caller to return.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

#if ERUI_TRANSFORMS_FULL

/**
 * @brief Presses and releases one finger at a point, and reports whether it reached the node.
 *
 * @param[in] x,y  Screen-space tap position.
 *
 * @return true when the tap fired ER_EVENT_PRESS.
 */
static bool tap_hits(int x, int y)
{
    const int before = s_press_count;
    embedded_renderer_touch(0, ER_TOUCH_DOWN, x, y);
    embedded_renderer_touch(0, ER_TOUCH_UP, x, y);
    return s_press_count != before;
}

/** @brief One transform state: what a scenario settles at, or what it moves to. */
typedef struct
{
    float scale;     /**< 0 = unset (identity scale). */
    float rot_z;     /**< Degrees. */
    float rot_y;     /**< Degrees (3D; ignored unless ERUI_3D_TRANSFORMS). */
    bool invertible; /**< What er_transform_is_invertible() must answer for it. */
} Xform;

/** @brief A settled state, the state one set_props moves it to, and what the pair is called. */
typedef struct
{
    Xform from, to;
    const char* name;
} Scenario;

/** @brief Applies a transform state to a props block. */
static void set_transform(ERProps* p, const Xform* x)
{
    p->transform_scale_x = x->scale;
    p->transform_scale_y = x->scale;
    p->transform_rotate_z = x->rot_z;
#if ERUI_3D_TRANSFORMS
    p->transform_rotate_y = x->rot_y;
#else
    (void)x->rot_y;
#endif
}

/** @brief Props for the root: opaque, screen-sized, so the node has something to fall through TO. */
static ERProps root_props(void)
{
    ERProps p;
    er_props_default(&p);
    p.width = SCREEN;
    p.height = SCREEN;
    p.background_color = 0xFF000000U;
    return p;
}

/** @brief Props for the node under test, before its transform is applied. */
static ERProps node_props(void)
{
    ERProps p;
    er_props_default(&p);
    p.position = ER_POS_ABSOLUTE;
    p.left = NODE_X;
    p.top = NODE_Y;
    p.width = NODE_W;
    p.height = NODE_H;
    p.background_color = 0xFF2244AAU;
    return p;
}

/**
 * @brief Settles a pressable on a transform and asserts it really took the capture path.
 *
 * @param[in]  x     Transform state to settle on.
 * @param[out] out   Receives the node.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE when the setup did not land where the scenario says.
 */
static int settle_pressable(const Xform* x, ERNode** out)
{
    er_reset();
    s_press_count = 0;

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = root_props();
    er_node_set_props(root, &rp);
    er_tree_set_root(root);

    ERNode* node = er_node_create(ER_NODE_PRESSABLE);
    ERProps np = node_props();
    set_transform(&np, x);
    er_node_set_props(node, &np);
    er_event_set(node, ER_EVENT_PRESS, on_press, NULL);
    er_tree_append_child(root, node);

    er_commit();
    er_commit(); /* settle: a painted footprint and a settled fallback flag */

    *out = node;

    /* The node has no scroll or keyboard shift above it, so its layout position IS the origin both
     * render_tree and node_map_point() hand the matrix. */
    if (er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_W, NODE_H) != x->invertible)
        return fail("the settled state did not land on the side of the epsilon the scenario names");
    if (!node->has_last_paint || node->last_paint_untransformed)
        return fail("the settled transform did not take the capture path — the scenario proves nothing");

    return EXIT_SUCCESS;
}

/**
 * @brief Drives one node across the invert threshold without committing, and taps its painted pixels.
 *
 * @param[in] s  Scenario to run.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_singular_crossing(const Scenario* s)
{
    ERNode* node = NULL;
    if (settle_pressable(&s->from, &node) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    /* Control: the settled state answers, and answers only over itself. Every state here keeps the
     * node's centre fixed (a centre pivot is the default), so the centre is on the paint throughout. */
    if (!tap_hits(NODE_CX, NODE_CY))
        return fail("a tap on the middle of a settled transformed node did not reach it");
    if (tap_hits(OUTSIDE_X, OUTSIDE_Y))
        return fail("a tap past a settled transformed node still reached it");

    ERProps np = node_props();
    set_transform(&np, &s->to);
    er_node_set_props(node, &np);

    if (er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_W, NODE_H) != s->to.invertible)
        return fail("the collapsed state did not land on the side of the epsilon the scenario names");
    /* The window under test: the props are the new ones, the panel is still showing the old paint, and
     * the flag still describes it. Without this the scenario could pass on a node that had quietly
     * repainted. */
    if (node->last_paint_untransformed)
        return fail("the fallback flag already retired — there is no stale window left to test");

    /* THE REGRESSION: pixels of the settled paint, unchanged on the panel, before any repaint. */
    if (!tap_hits(NODE_CX, NODE_CY))
        return fail("a tap on the painted pixels of a just-collapsed transform was dropped");
    /* ... and the fallback is the node's box, not the whole screen. */
    if (tap_hits(OUTSIDE_X, OUTSIDE_Y))
        return fail("a just-collapsed transform answered for a tap outside its box");

    er_commit(); /* the repaint that retires the flag; this side always worked */

    if (!node->last_paint_untransformed)
        return fail("the collapsed transform did not fall back to its raw box on the next paint");
    if (!tap_hits(NODE_CX, NODE_CY))
        return fail("a tap on a collapsed transform's raw box missed after the repaint");
    if (tap_hits(OUTSIDE_X, OUTSIDE_Y))
        return fail("a repainted collapsed transform answered for a tap outside its box");

    printf("%s: hit through the stale window and after the repaint\n", s->name);
    return EXIT_SUCCESS;
}

/**
 * @brief Checks a transform that stays invertible across the same change still maps through its matrix.
 *
 * The counterpart to check_singular_crossing, and the reason the fix has to ask about invertibility
 * rather than simply stop trusting the flag: at scale 0.002 about a centre pivot the node is drawn as a
 * speck at 110,110, so a touch a few pixels away is background. Answering it at the raw box — the
 * over-fix — would hand every one of those to a node that is not there.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_invertible_crossing_still_maps_through_transform(void)
{
    const Xform from = {0.5f, 0.0f, 0.0f, true};
    ERNode* node = NULL;
    if (settle_pressable(&from, &node) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    /* det = 4e-6, comfortably over the 1e-6 affine epsilon: still a real transform. */
    const Xform to = {0.002f, 0.0f, 0.0f, true};
    ERProps np = node_props();
    set_transform(&np, &to);
    er_node_set_props(node, &np);

    if (!er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_W, NODE_H))
        return fail("the control state is singular — it no longer straddles the epsilon");

    if (!tap_hits(NODE_CX, NODE_CY))
        return fail("a tap on the speck an invertible shrink draws did not reach it");
    /* Inside the raw box, far outside the speck: the transform, not the box, has to answer. */
    if (tap_hits(NODE_X + 10, NODE_Y + 10))
        return fail("a still-invertible transform was hit at its raw box — the touch stopped following "
                    "the matrix");

    printf("scale .5 -> .002 (invertible): still mapped through the matrix\n");
    return EXIT_SUCCESS;
}

/**
 * @brief Checks a dial whose transform has just collapsed still starts a native drag on its ring.
 *
 * er_arc_hit() and er_arc_value_at() both reach the matrix through arc_local_point() ->
 * node_map_point(), so the dropped-touch window costs an Arc more than a press: every drag sample
 * inside it is discarded, and a dial animating through the singular band goes dead mid-gesture rather
 * than simply refusing to start.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_singular_arc_still_drags(void)
{
    er_reset();
    s_change_count = 0;

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = root_props();
    er_node_set_props(root, &rp);
    er_tree_set_root(root);

    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps ap;
    er_props_default(&ap);
    ap.position = ER_POS_ABSOLUTE;
    ap.left = NODE_X;
    ap.top = NODE_Y;
    ap.width = NODE_W;
    ap.height = NODE_H;
    ap.arc_width = 16;
    ap.arc_value = 25.0f;
    ap.arc_max = 100.0f;
    ap.arc_step = 5.0f;
    ap.arc_adjustable = 1;
    ap.arc_track_color = 0xFF404040U;
    ap.arc_indicator_color = 0xFF00A0FFU;
    ap.transform_scale_x = 1.0f; /* a real transform on the capture path, and the identity matrix */
    ap.transform_scale_y = 1.0f;
    er_node_set_props(arc, &ap);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_change, NULL);
    er_tree_append_child(root, arc);

    er_commit();
    er_commit();

    if (!er_transform_is_invertible(arc, NODE_X, NODE_Y, NODE_W, NODE_H) || !arc->has_last_paint
        || arc->last_paint_untransformed)
        return fail("the settled dial did not take the capture path — the scenario proves nothing");

    /* Top of the ring: centre 110,110, r_mid = 50 - 16/2 = 42, and 270 degrees is inside the default
     * 270-degree sweep from 135. */
    const int ring_x = NODE_CX, ring_y = NODE_CY - 42;

    embedded_renderer_touch(0, ER_TOUCH_DOWN, ring_x, ring_y);
    if (s_change_count != 1 || arc->arc_drag_finger < 0)
        return fail("a touch on a settled dial's ring did not start a native drag");
    embedded_renderer_touch(0, ER_TOUCH_UP, ring_x, ring_y);

    /* Collapse it, without committing. Every other prop is re-supplied as it was. */
    ap.transform_scale_x = 0.0005f;
    ap.transform_scale_y = 0.0005f;
    er_node_set_props(arc, &ap);
    s_change_count = 0;

    if (er_transform_is_invertible(arc, NODE_X, NODE_Y, NODE_W, NODE_H))
        return fail("the collapsed dial transform still inverts — it no longer crosses the epsilon");
    if (arc->last_paint_untransformed)
        return fail("the dial's fallback flag already retired — no stale window left to test");

    embedded_renderer_touch(0, ER_TOUCH_DOWN, ring_x, ring_y);
    if (s_change_count != 1 || arc->arc_drag_finger < 0)
        return fail("a touch on the painted ring of a just-collapsed dial did not start a drag");
    embedded_renderer_touch(0, ER_TOUCH_UP, ring_x, ring_y);

    printf("arc scale 1 -> .0005 (singular): ring still dragged through the stale window\n");
    return EXIT_SUCCESS;
}

#endif /* ERUI_TRANSFORMS_FULL */

/**
 * @brief Test entry point.
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

#if ERUI_TRANSFORMS_FULL
    /* Vacuous unless the settled state really captures, which needs the node to fit the scratch. */
    if (!er_transform_source_fits(NODE_W, NODE_H))
    {
        printf("SKIP: a %dx%d node does not fit this build's transform scratch\n", NODE_W, NODE_H);
        return EXIT_SUCCESS;
    }

    static const Scenario k_scenarios[] = {
        /* The issue's repro: det = 2.5e-7, an order of magnitude under the 1e-6 affine epsilon, from a
         * settled identity whose paint is exactly the raw box the fallback is about to use. */
        {{1.0f, 0.0f, 0.0f, true}, {0.0005f, 0.0f, 0.0f, false}, "scale 1 -> .0005 (singular)"},
        /* The same crossing from a settled state whose paint is NOT the raw box, so the tap is on
         * pixels only the transform put there. */
        {{0.5f, 0.0f, 0.0f, true}, {0.0005f, 0.0f, 0.0f, false}, "scale .5 -> .0005 (singular)"},
        /* Rotation in the mix, so the collapsed matrix is not simply a scaled identity. */
        {{1.2f, 30.0f, 0.0f, true}, {0.0008f, 30.0f, 0.0f, false}, "rot 30, scale 1.2 -> .0008"},
#if ERUI_3D_TRANSFORMS
        /* The same hole on the 3D path: at rotate_y 90 the plane is edge-on, the homography's
         * determinant (~4.4e-8 orthographic) falls under the 1e-7 3D epsilon, and
         * er_transform_homography_invert() refuses where er_transform_invert() would. */
        {{0.0f, 0.0f, 45.0f, true}, {0.0f, 0.0f, 90.0f, false}, "3D rotateY 45 -> 90 (singular)"},
#endif
    };

    for (size_t i = 0; i < sizeof(k_scenarios) / sizeof(k_scenarios[0]); i++)
    {
        if (check_singular_crossing(&k_scenarios[i]) != EXIT_SUCCESS)
            return EXIT_FAILURE;
    }

    if (test_invertible_crossing_still_maps_through_transform() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_singular_arc_still_drags() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#else
    printf("SKIP: ERUI_TRANSFORMS is not FULL — there is no capture path for an inverse to fail\n");
#endif

    return EXIT_SUCCESS;
}
