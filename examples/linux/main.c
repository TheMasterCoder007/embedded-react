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

#define SCREEN_W 1440
#define SCREEN_H 1024

#define CYCLE_COUNT 5

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static float s_dpi_scale = 1.0f;

/* Dirty-rect diagnostic overlay toggle (D key). */
static bool s_dirty_overlay_on = false;

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

static const uint32_t k_cycle_colors[] = {0xFF2A9D8F, 0xFFE94560, 0xFFF4A261, 0xFF9B59B6, 0xFF3498DB};

/* Panel 4 — ScrollView */
static ERNode* s_sv_v_lbl = NULL; /**< Live vertical-offset readout label.   */
static ERNode* s_sv_h_lbl = NULL; /**< Live horizontal-offset readout label. */

/* Panel 5 — Transforms & Opacity (looping self-animated nodes) */
static ERNode* s_xform_translate = NULL;
static ERNode* s_xform_rotate = NULL;
static ERNode* s_xform_scale = NULL;
static ERNode* s_opacity_node = NULL;

/* Panel 6 — Spring, Sequence, Stagger */
static ERNode* s_spring_box = NULL;                          /**< Box that springs on button press. */
static bool s_spring_forward = false;                        /**< Direction toggle for spring demo. */
static ERNode* s_seq_boxes[3] = {NULL, NULL, NULL};          /**< Three boxes animated in sequence. */
static ERNode* s_stagger_bars[4] = {NULL, NULL, NULL, NULL}; /**< Four bars animated with stagger. */
static ERNode* s_spring_status_lbl = NULL;

/* Panel 7 — Text Features */
static ERNode* s_ls_lbl = NULL; /**< Letter-spacing demo label. */
static int s_ls_value = 0;      /**< Current letter_spacing value cycling through 0-4. */

/* Panel 8 — Components */
static ERNode* s_switch_node = NULL;     /**< Switch under test. */
static bool s_switch_on = false;         /**< Tracks logical Switch state. */
static ERNode* s_switch_label = NULL;    /**< Text label that mirrors Switch state. */
static ERNode* s_text_input_node = NULL; /**< TextInput under test. */
static ERNode* s_text_echo_lbl = NULL;   /**< Text node that mirrors TextInput content. */
static ERNode* s_modal_node = NULL;      /**< Modal under test. */
static ERNode* s_modal_open_btn_lbl = NULL;
static bool s_modal_visible = false;

static const uint32_t k_sv_v_colors[6] = {0xFF2A9D8F, 0xFFE94560, 0xFFF4A261, 0xFF9B59B6, 0xFF3498DB, 0xFF2ECC71};

static const uint32_t k_sv_h_colors[8] = {
    0xFF264653, 0xFF2A9D8F, 0xFF457B9D, 0xFFE94560, 0xFFF4A261, 0xFF9B59B6, 0xFF3498DB, 0xFF2ECC71};

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
    p.padding_horizontal = p.padding_vertical = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.margin_horizontal = p.margin_vertical = ER_LAYOUT_AUTO;
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

/**
 * @brief Starts a looping ping-pong timing animation on a transform property.
 *
 * @param[in] node   Target node.
 * @param[in] prop   Animatable property (ER_PROP_TRANSLATE_X/Y, SCALE_X/Y, ROTATE_Z, OPACITY).
 * @param[in] to_val Target value for the first half of each loop.
 * @param[in] ms     Duration of one half-cycle in milliseconds.
 */
static void anim_loop(ERNode* node, ERAnimProp prop, float to_val, uint32_t ms)
{
    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = ms;
    cfg.loop = true;
    er_anim_start(node, prop, to_val, &cfg);
}

/**
 * @brief Starts a spring animation on a transform property.
 *
 * @param[in] node      Target node.
 * @param[in] prop      Animatable property.
 * @param[in] to_val    Target value.
 * @param[in] stiffness Spring stiffness (higher = snappier).
 * @param[in] damping   Damping coefficient (higher = less bounce).
 */
static void anim_spring(ERNode* node, ERAnimProp prop, float to_val, float stiffness, float damping)
{
    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_SPRING;
    cfg.stiffness = stiffness;
    cfg.damping = damping;
    cfg.mass = 1.0f;
    er_anim_start(node, prop, to_val, &cfg);
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
    strncpy(
        lp.text, s_target_hidden ? "SHOW  (currently display:none)" : "HIDE  (currently display:flex)", ER_TEXT_MAX);
    lp.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_hide_btn_label, &lp);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Panel 4 callbacks (ScrollView)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief ER_EVENT_SCROLL callback for the vertical ScrollView demo.
 *
 * Updates the live offset readout label below the viewport.
 *
 * @param[in] node  Node that dispatched the event (unused).
 * @param[in] data  Event payload carrying data->scroll_y.
 * @param[in] ctx   User context pointer (unused).
 */
static void on_scroll_v(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)ctx;
    if (!s_sv_v_lbl)
        return;
    char buf[ER_TEXT_MAX + 1];
    snprintf(buf, sizeof(buf), "offset  y:  %.0f px", data->scroll_y);
    ERProps p = props_default();
    p.color = 0xFF4477AA;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, buf, ER_TEXT_MAX);
    p.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_sv_v_lbl, &p);
}

/**
 * @brief ER_EVENT_SCROLL callback for the horizontal ScrollView demo.
 *
 * Updates the live offset readout label below the viewport.
 *
 * @param[in] node  Node that dispatched the event (unused).
 * @param[in] data  Event payload carrying data->scroll_x.
 * @param[in] ctx   User context pointer (unused).
 */
static void on_scroll_h(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)ctx;
    if (!s_sv_h_lbl)
        return;
    char buf[ER_TEXT_MAX + 1];
    snprintf(buf, sizeof(buf), "offset  x:  %.0f px", data->scroll_x);
    ERProps p = props_default();
    p.color = 0xFF4477AA;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, buf, ER_TEXT_MAX);
    p.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_sv_h_lbl, &p);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Panel 6 callbacks (Spring / Sequence / Stagger)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief ER_EVENT_PRESS callback for the "SPRING" button in Panel 6.
 *
 * Toggles the spring box between translateX = 0 and translateX = dp(80),
 * using a bouncy underdamped spring (stiffness=200, damping=12).
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_spring_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_spring_box)
        return;
    s_spring_forward = !s_spring_forward;
    const float target = s_spring_forward ? (float)dp(80) : 0.0f;
    anim_spring(s_spring_box, ER_PROP_TRANSLATE_X, target, 200.0f, 12.0f);

    if (s_spring_status_lbl)
    {
        ERProps lp = props_default();
        lp.color = 0xFF4477AA;
        lp.font_size = (uint8_t)dp(11);
        strncpy(lp.text,
                s_spring_forward ? "springing forward  (stiffness=200  damping=12)" : "springing back",
                ER_TEXT_MAX);
        lp.text[ER_TEXT_MAX] = '\0';
        er_node_set_props(s_spring_status_lbl, &lp);
    }
}

