/* Must be defined before any SDL header to disable SDL's main() macro on Windows. */
#define SDL_MAIN_HANDLED

#include "er_scene.h"
#include "native_renderer.h"
#include "sdl_backend.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SCREEN_W 960
#define SCREEN_H 620

#define CYCLE_COUNT 5

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static float s_dpi_scale = 1.0f;

/* Panel 2 — Touch & Events */
static ERNode* s_touch_card = NULL;
static ERNode* s_touch_label = NULL;
static int s_press_count = 0;
static int s_lp_count = 0;

/* Panel 3 — Animation & Display:None */
static ERNode* s_color_target = NULL;
static int s_color_index = 0;
static ERNode* s_hide_target = NULL;
static ERNode* s_hide_btn_label = NULL;
static bool s_target_hidden = false;

/* Shared status for zIndex test */
static ERNode* s_zidx_result = NULL;

static const uint32_t k_cycle_colors[] = {
    0xFF2A9D8F, 0xFFE94560, 0xFFF4A261, 0xFF9B59B6, 0xFF3498DB
};

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Converts a density-independent pixel value to physical pixels.
 *
 * Multiplies v by the DPI scale factor captured at startup. Returns v unchanged
 * on 1× displays where s_dpi_scale == 1.0.
 *
 * @param[in] v  Value in density-independent pixels.
 *
 * @return Equivalent value in physical pixels, rounded to the nearest integer.
 */
static int16_t dp(int v)
{
    return (int16_t)((float)v * s_dpi_scale + 0.5f);
}

/**
 * @brief Converts an SDL window coordinate to a physical framebuffer pixel.
 *
 * SDL reports mouse/touch events in logical window pixels. On HiDPI displays the
 * framebuffer is larger than the window, so the coordinate must be scaled before
 * being fed to the engine.
 *
 * @param[in] v  SDL logical coordinate.
 *
 * @return Corresponding physical framebuffer coordinate.
 */
static int event_px(int v)
{
    return (int)((float)v * s_dpi_scale + 0.5f);
}

/**
 * @brief Packs an ARGB8888 color value into a float for er_anim_start().
 *
 * The engine's animation API passes color targets as float, with the four bytes
 * of the ARGB8888 word stored verbatim in the float's bit pattern. This helper
 * reinterprets the cast safely via memcpy.
 *
 * @param[in] argb  Straight-alpha ARGB8888 color (0xAARRGGBB).
 *
 * @return Float whose raw bits equal the argb word.
 */
static float color_bits(uint32_t argb)
{
    float f;
    memcpy(&f, &argb, sizeof(f));
    return f;
}

/**
 * @brief Returns an ERProps struct with all layout fields set to ER_LAYOUT_AUTO.
 *
 * Initializes every dimensional field to ER_LAYOUT_AUTO so that unset values
 * are treated as "let the engine decide" rather than 0. View-type opacity is
 * set to 255 (fully opaque) to match React Native's default.
 *
 * @return Zero-initialized ERProps with AUTO layout sentinels and opacity 255.
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
 * @brief Starts a timing animation on a node's background color.
 *
 * Convenience wrapper around er_anim_start() that builds an ER_ANIM_TIMING
 * config and converts the ARGB8888 color to the float bit-pattern the API
 * expects.
 *
 * @param[in] node   The target scene node whose background color will animate.
 * @param[in] color  Destination color in 0xAARRGGBB premultiplied ARGB8888.
 * @param[in] ms     Animation duration in milliseconds.
 */
static void anim_bg(ERNode* node, uint32_t color, uint32_t ms)
{
    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = ms;
    er_anim_start(node, ER_PROP_BACKGROUND_COLOR, color_bits(color), &cfg);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Panel 2 callbacks (Touch & Events)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Updates the text of the Panel 2 status label.
 *
 * Rebuilds the label's ERProps with the supplied string and pushes it to the
 * engine. Does nothing when s_touch_label has not yet been created.
 *
 * @param[in] text  Null-terminated string to display (truncated to ER_TEXT_MAX).
 */
static void set_touch_label(const char* text)
{
    if (!s_touch_label)
        return;
    ERProps p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, text, ER_TEXT_MAX);
    p.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_touch_label, &p);
}

