#include "er_node_internal.h"
#include "renderer_internal.h"
#include <stdbool.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_MAX_TOUCHES 5U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Per-finger press tracking state.
 */
typedef struct
{
    bool active;
    bool inside;
    uint16_t press_target_tag;
    uint16_t touch_target_tag;
} ERTouchState;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERTouchState s_touches[ER_MAX_TOUCHES];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Returns whether a point lies inside a node's computed rectangle.
 *
 * @param[in] node  Node whose computed rectangle should be tested.
 * @param[in] x     X coordinate in framebuffer pixels.
 * @param[in] y     Y coordinate in framebuffer pixels.
 *
 * @return true when the point is inside node.
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
 * @brief Recursively finds the topmost deepest node under a point.
 *
 * Children appended later are treated as visually above earlier siblings because the
 * renderer paints them later.
 *
 * @param[in] node  Subtree root to inspect.
 * @param[in] x     X coordinate in framebuffer pixels.
 * @param[in] y     Y coordinate in framebuffer pixels.
 *
 * @return Best hit node, or NULL when the point is outside the subtree.
 */
static ERNode* hit_test_node(ERNode* node, int x, int y)
{
    if (!node || !point_inside_node(node, x, y))
        return NULL;

    ERNode* best = node;
    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;

        ERNode* child_hit = hit_test_node(child, x, y);
        if (child_hit)
            best = child_hit;

        child_tag = child->next_sibling_tag;
    }

    return best;
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
 * @brief Finds the nearest ancestor, including self, with a handler for an event.
 *
 * @param[in] node   Starting node.
 * @param[in] event  Event type.
 *
 * @return Matching node, or NULL if none was found before the root.
 */
static ERNode* nearest_handler(ERNode* node, EREventType event)
{
    while (node)
    {
        if (has_handler(node, event))
            return node;
        node = er_get_node(node->parent_tag);
    }
    return NULL;
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
        if (has_handler(node, ER_EVENT_PRESS) || has_handler(node, ER_EVENT_PRESS_IN) ||
            has_handler(node, ER_EVENT_PRESS_OUT))
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

void er_dispatch_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    if (finger_id >= ER_MAX_TOUCHES)
        return;

    ERTouchState* touch = &s_touches[finger_id];
    ERNode* hit = hit_test(x, y);

    switch (phase)
    {
        case ER_TOUCH_DOWN:
        {
            ERNode* press_target = nearest_press_target(hit);
            ERNode* touch_target = nearest_handler(hit, ER_EVENT_TOUCH_START);

            touch->active = true;
            touch->inside = press_target ? point_inside_node(press_target, x, y) : false;
            touch->press_target_tag = press_target ? press_target->tag : ER_INVALID_TAG;
            touch->touch_target_tag = touch_target ? touch_target->tag : ER_INVALID_TAG;

            dispatch_to_node(touch_target, ER_EVENT_TOUCH_START, x, y);
            dispatch_to_node(press_target, ER_EVENT_PRESS_IN, x, y);
            break;
        }
        case ER_TOUCH_MOVE:
        {
            if (!touch->active)
                break;

            ERNode* touch_target = er_get_node(touch->touch_target_tag);
            ERNode* press_target = er_get_node(touch->press_target_tag);
            dispatch_to_node(touch_target, ER_EVENT_TOUCH_MOVE, x, y);

            if (press_target)
            {
                const bool inside = point_inside_node(press_target, x, y);
                if (inside != touch->inside)
                {
                    dispatch_to_node(press_target, inside ? ER_EVENT_PRESS_IN : ER_EVENT_PRESS_OUT, x, y);
                    touch->inside = inside;
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
            const bool inside = press_target && point_inside_node(press_target, x, y);

            dispatch_to_node(touch_target, ER_EVENT_TOUCH_END, x, y);
            if (press_target)
            {
                if (inside)
                    dispatch_to_node(press_target, ER_EVENT_PRESS, x, y);
                if (touch->inside)
                    dispatch_to_node(press_target, ER_EVENT_PRESS_OUT, x, y);
            }

            touch->active = false;
            touch->inside = false;
            touch->press_target_tag = ER_INVALID_TAG;
            touch->touch_target_tag = ER_INVALID_TAG;
            break;
        }
        case ER_TOUCH_CANCEL:
        {
            if (touch->active && touch->inside)
                dispatch_to_node(er_get_node(touch->press_target_tag), ER_EVENT_PRESS_OUT, x, y);

            touch->active = false;
            touch->inside = false;
            touch->press_target_tag = ER_INVALID_TAG;
            touch->touch_target_tag = ER_INVALID_TAG;
            break;
        }
        default:
            break;
    }
}
