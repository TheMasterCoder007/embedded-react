#include "er_scene.h"
#include "native_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define EVENT_LOG_MAX 64

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Counts event callbacks received during a test.
 */
typedef struct
{
    int press_count;
    int long_press_count;
    int press_in_count;
    int press_out_count;
    int touch_start_count;
    int touch_move_count;
    int touch_end_count;
    int touch_cancel_count;
    int last_x;
    int last_y;
    char log[EVENT_LOG_MAX];
    int log_len;
} EventCounts;

/**
 * @brief Records the last color drawn by a test backend.
 */
typedef struct
{
    uint32_t last_fill_color;
} RenderCounts;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Appends one event marker to a test event log.
 *
 * @param[in,out] counts  Counter state to update.
 * @param[in] marker      Event marker byte.
 */
static void append_log(EventCounts* counts, char marker)
{
    if (counts->log_len < EVENT_LOG_MAX - 1)
    {
        counts->log[counts->log_len++] = marker;
        counts->log[counts->log_len] = '\0';
    }
}

/**
 * @brief Backend fill callback used to verify render stacking order.
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
 * @brief Backend copy callback unused by input tests.
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
 * @brief Backend blend callback unused by input tests.
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
    append_log(counts, 'P');
}

/**
 * @brief Records a long-press event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_long_press(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    counts->long_press_count++;
    counts->last_x = data->x;
    counts->last_y = data->y;
    append_log(counts, 'L');
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
    append_log(counts, 'I');
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
    append_log(counts, 'O');
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
    append_log(counts, 'S');
}

/**
 * @brief Records a touch-move event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_touch_move(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    counts->touch_move_count++;
    counts->last_x = data->x;
    counts->last_y = data->y;
    append_log(counts, 'M');
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
    append_log(counts, 'E');
}

/**
 * @brief Records a touch-cancel event.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_touch_cancel(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    (void)data;
    counts->touch_cancel_count++;
    append_log(counts, 'C');
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

/**
 * @brief Creates a pressable test node.
 *
 * @param[in] x       Absolute X coordinate.
 * @param[in] y       Absolute Y coordinate.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] counts  Event counter context.
 *
 * @return New pressable node.
 */
static ERNode* create_pressable(int16_t x, int16_t y, int16_t w, int16_t h, EventCounts* counts)
{
    ERNode* node = er_node_create(ER_NODE_PRESSABLE);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = x;
    p.top = y;
    p.width = w;
    p.height = h;
    p.background_color = 0xFF101010U;
    er_node_set_props(node, &p);
    er_event_set(node, ER_EVENT_PRESS, on_press, counts);
    er_event_set(node, ER_EVENT_LONG_PRESS, on_long_press, counts);
    er_event_set(node, ER_EVENT_PRESS_IN, on_press_in, counts);
    er_event_set(node, ER_EVENT_PRESS_OUT, on_press_out, counts);
    er_event_set(node, ER_EVENT_TOUCH_START, on_touch_start, counts);
    er_event_set(node, ER_EVENT_TOUCH_MOVE, on_touch_move, counts);
    er_event_set(node, ER_EVENT_TOUCH_END, on_touch_end, counts);
    er_event_set(node, ER_EVENT_TOUCH_CANCEL, on_touch_cancel, counts);
    return node;
}

/**
 * @brief Creates a pressable test node with a zIndex.
 *
 * @param[in] x       Absolute X coordinate.
 * @param[in] y       Absolute Y coordinate.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] z_index Sibling stacking order.
 * @param[in] color   Background color.
 * @param[in] counts  Event counter context.
 *
 * @return New pressable node.
 */
static ERNode* create_pressable_z(int16_t x, int16_t y, int16_t w, int16_t h, int16_t z_index, uint32_t color,
                                  EventCounts* counts)
{
    ERNode* node = create_pressable(x, y, w, h, counts);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = x;
    p.top = y;
    p.width = w;
    p.height = h;
    p.background_color = color;
    p.z_index = z_index;
    er_node_set_props(node, &p);
    return node;
}

