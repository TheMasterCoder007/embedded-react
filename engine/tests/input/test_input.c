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

#include "er_scene.h"
#include "native_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define EVENT_LOG_MAX 64

/* A node this size cannot fit the transform source buffer, so er_transform_source_begin() refuses it
 * and render_tree paints it UNTRANSFORMED at its raw layout box. Derived from the build's own limit
 * rather than hard-coded, so the fallback is reachable in every configuration — including the default
 * one, where ERUI_XFORM_W/H tracks the composite scratch. */
#define XF_TOO_BIG_W (ERUI_XFORM_W + 10)
#define XF_TOO_BIG_H (ERUI_XFORM_H + 10)

/* Points on the ring of the dial mount_dial() builds: centre 70,70, mid-band radius 50 - 16/2 = 42. Both are
 * inside the default sweep, and set different values. */
#define DIAL_TOP_X 70
#define DIAL_TOP_Y (70 - 42)
#define DIAL_RIGHT_X (70 + 42)
#define DIAL_RIGHT_Y 70

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
    int focus_count;
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
    int query_count;           /**< Should-set queries asked through query_should_claim. */
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
 * @brief Captures the gesture fields (travel + speed) carried by raw touch events.
 */
typedef struct
{
    int move_count;
    int end_count;
    int cancel_count;
    int dx; /**< From the most recent event of any phase below. */
    int dy;
    float vx;
    float vy;
} TouchGestureRecord;

/**
 * @brief Records the last color drawn by a test backend.
 */
typedef struct
{
    uint32_t last_fill_color;
} RenderCounts;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Scroll counter that deliberately outlives its scene, so a scroller leaked out of one
 *        scenario can be caught still firing into the next.
 */
static int s_orphan_scroll_events = 0;

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
 * @brief Records the gesture payload of a raw touch-move.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to TouchGestureRecord.
 */
static void on_gesture_move(ERNode* node, const EREventData* data, void* user_data)
{
    TouchGestureRecord* rec = user_data;
    (void)node;
    rec->move_count++;
    rec->dx = data->dx;
    rec->dy = data->dy;
    rec->vx = data->vx;
    rec->vy = data->vy;
}

/**
 * @brief Records the gesture payload of a raw touch-end.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to TouchGestureRecord.
 */
static void on_gesture_end(ERNode* node, const EREventData* data, void* user_data)
{
    TouchGestureRecord* rec = user_data;
    (void)node;
    rec->end_count++;
    rec->dx = data->dx;
    rec->dy = data->dy;
    rec->vx = data->vx;
    rec->vy = data->vy;
}

/**
 * @brief Records the gesture payload of a raw touch-cancel.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Touch event payload.
 * @param[in] user_data  Pointer to TouchGestureRecord.
 */
static void on_gesture_cancel(ERNode* node, const EREventData* data, void* user_data)
{
    TouchGestureRecord* rec = user_data;
    (void)node;
    rec->cancel_count++;
    rec->dx = data->dx;
    rec->dy = data->dy;
    rec->vx = data->vx;
    rec->vy = data->vy;
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
 * @brief Records an ER_EVENT_FOCUS callback.
 *
 * @param[in] node       Node that received the event.
 * @param[in] data       Event payload.
 * @param[in] user_data  Pointer to EventCounts.
 */
static void on_focus(ERNode* node, const EREventData* data, void* user_data)
{
    EventCounts* counts = user_data;
    (void)node;
    (void)data;
    counts->focus_count++;
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
    rec->query_count++;
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
 * @brief Compares two velocities for equality within a pixel-per-second of slack.
 *
 * @param[in] a  Measured value.
 * @param[in] b  Expected value.
 *
 * @return true when they agree.
 */
static bool near_vel(float a, float b)
{
    const float d = a - b;
    return (d < 0.0f ? -d : d) < 0.001f;
}

/**
 * @brief Sends one touch-move and dispatches it, standing in for a host frame.
 *
 * embedded_renderer_touch() coalesces moves to the newest one per finger and dispatches that one at
 * the frame boundary, so a test that wants each move observed has to mark the boundary the way a host
 * loop does — er_commit() and the JS pump both flush, and embedded_renderer_flush_touch() is that same
 * flush on its own.
 *
 * @param[in] x  X coordinate of the move.
 * @param[in] y  Y coordinate of the move.
 */
static void touch_move(int x, int y)
{
    embedded_renderer_touch(0, ER_TOUCH_MOVE, x, y);
    embedded_renderer_flush_touch();
}

/**
 * @brief Presses and releases one finger at a single point.
 *
 * @param[in] x  X coordinate of the tap.
 * @param[in] y  Y coordinate of the tap.
 */
static void tap(int x, int y)
{
    embedded_renderer_touch(0, ER_TOUCH_DOWN, x, y);
    embedded_renderer_touch(0, ER_TOUCH_UP, x, y);
}

/**
 * @brief Starts a fresh scene: empties the node pool, then creates and installs a sized root.
 *
 * er_tree_set_root() only swaps the root tag — it never destroys the tree it replaces. Without the
 * reset every scenario's nodes would stay in the pool for the rest of the run, and the pool is what
 * the per-frame sweeps walk: an orphan left animating or coasting keeps firing its callbacks into a
 * stack frame that has already returned. Every scenario therefore starts here, before it creates any
 * node of its own.
 *
 * @param[in] w  Root width in pixels.
 * @param[in] h  Root height in pixels.
 *
 * @return New root node.
 */
static ERNode* create_root_sized(int16_t w, int16_t h)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.width = w;
    p.height = h;
    p.background_color = 0xFF000000U;
    er_node_set_props(root, &p);
    er_tree_set_root(root);
    return root;
}

/**
 * @brief Creates a fixed-size root node.
 *
 * @return New root node.
 */
static ERNode* create_root(void)
{
    return create_root_sized(240, 160);
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
    touch_move(140, 120);
    touch_move(35, 35);
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
 * @brief Checks a frame's worth of moves collapses to one dispatch at the newest position.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_move_coalescing_collapses_to_latest(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(20, 20, 80, 60, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 40, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 50, 34);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 60, 40);

    if (counts.touch_move_count != 0)
        return fail("moves must not dispatch until the frame boundary");

    embedded_renderer_flush_touch();

    if (counts.touch_move_count != 1)
        return fail("a frame's moves must collapse to a single dispatch");
    if (counts.last_x != 60 || counts.last_y != 40)
        return fail("the surviving move must carry the newest position");

    embedded_renderer_touch(0, ER_TOUCH_UP, 60, 40);
    if (counts.press_count != 1)
        return fail("release after a coalesced drag must still press");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks a parked move is dispatched ahead of the down/up that overtakes it.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_move_coalescing_flushes_before_up(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(20, 20, 80, 60, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 40, 40);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 55, 50);
    embedded_renderer_touch(0, ER_TOUCH_UP, 55, 50);

    if (counts.touch_move_count != 1)
        return fail("the release must flush exactly one parked move");
    if (strcmp(counts.log, "SIMEOP") != 0)
        return fail("a parked move must dispatch before the release that overtook it");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks a move that repeats the last dispatched position is dropped entirely.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_move_coalescing_drops_repeat_position(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(20, 20, 80, 60, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    /* A finger resting on a panel that keeps reporting: same point, frame after frame. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    for (int frame = 0; frame < 3; frame++)
    {
        embedded_renderer_touch(0, ER_TOUCH_MOVE, 30, 30);
        embedded_renderer_flush_touch();
    }
    if (counts.touch_move_count != 0)
        return fail("a move onto the last dispatched position must be dropped");

    embedded_renderer_touch(0, ER_TOUCH_MOVE, 31, 30);
    embedded_renderer_flush_touch();
    if (counts.touch_move_count != 1)
        return fail("a move to a new position must still dispatch");

    embedded_renderer_touch(0, ER_TOUCH_UP, 31, 30);
    return EXIT_SUCCESS;
}

/**
 * @brief Checks embedded_renderer_has_pending_touch() agrees with what the flush will actually do.
 *
 * The point of the query is "will flushing run a handler?", so a parked move the flush would DROP —
 * a finger held still on a panel that keeps reporting — has to answer false. Answering "something is
 * parked" instead would have a host prepare for JS on every frame of a resting finger.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_has_pending_touch_matches_dispatch(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(20, 20, 80, 60, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    if (embedded_renderer_has_pending_touch())
        return fail("nothing parked must answer false");

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 40, 35);
    if (!embedded_renderer_has_pending_touch())
        return fail("a move to a new position must answer true");

    embedded_renderer_flush_touch();
    if (embedded_renderer_has_pending_touch())
        return fail("a flushed move must answer false");
    if (counts.touch_move_count != 1)
        return fail("the move should have dispatched");

    /* The case the query used to get wrong: parked, but the flush would drop it. */
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 40, 35);
    if (embedded_renderer_has_pending_touch())
        return fail("a move repeating the last dispatched position must answer false");
    embedded_renderer_flush_touch();
    if (counts.touch_move_count != 1)
        return fail("...and must not dispatch either");

    embedded_renderer_touch(0, ER_TOUCH_UP, 40, 35);
    return EXIT_SUCCESS;
}

/**
 * @brief Checks each finger coalesces independently and both flush together.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_move_coalescing_is_per_finger(void)
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
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 20, 20);
    embedded_renderer_touch(1, ER_TOUCH_MOVE, 140, 20);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 30, 30);
    embedded_renderer_touch(1, ER_TOUCH_MOVE, 150, 30);
    embedded_renderer_flush_touch();

    if (left_counts.touch_move_count != 1 || right_counts.touch_move_count != 1)
        return fail("each finger must flush its own single coalesced move");
    if (left_counts.last_x != 30 || right_counts.last_x != 150)
        return fail("a finger's coalesced move must carry that finger's newest position");

    embedded_renderer_touch(0, ER_TOUCH_UP, 30, 30);
    embedded_renderer_touch(1, ER_TOUCH_UP, 150, 30);
    return EXIT_SUCCESS;
}

/**
 * @brief Checks er_commit() is itself a frame boundary, so a plain host loop needs no flush call.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_move_coalescing_flushed_by_commit(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(20, 20, 80, 60, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 45, 45);
    if (counts.touch_move_count != 0)
        return fail("a move must stay parked until the frame boundary");

    er_commit();
    if (counts.touch_move_count != 1)
        return fail("er_commit must dispatch parked moves before it lays out");
    if (counts.last_x != 45 || counts.last_y != 45)
        return fail("the move dispatched by er_commit carried the wrong position");

    embedded_renderer_touch(0, ER_TOUCH_UP, 45, 45);
    return EXIT_SUCCESS;
}

/**
 * @brief Checks coalescing can be turned off, restoring dispatch-per-sample.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_move_coalescing_can_be_disabled(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(20, 20, 80, 60, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_set_touch_coalescing(false);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 40, 30);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 50, 34);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 60, 40);
    const int moves = counts.touch_move_count;
    embedded_renderer_touch(0, ER_TOUCH_UP, 60, 40);

    embedded_renderer_set_touch_coalescing(true); /* restore the default for the tests that follow */

    if (moves != 3)
        return fail("with coalescing off every move must dispatch as it arrives");

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
    if (counts.press_out_count != 1)
        return fail("long press sequence did not finish normally");
    if (counts.press_count != 0)
        return fail("a delivered long press must replace onPress, not run alongside it");
    if (strcmp(counts.log, "SILEO") != 0)
        return fail("long press event order was wrong");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks delayLongPress replaces the default hold time for that node alone.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_long_press_delay(void)
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
    p.long_press_ms = 150U;
    er_node_set_props(pressable, &p);
    er_tree_append_child(root, pressable);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 12, 14);
    embedded_renderer_tick(149U);
    if (counts.long_press_count != 0)
        return fail("a shortened long press fired before its own threshold");

    embedded_renderer_tick(1U);
    if (counts.long_press_count != 1)
        return fail("delayLongPress did not shorten the hold time");
    embedded_renderer_touch(0, ER_TOUCH_UP, 12, 14);

    /* A longer threshold must equally hold the event back past the 500 ms default. */
    EventCounts slow = {0};
    ERNode* patient = create_pressable(0, 100, 80, 80, &slow);
    ERProps q = props_default();
    q.position = ER_POS_ABSOLUTE;
    q.left = 0;
    q.top = 100;
    q.width = 80;
    q.height = 80;
    q.long_press_ms = 1200U;
    er_node_set_props(patient, &q);
    er_tree_append_child(root, patient);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 12, 114);
    embedded_renderer_tick(1000U);
    if (slow.long_press_count != 0)
        return fail("a lengthened long press fired at the default threshold");
    embedded_renderer_tick(200U);
    if (slow.long_press_count != 1)
        return fail("a lengthened long press never fired");
    embedded_renderer_touch(0, ER_TOUCH_UP, 12, 114);

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
    touch_move(100, 100);
    touch_move(10, 10);
    embedded_renderer_tick(500U);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

    if (counts.long_press_count != 0)
        return fail("long press fired after leaving press target");
    if (counts.press_count != 1)
        return fail("press did not complete after returning inside");

    return EXIT_SUCCESS;
}