/**
 * @brief ER_EVENT_PRESS_IN callback for the Panel 2 touch card.
 *
 * Fires when a pointer first makes contact inside the card bounds. Updates the
 * status label and starts a background-color animation to signal the pressed
 * state.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_touch_press_in(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    set_touch_label("press_in  —  drag out to see press_out");
    anim_bg(s_touch_card, 0xFF264653, 80);
}

/**
 * @brief ER_EVENT_PRESS_OUT callback for the Panel 2 touch card.
 *
 * Fires when the pointer moves outside the card bounds while still held down.
 * Updates the status label and animates the background to the "drag-out" color.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_touch_press_out(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    set_touch_label("press_out  —  release inside to confirm");
    anim_bg(s_touch_card, 0xFFE94560, 120);
}

/**
 * @brief ER_EVENT_PRESS callback for the Panel 2 touch card.
 *
 * Fires on a confirmed short press (pointer down then up inside the card).
 * Increments the press counter, prints the tap coordinates in the status label,
 * and animates the card to the "confirmed" color.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload carrying the tap coordinates (data->x, data->y).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_touch_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)ctx;
    s_press_count++;
    char buf[ER_TEXT_MAX + 1];
    snprintf(buf, sizeof(buf), "press #%d  at (%d, %d)", s_press_count, data->x, data->y);
    set_touch_label(buf);
    anim_bg(s_touch_card, 0xFF2A9D8F, 200);
}

/**
 * @brief ER_EVENT_LONG_PRESS callback for the Panel 2 touch card.
 *
 * Fires when the pointer has been held inside the card bounds beyond the
 * long-press threshold. Increments the long-press counter, prints the hold
 * coordinates in the status label, and animates the card to the long-press color.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload carrying the hold coordinates (data->x, data->y).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_touch_long_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)ctx;
    s_lp_count++;
    char buf[ER_TEXT_MAX + 1];
    snprintf(buf, sizeof(buf), "long_press #%d  at (%d, %d)", s_lp_count, data->x, data->y);
    set_touch_label(buf);
    anim_bg(s_touch_card, 0xFFF4A261, 200);
}

/**
 * @brief ER_EVENT_TOUCH_CANCEL callback for the Panel 2 touch card.
 *
 * Fires when an in-progress touch sequence is canceled (e.g., the pointer
 * leaves the window). Updates the status label and returns the card to the
 * cancel color.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_touch_cancel(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    set_touch_label("cancel  (focus lost while pressed)");
    anim_bg(s_touch_card, 0xFFE94560, 120);
}

/**
 * @brief ER_EVENT_PRESS callback for the low-z card in the zIndex demo.
 *
 * Updates the zIndex result label to confirm that the z=1 card received the
 * press. This fires only when the user clicks an area not covered by the
 * higher-z card.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_zidx_low_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_zidx_result)
        return;
    ERProps p = props_default();
    p.color = 0xFF8BB8D0;
    p.font_size = (uint8_t)dp(12);
    strncpy(p.text, "LOW  z=1 received click  (not covered here)", ER_TEXT_MAX);
    er_node_set_props(s_zidx_result, &p);
}

/**
 * @brief ER_EVENT_PRESS callback for the high-z card in the zIndex demo.
 *
 * Updates the zIndex result label to confirm that the z=5 card received the
 * press. When the pointer is in the overlap area this fires instead of
 * on_zidx_low_press, proving that zIndex ordering is respected.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_zidx_high_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_zidx_result)
        return;
    ERProps p = props_default();
    p.color = 0xFF2A9D8F;
    p.font_size = (uint8_t)dp(12);
    strncpy(p.text, "HIGH  z=5 received click  (overlap area)", ER_TEXT_MAX);
    er_node_set_props(s_zidx_result, &p);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Panel 3 callbacks (Animation & Display:None)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief ER_EVENT_PRESS callback for the "CYCLE COLOR" button in Panel 3.
 *
 * Advances the color index through k_cycle_colors and starts a 1000 ms
 * timing animation on the color target node, demonstrating the background-
 * colour animation feature.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_cycle_color_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_color_target)
        return;
    s_color_index = (s_color_index + 1) % CYCLE_COUNT;
    anim_bg(s_color_target, k_cycle_colors[s_color_index], 1000);
}

/**
 * @brief ER_EVENT_PRESS callback for the "HIDE / SHOW" toggle button in Panel 3.
 *
 * Toggles s_target_hidden and rebuilds the target node's props with either
 * ER_DISPLAY_NONE or ER_DISPLAY_FLEX. The button label text is also updated
 * to reflect the new state. The next er_commit() picks up both changes.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_hide_toggle_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_hide_target || !s_hide_btn_label)
        return;

    s_target_hidden = !s_target_hidden;

    /* Rebuild props for the target with the new display mode. */
    ERProps tp = props_default();
    tp.height = dp(48);
    tp.background_color = 0xFF9B59B6;
    tp.border_radius = dp(8);
    tp.padding = dp(10);
    tp.justify_content = ER_JUSTIFY_CENTER;
    tp.align_items = ER_ALIGN_CENTER;
    tp.display = s_target_hidden ? ER_DISPLAY_NONE : ER_DISPLAY_FLEX;
    er_node_set_props(s_hide_target, &tp);

    /* Update the button label to reflect the new state. */
    ERProps lp = props_default();
    lp.color = 0xFFFFFFFF;
    lp.font_size = (uint8_t)dp(13);
    strncpy(lp.text,
            s_target_hidden ? "SHOW  (currently display:none)" : "HIDE  (currently display:flex)",
            ER_TEXT_MAX);
    lp.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_hide_btn_label, &lp);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — scene construction
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Creates a section-header TEXT node styled for panel titles.
 *
 * The node uses a small muted-blue colour and reduced font size so it reads as
 * a label rather than primary content.
 *
 * @param[in] text  Null-terminated header string (truncated to ER_TEXT_MAX).
 *
 * @return Newly created, props-set TEXT node. Caller is responsible for
 *         attaching it to the scene tree via er_tree_append_child().
 */
