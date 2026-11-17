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

/*
 * Page-flipped buffers brought up to date by a copy: EmbeddedRenderBackend.buffer_sync.
 *
 * A software backend draws into two or three buffers. One script of changes runs three ways:
 *   - one buffer, fully repainted at every step: the reference picture of each step,
 *   - page-flipped with buffer_sync: the buffer about to be shown must equal the reference at every step,
 *   - page-flipped without it: the same, with each change replayed into the other buffer as before.
 * Each page-flipped run is made twice: by a host that flips only after a commit that painted, and by one
 * that flips every frame, as a host drawing an overlay outside the commit does.
 * With buffer_sync a one-off change is rendered once. The commit after it copies what its buffer missed
 * and paints nothing, and the next change renders only its own damage.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define PANEL_WIDTH 96
#define PANEL_HEIGHT 64
#define BUFFER_SIDE 96 /* holds the picture at either orientation */
#define BUFFER_PIXELS (BUFFER_SIDE * BUFFER_SIDE)
#define BUFFERS_MAX 3
#define STALE_PIXEL 0xFFEE00EEU
#define OPACITY_OPAQUE 255U
#define CHANNEL_MAX 255U
#define EXTENT_FAR (1 << 29)
#define SYNC_RECTS_MAX 16

#define ROOT_COLOR 0xFF102030U
#define OPAQUE_BOX_COLOR 0xFF3366FFU
#define OPAQUE_BOX_COLOR_CHANGED 0xFFEE2222U
#define GLASS_BOX_COLOR 0x8022AA44U
#define GLASS_BOX_COLOR_CHANGED 0x80FFCC00U
#define CORNER_BOX_COLOR 0xFF777777U
#define CORNER_BOX_COLOR_CHANGED 0xFFFFFFFFU

/** @brief An opaque box, a translucent one over its corner, and a box far from both. */
static const ERRect s_opaque_box = {6, 6, 20, 20};
static const ERRect s_glass_box = {16, 16, 24, 20};
static const ERRect s_corner_box = {44, 46, 16, 12};
static const ERRect s_corner_box_moved = {40, 44, 16, 12};
static const ERRect s_corner_box_moved_again = {36, 42, 16, 12};

/** @brief The steps of the script, in order. */
typedef enum
{
    STEP_MOUNT = 0,
    STEP_RECOLOR_OPAQUE,
    STEP_IDLE,
    STEP_RECOLOR_CORNER,
    STEP_RECOLOR_GLASS,
    STEP_BURST_FIRST, /**< Not presented: the next step lands in the same buffer. */
    STEP_BURST_SECOND,
    STEP_MOVE_CORNER,
    STEP_FULL_REPAINT,
    STEP_AFTER_FULL_REPAINT,
    STEP_ROTATE,
    STEP_AFTER_ROTATE,
    STEP_AFTER_ROTATE_AGAIN,
    STEP_RESET,
    STEP_AFTER_RESET,
    STEP_AFTER_RESET_AGAIN,
    STEP_NEW_BUFFERS, /**< The host swaps in buffers holding junk: nothing to copy from. */
    STEP_AFTER_NEW_BUFFERS,
    STEP_REPEAT_FIRST, /**< The same box changes twice running: nothing to copy the second time. */
    STEP_REPEAT_SECOND,
    STEP_SLIDE_FIRST, /**< A box moves a little twice running: only what it left behind is copied. */
    STEP_SLIDE_SECOND,
    STEP_COUNT
} Step;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_buffers[BUFFERS_MAX][BUFFER_PIXELS];
static uint32_t s_reference[STEP_COUNT][BUFFER_PIXELS];
static int s_buffer_count;
static int s_back;      /**< The buffer being drawn. */
static int s_presented; /**< The buffer drawn before the last flip. */

static int s_logical_width;
static int s_logical_height;

static int s_operation_count;
static int s_paint_left, s_paint_top, s_paint_right, s_paint_bottom;
static int s_sync_calls;
static int s_sync_rect_count;
static ERRect s_sync_rects[SYNC_RECTS_MAX];

static ERNode* s_root;
static ERNode* s_opaque;
static ERNode* s_glass;
static ERNode* s_corner;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (backend)
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t div255(uint32_t value)
{
    value += 0x80U;
    return (value + (value >> 8)) >> 8;
}

