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
#include "er_limits.h"
#include "er_node_internal.h"
#include "renderer_internal.h"
#include "transform.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_MAX_TOUCHES 5U
#define ER_LONG_PRESS_MS 500U          /**< Long-press hold time when a node sets no delayLongPress of its own. */
#define ER_SCROLL_SLOP 5               /**< Minimum cumulative pan distance in pixels before auto-scroll claims. */
#define ER_SCROLL_FRICTION 0.002f      /**< Velocity decay per millisecond (≈ React Native deceleration:0.998/frame). */
#define ER_SCROLL_VELOCITY_STOP 0.001f /**< Velocity magnitude below which momentum scrolling stops. */
#define ER_SCROLL_VELOCITY_WINDOW_MS                                                                                   \
    200U /**< Maximum age in ms of the last recorded move used for velocity estimation. */
#define ER_ARC_HIT_GATE_MARGIN                                                                                         \
    16 /**< Pixels past an Arc's box and knob reach that still enter its precise hit test.                             \
        */

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Which phase of a responder negotiation the winner claimed in.
 *
 * A capture claim is made root→leaf, so it takes the gesture from everything below the claimant; a
 * bubble claim is made leaf→root, so the claimant is the deepest node that wanted it. Touch-down has
 * to tell the two apart: only the first cancels the press of the node under the finger.
 */
typedef enum
{
    ER_CLAIM_NONE = 0, /**< Nobody claimed the responder. */
    ER_CLAIM_CAPTURE,  /**< Claimed in the capture phase. */
    ER_CLAIM_BUBBLE    /**< Claimed in the bubble phase. */
} ERClaimPhase;

/**
 * @brief Per-finger press and gesture tracking state.
 */
typedef struct
{
    bool active;
    bool inside;
    bool long_press_fired;
    bool long_press_dispatched; /**< A handler actually received the long press, so the release skips onPress. */
    bool long_press_cancelled;
    uint32_t elapsed_ms;
    int last_x;
    int last_y;
    int start_x;                    /**< X coordinate at touch-down; used to compute dx for gesture events. */
    int start_y;                    /**< Y coordinate at touch-down; used to compute dy for gesture events. */
    int previous_move_x;            /**< X coordinate at the previous TOUCH_MOVE; used for velocity estimation. */
    int previous_move_y;            /**< Y coordinate at the previous TOUCH_MOVE; used for velocity estimation. */
    uint32_t previous_move_time_ms; /**< er_now_ms() at the last recorded TOUCH_MOVE. */
    float velocity_x;               /**< Finger velocity X in px/ms at the last sampled move. */
    float velocity_y;               /**< Finger velocity Y in px/ms at the last sampled move. */
    float initial_scroll_x;         /**< ScrollView scroll_offset_x when the responder was granted. */
    float initial_scroll_y;         /**< ScrollView scroll_offset_y when the responder was granted. */
    ERNodeRef press_target;
    ERNodeRef touch_target;
    ERNodeRef responder; /**< Node currently owning this touch as the gesture responder. */
} ERTouchState;

/**
 * @brief Newest un-dispatched touch-move for one finger, plus the last position actually dispatched.
 *
 * Hosts report moves as fast as their panel or window system produces them — a handful per frame on an
 * SDL or browser host, one per poll on a device. Dispatching each one runs the whole handler chain (and,
 * under the JS bridge, a React render) for a position that is already stale by the time the frame is
 * painted: only the last sample of the frame still describes where the finger is. Moves are therefore
 * parked here and dispatched once per frame by er_input_flush_moves().
 */
typedef struct
{
    bool pending;  /**< A coalesced move is waiting to be dispatched. */
    bool has_last; /**< last_x/last_y describe a real position (something has been dispatched). */
    int x;         /**< Newest move X, in the coordinates the host passed in. */
    int y;         /**< Newest move Y, in the coordinates the host passed in. */
    int last_x;    /**< X of the last touch dispatched for this finger. */
    int last_y;    /**< Y of the last touch dispatched for this finger. */
} ERPendingMove;

/**
 * @brief A node's ancestors, snapshotted for a walk that calls app code at each one.
 *
 * Bubbling a raw touch and negotiating the responder both visit the ancestors the touched node had when the
 * walk began, and any handler along the way can destroy nodes further up and create others in their pool
 * slots. The walk stays registered while it runs so er_input_forget_node() can blank a destroyed node's
 * entry, rather than the walk reaching whatever node inherits that tag.
 */
typedef struct ERChainWalk
{
    uint16_t* tags;            /**< Leaf-to-root tags; a destroyed node's entry reads ER_INVALID_TAG. */
    int length;                /**< Entries in tags. */
    struct ERChainWalk* outer; /**< The walk a handler interrupted by dispatching again, or NULL. */
} ERChainWalk;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERTouchState s_touches[ER_MAX_TOUCHES];

/** @brief Per-finger move coalescing buffer, drained by er_input_flush_moves(). */
static ERPendingMove s_pending_moves[ER_MAX_TOUCHES];

/** @brief When false, moves are dispatched as they arrive (er_input_set_move_coalescing). */
static bool s_coalesce_moves = true;

/** @brief Guards er_input_flush_moves() against a handler re-entering the frame path. */
static bool s_flushing_moves = false;

/** @brief Innermost ancestor walk in progress, linked to the walks it interrupted. */
static ERChainWalk* s_chain_walks = NULL;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Resets a touch slot to its idle state.
 *
 * @param[in,out] touch  Touch state to reset.
 */
static void reset_touch(ERTouchState* touch)
{
    memset(touch, 0, sizeof(*touch));
    touch->press_target = er_node_ref(NULL);
    touch->touch_target = er_node_ref(NULL);
    touch->responder = er_node_ref(NULL);
}

/**
 * @brief True when this finger's parked move would actually reach a handler.
 *
 * A move that lands on the position already dispatched tells the app nothing, and a finger resting
 * on a panel that reports at 100 Hz produces a stream of exactly those — so the flush drops them.
 * Being parked is therefore not the same as being about to dispatch, and er_input_has_pending_moves()
 * shares this predicate so it cannot answer differently from the flush it is describing.
 *
 * @param[in] pending_move  The finger's coalescing slot.
 *
 * @return true if flushing this finger now would dispatch a move.
 */
static bool finger_move_dispatches(const ERPendingMove* pending_move)
{
    return pending_move->pending
           && !(pending_move->has_last && pending_move->x == pending_move->last_x
                && pending_move->y == pending_move->last_y);
}

/**
 * @brief Dispatches this finger's parked move, if it still says anything new, and clears it.
 *
 * @param[in] finger_id  Finger index; must be below ER_MAX_TOUCHES.
 */
static void flush_finger_move(uint8_t finger_id)
{
    ERPendingMove* pending_move = &s_pending_moves[finger_id];
    if (!pending_move->pending)
        return;

    const bool dispatches = finger_move_dispatches(pending_move);
    pending_move->pending = false; /* consumed either way — a dropped repeat must not stay parked */
    if (!dispatches)
        return;

    pending_move->last_x = pending_move->x;
    pending_move->last_y = pending_move->y;
    pending_move->has_last = true;
    er_dispatch_touch(finger_id, ER_TOUCH_MOVE, pending_move->x, pending_move->y);
}

/**
 * @brief Returns true when a node is invisible and should be excluded from hit-testing.
 *
 * A node is invisible when display:none or when it is a view-type node with opacity 0.
 * Children of an invisible node are also excluded because hit_test_node returns NULL
 * before recursing into them.
 *
 * @param[in] node  Node to test.
 *
 * @return true when the node should not receive touch events.
 */
static bool node_is_invisible(const ERNode* node)
{
    if (node->layout.display == ER_DISPLAY_NONE)
        return true;
    if (node->type == ER_NODE_VIEW || node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_PRESSABLE)
    {
        if (node->props.view.opacity == 0)
            return true;
    }
    if (node->type == ER_NODE_MODAL && (!node->modal_visible || node->props.view.opacity == 0))
        return true;
    return false;
}

