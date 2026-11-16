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
 * Display rotation: at every angle,
 *   - the setting refuses bad input and reports the logical size swapped at 90 and 270,
 *   - er_rotate_copy() puts every logical pixel where the panel shows it, at 1 to 4 bytes per pixel,
 *     for the whole panel and for a clipped rect with odd edges that spans several strips,
 *   - a touch at a panel point presses the node drawn there,
 *   - er_get_dirty_rect() and er_get_dirty_rects() report the logical damage turned into panel space.
 */

#include "er_scene.h"
#include "native_renderer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define LOGICAL_WIDTH 80
#define LOGICAL_HEIGHT 48
#define STRIDE_PADDING_BYTES 5
#define MAX_BYTES_PER_PIXEL 4
#define UNTOUCHED_BYTE 0xEEU
#define OPACITY_OPAQUE 255U
#define ROTATION_COUNT 4

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static const ERDisplayRotation s_rotations[ROTATION_COUNT] = {
    ER_ROTATION_0, ER_ROTATION_90, ER_ROTATION_180, ER_ROTATION_270};

static uint8_t s_logical_pixels[LOGICAL_HEIGHT * (LOGICAL_WIDTH * MAX_BYTES_PER_PIXEL + STRIDE_PADDING_BYTES)];
static uint8_t s_panel_pixels[LOGICAL_WIDTH * (LOGICAL_WIDTH * MAX_BYTES_PER_PIXEL + STRIDE_PADDING_BYTES)];

static int s_press_count;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a failure and returns EXIT_FAILURE.
 *
 * @param[in] message   What went wrong.
 * @param[in] rotation  The rotation under test.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* message, ERDisplayRotation rotation)
{
    fprintf(stderr, "FAIL (%d): %s\n", (int)rotation, message);
    return EXIT_FAILURE;
}

/**
 * @brief Sets the rotation with the panel sized so the logical size is LOGICAL_WIDTH x LOGICAL_HEIGHT.
 *
 * @param[in] rotation  Rotation to set.
 */
static void set_rotation(ERDisplayRotation rotation)
{
    const bool swapped = rotation == ER_ROTATION_90 || rotation == ER_ROTATION_270;
    er_set_display_rotation(
        rotation, swapped ? LOGICAL_HEIGHT : LOGICAL_WIDTH, swapped ? LOGICAL_WIDTH : LOGICAL_HEIGHT);
}

/**
 * @brief Where a logical pixel lands on the panel, worked out independently of the engine.
 *
 * @param[in]  rotation   Rotation.
 * @param[in]  logical_x  Logical X.
 * @param[in]  logical_y  Logical Y.
 * @param[out] panel_x    Panel X.
 * @param[out] panel_y    Panel Y.
 */
static void logical_to_panel(ERDisplayRotation rotation, int logical_x, int logical_y, int* panel_x, int* panel_y)
{
    switch (rotation)
    {
        case ER_ROTATION_90: /* panel is LOGICAL_HEIGHT wide */
            *panel_x = LOGICAL_HEIGHT - 1 - logical_y;
            *panel_y = logical_x;
            break;
        case ER_ROTATION_180:
            *panel_x = LOGICAL_WIDTH - 1 - logical_x;
            *panel_y = LOGICAL_HEIGHT - 1 - logical_y;
            break;
        case ER_ROTATION_270: /* panel is LOGICAL_WIDTH tall */
            *panel_x = logical_y;
            *panel_y = LOGICAL_WIDTH - 1 - logical_x;
            break;
        default:
            *panel_x = logical_x;
            *panel_y = logical_y;
            break;
    }
}

/**
 * @brief The panel rect a logical rect covers, as the bounding box of its turned corners.
 *
 * @param[in] rotation  Rotation.
 * @param[in] logical   Logical rect (non-empty).
 *
 * @return The panel rect.
 */
static ERRect logical_rect_to_panel(ERDisplayRotation rotation, ERRect logical)
{
    int first_x, first_y, second_x, second_y;
    logical_to_panel(rotation, logical.x, logical.y, &first_x, &first_y);
    logical_to_panel(rotation, logical.x + logical.width - 1, logical.y + logical.height - 1, &second_x, &second_y);
    ERRect panel;
    panel.x = first_x < second_x ? first_x : second_x;
    panel.y = first_y < second_y ? first_y : second_y;
    panel.width = (first_x > second_x ? first_x : second_x) - panel.x + 1;
    panel.height = (first_y > second_y ? first_y : second_y) - panel.y + 1;
    return panel;
}

