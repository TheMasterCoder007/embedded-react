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
#include "transform.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_MAX_TOUCHES 5U
#define ER_LONG_PRESS_MS 500U
#define ER_SCROLL_SLOP 5          /**< Minimum cumulative pan distance in pixels before auto-scroll claims. */
#define ER_SCROLL_FRICTION 0.002f /**< Velocity decay per millisecond (≈ React Native deceleration:0.998/frame). */
#define ER_SCROLL_VEL_STOP 0.001f /**< Velocity magnitude below which momentum scrolling stops. */
#define ER_SCROLL_VEL_WINDOW 200U /**< Maximum age in ms of the last recorded move used for velocity estimation. */

#ifndef ERUI_MAX_NODES
#define ERUI_MAX_NODES 512
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Per-finger press and gesture tracking state.
 */
typedef struct
{
    bool active;
    bool inside;
    bool long_press_fired;
    bool long_press_cancelled;
    uint32_t elapsed_ms;
    int last_x;
    int last_y;
    int start_x;                /**< X coordinate at touch-down; used to compute dx for gesture events. */
    int start_y;                /**< Y coordinate at touch-down; used to compute dy for gesture events. */
    int prev_move_x;            /**< X coordinate at the previous TOUCH_MOVE; used for velocity estimation. */
    int prev_move_y;            /**< Y coordinate at the previous TOUCH_MOVE; used for velocity estimation. */
    uint32_t prev_move_time_ms; /**< er_now_ms() at the last recorded TOUCH_MOVE. */
    float vel_x;                /**< Finger velocity X in px/ms at the last sampled move. */
    float vel_y;                /**< Finger velocity Y in px/ms at the last sampled move. */
    float initial_scroll_x;     /**< ScrollView scroll_offset_x when the responder was granted. */
    float initial_scroll_y;     /**< ScrollView scroll_offset_y when the responder was granted. */
    uint16_t press_target_tag;
    uint16_t touch_target_tag;
    uint16_t responder_tag; /**< Node currently owning this touch as the gesture responder. */
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
    touch->press_target_tag = ER_INVALID_TAG;
    touch->touch_target_tag = ER_INVALID_TAG;
    touch->responder_tag = ER_INVALID_TAG;
}

/**
 * @brief Dispatches this finger's parked move, if it still says anything new, and clears it.
 *
 * @param[in] finger_id  Finger index; must be below ER_MAX_TOUCHES.
 */
static void flush_finger_move(uint8_t finger_id)
{
    ERPendingMove* pm = &s_pending_moves[finger_id];
    if (!pm->pending)
        return;

    pm->pending = false;

    /* A move that lands on the position already dispatched tells the app nothing, and a finger resting
     * on a panel that reports at 100 Hz produces a stream of exactly those. Drop them. */
    if (pm->has_last && pm->x == pm->last_x && pm->y == pm->last_y)
        return;

    pm->last_x = pm->x;
    pm->last_y = pm->y;
    pm->has_last = true;
    er_dispatch_touch(finger_id, ER_TOUCH_MOVE, pm->x, pm->y);
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
    const int x1 = node->computed.x;
    const int y1 = node->computed.y;
    const int x2 = x1 + node->computed.w;
    const int y2 = y1 + node->computed.h;

    return x >= x1 && y >= y1 && x < x2 && y < y2;
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
    const int x1 = (int)node->computed.x - (int)node->hit_slop_left;
    const int y1 = (int)node->computed.y - (int)node->hit_slop_top;
    const int x2 = (int)node->computed.x + (int)node->computed.w + (int)node->hit_slop_right;
    const int y2 = (int)node->computed.y + (int)node->computed.h + (int)node->hit_slop_bottom;

    return x >= x1 && y >= y1 && x < x2 && y < y2;
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
 * Asked only of a node render_tree would put on the capture path at all; the ActivityIndicator, which
 * it never does, is settled by the caller.
 *
 * @param[in] node  Node carrying a transform that is not translate-only.
 *
 * @return true when the transform is on screen and a touch must be inverse-mapped through it.
 */
static bool node_transform_reaches_screen(const ERNode* node)
{
    if (!er_transform_is_invertible(
            node, (int)node->computed.x, (int)node->computed.y, (int)node->computed.w, (int)node->computed.h))
        return false;
    if (node->has_last_paint)
        return !node->last_paint_untransformed;
    return er_transform_source_fits((int)node->animated.w, (int)node->animated.h);
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
    int qx = x, qy = y;
#if ERUI_TRANSFORMS_FULL
    const bool can_capture = node->type != ER_NODE_ACTIVITY_INDICATOR;
#endif
    if (node->has_transform)
    {
#if ERUI_TRANSFORMS_FULL
        if (can_capture && !er_transform_is_translate_only(node))
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
                float H[9], inv_H[9];
                er_transform_compute_homography_3d(
                    node, node->computed.x, node->computed.y, node->computed.w, node->computed.h, H);
                if (!er_transform_homography_invert(H, inv_H))
                    return false;
                /* Back-project screen point through the inverse homography. */
                const float sx_f = (float)x, sy_f = (float)y;
                const float Wp = inv_H[6] * sx_f + inv_H[7] * sy_f + inv_H[8];
                if (Wp <= 0.0f)
                    return false;
                qx = (int)((inv_H[0] * sx_f + inv_H[1] * sy_f + inv_H[2]) / Wp);
                qy = (int)((inv_H[3] * sx_f + inv_H[4] * sy_f + inv_H[5]) / Wp);
            }
            else
#endif
            {
                float a, b, c, d, ftx, fty;
                er_transform_compute_matrix(node,
                                            node->computed.x,
                                            node->computed.y,
                                            node->computed.w,
                                            node->computed.h,
                                            &a,
                                            &b,
                                            &c,
                                            &d,
                                            &ftx,
                                            &fty);
                float ia, ib, ic, id, itx, ity;
                if (!er_transform_invert(a, b, c, d, ftx, fty, &ia, &ib, &ic, &id, &itx, &ity))
                    return false;
                er_transform_map_point(ia, ib, ic, id, itx, ity, x, y, &qx, &qy);
            }
        }
        else
#endif
        {
            qx = x - (int)node->tp_translate_x;
            qy = y - (int)node->tp_translate_y;
        }
    }
    *out_x = qx;
    *out_y = qy;
    return true;
}