/**
 * @brief ER_EVENT_PRESS callback for the "SEQUENCE" button in Panel 6.
 *
 * Cycles each of the three sequence boxes through a color animation in order,
 * demonstrating the er_anim_sequence() API.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_sequence_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_seq_boxes[0] || !s_seq_boxes[1] || !s_seq_boxes[2])
        return;

    static const uint32_t k_seq_colors[3][2] = {
        {0xFF264653, 0xFF2A9D8F},
        {0xFF264653, 0xFFE94560},
        {0xFF264653, 0xFFF4A261},
    };

    static int seq_phase = 0;

    ERAnimEntry entries[3];
    memset(entries, 0, sizeof(entries));

    for (int i = 0; i < 3; i++)
    {
        entries[i].node = s_seq_boxes[i];
        entries[i].prop = ER_PROP_BACKGROUND_COLOR;
        entries[i].value = color_bits(k_seq_colors[i][seq_phase & 1]);
        entries[i].cfg.type = ER_ANIM_TIMING;
        entries[i].cfg.easing = ER_EASE_EASE_IN_OUT;
        entries[i].cfg.duration_ms = 350U;
    }

    seq_phase++;
    er_anim_sequence(entries, 3, NULL, NULL);
}

/**
 * @brief ER_EVENT_PRESS callback for the "STAGGER" button in Panel 6.
 *
 * Animates four bars from opacity 0.2 to 1.0 (or back), each starting
 * 80 ms after the previous, demonstrating er_anim_stagger().
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_stagger_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;

    static bool stagger_in = false;
    stagger_in = !stagger_in;

    ERAnimEntry entries[4];
    memset(entries, 0, sizeof(entries));

    for (int i = 0; i < 4; i++)
    {
        if (!s_stagger_bars[i])
            return;
        entries[i].node = s_stagger_bars[i];
        entries[i].prop = ER_PROP_OPACITY;
        entries[i].value = stagger_in ? 1.0f : 0.2f;
        entries[i].cfg.type = ER_ANIM_TIMING;
        entries[i].cfg.easing = ER_EASE_EASE_OUT;
        entries[i].cfg.duration_ms = 400U;
    }

    er_anim_stagger(entries, 4, 80U, NULL, NULL);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Panel 7 callbacks (Text Features)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief ER_EVENT_PRESS callback for the "LETTER SPACING" cycle button in Panel 7.
 *
 * Cycles the letter_spacing demo label through values 0, 1, 2, 3, 4 px, rebuilding
 * the node props each time so the change is visible immediately.
 *
 * @param[in] node  Node that received the event (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context pointer (unused).
 */
static void on_ls_cycle(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_ls_lbl)
        return;
    /* Cycle through 0, 2, 4, 6 px spacing so the change is clearly visible. */
    static const int k_ls_steps[] = {0, 2, 4, 6};
    s_ls_value = (s_ls_value + 1) % 4;
    const int ls_px = k_ls_steps[s_ls_value];
    ERProps p = props_default();
    p.color = 0xFFEEF4FF;
    p.font_size = (uint8_t)dp(13);
    p.letter_spacing = (int16_t)dp(ls_px);
    char buf[ER_TEXT_MAX + 1];
    snprintf(buf, sizeof(buf), "SPACING  (%d px)", dp(ls_px));
    strncpy(p.text, buf, ER_TEXT_MAX);
    p.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_ls_lbl, &p);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — Panel 8 callbacks (Components)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief ER_EVENT_PRESS callback fired by the Switch node itself.
 *
 * The engine already flipped the Switch's value and started the thumb-slide
 * animation by the time this callback runs.  This handler only mirrors the
 * new state into the adjacent label so the user can see what changed.
 *
 * @param[in] node  Switch node that was pressed.
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context (unused).
 */
static void on_switch_press(ERNode* node, const EREventData* data, void* ctx)
{
    (void)data;
    (void)ctx;
    if (!node || !s_switch_label)
        return;
    /* Engine already flipped the Switch's value before invoking this handler;
     * mirror the new logical state in our local tracker. */
    s_switch_on = !s_switch_on;

    ERProps lp = props_default();
    lp.color = s_switch_on ? 0xFF2A9D8F : 0xFF99AABB;
    lp.font_size = (uint8_t)dp(12);
    strncpy(lp.text, s_switch_on ? "ON  (tap again to toggle off)" : "OFF  (tap switch to toggle)", ER_TEXT_MAX);
    lp.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_switch_label, &lp);
}

/**
 * @brief ER_EVENT_CHANGE_TEXT callback for the Panel 8 TextInput.
 *
 * Mirrors the typed text into the echo label so the user can see the input
 * value rendered as plain text.
 *
 * @param[in] node  TextInput node (unused).
 * @param[in] data  Event payload containing the updated text.
 * @param[in] ctx   User context (unused).
 */
static void on_text_input_change(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)ctx;
    if (!s_text_echo_lbl || !data)
        return;
    const char* live = data->changed_text ? data->changed_text : "";

    ERProps p = props_default();
    p.color = live[0] ? 0xFFEEF4FF : 0xFF99AABB;
    p.font_size = (uint8_t)dp(12);
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    char buf[ER_TEXT_MAX + 1];
    if (live[0] == '\0')
        snprintf(buf, sizeof(buf), "(type to see echo here)");
    else
        snprintf(buf, sizeof(buf), "echo:  %s", live);
    strncpy(p.text, buf, ER_TEXT_MAX);
    p.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_text_echo_lbl, &p);
}

/**
 * @brief ER_EVENT_SUBMIT_EDITING callback for the Panel 8 TextInput.
 *
 * Fires when the user presses Enter while the input is focused. Echos the
 * submitted text below in green so the user sees a clear "submit" signal,
 * then clears the input so they can type the next value.
 *
 * @param[in] node  TextInput node that submitted.
 * @param[in] data  Event payload (data->changed_text holds the submitted text).
 * @param[in] ctx   User context (unused).
 */
static void on_text_input_submit(ERNode* node, const EREventData* data, void* ctx)
{
    (void)ctx;
    if (!node || !s_text_echo_lbl || !data)
        return;
    const char* submitted = data->changed_text ? data->changed_text : "";

    ERProps p = props_default();
    p.color = 0xFF2A9D8F;
    p.font_size = (uint8_t)dp(12);
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    char buf[ER_TEXT_MAX + 1];
    snprintf(buf, sizeof(buf), "submitted:  %s", submitted[0] ? submitted : "(empty)");
    strncpy(p.text, buf, ER_TEXT_MAX);
    p.text[ER_TEXT_MAX] = '\0';
    er_node_set_props(s_text_echo_lbl, &p);

    /* Clear the input so the next keystroke starts a fresh entry. */
    er_text_input_set_text(node, "");
}