/**
 * @brief Returns whether a point lies inside a node's strict computed rectangle.
 *
 * @param[in] node  Node whose computed rectangle should be tested.
 * @param[in] x     X coordinate in framebuffer pixels.
 * @param[in] y     Y coordinate in framebuffer pixels.
 *
 * @return true when the point is inside node's strict bounds.
 */
static bool point_inside_node(const ERNode* node, int x, int y)
{
    const int left = node->computed.x;
    const int top = node->computed.y;
    const int right = left + node->computed.width;
    const int bottom = top + node->computed.height;

    return x >= left && y >= top && x < right && y < bottom;
}

/**
 * @brief Returns whether a point lies inside a node's hit-slop-extended rectangle.
 *
 * Each edge is extended outward by the corresponding hit_slop field on the node.
 * When all slop values are zero this is identical to point_inside_node().
 *
 * @param[in] node  Node to test.
 * @param[in] x     X coordinate in framebuffer pixels.
 * @param[in] y     Y coordinate in framebuffer pixels.
 *
 * @return true when the point is inside the slop-extended bounds.
 */
static bool point_inside_node_with_slop(const ERNode* node, int x, int y)
{
    const int left = (int)node->computed.x - (int)node->hit_slop_left;
    const int top = (int)node->computed.y - (int)node->hit_slop_top;
    const int right = (int)node->computed.x + (int)node->computed.width + (int)node->hit_slop_right;
    const int bottom = (int)node->computed.y + (int)node->computed.height + (int)node->hit_slop_bottom;

    return x >= left && y >= top && x < right && y < bottom;
}

#if ERUI_TRANSFORMS_FULL
/**
 * @brief Whether a node's non-translate transform actually reaches the screen.
 *
 * render_tree paints a scale / rotate / 3D node by capturing its subtree into the transform scratch and
 * inverse-mapping it back out. Three things stop that capture — the node is larger than the source
 * buffer, an ancestor's capture is already running, or the matrix will not invert, so there is no
 * inverse map to blit the scratch back with — and on any of them it degrades to painting the node
 * untransformed at its raw layout box. The pixels a finger lands on then carry no transform at all, so
 * mapping the touch through one produces a phantom hit region that does not overlap what is drawn
 * (issue #141).
 *
 * The first two are answered the way the damage pre-pass answers them (see node_transform_damage), so
 * hit-testing and damage agree about where the node is: the flag the previous paint recorded is
 * authoritative, and the size half of the capture admission test stands in on a node's first frame,
 * before there is one. The size is read from `animated` because that is what render_tree offers the
 * capture — mid-animation the layout box is not yet the box being painted.
 *
 * Invertibility is asked first, and of the CURRENT matrix, because that flag cannot answer it: it
 * reports the previous paint, while node_map_point() is about to build a matrix from today's props and
 * invert it. A transform that has just collapsed makes the two disagree for exactly one commit — the
 * flag still says "transformed" because it was, the fresh matrix is singular, er_transform_invert()
 * fails, and the touch is declined on pixels that are plainly still on the panel (issue #159). Asking
 * it here answers with the very thresholds that inverse applies, so the gate and the matrix it gates
 * cannot come from different frames; it is also the answer the coming paint will record, since a
 * singular transform is exactly what render_tree degrades to the raw box. The first-frame branch gets
 * it too, so its size test stops standing in for the whole admission rule.
 *
 * The origin handed over is node_map_point()'s own — `computed`, not the animated-minus-scroll one
 * render_tree measures with — because what has to agree here is the inverse THIS function gates, not
 * the one the last paint took. On the affine path the determinant does not depend on the origin at all;
 * on the 3D one the pivot does, and following node_map_point() is what keeps the two in step.
 *
 * Asked only of a node er_node_has_complex_transform() already admitted, so the size test below is the
 * remaining half of er_node_can_capture_transform()'s rule; the ActivityIndicator is settled there.
 *
 * @param[in] node  Node carrying a transform that is not translate-only.
 *
 * @return true when the transform is on screen and a touch must be inverse-mapped through it.
 */
static bool node_transform_reaches_screen(const ERNode* node)
{
    if (!er_transform_is_invertible(
            node, (int)node->computed.x, (int)node->computed.y, (int)node->computed.width, (int)node->computed.height))
        return false;
    if (node->has_last_paint)
        return !node->last_paint_untransformed;
    return er_transform_source_fits((int)node->animated.width, (int)node->animated.height);
}
#endif /* ERUI_TRANSFORMS_FULL */

/**
 * @brief Maps a screen-space point into a node's own untransformed coordinate space.
 *
 * Shared by every input path that has to ask "where on this node did the finger land" — the press-inside
 * test and the native Arc drag both go through it, so a transformed dial answers the same way twice
 * rather than one path silently working in raw screen pixels.
 *
 * An untransformed node passes the point straight through; a translated one subtracts the offset; a
 * full-affine or 3D one applies the inverse matrix / homography — but only when that transform is
 * really on screen, since one render_tree could not capture paints at the raw box instead. A singular
 * matrix is one of the ways it could not, so it takes that same raw-box path rather than failing here;
 * the inverse calls below cannot fail on a matrix the gate has admitted, and stay as a guard in case
 * the two thresholds ever drift apart.
 *
 * @param[in]  node   Node whose space to map into.
 * @param[in]  x,y    Screen-space point.
 * @param[out] out_x  Receives the node-space X.
 * @param[out] out_y  Receives the node-space Y.
 *
 * @return false when the point projects behind the perspective plane (3D only).
 */
static bool node_map_point(const ERNode* node, int x, int y, int* out_x, int* out_y)
{
    int local_x = x, local_y = y;
    if (node->has_transform)
    {
#if ERUI_TRANSFORMS_FULL
        if (er_node_has_complex_transform(node))
        {
            /* Degraded to the raw-box paint — too large, an ancestor holds the capture, or the matrix
             * does not invert: the drawn pixels carry no transform, so neither may the touch. The
             * translate component goes with it — render_tree's fallback paints at the plain layout
             * position and does not apply one either. */
            if (!node_transform_reaches_screen(node))
            {
                *out_x = x;
                *out_y = y;
                return true;
            }
#if ERUI_3D_TRANSFORMS
            if (er_transform_is_3d(node))
            {
                float homography[9], inverse_homography[9];
                er_transform_compute_homography_3d(
                    node, node->computed.x, node->computed.y, node->computed.width, node->computed.height, homography);
                if (!er_transform_homography_invert(homography, inverse_homography))
                    return false;
                if (!er_transform_map_point_3d(inverse_homography, x, y, &local_x, &local_y))
                    return false;
            }
            else
#endif
            {
                float matrix_a, matrix_b, matrix_c, matrix_d, matrix_translate_x, matrix_translate_y;
                er_transform_compute_matrix(node,
                                            node->computed.x,
                                            node->computed.y,
                                            node->computed.width,
                                            node->computed.height,
                                            &matrix_a,
                                            &matrix_b,
                                            &matrix_c,
                                            &matrix_d,
                                            &matrix_translate_x,
                                            &matrix_translate_y);
                float inverse_a, inverse_b, inverse_c, inverse_d, inverse_translate_x, inverse_translate_y;
                if (!er_transform_invert(matrix_a,
                                         matrix_b,
                                         matrix_c,
                                         matrix_d,
                                         matrix_translate_x,
                                         matrix_translate_y,
                                         &inverse_a,
                                         &inverse_b,
                                         &inverse_c,
                                         &inverse_d,
                                         &inverse_translate_x,
                                         &inverse_translate_y))
                    return false;
                if (!er_transform_map_point(inverse_a,
                                            inverse_b,
                                            inverse_c,
                                            inverse_d,
                                            inverse_translate_x,
                                            inverse_translate_y,
                                            x,
                                            y,
                                            &local_x,
                                            &local_y))
                    return false;
            }
        }
        else
#endif
        {
            local_x = x - (int)node->transform_translate_x;
            local_y = y - (int)node->transform_translate_y;
        }
    }
    *out_x = local_x;
    *out_y = local_y;
    return true;
}

static bool point_inside_transformed_with_slop(const ERNode* node, int x, int y)
{
    int local_x, local_y;
    if (!node_map_point(node, x, y, &local_x, &local_y))
        return false;
    return point_inside_node_with_slop(node, local_x, local_y);
}