static bool point_inside_transformed_with_slop(const ERNode* node, int x, int y)
{
    int qx, qy;
    if (!node_map_point(node, x, y, &qx, &qy))
        return false;
    return point_inside_node_with_slop(node, qx, qy);
}

/**
 * @brief Collects child tags into an array in append order.
 *
 * @param[in] parent    Parent node whose children should be collected.
 * @param[out] tags     Output child tag buffer.
 * @param[in] max_tags  Capacity of tags.
 *
 * @return Number of child tags written.
 */
static int collect_children(const ERNode* parent, uint16_t* tags, int max_tags)
{
    int count = 0;
    uint16_t child_tag = parent->first_child_tag;

    while (child_tag != ER_INVALID_TAG && count < max_tags)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;

        tags[count++] = child_tag;
        child_tag = child->next_sibling_tag;
    }

    return count;
}

/**
 * @brief Sorts child tags by zIndex while preserving append order for equal zIndex.
 *
 * @param[in,out] tags   Child tag array to sort.
 * @param[in] count      Number of tags in the array.
 */
static void sort_children_by_z_index(uint16_t* tags, int count)
{
    for (int i = 1; i < count; i++)
    {
        const uint16_t key = tags[i];
        const ERNode* key_node = er_get_node(key);
        const int16_t key_z = key_node ? key_node->z_index : 0;
        int j = i - 1;

        while (j >= 0)
        {
            const ERNode* node = er_get_node(tags[j]);
            const int16_t z = node ? node->z_index : 0;
            if (z <= key_z)
                break;
            tags[j + 1] = tags[j];
            j--;
        }

        tags[j + 1] = key;
    }
}

/**
 * @brief Accumulates the total scroll offset of all ScrollView/FlatList ancestors.
 *
 * Walking from node's parent up to the root, sums every ancestor's
 * scroll_offset_x/y.  Adding the result to a raw screen-space coordinate
 * converts it into the layout coordinate space where computed.x/y live,
 * so that point_inside_transformed_with_slop gives the correct result even
 * when an ancestor scroll view has a non-zero scroll offset.
 *
 * @param[in]  node   Node whose ancestor offsets to accumulate (excluding self).
 * @param[out] out_x  Total horizontal scroll offset.
 * @param[out] out_y  Total vertical scroll offset.
 */