/**
 * @brief ER_EVENT_PRESS callback for the "OPEN MODAL" button.
 *
 * Sets the modal's modal_visible prop to 1, which causes the engine to render
 * the modal's backdrop and frame on top of the rest of the scene.
 *
 * @param[in] node  Pressable node (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context (unused).
 */
/**
 * @brief Builds the modal's ERProps with the given visibility.
 *
 * Centralised so on_modal_open / on_modal_close use the same layout — only the
 * modal_visible flag differs. align_items defaults to STRETCH so the dismiss
 * button receives the full inner width and is hit-testable.
 *
 * @param[in] visible  1 to show the modal, 0 to hide it.
 *
 * @return Fully populated ERProps ready for er_node_set_props.
 */
static ERProps modal_props(uint8_t visible)
{
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = dp(80);
    p.top = dp(180);
    p.width = dp(360);
    p.height = dp(200);
    p.background_color = 0xFF172233;
    p.border_radius = dp(12);
    p.border_color = 0xFF2A9D8F;
    p.border_width = dp(2);
    p.padding = dp(20);
    p.align_items = ER_ALIGN_STRETCH;
    p.justify_content = ER_JUSTIFY_CENTER;
    p.gap = dp(10);
    p.z_index = 1000;
    p.modal_visible = visible;
    p.backdrop_color = 0xCC000000;
    p.shadow_color = 0xFF000000;
    p.shadow_offset_y = (float)dp(10);
    p.shadow_opacity = 0.7f;
    p.shadow_radius = (uint8_t)dp(20);
    return p;
}

static void on_modal_open(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_modal_node)
        return;
    s_modal_visible = true;
    ERProps p = modal_props(1);
    er_node_set_props(s_modal_node, &p);
}

/**
 * @brief ER_EVENT_PRESS callback for the modal's "DISMISS" button.
 *
 * Sets modal_visible to 0, hiding the modal and its backdrop.
 *
 * @param[in] node  Pressable node (unused).
 * @param[in] data  Event payload (unused).
 * @param[in] ctx   User context (unused).
 */
static void on_modal_close(ERNode* node, const EREventData* data, void* ctx)
{
    (void)node;
    (void)data;
    (void)ctx;
    if (!s_modal_node)
        return;
    s_modal_visible = false;
    ERProps p = modal_props(0);
    er_node_set_props(s_modal_node, &p);
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
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
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
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
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
    /* align_items: STRETCH so the label fills the button's horizontal interior,
     * letting text_align: CENTER do the visual centering even when the text is
     * narrower than the button width. */
    p.align_items = ER_ALIGN_STRETCH;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(btn, &p);
    er_event_set(btn, ER_EVENT_PRESS, on_press, NULL);

    ERNode* lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFDDEEFF;
    p.font_size = (uint8_t)dp(13);
    p.text_align = ER_TEXT_ALIGN_CENTER;
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
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
 * @brief Builds Panel 6 — Spring, Sequence & Stagger.
 *
 * Three sub-sections:
 *   SPRING    — a single box that bounces to a target and back.
 *   SEQUENCE  — three boxes that light up one after another.
 *   STAGGER   — four bars that fade in with an offset delay between each.
 *
 * @return Fully populated panel VIEW node.
 */
static ERNode* build_spring_panel(void)
{
    ERNode* col = make_panel();
    ERProps p;

    /* ---- SPRING ---- */
    er_tree_append_child(col, make_section_header("SPRING"));
    er_tree_append_child(col, make_caption("stiffness=200  damping=12  (underdamped)"));

    /* Track area: holds the spring box in absolute position */
    ERNode* spring_track = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(48);
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(6);
    p.overflow = ER_OVERFLOW_HIDDEN;
    er_node_set_props(spring_track, &p);

    ERNode* spring_box = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.left = dp(8);
    p.top = dp(8);
    p.width = dp(32);
    p.height = dp(32);
    p.background_color = 0xFF2A9D8F;
    p.border_radius = dp(6);
    p.shadow_color = 0xFF2A9D8F;
    p.shadow_offset_x = 0.0f;
    p.shadow_offset_y = (float)dp(3);
    p.shadow_opacity = 0.7f;
    p.shadow_radius = (uint8_t)dp(5);
    er_node_set_props(spring_box, &p);
    s_spring_box = spring_box;
    er_tree_append_child(spring_track, spring_box);
    er_tree_append_child(col, spring_track);

    ERNode* spring_status = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF4477AA;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, "press button to launch", ER_TEXT_MAX);
    er_node_set_props(spring_status, &p);
    s_spring_status_lbl = spring_status;
    er_tree_append_child(col, spring_status);
    er_tree_append_child(col, make_button("SPRING", on_spring_press));

    /* ---- Divider ---- */
    ERNode* div1 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(1);
    p.background_color = 0xFF223344;
    p.margin_top = dp(3);
    p.margin_bottom = dp(3);
    er_node_set_props(div1, &p);
    er_tree_append_child(col, div1);

    /* ---- SEQUENCE ---- */
    er_tree_append_child(col, make_section_header("SEQUENCE"));
    er_tree_append_child(col, make_caption("three colors fire one after another  (350 ms each)"));

    static const uint32_t k_seq_init[3] = {0xFF264653, 0xFF264653, 0xFF264653};
    ERNode* seq_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(32);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(5);
    er_node_set_props(seq_row, &p);

    for (int i = 0; i < 3; i++)
    {
        ERNode* box = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.flex_grow = 1;
        p.background_color = k_seq_init[i];
        p.border_radius = dp(5);
        er_node_set_props(box, &p);
        s_seq_boxes[i] = box;
        er_tree_append_child(seq_row, box);
    }
    er_tree_append_child(col, seq_row);
    er_tree_append_child(col, make_button("PLAY SEQUENCE", on_sequence_press));

    /* ---- Divider ---- */
    ERNode* div2 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(1);
    p.background_color = 0xFF223344;
    p.margin_top = dp(3);
    p.margin_bottom = dp(3);
    er_node_set_props(div2, &p);
    er_tree_append_child(col, div2);

    /* ---- STAGGER ---- */
    er_tree_append_child(col, make_section_header("STAGGER"));
    er_tree_append_child(col, make_caption("four bars  —  80 ms offset between each start"));

    static const uint32_t k_bar_colors[4] = {0xFF2A9D8F, 0xFFE94560, 0xFFF4A261, 0xFF9B59B6};

    ERNode* stagger_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(32);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(4);
    er_node_set_props(stagger_row, &p);

    for (int i = 0; i < 4; i++)
    {
        ERNode* bar = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.flex_grow = 1;
        p.background_color = k_bar_colors[i];
        p.border_radius = dp(4);
        p.opacity = 51U; /* start at ~0.2 */
        er_node_set_props(bar, &p);
        s_stagger_bars[i] = bar;
        er_tree_append_child(stagger_row, bar);
    }
    er_tree_append_child(col, stagger_row);
    er_tree_append_child(col, make_button("STAGGER IN / OUT", on_stagger_press));

    return col;
}

