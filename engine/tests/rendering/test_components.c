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

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[320 * 240];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — null backend
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Null fill_rect; writes a uniform solid color into the test framebuffer.
 *
 * @param[in] argb  Straight-alpha ARGB8888 fill color.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width.
 * @param[in] h     Height.
 * @param[in] ctx   Unused.
 */
static void fb_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            if (row >= 0 && row < 240 && col >= 0 && col < 320)
                s_fb[row * 320 + col] = argb;
}

/**
 * @brief Null copy_rect — no-op for tests that only check structural behavior.
 *
 * @param[in] src            Source pixel buffer.
 * @param[in] src_stride_bytes  Row stride.
 * @param[in] x              Destination left edge.
 * @param[in] y              Destination top edge.
 * @param[in] w              Width.
 * @param[in] h              Height.
 * @param[in] ctx            Unused.
 */
static void fb_copy(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)src_stride_bytes;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief Null blend_rect — no-op for structural tests.
 *
 * @param[in] src            Source pixel buffer.
 * @param[in] src_stride_bytes  Row stride.
 * @param[in] alpha          Global blend alpha.
 * @param[in] x              Destination left edge.
 * @param[in] y              Destination top edge.
 * @param[in] w              Width.
 * @param[in] h              Height.
 * @param[in] ctx            Unused.
 */
static void fb_blend(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)src_stride_bytes;
    (void)alpha;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
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
    ERProps p;
    memset(&p, 0, sizeof(p));
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = p.height = -32768;
    p.min_width = p.max_width = -32768;
    p.min_height = p.max_height = -32768;
    p.padding = p.padding_left = p.padding_top = p.padding_right = p.padding_bottom = -32768;
    p.margin = p.margin_left = p.margin_top = p.margin_right = p.margin_bottom = -32768;
    p.gap = p.row_gap = p.column_gap = -32768;
    p.flex_basis = -32768;
    p.opacity = 255U;
    return p;
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
    memset(s_fb, 0, sizeof(s_fb));
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
    ERProps p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 320;
    p.height = 240;
    er_node_set_props(root, &p);

    ERNode* act = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 48;
    p.height = 48;
    p.indicator_color = 0xFFFFFFFFU;
    p.animating = 1;
    er_node_set_props(act, &p);

    er_tree_append_child(root, act);
    er_tree_set_root(root);

    /* Initial spin angle must be 0 before any tick. */
    assert(act->tp_rotate_z == 0.0f && "Initial spin angle must be 0");
    assert(act->props.act.animating == 1 && "ActivityIndicator must be animating");

    /* First commit renders without crash. */
    er_commit();

    /* Advance 500 ms — rotation should be at ~180 degrees (half of 1000 ms cycle). */
    embedded_renderer_tick(500);
    er_commit();

    assert(act->tp_rotate_z > 100.0f && act->tp_rotate_z < 260.0f && "Spin angle must advance after 500 ms tick");

    /* Stopping the animation via props clears rotation advancement. */
    p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 48;
    p.height = 48;
    p.indicator_color = 0xFFFFFFFFU;
    p.animating = 0;
    er_node_set_props(act, &p);
    const float stopped_angle = act->tp_rotate_z;
    embedded_renderer_tick(200);
    er_commit();
    assert(act->tp_rotate_z == stopped_angle && "Spin angle must not change when animating=0");

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
    ERProps p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 320;
    p.height = 240;
    p.background_color = 0xFF111111U;
    er_node_set_props(root, &p);

    ERNode* sw = er_node_create(ER_NODE_SWITCH);
    p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 60;
    p.height = 30;
    p.top = 105;
    p.left = 130;
    p.switch_value = 0;
    p.track_color_false = 0xFF767577U;
    p.track_color_true = 0xFF2A9D8FU;
    p.thumb_color = 0xFFFFFFFFU;
    er_node_set_props(sw, &p);

    er_tree_append_child(root, sw);
    er_tree_set_root(root);
    er_commit();

    /* switch_thumb_t should start at 0.0 (off). */
    assert(sw->switch_thumb_t < 0.1f && "Switch thumb must start in off position");

    /* Toggle the switch on. */
    p.switch_value = 1;
    er_node_set_props(sw, &p);

    /* After a 300 ms tick the animation should be complete (duration is 200 ms). */
    embedded_renderer_tick(300);
    er_commit();

    assert(sw->switch_thumb_t > 0.9f && "Switch thumb must reach on position after animation");
    assert(sw->props.sw.value == 1 && "Switch value must be 1 after set");

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
    ERProps p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 320;
    p.height = 240;
    er_node_set_props(root, &p);

    ERNode* ti = er_node_create(ER_NODE_TEXT_INPUT);
    p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 200;
    p.height = 36;
    p.top = 102;
    p.left = 60;
    p.background_color = 0xFF1A2840U;
    p.border_color = 0xFF2A9D8FU;
    p.border_width = 2;
    p.border_radius = 6;
    p.color = 0xFFFFFFFFU;
    p.font_size = 14;
    p.editable = 1;
    strncpy(p.placeholder, "Type here...", ER_PLACEHOLDER_MAX);
    er_node_set_props(ti, &p);

    er_event_set(ti, ER_EVENT_CHANGE_TEXT, on_change_text, NULL);

    er_tree_append_child(root, ti);
    er_tree_set_root(root);
    er_commit();

    /* Focus the input. */
    er_text_input_focus(ti);
    assert(ti->is_focused && "TextInput must be focused after er_text_input_focus");

    /* Type 'H', 'i'. */
    embedded_renderer_key(0, "H");
    embedded_renderer_key(0, "i");

    assert(s_change_count == 2 && "Change event must fire for each key");
    assert(strcmp(er_text_input_get_text(ti), "Hi") == 0 && "TextInput text must match typed keys");

    /* Backspace. */
    embedded_renderer_key(ER_KEY_BACKSPACE, NULL);
    assert(strcmp(er_text_input_get_text(ti), "H") == 0 && "Backspace must remove last character");

    /* Programmatic set. */
    er_text_input_set_text(ti, "Hello");
    assert(strcmp(er_text_input_get_text(ti), "Hello") == 0 && "er_text_input_set_text must replace text");

    /* Blur. */
    er_text_input_blur();
    assert(!ti->is_focused && "TextInput must not be focused after blur");

    /* Key events should be ignored when unfocused. */
    embedded_renderer_key(0, "X");
    assert(strcmp(er_text_input_get_text(ti), "Hello") == 0 && "Unfocused input must not accept key events");

    printf("PASS: test_text_input_keyboard\n");
}

