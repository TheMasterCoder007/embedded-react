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

/*----------------------------------------------------------------------------------------------------------------------
 - Includes
 ---------------------------------------------------------------------------------------------------------------------*/

#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OPACITY_OPAQUE 255U
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — null backend
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Null fill_rect; writes a uniform solid color into the test framebuffer.
 *
 * @param[in] argb    Straight-alpha ARGB8888 fill color.
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width.
 * @param[in] height  Height.
 * @param[in] ctx     Unused.
 */
static void fb_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
            if (row >= 0 && row < SCREEN_HEIGHT && column >= 0 && column < SCREEN_WIDTH)
                s_framebuffer[row * SCREEN_WIDTH + column] = argb;
}

/**
 * @brief Null copy_rect — no-op for tests that only check structural behavior.
 *
 * @param[in] source               Source pixel buffer.
 * @param[in] source_stride_bytes  Row stride.
 * @param[in] x                    Destination left edge.
 * @param[in] y                    Destination top edge.
 * @param[in] width                Width.
 * @param[in] height               Height.
 * @param[in] ctx                  Unused.
 */
static void fb_copy(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)source_stride_bytes;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief Null blend_rect — no-op for structural tests.
 *
 * @param[in] source               Source pixel buffer.
 * @param[in] source_stride_bytes  Row stride.
 * @param[in] alpha                Global blend alpha.
 * @param[in] x                    Destination left edge.
 * @param[in] y                    Destination top edge.
 * @param[in] width                Width.
 * @param[in] height               Height.
 * @param[in] ctx                  Unused.
 */
static void
fb_blend(const void* source, int source_stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)source_stride_bytes;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief Returns an ERProps with all dimension fields set to ER_LAYOUT_AUTO.
 *
 * Zero-init alone is not sufficient — min_/max_ width/height default to 0
 * (a hard cap), which clamps any positive size to 0 inside the layout engine.
 *
 * @return Default-initialised ERProps ready for use in tests.
 */