/**
 * @brief Creates one transform-demo sub-item: a label above an animated inner box.
 *
 * The outer VIEW is a fixed-height column container (background tinted dark).
 * The inner VIEW is the node that will receive the looping transform animation.
 * A pointer to the inner node is written to *out_node.
 *
 * @param[in]  caption   Short label describing the transform.
 * @param[in]  box_color Fill color for the animated inner box.
 * @param[out] out_node  Receives the inner animated node.
 *
 * @return The outer container VIEW, ready to be appended to the panel.
 */
static ERNode* make_xform_demo(const char* caption, uint32_t box_color, ERNode** out_node)
{
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.flex_grow = 1;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(7);
    p.gap = dp(6);
    p.padding = dp(8);
    er_node_set_props(outer, &p);

    ERNode* lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF7799BB;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, caption, ER_TEXT_MAX);
    er_node_set_props(lbl, &p);
    er_tree_append_child(outer, lbl);

    ERNode* box = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = dp(44);
    p.height = dp(44);
    p.background_color = box_color;
    p.border_radius = dp(6);
    p.shadow_color = box_color; /* colored shadow matching the box */
    p.shadow_offset_x = (float)dp(2);
    p.shadow_offset_y = (float)dp(3);
    p.shadow_opacity = 0.55f;
    p.shadow_radius = (uint8_t)dp(5);
    er_node_set_props(box, &p);
    er_tree_append_child(outer, box);

    if (out_node)
        *out_node = box;
    return outer;
}

/**
 * @brief Builds Panel 5 — Transforms & Opacity.
 *
 * Four demo sub-items arranged in two rows:
 *   Row 1: translate X  |  rotate Z
 *   Row 2: scale X/Y    |  opacity
 *
 * The animated nodes are stored in s_xform_* / s_opacity_node so that the
 * caller can start looping animations after the scene is fully assembled.
 *
 * @return Fully populated panel VIEW node.
 */
static ERNode* build_transforms_panel(void)
{
    ERNode* col = make_panel();
    er_tree_append_child(col, make_section_header("TRANSFORMS  OPACITY  SHADOWS"));
    er_tree_append_child(col, make_caption("looping animations  —  shadow tracks translateX and opacity"));

    /* Row 1 */
    ERNode* row1 = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.flex_grow = 1;
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(6);
    er_node_set_props(row1, &p);

    er_tree_append_child(row1, make_xform_demo("translateX", 0xFF2A9D8F, &s_xform_translate));
    er_tree_append_child(row1, make_xform_demo("rotateZ", 0xFFE94560, &s_xform_rotate));
    er_tree_append_child(col, row1);

    /* Row 2 */
    ERNode* row2 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.flex_grow = 1;
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(6);
    er_node_set_props(row2, &p);

    er_tree_append_child(row2, make_xform_demo("scale", 0xFFF4A261, &s_xform_scale));
    er_tree_append_child(row2, make_xform_demo("opacity", 0xFF9B59B6, &s_opacity_node));
    er_tree_append_child(col, row2);

    return col;
}

/**
 * @brief Creates one alignment-demo row: a label above a constrained text node.
 *
 * The text node uses the supplied alignment value so the three rows in the alignment
 * section can be compared side-by-side.
 *
 * @param[in] label_text  Caption shown above the text sample.
 * @param[in] sample      Sample text to render with the given alignment.
 * @param[in] align       ERTextAlign value to apply.
 * @param[in] bg          Background color for the text container.
 *
 * @return Outer container VIEW, ready to be appended to the panel.
 */
static ERNode* make_align_demo(const char* label_text, const char* sample, uint8_t align, uint32_t bg)
{
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps p = props_default();
    p.flex_grow = 1;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(3);
    er_node_set_props(outer, &p);

    ERNode* cap = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF7799BB;
    p.font_size = (uint8_t)dp(10);
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
    strncpy(p.text, label_text, ER_TEXT_MAX);
    er_node_set_props(cap, &p);
    er_tree_append_child(outer, cap);

    ERNode* box = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.flex_grow = 1;
    p.background_color = bg;
    p.border_radius = dp(5);
    p.padding = dp(4);
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(box, &p);

    ERNode* txt = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFFFFFFF;
    p.font_size = (uint8_t)dp(12);
    p.text_align = align;
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
    strncpy(p.text, sample, ER_TEXT_MAX);
    er_node_set_props(txt, &p);
    er_tree_append_child(box, txt);
    er_tree_append_child(outer, box);

    return outer;
}

/**
 * @brief Builds Panel 7 — Text Features.
 *
 * Demonstrates word-boundary wrapping, textAlign (left / center / right),
 * numberOfLines + tail ellipsis, letterSpacing, and textDecoration.
 *
 * @return Fully populated panel VIEW node.
 */