/**
 * @brief Reports whether two rects are the same.
 *
 * @return true when every field matches.
 */
static bool rects_equal(ERRect first, ERRect second)
{
    return first.x == second.x && first.y == second.y && first.width == second.width && first.height == second.height;
}

/**
 * @brief Checks the setter refuses bad input and the size is swapped at 90 and 270.
 *
 * @return EXIT_SUCCESS on pass.
 */
static int check_setting(void)
{
    if (er_set_display_rotation((ERDisplayRotation)45, 10, 10))
        return fail("an angle other than the four was accepted", (ERDisplayRotation)45);
    if (er_set_display_rotation(ER_ROTATION_90, 0, 10) || er_set_display_rotation(ER_ROTATION_90, 10, -1))
        return fail("a panel size below 1 was accepted", ER_ROTATION_90);
    if (!er_set_display_rotation(ER_ROTATION_270, 30, 50))
        return fail("a good setting was refused", ER_ROTATION_270);
    int width = 0;
    int height = 0;
    if (!er_get_display_size(&width, &height) || width != 50 || height != 30)
        return fail("the logical size is not the panel size swapped", ER_ROTATION_270);
    if (er_set_display_rotation(ER_ROTATION_0, 0, 0) || er_get_display_rotation() != ER_ROTATION_270)
        return fail("a refused setting changed the rotation", ER_ROTATION_270);
    er_reset();
    if (er_get_display_rotation() != ER_ROTATION_270)
        return fail("er_reset() dropped the rotation", ER_ROTATION_270);
    er_set_display_rotation(ER_ROTATION_180, 30, 50);
    if (!er_get_display_size(&width, &height) || width != 30 || height != 50)
        return fail("the logical size at 180 is not the panel size", ER_ROTATION_180);
    return EXIT_SUCCESS;
}

/**
 * @brief Copies a rect at one rotation and pixel size and checks every destination byte.
 *
 * @param[in] rotation         Rotation.
 * @param[in] bytes_per_pixel  Pixel size.
 * @param[in] panel_rect       Rect to copy (NULL = the whole panel).
 *
 * @return EXIT_SUCCESS on pass.
 */
static int check_copy(ERDisplayRotation rotation, int bytes_per_pixel, const ERRect* panel_rect)
{
    const bool swapped = rotation == ER_ROTATION_90 || rotation == ER_ROTATION_270;
    const int panel_width = swapped ? LOGICAL_HEIGHT : LOGICAL_WIDTH;
    const int panel_height = swapped ? LOGICAL_WIDTH : LOGICAL_HEIGHT;
    const int source_stride = LOGICAL_WIDTH * bytes_per_pixel + STRIDE_PADDING_BYTES;
    const int destination_stride = panel_width * bytes_per_pixel + STRIDE_PADDING_BYTES;

    for (int logical_y = 0; logical_y < LOGICAL_HEIGHT; logical_y++)
        for (int logical_x = 0; logical_x < LOGICAL_WIDTH; logical_x++)
            for (int byte = 0; byte < bytes_per_pixel; byte++)
                s_logical_pixels[logical_y * source_stride + logical_x * bytes_per_pixel + byte] =
                    (uint8_t)(logical_x * 7 + logical_y * 13 + byte * 101 + 1);
    memset(s_panel_pixels, UNTOUCHED_BYTE, sizeof(s_panel_pixels));

    set_rotation(rotation);
    er_rotate_copy(s_logical_pixels, source_stride, s_panel_pixels, destination_stride, bytes_per_pixel, panel_rect);

    /* The rect the copy should cover once clipped to the panel. */
    int left = 0, top = 0, right = panel_width, bottom = panel_height;
    if (panel_rect)
    {
        left = panel_rect->x > 0 ? panel_rect->x : 0;
        top = panel_rect->y > 0 ? panel_rect->y : 0;
        right = panel_rect->x + panel_rect->width < panel_width ? panel_rect->x + panel_rect->width : panel_width;
        bottom = panel_rect->y + panel_rect->height < panel_height ? panel_rect->y + panel_rect->height : panel_height;
    }

    for (int logical_y = 0; logical_y < LOGICAL_HEIGHT; logical_y++)
    {
        for (int logical_x = 0; logical_x < LOGICAL_WIDTH; logical_x++)
        {
            int panel_x, panel_y;
            logical_to_panel(rotation, logical_x, logical_y, &panel_x, &panel_y);
            const bool inside = panel_x >= left && panel_x < right && panel_y >= top && panel_y < bottom;
            const uint8_t* source = &s_logical_pixels[logical_y * source_stride + logical_x * bytes_per_pixel];
            const uint8_t* destination = &s_panel_pixels[panel_y * destination_stride + panel_x * bytes_per_pixel];
            for (int byte = 0; byte < bytes_per_pixel; byte++)
            {
                if (inside && destination[byte] != source[byte])
                    return fail("a copied pixel does not hold the logical pixel the panel shows there", rotation);
                if (!inside && destination[byte] != UNTOUCHED_BYTE)
                    return fail("the copy wrote outside its rect", rotation);
            }
        }
    }
    for (int panel_y = 0; panel_y < panel_height; panel_y++)
        for (int byte = panel_width * bytes_per_pixel; byte < destination_stride; byte++)
            if (s_panel_pixels[panel_y * destination_stride + byte] != UNTOUCHED_BYTE)
                return fail("the copy wrote into a row's padding", rotation);
    return EXIT_SUCCESS;
}

