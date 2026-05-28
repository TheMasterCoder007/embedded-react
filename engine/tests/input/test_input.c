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
 * @brief Accumulates ER_EVENT_LAYOUT callbacks for a node.
 */
typedef struct
{
    int count;
    ERRect rect;
} LayoutRecord;

/**
 * @brief Tracks gesture responder events and controls query responses for a node.
 */
typedef struct
{
    bool should_claim;         /**< Value returned by the query callbacks. */
    int min_abs_dx;            /**< Minimum |dx| required before claiming (0 = always). */
    bool yield_on_termination; /**< Value returned by ER_QUERY_TERMINATION_REQUEST. */
    int grant_count;
    int reject_count;
    int move_count;
    int release_count;
    int terminate_count;
    int termination_request_count;
    int last_dx;
    int last_dy;
} ResponderRecord;

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
    p.opacity = 255U;
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
 * @brief Records an ER_EVENT_LAYOUT callback.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Layout event payload.
 * @param[in] user_data  Pointer to LayoutRecord.
 */
static void on_layout(ERNode* node, const EREventData* data, void* user_data)
{
    LayoutRecord* rec = user_data;
    (void)node;
    rec->count++;
    rec->rect = data->layout_rect;
}

/**
 * @brief Query callback: claims the responder when should_claim is true and |dx| >= min_abs_dx.
 *
 * @param[in] node       Node being queried.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to ResponderRecord.
 *
 * @return true when the node wishes to claim the responder.
 */
static bool query_should_claim(ERNode* node, const EREventData* data, void* user_data)
{
    ResponderRecord* rec = user_data;
    (void)node;
    if (rec->min_abs_dx > 0 && data->dx < rec->min_abs_dx && data->dx > -rec->min_abs_dx)
        return false;
    return rec->should_claim;
}

/**
 * @brief Query callback for ER_QUERY_TERMINATION_REQUEST.
 *
 * @param[in] node       Node being queried.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to ResponderRecord.
 *
 * @return yield_on_termination field of the record.
 */
static bool query_termination_request(ERNode* node, const EREventData* data, void* user_data)
{
    ResponderRecord* rec = user_data;
    (void)node;
    (void)data;
    rec->termination_request_count++;
    return rec->yield_on_termination;
}

/**
 * @brief Records an ER_EVENT_RESPONDER_GRANT callback.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Event payload.
 * @param[in] user_data  Pointer to ResponderRecord.
 */
static void on_responder_grant(ERNode* node, const EREventData* data, void* user_data)
{
    ResponderRecord* rec = user_data;
    (void)node;
    (void)data;
    rec->grant_count++;
}

/**
 * @brief Records an ER_EVENT_RESPONDER_REJECT callback.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Event payload.
 * @param[in] user_data  Pointer to ResponderRecord.
 */
static void on_responder_reject(ERNode* node, const EREventData* data, void* user_data)
{
    ResponderRecord* rec = user_data;
    (void)node;
    (void)data;
    rec->reject_count++;
}

/**
 * @brief Records an ER_EVENT_RESPONDER_MOVE callback.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Event payload.
 * @param[in] user_data  Pointer to ResponderRecord.
 */
static void on_responder_move(ERNode* node, const EREventData* data, void* user_data)
{
    ResponderRecord* rec = user_data;
    (void)node;
    rec->move_count++;
    rec->last_dx = data->dx;
    rec->last_dy = data->dy;
}

/**
 * @brief Records an ER_EVENT_RESPONDER_RELEASE callback.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Event payload.
 * @param[in] user_data  Pointer to ResponderRecord.
 */
static void on_responder_release(ERNode* node, const EREventData* data, void* user_data)
{
    ResponderRecord* rec = user_data;
    (void)node;
    rec->release_count++;
    rec->last_dx = data->dx;
    rec->last_dy = data->dy;
}