/**
 * @brief A held press (past the long-press threshold) must still fire onPress on release when the node
 *        has NO onLongPress handler — the common case (e.g. a plain button).
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_long_press_without_handler(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* node = er_node_create(ER_NODE_PRESSABLE);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 80;
    p.height = 80;
    er_node_set_props(node, &p);
    er_event_set(node, ER_EVENT_PRESS, on_press, &counts);
    er_event_set(node, ER_EVENT_PRESS_IN, on_press_in, &counts);
    er_event_set(node, ER_EVENT_PRESS_OUT, on_press_out, &counts);
    /* Deliberately NO ER_EVENT_LONG_PRESS handler — matches a plain <Pressable onPress>. */
    er_tree_append_child(root, node);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 12, 14);
    embedded_renderer_tick(600U); /* exceed the 500 ms long-press threshold while held inside */
    embedded_renderer_touch(0, ER_TOUCH_UP, 12, 14);

    if (counts.press_count != 1)
        return fail("onPress must fire on release after a long hold when there is no onLongPress handler");
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
    touch_move(16, 16);
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

    touch_move(30, 10);

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
    touch_move(20, 10);

    if (rec.grant_count != 0)
        return fail("responder granted before reaching displacement threshold");

    /* Large move — crosses threshold, grant fires */
    touch_move(35, 10);

    if (rec.grant_count != 1)
        return fail("responder not granted after reaching displacement threshold");

    /* Subsequent move fires responder-move */
    touch_move(50, 10);

    if (rec.move_count != 1)
        return fail("responder did not receive move after being granted");
    if (rec.last_dx != 40)
        return fail("responder move had wrong dx");

    embedded_renderer_touch(0, ER_TOUCH_UP, 50, 10);

    if (rec.release_count != 1)
        return fail("responder did not receive release");

    return EXIT_SUCCESS;
}

/** @brief ER_EVENT_SCROLL callback: counts scroll events (auto-scroll observation). */
static void count_scroll(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (*(int*)user_data)++;
}

/**
 * @brief Checks that a ScrollView which YIELDS the responder to a move-should-set claimant stops
 *        dead: its momentum velocity is zeroed at the handover, so it does not keep scrolling
 *        (coasting) under the new responder's gesture.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_takeover_stops_scroll(void)
{
    ERNode* root = create_root();
    ResponderRecord rec = {0};
    rec.should_claim = true;
    rec.min_abs_dx = 30; /* claim only once the drag is well past the scroller's slop */
    int scroll_events = 0;

    ERNode* sv = er_node_create(ER_NODE_SCROLL_VIEW);
    ERProps sp = props_default();
    sp.position = ER_POS_ABSOLUTE;
    sp.left = 0;
    sp.top = 0;
    sp.width = 100;
    sp.height = 100;
    er_node_set_props(sv, &sp);
    er_event_set(sv, ER_EVENT_SCROLL, count_scroll, &scroll_events);

    ERNode* content = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.width = 400; /* wider than the viewport so horizontal scrolling has range */
    cp.height = 100;
    er_node_set_props(content, &cp);
    wire_responder(content, &rec);
    er_responder_query_set(content, ER_QUERY_MOVE_SHOULD_SET, query_should_claim, &rec);

    er_tree_append_child(root, sv);
    er_tree_append_child(sv, content);
    er_commit();

    /* Drag with ticks between moves so the scroller accrues real momentum velocity. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 80, 50);
    embedded_renderer_tick(16U);
    touch_move(70, 50); /* |dx| 10: past the scroll slop — auto-scroll claims and scrolls */
    embedded_renderer_tick(16U);
    touch_move(60, 50); /* |dx| 20: still the scroller's; velocity is now non-zero */

    if (scroll_events == 0)
        return fail("auto-scroll never scrolled before the takeover");

    embedded_renderer_tick(16U);
    touch_move(45, 50); /* |dx| 35: the claimant takes the gesture; the scroller must yield AND stop */

    if (rec.grant_count != 1)
        return fail("move-should-set claimant was not granted at takeover");

    const int at_takeover = scroll_events;
    for (int i = 0; i < 30; i++)
        embedded_renderer_tick(16U);

    if (scroll_events != at_takeover)
        return fail("yielded ScrollView kept coasting after the takeover");

    embedded_renderer_touch(0, ER_TOUCH_UP, 45, 50);
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
    touch_move(30, 20);

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
    touch_move(30, 20);

    if (child_rec.termination_request_count != 1)
        return fail("termination request was not sent to the current responder");
    if (child_rec.terminate_count != 0)
        return fail("current responder received terminate despite refusing to yield");
    if (parent_rec.reject_count != 1)
        return fail("rejected claimant did not receive reject event");
    if (parent_rec.grant_count != 0)
        return fail("rejected claimant incorrectly received grant");

    /* Child must still receive the next move */
    touch_move(40, 20);

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
 * @brief Builds a box-none overlay holding one inert (handler-less) child.
 *
 * @param[in]  parent     Node to append the overlay to.
 * @param[out] out_child  Receives the inert child (may be NULL).
 * @return The overlay node.
 */
static ERNode* create_box_none_overlay(ERNode* parent, ERNode** out_child)
{
    ERNode* overlay = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 0;
    p.top = 0;
    p.width = 120;
    p.height = 120;
    p.pointer_events = ER_POINTER_EVENTS_BOX_NONE;
    er_node_set_props(overlay, &p);

    ERNode* child = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = 40;
    cp.top = 40;
    cp.width = 40;
    cp.height = 40;
    er_node_set_props(child, &cp);

    er_tree_append_child(overlay, child);
    er_tree_append_child(parent, overlay);
    if (out_child)
        *out_child = child;
    return overlay;
}

/**
 * @brief Checks that a box-none node stays touch-transparent when the hit lands on an inert
 *        descendant, so the walk up from that descendant does not hand it the touch anyway.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_pointer_events_box_none_inert_child(void)
{
    ERNode* root = create_root();
    EventCounts overlay_counts = {0};

    ERNode* overlay = create_box_none_overlay(root, NULL);
    er_event_set(overlay, ER_EVENT_PRESS, on_press, &overlay_counts);
    er_event_set(overlay, ER_EVENT_TOUCH_START, on_touch_start, &overlay_counts);
    er_commit();

    /* The inert child is the hit; the overlay must receive neither the press nor the raw touch. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 50, 50);
    embedded_renderer_touch(0, ER_TOUCH_UP, 50, 50);

    if (overlay_counts.press_count != 0)
        return fail("pointer_events:box-none node received press via an inert child");
    if (overlay_counts.touch_start_count != 0)
        return fail("pointer_events:box-none node received a raw touch via an inert child");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that a box-none node cannot claim the gesture responder.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_pointer_events_box_none_responder(void)
{
    ERNode* root = create_root();
    ResponderRecord rec = {0};
    rec.should_claim = true;

    ERNode* overlay = create_box_none_overlay(root, NULL);
    wire_responder(overlay, &rec);
    er_responder_query_set(overlay, ER_QUERY_START_SHOULD_SET, query_should_claim, &rec);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 50, 50);
    embedded_renderer_touch(0, ER_TOUCH_UP, 50, 50);

    if (rec.grant_count != 0)
        return fail("pointer_events:box-none node claimed the responder");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that a box-none node excludes only itself: a touch landing on its inert child still
 *        reaches an interactive ancestor, and one landing on its bare area still reaches the sibling
 *        painted behind it.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_pointer_events_box_none_passes_through(void)
{
    /* An interactive ancestor above the box-none wrapper still gets the press. */
    {
        ERNode* root = create_root();
        EventCounts outer_counts = {0};
        ERNode* pressable = create_pressable(0, 0, 120, 120, &outer_counts);
        create_box_none_overlay(pressable, NULL);
        er_tree_append_child(root, pressable);
        er_commit();

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 50, 50);
        embedded_renderer_touch(0, ER_TOUCH_UP, 50, 50);

        if (outer_counts.press_count != 1)
            return fail("pointer_events:box-none blocked an interactive ancestor");
    }

    /* A sibling behind the overlay gets touches aimed at the overlay's bare area. */
    {
        ERNode* root = create_root();
        EventCounts behind_counts = {0};
        ERNode* behind = create_pressable(0, 0, 120, 120, &behind_counts);
        er_tree_append_child(root, behind);
        create_box_none_overlay(root, NULL);
        er_commit();

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
        embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);

        if (behind_counts.press_count != 1)
            return fail("pointer_events:box-none did not pass a touch to the sibling behind it");
    }

    return EXIT_SUCCESS;
}