static void accumulate_scroll_offsets(const ERNode* node, int* out_x, int* out_y)
{
    int sx = 0, sy = 0;
    const ERNode* n = er_get_node(node->parent_tag);
    while (n)
    {
        if (n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_FLAT_LIST)
        {
            sx += (int)n->scroll_offset_x;
            sy += (int)n->scroll_offset_y;
        }
        n = er_get_node(n->parent_tag);
    }
    *out_x = sx;
    *out_y = sy;
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

    const uint8_t pe = node->pointer_events;

    /* pointer_events:none — neither this node nor any descendant is hittable. */
    if (pe == ER_POINTER_EVENTS_NONE)
        return NULL;

    /* Convert the screen-space query point into the coordinate space where the node's computed rect
     * lives. node_map_point() owns that decision for every input path, so the entry gate here and the
     * press-inside test below cannot drift apart — a second copy of it living here is what let a 3D
     * node be gated by its 2D matrix while node_map_point back-projected the homography, and what let
     * #141 be fixed in one place and not the other. It fails only on a transform with no inverse (a
     * scale collapsed to nothing, a point behind the perspective plane), which is not hittable. */
    int qx, qy;
    if (!node_map_point(node, x, y, &qx, &qy))
        return NULL;

    /* Gate entry on the slop-extended bounds (using the transform-adjusted query). An Arc's knob and touch
     * slop reach past its box, so it gates on a wider rect and decides precisely below. */
    if (node->type == ER_NODE_ARC)
    {
        const int ext = (int)node->arc_overhang + 16;
        if (qx < (int)node->computed.x - ext || qy < (int)node->computed.y - ext
            || qx >= (int)node->computed.x + (int)node->computed.w + ext
            || qy >= (int)node->computed.y + (int)node->computed.h + ext)
            return NULL;
    }
    else if (!point_inside_node_with_slop(node, qx, qy))
        return NULL;

    /* Recurse into children unless box-only. */
    if (pe != ER_POINTER_EVENTS_BOX_ONLY)
    {
        /* overflow:hidden and overflow:scroll clip child hit-testing to strict bounds. */
        const bool clips = (node->layout.overflow == ER_OVERFLOW_HIDDEN || node->layout.overflow == ER_OVERFLOW_SCROLL);

        if (!clips || point_inside_node(node, qx, qy))
        {
            /* Translate query point into content space for ScrollView nodes so that
             * children are tested against their layout-computed positions. */
            const bool node_scrolls = (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST);
            const int child_x = node_scrolls ? qx + (int)node->scroll_offset_x : qx;
            const int child_y = node_scrolls ? qy + (int)node->scroll_offset_y : qy;

            uint16_t child_tags[ERUI_MAX_NODES];
            const int child_count = collect_children(node, child_tags, ERUI_MAX_NODES);
            sort_children_by_z_index(child_tags, child_count);

            for (int i = child_count - 1; i >= 0; i--)
            {
                ERNode* child = er_get_node(child_tags[i]);
                if (!child)
                    continue;

                ERNode* child_hit = hit_test_node(child, child_x, child_y);
                if (child_hit)
                    return child_hit;
            }
        }
    }

    /* pointer_events:box-none — this node is not hittable itself; children are. */
    if (pe == ER_POINTER_EVENTS_BOX_NONE)
        return NULL;

    /* An Arc is hittable only on its ring (plus slop) and knob — the hole and the unswept gap fall through
     * to whatever is behind, so a centre readout or a sibling under the dial still gets its taps. */
    if (node->type == ER_NODE_ARC && !er_arc_hit(node, qx, qy))
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
    return node && event <= ER_EVENT_LAYOUT && node->events[event].fn;
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
    node->events[event].fn(node, &data, node->events[event].user_data);
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
    node->events[event].fn(node, data, node->events[event].user_data);
}

/**
 * @brief Dispatches a raw touch event from target up through ancestors.
 *
 * @param[in] target  Original target node.
 * @param[in] event   Raw touch event type.
 * @param[in] data    Event payload to forward to each handler.
 */
static void dispatch_bubble_data(ERNode* target, EREventType event, const EREventData* data)
{
    ERNode* node = target;
    while (node)
    {
        if (node_takes_own_touches(node))
            dispatch_to_node_data(node, event, data);
        node = er_get_node(node->parent_tag);
    }
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
    data.vx = touch->vel_x;
    data.vy = touch->vel_y;
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
    const uint32_t elapsed = now_ms - touch->prev_move_time_ms;
    const bool sampled = elapsed > 0U && elapsed <= ER_SCROLL_VEL_WINDOW;
    if (sampled)
    {
        touch->vel_x = (float)(x - touch->prev_move_x) / (float)elapsed;
        touch->vel_y = (float)(y - touch->prev_move_y) / (float)elapsed;
    }
    touch->prev_move_x = x;
    touch->prev_move_y = y;
    touch->prev_move_time_ms = now_ms;
    return sampled;
}

