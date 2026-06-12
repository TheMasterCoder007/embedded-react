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
    float initial_scroll_x;     /**< ScrollView scroll_offset_x when the responder was granted. */
    float initial_scroll_y;     /**< ScrollView scroll_offset_y when the responder was granted. */
    uint16_t press_target_tag;
    uint16_t touch_target_tag;
    uint16_t responder_tag; /**< Node currently owning this touch as the gesture responder. */
} ERTouchState;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERTouchState s_touches[ER_MAX_TOUCHES];

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

/**
 * @brief Returns whether a screen-space point, after inverse-transforming through a node's
 *        2D transform, lies inside the node's slop-extended layout rectangle.
 *
 * For nodes without a transform this is identical to point_inside_node_with_slop().
 * For translated nodes the query is adjusted by the negative translation offset.
 * For full-affine nodes the full inverse matrix is applied.
 *
 * @param[in] node  Node to test.
 * @param[in] x     Screen-space X coordinate.
 * @param[in] y     Screen-space Y coordinate.
 *
 * @return true when the point maps into the node's slop-extended layout bounds.
 */
static bool point_inside_transformed_with_slop(const ERNode* node, int x, int y)
{
    int qx = x, qy = y;
    if (node->has_transform)
    {
#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
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
#if ERUI_TRANSFORMS_FULL
            if (!er_transform_is_translate_only(node))
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
        else
#endif
        {
            qx = x - (int)node->tp_translate_x;
            qy = y - (int)node->tp_translate_y;
        }
    }
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
 * @brief Finds the nearest ScrollView ancestor of a node (inclusive of the node itself).
 *
 * @param[in] node  Starting node.
 *
 * @return The first ScrollView found walking up the ancestor chain, or NULL.
 */
static ERNode* find_scroll_view_ancestor(ERNode* node)
{
    while (node)
    {
        if (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST)
            return node;
        node = er_get_node(node->parent_tag);
    }
    return NULL;
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

    /* Apply this node's transform (if any) to convert the screen-space query point into the
     * coordinate space where the node's computed rect lives.  For translate-only transforms,
     * subtract the translation offset.  For full affine transforms, apply the inverse matrix. */
    int qx = x, qy = y;
    if (node->has_transform)
    {
#if ERUI_TRANSFORMS_FULL
        if (!er_transform_is_translate_only(node))
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
                return NULL; /* Singular transform — not hittable. */
            er_transform_map_point(ia, ib, ic, id, itx, ity, x, y, &qx, &qy);
        }
        else
#endif
        {
            qx = x - (int)node->tp_translate_x;
            qy = y - (int)node->tp_translate_y;
        }
    }

    /* Gate entry on the slop-extended bounds (using the transform-adjusted query). */
    if (!point_inside_node_with_slop(node, qx, qy))
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
        if (has_handler(node, ER_EVENT_PRESS) || has_handler(node, ER_EVENT_LONG_PRESS)
            || has_handler(node, ER_EVENT_PRESS_IN) || has_handler(node, ER_EVENT_PRESS_OUT)
            || node->type == ER_NODE_TEXT_INPUT || node->type == ER_NODE_SWITCH)
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
 * @brief Dispatches a raw touch event from target up through ancestors.
 *
 * @param[in] target  Original target node.
 * @param[in] event   Raw touch event type.
 * @param[in] x       Touch X coordinate.
 * @param[in] y       Touch Y coordinate.
 */
static void dispatch_bubble(ERNode* target, EREventType event, int x, int y)
{
    ERNode* node = target;
    while (node)
    {
        dispatch_to_node(node, event, x, y);
        node = er_get_node(node->parent_tag);
    }
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
        if (!node)
            continue;
        const ERResponderQueryHandler* h = &node->queries[(uint8_t)capture_query];
        if (h->fn && h->fn(node, data, h->user_data))
            return node;
    }
    /* Bubble phase: leaf → root */
    for (int i = 0; i < chain_len; i++)
    {
        ERNode* node = er_get_node(chain[i]);
        if (!node)
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

/**
 * @brief Cancels an active touch sequence.
 *
 * @param[in,out] touch  Touch state to cancel.
 * @param[in] x          Touch X coordinate.
 * @param[in] y          Touch Y coordinate.
 */
static void cancel_touch(ERTouchState* touch, int x, int y)
{
    if (!touch->active)
        return;

    ERNode* touch_target = er_get_node(touch->touch_target_tag);
    ERNode* press_target = er_get_node(touch->press_target_tag);

    dispatch_bubble(touch_target, ER_EVENT_TOUCH_CANCEL, x, y);
    if (press_target && touch->inside)
        dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);

    EREventData rdata = {0};
    rdata.x = x;
    rdata.y = y;
    rdata.dx = x - touch->start_x;
    rdata.dy = y - touch->start_y;
    terminate_responder_if_active(touch, &rdata);

    reset_touch(touch);
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

void er_input_reset(void)
{
    for (int i = 0; i < ER_MAX_TOUCHES; i++)
        reset_touch(&s_touches[i]);

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

void er_dispatch_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    if (finger_id >= ER_MAX_TOUCHES)
        return;

    /* On-screen keyboard (if active) gets first refusal: taps inside its strip type into the focused input
     * and are consumed so they never reach the scene below it. */
    if (er_keyboard_dispatch_touch(phase, x, y))
        return;

    ERTouchState* touch = &s_touches[finger_id];

    switch (phase)
    {
        case ER_TOUCH_DOWN:
        {
            cancel_touch(touch, x, y);

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
            touch->press_target_tag = press_target ? press_target->tag : ER_INVALID_TAG;
            touch->touch_target_tag = hit ? hit->tag : ER_INVALID_TAG;

            dispatch_bubble(hit, ER_EVENT_TOUCH_START, x, y);
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
                EREventData data = {0};
                data.x = x;
                data.y = y;
                ERNode* claimant = negotiate_responder(
                    chain, chain_len, ER_QUERY_START_SHOULD_SET_CAPTURE, ER_QUERY_START_SHOULD_SET, &data);
                if (claimant)
                    grant_responder(touch, claimant, &data);
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
            dispatch_bubble(touch_target, ER_EVENT_TOUCH_MOVE, x, y);

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

            /* Build responder event data with cumulative displacement */
            const int dx = x - touch->start_x;
            const int dy = y - touch->start_y;
            EREventData rdata = {0};
            rdata.x = x;
            rdata.y = y;
            rdata.dx = dx;
            rdata.dy = dy;

            /* Dispatch move to the current gesture responder */
            ERNode* responder = er_get_node(touch->responder_tag);
            if (responder)
                dispatch_to_node_data(responder, ER_EVENT_RESPONDER_MOVE, &rdata);

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

                    /* Update live velocity so that TOUCH_UP always inherits the most
                     * recent movement speed, even when UP follows MOVE in the same tick. */
                    const uint32_t now_ms = er_now_ms();
                    const uint32_t vel_elapsed = now_ms - touch->prev_move_time_ms;
                    if (vel_elapsed > 0U && vel_elapsed <= ER_SCROLL_VEL_WINDOW)
                    {
                        active->scroll_vel_x = -(float)(x - touch->prev_move_x) / (float)vel_elapsed;
                        active->scroll_vel_y = -(float)(y - touch->prev_move_y) / (float)vel_elapsed;
                    }
                    touch->prev_move_x = x;
                    touch->prev_move_y = y;
                    touch->prev_move_time_ms = now_ms;
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
            const int dx = x - touch->start_x;
            const int dy = y - touch->start_y;

            dispatch_bubble(touch_target, ER_EVENT_TOUCH_END, x, y);
            if (press_target)
            {
                if (touch->inside)
                    dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);
                if (inside)
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
                    }
                    dispatch_to_node(press_target, ER_EVENT_PRESS, x, y);
                }
            }

            /* Release the gesture responder */
            ERNode* responder = er_get_node(touch->responder_tag);
            if (responder)
            {
                EREventData rdata = {0};
                rdata.x = x;
                rdata.y = y;
                rdata.dx = dx;
                rdata.dy = dy;
                dispatch_to_node_data(responder, ER_EVENT_RESPONDER_RELEASE, &rdata);

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
            cancel_touch(touch, x, y);
            break;
        }
        default:
            break;
    }
}