/**
 * @brief A drag that scrolls a ScrollView ends the press it began as: the row the finger started on gets
 *        its press-out and no press on release. On a list whose content fits, nothing scrolls, so the
 *        same drag still presses.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_scroll_cancels_press(void)
{
    for (int fits = 0; fits <= 1; fits++)
    {
        ERNode* root = create_root();
        ERNode* sv = er_node_create(ER_NODE_SCROLL_VIEW);
        ERProps sp = props_default();
        sp.width = 200;
        sp.height = 100;
        er_node_set_props(sv, &sp);

        ERNode* row = er_node_create(ER_NODE_PRESSABLE);
        ERProps rp = props_default();
        rp.width = 200;
        rp.height = fits ? 50 : 400; /* shorter than the viewport, or taller */
        er_node_set_props(row, &rp);
        EventCounts counts;
        memset(&counts, 0, sizeof(counts));
        er_event_set(row, ER_EVENT_PRESS, on_press, &counts);
        er_event_set(row, ER_EVENT_PRESS_IN, on_press_in, &counts);
        er_event_set(row, ER_EVENT_PRESS_OUT, on_press_out, &counts);

        er_tree_append_child(sv, row);
        er_tree_append_child(root, sv);
        er_commit();

        /* Down on the row, then up 2 px a frame, as a finger moves: past the 5 px slop by the third move. */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 40);
        for (int y = 38; y >= 20; y -= 2)
        {
            embedded_renderer_tick(16U);
            touch_move(100, y);
        }
        embedded_renderer_touch(0, ER_TOUCH_UP, 100, 20);

        if (counts.press_in_count != 1 || counts.press_out_count != 1)
            return fail("a pressed row did not get exactly one press-in and one press-out");
        if (!fits && counts.press_count != 0)
            return fail("a drag that scrolled the list pressed the row it started on");
        if (fits && counts.press_count != 1)
            return fail("a drag on a list that cannot scroll lost its press");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief A drag the pressed node claims for itself ends its press: once its own move-should-set takes the
 *        gesture, releasing inside it is not a tap. A move too short to claim still presses.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_self_claimed_drag_cancels_press(void)
{
    for (int claims = 0; claims <= 1; claims++)
    {
        ERNode* root = create_root();
        ResponderRecord rec = {0};
        rec.should_claim = true;
        rec.min_abs_dx = 20; /* claim only when |dx| >= 20 */

        ERNode* node = er_node_create(ER_NODE_PRESSABLE);
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 0;
        p.top = 0;
        p.width = 80;
        p.height = 80;
        er_node_set_props(node, &p);
        EventCounts counts;
        memset(&counts, 0, sizeof(counts));
        er_event_set(node, ER_EVENT_PRESS, on_press, &counts);
        er_event_set(node, ER_EVENT_PRESS_IN, on_press_in, &counts);
        er_event_set(node, ER_EVENT_PRESS_OUT, on_press_out, &counts);
        wire_responder(node, &rec);
        er_responder_query_set(node, ER_QUERY_MOVE_SHOULD_SET, query_should_claim, &rec);

        er_tree_append_child(root, node);
        er_commit();

        const int end_x = claims ? 40 : 20; /* |dx| 30 claims; |dx| 10 does not */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
        touch_move(end_x, 10);
        embedded_renderer_touch(0, ER_TOUCH_UP, end_x, 10);

        if (rec.grant_count != claims)
            return fail("the node's own move-should-set claim did not happen as set up");
        if (counts.press_in_count != 1 || counts.press_out_count != 1)
            return fail("a pressed node did not get exactly one press-in and one press-out");
        if (claims && counts.press_count != 0)
            return fail("a drag the pressed node claimed for itself still pressed it");
        if (!claims && counts.press_count != 1)
            return fail("a short move the node did not claim lost its press");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief A container that takes the gesture at touch-down decides whether the node under the finger
 *        presses at all. Claiming in the capture phase takes the touch from everything below it, so the
 *        row gets no press-in, no long press and no press on release. Claiming by bubbling does not:
 *        a Pressable inside a container that wants the gesture still reports its own taps.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_start_capture_suppresses_press(void)
{
    for (int capture = 0; capture <= 1; capture++)
    {
        ERNode* root = create_root();
        ResponderRecord rec = {0};
        rec.should_claim = true;

        ERNode* container = er_node_create(ER_NODE_VIEW);
        {
            ERProps p = props_default();
            p.position = ER_POS_ABSOLUTE;
            p.left = 0;
            p.top = 0;
            p.width = 80;
            p.height = 80;
            er_node_set_props(container, &p);
            wire_responder(container, &rec);
            er_responder_query_set(container,
                                   capture ? ER_QUERY_START_SHOULD_SET_CAPTURE : ER_QUERY_START_SHOULD_SET,
                                   query_should_claim,
                                   &rec);
        }

        ERNode* row = er_node_create(ER_NODE_PRESSABLE);
        EventCounts counts;
        memset(&counts, 0, sizeof(counts));
        {
            ERProps p = props_default();
            p.position = ER_POS_ABSOLUTE;
            p.left = 10;
            p.top = 10;
            p.width = 40;
            p.height = 40;
            er_node_set_props(row, &p);
            er_event_set(row, ER_EVENT_PRESS, on_press, &counts);
            er_event_set(row, ER_EVENT_PRESS_IN, on_press_in, &counts);
            er_event_set(row, ER_EVENT_PRESS_OUT, on_press_out, &counts);
            er_event_set(row, ER_EVENT_LONG_PRESS, on_long_press, &counts);
        }

        er_tree_append_child(container, row);
        er_tree_append_child(root, container);
        er_commit();

        /* A tap on the row: down and straight back up, never moving. */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

        /* The same touch held past the long-press threshold. */
        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        embedded_renderer_tick(600U);
        embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

        if (rec.grant_count != 2)
            return fail("the container did not take the gesture at both touch-downs");

        /* Both touches press when the container only bubbles; the held one's release is consumed by
         * the long press, so it is the tap alone that fires onPress. */
        const int pressed = capture ? 0 : 1;
        if (counts.press_in_count != 2 * pressed || counts.press_out_count != 2 * pressed)
            return fail("a captured touch-down still gave the row a press-in");
        if (counts.press_count != pressed)
            return fail("a captured touch-down still pressed the row on release");
        if (counts.long_press_count != pressed)
            return fail("a captured touch-down still held the row into a long press");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief The node an event handler unmounts, the parent to unlink it from, and what it mounts in its place.
 */
typedef struct
{
    ERNode* parent;
    ERNode* victim;
    bool detach_only;                            /**< Unlink the victim but keep it alive, rather than destroying it. */
    ERNode* (*mount)(ERNode* parent, void* ctx); /**< Mounts a new node after the unmount, or NULL. */
    void* mount_ctx;
    ERNode* mounted; /**< What mount returned — the victim's pool slot, when it was destroyed. */
} UnmountOnEvent;

/**
 * @brief What a dial built by mount_dial() receives.
 */
typedef struct
{
    ResponderRecord rec;
    int value_changes;
} DialRecord;

/**
 * @brief Destroys a node and everything under it, children first, as the JS bridge does.
 *
 * @param[in] node  Root of the subtree to destroy.
 */
static void destroy_subtree(ERNode* node)
{
    ERNode* child = er_node_first_child(node);
    while (child)
    {
        ERNode* next = er_node_next_sibling(child);
        destroy_subtree(child);
        child = next;
    }
    er_node_destroy(node);
}

/** @brief Event callback: unmounts a node, and mounts one in its place, as a React commit in the handler would. */
static void on_event_unmount(ERNode* node, const EREventData* data, void* user_data)
{
    UnmountOnEvent* unmount = user_data;
    (void)node;
    (void)data;
    if (!unmount->victim)
        return;
    er_tree_remove_child(unmount->parent, unmount->victim);
    if (!unmount->detach_only)
        destroy_subtree(unmount->victim);
    unmount->victim = NULL;
    if (unmount->mount)
        unmount->mounted = unmount->mount(unmount->parent, unmount->mount_ctx);
}

/** @brief ER_EVENT_VALUE_CHANGE callback: counts the changes into an int. */
static void on_value_change_count(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (*(int*)user_data)++;
}

/**
 * @brief Mounts a Pressable at 10,10 (40x40), counting its events.
 *
 * @param[in] parent  Node to append it to.
 * @param[in] ctx     EventCounts receiving its events.
 *
 * @return The Pressable.
 */
static ERNode* mount_pressable(ERNode* parent, void* ctx)
{
    ERNode* node = create_pressable(10, 10, 40, 40, ctx);
    er_tree_append_child(parent, node);
    return node;
}

/**
 * @brief The parent mount_pressable_under() mounts into, and what receives the Pressable's events.
 */
typedef struct
{
    ERNode* parent;
    EventCounts* counts;
} MountUnder;

/**
 * @brief Mounts a Pressable with mount_pressable(), under the parent ctx names rather than the one passed.
 *
 * @param[in] parent  Ignored.
 * @param[in] ctx     MountUnder naming the parent and the EventCounts.
 *
 * @return The Pressable.
 */
static ERNode* mount_pressable_under(ERNode* parent, void* ctx)
{
    const MountUnder* under = ctx;
    (void)parent;
    return mount_pressable(under->parent, under->counts);
}

/**
 * @brief Mounts an adjustable dial at 20,20 (100x100, 16 px band, value 25 of 100), recording its events.
 *
 * @param[in] parent  Node to append it to.
 * @param[in] ctx     DialRecord receiving its events.
 *
 * @return The dial.
 */
static ERNode* mount_dial(ERNode* parent, void* ctx)
{
    DialRecord* dial = ctx;
    ERNode* arc = er_node_create(ER_NODE_ARC);
    ERProps p;
    er_props_default(&p);
    p.position = ER_POS_ABSOLUTE;
    p.left = 20;
    p.top = 20;
    p.width = 100;
    p.height = 100;
    p.arc_width = 16;
    p.arc_value = 25.0f;
    p.arc_max = 100.0f;
    p.arc_step = 5.0f;
    p.arc_adjustable = 1;
    er_node_set_props(arc, &p);
    wire_responder(arc, &dial->rec);
    er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_value_change_count, &dial->value_changes);
    er_tree_append_child(parent, arc);
    return arc;
}

/**
 * @brief Mounts an editable TextInput at 10,10 (40x40), counting its focus, press and raw touch events.
 *
 * @param[in] parent  Node to append it to.
 * @param[in] ctx     EventCounts receiving its events.
 *
 * @return The TextInput.
 */
static ERNode* mount_text_input(ERNode* parent, void* ctx)
{
    ERNode* node = er_node_create(ER_NODE_TEXT_INPUT);
    ERProps p;
    er_props_default(&p);
    p.position = ER_POS_ABSOLUTE;
    p.left = 10;
    p.top = 10;
    p.width = 40;
    p.height = 40;
    er_node_set_props(node, &p);
    er_event_set(node, ER_EVENT_FOCUS, on_focus, ctx);
    er_event_set(node, ER_EVENT_PRESS, on_press, ctx);
    er_event_set(node, ER_EVENT_PRESS_IN, on_press_in, ctx);
    er_event_set(node, ER_EVENT_PRESS_OUT, on_press_out, ctx);
    er_event_set(node, ER_EVENT_TOUCH_MOVE, on_touch_move, ctx);
    er_event_set(node, ER_EVENT_TOUCH_END, on_touch_end, ctx);
    er_event_set(node, ER_EVENT_TOUCH_CANCEL, on_touch_cancel, ctx);
    er_tree_append_child(parent, node);
    return node;
}

/**
 * @brief Mounts a View at 150,10 (20x20), well away from the finger, recording its responder events.
 *
 * @param[in] parent  Node to append it to.
 * @param[in] ctx     ResponderRecord receiving its events.
 *
 * @return The View.
 */
static ERNode* mount_view(ERNode* parent, void* ctx)
{
    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 150;
    p.top = 10;
    p.width = 20;
    p.height = 20;
    er_node_set_props(node, &p);
    wire_responder(node, ctx);
    er_tree_append_child(parent, node);
    return node;
}

/** @brief Should-set query callback: unmounts a node (see on_event_unmount), then claims the responder. */
static bool query_unmount_and_claim(ERNode* node, const EREventData* data, void* user_data)
{
    on_event_unmount(node, data, user_data);
    return true;
}

/** @brief Should-set query callback: unmounts a node (see on_event_unmount), then declines the responder. */
static bool query_unmount_and_decline(ERNode* node, const EREventData* data, void* user_data)
{
    on_event_unmount(node, data, user_data);
    return false;
}

/**
 * @brief Mounts a View with mount_view() that also claims the responder on touch-down, in capture and bubble.
 *
 * @param[in] parent  Node to append it to.
 * @param[in] ctx     ResponderRecord receiving its events; its should_claim decides the claim.
 *
 * @return The View.
 */
static ERNode* mount_claiming_view(ERNode* parent, void* ctx)
{
    ERNode* node = mount_view(parent, ctx);
    er_responder_query_set(node, ER_QUERY_START_SHOULD_SET_CAPTURE, query_should_claim, ctx);
    er_responder_query_set(node, ER_QUERY_START_SHOULD_SET, query_should_claim, ctx);
    return node;
}

/**
 * @brief Creates an absolutely positioned View and appends it to a parent.
 *
 * @param[in] parent  Node to append it to.
 * @param[in] x,y     Position inside the parent.
 * @param[in] w,h     Size in pixels.
 *
 * @return The View.
 */
static ERNode* append_view(ERNode* parent, int16_t x, int16_t y, int16_t w, int16_t h)
{
    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = x;
    p.top = y;
    p.width = w;
    p.height = h;
    er_node_set_props(node, &p);
    er_tree_append_child(parent, node);
    return node;
}

/**
 * @brief The pressed node can be gone before its press even starts: the should-set queries and the
 *        grant are app code, and under the JS bridge that code can commit a React update that unmounts
 *        it. A destroyed node keeps its handlers until its pool slot is reused, so a press-in carried
 *        across the grant on a raw pointer still calls them — and a node the grant mounts into that slot
 *        is not the one the finger pressed.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_press_target_unmounted_during_grant(void)
{
    for (int replace = 0; replace <= 1; replace++)
    {
        ERNode* root = create_root();
        ResponderRecord rec = {0};
        rec.should_claim = true;

        ERNode* container = er_node_create(ER_NODE_VIEW);
        {
            ERProps p = props_default();
            p.position = ER_POS_ABSOLUTE;
            p.left = 0;
            p.top = 0;
            p.width = 80;
            p.height = 80;
            er_node_set_props(container, &p);
            /* Bubbling, not capturing: the path that leaves the press in place is the one that has to
             * survive the node underneath it going away. */
            er_responder_query_set(container, ER_QUERY_START_SHOULD_SET, query_should_claim, &rec);
        }

        ERNode* row = er_node_create(ER_NODE_PRESSABLE);
        EventCounts counts;
        memset(&counts, 0, sizeof(counts));
        {
            ERProps p = props_default();
            p.position = ER_POS_ABSOLUTE;
            p.left = 10;
            p.top = 10;
            p.width = 40;
            p.height = 40;
            er_node_set_props(row, &p);
            er_event_set(row, ER_EVENT_PRESS, on_press, &counts);
            er_event_set(row, ER_EVENT_PRESS_IN, on_press_in, &counts);
            er_event_set(row, ER_EVENT_PRESS_OUT, on_press_out, &counts);
        }

        er_tree_append_child(container, row);
        er_tree_append_child(root, container);
        er_commit();

        EventCounts successor;
        memset(&successor, 0, sizeof(successor));
        UnmountOnEvent unmount = {container, row, false, replace ? mount_pressable : NULL, &successor, NULL};
        er_event_set(container, ER_EVENT_RESPONDER_GRANT, on_event_unmount, &unmount);

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        er_commit(); /* lay the new node out under the finger, so releasing there would press it */
        embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

        if (unmount.victim != NULL)
            return fail("the grant handler never unmounted the pressed node");
        if (replace && unmount.mounted != row)
            return fail("the grant's new node did not reuse the pressed node's slot, so the scenario proves nothing");
        if (counts.press_in_count != 0 || counts.press_out_count != 0 || counts.press_count != 0)
            return fail("a node unmounted during the responder grant was still pressed");
        if (successor.press_in_count != 0 || successor.press_out_count != 0 || successor.press_count != 0)
            return fail("a node the grant mounted into the pressed node's slot was pressed in its place");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief A node that unmounts itself from its own onTouchStart takes no further part in the touch: no
 *        responder is negotiated from it, and a node that takes over its pool slot — mounted by that same
 *        handler, or later — is not handed the moves and release of a finger that never touched it.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_touch_target_unmounted_by_touch_start(void)
{
    for (int in_handler = 0; in_handler <= 1; in_handler++)
    {
        ERNode* root = create_root();
        ResponderRecord rec = {0};
        rec.should_claim = true;

        ERNode* container = er_node_create(ER_NODE_VIEW);
        {
            ERProps p = props_default();
            p.position = ER_POS_ABSOLUTE;
            p.left = 0;
            p.top = 0;
            p.width = 80;
            p.height = 80;
            er_node_set_props(container, &p);
            wire_responder(container, &rec);
            er_responder_query_set(container, ER_QUERY_START_SHOULD_SET, query_should_claim, &rec);
        }

        ERNode* target = er_node_create(ER_NODE_VIEW);
        {
            ERProps p = props_default();
            p.position = ER_POS_ABSOLUTE;
            p.left = 10;
            p.top = 10;
            p.width = 40;
            p.height = 40;
            er_node_set_props(target, &p);
        }

        er_tree_append_child(container, target);
        er_tree_append_child(root, container);
        er_commit();

        EventCounts successor;
        memset(&successor, 0, sizeof(successor));
        UnmountOnEvent unmount = {container, target, false, in_handler ? mount_pressable : NULL, &successor, NULL};
        er_event_set(target, ER_EVENT_TOUCH_START, on_event_unmount, &unmount);

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);

        if (unmount.victim != NULL)
            return fail("onTouchStart never unmounted the touched node");
        if (!in_handler)
            unmount.mounted = mount_pressable(container, &successor);
        if (unmount.mounted != target)
            return fail("the new node did not reuse the unmounted node's slot, so the scenario proves nothing");
        er_commit();

        touch_move(30, 20);
        embedded_renderer_touch(0, ER_TOUCH_UP, 30, 20);

        if (rec.grant_count != 0)
            return fail("a responder was negotiated from a node unmounted by its own onTouchStart");
        if (successor.touch_move_count != 0 || successor.touch_end_count != 0 || successor.press_count != 0)
            return fail(in_handler ? "a node onTouchStart mounted into the touched node's slot was handed its touch"
                                   : "a node that later took the touched node's slot was handed its touch");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief A node that unmounts itself from its own onTouchStart still bubbles it to the ancestors it had when the
 *        finger landed, whether it was detached, destroyed, or destroyed and its pool slot handed to a node
 *        mounted elsewhere. The ancestors of that new node are not handed the touch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_touch_start_bubbles_from_unmounted_target(void)
{
    enum
    {
        DETACH,
        DESTROY,
        REPLACE_ELSEWHERE
    };
    for (int mode = DETACH; mode <= REPLACE_ELSEWHERE; mode++)
    {
        ERNode* root = create_root();
        EventCounts ancestors;
        memset(&ancestors, 0, sizeof(ancestors));
        ERNode* grand = append_view(root, 0, 0, 120, 120);
        er_event_set(grand, ER_EVENT_TOUCH_START, on_touch_start, &ancestors);
        ERNode* parent = append_view(grand, 0, 0, 80, 80);
        er_event_set(parent, ER_EVENT_TOUCH_START, on_touch_start, &ancestors);
        ERNode* target = append_view(parent, 10, 10, 40, 40);

        EventCounts strangers;
        memset(&strangers, 0, sizeof(strangers));
        ERNode* other = append_view(root, 130, 0, 100, 100);
        er_event_set(other, ER_EVENT_TOUCH_START, on_touch_start, &strangers);
        er_commit();

        EventCounts successor;
        memset(&successor, 0, sizeof(successor));
        MountUnder under = {other, &successor};
        UnmountOnEvent unmount = {
            parent, target, mode == DETACH, mode == REPLACE_ELSEWHERE ? mount_pressable_under : NULL, &under, NULL};
        er_event_set(target, ER_EVENT_TOUCH_START, on_event_unmount, &unmount);

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

        if (unmount.victim != NULL)
            return fail("onTouchStart never unmounted the touched node");
        if (mode == REPLACE_ELSEWHERE && unmount.mounted != target)
            return fail("the new node did not reuse the touched node's slot, so the scenario proves nothing");
        if (ancestors.touch_start_count != 2)
            return fail("a node that unmounted itself in onTouchStart did not bubble it to its original ancestors");
        if (strangers.touch_start_count != 0 || successor.touch_start_count != 0)
            return fail("a node mounted into the touched node's slot carried its touch-start up its own ancestors");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief An adjustable Arc that leaves the scene from its own onTouchStart starts no native drag, whether
 *        it was destroyed or only detached: the drag writes the value of a dial no longer on screen and
 *        fires its handlers, so the negotiated responder would be a node the finger is not on.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_arc_unmounted_by_touch_start_does_not_drag(void)
{
    for (int destroy = 0; destroy <= 1; destroy++)
    {
        ERNode* root = create_root();
        DialRecord dial = {0};
        ERNode* arc = mount_dial(root, &dial);
        er_commit();

        /* An armed handler with nothing to unmount: the touch drags, so the scenario can tell. */
        UnmountOnEvent unmount = {root, NULL, destroy == 0, NULL, NULL, NULL};
        er_event_set(arc, ER_EVENT_TOUCH_START, on_event_unmount, &unmount);
        embedded_renderer_touch(0, ER_TOUCH_DOWN, DIAL_TOP_X, DIAL_TOP_Y);
        embedded_renderer_touch(0, ER_TOUCH_UP, DIAL_TOP_X, DIAL_TOP_Y);
        if (dial.rec.grant_count != 1 || dial.value_changes != 1)
            return fail("a touch on the ring did not drag the dial, so the scenario proves nothing");

        dial.rec.grant_count = 0;
        dial.value_changes = 0;

        unmount.victim = arc;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, DIAL_RIGHT_X, DIAL_RIGHT_Y);
        embedded_renderer_touch(0, ER_TOUCH_UP, DIAL_RIGHT_X, DIAL_RIGHT_Y);

        if (unmount.victim != NULL)
            return fail("onTouchStart never unmounted the dial");
        if (dial.rec.grant_count != 0 || dial.value_changes != 0)
            return fail(destroy ? "a dial destroyed by its own onTouchStart still started a drag"
                                : "a dial detached by its own onTouchStart still started a drag");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief The native drag grants the dial the responder before it latches the finger, and the grant is app
 *        code: a dial it unmounts or detaches takes no drag, and neither does a dial it mounts into the same
 *        pool slot.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_arc_unmounted_by_grant_does_not_drag(void)
{
    /* 0: destroyed, 1: destroyed and replaced in its slot, 2: detached but alive. */
    for (int mode = 0; mode <= 2; mode++)
    {
        const bool replace = mode == 1;
        ERNode* root = create_root();
        DialRecord dial = {0};
        ERNode* arc = mount_dial(root, &dial);
        er_commit();

        DialRecord successor = {0};
        UnmountOnEvent unmount = {root, arc, mode == 2, replace ? mount_dial : NULL, &successor, NULL};
        er_event_set(arc, ER_EVENT_RESPONDER_GRANT, on_event_unmount, &unmount);

        embedded_renderer_touch(0, ER_TOUCH_DOWN, DIAL_TOP_X, DIAL_TOP_Y);
        er_commit();
        touch_move(DIAL_RIGHT_X, DIAL_RIGHT_Y);
        embedded_renderer_touch(0, ER_TOUCH_UP, DIAL_RIGHT_X, DIAL_RIGHT_Y);

        if (unmount.victim != NULL)
            return fail("onResponderGrant never unmounted the dial");
        if (replace && unmount.mounted != arc)
            return fail("the grant's new dial did not reuse the old one's slot, so the scenario proves nothing");
        if (dial.value_changes != 0)
            return fail(mode == 2 ? "a dial detached by its own onResponderGrant was still dragged"
                                  : "a dial unmounted by its own onResponderGrant was still dragged");
        if (successor.value_changes != 0 || successor.rec.move_count != 0 || successor.rec.release_count != 0)
            return fail("a dial the grant mounted into the old one's slot was dragged by its touch");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief Builds a horizontally scrollable ScrollView holding one inert (handler-less) child.
 *
 * @param[in] parent          Node to append the ScrollView to.
 * @param[in] pointer_events  Pointer-events mode for the ScrollView itself.
 * @param[in] width           Viewport width; the content child is made four times as wide.
 * @param[in] scroll_events   Counter wired to ER_EVENT_SCROLL.
 *
 * @return The ScrollView node.
 */
static ERNode* create_scroll_view(ERNode* parent, ERPointerEvents pointer_events, int16_t width, int* scroll_events)
{
    ERNode* sv = er_node_create(ER_NODE_SCROLL_VIEW);
    ERProps sp = props_default();
    sp.position = ER_POS_ABSOLUTE;
    sp.left = 0;
    sp.top = 0;
    sp.width = width;
    sp.height = 100;
    sp.pointer_events = (uint8_t)pointer_events;
    er_node_set_props(sv, &sp);
    er_event_set(sv, ER_EVENT_SCROLL, count_scroll, scroll_events);

    ERNode* content = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.width = (int16_t)(width * 4); /* wider than the viewport so horizontal scrolling has range */
    cp.height = 100;
    er_node_set_props(content, &cp);

    er_tree_append_child(sv, content);
    er_tree_append_child(parent, sv);
    return sv;
}

/**
 * @brief Drags finger 0 leftwards across a ScrollView, well past ER_SCROLL_SLOP.
 */
static void drag_past_slop(void)
{
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 80, 50);
    embedded_renderer_tick(16U);
    touch_move(70, 50);
    embedded_renderer_tick(16U);
    touch_move(60, 50);
    embedded_renderer_tick(16U);
    touch_move(50, 50);
    embedded_renderer_touch(0, ER_TOUCH_UP, 50, 50);
}

/**
 * @brief A press dispatch is app code, and so is every raw touch around it: a handler that destroys the
 *        pressed node and mounts a TextInput into its pool slot must not see that input focused, pressed,
 *        or handed the rest of the touch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_press_target_replaced_mid_touch(void)
{
    static const struct
    {
        EREventType event;
        const char* failure;
    } k_swaps[] = {
        {ER_EVENT_PRESS_IN, "an input mounted into the pressed node's slot by onPressIn was focused or pressed"},
        {ER_EVENT_TOUCH_MOVE, "an input mounted into the pressed node's slot by onTouchMove was pressed"},
        {ER_EVENT_TOUCH_END, "an input mounted into the pressed node's slot by onTouchEnd was pressed"},
        {ER_EVENT_TOUCH_CANCEL, "an input mounted into the pressed node's slot by onTouchCancel was pressed"},
    };

    for (size_t i = 0; i < sizeof(k_swaps) / sizeof(k_swaps[0]); i++)
    {
        ERNode* root = create_root();
        EventCounts counts;
        memset(&counts, 0, sizeof(counts));
        ERNode* row = create_pressable(10, 10, 40, 40, &counts);
        er_tree_append_child(root, row);
        er_commit();

        EventCounts successor;
        memset(&successor, 0, sizeof(successor));
        UnmountOnEvent unmount = {root, row, false, mount_text_input, &successor, NULL};
        er_event_set(row, k_swaps[i].event, on_event_unmount, &unmount);

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        touch_move(22, 20);
        embedded_renderer_touch(0, k_swaps[i].event == ER_EVENT_TOUCH_CANCEL ? ER_TOUCH_CANCEL : ER_TOUCH_UP, 22, 20);

        if (unmount.victim != NULL)
            return fail("the handler never unmounted the pressed node");
        if (unmount.mounted != row)
            return fail("the new input did not reuse the pressed node's slot, so the scenario proves nothing");
        if (successor.focus_count != 0 || successor.press_in_count != 0 || successor.press_out_count != 0
            || successor.press_count != 0 || successor.touch_move_count != 0 || successor.touch_end_count != 0
            || successor.touch_cancel_count != 0)
            return fail(k_swaps[i].failure);
    }
    return EXIT_SUCCESS;
}

/**
 * @brief A should-set query is app code: a node that destroys itself from its own query and mounts another
 *        into its pool slot claims nothing, and the node in the slot is not granted a touch it never had.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_claimant_replaced_by_own_query(void)
{
    ERNode* root = create_root();
    ERNode* target = er_node_create(ER_NODE_VIEW);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 10;
        p.top = 10;
        p.width = 40;
        p.height = 40;
        er_node_set_props(target, &p);
    }
    er_tree_append_child(root, target);
    er_commit();

    ResponderRecord successor = {0};
    UnmountOnEvent unmount = {root, target, false, mount_view, &successor, NULL};
    er_responder_query_set(target, ER_QUERY_START_SHOULD_SET, query_unmount_and_claim, &unmount);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
    embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

    if (unmount.victim != NULL)
        return fail("the should-set query never unmounted the touched node");
    if (unmount.mounted != target)
        return fail("the new node did not reuse the claimant's slot, so the scenario proves nothing");
    if (successor.grant_count != 0 || successor.release_count != 0)
        return fail("a node the claimant's own query mounted into its slot was granted the responder");

    return EXIT_SUCCESS;
}

/**
 * @brief A responder move is app code: when it destroys the responder, the rest of that move must not run the
 *        hand-off negotiation from whatever the freed slot now holds.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_replaced_during_move(void)
{
    ERNode* root = create_root();
    ResponderRecord root_rec = {0};
    root_rec.should_claim = true;
    wire_responder(root, &root_rec);
    er_responder_query_set(root, ER_QUERY_MOVE_SHOULD_SET, query_should_claim, &root_rec);

    ResponderRecord rec = {0};
    rec.should_claim = true;
    ERNode* target = er_node_create(ER_NODE_VIEW);
    {
        ERProps p = props_default();
        p.position = ER_POS_ABSOLUTE;
        p.left = 10;
        p.top = 10;
        p.width = 40;
        p.height = 40;
        er_node_set_props(target, &p);
        wire_responder(target, &rec);
        er_responder_query_set(target, ER_QUERY_START_SHOULD_SET, query_should_claim, &rec);
    }
    er_tree_append_child(root, target);
    er_commit();

    ResponderRecord successor = {0};
    UnmountOnEvent unmount = {root, target, false, mount_view, &successor, NULL};
    er_event_set(target, ER_EVENT_RESPONDER_MOVE, on_event_unmount, &unmount);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
    if (rec.grant_count != 1)
        return fail("the touched node did not take the gesture, so the scenario proves nothing");
    touch_move(30, 20);
    embedded_renderer_touch(0, ER_TOUCH_UP, 30, 20);

    if (unmount.victim != NULL)
        return fail("onResponderMove never unmounted the responder");
    if (unmount.mounted != target)
        return fail("the new node did not reuse the responder's slot, so the scenario proves nothing");
    if (root_rec.grant_count != 0 || successor.grant_count != 0)
        return fail("a move negotiated the gesture from the node mounted into the unmounted responder's slot");

    return EXIT_SUCCESS;
}

/**
 * @brief Mounts a scrollable ScrollView with create_scroll_view(), for an unmount scenario to swap in.
 *
 * @param[in] parent  Node to append it to.
 * @param[in] ctx     int counting its ER_EVENT_SCROLL callbacks.
 *
 * @return The ScrollView.
 */
static ERNode* mount_scroll_view(ERNode* parent, void* ctx)
{
    return create_scroll_view(parent, ER_POINTER_EVENTS_AUTO, 100, ctx);
}

/**
 * @brief The release is app code: a ScrollView that destroys itself from onResponderRelease does not hand its
 *        fling to a ScrollView mounted into its pool slot.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_scroll_responder_replaced_on_release(void)
{
    ERNode* root = create_root();
    int scroll_events = 0;
    ERNode* sv = create_scroll_view(root, ER_POINTER_EVENTS_AUTO, 100, &scroll_events);
    er_commit();

    int successor_scrolls = 0;
    UnmountOnEvent unmount = {root, sv, false, mount_scroll_view, &successor_scrolls, NULL};
    er_event_set(sv, ER_EVENT_RESPONDER_RELEASE, on_event_unmount, &unmount);

    /* Lift away from the last move, a frame later, so the release measures a real velocity. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 80, 50);
    embedded_renderer_tick(16U);
    touch_move(60, 50);
    embedded_renderer_tick(16U);
    embedded_renderer_touch(0, ER_TOUCH_UP, 50, 50);

    if (scroll_events == 0)
        return fail("the drag never scrolled, so the scenario proves nothing");
    if (unmount.victim != NULL)
        return fail("onResponderRelease never unmounted the ScrollView");
    if (unmount.mounted != sv)
        return fail("the new ScrollView did not reuse the old one's slot, so the scenario proves nothing");

    er_commit();
    for (int i = 0; i < 10; i++)
        embedded_renderer_tick(16U);

    if (successor_scrolls != 0)
        return fail("a ScrollView mounted into the released scroller's slot inherited its fling");

    return EXIT_SUCCESS;
}

/**
 * @brief A responder destroyed mid-drag, by its own onResponderMove or by a commit between moves, gets nothing
 *        more of the gesture, and neither does a node mounted into its pool slot. The gesture stays claimed, so
 *        the ScrollView behind it does not take over the rest of the drag.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_responder_destroyed_mid_drag(void)
{
    for (int in_handler = 0; in_handler <= 1; in_handler++)
    {
        for (int replace = 0; replace <= 1; replace++)
        {
            ERNode* root = create_root();
            int scroll_events = 0;
            ERNode* sv = create_scroll_view(root, ER_POINTER_EVENTS_AUTO, 100, &scroll_events);
            ERNode* content = er_node_first_child(sv);

            ResponderRecord rec = {0};
            rec.should_claim = true;
            ERNode* target = append_view(content, 10, 10, 40, 40);
            wire_responder(target, &rec);
            er_responder_query_set(target, ER_QUERY_START_SHOULD_SET, query_should_claim, &rec);
            er_commit();

            ResponderRecord successor = {0};
            UnmountOnEvent unmount = {content, target, false, replace ? mount_view : NULL, &successor, NULL};
            if (in_handler)
                er_event_set(target, ER_EVENT_RESPONDER_MOVE, on_event_unmount, &unmount);

            embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
            if (rec.grant_count != 1)
                return fail("the touched node did not take the gesture, so the scenario proves nothing");
            touch_move(30, 20);
            if (!in_handler)
                on_event_unmount(NULL, NULL, &unmount);
            er_commit();

            if (unmount.victim != NULL)
                return fail("the responder was never unmounted");
            if (replace && unmount.mounted != target)
                return fail("the new node did not reuse the responder's slot, so the scenario proves nothing");

            const int moves_before = rec.move_count;
            touch_move(40, 20);
            touch_move(50, 20);
            embedded_renderer_touch(0, ER_TOUCH_UP, 50, 20);

            if (rec.move_count != moves_before || rec.release_count != 0 || rec.terminate_count != 0)
                return fail("a responder destroyed mid-drag was handed the rest of the gesture");
            if (successor.grant_count != 0 || successor.move_count != 0 || successor.release_count != 0
                || successor.terminate_count != 0)
                return fail("a node mounted into a destroyed responder's slot was handed the rest of its gesture");
            if (scroll_events != 0)
                return fail("the ScrollView behind a responder destroyed mid-drag took over the rest of the drag");
        }
    }
    return EXIT_SUCCESS;
}

/**
 * @brief A dial destroyed mid-drag, by its own onValueChange or by a commit between moves, is dragged no further.
 *        A dial mounted into its pool slot is neither dragged nor held by that finger: a second finger can still
 *        drag it, and lifting the first releases nothing.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_arc_destroyed_mid_drag(void)
{
    const int bottom_x = 70;
    const int bottom_y = 70 + 42;

    for (int in_handler = 0; in_handler <= 1; in_handler++)
    {
        ERNode* root = create_root();
        DialRecord dial = {0};
        ERNode* arc = mount_dial(root, &dial);
        er_commit();

        DialRecord successor = {0};
        UnmountOnEvent unmount = {root, NULL, false, mount_dial, &successor, NULL};
        if (in_handler)
            er_event_set(arc, ER_EVENT_VALUE_CHANGE, on_event_unmount, &unmount);

        embedded_renderer_touch(0, ER_TOUCH_DOWN, DIAL_TOP_X, DIAL_TOP_Y);
        if (dial.rec.grant_count != 1)
            return fail("a touch on the ring did not start a drag, so the scenario proves nothing");

        unmount.victim = arc;
        touch_move(DIAL_RIGHT_X, DIAL_RIGHT_Y);
        if (!in_handler)
            on_event_unmount(NULL, NULL, &unmount);
        er_commit();

        if (unmount.victim != NULL)
            return fail("the dial was never unmounted");
        if (unmount.mounted != arc)
            return fail("the new dial did not reuse the old one's slot, so the scenario proves nothing");

        touch_move(bottom_x, bottom_y);
        if (successor.value_changes != 0 || successor.rec.grant_count != 0 || successor.rec.move_count != 0)
            return fail("a dial mounted into a dragged dial's slot was dragged by that finger");

        embedded_renderer_touch(1, ER_TOUCH_DOWN, DIAL_TOP_X, DIAL_TOP_Y);
        embedded_renderer_touch(1, ER_TOUCH_UP, DIAL_TOP_X, DIAL_TOP_Y);
        if (successor.rec.grant_count != 1 || successor.value_changes != 1)
            return fail("a dial mounted into a dragged dial's slot was still held by the old drag's finger");

        embedded_renderer_touch(0, ER_TOUCH_UP, bottom_x, bottom_y);
        if (successor.rec.release_count != 1)
            return fail("lifting the old drag's finger released the dial mounted into its slot");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief A raw touch bubbles along the ancestors the touched node had when it was dispatched. A handler that
 *        swaps out a subtree on that path hands nothing to a node mounted into one of its pool slots, and the
 *        ancestors above the swap still receive the touch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_ancestor_replaced_while_bubbling(void)
{
    ERNode* root = create_root();
    EventCounts outer;
    memset(&outer, 0, sizeof(outer));
    ERNode* grand = append_view(root, 0, 0, 120, 120);
    er_event_set(grand, ER_EVENT_TOUCH_START, on_touch_start, &outer);
    ERNode* parent = append_view(grand, 0, 0, 80, 80);
    ERNode* leaf = append_view(parent, 10, 10, 40, 40);
    er_commit();

    EventCounts successor;
    memset(&successor, 0, sizeof(successor));
    UnmountOnEvent unmount = {grand, parent, false, mount_pressable, &successor, NULL};
    er_event_set(leaf, ER_EVENT_TOUCH_START, on_event_unmount, &unmount);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
    embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

    if (unmount.victim != NULL)
        return fail("onTouchStart never unmounted the touched node's parent");
    if (unmount.mounted != parent)
        return fail("the new node did not reuse the parent's slot, so the scenario proves nothing");
    if (successor.touch_start_count != 0)
        return fail("a node mounted into an ancestor's slot received the touch-start bubbling through it");
    if (outer.touch_start_count != 1)
        return fail("the touch-start stopped bubbling at the swapped subtree instead of reaching the node above it");

    return EXIT_SUCCESS;
}

/**
 * @brief Responder negotiation asks the ancestors the touched node had when it began. A should-set query that
 *        swaps out a subtree further along that path, in either phase, lets no node mounted into one of its pool
 *        slots be asked or claim the touch, and the ancestor above the swap is still asked.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_ancestor_replaced_during_negotiation(void)
{
    for (int capture = 0; capture <= 1; capture++)
    {
        ERNode* root = create_root();
        ResponderRecord outer = {0};
        outer.should_claim = true;
        ERNode* grand = append_view(root, 0, 0, 120, 120);
        wire_responder(grand, &outer);
        er_responder_query_set(grand, ER_QUERY_START_SHOULD_SET, query_should_claim, &outer);
        ERNode* parent = append_view(grand, 0, 0, 80, 80);
        ERNode* leaf = append_view(parent, 10, 10, 40, 40);
        er_commit();

        ResponderRecord successor = {0};
        successor.should_claim = true;
        UnmountOnEvent unmount = {grand, parent, false, mount_claiming_view, &successor, NULL};
        /* Capture runs root to leaf, so the swap is made from above it; bubble runs leaf to root, from below. */
        if (capture)
            er_responder_query_set(grand, ER_QUERY_START_SHOULD_SET_CAPTURE, query_unmount_and_decline, &unmount);
        else
            er_responder_query_set(leaf, ER_QUERY_START_SHOULD_SET, query_unmount_and_decline, &unmount);

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

        if (unmount.victim != NULL)
            return fail("the should-set query never unmounted the touched node's parent");
        if (unmount.mounted != parent)
            return fail("the new node did not reuse the parent's slot, so the scenario proves nothing");
        if (successor.query_count != 0)
            return fail("a node mounted into an ancestor's slot was asked whether it wants the touch");
        if (successor.grant_count != 0)
            return fail("a node mounted into an ancestor's slot claimed the touch in its place");
        if (outer.grant_count != 1 || outer.release_count != 1)
            return fail("negotiation stopped at the swapped subtree instead of asking the node above it");
    }
    return EXIT_SUCCESS;
}

/** @brief Event callback: empties the scene, as a reload from inside a handler would. */
static void on_event_reset_scene(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    er_reset();
}

/** @brief Should-set query callback: empties the scene, then claims the responder. */
static bool query_reset_and_claim(ERNode* node, const EREventData* data, void* user_data)
{
    on_event_reset_scene(node, data, user_data);
    return true;
}

/**
 * @brief Resetting the scene from a handler ends every node in it, even ones whose slots nothing reuses: the
 *        rest of the touch reaches none of them, and the pool reports them gone.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_scene_reset_during_touch(void)
{
    {
        ERNode* root = create_root();
        ResponderRecord rec = {0};
        ERNode* target = append_view(root, 10, 10, 40, 40);
        wire_responder(target, &rec);
        er_responder_query_set(target, ER_QUERY_START_SHOULD_SET, query_reset_and_claim, &rec);
        er_commit();

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

        if (er_node_in_use_count() != 0)
            return fail("nodes from before a reset are still reported in use");
        if (rec.grant_count != 0 || rec.release_count != 0)
            return fail("a node whose own query reset the scene was granted the touch");
    }
    {
        ERNode* root = create_root();
        EventCounts counts;
        memset(&counts, 0, sizeof(counts));
        ERNode* row = create_pressable(10, 10, 40, 40, &counts);
        er_tree_append_child(root, row);
        er_event_set(row, ER_EVENT_PRESS_OUT, on_event_reset_scene, NULL);
        er_commit();

        embedded_renderer_touch(0, ER_TOUCH_DOWN, 20, 20);
        if (counts.press_in_count != 1)
            return fail("the row was not pressed, so the scenario proves nothing");
        embedded_renderer_touch(0, ER_TOUCH_UP, 20, 20);

        if (counts.press_count != 0)
            return fail("releasing on a row whose onPressOut reset the scene still pressed it");
    }
    return EXIT_SUCCESS;
}

/**
 * @brief Checks that the auto-scroll grant honours pointer_events: a ScrollView declared
 *        box-none or none is transparent to a pan aimed at itself, while a plain one scrolls.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_pointer_events_box_none_scroll_view(void)
{
    /* Control: a plain ScrollView takes the pan. */
    {
        ERNode* root = create_root();
        int scroll_events = 0;
        create_scroll_view(root, ER_POINTER_EVENTS_AUTO, 100, &scroll_events);
        er_commit();

        drag_past_slop();

        if (scroll_events == 0)
            return fail("plain ScrollView did not auto-scroll on a pan past the slop");
    }

    {
        ERNode* root = create_root();
        int scroll_events = 0;
        create_scroll_view(root, ER_POINTER_EVENTS_BOX_NONE, 100, &scroll_events);
        er_commit();

        drag_past_slop();

        if (scroll_events != 0)
            return fail("pointer_events:box-none ScrollView was auto-granted the pan");
    }

    {
        ERNode* root = create_root();
        int scroll_events = 0;
        create_scroll_view(root, ER_POINTER_EVENTS_NONE, 100, &scroll_events);
        er_commit();

        drag_past_slop();

        if (scroll_events != 0)
            return fail("pointer_events:none ScrollView was auto-granted the pan");
    }

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that skipping a box-none ScrollView does not swallow the pan: the search keeps
 *        climbing and an outer scrollable ancestor takes it instead.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_box_none_scroll_view_defers_to_outer(void)
{
    ERNode* root = create_root();
    int outer_events = 0;
    int inner_events = 0;

    ERNode* outer = er_node_create(ER_NODE_SCROLL_VIEW);
    ERProps op = props_default();
    op.position = ER_POS_ABSOLUTE;
    op.left = 0;
    op.top = 0;
    op.width = 100;
    op.height = 100;
    er_node_set_props(outer, &op);
    er_event_set(outer, ER_EVENT_SCROLL, count_scroll, &outer_events);
    er_tree_append_child(root, outer);

    /* The inner scroller is box-none and overflows the outer, so the pan should pass it by and
     * scroll the outer one instead. */
    create_scroll_view(outer, ER_POINTER_EVENTS_BOX_NONE, 400, &inner_events);
    er_commit();

    drag_past_slop();

    if (inner_events != 0)
        return fail("box-none inner ScrollView took the pan");
    if (outer_events == 0)
        return fail("box-none inner ScrollView swallowed the pan instead of deferring outward");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks that starting a new scene stops the previous one's ScrollView dead.
 *
 * er_tree_set_root() only swaps the root tag, so without the pool reset in create_root_sized() a
 * ScrollView lifted mid-flick would keep coasting for the rest of the run — firing ER_EVENT_SCROLL
 * into whatever its user_data still points at, long after that scenario's stack frame has gone.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_new_scene_stops_orphan_scroller(void)
{
    ERNode* root = create_root();
    s_orphan_scroll_events = 0;
    create_scroll_view(root, ER_POINTER_EVENTS_AUTO, 100, &s_orphan_scroll_events);
    er_commit();

    /* Lift mid-flick, with no pause before the release, so the scroller keeps real momentum. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 80, 50);
    embedded_renderer_tick(16U);
    touch_move(70, 50);
    embedded_renderer_tick(16U);
    touch_move(60, 50);
    embedded_renderer_touch(0, ER_TOUCH_UP, 60, 50);

    embedded_renderer_tick(16U);
    if (s_orphan_scroll_events == 0)
        return fail("the flicked ScrollView never scrolled");

    create_root(); /* new scene: the flicked scroller is gone from the pool */
    er_commit();

    const int before = s_orphan_scroll_events;
    for (int i = 0; i < 30; i++)
        embedded_renderer_tick(16U);

    if (s_orphan_scroll_events != before)
        return fail("a ScrollView from the previous scene kept coasting into the next one");

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
 * @brief Checks an auto-height absolute container still hit-tests its children (issue #94).
 *
 * A `position:absolute` View given left/top/width but no height used to lay out 0 px tall. Its
 * children painted normally — nothing clips them — so the screen looked right, but hit_test_node
 * gates entry on the node's own rect, so the empty box turned the whole subtree into a dead zone and
 * every touch fell through to whatever sat behind it. The container now sizes to its content, so the
 * child inside it is hittable and the node behind it is not.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_auto_sized_absolute_is_hittable(void)
{
    ERNode* root = create_root();

    /* Full-bleed pressable behind everything: it must NOT collect the tap. */
    EventCounts behind = {0};
    ERNode* backdrop = create_pressable(0, 0, 240, 160, &behind);
    er_tree_append_child(root, backdrop);

    /* Absolute container with no height, holding a pressable child. */
    ERProps ap = props_default();
    ap.position = ER_POS_ABSOLUTE;
    ap.left = 20;
    ap.top = 30;
    ap.width = 100;
    LayoutRecord box_rect = {0};
    ERNode* box = er_node_create(ER_NODE_VIEW);
    er_node_set_props(box, &ap);
    er_event_set(box, ER_EVENT_LAYOUT, on_layout, &box_rect);

    EventCounts inner = {0};
    ERProps cp = props_default();
    cp.width = 80;
    cp.height = 40;
    cp.background_color = 0xFF202020U;
    ERNode* child = er_node_create(ER_NODE_PRESSABLE);
    er_node_set_props(child, &cp);
    er_event_set(child, ER_EVENT_PRESS, on_press, &inner);

    er_tree_append_child(box, child);
    er_tree_append_child(root, box);
    er_commit();

    if (box_rect.rect.w != 100 || box_rect.rect.h != 40)
        return fail("auto-height absolute container did not size to its content");

    tap(60, 50);

    if (inner.press_count != 1)
        return fail("child of an auto-height absolute container did not receive the press");
    if (behind.press_count != 0)
        return fail("press fell through an auto-height absolute container to the node behind it");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks a transformed node render_tree could not capture is hit where it is actually painted.
 *
 * A scale/rotate node is normally painted by capturing its subtree into the transform scratch and
 * inverse-mapping it back out, and hit-testing answers it by inverse-mapping the touch the same way.
 * When the capture cannot be started the node is painted UNTRANSFORMED at its raw layout box instead —
 * and hit-testing used to map the touch through the transform anyway, leaving a phantom hit region
 * that does not overlap a single drawn pixel (issue #141). At half scale about a top-left pivot that
 * region is the box's top-left quarter: everything past it looks like the node and answered as the
 * background.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_uncaptured_transform_hits_painted_box(void)
{
    ERNode* root = create_root_sized(XF_TOO_BIG_W + 80, XF_TOO_BIG_H + 80);
    EventCounts counts = {0};
    ERNode* box = create_pressable(40, 40, XF_TOO_BIG_W, XF_TOO_BIG_H, &counts);

    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 40;
    p.top = 40;
    p.width = XF_TOO_BIG_W;
    p.height = XF_TOO_BIG_H;
    p.background_color = 0xFF101010U;
    p.transform_scale_x = 0.5f;
    p.transform_scale_y = 0.5f;
    p.transform_origin_x = 0.0f; /* top-left pivot, so the phantom rect is the box's top-left quarter */
    p.transform_origin_y = 0.0f;
    er_node_set_props(box, &p);

    er_tree_append_child(root, box);
    er_commit();

    /* Three quarters across the painted box — outside the halved rect, on pixels that are plainly the
     * node. This is the tap the issue reported falling through to the background. */
    tap(40 + (XF_TOO_BIG_W * 3) / 4, 40 + (XF_TOO_BIG_H * 3) / 4);
    if (counts.press_count != 1)
        return fail("tap on the painted area of an uncaptured transformed node did not reach it");

    /* The near corner lies inside the paint AND the phantom rect, so it worked before the fix too. */
    tap(40 + XF_TOO_BIG_W / 4, 40 + XF_TOO_BIG_H / 4);
    if (counts.press_count != 2)
        return fail("tap near the pivot of an uncaptured transformed node did not reach it");

    /* Past the raw box there is nothing painted: following the pixels must not widen the node either. */
    tap(40 + XF_TOO_BIG_W + 20, 40 + XF_TOO_BIG_H + 20);
    if (counts.press_count != 2)
        return fail("tap beyond an uncaptured transformed node still reached it");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks an oversized TRANSLATE-only node still hit-tests at its offset.
 *
 * A translate has no capture and no fallback — render_tree just shifts the render position, whatever
 * the node's size — so the size that degrades a scale must not degrade it. The counterpart to
 * test_uncaptured_transform_hits_painted_box: same node, same too-big dimensions, offset preserved.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_oversized_translate_only_keeps_offset(void)
{
    ERNode* root = create_root_sized(XF_TOO_BIG_W + 80, XF_TOO_BIG_H + 80);
    EventCounts counts = {0};
    ERNode* box = create_pressable(40, 40, XF_TOO_BIG_W, XF_TOO_BIG_H, &counts);

    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 40;
    p.top = 40;
    p.width = XF_TOO_BIG_W;
    p.height = XF_TOO_BIG_H;
    p.background_color = 0xFF101010U;
    p.transform_translate_x = 30.0f;
    p.transform_translate_y = 20.0f;
    er_node_set_props(box, &p);

    er_tree_append_child(root, box);
    er_commit();

    /* Past the layout box's right edge, but inside the translated one the node is drawn at. */
    tap(40 + XF_TOO_BIG_W + 10, 40 + XF_TOO_BIG_H / 2);
    if (counts.press_count != 1)
        return fail("an oversized translate-only node lost its offset");

    /* Inside the layout box, left of everything the node actually covers. */
    tap(45, 45);
    if (counts.press_count != 1)
        return fail("an oversized translate-only node was hit at its untranslated box");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks a transformed node nested inside a CAPTURING one is hit where it is painted.
 *
 * The other way a capture fails, and the one no size test can predict: the outer node's capture is
 * already running, so the inner one — which fits the source perfectly well — paints untransformed
 * inside it. Only the flag the previous paint recorded says so, which is why hit-testing consults it
 * before it falls back to the size.
 *
 * The outer half-scale about a top-left pivot maps screen x to 2x-20, so the inner node's source-space
 * box (40..80) is drawn over screen 30..50, and a tap at 45 lands on it. Mapped through the inner
 * quarter-scale as well it would land at 160 — far outside the node, and outside the root.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_nested_uncaptured_transform_hits_painted_box(void)
{
    ERNode* root = create_root_sized(200, 200);
    EventCounts counts = {0};

    /* Outer: small enough to capture, so its transform genuinely reaches the screen. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 20;
    p.top = 20;
    p.width = 100;
    p.height = 100;
    p.background_color = 0xFF202020U;
    p.transform_scale_x = 0.5f;
    p.transform_scale_y = 0.5f;
    p.transform_origin_x = 0.0f;
    p.transform_origin_y = 0.0f;
    er_node_set_props(outer, &p);

    ERNode* inner = create_pressable(20, 20, 40, 40, &counts);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 20;
    p.top = 20;
    p.width = 40;
    p.height = 40;
    p.background_color = 0xFF303030U;
    p.transform_scale_x = 0.25f;
    p.transform_scale_y = 0.25f;
    p.transform_origin_x = 0.0f;
    p.transform_origin_y = 0.0f;
    er_node_set_props(inner, &p);

    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_commit();

    tap(45, 45);
    if (counts.press_count != 1)
        return fail("tap on a fallback-painted node inside a capturing transform did not reach it");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks a translated ActivityIndicator is hit where render_tree draws it.
 *
 * The spinner is the one node kept off the transform capture path — tp_rotate_z is an internal spin
 * angle, not an affine render — but it still honours a translate, which render_tree applies by shifting
 * the render position. Hit-testing has to make the same distinction: skip the capture, keep the offset.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_translated_activity_indicator_keeps_offset(void)
{
    ERNode* root = create_root_sized(200, 200);
    EventCounts counts = {0};

    ERNode* spinner = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 20;
    p.top = 20;
    p.width = 40;
    p.height = 40;
    p.color = 0xFF3366FFU;
    p.transform_translate_x = 60.0f;
    p.transform_translate_y = 60.0f;
    /* A spin angle alongside the offset: the node is not translate-only, so it takes the branch that
     * asks whether a capture happened — and must still come out at its offset, not its raw box. */
    p.transform_rotate_z = 45.0f;
    er_node_set_props(spinner, &p);
    er_event_set(spinner, ER_EVENT_PRESS, on_press, &counts);

    er_tree_append_child(root, spinner);
    er_commit();

    /* Where it is drawn: layout box 20,20 40x40 shifted by 60,60. */
    tap(90, 90);
    if (counts.press_count != 1)
        return fail("a translated activity indicator was not hit where it is drawn");

    /* Its raw layout box, which nothing is drawn at. */
    tap(30, 30);
    if (counts.press_count != 1)
        return fail("a translated activity indicator was hit at its untranslated box");

    return EXIT_SUCCESS;
}

#if ERUI_3D_TRANSFORMS
/**
 * @brief Checks a 3D-transformed node is hit through its homography, not its 2D matrix.
 *
 * render_tree paints a rotateX/rotateY/perspective node by back-projecting the inverse homography, and
 * node_map_point() maps touches the same way — but hit_test_node() used to carry its own copy of the
 * mapping that knew only the 2D affine matrix. For a pure rotateY that matrix is the identity, so the
 * entry gate was the raw layout box while the pixels sat in a projected trapezoid: taps on the drawn
 * node below the box were refused, and taps on empty background inside the box were accepted.
 *
 * Both edges are asserted. At rotateY 60 / perspective 300 a 100x100 box at 50,50 is drawn over roughly
 * 49,49 93x163 — narrower than the box on the far side, and half as tall again below it.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_3d_transform_hit_follows_projection(void)
{
    ERNode* root = create_root_sized(300, 300);
    EventCounts counts = {0};
    ERNode* box = create_pressable(50, 50, 100, 100, &counts);

    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = 50;
    p.top = 50;
    p.width = 100;
    p.height = 100;
    p.background_color = 0xFF101010U;
    p.transform_rotate_y = 60.0f;
    p.transform_perspective = 300.0f;
    er_node_set_props(box, &p);

    er_tree_append_child(root, box);
    er_commit();

    /* Dead centre: inside the box and inside the projection, so it hits either way. */
    tap(100, 100);
    if (counts.press_count != 1)
        return fail("tap on the middle of a 3D-transformed node did not reach it");

    /* Below the layout box (which ends at 150) but on drawn pixels: the projection reaches past y=180. */
    tap(100, 175);
    if (counts.press_count != 2)
        return fail("tap on the projected part of a 3D-transformed node outside its box missed it");

    /* Inside the layout box but past the far edge of the projection, which stops short of x=142. */
    tap(145, 100);
    if (counts.press_count != 2)
        return fail("tap inside a 3D-transformed node's box but off its projection still hit it");

    return EXIT_SUCCESS;
}
#endif /* ERUI_3D_TRANSFORMS */

/**
 * @brief Test entry point for hit-testing and press dispatch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
/**
 * @brief Builds a full-screen pressable whose raw touch events report into a TouchGestureRecord.
 *
 * @param[in]  root    Scene root to attach to.
 * @param[in]  counts  Scratch counter block the pressable's press handlers need.
 * @param[out] rec     Record the gesture handlers write into.
 *
 * @return The new node.
 */
static ERNode* create_gesture_target(ERNode* root, EventCounts* counts, TouchGestureRecord* rec)
{
    ERNode* node = create_pressable(0, 0, 240, 160, counts);
    er_event_set(node, ER_EVENT_TOUCH_MOVE, on_gesture_move, rec);
    er_event_set(node, ER_EVENT_TOUCH_END, on_gesture_end, rec);
    er_event_set(node, ER_EVENT_TOUCH_CANCEL, on_gesture_cancel, rec);
    er_tree_append_child(root, node);
    er_commit();
    return node;
}

/**
 * @brief Checks a raw touch-move carries travel from touch-down and the speed it was travelling at.
 *
 * This is what lets a plain onTouchMove/onTouchEnd recognise a flick without an app re-deriving any of
 * it — which an AOT-compiled app cannot do at all, its handler subset having no clock and no deltas.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_touch_move_carries_travel_and_velocity(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    TouchGestureRecord rec = {0};
    create_gesture_target(root, &counts, &rec);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 100);
    embedded_renderer_tick(16U);
    touch_move(140, 108); /* +40, +8 in 16 ms */

    if (rec.move_count != 1)
        return fail("expected one raw touch-move");
    if (rec.dx != 40 || rec.dy != 8)
        return fail("raw touch-move must carry displacement from touch-down");
    if (!near_vel(rec.vx, 40.0f / 16.0f) || !near_vel(rec.vy, 8.0f / 16.0f))
        return fail("raw touch-move must carry px/ms velocity");

    embedded_renderer_tick(16U);
    touch_move(160, 108); /* +20 more in 16 ms: velocity is the LAST leg, travel is cumulative */

    if (rec.dx != 60)
        return fail("displacement must accumulate from touch-down, not from the previous move");
    if (!near_vel(rec.vx, 20.0f / 16.0f) || !near_vel(rec.vy, 0.0f))
        return fail("velocity must be measured over the last move, not the whole gesture");

    embedded_renderer_touch(0, ER_TOUCH_UP, 160, 108);
    return EXIT_SUCCESS;
}

/**
 * @brief Checks touch-up reports the flick's velocity even when the finger rested before lifting.
 *
 * A fling ends with the finger still for a frame or two; re-measuring across that rest would report
 * zero and throw the throw away, so the release carries the last MOVE's reading (as RN does).
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_touch_end_keeps_flick_velocity(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    TouchGestureRecord rec = {0};
    create_gesture_target(root, &counts, &rec);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 100);
    embedded_renderer_tick(16U);
    touch_move(132, 100);        /* a fast flick: +32 in 16 ms */
    embedded_renderer_tick(16U); /* …then a frame of rest before the finger lifts */
    embedded_renderer_touch(0, ER_TOUCH_UP, 132, 100);

    if (rec.end_count != 1)
        return fail("expected one raw touch-end");
    if (rec.dx != 32 || rec.dy != 0)
        return fail("raw touch-end must carry the gesture's total displacement");
    if (!near_vel(rec.vx, 32.0f / 16.0f))
        return fail("touch-end must keep the last move's velocity across a rest before the lift");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks a cancelled sequence also reports its travel and speed.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_touch_cancel_carries_gesture(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    TouchGestureRecord rec = {0};
    create_gesture_target(root, &counts, &rec);

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 100, 100);
    embedded_renderer_tick(16U);
    touch_move(100, 76); /* -24 in 16 ms */
    embedded_renderer_touch(0, ER_TOUCH_CANCEL, 100, 76);

    if (rec.cancel_count != 1)
        return fail("expected one raw touch-cancel");
    if (rec.dx != 0 || rec.dy != -24)
        return fail("raw touch-cancel must carry the gesture's displacement");
    if (!near_vel(rec.vy, -24.0f / 16.0f))
        return fail("raw touch-cancel must carry the last move's velocity");

    return EXIT_SUCCESS;
}

/**
 * @brief Checks er_touch_active_count() tracks fingers down (RN's gestureState.numberActiveTouches).
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on failure.
 */
static int test_touch_active_count(void)
{
    ERNode* root = create_root();
    EventCounts counts = {0};
    ERNode* pressable = create_pressable(0, 0, 240, 160, &counts);
    er_tree_append_child(root, pressable);
    er_commit();

    if (er_touch_active_count() != 0)
        return fail("no fingers down must count zero");

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 30, 30);
    if (er_touch_active_count() != 1)
        return fail("one finger down must count one");

    embedded_renderer_touch(1, ER_TOUCH_DOWN, 60, 60);
    if (er_touch_active_count() != 2)
        return fail("two fingers down must count two");

    embedded_renderer_touch(0, ER_TOUCH_UP, 30, 30);
    if (er_touch_active_count() != 1)
        return fail("lifting one of two fingers must count one");

    embedded_renderer_touch(1, ER_TOUCH_UP, 60, 60);
    if (er_touch_active_count() != 0)
        return fail("lifting the last finger must count zero");

    return EXIT_SUCCESS;
}

int main(void)
{
    embedded_renderer_set_backend(NULL);

    if (test_press_order() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_out_and_back_in() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_coalescing_collapses_to_latest() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_coalescing_flushes_before_up() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_coalescing_drops_repeat_position() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_coalescing_is_per_finger() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_coalescing_flushed_by_commit() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_move_coalescing_can_be_disabled() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_cancel() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_long_press() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_long_press_delay() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_long_press_cancelled_by_exit() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_long_press_without_handler() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_raw_touch_bubbling() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_touch_move_carries_travel_and_velocity() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_touch_end_keeps_flick_velocity() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_touch_cancel_carries_gesture() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_touch_active_count() != EXIT_SUCCESS)
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
    if (test_auto_sized_absolute_is_hittable() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_none() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_box_none() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_box_none_inert_child() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_box_none_responder() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_box_none_passes_through() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_pointer_events_box_none_scroll_view() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_box_none_scroll_view_defers_to_outer() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_new_scene_stops_orphan_scroller() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_scroll_cancels_press() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_self_claimed_drag_cancels_press() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_start_capture_suppresses_press() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_press_target_unmounted_during_grant() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_touch_target_unmounted_by_touch_start() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_touch_start_bubbles_from_unmounted_target() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_arc_unmounted_by_touch_start_does_not_drag() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_arc_unmounted_by_grant_does_not_drag() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_press_target_replaced_mid_touch() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_claimant_replaced_by_own_query() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_replaced_during_move() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_scroll_responder_replaced_on_release() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_destroyed_mid_drag() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_arc_destroyed_mid_drag() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_ancestor_replaced_while_bubbling() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_ancestor_replaced_during_negotiation() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_scene_reset_during_touch() != EXIT_SUCCESS)
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
    if (test_responder_takeover_stops_scroll() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_capture_wins_over_bubble() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_termination_accepted() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_responder_termination_rejected() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_has_pending_touch_matches_dispatch() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_uncaptured_transform_hits_painted_box() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_oversized_translate_only_keeps_offset() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_nested_uncaptured_transform_hits_painted_box() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (test_translated_activity_indicator_keeps_offset() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#if ERUI_3D_TRANSFORMS
    if (test_3d_transform_hit_follows_projection() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#endif

    return EXIT_SUCCESS;
}
