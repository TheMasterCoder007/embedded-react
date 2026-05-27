#include "er_scene.h"
#include "native_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Counts event callbacks received during a test.
 */
typedef struct
{
    int press_count;
    int press_in_count;
    int press_out_count;
    int touch_start_count;
    int touch_end_count;
    int last_x;
    int last_y;
} EventCounts;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

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
    return p;
}

/**
 * @brief Records a press event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_press(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    counts->press_count++;
    counts->last_x = data->x;
    counts->last_y = data->y;
}

/**
 * @brief Records a press-in event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_press_in(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    (void)data;
    counts->press_in_count++;
}

/**
 * @brief Records a press-out event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_press_out(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    (void)data;
    counts->press_out_count++;
}

/**
 * @brief Records a touch-start event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_touch_start(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    (void)data;
    counts->touch_start_count++;
}

/**
 * @brief Records a touch-end event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_touch_end(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    (void)data;
    counts->touch_end_count++;
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

/**
 * @brief Creates a fixed-size root node.
 *
 * @return New root node.
 */
static ERNode* create_root(void)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.width = 240;
    p.height = 160;
    p.background_color = 0xFF000000U;
    er_node_set_props(root, &p);
    er_tree_set_root(root);
    return root;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point for hit-testing and basic press dispatch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    embedded_renderer_set_backend(NULL);

    ERNode* root = create_root();

    EventCounts parent_counts = {0};
    ERNode* pressable = er_node_create(ER_NODE_PRESSABLE);
    ERProps p = props_default();
    p.width = 120;
    p.height = 80;
    p.background_color = 0xFF101010U;
    er_node_set_props(pressable, &p);
    er_event_set(pressable, ER_EVENT_PRESS, on_press, &parent_counts);
    er_event_set(pressable, ER_EVENT_PRESS_IN, on_press_in, &parent_counts);
    er_event_set(pressable, ER_EVENT_PRESS_OUT, on_press_out, &parent_counts);
    er_event_set(pressable, ER_EVENT_TOUCH_START, on_touch_start, &parent_counts);
    er_event_set(pressable, ER_EVENT_TOUCH_END, on_touch_end, &parent_counts);

    ERNode* label = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.height = 24;
    p.color = 0xFFFFFFFFU;
    p.font_size = 16;
    strncpy(p.text, "Tap", ER_TEXT_MAX);
    er_node_set_props(label, &p);

    er_tree_append_child(pressable, label);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

    if (parent_counts.press_count != 1)
        return fail("press did not bubble from child text to parent pressable");
    if (parent_counts.press_in_count != 1 || parent_counts.press_out_count != 1)
        return fail("press in/out counts were wrong");
    if (parent_counts.touch_start_count != 1 || parent_counts.touch_end_count != 1)
        return fail("touch start/end counts were wrong");
    if (parent_counts.last_x != 10 || parent_counts.last_y != 10)
        return fail("event payload did not preserve touch coordinates");

    EventCounts bottom_counts = {0};
    EventCounts top_counts = {0};
    ERNode* bottom = er_node_create(ER_NODE_PRESSABLE);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 20;
    p.top = 20;
    p.width = 80;
    p.height = 80;
    p.background_color = 0xFF202020U;
    er_node_set_props(bottom, &p);
    er_event_set(bottom, ER_EVENT_PRESS, on_press, &bottom_counts);

    ERNode* top = er_node_create(ER_NODE_PRESSABLE);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 30;
    p.top = 30;
    p.width = 80;
    p.height = 80;
    p.background_color = 0xFF303030U;
    er_node_set_props(top, &p);
    er_event_set(top, ER_EVENT_PRESS, on_press, &top_counts);

    er_tree_append_child(root, bottom);
    er_tree_append_child(root, top);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 40, 40);
    embedded_renderer_touch(0, ER_TOUCH_UP, 40, 40);

    if (bottom_counts.press_count != 0)
        return fail("bottom overlapping sibling received press");
    if (top_counts.press_count != 1)
        return fail("top overlapping sibling did not receive press");

    return EXIT_SUCCESS;
}