static ERNode* make_section_header(const char* text)
{
    ERNode* n = er_node_create(ER_NODE_TEXT);
    ERProps p = props_default();
    p.color = 0xFF5588AA;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, text, ER_TEXT_MAX);
    er_node_set_props(n, &p);
    return n;
}

/**
 * @brief Creates a single-line info TEXT node styled as a caption.
 *
 * Captions use a dim colour and small font so they provide context without
 * competing with the interactive elements above them.
 *
 * @param[in] text  Null-terminated caption string (truncated to ER_TEXT_MAX).
 *
 * @return Newly created, props-set TEXT node. Caller is responsible for
 *         attaching it to the scene tree via er_tree_append_child().
 */
static ERNode* make_caption(const char* text)
{
    ERNode* n = er_node_create(ER_NODE_TEXT);
    ERProps p = props_default();
    p.color = 0xFF99AABB;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, text, ER_TEXT_MAX);
    er_node_set_props(n, &p);
    return n;
}

/**
 * @brief Creates a PRESSABLE button node with a centred text label.
 *
 * The button is a dark-background rounded-rect PRESSABLE. The supplied
 * callback is registered for ER_EVENT_PRESS. The text label is created as a
 * child TEXT node.
 *
 * @param[in] label_text  Null-terminated label string (truncated to ER_TEXT_MAX).
 * @param[in] on_press    Callback invoked when the button is pressed.
 *
 * @return Newly created PRESSABLE node with the label already appended as a
 *         child. Caller must attach it to the scene tree.
 */