/** @brief Source-over of a premultiplied pixel onto an opaque one. */
static uint32_t over(uint32_t destination, uint32_t alpha, uint32_t red, uint32_t green, uint32_t blue)
{
    const uint32_t inverse = CHANNEL_MAX - alpha;
    red += div255(((destination >> 16) & 0xFFU) * inverse);
    green += div255(((destination >> 8) & 0xFFU) * inverse);
    blue += div255((destination & 0xFFU) * inverse);
    return 0xFF000000U | (red << 16) | (green << 8) | blue;
}

/** @brief Clips an op to the picture and notes it. Returns false when nothing is left. */
static bool note_operation(int* x, int* y, int* width, int* height, int* source_x, int* source_y)
{
    *source_x = *source_y = 0;
    if (*x < 0)
    {
        *source_x = -*x;
        *width += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *source_y = -*y;
        *height += *y;
        *y = 0;
    }
    if (*x + *width > s_logical_width)
        *width = s_logical_width - *x;
    if (*y + *height > s_logical_height)
        *height = s_logical_height - *y;
    if (*width <= 0 || *height <= 0)
        return false;

    s_operation_count++;
    if (*x < s_paint_left)
        s_paint_left = *x;
    if (*y < s_paint_top)
        s_paint_top = *y;
    if (*x + *width > s_paint_right)
        s_paint_right = *x + *width;
    if (*y + *height > s_paint_bottom)
        s_paint_bottom = *y + *height;
    return true;
}

static void fill_rect(uint32_t argb, int x, int y, int width, int height, void* context)
{
    (void)context;
    int source_x, source_y;
    const uint32_t alpha = argb >> 24;
    if (alpha == 0U || !note_operation(&x, &y, &width, &height, &source_x, &source_y))
        return;
    const uint32_t red = div255(((argb >> 16) & 0xFFU) * alpha);
    const uint32_t green = div255(((argb >> 8) & 0xFFU) * alpha);
    const uint32_t blue = div255((argb & 0xFFU) * alpha);
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
        {
            uint32_t* pixel = &s_buffers[s_back][row * BUFFER_SIDE + column];
            *pixel = over(*pixel, alpha, red, green, blue);
        }
}

static void
blend_rect(const void* source, int stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* context)
{
    (void)context;
    int source_x, source_y;
    if (!source || alpha == 0U || !note_operation(&x, &y, &width, &height, &source_x, &source_y))
        return;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row =
            (const uint32_t*)(const void*)((const uint8_t*)source + (size_t)(source_y + row) * (size_t)stride_bytes)
            + source_x;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = source_row[column];
            uint32_t* pixel = &s_buffers[s_back][(y + row) * BUFFER_SIDE + x + column];
            *pixel = over(*pixel,
                          div255((source_pixel >> 24) * alpha),
                          div255(((source_pixel >> 16) & 0xFFU) * alpha),
                          div255(((source_pixel >> 8) & 0xFFU) * alpha),
                          div255((source_pixel & 0xFFU) * alpha));
        }
    }
}

static void copy_rect(const void* source, int stride_bytes, int x, int y, int width, int height, void* context)
{
    blend_rect(source, stride_bytes, (uint8_t)OPACITY_OPAQUE, x, y, width, height, context);
}