/**
 * @brief Builds a leaf-to-root array of node tags starting from a given node.
 *
 * chain[0] is start (leaf); chain[return_value - 1] is the root-most ancestor reached.
 *
 * @param[in]  start    Leaf node to start from.
 * @param[out] chain    Output tag buffer (caller-allocated).
 * @param[in]  max_len  Capacity of chain.
 *
 * @return Number of tags written into chain.
 */
static int build_ancestor_chain(const ERNode* start, uint16_t* chain, int max_len)
{
    int count = 0;
    const ERNode* node = start;
    while (node && count < max_len)
    {
        chain[count++] = node->tag;
        node = er_get_node(node->parent_tag);
    }
    return count;
}

/**
 * @brief Runs capture-then-bubble responder negotiation along an ancestor chain.
 *
 * Iterates root→leaf (capture phase) then leaf→root (bubble phase), calling the
 * corresponding query callback on each node. Returns the first node whose callback
 * returns true, or NULL if no node claims the responder.
 *
 * @param[in] chain          Tag array built by build_ancestor_chain (chain[0]=leaf).
 * @param[in] chain_len      Number of entries in chain.
 * @param[in] capture_query  Query type for the capture phase.
 * @param[in] bubble_query   Query type for the bubble phase.
 * @param[in] data           Event data forwarded to each callback.
 *
 * @return The claiming node, or NULL when no node claims the responder.
 */