static ERNode* make_button(const char* label_text, EREventFn on_press)
{
    ERNode* btn = er_node_create(ER_NODE_PRESSABLE);
    ERProps p = props_default();
    p.height = dp(36);
    p.background_color = 0xFF243447;
    p.border_radius = dp(6);
    p.padding_left = p.padding_right = dp(12);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(btn, &p);
    er_event_set(btn, ER_EVENT_PRESS, on_press, NULL);

    ERNode* lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFDDEEFF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, label_text, ER_TEXT_MAX);
    er_node_set_props(lbl, &p);
    er_tree_append_child(btn, lbl);
    return btn;
}

/**
 * @brief Creates a vertical-column panel container with equal flex growth.
 *
 * Each panel occupies one third of the column row by virtue of flex_grow == 1
 * on all three sibling panels. Children are stretched to full width and spaced
 * with a uniform gap.
 *
 * @return Newly created VIEW node configured as a column panel. Caller must
 *         append child widgets and attach the panel to the scene tree.
 */
static ERNode* make_panel(void)
{
    ERNode* col = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.flex_grow = 1;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_STRETCH;
    p.background_color = 0xFF0D1B2A;
    p.border_radius = dp(10);
    p.padding = dp(14);
    p.gap = dp(9);
    er_node_set_props(col, &p);
    return col;
}

/**
 * @brief Builds the demo scene.
 *
 * Three panels demonstrate every engine feature that is currently working:
 *   Panel 1 — Layout & Style: flexbox rows, rounded rects, borders, justify-content.
 *   Panel 2 — Touch & Events: press/long-press/cancel, event coordinates, zIndex.
 *   Panel 3 — Animation & Display:None: bg-color timing animation, display:none toggle.
 *
 * @param[in] phys_w  Physical framebuffer width in pixels.
 * @param[in] phys_h  Physical framebuffer height in pixels.
 */