/**
 * @brief Most runs of one color along any row of a box in the test framebuffer.
 *
 * @param[in] argb  Color to look for.
 * @param[in] x     Box left edge.
 * @param[in] y     Box top edge.
 * @param[in] w     Box width.
 * @param[in] h     Box height.
 *
 * @return The largest number of separate runs on one row.
 */
static int max_runs_in_box(uint32_t argb, int x, int y, int w, int h)
{
    int best = 0;
    for (int row = y; row < y + h; row++)
    {
        int runs = 0;
        bool in_run = false;
        for (int col = x; col < x + w; col++)
        {
            const bool hit = s_fb[row * 320 + col] == argb;
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
 * @param[in] argb  Color to look for.
 * @param[in] x     Box left edge.
 * @param[in] y     Box top edge.
 * @param[in] w     Box width.
 * @param[in] h     Box height.
 *
 * @return The column, or -1 when the color is not there.
 */
static int first_col_in_box(uint32_t argb, int x, int y, int w, int h)
{
    for (int col = x; col < x + w; col++)
        for (int row = y; row < y + h; row++)
            if (s_fb[row * 320 + col] == argb)
                return col;
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
    ERProps p = props_auto();
    p.width = 320;
    p.height = 240;
    er_node_set_props(root, &p);

    ERNode* ti = er_node_create(ER_NODE_TEXT_INPUT);
    p = props_auto();
    p.width = 200;
    p.height = 36;
    p.background_color = 0xFF1A2840U;
    p.color = 0xFFFFFFFFU;
    p.cursor_color = 0xFFFF0000U;
    p.font_size = 16;
    p.editable = 1;
    p.secure_text_entry = 1;
    p.padding_horizontal = p.padding_vertical = -32768; /* unset, so the field's own padding applies */
    er_node_set_props(ti, &p);
    er_tree_append_child(root, ti);
    er_tree_set_root(root);
    er_text_input_set_text(ti, "a\303\251b"); /* three characters in four bytes */
    er_commit();

    /* The content box: the field's default 4 px / 3 px padding, no border. */
    const int cx = ti->computed.x + 4;
    const int cy = ti->computed.y + 3;
    const int cw = ti->computed.w - 8;
    const int ch = ti->computed.h - 6;
    assert(max_runs_in_box(0xFFFFFFFFU, cx, cy, cw, ch) == 3 && "One dot per character, not per byte");

    /* At 16 px the dots are 6 px wide and 4 px apart, the first 2 px in; the cursor goes after the last. */
    er_text_input_set_text(ti, "WWWWWW");
    er_text_input_focus(ti);
    const uint32_t phase = (uint32_t)(er_now_ms64() % 1000U); /* the cursor shows for the first half of 1 s */
    if (phase >= 500U)
        embedded_renderer_tick(1000U - phase);
    er_commit();
    assert(max_runs_in_box(0xFFFFFFFFU, cx, cy, cw, ch) == 6 && "A dot per character");
    assert(first_col_in_box(0xFFFF0000U, cx, cy, cw, ch) == cx + 6 * 10 && "The cursor must follow the dots");

    p.secure_text_entry = 0;
    er_node_set_props(ti, &p);
    er_commit();
    assert(max_runs_in_box(0xFFFFFFFFU, cx, cy, cw, ch) != 6 && "No dots once secureTextEntry is cleared");

    /* A font size past the text renderer's range is clamped for the dots too: 2 dots at 96 px advance 60 each. */
    p.secure_text_entry = 1;
    p.font_size = 200;
    er_node_set_props(ti, &p);
    er_text_input_set_text(ti, "ab");
    er_commit();
    assert(first_col_in_box(0xFFFF0000U, cx, cy, cw, ch) == cx + 2 * 60 && "Dots past 96 px must size as at 96 px");

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
    ERProps p = props_auto();
    p.width = 320;
    p.height = 240;
    er_node_set_props(root, &p);

    ERNode* ti = er_node_create(ER_NODE_TEXT_INPUT);
    p = props_auto();
    p.width = 200;
    p.height = 36;
    p.editable = 0;
    er_node_set_props(ti, &p);
    er_event_set(ti, ER_EVENT_CHANGE_TEXT, on_change_text, NULL);
    er_tree_append_child(root, ti);
    er_tree_set_root(root);
    er_text_input_set_text(ti, "Hi");
    er_commit();

    er_text_input_focus(ti);
    embedded_renderer_key(0, "X");
    assert(strcmp(er_text_input_get_text(ti), "Hi") == 0 && "A read-only input must ignore typing");
    embedded_renderer_key(ER_KEY_BACKSPACE, NULL);
    assert(strcmp(er_text_input_get_text(ti), "Hi") == 0 && "A read-only input must ignore backspace");
    assert(s_change_count == 0 && "A read-only input must not report a change");

    er_props_default(&p);
    p.width = 200;
    p.height = 36;
    er_node_set_props(ti, &p);
    embedded_renderer_key(0, "!");
    assert(strcmp(er_text_input_get_text(ti), "Hi!") == 0 && "Default props must leave the input editable");

    er_text_input_blur();
    printf("PASS: test_text_input_read_only\n");
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
    ERProps p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 320;
    p.height = 240;
    p.background_color = 0xFF223344U; /* distinct background color */
    er_node_set_props(root, &p);

    ERNode* modal = er_node_create(ER_NODE_MODAL);
    p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    /* Absolute position fills the root. */
    p.position = 1; /* ER_POS_ABSOLUTE */
    p.left = 0;
    p.top = 0;
    p.width = 320;
    p.height = 240;
    p.background_color = 0xFF172233U;
    p.modal_visible = 0;
    p.backdrop_color = 0x88000000U;
    er_node_set_props(modal, &p);

    er_tree_append_child(root, modal);
    er_tree_set_root(root);
    er_commit();

    /* With modal_visible=0 the modal should have display:none — no backdrop. */
    assert(modal->layout.display == 1 && "Hidden modal must have display:none"); /* ER_DISPLAY_NONE=1 */

    /* Show the modal. */
    p.modal_visible = 1;
    er_node_set_props(modal, &p);
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
    ERProps p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.width = 320;
    p.height = 240;
    er_node_set_props(root, &p);

    ERNode* fl = er_node_create(ER_NODE_FLAT_LIST);
    p = props_auto();
    p.left = p.top = p.right = p.bottom = -32768;
    p.flex_basis = -32768; /* ER_LAYOUT_AUTO */
    p.width = 320;
    p.height = 120;
    p.overflow = 2;       /* ER_OVERFLOW_SCROLL */
    p.flex_direction = 0; /* ER_FLEX_COL */
    p.align_items = 1;    /* ER_ALIGN_STRETCH */
    er_node_set_props(fl, &p);

    /* Add 5 tall rows to exceed the viewport. */
    for (int i = 0; i < 5; i++)
    {
        ERNode* row = er_node_create(ER_NODE_VIEW);
        p = props_auto();
        p.left = p.top = p.right = p.bottom = -32768;
        p.flex_basis = -32768; /* ER_LAYOUT_AUTO */
        p.height = 40;
        p.flex_shrink = 0; /* do not shrink below explicit height */
        er_node_set_props(row, &p);
        er_tree_append_child(fl, row);
    }

    er_tree_append_child(root, fl);
    er_tree_set_root(root);
    er_commit();

    /* Content size (5×40=200) must exceed viewport (120). */
    assert(fl->scroll_content_h > fl->computed.h && "FlatList content must exceed viewport height");

    /* Programmatic scroll. */
    er_scroll_view_set_offset(fl, 0.0f, 60.0f);
    assert(fl->scroll_offset_y == 60.0f && "FlatList scroll offset must update");

    printf("PASS: test_flatlist_scrolls\n");
}

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
    test_modal_visible();
    test_flatlist_scrolls();

    printf("All component tests passed.\n");
    return 0;
}