/** @brief Copies the rects from the buffer drawn before the last flip. Not counted as painting. */
static void buffer_sync(const ERRect* rects, int count, void* context)
{
    (void)context;
    s_sync_calls++;
    for (int rect_index = 0; rect_index < count; rect_index++)
    {
        const ERRect* rect = &rects[rect_index];
        if (s_sync_rect_count < SYNC_RECTS_MAX)
            s_sync_rects[s_sync_rect_count] = *rect;
        s_sync_rect_count++;
        for (int row = rect->y; row < rect->y + rect->height; row++)
            memcpy(&s_buffers[s_back][row * BUFFER_SIDE + rect->x],
                   &s_buffers[s_presented][row * BUFFER_SIDE + rect->x],
                   (size_t)rect->width * sizeof(uint32_t));
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (scene)
 ---------------------------------------------------------------------------------------------------------------------*/

static int fail(const char* message, int buffer_count, bool with_sync, int step)
{
    fprintf(
        stderr, "FAIL (%d buffers, %s, step %d): %s\n", buffer_count, with_sync ? "sync" : "no sync", step, message);
    return EXIT_FAILURE;
}

static ERProps props_default(void)
{
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

static void fill_buffers_with_junk(void)
{
    for (int buffer = 0; buffer < BUFFERS_MAX; buffer++)
        for (int pixel = 0; pixel < BUFFER_PIXELS; pixel++)
            s_buffers[buffer][pixel] = STALE_PIXEL;
}

static void set_box(ERNode* node, const ERRect* box, uint32_t color)
{
    ERProps props = props_default();
    props.position = ER_POSITION_ABSOLUTE;
    props.left = box->x;
    props.top = box->y;
    props.width = box->width;
    props.height = box->height;
    props.background_color = color;
    er_node_set_props(node, &props);
}

static void set_root_size(void)
{
    ERProps props = props_default();
    props.width = s_logical_width;
    props.height = s_logical_height;
    props.background_color = ROOT_COLOR;
    er_node_set_props(s_root, &props);
}

static void build_scene(void)
{
    s_root = er_node_create(ER_NODE_VIEW);
    s_opaque = er_node_create(ER_NODE_VIEW);
    s_glass = er_node_create(ER_NODE_VIEW);
    s_corner = er_node_create(ER_NODE_VIEW);
    set_root_size();
    set_box(s_opaque, &s_opaque_box, OPAQUE_BOX_COLOR);
    set_box(s_glass, &s_glass_box, GLASS_BOX_COLOR);
    set_box(s_corner, &s_corner_box, CORNER_BOX_COLOR);
    er_tree_append_child(s_root, s_opaque);
    er_tree_append_child(s_root, s_glass);
    er_tree_append_child(s_root, s_corner);
    er_tree_set_root(s_root);
}

/** @brief Makes one step's change to the scene. */
static void apply_step(Step step)
{
    switch (step)
    {
        case STEP_MOUNT:
            build_scene();
            break;
        case STEP_RECOLOR_OPAQUE:
        case STEP_BURST_FIRST:
        case STEP_AFTER_FULL_REPAINT:
        case STEP_AFTER_ROTATE:
        case STEP_AFTER_RESET:
        case STEP_AFTER_NEW_BUFFERS:
            set_box(s_opaque,
                    &s_opaque_box,
                    step == STEP_RECOLOR_OPAQUE || step == STEP_AFTER_FULL_REPAINT || step == STEP_AFTER_RESET
                        ? OPAQUE_BOX_COLOR_CHANGED
                        : OPAQUE_BOX_COLOR);
            break;
        case STEP_RECOLOR_CORNER:
        case STEP_BURST_SECOND:
        case STEP_AFTER_ROTATE_AGAIN:
        case STEP_AFTER_RESET_AGAIN:
            set_box(s_corner,
                    step == STEP_AFTER_ROTATE_AGAIN ? &s_corner_box_moved : &s_corner_box,
                    step == STEP_BURST_SECOND ? CORNER_BOX_COLOR : CORNER_BOX_COLOR_CHANGED);
            break;
        case STEP_RECOLOR_GLASS:
            set_box(s_glass, &s_glass_box, GLASS_BOX_COLOR_CHANGED);
            break;
        case STEP_MOVE_CORNER:
            set_box(s_corner, &s_corner_box_moved, CORNER_BOX_COLOR);
            break;
        case STEP_FULL_REPAINT:
            er_force_full_repaint();
            break;
        case STEP_ROTATE:
            er_set_display_rotation(ER_ROTATION_90, PANEL_WIDTH, PANEL_HEIGHT);
            s_logical_width = PANEL_HEIGHT;
            s_logical_height = PANEL_WIDTH;
            set_root_size();
            break;
        case STEP_RESET:
            er_reset();
            build_scene();
            break;
        case STEP_REPEAT_FIRST:
        case STEP_REPEAT_SECOND:
            set_box(s_opaque, &s_opaque_box, step == STEP_REPEAT_FIRST ? OPAQUE_BOX_COLOR_CHANGED : OPAQUE_BOX_COLOR);
            break;
        case STEP_SLIDE_FIRST:
        case STEP_SLIDE_SECOND:
            set_box(s_corner,
                    step == STEP_SLIDE_FIRST ? &s_corner_box_moved : &s_corner_box_moved_again,
                    CORNER_BOX_COLOR_CHANGED);
            break;
        case STEP_NEW_BUFFERS:
            fill_buffers_with_junk();
            s_back = 0;
            s_presented = 0;
            er_set_display_buffer_count(s_buffer_count);
            break;
        default:
            break;
    }
}

static bool rects_intersect(const ERRect* first, const ERRect* second)
{
    return first->x < second->x + second->width && second->x < first->x + first->width
           && first->y < second->y + second->height && second->y < first->y + first->height;
}

/** @brief Reports whether the buffer holds the reference picture of the step. */
static bool buffer_matches_reference(const uint32_t* buffer, Step step)
{
    for (int y = 0; y < s_logical_height; y++)
        for (int x = 0; x < s_logical_width; x++)
            if (buffer[y * BUFFER_SIDE + x] != s_reference[step][y * BUFFER_SIDE + x])
            {
                fprintf(stderr,
                        "(%d, %d): %08x, expected %08x\n",
                        x,
                        y,
                        (unsigned)buffer[y * BUFFER_SIDE + x],
                        (unsigned)s_reference[step][y * BUFFER_SIDE + x]);
                return false;
            }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (runs)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Starts a run on a clean engine with every buffer holding junk. */
static void begin_run(int buffer_count, bool with_sync)
{
    static EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fill_rect;
    backend.copy_rect = copy_rect;
    backend.blend_rect = blend_rect;
    backend.buffer_sync = with_sync ? buffer_sync : NULL;

    er_reset();
    er_set_display_rotation(ER_ROTATION_0, PANEL_WIDTH, PANEL_HEIGHT);
    s_logical_width = PANEL_WIDTH;
    s_logical_height = PANEL_HEIGHT;
    embedded_renderer_set_backend(&backend);
    er_set_display_buffer_count(buffer_count);

    fill_buffers_with_junk();
    s_buffer_count = buffer_count;
    s_back = 0;
    s_presented = 0;
}

/** @brief Commits, noting what was painted and copied. Returns true when something was painted. */
static bool commit(void)
{
    s_operation_count = 0;
    s_paint_left = s_paint_top = EXTENT_FAR;
    s_paint_right = s_paint_bottom = -EXTENT_FAR;
    s_sync_calls = 0;
    s_sync_rect_count = 0;
    er_commit();
    return s_operation_count > 0;
}

/** @brief Flips to the next buffer, as a host does after a commit that painted. */
static void present(void)
{
    s_presented = s_back;
    s_back = (s_back + 1) % s_buffer_count;
    er_display_present();
}

/** @brief One buffer, fully repainted at every step: records what each step should look like. */
static void run_reference(void)
{
    begin_run(1, false);
    for (int step = 0; step < STEP_COUNT; step++)
    {
        apply_step((Step)step);
        er_force_full_repaint();
        commit();
        memcpy(s_reference[step], s_buffers[0], sizeof(s_reference[step]));
    }
}

/**
 * @brief Runs the script page-flipped, checking the buffer just drawn at every step.
 *
 * @param[in] flip_every_frame  Flip after every commit, not only after one that painted.
 *
 * @return EXIT_SUCCESS on pass.
 */
static int run_page_flipped(int buffer_count, bool with_sync, bool flip_every_frame)
{
    begin_run(buffer_count, with_sync);
    for (int step = 0; step < STEP_COUNT; step++)
    {
        apply_step((Step)step);
        const bool painted = commit();
        const ERRect paint = {s_paint_left, s_paint_top, s_paint_right - s_paint_left, s_paint_bottom - s_paint_top};
        const bool painted_whole = painted && paint.width == s_logical_width && paint.height == s_logical_height;

        if (!with_sync && s_sync_calls != 0)
            return fail("a backend with no hook was asked to copy", buffer_count, with_sync, step);
        if (step == STEP_IDLE && with_sync && painted)
            return fail("a commit with no change painted", buffer_count, with_sync, step);
        if (step == STEP_IDLE && !with_sync && !painted)
            return fail("the change was not replayed into the other buffer", buffer_count, with_sync, step);
        if (step != STEP_IDLE && !painted)
            return fail("a change painted nothing", buffer_count, with_sync, step);
        /* Painted or not, this buffer is current: a host may show it. */
        if (!buffer_matches_reference(s_buffers[s_back], (Step)step))
            return fail("the buffer just drawn is not the reference picture", buffer_count, with_sync, step);

        if (with_sync && step == STEP_IDLE)
        {
            /* The buffer missed the opaque box: that is copied, with nothing rendered. */
            bool copied_opaque_box = false;
            for (int rect_index = 0; rect_index < s_sync_rect_count && rect_index < SYNC_RECTS_MAX; rect_index++)
                copied_opaque_box = copied_opaque_box || rects_intersect(&s_sync_rects[rect_index], &s_opaque_box);
            if (s_sync_calls != 1 || !copied_opaque_box)
                return fail("the missed change was not copied", buffer_count, with_sync, step);
            const bool copied_whole = s_sync_rects[0].width == s_logical_width;
            if (buffer_count == 2 && copied_whole)
                return fail("one missed box was copied as the whole screen", buffer_count, with_sync, step);
        }
        if (with_sync && step == STEP_RECOLOR_CORNER)
        {
            /* Only the corner box is rendered. A buffer that was not flipped away has nothing to copy. */
            if (rects_intersect(&paint, &s_opaque_box))
                return fail("the missed change was rendered again", buffer_count, with_sync, step);
            if (!flip_every_frame && s_sync_calls != 0)
                return fail("a buffer already copied into was copied into again", buffer_count, with_sync, step);
        }
        if (with_sync && step == STEP_BURST_SECOND && s_sync_calls != 0)
            return fail("a second commit into the same buffer copied again", buffer_count, with_sync, step);
        if (with_sync
            && (step == STEP_AFTER_FULL_REPAINT || step == STEP_AFTER_ROTATE || step == STEP_AFTER_RESET
                || step == STEP_AFTER_NEW_BUFFERS))
        {
            /* The buffer owes the whole screen: all of it but the box about to be repainted is copied. */
            int copied_pixels = 0;
            bool copied_changed_box = false;
            for (int rect_index = 0; rect_index < s_sync_rect_count && rect_index < SYNC_RECTS_MAX; rect_index++)
            {
                copied_pixels += s_sync_rects[rect_index].width * s_sync_rects[rect_index].height;
                copied_changed_box = copied_changed_box || rects_intersect(&s_sync_rects[rect_index], &s_opaque_box);
            }
            const int screen_pixels = s_logical_width * s_logical_height;
            const int box_pixels = s_opaque_box.width * s_opaque_box.height;
            if (s_sync_calls != 1 || copied_changed_box || copied_pixels < screen_pixels - 2 * box_pixels)
                return fail("a whole-screen debt was not copied around the change", buffer_count, with_sync, step);
            if (painted_whole)
                return fail("the whole screen was rendered after a whole-screen copy", buffer_count, with_sync, step);
        }
        if ((step == STEP_FULL_REPAINT || step == STEP_ROTATE || step == STEP_RESET || step == STEP_NEW_BUFFERS)
            && (!painted_whole || s_sync_calls != 0))
            return fail("a full repaint was not rendered whole", buffer_count, with_sync, step);

        if (with_sync && step == STEP_REPEAT_SECOND && s_sync_rect_count != 0)
            return fail("a box about to be repainted was copied first", buffer_count, with_sync, step);
        if (with_sync && step == STEP_SLIDE_SECOND && buffer_count == 2)
        {
            /* The buffer missed the first move. This move repaints most of that, so little is copied. */
            int copied_pixels = 0;
            for (int rect_index = 0; rect_index < s_sync_rect_count && rect_index < SYNC_RECTS_MAX; rect_index++)
                copied_pixels += s_sync_rects[rect_index].width * s_sync_rects[rect_index].height;
            if (copied_pixels == 0 || copied_pixels >= s_corner_box.width * s_corner_box.height)
                return fail("a moving box was not copied as what it left behind", buffer_count, with_sync, step);
        }

        /* The first commit of the burst is not shown. */
        if ((painted || flip_every_frame) && step != STEP_BURST_FIRST)
            present();
    }
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    run_reference();
    for (int buffer_count = 2; buffer_count <= BUFFERS_MAX; buffer_count++)
    {
        for (int flip_every_frame = 0; flip_every_frame <= 1; flip_every_frame++)
            if (run_page_flipped(buffer_count, true, flip_every_frame != 0) != EXIT_SUCCESS
                || run_page_flipped(buffer_count, false, flip_every_frame != 0) != EXIT_SUCCESS)
                return EXIT_FAILURE;
    }
    printf("PASS: page-flipped buffers are brought up to date by a copy\n");
    return EXIT_SUCCESS;
}