/**
 * @brief Records an ER_EVENT_RESPONDER_TERMINATE callback.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Event payload.
 * @param[in] user_data  Pointer to ResponderRecord.
 */
static void on_responder_terminate(ERNode* node, const EREventData* data, void* user_data)
{
    ResponderRecord* rec = user_data;
    (void)node;
    (void)data;
    rec->terminate_count++;
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
static ERNode*
create_pressable_z(int16_t x, int16_t y, int16_t w, int16_t h, int16_t z_index, uint32_t color, EventCounts* counts)
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

/**
 * @brief Registers all responder event handlers and query callbacks on a node.
 *
 * @param[in] node  Target node.
 * @param[in] rec   Pointer to the ResponderRecord that receives all callbacks.
 */
static void wire_responder(ERNode* node, ResponderRecord* rec)
{
    er_event_set(node, ER_EVENT_RESPONDER_GRANT, on_responder_grant, rec);
    er_event_set(node, ER_EVENT_RESPONDER_REJECT, on_responder_reject, rec);
    er_event_set(node, ER_EVENT_RESPONDER_MOVE, on_responder_move, rec);
    er_event_set(node, ER_EVENT_RESPONDER_RELEASE, on_responder_release, rec);
    er_event_set(node, ER_EVENT_RESPONDER_TERMINATE, on_responder_terminate, rec);
}

/**
 * @brief Checks that a node claiming ER_QUERY_START_SHOULD_SET becomes the responder on
 *        touch-down and receives grant, move, and release events.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_start_should_set(void)
{
    ERNode* root = create_root();
    ResponderRecord rec = {0};
    rec.should_claim = true;

    ERNode* view = er_node_create(ER_NODE_PRESSABLE);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 80;
    p.height = 80;
    er_node_set_props(view, &p);
    wire_responder(view, &rec);
    er_responder_query_set(view, ER_QUERY_START_SHOULD_SET, query_should_claim, &rec);

    er_tree_append_child(root, view);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);

    if (rec.grant_count != 1)
        return fail("responder not granted on start-should-set");

    embedded_renderer_touch(0, ER_TOUCH_MOVE, 30, 10);

    if (rec.move_count != 1)
        return fail("responder did not receive move event");
    if (rec.last_dx != 20 || rec.last_dy != 0)
        return fail("responder move event had wrong dx/dy");

    embedded_renderer_touch(0, ER_TOUCH_UP, 30, 10);

    if (rec.release_count != 1)
        return fail("responder did not receive release event");
    if (rec.last_dx != 20)
        return fail("responder release event had wrong dx");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that a node claiming ER_QUERY_MOVE_SHOULD_SET becomes the responder only
 *        after sufficient displacement, and then receives move and release events.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_move_should_set(void)
{
    ERNode* root = create_root();
    ResponderRecord rec = {0};
    rec.should_claim = true;
    rec.min_abs_dx = 20; /* claim only when |dx| >= 20 */

    ERNode* view = er_node_create(ER_NODE_PRESSABLE);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 80;
    p.height = 80;
    er_node_set_props(view, &p);
    wire_responder(view, &rec);
    er_responder_query_set(view, ER_QUERY_MOVE_SHOULD_SET, query_should_claim, &rec);

    er_tree_append_child(root, view);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);

    if (rec.grant_count != 0)
        return fail("responder granted at touch-down without start-should-set");

    /* Small move — below threshold, no grant */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 20, 10);

    if (rec.grant_count != 0)
        return fail("responder granted before reaching displacement threshold");

    /* Large move — crosses threshold, grant fires */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 35, 10);

    if (rec.grant_count != 1)
        return fail("responder not granted after reaching displacement threshold");

    /* Subsequent move fires responder-move */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 50, 10);

    if (rec.move_count != 1)
        return fail("responder did not receive move after being granted");
    if (rec.last_dx != 40)
        return fail("responder move had wrong dx");

    embedded_renderer_touch(0, ER_TOUCH_UP, 50, 10);

    if (rec.release_count != 1)
        return fail("responder did not receive release");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that a capture-phase claim (ER_QUERY_START_SHOULD_SET_CAPTURE on the parent)
 *        wins over a bubble-phase claim (ER_QUERY_START_SHOULD_SET on the child).
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_capture_wins_over_bubble(void)
{
    ERNode* root = create_root();
    ResponderRecord parent_rec = {0};
    ResponderRecord child_rec = {0};
    parent_rec.should_claim = true;
    child_rec.should_claim = true;

    /* Parent at [0,0,80,80] claims via capture */
    ERNode* parent = er_node_create(ER_NODE_VIEW);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 0;
        p.top = 0;
        p.width = 80;
        p.height = 80;
        er_node_set_props(parent, &p);
        wire_responder(parent, &parent_rec);
        er_responder_query_set(parent, ER_QUERY_START_SHOULD_SET_CAPTURE, query_should_claim, &parent_rec);
    }

    /* Child at [10,10,40,40] claims via bubble */
    ERNode* child = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 10;
        p.top = 10;
        p.width = 40;
        p.height = 40;
        er_node_set_props(child, &p);
        wire_responder(child, &child_rec);
        er_responder_query_set(child, ER_QUERY_START_SHOULD_SET, query_should_claim, &child_rec);
    }

    er_tree_append_child(parent, child);
    er_tree_append_child(root, parent);
    er_commit();

    /* Touch inside child — parent capture must win */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);

    if (parent_rec.grant_count != 1)
        return fail("parent capture did not win responder negotiation");
    if (child_rec.grant_count != 0)
        return fail("child bubble incorrectly claimed responder over parent capture");

    embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

    if (parent_rec.release_count != 1)
        return fail("parent responder did not receive release");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that when a new claimant wins move-should-set and the current responder
 *        yields (ER_QUERY_TERMINATION_REQUEST returns true), the responder transfers.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_termination_accepted(void)
{
    ERNode* root = create_root();
    ResponderRecord child_rec = {0};
    ResponderRecord parent_rec = {0};
    child_rec.should_claim = true; /* child claims on start */
    child_rec.yield_on_termination = true;
    parent_rec.should_claim = true; /* parent claims on move (capture) */

    /* Parent container — claims the responder via move capture */
    ERNode* parent = er_node_create(ER_NODE_VIEW);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 0;
        p.top = 0;
        p.width = 80;
        p.height = 80;
        er_node_set_props(parent, &p);
        wire_responder(parent, &parent_rec);
        er_responder_query_set(parent, ER_QUERY_MOVE_SHOULD_SET_CAPTURE, query_should_claim, &parent_rec);
    }

    /* Child node — claims the responder on touch-down */
    ERNode* child = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 10;
        p.top = 10;
        p.width = 40;
        p.height = 40;
        er_node_set_props(child, &p);
        wire_responder(child, &child_rec);
        er_responder_query_set(child, ER_QUERY_START_SHOULD_SET, query_should_claim, &child_rec);
        er_responder_query_set(child, ER_QUERY_TERMINATION_REQUEST, query_termination_request, &child_rec);
    }

    er_tree_append_child(parent, child);
    er_tree_append_child(root, parent);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);

    if (child_rec.grant_count != 1)
        return fail("child did not become responder on touch-down");

    /* Move: parent capture fires → child asked to yield → child yields → transfer */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 30, 20);

    if (child_rec.termination_request_count != 1)
        return fail("termination request was not sent to the current responder");
    if (child_rec.terminate_count != 1)
        return fail("current responder did not receive terminate after yielding");
    if (parent_rec.grant_count != 1)
        return fail("new claimant did not receive grant after termination was accepted");

    embedded_renderer_touch(0, ER_TOUCH_UP, 30, 20);

    if (parent_rec.release_count != 1)
        return fail("new responder did not receive release");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that when a new claimant wins move-should-set but the current responder
 *        refuses to yield (ER_QUERY_TERMINATION_REQUEST returns false), the claimant is
 *        rejected and the original responder keeps receiving events.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_termination_rejected(void)
{
    ERNode* root = create_root();
    ResponderRecord child_rec = {0};
    ResponderRecord parent_rec = {0};
    child_rec.should_claim = true;
    child_rec.yield_on_termination = false; /* child refuses to yield */
    parent_rec.should_claim = true;

    ERNode* parent = er_node_create(ER_NODE_VIEW);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 0;
        p.top = 0;
        p.width = 80;
        p.height = 80;
        er_node_set_props(parent, &p);
        wire_responder(parent, &parent_rec);
        er_responder_query_set(parent, ER_QUERY_MOVE_SHOULD_SET_CAPTURE, query_should_claim, &parent_rec);
    }

    ERNode* child = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 10;
        p.top = 10;
        p.width = 40;
        p.height = 40;
        er_node_set_props(child, &p);
        wire_responder(child, &child_rec);
        er_responder_query_set(child, ER_QUERY_START_SHOULD_SET, query_should_claim, &child_rec);
        er_responder_query_set(child, ER_QUERY_TERMINATION_REQUEST, query_termination_request, &child_rec);
    }

    er_tree_append_child(parent, child);
    er_tree_append_child(root, parent);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);

    if (child_rec.grant_count != 1)
        return fail("child did not become responder on touch-down");

    /* Move: parent capture fires → child refuses to yield → parent gets reject */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 30, 20);

    if (child_rec.termination_request_count != 1)
        return fail("termination request was not sent to the current responder");
    if (child_rec.terminate_count != 0)
        return fail("current responder received terminate despite refusing to yield");
    if (parent_rec.reject_count != 1)
        return fail("rejected claimant did not receive reject event");
    if (parent_rec.grant_count != 0)
        return fail("rejected claimant incorrectly received grant");

    /* Child must still receive the next move */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 40, 20);

    if (child_rec.move_count < 1)
        return fail("original responder stopped receiving moves after rejection");

    embedded_renderer_touch(0, ER_TOUCH_UP, 40, 20);

    if (child_rec.release_count != 1)
        return fail("original responder did not receive release");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that pointer_events:none prevents the node and all children from receiving touches.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_pointer_events_none(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = er_node_create(ER_NODE_PRESSABLE);

    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 80;
    p.height = 80;
    p.background_color = 0xFF101010U;
    p.pointer_events = ER_POINTER_EVENTS_NONE;
    er_node_set_props(pressable, &p);
    er_event_set(pressable, ER_EVENT_PRESS, on_press, &counts);
    er_event_set(pressable, ER_EVENT_TOUCH_START, on_touch_start, &counts);

    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

    if (counts.press_count != 0 || counts.touch_start_count != 0)
        return fail("pointer_events:none node received touch events");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that pointer_events:box-none passes touches to children but not to the node itself.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_pointer_events_box_none(void)
{
    ERNode* root = create_root();
    EventCounts parent_counts = {0};
    EventCounts child_counts = {0};

    /* Parent view with BOX_NONE — transparent to touches, but passes them through */
    ERNode* parent = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 0;
        p.top = 0;
        p.width = 80;
        p.height = 80;
        p.pointer_events = ER_POINTER_EVENTS_BOX_NONE;
        er_node_set_props(parent, &p);
        er_event_set(parent, ER_EVENT_PRESS, on_press, &parent_counts);
        er_event_set(parent, ER_EVENT_TOUCH_START, on_touch_start, &parent_counts);
    }

    /* Child pressable inside parent */
    ERNode* child = create_pressable(10, 10, 40, 40, &child_counts);
    er_tree_append_child(parent, child);
    er_tree_append_child(root, parent);
    er_commit();

    /* Touch inside child — child must receive press, parent must not */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 15, 15);
    embedded_renderer_touch(0, ER_TOUCH_UP, 15, 15);

    if (child_counts.press_count != 1)
        return fail("pointer_events:box-none child did not receive press");
    if (parent_counts.press_count != 0)
        return fail("pointer_events:box-none parent received press when child was hit");

    /* Touch inside parent but outside child — nobody receives press */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 60, 60);
    embedded_renderer_touch(0, ER_TOUCH_UP, 60, 60);

    if (parent_counts.press_count != 0)
        return fail("pointer_events:box-none parent received press on direct hit");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that pointer_events:box-only delivers touches to the node but not its children.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_pointer_events_box_only(void)
{
    ERNode* root = create_root();
    EventCounts parent_counts = {0};
    EventCounts child_counts = {0};

    /* Parent pressable with BOX_ONLY — absorbs all touches, children get nothing */
    ERNode* parent = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 0;
        p.top = 0;
        p.width = 80;
        p.height = 80;
        p.pointer_events = ER_POINTER_EVENTS_BOX_ONLY;
        er_node_set_props(parent, &p);
        er_event_set(parent, ER_EVENT_PRESS, on_press, &parent_counts);
        er_event_set(parent, ER_EVENT_TOUCH_START, on_touch_start, &parent_counts);
    }

    ERNode* child = create_pressable(10, 10, 40, 40, &child_counts);
    er_tree_append_child(parent, child);
    er_tree_append_child(root, parent);
    er_commit();

    /* Touch inside child — parent must receive press, child must not */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 15, 15);
    embedded_renderer_touch(0, ER_TOUCH_UP, 15, 15);

    if (parent_counts.press_count != 1)
        return fail("pointer_events:box-only parent did not receive press");
    if (child_counts.press_count != 0)
        return fail("pointer_events:box-only child received press");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that hitSlop extends the hit area beyond the node's strict bounds.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_hit_slop(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};

    /* Pressable at [50,50,80,80] with 20px left slop — hittable from x=30 */
    ERNode* pressable = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 50;
        p.top = 50;
        p.width = 80;
        p.height = 80;
        p.hit_slop_left = 20;
        er_node_set_props(pressable, &p);
        er_event_set(pressable, ER_EVENT_PRESS, on_press, &counts);
        er_event_set(pressable, ER_EVENT_PRESS_IN, on_press_in, &counts);
        er_event_set(pressable, ER_EVENT_PRESS_OUT, on_press_out, &counts);
        er_event_set(pressable, ER_EVENT_TOUCH_START, on_touch_start, &counts);
    }
    er_tree_append_child(root, pressable);
    er_commit();

    /* Touch in slop zone (x=35, inside [30,50)) — press must fire */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 35, 80);
    embedded_renderer_touch(0, ER_TOUCH_UP, 35, 80);

    if (counts.press_count != 1)
        return fail("hitSlop touch in slop zone did not fire press");

    /* Touch outside slop zone (x=25, outside [30,...)) — no press */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 25, 80);
    embedded_renderer_touch(0, ER_TOUCH_UP, 25, 80);

    if (counts.press_count != 1)
        return fail("hitSlop touch outside slop zone fired press");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that overflow:hidden prevents children from being hit outside the strict parent bounds,
 *        even when the parent has hitSlop that would otherwise allow entry.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_overflow_hidden_clips_hit(void)
{
    ERNode* root = create_root();
    EventCounts parent_counts = {0};
    EventCounts child_counts = {0};

    /*
     * Parent PRESSABLE at [20,20,80,80] with overflow:hidden and 30px right slop.
     * Strict right edge at x=100; slop extends hittable zone to x=130.
     */
    ERNode* parent = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 20;
        p.top = 20;
        p.width = 80;
        p.height = 80;
        p.overflow = ER_OVERFLOW_HIDDEN;
        p.hit_slop_right = 30;
        er_node_set_props(parent, &p);
        er_event_set(parent, ER_EVENT_PRESS, on_press, &parent_counts);
        er_event_set(parent, ER_EVENT_TOUCH_START, on_touch_start, &parent_counts);
    }

    /*
     * Child PRESSABLE at left=70,top=0 relative to parent → absolute [90,20,60,60].
     * Its right edge reaches x=150, well outside the parent's strict right edge (x=100).
     */
    ERNode* child = er_node_create(ER_NODE_PRESSABLE);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 70;
        p.top = 0;
        p.width = 60;
        p.height = 60;
        er_node_set_props(child, &p);
        er_event_set(child, ER_EVENT_PRESS, on_press, &child_counts);
        er_event_set(child, ER_EVENT_TOUCH_START, on_touch_start, &child_counts);
    }

    er_tree_append_child(parent, child);
    er_tree_append_child(root, parent);
    er_commit();

    /*
     * Touch at (110, 30): inside child [90,20,60,60] and inside parent slop zone
     * (100 < 110 < 130) but outside parent strict bounds.
     * overflow:hidden must prevent the child from being hit.
     * The parent itself is hittable in its slop zone — press must fire on parent.
     */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 110, 30);
    embedded_renderer_touch(0, ER_TOUCH_UP, 110, 30);

    if (child_counts.touch_start_count != 0 || child_counts.press_count != 0)
        return fail("overflow:hidden did not clip child hit in parent slop zone");
    if (parent_counts.press_count != 1)
        return fail("overflow:hidden parent was not hittable in its own slop zone");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that ER_EVENT_LAYOUT fires when a node's computed rect changes.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_layout_event_dispatch(void)
{
    ERNode* root = create_root();
    LayoutRecord rec = {0};

    ERNode* view = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 10;
    p.top = 20;
    p.width = 60;
    p.height = 40;
    er_node_set_props(view, &p);
    er_event_set(view, ER_EVENT_LAYOUT, on_layout, &rec);
    er_tree_append_child(root, view);

    er_commit();

    if (rec.count != 1)
        return fail("layout event did not fire on first commit");
    if (rec.rect.x != 10 || rec.rect.y != 20 || rec.rect.w != 60 || rec.rect.h != 40)
        return fail("layout event payload was wrong");

    er_commit();

    if (rec.count != 1)
        return fail("layout event fired again with no layout change");

    /* Move the node — layout event must fire again with the new rect */
    ERProps p2 = props_default();
    p2.position = ER_POS_ABSOLUTE;
    p2.left = 30;
    p2.top = 20;
    p2.width = 60;
    p2.height = 40;
    er_node_set_props(view, &p2);
    er_commit();

    if (rec.count != 2)
        return fail("layout event did not fire after rect change");
    if (rec.rect.x != 30)
        return fail("layout event payload did not reflect moved position");

    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Checks that a display:none node and its children never receive touch events.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_display_none_not_hittable(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(0, 0, 80, 80, &counts);

    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 80;
    p.height = 80;
    p.background_color = 0xFF101010U;
    p.display = ER_DISPLAY_NONE;
    er_node_set_props(pressable, &p);

    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

    if (counts.press_count != 0 || counts.touch_start_count != 0)
        return fail("display:none node received touch events");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that an opacity:0 view node does not receive touch events.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_opacity_zero_not_hittable(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(0, 0, 80, 80, &counts);

    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 80;
    p.height = 80;
    p.background_color = 0xFF101010U;
    p.opacity = 0;
    er_node_set_props(pressable, &p);

    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

    if (counts.press_count != 0 || counts.touch_start_count != 0)
        return fail("opacity:0 node received touch events");

    return EXIT_SUCCESS;
}

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
    if (test_display_none_not_hittable() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_opacity_zero_not_hittable() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_none() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_box_none() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_box_only() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_hit_slop() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_overflow_hidden_clips_hit() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_layout_event_dispatch() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_start_should_set() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_move_should_set() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_capture_wins_over_bubble() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_termination_accepted() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_termination_rejected() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