static void build_scene(int phys_w, int phys_h)
{
    ERProps p;

    /* =====================================================================
     * ROOT
     * =================================================================== */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = (int16_t)phys_w;
    p.height = (int16_t)phys_h;
    p.background_color = 0xFF111927;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_STRETCH;
    p.padding = dp(16);
    p.gap = dp(12);
    er_node_set_props(root, &p);

    /* =====================================================================
     * HEADER BAR
     * =================================================================== */
    ERNode* header = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(46);
    p.background_color = 0xFF172233;
    p.border_radius = dp(8);
    p.padding_left = dp(18);
    p.padding_right = dp(18);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_SPACE_BETWEEN;
    er_node_set_props(header, &p);

    ERNode* hdr_title = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFEEF4FF;
    p.font_size = (uint8_t)dp(20);
    strncpy(p.text, "embedded-react  engine demo", ER_TEXT_MAX);
    er_node_set_props(hdr_title, &p);

    ERNode* hdr_sub = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF556677;
    p.font_size = (uint8_t)dp(12);
    strncpy(p.text, "C99  Yoga flexbox  SDL2", ER_TEXT_MAX);
    er_node_set_props(hdr_sub, &p);

    er_tree_append_child(header, hdr_title);
    er_tree_append_child(header, hdr_sub);

    /* =====================================================================
     * COLUMN ROW
     * =================================================================== */
    ERNode* columns = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.flex_grow = 1;
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(12);
    er_node_set_props(columns, &p);

    /* =====================================================================
     * PANEL 1 — Layout & Style
     * =================================================================== */
    ERNode* col1 = make_panel();
    er_tree_append_child(col1, make_section_header("LAYOUT & STYLE"));

    /* Row A: three equal-width colored boxes */
    ERNode* row_a = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(44);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(6);
    er_node_set_props(row_a, &p);

    for (int i = 0; i < 3; i++)
    {
        const uint32_t row_a_colors[] = {0xFF2A9D8F, 0xFFE94560, 0xFFF4A261};
        ERNode* box = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.flex_grow = 1;
        p.background_color = row_a_colors[i];
        p.border_radius = dp(6);
        er_node_set_props(box, &p);
        er_tree_append_child(row_a, box);
    }
    er_tree_append_child(col1, row_a);
    er_tree_append_child(col1, make_caption("flex-row  equal grow  1:1:1"));

    /* Row B: grow ratio 1:2:1 */
    ERNode* row_b = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(44);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(4);
    er_node_set_props(row_b, &p);

    for (int i = 0; i < 3; i++)
    {
        const uint32_t row_b_colors[] = {0xFF264653, 0xFF2A9D8F, 0xFF457B9D};
        const int row_b_grow[] = {1, 2, 1};
        ERNode* box = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.flex_grow = (int16_t)row_b_grow[i];
        p.background_color = row_b_colors[i];
        p.border_radius = dp(5);
        er_node_set_props(box, &p);
        er_tree_append_child(row_b, box);
    }
    er_tree_append_child(col1, row_b);
    er_tree_append_child(col1, make_caption("flex-row  grow 1:2:1"));

    /* Bordered card */
    ERNode* bordered = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(56);
    p.background_color = 0xFF16273E;
    p.border_radius = dp(9);
    p.border_width = dp(2);
    p.border_color = 0xFF2A9D8F;
    p.padding = dp(12);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(bordered, &p);

    ERNode* bordered_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFEEF4FF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, "border_radius  +  border_color  +  border_width", ER_TEXT_MAX);
    er_node_set_props(bordered_lbl, &p);
    er_tree_append_child(bordered, bordered_lbl);
    er_tree_append_child(col1, bordered);

    /* justify-content: space-between demo */
    ERNode* jc_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(56);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.justify_content = ER_JUSTIFY_SPACE_BETWEEN;
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(7);
    p.padding = dp(8);
    er_node_set_props(jc_row, &p);

    for (int i = 0; i < 3; i++)
    {
        const uint32_t jc_colors[] = {0xFF9B59B6, 0xFF3498DB, 0xFF2ECC71};
        ERNode* box = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.width = dp(52);
        p.background_color = jc_colors[i];
        p.border_radius = dp(5);
        er_node_set_props(box, &p);
        er_tree_append_child(jc_row, box);
    }
    er_tree_append_child(col1, jc_row);
    er_tree_append_child(col1, make_caption("justify-content: space-between"));

    /* align-self demo */
    ERNode* as_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(56);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_FLEX_START;
    p.justify_content = ER_JUSTIFY_SPACE_AROUND;
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(7);
    p.padding = dp(6);
    p.gap = dp(4);
    er_node_set_props(as_row, &p);

    for (int i = 0; i < 4; i++)
    {
        const uint32_t as_colors[] = {0xFFE94560, 0xFFF4A261, 0xFF2A9D8F, 0xFF9B59B6};
        const int as_heights[] = {20, 34, 14, 44};
        ERNode* bar = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.flex_grow = 1;
        p.height = dp(as_heights[i]);
        p.background_color = as_colors[i];
        p.border_radius = dp(4);
        er_node_set_props(bar, &p);
        er_tree_append_child(as_row, bar);
    }
    er_tree_append_child(col1, as_row);
    er_tree_append_child(col1, make_caption("align-items: flex-start  (explicit heights)"));

    /* =====================================================================
     * PANEL 2 — Touch & Events
     * =================================================================== */
    ERNode* col2 = make_panel();
    er_tree_append_child(col2, make_section_header("TOUCH & EVENTS"));

    /* Interactive press card */
    ERNode* touch_card = er_node_create(ER_NODE_PRESSABLE);
    p = props_default();
    p.height = dp(78);
    p.background_color = 0xFFE94560;
    p.border_radius = dp(9);
    p.padding = dp(14);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(touch_card, &p);
    er_event_set(touch_card, ER_EVENT_PRESS, on_touch_press, NULL);
    er_event_set(touch_card, ER_EVENT_PRESS_IN, on_touch_press_in, NULL);
    er_event_set(touch_card, ER_EVENT_PRESS_OUT, on_touch_press_out, NULL);
    er_event_set(touch_card, ER_EVENT_LONG_PRESS, on_touch_long_press, NULL);
    er_event_set(touch_card, ER_EVENT_TOUCH_CANCEL, on_touch_cancel, NULL);
    s_touch_card = touch_card;

    ERNode* touch_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, "Press, long-press, or drag", ER_TEXT_MAX);
    er_node_set_props(touch_lbl, &p);
    s_touch_label = touch_lbl;
    er_tree_append_child(touch_card, touch_lbl);
    er_tree_append_child(col2, touch_card);

    er_tree_append_child(col2, make_caption(
                             "press   press_in   press_out   long_press   cancel"));

    /* zIndex stacking area */
    er_tree_append_child(col2, make_section_header("zINDEX STACKING"));
    er_tree_append_child(col2, make_caption("Click the overlap — higher z wins hit-test"));

    ERNode* zidx_area = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(96);
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(7);
    er_node_set_props(zidx_area, &p);

    /* LOW card: z=1, appended first */
    ERNode* z_low = er_node_create(ER_NODE_PRESSABLE);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = dp(10);
    p.top = dp(14);
    p.width = dp(140);
    p.height = dp(60);
    p.background_color = 0xFF457B9D;
    p.border_radius = dp(7);
    p.padding = dp(10);
    p.z_index = 1;
    er_node_set_props(z_low, &p);
    er_event_set(z_low, ER_EVENT_PRESS, on_zidx_low_press, NULL);

    ERNode* z_low_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, "LOW  z=1", ER_TEXT_MAX);
    er_node_set_props(z_low_lbl, &p);
    er_tree_append_child(z_low, z_low_lbl);

    /* HIGH card: z=5, overlaps LOW, appended after */
    ERNode* z_high = er_node_create(ER_NODE_PRESSABLE);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = dp(80);
    p.top = dp(22);
    p.width = dp(140);
    p.height = dp(60);
    p.background_color = 0xFF2A9D8F;
    p.border_radius = dp(7);
    p.padding = dp(10);
    p.z_index = 5;
    er_node_set_props(z_high, &p);
    er_event_set(z_high, ER_EVENT_PRESS, on_zidx_high_press, NULL);

    ERNode* z_high_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, "HIGH  z=5", ER_TEXT_MAX);
    er_node_set_props(z_high_lbl, &p);
    er_tree_append_child(z_high, z_high_lbl);

    er_tree_append_child(zidx_area, z_low);
    er_tree_append_child(zidx_area, z_high);
    er_tree_append_child(col2, zidx_area);

    ERNode* zidx_result = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF4477AA;
    p.font_size = (uint8_t)dp(12);
    strncpy(p.text, "click either card or their overlap", ER_TEXT_MAX);
    er_node_set_props(zidx_result, &p);
    s_zidx_result = zidx_result;
    er_tree_append_child(col2, zidx_result);

    /* =====================================================================
     * PANEL 3 — Animation & Display:None
     * =================================================================== */
    ERNode* col3 = make_panel();
    er_tree_append_child(col3, make_section_header("ANIMATION"));
    er_tree_append_child(col3, make_caption("background_color  timing  300 ms"));

    /* Color animation target */
    ERNode* color_target = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(52);
    p.background_color = k_cycle_colors[0];
    p.border_radius = dp(8);
    p.padding = dp(10);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(color_target, &p);
    s_color_target = color_target;

    ERNode* color_target_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(12);
    strncpy(p.text, "animating target", ER_TEXT_MAX);
    er_node_set_props(color_target_lbl, &p);
    er_tree_append_child(color_target, color_target_lbl);
    er_tree_append_child(col3, color_target);

    er_tree_append_child(col3, make_button("CYCLE COLOR  (1000 ms timing)", on_cycle_color_press));

    /* Divider gap via a thin VIEW */
    ERNode* divider = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(1);
    p.background_color = 0xFF223344;
    p.margin_top = dp(4);
    p.margin_bottom = dp(4);
    er_node_set_props(divider, &p);
    er_tree_append_child(col3, divider);

    er_tree_append_child(col3, make_section_header("DISPLAY:NONE"));
    er_tree_append_child(col3, make_caption("toggle removes node from layout + render + hit-test"));

    /* Visibility target */
    ERNode* hide_target = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(48);
    p.background_color = 0xFF9B59B6;
    p.border_radius = dp(8);
    p.padding = dp(10);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(hide_target, &p);
    s_hide_target = hide_target;

    ERNode* hide_target_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, "VISIBLE  (display:flex)", ER_TEXT_MAX);
    er_node_set_props(hide_target_lbl, &p);
    er_tree_append_child(hide_target, hide_target_lbl);
    er_tree_append_child(col3, hide_target);

    /* Hide/Show button */
    ERNode* hide_btn = er_node_create(ER_NODE_PRESSABLE);
    p = props_default();
    p.height = dp(36);
    p.background_color = 0xFF243447;
    p.border_radius = dp(6);
    p.padding_left = p.padding_right = dp(12);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(hide_btn, &p);
    er_event_set(hide_btn, ER_EVENT_PRESS, on_hide_toggle_press, NULL);

    ERNode* hide_btn_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFDDEEFF;
    p.font_size = (uint8_t)dp(13);
    strncpy(p.text, "HIDE  (currently display:flex)", ER_TEXT_MAX);
    er_node_set_props(hide_btn_lbl, &p);
    s_hide_btn_label = hide_btn_lbl;
    er_tree_append_child(hide_btn, hide_btn_lbl);
    er_tree_append_child(col3, hide_btn);

    /* =====================================================================
     * ASSEMBLE
     * =================================================================== */
    er_tree_append_child(columns, col1);
    er_tree_append_child(columns, col2);
    er_tree_append_child(columns, col3);

    er_tree_append_child(root, header);
    er_tree_append_child(root, columns);
    er_tree_set_root(root);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Application entry point.
 *
 * Initialises SDL2, creates a window and renderer, boots the SDL backend,
 * builds the demo scene, and runs the event/render loop until the user closes
 * the window or presses Escape.
 *
 * @return 0 on clean exit, 1 if any initialisation step fails.
 */