/**
 * @brief Accumulates the total scroll offset of all ScrollView/FlatList ancestors, less their translates.
 *
 * Walking from node's parent up to the root, sums every ancestor's
 * scroll_offset_x/y.  Adding the result to a raw screen-space coordinate
 * converts it into the layout coordinate space where computed.x/y live,
 * so that point_inside_transformed_with_slop gives the correct result even
 * when an ancestor scroll view has a non-zero scroll offset.
 *
 * @param[in]  node   Node whose ancestor offsets to accumulate (excluding self).
 * @param[out] out_x  Total horizontal scroll offset, minus the ancestors' translates.
 * @param[out] out_y  Total vertical scroll offset, minus the ancestors' translates.
 */
static void accumulate_scroll_offsets(const ERNode* node, int* out_x, int* out_y)
{
    int scroll_x = 0, scroll_y = 0;
    const ERNode* ancestor = er_get_node(node->parent_tag);
    while (ancestor)
    {
        if (ancestor->type == ER_NODE_SCROLL_VIEW || ancestor->type == ER_NODE_FLAT_LIST)
        {
            scroll_x += (int)ancestor->scroll_offset_x;
            scroll_y += (int)ancestor->scroll_offset_y;
        }
        if (er_node_has_offset_translate(ancestor))
        {
            scroll_x -= (int)ancestor->transform_translate_x;
            scroll_y -= (int)ancestor->transform_translate_y;
        }
        ancestor = er_get_node(ancestor->parent_tag);
    }
    *out_x = scroll_x;
    *out_y = scroll_y;
}

/**
 * @brief Recursively finds the topmost deepest hittable node under a point.
 *
 * Respects pointer_events, hitSlop, and overflow:hidden / overflow:scroll clipping.
 * For ScrollView nodes the hit coordinates are adjusted into content space before
 * recursing, so children are tested at their layout-computed positions regardless of
 * the current scroll offset.
 *
 * @param[in] node  Subtree root to inspect.
 * @param[in] x     X coordinate in framebuffer pixels (screen space).
 * @param[in] y     Y coordinate in framebuffer pixels (screen space).
 *
 * @return Best hit node, or NULL when the point is outside the hittable subtree.
 */
static ERNode* hit_test_node(ERNode* node, int x, int y)
{
    if (!node || node_is_invisible(node))
        return NULL;

    const uint8_t pointer_events = node->pointer_events;

    /* pointer_events:none — neither this node nor any descendant is hittable. */
    if (pointer_events == ER_POINTER_EVENTS_NONE)
        return NULL;

    /* Convert the screen-space query point into the coordinate space where the node's computed rect
     * lives. node_map_point() owns that decision for every input path, so the entry gate here and the
     * press-inside test below cannot drift apart — a second copy of it living here is what let a 3D
     * node be gated by its 2D matrix while node_map_point back-projected the homography, and what let
     * #141 be fixed in one place and not the other. It fails only on a transform with no inverse (a
     * scale collapsed to nothing, a point behind the perspective plane), which is not hittable. */
    int local_x, local_y;
    if (!node_map_point(node, x, y, &local_x, &local_y))
        return NULL;

    /* Gate entry on the slop-extended bounds (using the transform-adjusted query). An Arc's knob and touch
     * slop reach past its box, so it gates on a wider rect and decides precisely below. */
    if (node->type == ER_NODE_ARC)
    {
        const int reach = (int)node->arc_overhang + ER_ARC_HIT_GATE_MARGIN;
        if (local_x < (int)node->computed.x - reach || local_y < (int)node->computed.y - reach
            || local_x >= (int)node->computed.x + (int)node->computed.width + reach
            || local_y >= (int)node->computed.y + (int)node->computed.height + reach)
            return NULL;
    }
    else if (!point_inside_node_with_slop(node, local_x, local_y))
        return NULL;

    /* Recurse into children unless box-only. */
    if (pointer_events != ER_POINTER_EVENTS_BOX_ONLY)
    {
        /* overflow:hidden and overflow:scroll clip child hit-testing to strict bounds. */
        const bool clips = (node->layout.overflow == ER_OVERFLOW_HIDDEN || node->layout.overflow == ER_OVERFLOW_SCROLL);

        if (!clips || point_inside_node(node, local_x, local_y))
        {
            /* Translate query point into content space for ScrollView nodes so that
             * children are tested against their layout-computed positions. */
            const bool node_scrolls = (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST);
            const int child_x = node_scrolls ? local_x + (int)node->scroll_offset_x : local_x;
            const int child_y = node_scrolls ? local_y + (int)node->scroll_offset_y : local_y;

            /* The topmost hit is the last one in paint order; a missed child fails its own bounds check. */
            ERNode* top_hit = NULL;
            ERChildCursor cursor;
            for (ERNode* child = er_child_first(&cursor, node); child; child = er_child_next(&cursor))
            {
                ERNode* child_hit = hit_test_node(child, child_x, child_y);
                if (child_hit)
                    top_hit = child_hit;
            }
            if (top_hit)
                return top_hit;
        }
    }

    /* pointer_events:box-none — this node is not hittable itself; children are. */
    if (pointer_events == ER_POINTER_EVENTS_BOX_NONE)
        return NULL;

    /* An Arc is hittable only on its ring (plus slop) and knob — the hole and the unswept gap fall through
     * to whatever is behind, so a centre readout or a sibling under the dial still gets its taps. */
    if (node->type == ER_NODE_ARC && !er_arc_hit(node, local_x, local_y))
        return NULL;

    return node;
}

/**
 * @brief Finds the topmost deepest node under a point from the current root.
 *
 * @param[in] x  X coordinate in framebuffer pixels.
 * @param[in] y  Y coordinate in framebuffer pixels.
 *
 * @return Hit node, or NULL when no root is set or the point is outside the root.
 */
static ERNode* hit_test(int x, int y)
{
    return hit_test_node(er_get_root_node(), x, y);
}

/**
 * @brief Returns whether a node has a handler for an event.
 *
 * @param[in] node   Node to inspect.
 * @param[in] event  Event type.
 *
 * @return true when a callback is registered.
 */
static bool has_handler(const ERNode* node, EREventType event)
{
    return node && event <= ER_EVENT_LAYOUT && node->events[event].callback;
}

/**
 * @brief Returns whether a node may receive touch events aimed at itself.
 *
 * The hit test already refuses to return a box-none node, but every dispatch path then walks up from
 * the hit node — for a press target, to bubble raw touches, to negotiate the responder — and those
 * walks would hand the node the very touches it opted out of. A box-none node with an inert child is
 * the case that bites: the child is hit, has no handler, and the climb lands on the parent.
 *
 * Only the node's own eligibility is decided here; its ancestors are still free to handle the touch.
 *
 * @param[in] node  Node to test.
 *
 * @return true when the node itself is a legal touch target.
 */
static bool node_takes_own_touches(const ERNode* node)
{
    return node && node->pointer_events != ER_POINTER_EVENTS_BOX_NONE && node->pointer_events != ER_POINTER_EVENTS_NONE;
}

/**
 * @brief Finds the nearest ancestor with any press-related handler.
 *
 * @param[in] node  Starting node.
 *
 * @return Matching node, or NULL if none was found before the root.
 */
static ERNode* nearest_press_target(ERNode* node)
{
    while (node)
    {
        /* TextInput and Switch nodes act as press targets even without explicit press
         * callbacks so that auto-focus / built-in toggle behavior works without
         * requiring the caller to register a handler on every instance. */
        if (node_takes_own_touches(node)
            && (has_handler(node, ER_EVENT_PRESS) || has_handler(node, ER_EVENT_LONG_PRESS)
                || has_handler(node, ER_EVENT_PRESS_IN) || has_handler(node, ER_EVENT_PRESS_OUT)
                || node->type == ER_NODE_TEXT_INPUT || node->type == ER_NODE_SWITCH))
            return node;
        node = er_get_node(node->parent_tag);
    }
    return NULL;
}