static ERNode* build_text_panel(void)
{
    ERNode* col = make_panel();
    ERProps p;

    /* ---- Section: Alignment ---- */
    er_tree_append_child(col, make_section_header("TEXT ALIGN"));

    ERNode* align_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(56);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(4);
    er_node_set_props(align_row, &p);

    er_tree_append_child(align_row, make_align_demo("left", "align left", ER_TEXT_ALIGN_LEFT, 0xFF1A3048));
    er_tree_append_child(align_row, make_align_demo("center", "center", ER_TEXT_ALIGN_CENTER, 0xFF1A2840));
    er_tree_append_child(align_row, make_align_demo("right", "align right", ER_TEXT_ALIGN_RIGHT, 0xFF1A2038));
    er_tree_append_child(col, align_row);

    /* ---- Section: numberOfLines + ellipsis ---- */
    er_tree_append_child(col, make_section_header("NUMBER OF LINES"));
    er_tree_append_child(col, make_caption("1-line  +  tail ellipsis"));

    /* Fixed-height boxes so multi-line text has room to render. */
    ERNode* ellip_box = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(34);
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(5);
    p.padding_left = p.padding_right = dp(6);
    p.align_items = ER_ALIGN_STRETCH;      /* text fills width → left-adjusted */
    p.justify_content = ER_JUSTIFY_CENTER; /* vertically centre the single line */
    er_node_set_props(ellip_box, &p);

    ERNode* ellip_txt = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFF4A261;
    p.font_size = (uint8_t)dp(13);
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(p.text, "Long sentence intentionally exceeds the box width so tail ellipsis fires", ER_TEXT_MAX);
    er_node_set_props(ellip_txt, &p);
    er_tree_append_child(ellip_box, ellip_txt);
    er_tree_append_child(col, ellip_box);

    /* 2-line version: a VIEW with height:auto does not yet size to its children in the
     * engine, so the box is given an explicit height tall enough for two lines + padding. */
    er_tree_append_child(col, make_caption("2-line cap  +  ellipsis"));

    ERNode* ellip2_box = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(54);
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(5);
    p.padding = dp(6);
    er_node_set_props(ellip2_box, &p);

    ERNode* ellip2_txt = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF2A9D8F;
    p.font_size = (uint8_t)dp(13);
    p.number_of_lines = 2;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(p.text,
            "This longer paragraph wraps across several lines and is then capped at two lines so the tail "
            "ellipsis truncates the remaining overflow text",
            ER_TEXT_MAX);
    er_node_set_props(ellip2_txt, &p);
    er_tree_append_child(ellip2_box, ellip2_txt);
    er_tree_append_child(col, ellip2_box);

    /* ---- Section: letterSpacing ---- */
    er_tree_append_child(col, make_section_header("LETTER SPACING"));

    /* Fixed height so the label never wraps and always reads clearly. */
    ERNode* ls_box = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(34);
    p.background_color = 0xFF0F1E2D;
    p.border_radius = dp(5);
    p.padding_left = p.padding_right = dp(6);
    p.align_items = ER_ALIGN_CENTER;
    er_node_set_props(ls_box, &p);

    ERNode* ls_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFEEF4FF;
    p.font_size = (uint8_t)dp(13);
    p.letter_spacing = 0;
    strncpy(p.text, "SPACING  (0 px)", ER_TEXT_MAX);
    er_node_set_props(ls_lbl, &p);
    s_ls_lbl = ls_lbl;
    er_tree_append_child(ls_box, ls_lbl);
    er_tree_append_child(col, ls_box);
    er_tree_append_child(col, make_button("CYCLE SPACING", on_ls_cycle));

    /* ---- Section: textDecoration ---- */
    er_tree_append_child(col, make_section_header("TEXT DECORATION"));

    ERNode* deco_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(36);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_CENTER;
    p.gap = dp(12);
    er_node_set_props(deco_row, &p);

    ERNode* under_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF2A9D8F;
    p.font_size = (uint8_t)dp(14);
    p.text_decoration = ER_TEXT_DECORATION_UNDERLINE;
    strncpy(p.text, "underline", ER_TEXT_MAX);
    er_node_set_props(under_lbl, &p);
    er_tree_append_child(deco_row, under_lbl);

    ERNode* strike_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFE94560;
    p.font_size = (uint8_t)dp(14);
    p.text_decoration = ER_TEXT_DECORATION_LINE_THROUGH;
    strncpy(p.text, "line-through", ER_TEXT_MAX);
    er_node_set_props(strike_lbl, &p);
    er_tree_append_child(deco_row, strike_lbl);

    er_tree_append_child(col, deco_row);

    return col;
}

/**
 * @brief Builds Panel 8 — Components.
 *
 * Demonstrates ActivityIndicator (spinner), Switch (animated toggle), TextInput
 * (keyboard-driven editing), FlatList (virtualised list, behaves as a scroller),
 * and Modal (overlay with backdrop and dismiss).
 *
 * @return Fully populated panel VIEW node.
 */
static ERNode* build_components_panel(void)
{
    ERNode* col = make_panel();
    ERProps p;

    /* ---- ACTIVITY INDICATOR ---- */
    er_tree_append_child(col, make_section_header("ACTIVITY INDICATOR"));
    er_tree_append_child(col, make_caption("8-dot fading ring  —  auto-spinning"));

    ERNode* act_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(60);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_SPACE_AROUND;
    er_node_set_props(act_row, &p);

    ERNode* act_small = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    p = props_default();
    p.width = dp(28);
    p.height = dp(28);
    p.indicator_color = 0xFF2A9D8F;
    p.animating = 1;
    er_node_set_props(act_small, &p);
    er_tree_append_child(act_row, act_small);

    ERNode* act_med = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    p = props_default();
    p.width = dp(44);
    p.height = dp(44);
    p.indicator_color = 0xFFF4A261;
    p.animating = 1;
    er_node_set_props(act_med, &p);
    er_tree_append_child(act_row, act_med);

    ERNode* act_big = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    p = props_default();
    p.width = dp(54);
    p.height = dp(54);
    p.indicator_color = 0xFFE94560;
    p.animating = 1;
    er_node_set_props(act_big, &p);
    er_tree_append_child(act_row, act_big);

    er_tree_append_child(col, act_row);

    /* Divider */
    ERNode* div1 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(1);
    p.background_color = 0xFF223344;
    p.margin_top = dp(3);
    p.margin_bottom = dp(3);
    er_node_set_props(div1, &p);
    er_tree_append_child(col, div1);

    /* ---- SWITCH ---- */
    er_tree_append_child(col, make_section_header("SWITCH"));
    er_tree_append_child(col, make_caption("tap button  —  animated track + thumb"));

    ERNode* sw_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(36);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_SPACE_BETWEEN;
    er_node_set_props(sw_row, &p);

    ERNode* sw = er_node_create(ER_NODE_SWITCH);
    p = props_default();
    p.width = dp(50);
    p.height = dp(28);
    p.switch_value = 0;
    p.track_color_false = 0xFF455A64;
    p.track_color_true = 0xFF2A9D8F;
    p.thumb_color = 0xFFFFFFFF;
    er_node_set_props(sw, &p);
    s_switch_node = sw;
    er_event_set(sw, ER_EVENT_PRESS, on_switch_press, NULL);
    er_tree_append_child(sw_row, sw);

    ERNode* sw_label = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF99AABB;
    p.font_size = (uint8_t)dp(12);
    strncpy(p.text, "OFF  (tap switch to toggle)", ER_TEXT_MAX);
    er_node_set_props(sw_label, &p);
    s_switch_label = sw_label;
    er_tree_append_child(sw_row, sw_label);

    er_tree_append_child(col, sw_row);

    /* Divider */
    ERNode* div2 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(1);
    p.background_color = 0xFF223344;
    p.margin_top = dp(3);
    p.margin_bottom = dp(3);
    er_node_set_props(div2, &p);
    er_tree_append_child(col, div2);

    /* ---- TEXT INPUT ---- */
    er_tree_append_child(col, make_section_header("TEXT INPUT"));
    er_tree_append_child(col, make_caption("click to focus  —  type to edit"));

    ERNode* ti = er_node_create(ER_NODE_TEXT_INPUT);
    p = props_default();
    p.height = dp(34);
    p.background_color = 0xFF0F1E2D;
    p.border_color = 0xFF2A4D6B;
    p.border_width = dp(1);
    p.border_radius = dp(6);
    p.color = 0xFFFFFFFF;
    p.placeholder_color = 0xFF55708C;
    p.cursor_color = 0xFF2A9D8F;
    p.font_size = (uint8_t)dp(13);
    p.editable = 1;
    strncpy(p.placeholder, "Type something...", ER_PLACEHOLDER_MAX);
    er_node_set_props(ti, &p);
    s_text_input_node = ti;
    er_event_set(ti, ER_EVENT_CHANGE_TEXT, on_text_input_change, NULL);
    er_event_set(ti, ER_EVENT_SUBMIT_EDITING, on_text_input_submit, NULL);
    er_tree_append_child(col, ti);

    ERNode* echo = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF99AABB;
    p.font_size = (uint8_t)dp(12);
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(p.text, "(type to see echo here)", ER_TEXT_MAX);
    er_node_set_props(echo, &p);
    s_text_echo_lbl = echo;
    er_tree_append_child(col, echo);

    /* Divider */
    ERNode* div3 = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(1);
    p.background_color = 0xFF223344;
    p.margin_top = dp(3);
    p.margin_bottom = dp(3);
    er_node_set_props(div3, &p);
    er_tree_append_child(col, div3);

    /* ---- MODAL ---- */
    er_tree_append_child(col, make_section_header("MODAL"));
    er_tree_append_child(col, make_caption("backdrop overlay  +  centered card"));
    er_tree_append_child(col, make_button("OPEN MODAL", on_modal_open));

    return col;
}