/**
 * @brief Checks basic press ordering from touch down to touch up.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_press_order(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(0, 0, 120, 80, &counts);

    ERNode* label = er_node_create(ER_NODE_TEXT);
    ERProps p = props_default();
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

    if (counts.press_count != 1)
        return fail("press did not bubble from child text to parent pressable");
    if (strcmp(counts.log, "SIEOP") != 0)
        return fail("basic press event order was wrong");
    if (counts.last_x != 10 || counts.last_y != 10)
        return fail("event payload did not preserve touch coordinates");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks move-out, move-back-in, and final press dispatch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_move_out_and_back_in(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(20, 20, 80, 60, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 140, 120);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 35, 35);
    embedded_renderer_touch(0, ER_TOUCH_UP, 35, 35);

    if (counts.press_count != 1)
        return fail("press did not fire after moving out and back in");
    if (counts.press_in_count != 2 || counts.press_out_count != 2)
        return fail("press in/out counts for move out/back in were wrong");
    if (strcmp(counts.log, "SIMOMIEOP") != 0)
        return fail("move out/back in event order was wrong");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks cancellation dispatches touch cancel and press out without press.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_cancel(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(0, 0, 80, 80, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_CANCEL, 10, 10);

    if (counts.touch_cancel_count != 1)
        return fail("touch cancel did not fire");
    if (counts.press_out_count != 1)
        return fail("cancel did not emit press out");
    if (counts.press_count != 0 || counts.touch_end_count != 0)
        return fail("cancel emitted press or touch end");
    if (strcmp(counts.log, "SICO") != 0)
        return fail("cancel event order was wrong");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks long-press timing and single-fire behaviour.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_long_press(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(0, 0, 80, 80, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 12, 14);
    embedded_renderer_tick(499U);
    if (counts.long_press_count != 0)
        return fail("long press fired before threshold");

    embedded_renderer_tick(1U);
    embedded_renderer_tick(1000U);
    if (counts.long_press_count != 1)
        return fail("long press did not fire exactly once");
    if (counts.last_x != 12 || counts.last_y != 14)
        return fail("long press did not preserve latest coordinates");

    embedded_renderer_touch(0, ER_TOUCH_UP, 12, 14);
    if (counts.press_count != 1 || counts.press_out_count != 1)
        return fail("long press sequence did not finish normally");
    if (strcmp(counts.log, "SILEOP") != 0)
        return fail("long press event order was wrong");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks leaving the press target cancels long press even if the finger returns.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_long_press_cancelled_by_exit(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(0, 0, 80, 80, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_tick(250U);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 100, 100);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 10, 10);
    embedded_renderer_tick(500U);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

    if (counts.long_press_count != 0)
        return fail("long press fired after leaving press target");
    if (counts.press_count != 1)
        return fail("press did not complete after returning inside");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks raw touch events bubble through ancestors.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_raw_touch_bubbling(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};

    ERNode* parent = create_pressable(0, 0, 100, 80, &counts);
    ERNode* child = create_pressable(10, 10, 40, 30, &counts);
    er_tree_append_child(parent, child);
    er_tree_append_child(root, parent);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 15, 15);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 16, 16);
    embedded_renderer_touch(0, ER_TOUCH_UP, 16, 16);

    if (counts.touch_start_count != 2 || counts.touch_move_count != 2 || counts.touch_end_count != 2)
        return fail("raw touch events did not bubble through target ancestors");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks two active fingers can press separate targets independently.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_multi_touch(void)
{
    ERNode* root = create_root();
    EventCounts left_counts = {0};
    EventCounts right_counts = {0};
    ERNode* left = create_pressable(0, 0, 80, 80, &left_counts);
    ERNode* right = create_pressable(120, 0, 80, 80, &right_counts);
    er_tree_append_child(root, left);
    er_tree_append_child(root, right);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(1, ER_TOUCH_DOWN, 130, 10);
    embedded_renderer_touch(1, ER_TOUCH_UP, 130, 10);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

    if (left_counts.press_count != 1 || right_counts.press_count != 1)
        return fail("multi-touch presses did not complete independently");
    if (left_counts.press_in_count != 1 || right_counts.press_in_count != 1)
        return fail("multi-touch press-in counts were wrong");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks later overlapping siblings win hit-testing.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_overlapping_siblings(void)
{
    ERNode* root = create_root();
    EventCounts bottom_counts = {0};
    EventCounts top_counts = {0};
    ERNode* bottom = create_pressable(20, 20, 80, 80, &bottom_counts);
    ERNode* top = create_pressable(30, 30, 80, 80, &top_counts);

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

/**
 * @brief Checks higher zIndex wins hit-testing even when appended earlier.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_z_index_hit_order(void)
{
    ERNode* root = create_root();
    EventCounts high_counts = {0};
    EventCounts low_counts = {0};
    ERNode* high = create_pressable_z(20, 20, 80, 80, 10, 0xFF00FF00U, &high_counts);
    ERNode* low = create_pressable_z(30, 30, 80, 80, 0, 0xFFFF0000U, &low_counts);

    er_tree_append_child(root, high);
    er_tree_append_child(root, low);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 40, 40);
    embedded_renderer_touch(0, ER_TOUCH_UP, 40, 40);

    if (high_counts.press_count != 1)
        return fail("higher zIndex sibling did not receive press");
    if (low_counts.press_count != 0)
        return fail("lower zIndex sibling received press");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks higher zIndex renders after lower zIndex.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_z_index_render_order(void)
{
    RenderCounts render_counts = {0};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &render_counts};
    embedded_renderer_set_backend(&be);

    ERNode* root = create_root();
    EventCounts high_counts = {0};
    EventCounts low_counts = {0};
    ERNode* high = create_pressable_z(20, 20, 80, 80, 10, 0xFF00FF00U, &high_counts);
    ERNode* low = create_pressable_z(30, 30, 80, 80, 0, 0xFFFF0000U, &low_counts);

    er_tree_append_child(root, high);
    er_tree_append_child(root, low);
    er_commit();

    if (render_counts.last_fill_color != 0xFF00FF00U)
        return fail("higher zIndex sibling did not render last");

    embedded_renderer_set_backend(NULL);
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point for hit-testing and press dispatch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    embedded_renderer_set_backend(NULL);

    if (test_press_order() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_out_and_back_in() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_cancel() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_long_press() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_long_press_cancelled_by_exit() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_raw_touch_bubbling() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_multi_touch() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_overlapping_siblings() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_z_index_hit_order() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_z_index_render_order() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