/**
 * @brief Finds the nearest ScrollView ancestor eligible to auto-scroll (inclusive of the node itself).
 *
 * A scroller declared box-none or none is transparent to touches aimed at itself, so it is passed
 * over and the search continues outward — an outer scroller is still free to take the pan.
 *
 * @param[in] node  Starting node.
 *
 * @return The first eligible ScrollView found walking up the ancestor chain, or NULL.
 */
static ERNode* find_scroll_view_ancestor(ERNode* node)
{
    while (node)
    {
        if ((node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST) && node_takes_own_touches(node))
            return node;
        node = er_get_node(node->parent_tag);
    }
    return NULL;
}

/**
 * @brief Invokes a node event handler if one is registered.
 *
 * @param[in] node   Target node.
 * @param[in] event  Event type.
 * @param[in] x      Touch X coordinate.
 * @param[in] y      Touch Y coordinate.
 */
static void dispatch_to_node(ERNode* node, EREventType event, int x, int y)
{
    if (!has_handler(node, event))
        return;

    EREventData data = {0};
    data.x = x;
    data.y = y;
    node->events[event].callback(node, &data, node->events[event].user_data);
}

/**
 * @brief Invokes a node event handler with a pre-built event data payload.
 *
 * @param[in] node   Target node.
 * @param[in] event  Event type.
 * @param[in] data   Event payload to forward to the handler.
 */
static void dispatch_to_node_data(ERNode* node, EREventType event, const EREventData* data)
{
    if (!has_handler(node, event))
        return;
    node->events[event].callback(node, data, node->events[event].user_data);
}

/**
 * @brief Snapshots a node's ancestors and registers the walk over them.
 *
 * Every chain_walk_begin() is paired with a chain_walk_end() once the last handler along the chain returns.
 *
 * @param[out] walk   Walk to register.
 * @param[out] tags   Caller buffer of ERUI_MAX_NODES tags; receives the chain, leaf first.
 * @param[in]  start  Leaf node; NULL gives an empty chain.
 */
static void chain_walk_begin(ERChainWalk* walk, uint16_t* tags, const ERNode* start)
{
    walk->tags = tags;
    walk->length = 0;
    for (const ERNode* node = start; node && walk->length < (int)ERUI_MAX_NODES; node = er_get_node(node->parent_tag))
        tags[walk->length++] = node->tag;

    walk->outer = s_chain_walks;
    s_chain_walks = walk;
}

/**
 * @brief Unregisters the innermost walk, started by chain_walk_begin().
 *
 * @param[in] walk  The walk to end.
 */
static void chain_walk_end(const ERChainWalk* walk)
{
    s_chain_walks = walk->outer;
}

/**
 * @brief Dispatches a raw touch event from target up through the ancestors it had when the dispatch began.
 *
 * @param[out] chain   Scratch buffer of ERUI_MAX_NODES tags for the walk.
 * @param[in]  target  Original target node.
 * @param[in]  event   Raw touch event type.
 * @param[in]  data    Event payload to forward to each handler.
 */
static void dispatch_bubble_data(uint16_t* chain, const ERNode* target, EREventType event, const EREventData* data)
{
    ERChainWalk walk;
    chain_walk_begin(&walk, chain, target);
    for (int entry_index = 0; entry_index < walk.length; entry_index++)
    {
        ERNode* node = er_get_node(walk.tags[entry_index]);
        if (node_takes_own_touches(node))
            dispatch_to_node_data(node, event, data);
    }
    chain_walk_end(&walk);
}

/**
 * @brief Builds the payload every gesture-bearing dispatch shares: point, travel and speed.
 *
 * Raw touch events carry the same three things as the responder events do — where the finger is, how
 * far it has come since touch-down, and how fast it is moving — so a plain onTouchEnd can recognise a
 * flick without the app re-deriving any of it (which it cannot do at all under the AOT, whose handler
 * subset has no clock).
 *
 * @param[in] touch  Touch slot the event belongs to.
 * @param[in] x,y    Current touch point.
 *
 * @return The filled payload.
 */
static EREventData gesture_data(const ERTouchState* touch, int x, int y)
{
    EREventData data = {0};
    data.x = x;
    data.y = y;
    data.dx = x - touch->start_x;
    data.dy = y - touch->start_y;
    data.vx = touch->velocity_x;
    data.vy = touch->velocity_y;
    return data;
}

/**
 * @brief Samples this finger's velocity against the previous move, then re-anchors the sample point.
 *
 * One sampler for both consumers: the gesture velocity handlers read (EREventData.vx/vy) and the
 * ScrollView momentum that starts at touch-up are the same measurement, so they cannot disagree.
 * A sample outside the window (two moves in one host tick, or a long pause) leaves the last reading
 * standing — nothing new has been measured, and reporting zero would throw a fling away.
 *
 * @param[in,out] touch  Touch slot to sample and re-anchor.
 * @param[in]     x,y    Current touch point.
 *
 * @return true when a fresh velocity was measured.
 */
static bool track_touch_velocity(ERTouchState* touch, int x, int y)
{
    const uint32_t now_ms = er_now_ms();
    const uint32_t elapsed = now_ms - touch->previous_move_time_ms;
    const bool sampled = elapsed > 0U && elapsed <= ER_SCROLL_VELOCITY_WINDOW_MS;
    if (sampled)
    {
        touch->velocity_x = (float)(x - touch->previous_move_x) / (float)elapsed;
        touch->velocity_y = (float)(y - touch->previous_move_y) / (float)elapsed;
    }
    touch->previous_move_x = x;
    touch->previous_move_y = y;
    touch->previous_move_time_ms = now_ms;
    return sampled;
}

/**
 * @brief Asks one node whether it wants the responder.
 *
 * @param[in] node   Node to ask; may be NULL.
 * @param[in] query  Query to run.
 * @param[in] data   Event data forwarded to the callback.
 *
 * @return The node when it claims and is still the same live node afterwards, otherwise NULL.
 */
static ERNode* query_claims(ERNode* node, ERResponderQuery query, const EREventData* data)
{
    if (!node_takes_own_touches(node))
        return NULL;
    const ERResponderQueryHandler* handler = &node->queries[(uint8_t)query];
    if (!handler->callback)
        return NULL;
    /* The query is app code: a node it unmounted claims nothing, whatever now holds its slot. */
    const ERNodeRef ref = er_node_ref(node);
    return handler->callback(node, data, handler->user_data) ? er_node_deref(ref) : NULL;
}

/**
 * @brief Runs capture-then-bubble responder negotiation along a node's ancestors.
 *
 * Iterates root→leaf (capture phase) then leaf→root (bubble phase), calling the
 * corresponding query callback on each node. Returns the first node whose callback
 * returns true, or NULL if no node claims the responder.
 *
 * @param[out] chain          Scratch buffer of ERUI_MAX_NODES tags for the walk.
 * @param[in]  start          Leaf node the negotiation starts from.
 * @param[in]  capture_query  Query type for the capture phase.
 * @param[in]  bubble_query   Query type for the bubble phase.
 * @param[in]  data           Event data forwarded to each callback.
 * @param[out] out_phase      Receives the phase the claim was won in; may be NULL.
 *
 * @return The claiming node, or NULL when no node claims the responder.
 */
static ERNode* negotiate_responder(uint16_t* chain,
                                   const ERNode* start,
                                   ERResponderQuery capture_query,
                                   ERResponderQuery bubble_query,
                                   const EREventData* data,
                                   ERClaimPhase* out_phase)
{
    ERChainWalk walk;
    chain_walk_begin(&walk, chain, start);
    ERNode* claimant = NULL;
    ERClaimPhase phase = ER_CLAIM_NONE;

    /* Capture phase: root → leaf */
    for (int entry_index = walk.length - 1; entry_index >= 0 && !claimant; entry_index--)
    {
        claimant = query_claims(er_get_node(walk.tags[entry_index]), capture_query, data);
        if (claimant)
            phase = ER_CLAIM_CAPTURE;
    }
    /* Bubble phase: leaf → root */
    for (int entry_index = 0; entry_index < walk.length && !claimant; entry_index++)
    {
        claimant = query_claims(er_get_node(walk.tags[entry_index]), bubble_query, data);
        if (claimant)
            phase = ER_CLAIM_BUBBLE;
    }

    chain_walk_end(&walk);
    if (out_phase)
        *out_phase = phase;
    return claimant;
}

