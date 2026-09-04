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