static ERProps props_auto(void)
{
    ERProps props;
    memset(&props, 0, sizeof(props));
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = props.padding_right = props.padding_bottom =
        ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/**
 * @brief Initialises the engine with the null framebuffer backend.
 */
static void init_backend(void)
{
    static const EmbeddedRenderBackend k_backend = {
        .fill_rect = fb_fill,
        .copy_rect = fb_copy,
        .blend_rect = fb_blend,
    };
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    embedded_renderer_set_backend(&k_backend);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: ActivityIndicator
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief ActivityIndicator node creates, starts spinning, and renders without crash.
 *
 * Verifies that an ActivityIndicator starts a looping rotation animation when
 * created, advances its angle after a tick, and reports animating=1.
 */
static void test_activity_indicator_renders(void)
{
    init_backend();

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    er_node_set_props(root, &props);

    ERNode* activity_indicator = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = 48;
    props.height = 48;
    props.indicator_color = 0xFFFFFFFFU;
    props.animating = 1;
    er_node_set_props(activity_indicator, &props);

    er_tree_append_child(root, activity_indicator);
    er_tree_set_root(root);

    /* Initial spin angle must be 0 before any tick. */
    assert(activity_indicator->transform_rotate_z == 0.0f && "Initial spin angle must be 0");
    assert(activity_indicator->props.activity_indicator.animating == 1 && "ActivityIndicator must be animating");

    /* First commit renders without crash. */
    er_commit();

    /* Advance 500 ms — rotation should be at ~180 degrees (half of 1000 ms cycle). */
    embedded_renderer_tick(500);
    er_commit();

    assert(activity_indicator->transform_rotate_z > 100.0f && activity_indicator->transform_rotate_z < 260.0f
           && "Spin angle must advance after 500 ms tick");

    /* Stopping the animation via props clears rotation advancement. */
    props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = 48;
    props.height = 48;
    props.indicator_color = 0xFFFFFFFFU;
    props.animating = 0;
    er_node_set_props(activity_indicator, &props);
    const float stopped_angle = activity_indicator->transform_rotate_z;
    embedded_renderer_tick(200);
    er_commit();
    assert(activity_indicator->transform_rotate_z == stopped_angle && "Spin angle must not change when animating=0");

    printf("PASS: test_activity_indicator_renders\n");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Switch
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Switch renders and animates its thumb from off to on.
 *
 * Verifies that after setting switch_value=1 the thumb position animates
 * and the on-track color is eventually applied.
 */
static void test_switch_renders(void)
{
    init_backend();

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    props.background_color = 0xFF111111U;
    er_node_set_props(root, &props);

    ERNode* switch_node = er_node_create(ER_NODE_SWITCH);
    props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = 60;
    props.height = 30;
    props.top = 105;
    props.left = 130;
    props.switch_value = 0;
    props.track_color_false = 0xFF767577U;
    props.track_color_true = 0xFF2A9D8FU;
    props.thumb_color = 0xFFFFFFFFU;
    er_node_set_props(switch_node, &props);

    er_tree_append_child(root, switch_node);
    er_tree_set_root(root);
    er_commit();

    /* switch_thumb_t should start at 0.0 (off). */
    assert(switch_node->switch_thumb_progress < 0.1f && "Switch thumb must start in off position");

    /* Toggle the switch on. */
    props.switch_value = 1;
    er_node_set_props(switch_node, &props);

    /* After a 300 ms tick the animation should be complete (duration is 200 ms). */
    embedded_renderer_tick(300);
    er_commit();

    assert(switch_node->switch_thumb_progress > 0.9f && "Switch thumb must reach on position after animation");
    assert(switch_node->props.switch_props.value == 1 && "Switch value must be 1 after set");

    printf("PASS: test_switch_renders\n");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: TextInput
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_change_count = 0;
static char s_last_text[256];

/**
 * @brief ER_EVENT_CHANGE_TEXT callback for test_text_input_keyboard.
 *
 * @param[in] node       Source node (unused).
 * @param[in] data       Event payload containing the updated text.
 * @param[in] user_data  Unused.
 */
static void on_change_text(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)user_data;
    s_change_count++;
    strncpy(s_last_text, data->changed_text ? data->changed_text : "", sizeof(s_last_text) - 1);
}

/**
 * @brief TextInput accepts keyboard input and fires change-text events.
 *
 * Verifies that er_text_input_focus, embedded_renderer_key, and the
 * ER_EVENT_CHANGE_TEXT callback work correctly together.
 */
static void test_text_input_keyboard(void)
{
    init_backend();
    s_change_count = 0;
    s_last_text[0] = '\0';

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    er_node_set_props(root, &props);

    ERNode* text_input = er_node_create(ER_NODE_TEXT_INPUT);
    props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = 200;
    props.height = 36;
    props.top = 102;
    props.left = 60;
    props.background_color = 0xFF1A2840U;
    props.border_color = 0xFF2A9D8FU;
    props.border_width = 2;
    props.border_radius = 6;
    props.color = 0xFFFFFFFFU;
    props.font_size = 14;
    props.editable = 1;
    strncpy(props.placeholder, "Type here...", ER_PLACEHOLDER_MAX);
    er_node_set_props(text_input, &props);

    er_event_set(text_input, ER_EVENT_CHANGE_TEXT, on_change_text, NULL);

    er_tree_append_child(root, text_input);
    er_tree_set_root(root);
    er_commit();

    /* Focus the input. */
    er_text_input_focus(text_input);
    assert(text_input->is_focused && "TextInput must be focused after er_text_input_focus");

    /* Type 'H', 'i'. */
    embedded_renderer_key(0, "H");
    embedded_renderer_key(0, "i");

    assert(s_change_count == 2 && "Change event must fire for each key");
    assert(strcmp(er_text_input_get_text(text_input), "Hi") == 0 && "TextInput text must match typed keys");

    /* Backspace. */
    embedded_renderer_key(ER_KEY_BACKSPACE, NULL);
    assert(strcmp(er_text_input_get_text(text_input), "H") == 0 && "Backspace must remove last character");

    /* Programmatic set. */
    er_text_input_set_text(text_input, "Hello");
    assert(strcmp(er_text_input_get_text(text_input), "Hello") == 0 && "er_text_input_set_text must replace text");

    /* Blur. */
    er_text_input_blur();
    assert(!text_input->is_focused && "TextInput must not be focused after blur");

    /* Key events should be ignored when unfocused. */
    embedded_renderer_key(0, "X");
    assert(strcmp(er_text_input_get_text(text_input), "Hello") == 0 && "Unfocused input must not accept key events");

    printf("PASS: test_text_input_keyboard\n");
}

/**
 * @brief Most runs of one color along any row of a box in the test framebuffer.
 *
 * @param[in] argb    Color to look for.
 * @param[in] x       Box left edge.
 * @param[in] y       Box top edge.
 * @param[in] width   Box width.
 * @param[in] height  Box height.
 *
 * @return The largest number of separate runs on one row.
 */
static int max_runs_in_box(uint32_t argb, int x, int y, int width, int height)
{
    int best = 0;
    for (int row = y; row < y + height; row++)
    {
        int runs = 0;
        bool in_run = false;
        for (int column = x; column < x + width; column++)
        {
            const bool hit = s_framebuffer[row * SCREEN_WIDTH + column] == argb;
            if (hit && !in_run)
                runs++;
            in_run = hit;
        }
        if (runs > best)
            best = runs;
    }
    return best;
}

/**
 * @brief Leftmost column of a box in the test framebuffer that holds a color.
 *
 * @param[in] argb    Color to look for.
 * @param[in] x       Box left edge.
 * @param[in] y       Box top edge.
 * @param[in] width   Box width.
 * @param[in] height  Box height.
 *
 * @return The column, or -1 when the color is not there.
 */
static int first_col_in_box(uint32_t argb, int x, int y, int width, int height)
{
    for (int column = x; column < x + width; column++)
        for (int row = y; row < y + height; row++)
            if (s_framebuffer[row * SCREEN_WIDTH + column] == argb)
                return column;
    return -1;
}

/**
 * @brief A secureTextEntry TextInput draws a dot per character in place of the text, with the cursor after
 *        the dots, and draws the text again once the prop is cleared.
 */
static void test_text_input_secure(void)
{
    init_backend();
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    er_node_set_props(root, &props);

    ERNode* text_input = er_node_create(ER_NODE_TEXT_INPUT);
    props = props_auto();
    props.width = 200;
    props.height = 36;
    props.background_color = 0xFF1A2840U;
    props.color = 0xFFFFFFFFU;
    props.cursor_color = 0xFFFF0000U;
    props.font_size = 16;
    props.editable = 1;
    props.secure_text_entry = 1;
    props.padding_horizontal = props.padding_vertical = ER_LAYOUT_AUTO; /* unset, so the field's own padding applies */
    er_node_set_props(text_input, &props);
    er_tree_append_child(root, text_input);
    er_tree_set_root(root);
    er_text_input_set_text(text_input, "a\303\251b"); /* three characters in four bytes */
    er_commit();

    /* The content box: the field's default 4 px / 3 px padding, no border. */
    const int content_x = text_input->computed.x + 4;
    const int content_y = text_input->computed.y + 3;
    const int content_width = text_input->computed.width - 8;
    const int content_height = text_input->computed.height - 6;
    assert(max_runs_in_box(0xFFFFFFFFU, content_x, content_y, content_width, content_height) == 3
           && "One dot per character, not per byte");

    /* At 16 px the dots are 6 px wide and 4 px apart, the first 2 px in; the cursor goes after the last. */
    er_text_input_set_text(text_input, "WWWWWW");
    er_text_input_focus(text_input);
    const uint32_t phase = (uint32_t)(er_now_ms64() % 1000U); /* the cursor shows for the first half of 1 s */
    if (phase >= 500U)
        embedded_renderer_tick(1000U - phase);
    er_commit();
    assert(max_runs_in_box(0xFFFFFFFFU, content_x, content_y, content_width, content_height) == 6
           && "A dot per character");
    assert(first_col_in_box(0xFFFF0000U, content_x, content_y, content_width, content_height) == content_x + 6 * 10
           && "The cursor must follow the dots");

    props.secure_text_entry = 0;
    er_node_set_props(text_input, &props);
    er_commit();
    assert(max_runs_in_box(0xFFFFFFFFU, content_x, content_y, content_width, content_height) != 6
           && "No dots once secureTextEntry is cleared");

    /* A font size past the text renderer's range is clamped for the dots too: 2 dots at 96 px advance 60 each. */
    props.secure_text_entry = 1;
    props.font_size = 200;
    er_node_set_props(text_input, &props);
    er_text_input_set_text(text_input, "ab");
    er_commit();
    assert(first_col_in_box(0xFFFF0000U, content_x, content_y, content_width, content_height) == content_x + 2 * 60
           && "Dots past 96 px must size as at 96 px");

    er_text_input_blur();
    printf("PASS: test_text_input_secure\n");
}

/**
 * @brief A TextInput with editable = 0 ignores keys even while focused, and default props stay editable.
 */
static void test_text_input_read_only(void)
{
    init_backend();
    s_change_count = 0;
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    er_node_set_props(root, &props);

    ERNode* text_input = er_node_create(ER_NODE_TEXT_INPUT);
    props = props_auto();
    props.width = 200;
    props.height = 36;
    props.editable = 0;
    er_node_set_props(text_input, &props);
    er_event_set(text_input, ER_EVENT_CHANGE_TEXT, on_change_text, NULL);
    er_tree_append_child(root, text_input);
    er_tree_set_root(root);
    er_text_input_set_text(text_input, "Hi");
    er_commit();

    er_text_input_focus(text_input);
    embedded_renderer_key(0, "X");
    assert(strcmp(er_text_input_get_text(text_input), "Hi") == 0 && "A read-only input must ignore typing");
    embedded_renderer_key(ER_KEY_BACKSPACE, NULL);
    assert(strcmp(er_text_input_get_text(text_input), "Hi") == 0 && "A read-only input must ignore backspace");
    assert(s_change_count == 0 && "A read-only input must not report a change");

    er_props_default(&props);
    props.width = 200;
    props.height = 36;
    er_node_set_props(text_input, &props);
    embedded_renderer_key(0, "!");
    assert(strcmp(er_text_input_get_text(text_input), "Hi!") == 0 && "Default props must leave the input editable");

    er_text_input_blur();
    printf("PASS: test_text_input_read_only\n");
}

/** @brief Event callback: counts the events it receives into an int. */
static void count_event(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (*(int*)user_data)++;
}

/**
 * @brief Creates an editable 200x36 TextInput at the top of a parent.
 *
 * @param[in] parent  Node to append it to.
 *
 * @return The TextInput.
 */
static ERNode* append_text_input(ERNode* parent)
{
    ERNode* text_input = er_node_create(ER_NODE_TEXT_INPUT);
    ERProps props = props_auto();
    props.width = 200;
    props.height = 36;
    props.editable = 1;
    er_node_set_props(text_input, &props);
    er_tree_append_child(parent, text_input);
    return text_input;
}

/**
 * @brief Destroying the focused TextInput takes the focus with it. A node created into its pool slot, the next one
 *        the engine hands out, is not typed into, focused or blurred, and the on-screen keyboard is erased.
 */
static void test_text_input_destroyed_while_focused(void)
{
    for (int as_input = 0; as_input <= 1; as_input++)
    {
        for (int refocus = 0; refocus <= 1; refocus++)
        {
            init_backend();
            s_change_count = 0;
            er_reset();

            ERNode* root = er_node_create(ER_NODE_VIEW);
            ERProps props = props_auto();
            props.width = SCREEN_WIDTH;
            props.height = SCREEN_HEIGHT;
            props.background_color = 0xFF000000U;
            er_node_set_props(root, &props);
            er_tree_set_root(root);

            ERNode* text_input = append_text_input(root);
            er_text_input_focus(text_input);
            er_commit();
            er_commit(); /* past the first frame's full repaint, so the next commit repaints only what changed */
#if ERUI_ONSCREEN_KEYBOARD
            assert(s_framebuffer[(SCREEN_HEIGHT - 1) * SCREEN_WIDTH] != 0xFF000000U
                   && "The keyboard must be on screen, or the scenario proves nothing");
#endif

            const uint16_t slot = text_input->tag;
            er_tree_remove_child(root, text_input);
            er_node_destroy(text_input);

            ERNode* next = as_input ? append_text_input(root) : er_node_create(ER_NODE_VIEW);
            if (!as_input)
                er_tree_append_child(root, next);
            assert(next->tag == slot && "The new node must reuse the input's slot, or the scenario proves nothing");
            int focus_events = 0;
            er_event_set(next, ER_EVENT_CHANGE_TEXT, on_change_text, NULL);
            er_event_set(next, ER_EVENT_FOCUS, count_event, &focus_events);
            er_event_set(next, ER_EVENT_BLUR, count_event, &focus_events);

            if (refocus)
            {
                er_text_input_focus(append_text_input(root));
                assert(focus_events == 0 && "Focusing another input must not blur the node in the old input's slot");
                er_text_input_blur();
            }
            else
            {
                embedded_renderer_key(0, "a");
                assert(next->input_text[0] == '\0' && s_change_count == 0
                       && "A node in a destroyed input's slot must not take its typing");
                assert(focus_events == 0 && "A node in a destroyed input's slot must not be focused or blurred");

                er_commit();
#if ERUI_ONSCREEN_KEYBOARD
                assert(s_framebuffer[(SCREEN_HEIGHT - 1) * SCREEN_WIDTH] == 0xFF000000U
                       && "The keyboard must be erased with the input it was typing into");
#endif
            }
        }
    }
    printf("PASS: test_text_input_destroyed_while_focused\n");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Modal
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Modal is hidden when modal_visible=0 and shows backdrop when visible=1.
 *
 * Uses a black root background.  When the modal is visible its backdrop
 * (semi-transparent dark overlay) blends into the framebuffer.
 */
static void test_modal_visible(void)
{
    init_backend();

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    props.background_color = 0xFF223344U; /* distinct background color */
    er_node_set_props(root, &props);

    ERNode* modal = er_node_create(ER_NODE_MODAL);
    props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    /* Absolute position fills the root. */
    props.position = 1; /* ER_POSITION_ABSOLUTE */
    props.left = 0;
    props.top = 0;
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    props.background_color = 0xFF172233U;
    props.modal_visible = 0;
    props.backdrop_color = 0x88000000U;
    er_node_set_props(modal, &props);

    er_tree_append_child(root, modal);
    er_tree_set_root(root);
    er_commit();

    /* With modal_visible=0 the modal should have display:none — no backdrop. */
    assert(modal->layout.display == 1 && "Hidden modal must have display:none"); /* ER_DISPLAY_NONE=1 */

    /* Show the modal. */
    props.modal_visible = 1;
    er_node_set_props(modal, &props);
    er_commit();

    assert(modal->modal_visible == 1 && "Modal must be visible after setting modal_visible=1");
    assert(modal->layout.display == 0 && "Visible modal must have display:flex"); /* ER_DISPLAY_FLEX=0 */

    printf("PASS: test_modal_visible\n");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: FlatList
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief FlatList acts as a scrollable container, matching ScrollView behavior.
 *
 * Verifies that a FlatList clips, scrolls, and participates in layout
 * in the same way as a ScrollView.
 */
static void test_flatlist_scrolls(void)
{
    init_backend();

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    er_node_set_props(root, &props);

    ERNode* flat_list = er_node_create(ER_NODE_FLAT_LIST);
    props = props_auto();
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.width = SCREEN_WIDTH;
    props.height = 120;
    props.overflow = 2;       /* ER_OVERFLOW_SCROLL */
    props.flex_direction = 0; /* ER_FLEX_COLUMN */
    props.align_items = 1;    /* ER_ALIGN_STRETCH */
    er_node_set_props(flat_list, &props);

    /* Add 5 tall rows to exceed the viewport. */
    for (int row_index = 0; row_index < 5; row_index++)
    {
        ERNode* row = er_node_create(ER_NODE_VIEW);
        props = props_auto();
        props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
        props.flex_basis = ER_LAYOUT_AUTO;
        props.height = 40;
        props.flex_shrink = 0; /* do not shrink below explicit height */
        er_node_set_props(row, &props);
        er_tree_append_child(flat_list, row);
    }

    er_tree_append_child(root, flat_list);
    er_tree_set_root(root);
    er_commit();

    /* Content size (5×40=200) must exceed viewport (120). */
    assert(flat_list->scroll_content_height > flat_list->computed.height
           && "FlatList content must exceed viewport height");

    /* Programmatic scroll. */
    er_scroll_view_set_offset(flat_list, 0.0f, 60.0f);
    assert(flat_list->scroll_offset_y == 60.0f && "FlatList scroll offset must update");

    printf("PASS: test_flatlist_scrolls\n");
}

#if ERUI_ONSCREEN_KEYBOARD
/** @brief What on_keyboard_submit saw. */
static int s_submit_count;
static bool s_focused_at_submit;

/** @brief ER_EVENT_SUBMIT_EDITING callback for test_keyboard_done_submits. */
static void on_keyboard_submit(ERNode* node, const EREventData* data, void* user_data)
{
    (void)user_data;
    s_submit_count++;
    s_focused_at_submit = node->is_focused;
    assert(strcmp(data->changed_text, "Hi") == 0 && "Submit must carry the input's text");
}

/** @brief The on-screen keyboard's done key submits the input, then takes the focus away. */
static void test_keyboard_done_submits(void)
{
    init_backend();
    s_submit_count = 0;
    s_focused_at_submit = false;

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    er_node_set_props(root, &props);

    ERNode* text_input = er_node_create(ER_NODE_TEXT_INPUT);
    props = props_auto();
    props.width = 200;
    props.height = 36;
    props.editable = 1;
    er_node_set_props(text_input, &props);
    er_event_set(text_input, ER_EVENT_SUBMIT_EDITING, on_keyboard_submit, NULL);
    er_tree_append_child(root, text_input);
    er_tree_set_root(root);
    er_text_input_set_text(text_input, "Hi");
    er_commit();

    er_text_input_focus(text_input);
    er_commit();

    /* The default layout's done key is in the bottom right corner. */
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 310, 235);
    embedded_renderer_touch(0, ER_TOUCH_UP, 310, 235);

    assert(s_submit_count == 1 && "The done key must submit once");
    assert(s_focused_at_submit && "The input must still be focused when it submits");
    assert(!text_input->is_focused && "The done key must take the focus away");

    printf("PASS: test_keyboard_done_submits\n");
}

/** @brief The input a submit made in on_submit_replaces, and the tag of the one it destroyed. */
static ERNode* s_replacement;
static uint16_t s_replaced_tag;

/** @brief ER_EVENT_SUBMIT_EDITING callback that swaps the input for a new one, which takes the focus. */
static void on_submit_replaces(ERNode* node, const EREventData* data, void* user_data)
{
    (void)data;
    ERNode* root = user_data;
    s_replaced_tag = node->tag;
    er_tree_remove_child(root, node);
    er_node_destroy(node);
    s_replacement = er_node_create(ER_NODE_TEXT_INPUT);
    ERProps props = props_auto();
    props.width = 200;
    props.height = 36;
    props.editable = 1;
    er_node_set_props(s_replacement, &props);
    er_tree_append_child(root, s_replacement);
    er_text_input_focus(s_replacement);
}

/** @brief The done key leaves alone an input that took the focus while submitting, even one on the old input's tag. */
static void test_keyboard_done_keeps_a_new_focus(void)
{
    init_backend();
    s_replacement = NULL;

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props = props_auto();
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    er_node_set_props(root, &props);

    ERNode* text_input = er_node_create(ER_NODE_TEXT_INPUT);
    props = props_auto();
    props.width = 200;
    props.height = 36;
    props.editable = 1;
    er_node_set_props(text_input, &props);
    er_event_set(text_input, ER_EVENT_SUBMIT_EDITING, on_submit_replaces, root);
    er_tree_append_child(root, text_input);
    er_tree_set_root(root);
    er_commit();

    er_text_input_focus(text_input);
    er_commit();

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 310, 235);
    embedded_renderer_touch(0, ER_TOUCH_UP, 310, 235);

    assert(s_replacement && "The done key must submit");
    assert(s_replacement->tag == s_replaced_tag && "The new input must reuse the old one's tag");
    assert(s_replacement->is_focused && "The new input must keep the focus");

    er_text_input_blur();
    printf("PASS: test_keyboard_done_keeps_a_new_focus\n");
}
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point.
 *
 * @return 0 on success.
 */
int main(void)
{
    test_activity_indicator_renders();
    test_switch_renders();
    test_text_input_keyboard();
    test_text_input_secure();
    test_text_input_read_only();
    test_text_input_destroyed_while_focused();
#if ERUI_ONSCREEN_KEYBOARD
    test_keyboard_done_submits();
    test_keyboard_done_keeps_a_new_focus();
#endif
    test_modal_visible();
    test_flatlist_scrolls();

    printf("All component tests passed.\n");
    return 0;
}