/**
 * @brief Fires ER_EVENT_RESPONDER_TERMINATE on the current responder and clears the slot.
 *
 * Does nothing when no responder is active.
 *
 * @param[in,out] touch  Touch slot owning the responder.
 * @param[in]     data   Event data forwarded to the terminate callback.
 */
static void terminate_responder_if_active(ERTouchState* touch, const EREventData* data)
{
    ERNode* responder = er_node_deref(touch->responder);
    if (responder)
        dispatch_to_node_data(responder, ER_EVENT_RESPONDER_TERMINATE, data);
    touch->responder = er_node_ref(NULL);
}

/**
 * @brief Grants the gesture responder role to a node and fires ER_EVENT_RESPONDER_GRANT.
 *
 * When the granted node is a ScrollView the touch slot records the scroll offset at
 * the time of the grant and the current move position so that er_dispatch_touch can
 * derive absolute offsets and momentum velocity without needing per-frame deltas.
 *
 * @param[in,out] touch  Touch slot to update.
 * @param[in]     node   Node to grant.
 * @param[in]     data   Event data forwarded to the grant callback.
 *
 * @return The granted node, or NULL when the grant handler unmounted it.
 */
static ERNode* grant_responder(ERTouchState* touch, ERNode* node, const EREventData* data)
{
    const ERNodeRef ref = er_node_ref(node);
    touch->responder = ref;
    dispatch_to_node_data(node, ER_EVENT_RESPONDER_GRANT, data);

    /* The grant handler is app code, and app code can unmount the node it was just handed — so look it
     * up again rather than reading the pointer it was called with. */
    node = er_node_deref(ref);
    if (node && (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST))
    {
        touch->initial_scroll_x = node->scroll_offset_x;
        touch->initial_scroll_y = node->scroll_offset_y;
    }
    return node;
}

/**
 * @brief Fires ER_EVENT_RESPONDER_REJECT on a node whose responder claim was denied.
 *
 * @param[in] node  Node that was rejected.
 * @param[in] data  Event data forwarded to the reject callback.
 */
static void reject_responder(ERNode* node, const EREventData* data)
{
    dispatch_to_node_data(node, ER_EVENT_RESPONDER_REJECT, data);
}

static void arc_drag_end(const ERTouchState* touch, uint8_t finger_id);

/**
 * @brief Cancels an active touch sequence.
 *
 * @param[in,out] touch      Touch state to cancel.
 * @param[in]     finger_id  Finger this slot belongs to (releases only a drag IT owns).
 * @param[in]     x,y        Touch coordinates.
 * @param[out]    chain      Scratch buffer of ERUI_MAX_NODES tags for the bubbling cancel.
 */
static void cancel_touch(ERTouchState* touch, uint8_t finger_id, int x, int y, uint16_t* chain)
{
    if (!touch->active)
        return;

    const EREventData gesture = gesture_data(touch, x, y);
    dispatch_bubble_data(chain, er_node_deref(touch->touch_target), ER_EVENT_TOUCH_CANCEL, &gesture);
    ERNode* press_target = er_node_deref(touch->press_target);
    if (press_target && touch->inside)
        dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);

    arc_drag_end(touch, finger_id);
    terminate_responder_if_active(touch, &gesture);

    reset_touch(touch);
}

/**
 * @brief Ends the press a gesture began as, once a move hands the gesture to a responder: its press-in gets
 *        a press-out now, and releasing presses nothing, so scrolling a list never taps the row it started on.
 *
 * @param[in] touch  The finger's state.
 * @param[in] x,y    Where the finger is.
 */
static void cancel_press(ERTouchState* touch, int x, int y)
{
    ERNode* press_target = er_node_deref(touch->press_target);
    if (press_target && touch->inside)
        dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);
    touch->press_target = er_node_ref(NULL);
    touch->inside = false;
    touch->long_press_cancelled = true;
}

/** @brief Whether a ScrollView's content is larger than its viewport, so a drag can move it. */
static bool scroll_view_can_scroll(const ERNode* scroll_view)
{
    return scroll_view->scroll_content_height > scroll_view->computed.height
           || scroll_view->scroll_content_width > scroll_view->computed.width;
}

/**
 * @brief Maps a screen point into an Arc's own coordinate space (ancestor scroll, then its transform).
 *
 * Every native-drag geometry query goes through this, so the ring hit test, the end latch and the value
 * lookup all agree with each other and with the ordinary hit test. Without it a translated or scaled dial
 * is visibly under the finger yet answers from raw screen pixels, so it never becomes the drag target.
 *
 * @param[in]  arc              Arc node.
 * @param[in]  x,y              Screen-space point.
 * @param[out] local_x,local_y  Receives the arc-space point.
 *
 * @return false when the point projects behind the arc's perspective plane.
 */
static bool arc_local_point(const ERNode* arc, int x, int y, int* local_x, int* local_y)
{
    int scroll_x = 0, scroll_y = 0;
    accumulate_scroll_offsets(arc, &scroll_x, &scroll_y);
    return node_map_point(arc, x + scroll_x, y + scroll_y, local_x, local_y);
}

/**
 * @brief Finds the adjustable Arc a touch should drag, walking up from the hit node.
 *
 * A dial almost always has content inside it — a centre readout, a label — and that content is a real
 * node, so the hit lands on IT rather than on the arc and a naive check would silently kill the drag.
 * Walking up finds the arc anyway, and er_arc_hit() then applies the usual ring-only rule, so a readout
 * parked in the hole stays inert while a decorative overlay that reaches across the band does not block
 * it. Any node between the touch and the arc that does its own press/touch handling keeps the gesture —
 * a real control on top of a dial is still a control.
 *
 * @param[in] hit   Deepest hit node (may be NULL).
 * @param[in] x,y   Touch point in framebuffer pixels.
 *
 * @return The Arc to drag, or NULL.
 */
static ERNode* nearest_arc_drag_target(ERNode* hit, int x, int y)
{
    ERNode* candidate = hit;
    while (candidate)
    {
        if (candidate->type == ER_NODE_ARC)
        {
            if (!candidate->props.arc.adjustable)
                return NULL;

            if (candidate->subtree_hidden)
                return NULL;

            if (!node_takes_own_touches(candidate))
                return NULL;
            int local_x, local_y;
            if (!arc_local_point(candidate, x, y, &local_x, &local_y))
                return NULL;
            return er_arc_hit(candidate, local_x, local_y) ? candidate : NULL;
        }
        if (has_handler(candidate, ER_EVENT_PRESS) || has_handler(candidate, ER_EVENT_LONG_PRESS)
            || has_handler(candidate, ER_EVENT_TOUCH_START) || has_handler(candidate, ER_EVENT_TOUCH_MOVE))
            return NULL; /* an interactive node above the dial owns this touch */
        candidate = er_get_node(candidate->parent_tag);
    }
    return NULL;
}

/**
 * @brief Looks a dial up again after app code has run, for a drag that may only start on one still on screen.
 *
 * @param[in] ref  The dial.
 *
 * @return The dial, or NULL once it is destroyed, detached or hidden.
 */
static ERNode* arc_on_screen(ERNodeRef ref)
{
    ERNode* arc = er_node_deref(ref);
    return (arc && !arc->subtree_hidden) ? arc : NULL;
}

/**
 * @brief Returns the Arc node a touch slot is natively dragging, or NULL.
 */
static ERNode* active_arc_drag(const ERTouchState* touch, uint8_t finger_id)
{
    ERNode* responder = er_node_deref(touch->responder);
    return (responder && responder->type == ER_NODE_ARC && responder->arc_drag_finger == (int8_t)finger_id) ? responder
                                                                                                            : NULL;
}

/**
 * @brief Applies the value under a touch point to an adjustable Arc and fires ER_EVENT_VALUE_CHANGE when
 *        the quantized value moved.
 *
 * @param[in,out] arc        Arc node (responder of the drag).
 * @param[in]     x,y        Touch point in framebuffer pixels (keyboard offset already applied).
 * @param[in]     anti_wrap  false on touch-down (jump straight to the point), true on moves.
 */