/**
 * @brief Builds Panel 9 — Borders & Layout Additions.
 *
 * Demonstrates the new layout and border features:
 *   - Per-corner border radii (borderTopLeftRadius, etc.)
 *   - borderStyle: solid, dashed, and dotted
 *   - aspectRatio: a 1:1 square derived purely from width
 *   - flex_basis_pct: two boxes each claiming 50% of the row
 *   - paddingHorizontal / marginHorizontal shorthands
 *
 * @return Fully populated panel VIEW node.
 */
static ERNode* build_borders_layout_panel(void)
{
    ERNode* col = make_panel();
    ERProps p;

    er_tree_append_child(col, make_section_header("BORDER STYLES"));
    er_tree_append_child(col, make_caption("solid  /  dashed  /  dotted"));

    /* Row: three boxes demonstrating each border style. */
    ERNode* style_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.flex_direction = ER_FLEX_ROW;
    p.height = dp(38);
    p.gap = dp(6);
    er_node_set_props(style_row, &p);

    const uint8_t styles[3] = {ER_BORDER_SOLID, ER_BORDER_DASHED, ER_BORDER_DOTTED};
    const char* style_labels[3] = {"solid", "dashed", "dotted"};
    for (int i = 0; i < 3; i++)
    {
        ERNode* box = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.flex_grow = 1;
        p.align_items = ER_ALIGN_CENTER;
        p.justify_content = ER_JUSTIFY_CENTER;
        p.background_color = 0xFF1A2D42;
        p.border_color = 0xFF4499DD;
        p.border_width = dp(2);
        p.border_style = styles[i];
        er_node_set_props(box, &p);

        ERNode* lbl = er_node_create(ER_NODE_TEXT);
        p = props_default();
        p.color = 0xFF88BBDD;
        p.font_size = (uint8_t)dp(11);
        strncpy(p.text, style_labels[i], ER_TEXT_MAX);
        er_node_set_props(lbl, &p);
        er_tree_append_child(box, lbl);
        er_tree_append_child(style_row, box);
    }
    er_tree_append_child(col, style_row);

    er_tree_append_child(col, make_section_header("PER-CORNER RADIUS"));

    /* Card: top corners rounded only (banner / tab style). */
    ERNode* card_top = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(38);
    p.background_color = 0xFF2A3D52;
    p.border_color = 0xFF3A9D8F;
    p.border_width = dp(1);
    p.border_top_left_radius = dp(14);
    p.border_top_right_radius = dp(14);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card_top, &p);

    ERNode* card_top_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFCCEEDD;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, "TL:14  TR:14  BR:0  BL:0", ER_TEXT_MAX);
    er_node_set_props(card_top_lbl, &p);
    er_tree_append_child(card_top, card_top_lbl);
    er_tree_append_child(col, card_top);

    /* Card: asymmetric radii (diagonal corners). */
    ERNode* card_asym = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(38);
    p.background_color = 0xFF2A2040;
    p.border_color = 0xFF9B59B6;
    p.border_width = dp(1);
    p.border_top_left_radius = dp(2);
    p.border_top_right_radius = dp(14);
    p.border_bottom_right_radius = dp(2);
    p.border_bottom_left_radius = dp(14);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(card_asym, &p);

    ERNode* card_asym_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFDDBBFF;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, "TL:2  TR:14  BR:2  BL:14", ER_TEXT_MAX);
    er_node_set_props(card_asym_lbl, &p);
    er_tree_append_child(card_asym, card_asym_lbl);
    er_tree_append_child(col, card_asym);

    er_tree_append_child(col, make_section_header("ASPECT RATIO  +  FLEX BASIS %"));

    /* Row: 1:1 aspect-ratio box + two flex-basis 50% boxes. */
    ERNode* ar_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_CENTER;
    p.gap = dp(8);
    p.height = dp(64);
    er_node_set_props(ar_row, &p);

    /* 1:1 square: explicit width, height derived from aspect_ratio. */
    ERNode* square = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.width = dp(48);
    p.aspect_ratio = 1.0f;
    p.background_color = 0xFFF4A261;
    p.border_radius = dp(6);
    p.align_items = ER_ALIGN_CENTER;
    p.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(square, &p);

    ERNode* square_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF111111;
    p.font_size = (uint8_t)dp(10);
    strncpy(p.text, "1:1", ER_TEXT_MAX);
    er_node_set_props(square_lbl, &p);
    er_tree_append_child(square, square_lbl);
    er_tree_append_child(ar_row, square);

    /* Flex-basis 50% container (the remainder after the square).
     * align_self = STRETCH is required here: the parent ar_row uses align_items = CENTER
     * so that the square stays at its aspect-ratio height; without an explicit STRETCH
     * override, flex_row would resolve to zero cross-axis size and its children would
     * have zero height and invisible backgrounds.
     * No gap: two children at 50% each already fill exactly 100% of the container,
     * and adding a gap would cause overflow (flex_shrink defaults to 0). */
    ERNode* flex_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.flex_grow = 1;
    p.align_self = ER_ALIGN_STRETCH;
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    er_node_set_props(flex_row, &p);

    for (int i = 0; i < 2; i++)
    {
        const uint32_t pct_colors[2] = {0xFF2A9D8F, 0xFFE94560};
        ERNode* half = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.flex_basis_pct = 50.0f;
        p.background_color = pct_colors[i];
        p.border_radius = dp(5);
        p.align_items = ER_ALIGN_CENTER;
        p.justify_content = ER_JUSTIFY_CENTER;
        er_node_set_props(half, &p);

        ERNode* half_lbl = er_node_create(ER_NODE_TEXT);
        p = props_default();
        p.color = 0xFFFFFFFF;
        p.font_size = (uint8_t)dp(11);
        strncpy(p.text, "50%", ER_TEXT_MAX);
        er_node_set_props(half_lbl, &p);
        er_tree_append_child(half, half_lbl);
        er_tree_append_child(flex_row, half);
    }
    er_tree_append_child(ar_row, flex_row);
    er_tree_append_child(col, ar_row);

    er_tree_append_child(col, make_section_header("MARGIN/PADDING H/V SHORTHANDS"));

    /* Three items with paddingHorizontal + marginHorizontal. */
    ERNode* hv_col = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.flex_direction = ER_FLEX_COL;
    p.gap = dp(5);
    er_node_set_props(hv_col, &p);

    const char* hv_labels[3] = {"paddingH:12  marginH:8", "paddingV:4   marginV:6", "pV:6 pH:10 mH:16"};
    for (int i = 0; i < 3; i++)
    {
        ERNode* item = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.background_color = 0xFF172840;
        p.border_radius = dp(5);
        p.border_color = 0xFF334455;
        p.border_width = dp(1);
        if (i == 0)
        {
            p.padding_horizontal = dp(12);
            p.margin_horizontal = dp(8);
        }
        else if (i == 1)
        {
            p.padding_vertical = dp(4);
            p.margin_vertical = dp(6);
        }
        else
        {
            p.padding_vertical = dp(6);
            p.padding_horizontal = dp(10);
            p.margin_horizontal = dp(16);
        }
        er_node_set_props(item, &p);

        ERNode* item_lbl = er_node_create(ER_NODE_TEXT);
        p = props_default();
        p.color = 0xFF8899AA;
        p.font_size = (uint8_t)dp(10);
        strncpy(p.text, hv_labels[i], ER_TEXT_MAX);
        er_node_set_props(item_lbl, &p);
        er_tree_append_child(item, item_lbl);
        er_tree_append_child(hv_col, item);
    }
    er_tree_append_child(col, hv_col);

    return col;
}