/**
 * @brief Runs the copy check at every rotation and pixel size, whole and clipped.
 *
 * @return EXIT_SUCCESS on pass.
 */
static int check_rotate_copy(void)
{
    /* Odd edges, more than one 16-pixel strip wide, and hanging off the top-left and bottom-right. */
    const ERRect odd = {3, 5, 21, 37};
    const ERRect overhanging = {-4, -3, 30, 200};
    for (int rotation_index = 0; rotation_index < ROTATION_COUNT; rotation_index++)
    {
        for (int bytes_per_pixel = 1; bytes_per_pixel <= MAX_BYTES_PER_PIXEL; bytes_per_pixel++)
        {
            const ERDisplayRotation rotation = s_rotations[rotation_index];
            if (check_copy(rotation, bytes_per_pixel, NULL) != EXIT_SUCCESS
                || check_copy(rotation, bytes_per_pixel, &odd) != EXIT_SUCCESS
                || check_copy(rotation, bytes_per_pixel, &overhanging) != EXIT_SUCCESS)
                return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

/**
 * @brief Counts presses.
 */
static void on_press(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_press_count++;
}

/**
 * @brief Props with every layout field unset.
 *
 * @return Default props.
 */
static ERProps props_default(void)
{
    ERProps props;
    er_props_default(&props);
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/**
 * @brief Makes an absolutely placed view.
 *
 * @param[in] parent  Parent node.
 * @param[in] rect    Logical rect.
 * @param[in] color   Background color.
 * @param[in] type    Node type.
 *
 * @return The node.
 */
static ERNode* add_box(ERNode* parent, ERRect rect, uint32_t color, ERNodeType type)
{
    ERNode* node = er_node_create(type);
    ERProps props = props_default();
    props.position = ER_POSITION_ABSOLUTE;
    props.left = (int16_t)rect.x;
    props.top = (int16_t)rect.y;
    props.width = (int16_t)rect.width;
    props.height = (int16_t)rect.height;
    props.background_color = color;
    er_node_set_props(node, &props);
    er_tree_append_child(parent, node);
    return node;
}

/**
 * @brief Builds an empty logical-size root.
 *
 * @return The root.
 */
static ERNode* build_root(void)
{
    er_reset();
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_default();
    props.width = LOGICAL_WIDTH;
    props.height = LOGICAL_HEIGHT;
    props.background_color = 0xFF000000U;
    er_node_set_props(root, &props);
    er_tree_set_root(root);
    return root;
}

/**
 * @brief Checks a touch at a panel point presses the node drawn there, and only that one.
 *
 * @return EXIT_SUCCESS on pass.
 */
static int check_touch(void)
{
    const ERRect button = {60, 10, 12, 12};
    for (int rotation_index = 0; rotation_index < ROTATION_COUNT; rotation_index++)
    {
        const ERDisplayRotation rotation = s_rotations[rotation_index];
        set_rotation(rotation);
        ERNode* root = build_root();
        ERNode* pressable = add_box(root, button, 0xFF2060C0U, ER_NODE_PRESSABLE);
        er_event_set(pressable, ER_EVENT_PRESS, on_press, NULL);
        er_commit();

        int panel_x, panel_y;
        s_press_count = 0;
        logical_to_panel(rotation, button.x + 3, button.y + 8, &panel_x, &panel_y);
        embedded_renderer_touch(0, ER_TOUCH_DOWN, panel_x, panel_y);
        embedded_renderer_touch(0, ER_TOUCH_UP, panel_x, panel_y);
        if (s_press_count != 1)
            return fail("a touch on the button's panel position did not press it", rotation);

        /* The opposite corner of the logical frame holds nothing. */
        s_press_count = 0;
        logical_to_panel(rotation, 4, LOGICAL_HEIGHT - 4, &panel_x, &panel_y);
        embedded_renderer_touch(0, ER_TOUCH_DOWN, panel_x, panel_y);
        embedded_renderer_touch(0, ER_TOUCH_UP, panel_x, panel_y);
        if (s_press_count != 0)
            return fail("a touch away from the button pressed it", rotation);
    }
    return EXIT_SUCCESS;
}

/**
 * @brief Recolors two boxes in opposite corners and returns the logical damage at rotation 0.
 *
 * @param[out] rects       Rects er_get_dirty_rects() reported.
 * @param[out] rect_count  How many.
 * @param[out] bounds      What er_get_dirty_rect() reported.
 */
static void damage_two_corners(ERRect* rects, int* rect_count, ERRect* bounds)
{
    const ERRect top_left = {2, 3, 9, 5};
    const ERRect bottom_right = {61, 37, 14, 7};
    ERNode* root = build_root();
    ERNode* first = add_box(root, top_left, 0xFF404040U, ER_NODE_VIEW);
    ERNode* second = add_box(root, bottom_right, 0xFF404040U, ER_NODE_VIEW);
    er_commit();

    ERProps props = props_default();
    props.position = ER_POSITION_ABSOLUTE;
    props.background_color = 0xFFC02020U;
    props.left = (int16_t)top_left.x;
    props.top = (int16_t)top_left.y;
    props.width = (int16_t)top_left.width;
    props.height = (int16_t)top_left.height;
    er_node_set_props(first, &props);
    props.left = (int16_t)bottom_right.x;
    props.top = (int16_t)bottom_right.y;
    props.width = (int16_t)bottom_right.width;
    props.height = (int16_t)bottom_right.height;
    er_node_set_props(second, &props);
    er_commit();

    *rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    er_get_dirty_rect(bounds);
}

/**
 * @brief Checks the dirty rects come back as the rotation-0 rects turned into panel space.
 *
 * @return EXIT_SUCCESS on pass.
 */
static int check_dirty_rects(void)
{
    ERRect logical_rects[ER_DAMAGE_RECTS_MAX];
    ERRect logical_bounds;
    ERRect logical_collapsed;
    int logical_count = 0;
    set_rotation(ER_ROTATION_0);
    damage_two_corners(logical_rects, &logical_count, &logical_bounds);
    if (logical_count != 2)
        return fail("the two corners did not damage two rects", ER_ROTATION_0);
    er_get_dirty_rects(&logical_collapsed, 1);

    for (int rotation_index = 1; rotation_index < ROTATION_COUNT; rotation_index++)
    {
        const ERDisplayRotation rotation = s_rotations[rotation_index];
        ERRect panel_rects[ER_DAMAGE_RECTS_MAX];
        ERRect panel_bounds;
        int panel_count = 0;
        set_rotation(rotation);
        damage_two_corners(panel_rects, &panel_count, &panel_bounds);
        if (panel_count != logical_count)
            return fail("the rotation changed how many rects were damaged", rotation);
        for (int rect_index = 0; rect_index < panel_count; rect_index++)
            if (!rects_equal(panel_rects[rect_index], logical_rect_to_panel(rotation, logical_rects[rect_index])))
                return fail("er_get_dirty_rects() is not the logical rect in panel space", rotation);
        if (!rects_equal(panel_bounds, logical_rect_to_panel(rotation, logical_bounds)))
            return fail("er_get_dirty_rect() is not the logical bounds in panel space", rotation);

        ERRect collapsed;
        if (er_get_dirty_rects(&collapsed, 1) != 1
            || !rects_equal(collapsed, logical_rect_to_panel(rotation, logical_collapsed)))
            return fail("the one-rect fallback is not the logical fallback in panel space", rotation);
    }
    return EXIT_SUCCESS;
}

/**
 * @brief Fill callback that draws nothing; the checks read the engine's reports, not pixels.
 */
static void fill_nothing(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    static const EmbeddedRenderBackend backend = {fill_nothing, NULL, NULL, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    if (check_setting() != EXIT_SUCCESS || check_rotate_copy() != EXIT_SUCCESS || check_touch() != EXIT_SUCCESS
        || check_dirty_rects() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    printf("display rotation: all checks passed\n");
    return EXIT_SUCCESS;
}