static void arc_drag_to(ERNode* arc, int x, int y, bool anti_wrap)
{
    int local_x, local_y;
    if (!arc_local_point(arc, x, y, &local_x, &local_y))
        return;
    const float value = er_arc_value_at(arc, local_x, local_y, anti_wrap);
    /* RANGE mode moves only the end the gesture latched onto on touch-down. */
    const bool changed = arc->arc_drag_low ? er_arc_apply_value_start(arc, value) : er_arc_apply_value(arc, value);
    if (changed)
    {
        er_mark_dirty_upward(arc);
        const EREventHandler* handler = &arc->events[ER_EVENT_VALUE_CHANGE];
        if (handler->callback)
        {
            EREventData data = {0};
            data.x = x;
            data.y = y;
            data.value = arc->arc_value;
            data.value_start = arc->props.arc.range ? arc->arc_value_start : arc->arc_value;
            handler->callback(arc, &data, handler->user_data);
        }
    }
}

/**
 * @brief Ends a native Arc drag (touch up or cancel): releases the node's ownership of its value.
 */
static void arc_drag_end(const ERTouchState* touch, uint8_t finger_id)
{
    ERNode* arc = active_arc_drag(touch, finger_id);
    if (arc)
    {
        arc->arc_drag_finger = -1;
        arc->arc_drag_low = false;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_event_set(ERNode* node, EREventType event, EREventFn callback, void* user_data)
{
    if (!node || (unsigned)event >= (unsigned)ER_EVENT_TYPE_COUNT_)
        return;

    node->events[event].callback = callback;
    node->events[event].user_data = user_data;
}

void er_responder_query_set(ERNode* node, ERResponderQuery query, ERResponderQueryFn callback, void* user_data)
{
    if (!node || (uint8_t)query >= ER_RESPONDER_QUERY_COUNT)
        return;

    node->queries[(uint8_t)query].callback = callback;
    node->queries[(uint8_t)query].user_data = user_data;
}

int er_touch_active_count(void)
{
    int count = 0;
    for (uint8_t finger_index = 0U; finger_index < (uint8_t)ER_MAX_TOUCHES; finger_index++)
    {
        if (s_touches[finger_index].active)
            count++;
    }
    return count;
}

void er_input_reset(void)
{
    for (int finger_index = 0; finger_index < ER_MAX_TOUCHES; finger_index++)
        reset_touch(&s_touches[finger_index]);

    /* Drop any coalesced move: it names a node from the scene being torn down. The coalescing policy
     * itself is the host's, not the scene's, so s_coalesce_moves survives the reset. */
    memset(s_pending_moves, 0, sizeof(s_pending_moves));
    s_flushing_moves = false;

    /* A reset from inside a handler recycles every tag without destroying a node, so a walk still in progress
     * must not reach any of them. Each walk unregisters itself as it returns. */
    for (ERChainWalk* walk = s_chain_walks; walk; walk = walk->outer)
    {
        for (int entry_index = 0; entry_index < walk->length; entry_index++)
            walk->tags[entry_index] = ER_INVALID_TAG;
    }

    /* Zero momentum velocities on every pool node so that scroll state from a previous
     * scene (or a previous test) cannot outlive the backend reset and fire a stale
     * event callback.  er_get_node() returns NULL for unused slots. */
    for (uint16_t tag = 0U; tag < (uint16_t)ERUI_MAX_NODES; tag++)
    {
        ERNode* node = er_get_node(tag);
        if (node && (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST))
        {
            node->scroll_velocity_x = 0.0f;
            node->scroll_velocity_y = 0.0f;
        }
    }
}

void er_input_forget_node(uint16_t tag)
{
    for (ERChainWalk* walk = s_chain_walks; walk; walk = walk->outer)
    {
        for (int entry_index = 0; entry_index < walk->length; entry_index++)
        {
            if (walk->tags[entry_index] == tag)
                walk->tags[entry_index] = ER_INVALID_TAG;
        }
    }
}

void er_input_tick(uint32_t delta_ms)
{
    for (int finger_index = 0; finger_index < ER_MAX_TOUCHES; finger_index++)
    {
        ERTouchState* touch = &s_touches[finger_index];
        if (!touch->active || touch->long_press_fired || touch->long_press_cancelled || !touch->inside)
            continue;

        if (UINT32_MAX - touch->elapsed_ms < delta_ms)
            touch->elapsed_ms = UINT32_MAX;
        else
            touch->elapsed_ms += delta_ms;

        /* The hold time is the target's own delayLongPress when it set one, otherwise the default. */
        ERNode* press_target = er_node_deref(touch->press_target);
        const uint32_t threshold =
            (press_target && press_target->long_press_ms) ? press_target->long_press_ms : ER_LONG_PRESS_MS;

        if (touch->elapsed_ms >= threshold)
        {
            /* Remember whether a handler was actually there: a node without one holds an ordinary press,
             * and its release must still fire onPress (React Native's isPressCanceledByLongPress). */
            touch->long_press_dispatched = has_handler(press_target, ER_EVENT_LONG_PRESS);
            dispatch_to_node(press_target, ER_EVENT_LONG_PRESS, touch->last_x, touch->last_y);
            touch->long_press_fired = true;
        }
    }

    /* Momentum scrolling: decay velocity on all ScrollView nodes and update offsets.
     * Velocity decay approximates React Native's deceleration:0.998 per 16.67ms frame. */
    const float decay_factor = 1.0f - ER_SCROLL_FRICTION * (float)delta_ms;
    const float factor = decay_factor < 0.0f ? 0.0f : decay_factor;

    for (uint16_t tag = 0U; tag < (uint16_t)ERUI_MAX_NODES; tag++)
    {
        ERNode* scroll_view = er_get_node(tag);
        if (!scroll_view || (scroll_view->type != ER_NODE_SCROLL_VIEW && scroll_view->type != ER_NODE_FLAT_LIST))
            continue;
        if (scroll_view->scroll_velocity_x == 0.0f && scroll_view->scroll_velocity_y == 0.0f)
            continue;

        scroll_view->scroll_velocity_x *= factor;
        scroll_view->scroll_velocity_y *= factor;

        if (fabsf(scroll_view->scroll_velocity_x) < ER_SCROLL_VELOCITY_STOP)
            scroll_view->scroll_velocity_x = 0.0f;
        if (fabsf(scroll_view->scroll_velocity_y) < ER_SCROLL_VELOCITY_STOP)
            scroll_view->scroll_velocity_y = 0.0f;

        er_scroll_view_set_offset(scroll_view,
                                  scroll_view->scroll_offset_x + scroll_view->scroll_velocity_x * (float)delta_ms,
                                  scroll_view->scroll_offset_y + scroll_view->scroll_velocity_y * (float)delta_ms);
    }
}

void er_input_queue_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    if (finger_id >= ER_MAX_TOUCHES)
        return;

    ERPendingMove* pending_move = &s_pending_moves[finger_id];

    if (phase == ER_TOUCH_MOVE && s_coalesce_moves)
    {
        pending_move->x = x;
        pending_move->y = y;
        pending_move->pending = true;
        return;
    }

    /* Down, up and cancel carry the shape of the gesture rather than just a position, so they are never
     * coalesced. A move parked behind one is flushed first, so the app still sees the finger travel to
     * the release point before it sees the release. */
    flush_finger_move(finger_id);
    pending_move->last_x = x;
    pending_move->last_y = y;
    pending_move->has_last = true;
    er_dispatch_touch(finger_id, phase, x, y);
}

bool er_input_has_pending_moves(void)
{
    for (int finger_index = 0; finger_index < ER_MAX_TOUCHES; finger_index++)
    {
        if (finger_move_dispatches(&s_pending_moves[finger_index]))
        {
            return true;
        }
    }
    return false;
}

void er_input_flush_moves(void)
{
    if (s_flushing_moves)
        return; /* a handler re-entered the frame path (e.g. NativeUI.commit()) — don't recurse */

    s_flushing_moves = true;
    for (uint8_t finger_index = 0U; finger_index < (uint8_t)ER_MAX_TOUCHES; finger_index++)
        flush_finger_move(finger_index);
    s_flushing_moves = false;
}

void er_input_set_move_coalescing(bool enabled)
{
    if (!enabled)
        er_input_flush_moves(); /* never strand a parked move when switching to immediate dispatch */
    s_coalesce_moves = enabled;
}

void er_dispatch_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    if (finger_id >= ER_MAX_TOUCHES)
        return;

    /* On-screen keyboard (if active) gets first refusal: taps inside its strip type into the focused input
     * and are consumed so they never reach the scene below it. The keyboard is drawn in fixed screen space,
     * so it sees the raw point. */
    if (er_keyboard_dispatch_touch(phase, x, y))
        return;

    /* Map the screen point back into the scene when it has been shifted up for keyboard avoidance, so taps
     * land on the nodes where they're actually drawn. */
    y += er_keyboard_avoid_offset();

    ERTouchState* touch = &s_touches[finger_id];

    /* Every ancestor walk below uses this one buffer: each walk ends before the next begins, and a handler that
     * dispatches again gets a buffer of its own. */
    uint16_t chain[ERUI_MAX_NODES];

    switch (phase)
    {
        case ER_TOUCH_DOWN:
        {
            cancel_touch(touch, finger_id, x, y, chain);

            ERNode* hit = hit_test(x, y);
            ERNode* press_target = nearest_press_target(hit);

            touch->active = hit != NULL;
            if (press_target)
            {
                int scroll_x = 0, scroll_y = 0;
                accumulate_scroll_offsets(press_target, &scroll_x, &scroll_y);
                touch->inside = point_inside_transformed_with_slop(press_target, x + scroll_x, y + scroll_y);
            }
            else
            {
                touch->inside = false;
            }
            touch->long_press_fired = false;
            touch->long_press_dispatched = false;
            touch->long_press_cancelled = false;
            touch->elapsed_ms = 0U;
            touch->last_x = x;
            touch->last_y = y;
            touch->start_x = x;
            touch->start_y = y;
            touch->previous_move_x = x;
            touch->previous_move_y = y;
            touch->previous_move_time_ms = er_now_ms();
            touch->velocity_x = 0.0f;
            touch->velocity_y = 0.0f;
            touch->press_target = er_node_ref(press_target);
            touch->touch_target = er_node_ref(hit);

            const EREventData gesture = gesture_data(touch, x, y);
            dispatch_bubble_data(chain, hit, ER_EVENT_TOUCH_START, &gesture);

            hit = er_node_deref(touch->touch_target);

            /* Gesture responder negotiation: start-should-set. It settles BEFORE the press begins,
             * because a capture claim takes the gesture from everything below the claimant — the node
             * under the finger never presses, so it must not be handed a press-in to take back. A claim
             * won by bubbling is the deepest node that wanted the gesture, and leaves the press alone:
             * that is how a Pressable inside a claiming container still reports its own taps. */
            if (hit)
            {
                ERClaimPhase claim_phase = ER_CLAIM_NONE;
                ERNode* claimant = negotiate_responder(
                    chain, hit, ER_QUERY_START_SHOULD_SET_CAPTURE, ER_QUERY_START_SHOULD_SET, &gesture, &claim_phase);
                if (claimant)
                {
                    const bool claimant_is_pressed = claimant == er_node_deref(touch->press_target);
                    grant_responder(touch, claimant, &gesture);
                    if (claim_phase == ER_CLAIM_CAPTURE && !claimant_is_pressed)
                    {
                        /* The touch is the claimant's gesture, not a press: nothing in, nothing held,
                         * nothing to fire when the finger lifts. */
                        touch->press_target = er_node_ref(NULL);
                        touch->inside = false;
                        touch->long_press_cancelled = true;
                    }
                }
            }

            /* Everything above — the raw touch, the should-set queries, the grant — is app code, and
             * under the JS bridge it can commit a React update that unmounts the pressed node and hands
             * its pool slot to another. So the press begins from a fresh lookup, not from the pointer
             * taken before any of them ran, and a node that went away presses nothing. */
            press_target = er_node_deref(touch->press_target);

            dispatch_to_node(press_target, ER_EVENT_PRESS_IN, x, y);
            press_target = er_node_deref(touch->press_target);

            /* Auto-focus TextInput on press; blur any focused TextInput when tapping
             * anything else so the keyboard is dismissed on outside taps. */
            if (press_target && press_target->type == ER_NODE_TEXT_INPUT && press_target->props.text_input.editable)
                er_text_input_focus(press_target);
            else
                er_text_input_blur();

            /* The grant and the press-in are app code too: a node unmounted by either starts no drag. */
            hit = er_node_deref(touch->touch_target);
            if (hit)
            {
                /* Built-in Arc drag-to-set: an adjustable Arc under the finger takes the gesture natively —
                 * over a JS claimant and ahead of any ScrollView's auto-scroll — and jumps to the touched
                 * point. The responder stays with it until release, so a scroller never steals the drag. */
                ERNode* arc = nearest_arc_drag_target(hit, x, y);
                /* A dial has ONE value, so one finger drives it: if another is already dragging this
                 * arc, leave the gesture alone rather than re-latching its end and fighting over the
                 * value (and so that lifting either finger doesn't end the other's drag). */
                if (arc && arc->arc_drag_finger >= 0 && arc->arc_drag_finger != (int8_t)finger_id)
                    arc = NULL;
                if (arc && er_node_deref(touch->responder) != arc)
                {
                    /* Ending the old responder and granting the arc are app code too, and can unmount it. */
                    const ERNodeRef arc_ref = er_node_ref(arc);
                    terminate_responder_if_active(touch, &gesture);
                    arc = arc_on_screen(arc_ref);
                    if (arc)
                    {
                        grant_responder(touch, arc, &gesture);
                        arc = arc_on_screen(arc_ref);
                    }
                }
                if (arc)
                {
                    arc->arc_drag_finger = (int8_t)finger_id;
                    /* Latch which end of a RANGE band this gesture owns, ONCE, at the point it started —
                     * so dragging one setpoint past the other does not hand the finger to its neighbour. */
                    {
                        int local_x, local_y;
                        arc->arc_drag_low =
                            arc_local_point(arc, x, y, &local_x, &local_y) && er_arc_grab_low(arc, local_x, local_y);
                    }
                    arc_drag_to(arc, x, y, false);
                }
            }
            break;
        }
        case ER_TOUCH_MOVE:
        {
            if (!touch->active)
                break;

            touch->last_x = x;
            touch->last_y = y;

            /* Sample speed FIRST: everything dispatched below — the raw move, the responder move, the
             * should-set queries — then reports the same, current velocity. */
            const bool velocity_sampled = track_touch_velocity(touch, x, y);
            const EREventData gesture = gesture_data(touch, x, y);
            const int delta_x = gesture.dx;
            const int delta_y = gesture.dy;
            dispatch_bubble_data(chain, er_node_deref(touch->touch_target), ER_EVENT_TOUCH_MOVE, &gesture);

            ERNode* press_target = er_node_deref(touch->press_target);
            if (press_target)
            {
                int scroll_x = 0, scroll_y = 0;
                accumulate_scroll_offsets(press_target, &scroll_x, &scroll_y);
                const bool inside = point_inside_transformed_with_slop(press_target, x + scroll_x, y + scroll_y);
                if (!inside)
                    touch->long_press_cancelled = true;
                if (inside != touch->inside)
                {
                    dispatch_to_node(press_target, inside ? ER_EVENT_PRESS_IN : ER_EVENT_PRESS_OUT, x, y);
                    touch->inside = inside;
                }
            }

            /* Dispatch move to the current gesture responder */
            ERNode* responder = er_node_deref(touch->responder);
            if (responder)
                dispatch_to_node_data(responder, ER_EVENT_RESPONDER_MOVE, &gesture);

            /* Native Arc drag: track the finger; nobody else may negotiate the responder away mid-drag. */
            {
                ERNode* arc = active_arc_drag(touch, finger_id);
                if (arc)
                {
                    arc_drag_to(arc, x, y, true);
                    break;
                }
            }

            /* Move-should-set negotiation: any node in the chain may claim the responder */
            ERNode* touch_target = er_node_deref(touch->touch_target);
            if (touch_target)
            {
                ERNode* claimant = negotiate_responder(
                    chain, touch_target, ER_QUERY_MOVE_SHOULD_SET_CAPTURE, ER_QUERY_MOVE_SHOULD_SET, &gesture, NULL);
                responder = er_node_deref(touch->responder);

                if (claimant && claimant != responder)
                {
                    const ERNodeRef claimant_ref = er_node_ref(claimant);
                    bool yields = true;
                    if (responder)
                    {
                        const ERResponderQueryHandler* termination_query =
                            &responder->queries[ER_QUERY_TERMINATION_REQUEST];
                        if (termination_query->callback)
                        {
                            yields = termination_query->callback(responder, &gesture, termination_query->user_data);
                            responder = er_node_deref(touch->responder);
                        }
                    }

                    if (yields)
                    {
                        /* A scroller losing the gesture must stop DEAD, not coast: its live velocity
                         * was fed by the very moves that are now the claimant's, so momentum after
                         * the handover would fight the new responder's gesture. */
                        if (responder
                            && (responder->type == ER_NODE_SCROLL_VIEW || responder->type == ER_NODE_FLAT_LIST))
                        {
                            responder->scroll_velocity_x = 0.0f;
                            responder->scroll_velocity_y = 0.0f;
                        }
                        terminate_responder_if_active(touch, &gesture);
                        claimant = er_node_deref(claimant_ref);
                        if (claimant)
                            grant_responder(touch, claimant, &gesture);
                        /* The gesture is the claimant's now, a drag and not a tap, even when the claimant is
                         * the node the finger pressed. */
                        cancel_press(touch, x, y);
                    }
                    else
                    {
                        reject_responder(er_node_deref(claimant_ref), &gesture);
                    }
                }
            }

            /* Auto-scroll: if no responder was claimed and the pan exceeds slop, find the
             * nearest ScrollView ancestor and grant it automatically. */
            touch_target = er_node_deref(touch->touch_target);
            if (touch->responder.tag == ER_INVALID_TAG && touch_target)
            {
                const int distance_x = delta_x < 0 ? -delta_x : delta_x;
                const int distance_y = delta_y < 0 ? -delta_y : delta_y;
                if (distance_x >= ER_SCROLL_SLOP || distance_y >= ER_SCROLL_SLOP)
                {
                    ERNode* scroll_view = find_scroll_view_ancestor(touch_target);
                    if (scroll_view)
                    {
                        scroll_view = grant_responder(touch, scroll_view, &gesture);
                        /* A scroller that can move takes the drag as a scroll, so releasing must not press
                           the row it started on. One whose content fits leaves a jittery tap a tap. */
                        if (scroll_view && scroll_view_can_scroll(scroll_view))
                            cancel_press(touch, x, y);
                    }
                }
            }

            /* Apply incremental scroll to any active ScrollView responder. */
            {
                ERNode* active = er_node_deref(touch->responder);
                if (active && (active->type == ER_NODE_SCROLL_VIEW || active->type == ER_NODE_FLAT_LIST))
                {
                    /* Update live velocity so that TOUCH_UP always inherits the most recent movement
                     * speed, even when UP follows MOVE in the same tick. The content moves OPPOSITE the
                     * finger, hence the sign flip on the shared sample. It goes first because moving the
                     * content fires onScroll, which is app code. */
                    if (velocity_sampled)
                    {
                        active->scroll_velocity_x = -touch->velocity_x;
                        active->scroll_velocity_y = -touch->velocity_y;
                    }

                    er_scroll_view_set_offset(
                        active, touch->initial_scroll_x - (float)delta_x, touch->initial_scroll_y - (float)delta_y);
                }
            }
            break;
        }
        case ER_TOUCH_UP:
        {
            if (!touch->active)
                break;

            ERNode* press_target = er_node_deref(touch->press_target);
            bool inside = false;
            if (press_target)
            {
                int scroll_x = 0, scroll_y = 0;
                accumulate_scroll_offsets(press_target, &scroll_x, &scroll_y);
                inside = point_inside_transformed_with_slop(press_target, x + scroll_x, y + scroll_y);
            }
            /* The velocity reported here is the LAST MOVE's, not a fresh sample: a fling ends with the
             * finger resting for a frame or two, and measuring across that rest reports zero and throws
             * the throw away. (The ScrollView's momentum below deliberately DOES re-sample — coming to
             * rest before lifting should not launch the content.) */
            const EREventData gesture = gesture_data(touch, x, y);

            dispatch_bubble_data(chain, er_node_deref(touch->touch_target), ER_EVENT_TOUCH_END, &gesture);
            press_target = er_node_deref(touch->press_target);
            if (press_target)
            {
                /* Every dispatch below can run app code, and under the synchronous QuickJS root that code
                 * can commit a React update that unmounts this node — returning its pool slot, possibly to
                 * a different node. So look it up again between dispatches and stop if it went away, rather
                 * than carrying the raw pointer across a callback. */
                const ERNodeRef press_ref = er_node_ref(press_target);
                if (touch->inside)
                {
                    dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);
                    press_target = er_node_deref(press_ref);
                }
                if (inside && press_target)
                {
                    /* Built-in Switch toggle: flipping value on press kicks off the
                     * ER_SWITCH_THUMB_MS thumb animation in er_node_set_props. The user's
                     * ER_EVENT_PRESS handler (if any) runs immediately after. */
                    if (press_target->type == ER_NODE_SWITCH)
                    {
                        const uint8_t new_value = press_target->props.switch_props.value ? 0U : 1U;
                        press_target->props.switch_props.value = new_value;
                        ERAnimationConfig config;
                        memset(&config, 0, sizeof(config));
                        config.type = ER_ANIMATION_TIMING;
                        config.easing = ER_EASE_EASE_IN_OUT;
                        config.duration_ms = ER_SWITCH_THUMB_MS;
                        er_animation_start(press_target, ER_PROP_SWITCH_THUMB, new_value ? 1.0f : 0.0f, &config);
                        er_mark_dirty_upward(press_target);
                        /* onValueChange: the new value, so a host needs no PRESS-then-guess round-trip. */
                        const EREventHandler* value_handler = &press_target->events[ER_EVENT_VALUE_CHANGE];
                        if (value_handler->callback)
                        {
                            EREventData value_data = {0};
                            value_data.x = x;
                            value_data.y = y;
                            value_data.value = new_value ? 1.0f : 0.0f;
                            value_data.value_start = value_data.value;
                            value_handler->callback(press_target, &value_data, value_handler->user_data);
                            press_target = er_node_deref(press_ref);
                        }
                    }
                    /* A long press that reached a handler REPLACES the tap: the gesture already did its
                     * job, so releasing must not also run onPress. */
                    if (press_target && !touch->long_press_dispatched)
                        dispatch_to_node(press_target, ER_EVENT_PRESS, x, y);
                }
            }

            /* Release the gesture responder */
            arc_drag_end(touch, finger_id);
            ERNode* responder = er_node_deref(touch->responder);
            if (responder)
            {
                dispatch_to_node_data(responder, ER_EVENT_RESPONDER_RELEASE, &gesture);
                responder = er_node_deref(touch->responder);

                /* Final velocity update for ScrollView responders: if real time elapsed
                 * between the last MOVE and this UP (finger held still before release),
                 * re-sample to capture deceleration.  Velocity was already set during the
                 * last MOVE and is kept unchanged when elapsed == 0. */
                if (responder && (responder->type == ER_NODE_SCROLL_VIEW || responder->type == ER_NODE_FLAT_LIST))
                {
                    const uint32_t now_ms = er_now_ms();
                    const uint32_t elapsed = now_ms - touch->previous_move_time_ms;
                    if (elapsed > 0U && elapsed <= ER_SCROLL_VELOCITY_WINDOW_MS)
                    {
                        responder->scroll_velocity_x = -(float)(x - touch->previous_move_x) / (float)elapsed;
                        responder->scroll_velocity_y = -(float)(y - touch->previous_move_y) / (float)elapsed;
                    }
                }
            }

            reset_touch(touch);
            break;
        }
        case ER_TOUCH_CANCEL:
        {
            cancel_touch(touch, finger_id, x, y, chain);
            break;
        }
        default:
            break;
    }
}
