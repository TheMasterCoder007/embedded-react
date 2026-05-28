#include "er_node_internal.h"
#include "renderer_internal.h"
#include <stdbool.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_MAX_TOUCHES 5U
#define ER_LONG_PRESS_MS 500U

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
    int start_x; /**< X coordinate at touch-down; used to compute dx for gesture events. */
    int start_y; /**< Y coordinate at touch-down; used to compute dy for gesture events. */
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
    if (node->type == ER_NODE_VIEW || node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_PRESSABLE
        || node->type == ER_NODE_MODAL)
    {
        if (node->props.view.opacity == 0)
            return true;
    }
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
 * @brief Recursively finds the topmost deepest hittable node under a point.
 *
 * Respects pointer_events, hitSlop, and overflow:hidden clipping. Children appended
 * later win over earlier siblings; higher zIndex wins over lower.
 *
 * @param[in] node  Subtree root to inspect.
 * @param[in] x     X coordinate in framebuffer pixels.
 * @param[in] y     Y coordinate in framebuffer pixels.
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

    /* Gate entry on the slop-extended bounds. */
    if (!point_inside_node_with_slop(node, x, y))
        return NULL;

    /* Recurse into children unless box-only. */
    if (pe != ER_POINTER_EVENTS_BOX_ONLY)
    {
        /* overflow:hidden and overflow:scroll clip child hit-testing to strict bounds. */
        const bool clips = (node->layout.overflow == ER_OVERFLOW_HIDDEN || node->layout.overflow == ER_OVERFLOW_SCROLL);

        if (!clips || point_inside_node(node, x, y))
        {
            uint16_t child_tags[ERUI_MAX_NODES];
            const int child_count = collect_children(node, child_tags, ERUI_MAX_NODES);
            sort_children_by_z_index(child_tags, child_count);

            for (int i = child_count - 1; i >= 0; i--)
            {
                ERNode* child = er_get_node(child_tags[i]);
                if (!child)
                    continue;

                ERNode* child_hit = hit_test_node(child, x, y);
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
        if (has_handler(node, ER_EVENT_PRESS) || has_handler(node, ER_EVENT_LONG_PRESS)
            || has_handler(node, ER_EVENT_PRESS_IN) || has_handler(node, ER_EVENT_PRESS_OUT))
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
 * @param[in,out] touch  Touch slot to update.
 * @param[in]     node   Node to grant.
 * @param[in]     data   Event data forwarded to the grant callback.
 */
static void grant_responder(ERTouchState* touch, ERNode* node, const EREventData* data)
{
    touch->responder_tag = node->tag;
    dispatch_to_node_data(node, ER_EVENT_RESPONDER_GRANT, data);
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
    if (!node || event > ER_EVENT_LAYOUT)
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
}

void er_dispatch_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    if (finger_id >= ER_MAX_TOUCHES)
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
            touch->inside = press_target ? point_inside_node_with_slop(press_target, x, y) : false;
            touch->long_press_fired = false;
            touch->long_press_cancelled = false;
            touch->elapsed_ms = 0U;
            touch->last_x = x;
            touch->last_y = y;
            touch->start_x = x;
            touch->start_y = y;
            touch->press_target_tag = press_target ? press_target->tag : ER_INVALID_TAG;
            touch->touch_target_tag = hit ? hit->tag : ER_INVALID_TAG;

            dispatch_bubble(hit, ER_EVENT_TOUCH_START, x, y);
            dispatch_to_node(press_target, ER_EVENT_PRESS_IN, x, y);

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
                const bool inside = point_inside_node_with_slop(press_target, x, y);
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
            break;
        }
        case ER_TOUCH_UP:
        {
            if (!touch->active)
                break;

            ERNode* touch_target = er_get_node(touch->touch_target_tag);
            ERNode* press_target = er_get_node(touch->press_target_tag);
            const bool inside = press_target && point_inside_node_with_slop(press_target, x, y);
            const int dx = x - touch->start_x;
            const int dy = y - touch->start_y;

            dispatch_bubble(touch_target, ER_EVENT_TOUCH_END, x, y);
            if (press_target)
            {
                if (touch->inside)
                    dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);
                if (inside)
                    dispatch_to_node(press_target, ER_EVENT_PRESS, x, y);
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