/**
 * @brief Builds the demo scene.
 *
 * Two rows demonstrate every engine feature that is currently working:
 *   Row 1 Panel 1 — Layout & Style: flexbox rows, rounded rects, borders, justify-content.
 *   Row 1 Panel 2 — Touch & Events: press/long-press/cancel, event coordinates, zIndex.
 *   Row 1 Panel 3 — Animation & Display:None: bg-color timing animation, display:none toggle.
 *   Row 2 Panel 4 — ScrollView: vertical + horizontal viewports, clip, gesture, momentum.
 *   Row 2 Panel 9 — Borders & Layout: per-corner radii, borderStyle, aspectRatio, flexBasis%, paddingH/V.
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
    p.shadow_color = 0xFF000000;
    p.shadow_offset_y = (float)dp(4);
    p.shadow_opacity = 0.45f;
    p.shadow_radius = (uint8_t)dp(7);
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
#if ERUI_SHADOWS
    strncpy(p.text,
            "C99  Yoga flexbox  SDL2  |  shadows  spring  sequence  text  —  press D = dirty rect overlay",
            ER_TEXT_MAX);
#else
    strncpy(p.text,
            "C99  Yoga flexbox  SDL2  |  spring  sequence  text align  —  press D = dirty rect overlay",
            ER_TEXT_MAX);
#endif
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
    p.shadow_color = 0xFF2A9D8F; /* teal shadow matching border */
    p.shadow_offset_x = (float)dp(3);
    p.shadow_offset_y = (float)dp(3);
    p.shadow_opacity = 0.55f;
    p.shadow_radius = (uint8_t)dp(5);
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
    p.shadow_color = 0xFF000000;
    p.shadow_offset_x = (float)dp(2);
    p.shadow_offset_y = (float)dp(4);
    p.shadow_opacity = 0.5f;
    p.shadow_radius = (uint8_t)dp(6);
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

    er_tree_append_child(col2, make_caption("press   press_in   press_out   long_press   cancel"));

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
    p.shadow_color = 0xFF000000;
    p.shadow_offset_y = (float)dp(3);
    p.shadow_opacity = 0.35f;
    p.shadow_radius = (uint8_t)dp(4);
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
    p.shadow_color = 0xFF000000;
    p.shadow_offset_y = (float)dp(5);
    p.shadow_opacity = 0.5f;
    p.shadow_radius = (uint8_t)dp(6);
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
     * PANEL 4 — ScrollView (bottom row, full width, split into two halves)
     * =================================================================== */

    /* ---- Left half: vertical ScrollView -------------------------------- */
    ERNode* sv_left = make_panel();
    er_tree_append_child(sv_left, make_section_header("SCROLL VIEW  vertical"));
    er_tree_append_child(sv_left, make_caption("drag up / down  —  fling for momentum"));

    ERNode* sv_v = er_node_create(ER_NODE_SCROLL_VIEW);
    p = props_default();
    p.height = dp(200);
    p.overflow = ER_OVERFLOW_SCROLL;
    p.flex_direction = ER_FLEX_COL;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(4);
    er_node_set_props(sv_v, &p);
    er_event_set(sv_v, ER_EVENT_SCROLL, on_scroll_v, NULL);

    for (int i = 0; i < 6; i++)
    {
        ERNode* card = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.height = dp(36);
        p.background_color = k_sv_v_colors[i];
        p.border_radius = dp(5);
        p.align_items = ER_ALIGN_CENTER;
        p.justify_content = ER_JUSTIFY_CENTER;
        er_node_set_props(card, &p);

        ERNode* card_lbl = er_node_create(ER_NODE_TEXT);
        p = props_default();
        p.color = 0xFFFFFFFF;
        p.font_size = (uint8_t)dp(11);
        char row_txt[16];
        snprintf(row_txt, sizeof(row_txt), "row %d", i + 1);
        strncpy(p.text, row_txt, ER_TEXT_MAX);
        er_node_set_props(card_lbl, &p);
        er_tree_append_child(card, card_lbl);
        er_tree_append_child(sv_v, card);
    }
    er_tree_append_child(sv_left, sv_v);

    ERNode* sv_v_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF4477AA;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, "offset  y:  0 px", ER_TEXT_MAX);
    er_node_set_props(sv_v_lbl, &p);
    s_sv_v_lbl = sv_v_lbl;
    er_tree_append_child(sv_left, sv_v_lbl);

    /* ---- Right half: horizontal ScrollView ----------------------------- */
    ERNode* sv_right = make_panel();
    er_tree_append_child(sv_right, make_section_header("SCROLL VIEW  horizontal"));
    er_tree_append_child(sv_right, make_caption("swipe left / right"));

    ERNode* sv_h = er_node_create(ER_NODE_SCROLL_VIEW);
    p = props_default();
    p.height = dp(80);
    p.overflow = ER_OVERFLOW_SCROLL;
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(5);
    er_node_set_props(sv_h, &p);
    er_event_set(sv_h, ER_EVENT_SCROLL, on_scroll_h, NULL);

    for (int i = 0; i < 8; i++)
    {
        ERNode* card = er_node_create(ER_NODE_VIEW);
        p = props_default();
        p.width = dp(80);
        p.background_color = k_sv_h_colors[i];
        p.border_radius = dp(5);
        p.align_items = ER_ALIGN_CENTER;
        p.justify_content = ER_JUSTIFY_CENTER;
        er_node_set_props(card, &p);

        ERNode* card_lbl = er_node_create(ER_NODE_TEXT);
        p = props_default();
        p.color = 0xFFFFFFFF;
        p.font_size = (uint8_t)dp(11);
        char col_txt[16];
        snprintf(col_txt, sizeof(col_txt), "col %d", i + 1);
        strncpy(p.text, col_txt, ER_TEXT_MAX);
        er_node_set_props(card_lbl, &p);
        er_tree_append_child(card, card_lbl);
        er_tree_append_child(sv_h, card);
    }
    er_tree_append_child(sv_right, sv_h);

    ERNode* sv_h_lbl = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF4477AA;
    p.font_size = (uint8_t)dp(11);
    strncpy(p.text, "offset  x:  0 px", ER_TEXT_MAX);
    er_node_set_props(sv_h_lbl, &p);
    s_sv_h_lbl = sv_h_lbl;
    er_tree_append_child(sv_right, sv_h_lbl);

    /* ---- Bottom row container ------------------------------------------ */
    ERNode* sv_row = er_node_create(ER_NODE_VIEW);
    p = props_default();
    p.height = dp(500);
    p.flex_direction = ER_FLEX_ROW;
    p.align_items = ER_ALIGN_STRETCH;
    p.gap = dp(12);
    er_node_set_props(sv_row, &p);
    er_tree_append_child(sv_row, sv_left);
    er_tree_append_child(sv_row, sv_right);
    er_tree_append_child(sv_row, build_transforms_panel());
    er_tree_append_child(sv_row, build_spring_panel());
    er_tree_append_child(sv_row, build_text_panel());
    er_tree_append_child(sv_row, build_components_panel());
    er_tree_append_child(sv_row, build_borders_layout_panel());

    /* =====================================================================
     * ASSEMBLE
     * =================================================================== */
    er_tree_append_child(columns, col1);
    er_tree_append_child(columns, col2);
    er_tree_append_child(columns, col3);

    er_tree_append_child(root, header);
    er_tree_append_child(root, columns);
    er_tree_append_child(root, sv_row);

    /* ---- Modal overlay node (hidden by default; appears on OPEN MODAL press) ---- */
    ERNode* modal = er_node_create(ER_NODE_MODAL);
    p = modal_props(0);
    er_node_set_props(modal, &p);
    s_modal_node = modal;

    /* Modal content: title, body, dismiss button. */
    ERNode* modal_title = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFFEEF4FF;
    p.font_size = (uint8_t)dp(18);
    p.text_align = ER_TEXT_ALIGN_CENTER;
    p.number_of_lines = 1;
    p.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
    strncpy(p.text, "Modal Dialog", ER_TEXT_MAX);
    er_node_set_props(modal_title, &p);
    er_tree_append_child(modal, modal_title);

    ERNode* modal_body = er_node_create(ER_NODE_TEXT);
    p = props_default();
    p.color = 0xFF99AABB;
    p.font_size = (uint8_t)dp(12);
    p.text_align = ER_TEXT_ALIGN_CENTER;
    p.number_of_lines = 2;
    p.height = dp(40); /* room for two lines */
    strncpy(p.text, "Backdrop dims the rest of the scene.  Tap dismiss to close.", ER_TEXT_MAX);
    er_node_set_props(modal_body, &p);
    er_tree_append_child(modal, modal_body);

    /* Constrain the dismiss button so it doesn't span the entire modal width. */
    ERNode* dismiss = make_button("DISMISS", on_modal_close);
    ERProps dp2 = props_default();
    dp2.height = dp(36);
    dp2.width = dp(160);
    dp2.align_self = ER_ALIGN_CENTER;
    dp2.background_color = 0xFFE94560;
    dp2.border_radius = dp(6);
    dp2.align_items = ER_ALIGN_STRETCH;
    dp2.justify_content = ER_JUSTIFY_CENTER;
    er_node_set_props(dismiss, &dp2);
    er_tree_append_child(modal, dismiss);
    er_tree_append_child(root, modal);

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

    SDL_Window* window = SDL_CreateWindow("embedded-react  engine demo",
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

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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

    /* Start looping transform / opacity animations for Panel 5.
     * Each animates to a target and ping-pongs back on the next half-cycle. */
    if (s_xform_translate)
        anim_loop(s_xform_translate, ER_PROP_TRANSLATE_X, (float)dp(36), 700);
    if (s_xform_rotate)
        anim_loop(s_xform_rotate, ER_PROP_ROTATE_Z, 45.0f, 900);
    if (s_xform_scale)
    {
        anim_loop(s_xform_scale, ER_PROP_SCALE_X, 1.25f, 800);
        anim_loop(s_xform_scale, ER_PROP_SCALE_Y, 1.25f, 800);
    }
    if (s_opacity_node)
        anim_loop(s_opacity_node, ER_PROP_OPACITY, 0.15f, 1100);

    bool running = true;
    uint32_t prev = SDL_GetTicks();

    SDL_StartTextInput();

    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
                running = false;
            if (ev.type == SDL_KEYDOWN)
            {
                /* ESC dismisses the modal first; only quits when nothing is open. */
                if (ev.key.keysym.sym == SDLK_ESCAPE)
                {
                    if (s_modal_visible)
                        on_modal_close(NULL, NULL, NULL);
                    else
                        running = false;
                }
                else if (ev.key.keysym.sym == SDLK_d)
                {
                    /* Toggle dirty-rect diagnostic overlay (yellow outline). */
                    s_dirty_overlay_on = !s_dirty_overlay_on;
                    er_sdl_set_show_dirty_rect(s_dirty_overlay_on, s_dpi_scale);
                }
                else if (ev.key.keysym.sym == SDLK_BACKSPACE)
                    embedded_renderer_key(ER_KEY_BACKSPACE, NULL);
                else if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER)
                    embedded_renderer_key(ER_KEY_RETURN, NULL);
            }
            if (ev.type == SDL_TEXTINPUT)
            {
                /* SDL gives the typed UTF-8 character(s) here. */
                embedded_renderer_key(0, ev.text.text);
            }
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