int main(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "embedded-react  engine demo",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_W,
        SCREEN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int phys_w = SCREEN_W, phys_h = SCREEN_H;
    SDL_GetRendererOutputSize(renderer, &phys_w, &phys_h);
    s_dpi_scale = (float)phys_w / (float)SCREEN_W;

    if (!er_sdl_backend_init(renderer, phys_w, phys_h))
    {
        SDL_Log("er_sdl_backend_init failed");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    build_scene(phys_w, phys_h);

    bool running = true;
    uint32_t prev = SDL_GetTicks();

    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
                running = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running = false;
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
                embedded_renderer_touch(0, ER_TOUCH_DOWN, event_px(ev.button.x), event_px(ev.button.y));
            if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
                embedded_renderer_touch(0, ER_TOUCH_UP, event_px(ev.button.x), event_px(ev.button.y));
            if (ev.type == SDL_MOUSEMOTION && (ev.motion.state & SDL_BUTTON_LMASK))
                embedded_renderer_touch(0, ER_TOUCH_MOVE, event_px(ev.motion.x), event_px(ev.motion.y));
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_LEAVE)
                embedded_renderer_touch(0, ER_TOUCH_CANCEL, 0, 0);
        }

        er_commit();
        er_sdl_present();

        const uint32_t now = SDL_GetTicks();
        embedded_renderer_tick(now - prev);
        prev = now;
    }

    er_sdl_backend_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