static ERNode* negotiate_responder(const uint16_t* chain,
                                   int chain_len,
                                   ERResponderQuery capture_query,
                                   ERResponderQuery bubble_query,
                                   const EREventData* data)
{
    /* Capture phase: root → leaf */
    for (int i = chain_len - 1; i >= 0; i--)
    {
        ERNode* node = er_get_node(chain[i]);
        if (!node_takes_own_touches(node))
            continue;
        const ERResponderQueryHandler* h = &node->queries[(uint8_t)capture_query];
        if (h->fn && h->fn(node, data, h->user_data))
            return node;
    }
    /* Bubble phase: leaf → root */
    for (int i = 0; i < chain_len; i++)
    {
        ERNode* node = er_get_node(chain[i]);
        if (!node_takes_own_touches(node))
            continue;
        const ERResponderQueryHandler* h = &node->queries[(uint8_t)bubble_query];
        if (h->fn && h->fn(node, data, h->user_data))
            return node;
    }
    return NULL;
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
    if (touch->responder_tag == ER_INVALID_TAG)
        return;
    ERNode* responder = er_get_node(touch->responder_tag);
    if (responder)
        dispatch_to_node_data(responder, ER_EVENT_RESPONDER_TERMINATE, data);
    touch->responder_tag = ER_INVALID_TAG;
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
 */
static void grant_responder(ERTouchState* touch, ERNode* node, const EREventData* data)
{
    touch->responder_tag = node->tag;
    dispatch_to_node_data(node, ER_EVENT_RESPONDER_GRANT, data);

    if (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST)
    {
        touch->initial_scroll_x = node->scroll_offset_x;
        touch->initial_scroll_y = node->scroll_offset_y;
    }
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
 */
static void cancel_touch(ERTouchState* touch, uint8_t finger_id, int x, int y)
{
    if (!touch->active)
        return;

    ERNode* touch_target = er_get_node(touch->touch_target_tag);
    ERNode* press_target = er_get_node(touch->press_target_tag);

    const EREventData rdata = gesture_data(touch, x, y);
    dispatch_bubble_data(touch_target, ER_EVENT_TOUCH_CANCEL, &rdata);
    if (press_target && touch->inside)
        dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);

    arc_drag_end(touch, finger_id);
    terminate_responder_if_active(touch, &rdata);

    reset_touch(touch);
}

/**
 * @brief Maps a screen point into an Arc's own coordinate space (ancestor scroll, then its transform).
 *
 * Every native-drag geometry query goes through this, so the ring hit test, the end latch and the value
 * lookup all agree with each other and with the ordinary hit test. Without it a translated or scaled dial
 * is visibly under the finger yet answers from raw screen pixels, so it never becomes the drag target.
 *
 * @param[in]  arc   Arc node.
 * @param[in]  x,y   Screen-space point.
 * @param[out] qx,qy Receives the arc-space point.
 *
 * @return false when the point projects behind the arc's perspective plane.
 */
static bool arc_local_point(const ERNode* arc, int x, int y, int* qx, int* qy)
{
    int sx = 0, sy = 0;
    accumulate_scroll_offsets(arc, &sx, &sy);
    return node_map_point(arc, x + sx, y + sy, qx, qy);
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
    ERNode* n = hit;
    while (n)
    {
        if (n->type == ER_NODE_ARC)
        {
            if (!n->props.arc.adjustable)
                return NULL;

            if (!node_takes_own_touches(n))
                return NULL;
            int qx, qy;
            if (!arc_local_point(n, x, y, &qx, &qy))
                return NULL;
            return er_arc_hit(n, qx, qy) ? n : NULL;
        }
        if (has_handler(n, ER_EVENT_PRESS) || has_handler(n, ER_EVENT_LONG_PRESS)
            || has_handler(n, ER_EVENT_TOUCH_START) || has_handler(n, ER_EVENT_TOUCH_MOVE))
            return NULL; /* an interactive node above the dial owns this touch */
        n = er_get_node(n->parent_tag);
    }
    return NULL;
}

/**
 * @brief Returns the Arc node a touch slot is natively dragging, or NULL.
 */
static ERNode* active_arc_drag(const ERTouchState* touch, uint8_t finger_id)
{
    ERNode* r = er_get_node(touch->responder_tag);
    return (r && r->type == ER_NODE_ARC && r->arc_drag_finger == (int8_t)finger_id) ? r : NULL;
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
    int qx, qy;
    if (!arc_local_point(arc, x, y, &qx, &qy))
        return;
    const float v = er_arc_value_at(arc, qx, qy, anti_wrap);
    /* RANGE mode moves only the end the gesture latched onto on touch-down. */
    const bool changed = arc->arc_drag_low ? er_arc_apply_value_start(arc, v) : er_arc_apply_value(arc, v);
    if (changed)
    {
        er_mark_dirty_upward(arc);
        const EREventHandler* h = &arc->events[ER_EVENT_VALUE_CHANGE];
        if (h->fn)
        {
            EREventData d = {0};
            d.x = x;
            d.y = y;
            d.value = arc->arc_value;
            d.value_start = arc->props.arc.range ? arc->arc_value_start : arc->arc_value;
            h->fn(arc, &d, h->user_data);
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

void er_event_set(ERNode* node, EREventType event, EREventFn fn, void* user_data)
{
    if (!node || (unsigned)event >= (unsigned)ER_EVENT_TYPE_COUNT_)
        return;

    node->events[event].fn = fn;
    node->events[event].user_data = user_data;
}

void er_responder_query_set(ERNode* node, ERResponderQuery query, ERResponderQueryFn fn, void* user_data)
{
    if (!node || (uint8_t)query >= ER_RESPONDER_QUERY_COUNT)
        return;

    node->queries[(uint8_t)query].fn = fn;
    node->queries[(uint8_t)query].user_data = user_data;
}

int er_touch_active_count(void)
{
    int count = 0;
    for (uint8_t i = 0U; i < (uint8_t)ER_MAX_TOUCHES; i++)
    {
        if (s_touches[i].active)
            count++;
    }
    return count;
}

void er_input_reset(void)
{
    for (int i = 0; i < ER_MAX_TOUCHES; i++)
        reset_touch(&s_touches[i]);

    /* Drop any coalesced move: it names a node from the scene being torn down. The coalescing policy
     * itself is the host's, not the scene's, so s_coalesce_moves survives the reset. */
    memset(s_pending_moves, 0, sizeof(s_pending_moves));
    s_flushing_moves = false;

    /* Zero momentum velocities on every pool node so that scroll state from a previous
     * scene (or a previous test) cannot outlive the backend reset and fire a stale
     * event callback.  er_get_node() returns NULL for unused slots. */
    for (uint16_t tag = 0U; tag < (uint16_t)ERUI_MAX_NODES; tag++)
    {
        ERNode* n = er_get_node(tag);
        if (n && (n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_FLAT_LIST))
        {
            n->scroll_vel_x = 0.0f;
            n->scroll_vel_y = 0.0f;
        }
    }
}

void er_input_tick(uint32_t delta_ms)
{
    for (int i = 0; i < ER_MAX_TOUCHES; i++)
    {
        ERTouchState* touch = &s_touches[i];
        if (!touch->active || touch->long_press_fired || touch->long_press_cancelled || !touch->inside)
            continue;

        if (UINT32_MAX - touch->elapsed_ms < delta_ms)
            touch->elapsed_ms = UINT32_MAX;
        else
            touch->elapsed_ms += delta_ms;

        if (touch->elapsed_ms >= ER_LONG_PRESS_MS)
        {
            ERNode* press_target = er_get_node(touch->press_target_tag);
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
        ERNode* sv = er_get_node(tag);
        if (!sv || (sv->type != ER_NODE_SCROLL_VIEW && sv->type != ER_NODE_FLAT_LIST))
            continue;
        if (sv->scroll_vel_x == 0.0f && sv->scroll_vel_y == 0.0f)
            continue;

        sv->scroll_vel_x *= factor;
        sv->scroll_vel_y *= factor;

        if (fabsf(sv->scroll_vel_x) < ER_SCROLL_VEL_STOP)
            sv->scroll_vel_x = 0.0f;
        if (fabsf(sv->scroll_vel_y) < ER_SCROLL_VEL_STOP)
            sv->scroll_vel_y = 0.0f;

        er_scroll_view_set_offset(sv,
                                  sv->scroll_offset_x + sv->scroll_vel_x * (float)delta_ms,
                                  sv->scroll_offset_y + sv->scroll_vel_y * (float)delta_ms);
    }
}

void er_input_queue_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    if (finger_id >= ER_MAX_TOUCHES)
        return;

    ERPendingMove* pm = &s_pending_moves[finger_id];

    if (phase == ER_TOUCH_MOVE && s_coalesce_moves)
    {
        pm->x = x;
        pm->y = y;
        pm->pending = true;
        return;
    }

    /* Down, up and cancel carry the shape of the gesture rather than just a position, so they are never
     * coalesced. A move parked behind one is flushed first, so the app still sees the finger travel to
     * the release point before it sees the release. */
    flush_finger_move(finger_id);
    pm->last_x = x;
    pm->last_y = y;
    pm->has_last = true;
    er_dispatch_touch(finger_id, phase, x, y);
}

void er_input_flush_moves(void)
{
    if (s_flushing_moves)
        return; /* a handler re-entered the frame path (e.g. NativeUI.commit()) — don't recurse */

    s_flushing_moves = true;
    for (uint8_t i = 0U; i < (uint8_t)ER_MAX_TOUCHES; i++)
        flush_finger_move(i);
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

    switch (phase)
    {
        case ER_TOUCH_DOWN:
        {
            cancel_touch(touch, finger_id, x, y);

            ERNode* hit = hit_test(x, y);
            ERNode* press_target = nearest_press_target(hit);

            touch->active = hit != NULL;
            if (press_target)
            {
                int sx = 0, sy = 0;
                accumulate_scroll_offsets(press_target, &sx, &sy);
                touch->inside = point_inside_transformed_with_slop(press_target, x + sx, y + sy);
            }
            else
            {
                touch->inside = false;
            }
            touch->long_press_fired = false;
            touch->long_press_cancelled = false;
            touch->elapsed_ms = 0U;
            touch->last_x = x;
            touch->last_y = y;
            touch->start_x = x;
            touch->start_y = y;
            touch->prev_move_x = x;
            touch->prev_move_y = y;
            touch->prev_move_time_ms = er_now_ms();
            touch->vel_x = 0.0f;
            touch->vel_y = 0.0f;
            touch->press_target_tag = press_target ? press_target->tag : ER_INVALID_TAG;
            touch->touch_target_tag = hit ? hit->tag : ER_INVALID_TAG;

            const EREventData ddata = gesture_data(touch, x, y);
            dispatch_bubble_data(hit, ER_EVENT_TOUCH_START, &ddata);
            dispatch_to_node(press_target, ER_EVENT_PRESS_IN, x, y);

            /* Auto-focus TextInput on press; blur any focused TextInput when tapping
             * anything else so the keyboard is dismissed on outside taps. */
            if (press_target && press_target->type == ER_NODE_TEXT_INPUT && press_target->props.text_input.editable)
                er_text_input_focus(press_target);
            else
                er_text_input_blur();

            /* Gesture responder negotiation: start-should-set */
            if (hit)
            {
                uint16_t chain[ERUI_MAX_NODES];
                const int chain_len = build_ancestor_chain(hit, chain, ERUI_MAX_NODES);
                ERNode* claimant = negotiate_responder(
                    chain, chain_len, ER_QUERY_START_SHOULD_SET_CAPTURE, ER_QUERY_START_SHOULD_SET, &ddata);
                if (claimant)
                    grant_responder(touch, claimant, &ddata);

                /* Built-in Arc drag-to-set: an adjustable Arc under the finger takes the gesture natively —
                 * over a JS claimant and ahead of any ScrollView's auto-scroll — and jumps to the touched
                 * point. The responder stays with it until release, so a scroller never steals the drag. */
                ERNode* arc = nearest_arc_drag_target(hit, x, y);
                /* A dial has ONE value, so one finger drives it: if another is already dragging this
                 * arc, leave the gesture alone rather than re-latching its end and fighting over the
                 * value (and so that lifting either finger doesn't end the other's drag). */
                if (arc && arc->arc_drag_finger >= 0 && arc->arc_drag_finger != (int8_t)finger_id)
                    arc = NULL;
                if (arc)
                {
                    if (touch->responder_tag != arc->tag)
                    {
                        terminate_responder_if_active(touch, &ddata);
                        grant_responder(touch, arc, &ddata);
                    }
                    arc->arc_drag_finger = (int8_t)finger_id;
                    /* Latch which end of a RANGE band this gesture owns, ONCE, at the point it started —
                     * so dragging one setpoint past the other does not hand the finger to its neighbour. */
                    {
                        int qx, qy;
                        arc->arc_drag_low = arc_local_point(arc, x, y, &qx, &qy) && er_arc_grab_low(arc, qx, qy);
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

            ERNode* touch_target = er_get_node(touch->touch_target_tag);
            ERNode* press_target = er_get_node(touch->press_target_tag);
            touch->last_x = x;
            touch->last_y = y;

            /* Sample speed FIRST: everything dispatched below — the raw move, the responder move, the
             * should-set queries — then reports the same, current velocity. */
            const bool vel_sampled = track_touch_velocity(touch, x, y);
            const EREventData rdata = gesture_data(touch, x, y);
            const int dx = rdata.dx;
            const int dy = rdata.dy;
            dispatch_bubble_data(touch_target, ER_EVENT_TOUCH_MOVE, &rdata);

            if (press_target)
            {
                int sx = 0, sy = 0;
                accumulate_scroll_offsets(press_target, &sx, &sy);
                const bool inside = point_inside_transformed_with_slop(press_target, x + sx, y + sy);
                if (!inside)
                    touch->long_press_cancelled = true;
                if (inside != touch->inside)
                {
                    dispatch_to_node(press_target, inside ? ER_EVENT_PRESS_IN : ER_EVENT_PRESS_OUT, x, y);
                    touch->inside = inside;
                }
            }

            /* Dispatch move to the current gesture responder */
            ERNode* responder = er_get_node(touch->responder_tag);
            if (responder)
                dispatch_to_node_data(responder, ER_EVENT_RESPONDER_MOVE, &rdata);

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
            if (touch_target)
            {
                uint16_t chain[ERUI_MAX_NODES];
                const int chain_len = build_ancestor_chain(touch_target, chain, ERUI_MAX_NODES);
                ERNode* claimant = negotiate_responder(
                    chain, chain_len, ER_QUERY_MOVE_SHOULD_SET_CAPTURE, ER_QUERY_MOVE_SHOULD_SET, &rdata);

                if (claimant && claimant != responder)
                {
                    bool yields = true;
                    if (responder)
                    {
                        const ERResponderQueryHandler* rq = &responder->queries[ER_QUERY_TERMINATION_REQUEST];
                        if (rq->fn)
                            yields = rq->fn(responder, &rdata, rq->user_data);
                    }

                    if (yields)
                    {
                        /* A scroller losing the gesture must stop DEAD, not coast: its live velocity
                         * was fed by the very moves that are now the claimant's, so momentum after
                         * the handover would fight the new responder's gesture. */
                        if (responder
                            && (responder->type == ER_NODE_SCROLL_VIEW || responder->type == ER_NODE_FLAT_LIST))
                        {
                            responder->scroll_vel_x = 0.0f;
                            responder->scroll_vel_y = 0.0f;
                        }
                        terminate_responder_if_active(touch, &rdata);
                        grant_responder(touch, claimant, &rdata);
                    }
                    else
                    {
                        reject_responder(claimant, &rdata);
                    }
                }
            }

            /* Auto-scroll: if no responder was claimed and the pan exceeds slop, find the
             * nearest ScrollView ancestor and grant it automatically. */
            if (touch->responder_tag == ER_INVALID_TAG && touch_target)
            {
                const int abs_dx = dx < 0 ? -dx : dx;
                const int abs_dy = dy < 0 ? -dy : dy;
                if (abs_dx >= ER_SCROLL_SLOP || abs_dy >= ER_SCROLL_SLOP)
                {
                    ERNode* sv = find_scroll_view_ancestor(touch_target);
                    if (sv)
                        grant_responder(touch, sv, &rdata);
                }
            }

            /* Apply incremental scroll to any active ScrollView responder. */
            {
                ERNode* active = er_get_node(touch->responder_tag);
                if (active && (active->type == ER_NODE_SCROLL_VIEW || active->type == ER_NODE_FLAT_LIST))
                {
                    er_scroll_view_set_offset(
                        active, touch->initial_scroll_x - (float)dx, touch->initial_scroll_y - (float)dy);

                    /* Update live velocity so that TOUCH_UP always inherits the most recent movement
                     * speed, even when UP follows MOVE in the same tick. The content moves OPPOSITE the
                     * finger, hence the sign flip on the shared sample. */
                    if (vel_sampled)
                    {
                        active->scroll_vel_x = -touch->vel_x;
                        active->scroll_vel_y = -touch->vel_y;
                    }
                }
            }
            break;
        }
        case ER_TOUCH_UP:
        {
            if (!touch->active)
                break;

            ERNode* touch_target = er_get_node(touch->touch_target_tag);
            ERNode* press_target = er_get_node(touch->press_target_tag);
            bool inside = false;
            if (press_target)
            {
                int sx = 0, sy = 0;
                accumulate_scroll_offsets(press_target, &sx, &sy);
                inside = point_inside_transformed_with_slop(press_target, x + sx, y + sy);
            }
            /* The velocity reported here is the LAST MOVE's, not a fresh sample: a fling ends with the
             * finger resting for a frame or two, and measuring across that rest reports zero and throws
             * the throw away. (The ScrollView's momentum below deliberately DOES re-sample — coming to
             * rest before lifting should not launch the content.) */
            const EREventData udata = gesture_data(touch, x, y);

            dispatch_bubble_data(touch_target, ER_EVENT_TOUCH_END, &udata);
            if (press_target)
            {
                /* Every dispatch below can run app code, and under the synchronous QuickJS root that code
                 * can commit a React update that unmounts this node — returning its pool slot, possibly to
                 * a different node. So re-fetch by TAG between dispatches and stop if it went away, rather
                 * than carrying the raw pointer across a callback. */
                const uint16_t press_tag = press_target->tag;
                if (touch->inside)
                {
                    dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);
                    press_target = er_get_node(press_tag);
                }
                if (inside && press_target)
                {
                    /* Built-in Switch toggle: flipping value on press kicks off the
                     * 200 ms thumb animation in er_node_set_props. The user's
                     * ER_EVENT_PRESS handler (if any) runs immediately after. */
                    if (press_target->type == ER_NODE_SWITCH)
                    {
                        const uint8_t new_val = press_target->props.sw.value ? 0U : 1U;
                        press_target->props.sw.value = new_val;
                        ERAnimConfig cfg;
                        memset(&cfg, 0, sizeof(cfg));
                        cfg.type = ER_ANIM_TIMING;
                        cfg.easing = ER_EASE_EASE_IN_OUT;
                        cfg.duration_ms = 200U;
                        er_anim_start(press_target, ER_PROP_SWITCH_THUMB, new_val ? 1.0f : 0.0f, &cfg);
                        er_mark_dirty_upward(press_target);
                        /* onValueChange: the new value, so a host needs no PRESS-then-guess round-trip. */
                        const EREventHandler* vh = &press_target->events[ER_EVENT_VALUE_CHANGE];
                        if (vh->fn)
                        {
                            EREventData vd = {0};
                            vd.x = x;
                            vd.y = y;
                            vd.value = new_val ? 1.0f : 0.0f;
                            vd.value_start = vd.value;
                            vh->fn(press_target, &vd, vh->user_data);
                            press_target = er_get_node(press_tag);
                        }
                    }
                    if (press_target)
                        dispatch_to_node(press_target, ER_EVENT_PRESS, x, y);
                }
            }

            /* Release the gesture responder */
            arc_drag_end(touch, finger_id);
            ERNode* responder = er_get_node(touch->responder_tag);
            if (responder)
            {
                dispatch_to_node_data(responder, ER_EVENT_RESPONDER_RELEASE, &udata);

                /* Final velocity update for ScrollView responders: if real time elapsed
                 * between the last MOVE and this UP (finger held still before release),
                 * re-sample to capture deceleration.  Velocity was already set during the
                 * last MOVE and is kept unchanged when elapsed == 0. */
                if (responder->type == ER_NODE_SCROLL_VIEW || responder->type == ER_NODE_FLAT_LIST)
                {
                    const uint32_t now_ms = er_now_ms();
                    const uint32_t elapsed = now_ms - touch->prev_move_time_ms;
                    if (elapsed > 0U && elapsed <= ER_SCROLL_VEL_WINDOW)
                    {
                        responder->scroll_vel_x = -(float)(x - touch->prev_move_x) / (float)elapsed;
                        responder->scroll_vel_y = -(float)(y - touch->prev_move_y) / (float)elapsed;
                    }
                }
            }

            reset_touch(touch);
            break;
        }
        case ER_TOUCH_CANCEL:
        {
            cancel_touch(touch, finger_id, x, y);
            break;
        }
        default:
            break;
    }
}
