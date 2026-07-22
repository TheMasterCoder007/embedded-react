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

#include "er_node_internal.h"
#include "gradient.h"
#include "image_scaler.h"
#include "layout_anim.h"
#include "layout_engine.h"
#include "native_renderer.h"
#include "renderer_internal.h"
#include "rrect.h"
#include "scratch_pool.h"
#include "shadow.h"
#include "text_renderer.h"
#include "transform.h"
#include "vector.h"
#include <math.h>
#include <string.h>

#ifndef ERUI_MAX_NODES
#define ERUI_MAX_NODES 512
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERNode s_nodes[ERUI_MAX_NODES];
static uint16_t s_next_tag = 0;
static uint16_t s_free_list[ERUI_MAX_NODES]; /**< LIFO stack of destroyed node slots available for reuse. */
static uint16_t s_free_count = 0;            /**< Number of entries currently in s_free_list. */
static uint16_t s_root_tag = ER_INVALID_TAG;
static uint32_t s_now_ms = 0;
static uint16_t s_focused_input_tag = ER_INVALID_TAG; /**< Currently focused TextInput node. */
static uint8_t s_last_cursor_phase = 2U; /**< Last cursor blink phase (0/1) seen by er_commit; 2 = unknown. */
static bool s_kbd_dirty =
    false;                      /**< Set when the keyboard shows/hides/switches layer so the next commit repaints it. */
static uint8_t s_kbd_layer = 0; /**< Active key layer index into the (default or app-supplied) layout. */
static int s_kbd_avoid_y = 0;   /**< Pixels the whole scene is shifted UP so a focused input clears the keyboard
                                     (0 when no keyboard / input already visible). Applied in render_tree +
                                     node_screen_rect so render and damage stay in sync. */

/* Dirty-rect tracking: union of all screen rects repainted during the current er_commit(). */
static ERRect s_dirty_rect;
static bool s_has_dirty = false;

/* Damage-clipped rendering: when true the next commit repaints the whole screen (first frame, an
 * invalidated framebuffer, or a new root). Otherwise render_tree is scissored to just the rects that
 * actually changed, so a small animation flushes a small region instead of all 800x480. */
static bool s_force_full_repaint = true;

/* Pending damage from removed/destroyed nodes (their last painted rects): merged into the next
 * commit's clip so the vacated pixels are erased without a full-screen repaint. */
static ERRect s_removed_damage;
static bool s_have_removed_damage = false;

/* Layout-dirty gate: er_commit() re-runs the flex + text-measure layout pass only when
 * something that can change a computed rect has happened since the last commit (a prop set,
 * a tree mutation, or a node destroy). Animations mutate render-only props and never set this,
 * so a static or purely animation-driven frame skips the entire layout pass. Initialised true
 * so the first commit always lays out. */
static bool s_layout_dirty = true;

/* Diagnostic: number of times the layout pass has actually run inside er_commit(). Exposed via
 * er_layout_pass_count() so callers (and tests) can confirm static frames skip layout. */
static uint32_t s_layout_pass_count = 0;

/* Multi-buffer (page-flip) damage tracking. A buffer's outstanding "debt": the pixels it has not yet
 * absorbed — either a single bounding rect (has), the whole screen (full), or nothing (already current). */
typedef struct
{
    ERRect rect;
    bool has;
    bool full;
} ERDamageSlot;

/* Number of rotating display buffers the host page-flips (1 = single persistent framebuffer). */
static int s_display_buffer_count = 1;

/* Per-buffer damage debt. Every commit's own damage is unioned into ALL buffers' debt; the buffer being
 * rendered this commit is repainted with its debt (bringing it current) and its debt is then cleared.
 * er_display_present() advances s_cur_buf, so a buffer accumulates every commit that lands while some
 * OTHER buffer is being shown — exactly what it must replay when it next comes around. This is robust to
 * any commit:present cadence (React drives commits in bursts; the host drives presents), unlike a
 * commit-counting ring. Index [0] is the sole slot used when count == 1 (pure incremental). */
static ERDamageSlot s_buf_debt[ER_DISPLAY_BUFFERS_MAX];

/* Index of the buffer currently being rendered; advanced by er_display_present() at each page-flip. */
static int s_cur_buf = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Fills an ERLayoutSpec with Yoga-compatible defaults (all fields auto).
 *
 * @param[in,out] L  Layout spec to initialise.
 */
static void init_layout_defaults(ERLayoutSpec* L)
{
    L->left = L->top = L->right = L->bottom = ER_LAYOUT_AUTO;
    L->width = L->height = ER_LAYOUT_AUTO;
    L->min_width = L->max_width = ER_LAYOUT_AUTO;
    L->min_height = L->max_height = ER_LAYOUT_AUTO;
    L->padding = ER_LAYOUT_AUTO;
    L->padding_left = L->padding_top = L->padding_right = L->padding_bottom = ER_LAYOUT_AUTO;
    L->margin = ER_LAYOUT_AUTO;
    L->margin_left = L->margin_top = L->margin_right = L->margin_bottom = ER_LAYOUT_AUTO;
    L->gap = L->row_gap = L->column_gap = ER_LAYOUT_AUTO;
    L->flex_grow = 0;
    L->flex_shrink = 0;
    L->flex_basis = ER_LAYOUT_AUTO;
    L->flex_direction = ER_FLEX_COL;
    L->flex_wrap = ER_WRAP_NOWRAP;
    L->align_items = ER_ALIGN_STRETCH;
    L->align_self = ER_ALIGN_AUTO;
    L->align_content = ER_ALIGN_CONTENT_FLEX_START;
    L->justify_content = ER_JUSTIFY_FLEX_START;
    L->position = ER_POS_RELATIVE;
    L->aspect_ratio = 0.0f;
    L->flex_basis_pct = 0.0f;
    L->width_pct = 0.0f;
    L->height_pct = 0.0f;
}

/**
 * @brief Collects child tags into an array in append order.
 *
 * @param[in] parent    Parent node whose children should be collected.
 * @param[out] tags     Output child tag buffer.
 * @param[in] max_tags  Capacity of tags.
 *
 * @return Number of child tags written.
 */
static int collect_children(const ERNode* parent, uint16_t* tags, int max_tags)
{
    int count = 0;
    uint16_t child_tag = parent->first_child_tag;

    while (child_tag != ER_INVALID_TAG && count < max_tags)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;

        tags[count++] = child_tag;
        child_tag = child->next_sibling_tag;
    }

    return count;
}

/**
 * @brief Sorts child tags by zIndex while preserving append order for equal zIndex.
 *
 * @param[in,out] tags   Child tag array to sort.
 * @param[in] count      Number of tags in the array.
 */
static void sort_children_by_z_index(uint16_t* tags, int count)
{
    for (int i = 1; i < count; i++)
    {
        const uint16_t key = tags[i];
        const ERNode* key_node = er_get_node(key);
        const int16_t key_z = key_node ? key_node->z_index : 0;
        int j = i - 1;

        while (j >= 0)
        {
            const ERNode* node = er_get_node(tags[j]);
            const int16_t z = node ? node->z_index : 0;
            if (z <= key_z)
                break;
            tags[j + 1] = tags[j];
            j--;
        }

        tags[j + 1] = key;
    }
}

/**
 * @brief Linearly interpolates a single ARGB8888 color channel.
 *
 * @param[in] a  From channel value.
 * @param[in] b  To channel value.
 * @param[in] t  Interpolation fraction [0.0, 1.0].
 *
 * @return Interpolated channel value.
 */
static uint8_t lerp_ch(uint8_t a, uint8_t b, float t)
{
    return (uint8_t)((float)a + ((float)b - (float)a) * t + 0.5f);
}

/**
 * @brief Linearly interpolates between two ARGB8888 colors per-channel.
 *
 * @param[in] a  From color.
 * @param[in] b  To color.
 * @param[in] t  Interpolation fraction [0.0, 1.0].
 *
 * @return Interpolated ARGB8888 color.
 */
static uint32_t lerp_color32(uint32_t a, uint32_t b, float t)
{
    const uint8_t fa = lerp_ch((uint8_t)(a >> 24), (uint8_t)(b >> 24), t);
    const uint8_t fr = lerp_ch((uint8_t)(a >> 16), (uint8_t)(b >> 16), t);
    const uint8_t fg = lerp_ch((uint8_t)(a >> 8), (uint8_t)(b >> 8), t);
    const uint8_t fb = lerp_ch((uint8_t)a, (uint8_t)b, t);
    return ((uint32_t)fa << 24) | ((uint32_t)fr << 16) | ((uint32_t)fg << 8) | fb;
}

/**
 * @brief Renders an ActivityIndicator as a ring of 8 fading dots.
 *
 * The current spin angle is read from n->tp_rotate_z (driven by a looping rotate_z
 * animation started when animating=1).  Each dot's opacity fades from the leading
 * dot (full) to the trailing dot (~15%), creating the classic spinner illusion.
 *
 * @param[in] n   ActivityIndicator node to render.
 * @param[in] px  Left edge of the node in framebuffer pixels.
 * @param[in] py  Top edge of the node in framebuffer pixels.
 * @param[in] w   Width of the node in pixels.
 * @param[in] h   Height of the node in pixels.
 */
static void render_activity_indicator(const ERNode* n, int px, int py, int w, int h)
{
#define ACTIND_DOT_COUNT 8
    const int dia = w < h ? w : h;
    const int dot_size = dia / 5;
    if (dot_size < 1)
        return;

    const int cx = px + w / 2;
    const int cy = py + h / 2;
    const int ring_r = dia / 2 - dot_size / 2 - 2;

    const uint32_t base_color = n->props.act.color ? n->props.act.color : 0xFFFFFFFFU;
    const float base_a = (float)((base_color >> 24) & 0xFFU);
    const float angle_offset_deg = n->tp_rotate_z;

    for (int i = 0; i < ACTIND_DOT_COUNT; i++)
    {
        const float angle_deg = angle_offset_deg + (float)i * (360.0f / ACTIND_DOT_COUNT);
        const float angle_rad = angle_deg * (float)(3.14159265358979323846 / 180.0);
        const int dot_cx = cx + (int)((float)ring_r * cosf(angle_rad) + 0.5f);
        const int dot_cy = cy + (int)((float)ring_r * sinf(angle_rad) + 0.5f);
        const int dot_px = dot_cx - dot_size / 2;
        const int dot_py = dot_cy - dot_size / 2;

        /* Leading dot (i==0) is full opacity; trailing dots fade to ~15%. */
        const float fade = 1.0f - (float)i / ACTIND_DOT_COUNT * 0.85f;
        const uint8_t alpha = (uint8_t)(base_a * fade + 0.5f);
        const uint32_t dot_color = ((uint32_t)alpha << 24) | (base_color & 0x00FFFFFFu);

        er_rrect_fill_bordered(dot_color, 0x00000000U, 0, dot_px, dot_py, dot_size, dot_size, dot_size / 2);
    }
#undef ACTIND_DOT_COUNT
}

/**
 * @brief Expands the per-commit dirty rectangle to include the given screen-space rect.
 *
 * Called from render_tree() whenever a node is actually repainted. The union grows
 * monotonically within each commit and is reset at the start of the next one.
 *
 * @param[in] x  Left edge of the repainted region in framebuffer pixels.
 * @param[in] y  Top edge of the repainted region in framebuffer pixels.
 * @param[in] w  Width of the repainted region in pixels.
 * @param[in] h  Height of the repainted region in pixels.
 */
static void union_dirty_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    if (!s_has_dirty)
    {
        s_dirty_rect.x = x;
        s_dirty_rect.y = y;
        s_dirty_rect.w = w;
        s_dirty_rect.h = h;
        s_has_dirty = true;
        return;
    }

    const int x2 = s_dirty_rect.x + s_dirty_rect.w;
    const int y2 = s_dirty_rect.y + s_dirty_rect.h;
    const int nx2 = x + w;
    const int ny2 = y + h;

    if (x < s_dirty_rect.x)
        s_dirty_rect.x = x;
    if (y < s_dirty_rect.y)
        s_dirty_rect.y = y;
    s_dirty_rect.w = (nx2 > x2 ? nx2 : x2) - s_dirty_rect.x;
    s_dirty_rect.h = (ny2 > y2 ? ny2 : y2) - s_dirty_rect.y;
}

/**
 * @brief Marks a node and all ancestors dirty so stale child pixels are repainted.
 *
 * The renderer currently paints into a persistent framebuffer. If a child changes
 * shape or text, the parent background must be redrawn before the child is painted
 * again, otherwise old pixels can remain visible.
 *
 * @param[in,out] node  Node whose ancestor chain should be invalidated.
 */
void er_force_full_repaint(void)
{
    s_force_full_repaint = true;
}

/* Mark every rotating buffer as owing a full-screen repaint (first frame, reset, or a full-repaint
 * request): each buffer repaints fully the next time it is rendered, then reverts to incremental. */
static void debt_reset_all_full(void)
{
    for (int i = 0; i < ER_DISPLAY_BUFFERS_MAX; i++)
    {
        s_buf_debt[i].has = false;
        s_buf_debt[i].full = true;
        s_buf_debt[i].rect.x = s_buf_debt[i].rect.y = s_buf_debt[i].rect.w = s_buf_debt[i].rect.h = 0;
    }
}

void er_set_display_buffer_count(int n)
{
    if (n < 1)
        n = 1;
    if (n > ER_DISPLAY_BUFFERS_MAX)
        n = ER_DISPLAY_BUFFERS_MAX;
    s_display_buffer_count = n;
    s_cur_buf = 0;
    debt_reset_all_full(); /* every rotating buffer must start from a full frame */
}

int er_get_display_buffer_count(void)
{
    return s_display_buffer_count;
}

void er_display_present(void)
{
    /* One page-flip has occurred: the next commit renders the next rotating buffer. Wrapping by the
     * buffer count keeps s_cur_buf a valid debt index; for count == 1 this is a no-op. */
    if (s_display_buffer_count > 1)
        s_cur_buf = (s_cur_buf + 1) % s_display_buffer_count;
}

/* Global content generation: advanced by every mutation that changes rendered subtree content
 * (er_mark_dirty_upward and friends). The fade cache stores the generation it captured at; any
 * bump invalidates it. Deliberately coarse — a content change anywhere invalidates the one
 * cached subtree — which keeps the check O(1) and always correct. */
static uint32_t s_content_gen = 1U;

void er_mark_dirty_upward_visual(ERNode* node)
{
    /* Mark only the initiating node as source_dirty so the dirty-rect
     * accumulator can track which pixels actually changed, not which
     * ancestors were incidentally re-rendered to clear the background. */
    if (node)
        node->source_dirty = true;

    while (node)
    {
        node->dirty = true;
        node = er_get_node(node->parent_tag);
    }
}

void er_mark_dirty_upward(ERNode* node)
{
    s_content_gen++;
    er_mark_dirty_upward_visual(node);
}

/**
 * @brief Flags the scene as needing a layout pass on the next er_commit().
 *
 * Called from every mutation that can change a node's computed rect: prop sets, text-span
 * changes, tree append/remove, root changes, and node destruction. Render-only mutations
 * (animations, scroll offset, cursor blink) deliberately do NOT call this, so frames that
 * only repaint without moving anything skip the layout pass entirely.
 */
static void mark_layout_dirty(void)
{
    s_layout_dirty = true;
}

/**
 * @brief Renders the background and border of a View-family node.
 *
 * Resolves per-corner radii, per-edge widths/colors, and border style from ERViewProps,
 * then dispatches to er_rrect_fill_bordered (fast uniform path) or the general
 * per-corner/per-edge path using er_rrect_fill_corners and er_rrect_border_edge.
 *
 * @param[in] vp  Visual properties of the node.
 * @param[in] px  Left edge of the node in framebuffer pixels (after scroll offset).
 * @param[in] py  Top edge of the node in framebuffer pixels.
 * @param[in] w   Node width in pixels.
 * @param[in] h   Node height in pixels.
 */
static void render_view_bg(const ERViewProps* vp, int px, int py, int w, int h)
{
    /* Resolve per-corner radii (0 = fall back to uniform border_radius). */
    const int r_tl = vp->border_tl_radius > 0 ? (int)vp->border_tl_radius : (int)vp->border_radius;
    const int r_tr = vp->border_tr_radius > 0 ? (int)vp->border_tr_radius : (int)vp->border_radius;
    const int r_br = vp->border_br_radius > 0 ? (int)vp->border_br_radius : (int)vp->border_radius;
    const int r_bl = vp->border_bl_radius > 0 ? (int)vp->border_bl_radius : (int)vp->border_radius;

    /* Resolve per-edge widths (0 = fall back to uniform border_width). */
    const int bw_l = vp->border_left_width > 0 ? (int)vp->border_left_width : (int)vp->border_width;
    const int bw_t = vp->border_top_width > 0 ? (int)vp->border_top_width : (int)vp->border_width;
    const int bw_r = vp->border_right_width > 0 ? (int)vp->border_right_width : (int)vp->border_width;
    const int bw_b = vp->border_bottom_width > 0 ? (int)vp->border_bottom_width : (int)vp->border_width;

    /* Resolve per-edge colors (0 = fall back to uniform border_color). */
    const uint32_t bc_l = vp->border_left_color ? vp->border_left_color : vp->border_color;
    const uint32_t bc_t = vp->border_top_color ? vp->border_top_color : vp->border_color;
    const uint32_t bc_r = vp->border_right_color ? vp->border_right_color : vp->border_color;
    const uint32_t bc_b = vp->border_bottom_color ? vp->border_bottom_color : vp->border_color;

    /* When a gradient is active (type != NONE and at least 2 stops) it fills the background;
     * skip background_color so gradient pixels remain visible beneath the border. */
#if ERUI_GRADIENT
    const uint32_t bg_color =
        (vp->gradient_type != ER_GRADIENT_NONE && vp->gradient_stop_count >= 2u) ? 0u : vp->background_color;
#else
    const uint32_t bg_color = vp->background_color;
#endif

    /* Fast path: uniform border width, color, radius, and solid style. */
    if (bw_l == bw_t && bw_t == bw_r && bw_r == bw_b && bc_l == bc_t && bc_t == bc_r && bc_r == bc_b && r_tl == r_tr
        && r_tr == r_br && r_br == r_bl && vp->border_style == 0)
    {
        er_rrect_fill_bordered(bg_color, bc_l, bw_l, px, py, w, h, r_tl);
        return;
    }

    const bool has_border = (bw_l > 0 || bw_t > 0 || bw_r > 0 || bw_b > 0);
    const bool uniform_bw = (bw_l == bw_t && bw_t == bw_r && bw_r == bw_b);
    const bool uniform_bc = (bc_l == bc_t && bc_t == bc_r && bc_r == bc_b);

    if (has_border && uniform_bw && uniform_bc && vp->border_style == 0 && (bc_l >> 24))
    {
        /* Uniform solid border with per-corner radii: outer → background inset. */
        er_rrect_fill_corners(bc_l, px, py, w, h, r_tl, r_tr, r_br, r_bl);
        const int ix = px + bw_l;
        const int iy = py + bw_t;
        const int iw = w - bw_l - bw_r;
        const int ih = h - bw_t - bw_b;
        /* Inset radii: shrink each corner by the adjacent border width. */
        const int ir_tl = (r_tl > bw_l) ? r_tl - bw_l : 0;
        const int ir_tr = (r_tr > bw_r) ? r_tr - bw_r : 0;
        const int ir_br = (r_br > bw_r) ? r_br - bw_r : 0;
        const int ir_bl = (r_bl > bw_l) ? r_bl - bw_l : 0;
        if (iw > 0 && ih > 0)
            er_rrect_fill_corners(bg_color, ix, iy, iw, ih, ir_tl, ir_tr, ir_br, ir_bl);
    }
    else
    {
        /* Per-edge or styled borders: fill background shape first, then overlay each edge. */
        er_rrect_fill_corners(bg_color, px, py, w, h, r_tl, r_tr, r_br, r_bl);
        if (bw_t > 0 && (bc_t >> 24))
            er_rrect_border_edge(bc_t, vp->border_style, px, py, w, bw_t, 1);
        if (bw_b > 0 && (bc_b >> 24))
            er_rrect_border_edge(bc_b, vp->border_style, px, py + h - bw_b, w, bw_b, 1);
        if (bw_l > 0 && (bc_l >> 24))
            er_rrect_border_edge(bc_l, vp->border_style, px, py + bw_t, bw_l, h - bw_t - bw_b, 0);
        if (bw_r > 0 && (bc_r >> 24))
            er_rrect_border_edge(bc_r, vp->border_style, px + w - bw_r, py + bw_t, bw_r, h - bw_t - bw_b, 0);
    }
}

/**
 * @brief Grows an accumulator rect to include (x,y,w,h). Skips empty inputs; seeds on first union.
 *
 * @param[in,out] acc   Accumulator rectangle.
 * @param[in,out] have  Whether @p acc has been seeded yet.
 * @param[in]     x,y,w,h  Rectangle to merge in.
 */
static void damage_union(ERRect* acc, bool* have, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (!*have)
    {
        acc->x = x;
        acc->y = y;
        acc->w = w;
        acc->h = h;
        *have = true;
        return;
    }
    const int x2 = acc->x + acc->w;
    const int y2 = acc->y + acc->h;
    const int nx2 = x + w;
    const int ny2 = y + h;
    if (x < acc->x)
        acc->x = x;
    if (y < acc->y)
        acc->y = y;
    acc->w = (nx2 > x2 ? nx2 : x2) - acc->x;
    acc->h = (ny2 > y2 ? ny2 : y2) - acc->y;
}

/**
 * @brief Accumulates a removed subtree's last-painted rects into the pending removal damage.
 *
 * Called while the subtree is still intact (before detach). The next er_commit() merges this into its
 * clip so the vacated pixels are repainted (erased) without forcing a full-screen redraw.
 *
 * @param[in] n  Root of the subtree being removed.
 */
static void note_removed_subtree(const ERNode* n)
{
    if (!n)
        return;
    if (n->has_last_paint)
        damage_union(&s_removed_damage,
                     &s_have_removed_damage,
                     (int)n->last_paint_rect.x,
                     (int)n->last_paint_rect.y,
                     (int)n->last_paint_rect.w,
                     (int)n->last_paint_rect.h);
    uint16_t c = n->first_child_tag;
    while (c != ER_INVALID_TAG)
    {
        const ERNode* ch = er_get_node(c);
        if (!ch)
            break;
        note_removed_subtree(ch);
        c = ch->next_sibling_tag;
    }
}

/**
 * @brief Computes a node's current screen rect for damage tracking.
 *
 * Mirrors render_tree's position math: absolute layout position minus accumulated ancestor scroll,
 * plus the translate-transform offset. Returns false for non-translate transforms (rotate/scale/3D)
 * whose painted bounding box this fast path can't reproduce — the caller then repaints in full.
 *
 * @param[in]  n             Node to measure.
 * @param[out] rx,ry,rw,rh   Receive the node's screen rectangle.
 *
 * @return true if the rect was computed; false if the node needs a full repaint instead.
 */
static bool node_screen_rect(const ERNode* n, int* rx, int* ry, int* rw, int* rh)
{
#if ERUI_TRANSFORMS_FULL
    if (n->has_transform && !er_transform_is_translate_only(n))
        return false;
#endif
    int sx = 0;
    int sy = 0;
    const ERNode* a = er_get_node(n->parent_tag);
    while (a)
    {
        if (a->type == ER_NODE_SCROLL_VIEW || a->type == ER_NODE_FLAT_LIST)
        {
            sx += (int)a->scroll_offset_x;
            sy += (int)a->scroll_offset_y;
        }
        a = er_get_node(a->parent_tag);
    }
    *rx = (int)n->animated.x - sx + (int)n->tp_translate_x;
    *ry = (int)n->animated.y - sy + (int)n->tp_translate_y - s_kbd_avoid_y;
    *rw = (int)n->animated.w;
    *rh = (int)n->animated.h;
    return true;
}

/**
 * @brief Computes the screen-space AABB of a node under a full transform (2D affine or 3D/perspective).
 *
 * Mirrors render_tree's transform math — same pre-transform origin (layout position minus accumulated
 * ancestor scroll, minus the keyboard-avoidance shift), then the matching projection: 2D scale/rotate via
 * er_transform_compute_matrix + er_transform_aabb, or 3D/perspective via er_transform_compute_homography_3d
 * + er_transform_aabb_3d — so the damage pre-pass can bound an animated transform to its own box instead of
 * falling back to a full-screen repaint. tp_translate_x/y is folded into the matrix/homography, so it is
 * NOT added to the origin here.
 *
 * Returns false (leaving the caller on its full-repaint fallback) for the cases this can't bound: builds
 * without the affine path, translate-only nodes (handled by node_screen_rect), the ActivityIndicator
 * (whose rotate_z is an internal spin, not an affine render), or a degenerate / off-screen-projected
 * (zero-area) result — including a 3D node whose corners all fall behind the perspective plane.
 *
 * @param[in]  n             Node to measure.
 * @param[out] rx,ry,rw,rh   Receive the node's transformed screen AABB.
 *
 * @return true if a finite AABB was produced; false to fall back to a full repaint.
 */
static bool node_transformed_screen_rect(const ERNode* n, int* rx, int* ry, int* rw, int* rh)
{
#if ERUI_TRANSFORMS_FULL
    if (!n->has_transform || er_transform_is_translate_only(n) || n->type == ER_NODE_ACTIVITY_INDICATOR)
        return false;
    int sx = 0;
    int sy = 0;
    const ERNode* a = er_get_node(n->parent_tag);
    while (a)
    {
        if (a->type == ER_NODE_SCROLL_VIEW || a->type == ER_NODE_FLAT_LIST)
        {
            sx += (int)a->scroll_offset_x;
            sy += (int)a->scroll_offset_y;
        }
        a = er_get_node(a->parent_tag);
    }
    const int px = (int)n->animated.x - sx;
    const int py = (int)n->animated.y - sy - s_kbd_avoid_y;
    const int w = (int)n->animated.w;
    const int h = (int)n->animated.h;
#if ERUI_3D_TRANSFORMS
    if (er_transform_is_3d(n))
    {
        /* 3D/perspective: project the node's box through the same homography render_tree paints with,
         * so the damage bounds exactly match the painted dst rect. A node whose corners all fall behind
         * the perspective plane yields a zero-area AABB → false → full-repaint fallback. */
        float H[9];
        er_transform_compute_homography_3d(n, px, py, w, h, H);
        er_transform_aabb_3d(px, py, w, h, H, rx, ry, rw, rh);
        return (*rw > 0 && *rh > 0);
    }
#endif
    float ma, mb, mc, md, mtx, mty;
    er_transform_compute_matrix(n, px, py, w, h, &ma, &mb, &mc, &md, &mtx, &mty);
    er_transform_aabb(px, py, w, h, ma, mb, mc, md, mtx, mty, rx, ry, rw, rh);
    return (*rw > 0 && *rh > 0);
#else
    (void)n;
    (void)rx;
    (void)ry;
    (void)rw;
    (void)rh;
    return false;
#endif
}

/**
 * @brief Intersects a node's screen rect with every clipping ancestor's box (ScrollView / overflow:hidden).
 *
 * A scrolled child's screen rect (node_screen_rect) is the UN-clipped position, so a row scrolled near a
 * ScrollView's edge reaches outside the list (e.g. over a title above it). Clipping its DAMAGE contribution
 * to the clippers keeps a scroll's dirty region inside the list, so siblings outside it aren't dragged into
 * the repaint and momentarily cleared. (node_screen_rect itself stays un-clipped so move-detection holds.)
 * Sets w/h to 0 when fully outside a clipper.
 */
static void clip_rect_to_clippers(const ERNode* n, int* rx, int* ry, int* rw, int* rh)
{
    int x0 = *rx, y0 = *ry, x1 = *rx + *rw, y1 = *ry + *rh;
    const ERNode* a = er_get_node(n->parent_tag);
    while (a)
    {
        const bool clips = (a->layout.overflow == ER_OVERFLOW_HIDDEN || a->layout.overflow == ER_OVERFLOW_SCROLL
                            || a->type == ER_NODE_SCROLL_VIEW || a->type == ER_NODE_FLAT_LIST);
        int ax, ay, aw, ah;
        if (clips && node_screen_rect(a, &ax, &ay, &aw, &ah))
        {
            if (ax > x0)
                x0 = ax;
            if (ay > y0)
                y0 = ay;
            if (ax + aw < x1)
                x1 = ax + aw;
            if (ay + ah < y1)
                y1 = ay + ah;
        }
        a = er_get_node(a->parent_tag);
    }
    *rx = x0;
    *ry = y0;
    *rw = (x1 > x0) ? (x1 - x0) : 0;
    *rh = (y1 > y0) ? (y1 - y0) : 0;
}

/* Set per-commit: true when the cached subtree bounds are trustworthy this frame (no layout
 * animation interpolating positions), so render_tree() may use them to skip untouched subtrees. */
static bool s_prune_ok = false;

/**
 * @brief Recomputes cached subtree paint bounds for the whole tree (post-order).
 *
 * For each node, sub_{x,y,w,h} becomes the union of the node's own computed box and the paint
 * bounds of every descendant that can draw outside it. A node that clips its children (overflow
 * hidden/scroll, ScrollView, FlatList) bounds them to its own box, so its subtree bounds collapse
 * to that box no matter where the children lay out. subtree_prunable is cleared whenever the
 * subtree contains a transform — whose scratch-rendered output is not captured by the computed
 * box — so render_tree() never prunes such a subtree.
 *
 * Bounds are in computed (pre-scroll) space; render_tree() subtracts the running scroll
 * translation before testing them against the damage clip. Refreshed only on layout commits;
 * during a static drag the previous layout's bounds remain valid because positions do not move.
 *
 * @param[in,out] n  Node whose subtree bounds to (re)compute.
 */
static void compute_subtree_bounds(ERNode* n)
{
    if (!n)
        return;

    int x0 = n->computed.x;
    int y0 = n->computed.y;
    int x1 = x0 + n->computed.w;
    int y1 = y0 + n->computed.h;
    bool prunable = !n->has_transform;

    const bool clips = (n->layout.overflow == ER_OVERFLOW_HIDDEN || n->layout.overflow == ER_OVERFLOW_SCROLL
                        || n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_FLAT_LIST);

    uint16_t child_tag = n->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* c = er_get_node(child_tag);
        if (!c)
            break;
        compute_subtree_bounds(c);
        if (!clips)
        {
            /* Non-clipping parent: a child may paint past this box, so grow to cover its subtree. */
            if (c->sub_x < x0)
                x0 = c->sub_x;
            if (c->sub_y < y0)
                y0 = c->sub_y;
            if (c->sub_x + c->sub_w > x1)
                x1 = c->sub_x + c->sub_w;
            if (c->sub_y + c->sub_h > y1)
                y1 = c->sub_y + c->sub_h;
            if (!c->subtree_prunable)
                prunable = false;
        }
        child_tag = c->next_sibling_tag;
    }

    n->sub_x = (int16_t)x0;
    n->sub_y = (int16_t)y0;
    n->sub_w = (int16_t)(x1 - x0);
    n->sub_h = (int16_t)(y1 - y0);
    n->subtree_prunable = prunable;
}

/* Banded opacity compositing: when a composite region exceeds one scratch strip, the subtree
 * is re-rendered once per strip-sized tile. The tile currently being composited is recorded
 * here (screen space) so the subtree-bounds prune can skip untransformed subtrees that miss
 * it, and so nested composites can bound their own region to the tile. It is deliberately
 * NOT a clip-stack entry — clipping the walk would truncate nested transform-source captures
 * at the seam (same rule as the ER_LCD_BANDED backend band). */
static bool s_tile_active = false;
static int s_tile_x = 0;
static int s_tile_y = 0;
static int s_tile_w = 0;
static int s_tile_h = 0;

/* Banded-pass bookkeeping for the transform source cache: each band loop gets a generation,
 * and only its FIRST tile captures a transform subtree — later tiles replay the emit from the
 * cached source (see er_transform_source_is_cached), skipping the subtree re-render. */
static uint32_t s_band_gen_counter = 0;
static uint32_t s_cur_loop_gen = 0;
static bool s_tile_first = false;

/* Fade cache: the composited subtree of ONE translucent group, kept across commits. During a
 * pure opacity animation the subtree's content is identical every frame — only the blend alpha
 * changes — so after a first full capture, each frame is a single blend of this buffer at the
 * new alpha instead of a re-render + re-composite. Valid while the owning node, its size, and
 * the global content generation are unchanged; ANY content mutation anywhere invalidates it
 * (coarse but O(1) and always safe). Sized by ERUI_FADE_CACHE_W/H; 0 disables the feature. */
#ifndef ERUI_FADE_CACHE_W
#define ERUI_FADE_CACHE_W 0
#endif
#ifndef ERUI_FADE_CACHE_H
#define ERUI_FADE_CACHE_H 0
#endif
#define ER_FADE_CACHE_ENABLED (ERUI_FADE_CACHE_W > 0 && ERUI_FADE_CACHE_H > 0)
#if ER_FADE_CACHE_ENABLED
static uint32_t s_fade_cache[(size_t)ERUI_FADE_CACHE_H * ERUI_FADE_CACHE_W];
static uint16_t s_fade_cache_tag = 0xFFFFu; /* owning node; 0xFFFF = invalid */
static uint32_t s_fade_cache_gen = 0;       /* s_content_gen at capture */
static int s_fade_cache_w = 0;
static int s_fade_cache_h = 0;
static uint32_t s_fade_capture_commit = 0; /* anti-thrash: at most one capture per commit */
#endif
static uint32_t s_commit_seq = 0;

/** @brief Drops the fade cache (owner destroyed, scene reset, or tag about to be recycled). */
static void fade_cache_invalidate(void)
{
#if ER_FADE_CACHE_ENABLED
    s_fade_cache_tag = 0xFFFFu;
#endif
}

#if ER_PROF
/* TEMP on-device profiling: phase accumulators printed every 30 commits (host provides the clock). */
#include <stdio.h>
extern uint32_t er_prof_now_us(void);
static uint32_t s_prof_content_us = 0;  /* subtree render time inside composites */
static uint32_t s_prof_blend_us = 0;    /* strip pop_blend time */
static uint32_t s_prof_push_us = 0;     /* strip push (clear) time */
static uint32_t s_prof_passes = 0;      /* band passes */
static uint32_t s_prof_composites = 0;  /* composite_with_opacity calls that composited */
#define ER_PROF_MARK(var) const uint32_t var = er_prof_now_us()
#define ER_PROF_ACC(acc, from) (acc += er_prof_now_us() - (from))
#else
#define ER_PROF_MARK(var)
#define ER_PROF_ACC(acc, from)
#endif

static void render_tree(ERNode* n, bool parent_dirty, int translate_x, int translate_y);

/**
 * @brief Renders a node's own content and recurses into its children.
 *
 * This is the repeatable "body" of render_tree: the per-type draw switch, the overflow
 * scissor push, and the z-sorted child recursion. It has no side effects on n's dirty
 * flags (except one-shot consumption of vec_has_dirty by the vector rasterizer), so the
 * banded opacity compositor can invoke it once per strip tile.
 *
 * @param[in] n              Node to render.
 * @param[in] should_render  true when the node itself (not only descendants) must repaint.
 * @param[in] px             Screen-space left edge (scroll translation applied).
 * @param[in] py             Screen-space top edge.
 * @param[in] w              Node width in pixels.
 * @param[in] h              Node height in pixels.
 * @param[in] translate_x    Accumulated horizontal scroll offset for children.
 * @param[in] translate_y    Accumulated vertical scroll offset for children.
 */
static void render_node_content(ERNode* n, bool should_render, int px, int py, int w, int h, int translate_x,
                                int translate_y);

/**
 * @brief Composites a translucent subtree through the persistent fade cache when possible.
 *
 * Cache hit: one blend of the previously composited subtree at the current alpha — the whole
 * subtree re-render is skipped. Cache miss (different node, content changed, first frame):
 * captures the subtree once into the cache buffer (full size, unbanded), then blends it.
 *
 * Only engages for a top-level composite (no enclosing band tile, capture, or inherited
 * alpha), when the node fits the cache buffer, and when the active scissor covers the whole
 * node (a partial repaint would capture partial content). At most one capture per commit so
 * two simultaneously-animating fades don't thrash the single cache slot.
 *
 * @return true when the subtree was composited via the cache; false → use the strip pool.
 */
static bool composite_from_cache(ERNode* n, uint8_t alpha, int px, int py, int w, int h, int translate_x,
                                 int translate_y)
{
#if ER_FADE_CACHE_ENABLED
    if (s_tile_active || !er_scratch_idle() || er_get_draw_alpha() != 255U)
        return false;
    if (w <= 0 || h <= 0 || w > ERUI_FADE_CACHE_W || h > ERUI_FADE_CACHE_H)
        return false;
    int cx, cy, cw, ch;
    if (er_get_clip_rect(&cx, &cy, &cw, &ch) && (cx > px || cy > py || cx + cw < px + w || cy + ch < py + h))
        return false;

    if (s_fade_cache_tag == n->tag && s_fade_cache_gen == s_content_gen && s_fade_cache_w == w
        && s_fade_cache_h == h)
    {
        er_blit_blend(s_fade_cache, ERUI_FADE_CACHE_W * (int)sizeof(uint32_t), alpha, px, py, w, h);
        return true;
    }

    if (s_fade_capture_commit == s_commit_seq)
        return false;
    s_fade_capture_commit = s_commit_seq;

    for (int row = 0; row < h; row++)
        memset(s_fade_cache + (size_t)row * ERUI_FADE_CACHE_W, 0, (size_t)w * sizeof(uint32_t));
    er_scratch_push_base(s_fade_cache, ERUI_FADE_CACHE_W, ERUI_FADE_CACHE_H, px, py);
    render_node_content(n, true, px, py, w, h, translate_x, translate_y);
    er_scratch_pop_base();

    s_fade_cache_tag = n->tag;
    s_fade_cache_gen = s_content_gen;
    s_fade_cache_w = w;
    s_fade_cache_h = h;
    er_blit_blend(s_fade_cache, ERUI_FADE_CACHE_W * (int)sizeof(uint32_t), alpha, px, py, w, h);
    return true;
#else
    (void)n;
    (void)alpha;
    (void)px;
    (void)py;
    (void)w;
    (void)h;
    (void)translate_x;
    (void)translate_y;
    return false;
#endif
}

/**
 * @brief Composites a node subtree at a group alpha through the scratch strip pool.
 *
 * The composite region is the node rect intersected with the active scissor and any
 * enclosing composite tile — group opacity is a per-pixel-local operation, so bounding
 * the composite to the visible region is exact, and a small damage clip on a large faded
 * node composites in a single pass. When the region fits one strip this is the classic
 * single push/blend; otherwise the region is walked in strip-sized tiles, re-rendering
 * the subtree once per tile (any node size composites correctly, bounded RAM).
 *
 * @param[in] n            Node whose subtree is composited.
 * @param[in] alpha        Group opacity 0–255.
 * @param[in] px           Screen-space left edge of the node.
 * @param[in] py           Screen-space top edge.
 * @param[in] w            Node width in pixels.
 * @param[in] h            Node height in pixels.
 * @param[in] translate_x  Accumulated scroll offset for children.
 * @param[in] translate_y  Accumulated scroll offset for children.
 *
 * @return true when the subtree was composited; false when no slot was available or the
 *         region is empty (caller falls back to a direct render).
 */
static bool composite_with_opacity(ERNode* n, uint8_t alpha, int px, int py, int w, int h, int translate_x,
                                   int translate_y)
{
    if (!er_scratch_avail())
        return false;

    int rx = px, ry = py, rx1 = px + w, ry1 = py + h;
    int gx, gy, gw, gh;
    if (er_get_clip_rect(&gx, &gy, &gw, &gh))
    {
        if (gx > rx)
            rx = gx;
        if (gy > ry)
            ry = gy;
        if (gx + gw < rx1)
            rx1 = gx + gw;
        if (gy + gh < ry1)
            ry1 = gy + gh;
    }
    if (s_tile_active)
    {
        if (s_tile_x > rx)
            rx = s_tile_x;
        if (s_tile_y > ry)
            ry = s_tile_y;
        if (s_tile_x + s_tile_w < rx1)
            rx1 = s_tile_x + s_tile_w;
        if (s_tile_y + s_tile_h < ry1)
            ry1 = s_tile_y + s_tile_h;
    }
    const int rw = rx1 - rx;
    const int rh = ry1 - ry;
    if (rw <= 0 || rh <= 0)
        return false;

    const int sw = er_scratch_strip_w();
    const int sh = er_scratch_strip_h();

    if (rw <= sw && rh <= sh)
    {
        ER_PROF_MARK(t_push);
        if (!er_scratch_push(rx, ry, rw, rh))
            return false;
        ER_PROF_ACC(s_prof_push_us, t_push);
        /* Scissor to the composite region so rasterisers do region-sized work, not node-sized. */
        er_push_clip_rect(rx, ry, rw, rh);
        ER_PROF_MARK(t_content);
        render_node_content(n, true, px, py, w, h, translate_x, translate_y);
        ER_PROF_ACC(s_prof_content_us, t_content);
        er_pop_clip_rect();
        ER_PROF_MARK(t_blend);
        er_scratch_pop_blend(alpha, rx, ry, rw, rh);
        ER_PROF_ACC(s_prof_blend_us, t_blend);
#if ER_PROF
        s_prof_passes++;
        s_prof_composites++;
#endif
        return true;
    }

    /* Banded pass: walk the region in strip-sized tiles. Each tile is pushed as a REAL clip
     * rect so rasterisers (rrect AA, text, gradients, vector) do strip-sized work per pass —
     * transform source captures are unaffected because er_transform_source_begin pushes a
     * clip reset for the duration of the capture. */
    const bool outer_active = s_tile_active;
    const int ox = s_tile_x, oy = s_tile_y, ow = s_tile_w, oh = s_tile_h;
    const uint32_t outer_gen = s_cur_loop_gen;
    const bool outer_first = s_tile_first;
    s_cur_loop_gen = ++s_band_gen_counter;
    bool first = true;
    for (int by = ry; by < ry1; by += sh)
    {
        const int bh = (ry1 - by) < sh ? (ry1 - by) : sh;
        for (int bx = rx; bx < rx1; bx += sw)
        {
            const int bw = (rx1 - bx) < sw ? (rx1 - bx) : sw;
            ER_PROF_MARK(t_push);
            if (!er_scratch_push(bx, by, bw, bh))
            {
                /* Defensive fallback: avoid leaving holes if the scratch pool is unexpectedly exhausted. */
                const uint8_t saved_alpha = er_get_draw_alpha();
                er_set_draw_alpha((uint8_t)((uint32_t)saved_alpha * alpha / 255U));

                s_tile_active = true;
                s_tile_x = bx;
                s_tile_y = by;
                s_tile_w = bw;
                s_tile_h = bh;
                s_tile_first = first;
                er_push_clip_rect(bx, by, bw, bh);
                render_node_content(n, true, px, py, w, h, translate_x, translate_y);
                er_pop_clip_rect();

                s_tile_active = outer_active;
                s_tile_x = ox;
                s_tile_y = oy;
                s_tile_w = ow;
                s_tile_h = oh;
                er_set_draw_alpha(saved_alpha);

                first = false;
                continue;
            }
            ER_PROF_ACC(s_prof_push_us, t_push);
            s_tile_active = true;
            s_tile_x = bx;
            s_tile_y = by;
            s_tile_w = bw;
            s_tile_h = bh;
            s_tile_first = first;
            er_push_clip_rect(bx, by, bw, bh);
            ER_PROF_MARK(t_content);
            render_node_content(n, true, px, py, w, h, translate_x, translate_y);
            ER_PROF_ACC(s_prof_content_us, t_content);
            er_pop_clip_rect();
            s_tile_active = outer_active;
            s_tile_x = ox;
            s_tile_y = oy;
            s_tile_w = ow;
            s_tile_h = oh;
            ER_PROF_MARK(t_blend);
            er_scratch_pop_blend(alpha, bx, by, bw, bh);
            ER_PROF_ACC(s_prof_blend_us, t_blend);
#if ER_PROF
            s_prof_passes++;
#endif
            first = false;
        }
    }
    s_cur_loop_gen = outer_gen;
    s_tile_first = outer_first;
#if ER_PROF
    s_prof_composites++;
#endif
    return true;
}

/**
 * @brief Recursively renders a node and its children depth-first.
 *
 * translate_x / translate_y accumulate the total scroll offset contributed by all
 * ScrollView ancestors.  Each node's actual screen position is computed by subtracting
 * the accumulated translation from its layout-computed position.
 *
 * overflow:hidden and overflow:scroll nodes push a scissor clip rectangle around their
 * children so that out-of-bounds content is not visible.  ScrollView nodes also add
 * their current scroll offset to the running translation for their children.
 *
 * @param[in] n             Node to render.
 * @param[in] parent_dirty  true when an ancestor was dirty this frame.
 * @param[in] translate_x   Accumulated horizontal scroll offset from ancestor ScrollViews.
 * @param[in] translate_y   Accumulated vertical scroll offset from ancestor ScrollViews.
 */
static void render_tree(ERNode* n, bool parent_dirty, int translate_x, int translate_y)
{
    if (n->layout.display == ER_DISPLAY_NONE)
        return;

    /* Subtree-bounds pruning: if this whole subtree's cached paint bounds fall entirely outside the
     * active damage clip it cannot contribute to the changed region, so skip it (and every descendant)
     * without touching their structs — the win that turns the per-commit walk from O(all nodes) into
     * O(nodes near the change). Only applies when bounds are trustworthy (s_prune_ok), the subtree has
     * no transform (subtree_prunable), and a clip is active (a full repaint pushes none, so this is a
     * no-op then). Bounds are computed-space; subtract the scroll translation to compare in screen space. */
    if (s_prune_ok && n->subtree_prunable)
    {
        const int bx0 = (int)n->sub_x - translate_x;
        const int by0 = (int)n->sub_y - translate_y;
        const int bx1 = bx0 + (int)n->sub_w;
        const int by1 = by0 + (int)n->sub_h;
        int gx, gy, gw, gh;
        if (er_get_clip_rect(&gx, &gy, &gw, &gh))
        {
            if (bx1 <= gx || by1 <= gy || bx0 >= gx + gw || by0 >= gy + gh)
                return;
        }
        /* Banded render: skip subtrees entirely outside the strip currently being emitted. Transformed
         * subtrees are never prunable, so they still render in full each strip — keeping their offscreen
         * scratch source complete across strip seams. */
        int band_oy, band_h;
        if (er_band_active(&band_oy, &band_h) && (by1 <= band_oy || by0 >= band_oy + band_h))
            return;
        /* Banded opacity compositing: skip subtrees entirely outside the strip tile currently being
         * composited (same never-prune-transformed rule as above). */
        if (s_tile_active
            && (bx1 <= s_tile_x || by1 <= s_tile_y || bx0 >= s_tile_x + s_tile_w || by0 >= s_tile_y + s_tile_h))
            return;
    }

    const bool should_render = n->dirty || parent_dirty;

    /* Actual screen position after applying all ancestor scroll offsets.
     * Use node->animated rather than node->computed so that LayoutAnimation
     * transitions show intermediate positions during the animation. */
    int px = n->animated.x - translate_x;
    int py = n->animated.y - translate_y;
    const int w = n->animated.w;
    const int h = n->animated.h;

    /* --- 2D/3D transform application --- */
    bool doing_affine = false;
    bool doing_replay = false;
    bool doing_3d = false;
    int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
    float xf_ia = 1.0f, xf_ib = 0.0f, xf_ic = 0.0f, xf_id = 1.0f, xf_itx = 0.0f, xf_ity = 0.0f;
#if ERUI_3D_TRANSFORMS
    float xf_inv_H[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
#endif

    /* ActivityIndicator uses tp_rotate_z as its internal spin angle — skip the affine
     * transform path which would rasterize the whole node into a scratch buffer. */
    if (n->has_transform && n->type != ER_NODE_ACTIVITY_INDICATOR)
    {
#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
        if (er_transform_is_3d(n))
        {
            /* 3D perspective path: compute homography, render into scratch, back-project blit. */
            float H[9];
            er_transform_compute_homography_3d(n, px, py, w, h, H);
            er_transform_aabb_3d(px, py, w, h, H, &dst_x, &dst_y, &dst_w, &dst_h);

            if (er_transform_homography_invert(H, xf_inv_H))
            {
                if (s_tile_active && !s_tile_first && er_transform_source_is_cached(n->tag, s_cur_loop_gen))
                {
                    doing_replay = true;
                    doing_3d = true;
                }
                else if (er_transform_source_begin(px, py, w, h))
                {
                    doing_affine = true;
                    doing_3d = true;
                    er_transform_source_note(n->tag, s_tile_active ? s_cur_loop_gen : 0U);
                }
            }
        }
        else
#endif
#if ERUI_TRANSFORMS_FULL
            if (!er_transform_is_translate_only(n))
        {
            /* Full affine: render into scratch, then inverse-map blit. */
            float a, b, c, d, ftx, fty;
            er_transform_compute_matrix(n, px, py, w, h, &a, &b, &c, &d, &ftx, &fty);
            er_transform_aabb(px, py, w, h, a, b, c, d, ftx, fty, &dst_x, &dst_y, &dst_w, &dst_h);

            if (er_transform_invert(a, b, c, d, ftx, fty, &xf_ia, &xf_ib, &xf_ic, &xf_id, &xf_itx, &xf_ity))
            {
                if (s_tile_active && !s_tile_first && er_transform_source_is_cached(n->tag, s_cur_loop_gen))
                {
                    doing_replay = true;
                }
                else if (er_transform_source_begin(px, py, w, h))
                {
                    doing_affine = true;
                    er_transform_source_note(n->tag, s_tile_active ? s_cur_loop_gen : 0U);
                }
            }
            /* On failure (too large or singular matrix) fall through to normal render at
             * the untransformed position as a graceful degradation. */
        }
        else
#endif
        {
            /* Translate-only fast path: shift the render position by the prop offsets. */
            px += (int)n->tp_translate_x;
            py += (int)n->tp_translate_y;
        }
    }

    /* Banded-composite replay: later tiles of the same band pass reuse the transform source
     * captured in the first tile — emit only, bounded to the tile scissor. The subtree
     * re-render, damage recording, and dirty bookkeeping all happened in the first pass. */
    if (doing_replay)
    {
#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
        if (doing_3d)
        {
            er_transform_source_replay_blit_3d(px, py, w, h, xf_inv_H, dst_x, dst_y, dst_w, dst_h);
            return;
        }
#endif
#if ERUI_TRANSFORMS_FULL
        er_transform_source_replay_blit(
            px, py, w, h, xf_ia, xf_ib, xf_ic, xf_id, xf_itx, xf_ity, dst_x, dst_y, dst_w, dst_h);
#endif
        return;
    }

    /* A transform capture renders in SOURCE space: suspend the composite tile for its
     * duration (the tile bounds the transformed OUTPUT via the clip stack at emit time), so
     * pruning and nested composite regions inside the capture aren't bounded by a
     * post-transform rectangle. er_transform_source_begin pushed the matching clip reset. */
    const bool xf_saved_tile_active = s_tile_active;
    if (doing_affine)
        s_tile_active = false;

    /* Record where this node is painted so the next commit can damage-clip a move: the old rect
     * (stored here) unioned with the new rect erases the node's trail without a full-screen repaint. */
    if (should_render)
    {
        n->last_paint_rect.x = (int16_t)(doing_affine ? dst_x : px);
        n->last_paint_rect.y = (int16_t)(doing_affine ? dst_y : py);
        n->last_paint_rect.w = (int16_t)(doing_affine ? dst_w : w);
        n->last_paint_rect.h = (int16_t)(doing_affine ? dst_h : h);
        n->has_last_paint = true;
    }

    /* Shadow: rendered before opacity scratch so the shadow lands in the outer destination
     * (framebuffer or an ancestor opacity slot) rather than inside this node's own composite.
     * Affine-transformed nodes are skipped for v1 — the shadow would otherwise be rasterised
     * into the transform scratch and distorted by the inverse-map blit. */
    if (should_render && !doing_affine)
    {
        switch (n->type)
        {
            case ER_NODE_VIEW:
            case ER_NODE_SCROLL_VIEW:
            case ER_NODE_PRESSABLE:
                er_shadow_render(&n->props.view, px, py, w, h);
                break;
            case ER_NODE_MODAL:
                if (n->modal_visible)
                    er_shadow_render(&n->props.view, px, py, w, h);
                break;
            default:
                break;
        }
    }

    /* Dirty-rect accumulation: only the node that was DIRECTLY dirtied (source_dirty)
     * contributes to the dirty rect, not ancestors that re-render merely to clear
     * backgrounds.  This keeps the reported rect tight around the actually-changed
     * pixels so MCU display drivers can restrict partial DMA transfers. */
    if (n->source_dirty)
    {
        if (doing_affine)
        {
            union_dirty_rect(dst_x, dst_y, dst_w, dst_h);
        }
        else
        {
            int ux = px, uy = py, uw = w, uh = h;
            if (n->type == ER_NODE_VECTOR && n->vec_has_dirty)
            {
                /* Match the sub-region damage so the engine's dirty-rect tracker stays tight too. */
                ux = px + (int)n->vec_dirty_x;
                uy = py + (int)n->vec_dirty_y;
                uw = (int)n->vec_dirty_w;
                uh = (int)n->vec_dirty_h;
            }
#if ERUI_SHADOWS
            /* Expand conservatively for shadow bleed outside the node layout rect. */
            if ((n->type == ER_NODE_VIEW || n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_PRESSABLE
                 || (n->type == ER_NODE_MODAL && n->modal_visible))
                && (n->props.view.shadow_opacity > 0.0f || n->props.view.elevation > 0))
            {
                const int r = (int)n->props.view.shadow_radius;
                const int ox = (int)(fabsf(n->props.view.shadow_offset_x) + 0.5f);
                const int oy = (int)(fabsf(n->props.view.shadow_offset_y) + 0.5f);
                const int exp = r + (ox > oy ? ox : oy);
                ux -= exp;
                uy -= exp;
                uw += 2 * exp;
                uh += 2 * exp;
            }
#endif
            union_dirty_rect(ux, uy, uw, uh);
        }
        n->source_dirty = false;
        /* NB: vec_has_dirty is consumed + cleared by the vector render below (which runs later in this
         * function), not here — clearing it now would hide the sub-region from the rasterize clip. */
    }

    /* Opacity compositing: View-family nodes with opacity < 255 render into off-screen
     * scratch strips which are then blended at the node's alpha. Regions larger than one
     * strip are composited in multiple band passes (see composite_with_opacity); when no
     * slot is available at all the subtree falls back to a direct render. */
    const uint8_t node_opacity =
        (n->type == ER_NODE_VIEW || n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_PRESSABLE
         || (n->type == ER_NODE_MODAL && n->modal_visible) || n->type == ER_NODE_TEXT_INPUT)
            ? n->props.view.opacity
            : 255U;

    bool composited = false;
    if (node_opacity < 255U && should_render)
    {
        composited = composite_from_cache(n, node_opacity, px, py, w, h, translate_x, translate_y);
        if (!composited)
            composited = composite_with_opacity(n, node_opacity, px, py, w, h, translate_x, translate_y);
    }
    if (!composited)
    {
        /* Graceful degradation: when the group cannot be composited (scratch pool exhausted),
         * multiply its opacity into every primitive draw instead of dropping it. Exact wherever
         * siblings don't overlap; far closer to correct than rendering fully opaque. */
        const uint8_t saved_alpha = er_get_draw_alpha();
        if (node_opacity < 255U && should_render)
            er_set_draw_alpha((uint8_t)((uint32_t)saved_alpha * node_opacity / 255U));
        render_node_content(n, should_render, px, py, w, h, translate_x, translate_y);
        er_set_draw_alpha(saved_alpha);
    }

    n->dirty = false;

    /* Affine/perspective transform: end source capture and blit the transformed result. */
    if (doing_affine)
    {
#if ERUI_3D_TRANSFORMS
        if (doing_3d)
            er_transform_source_end_blit_3d(px, py, w, h, xf_inv_H, dst_x, dst_y, dst_w, dst_h);
        else
#endif
            er_transform_source_end_blit(
                px, py, w, h, xf_ia, xf_ib, xf_ic, xf_id, xf_itx, xf_ity, dst_x, dst_y, dst_w, dst_h);
        s_tile_active = xf_saved_tile_active;
    }
}

static void render_node_content(ERNode* n, bool should_render, int px, int py, int w, int h, int translate_x,
                                int translate_y)
{
    if (should_render)
    {
        switch (n->type)
        {
            case ER_NODE_VIEW:
            case ER_NODE_SCROLL_VIEW:
            case ER_NODE_PRESSABLE:
            case ER_NODE_FLAT_LIST:
            {
#if ERUI_GRADIENT
                er_gradient_render(&n->props.view, px, py, w, h);
#endif
                render_view_bg(&n->props.view, px, py, w, h);
                break;
            }
            case ER_NODE_MODAL:
            {
                if (!n->modal_visible)
                    break;
                /* Draw backdrop over the entire root before the modal's own background. */
                ERNode* root = er_get_root_node();
                if (root)
                {
                    const uint32_t bd = n->modal_backdrop_color ? n->modal_backdrop_color : 0x99000000U;
                    er_blit_fill(bd, root->computed.x, root->computed.y, root->computed.w, root->computed.h);
                }
                const ERViewProps* vp = &n->props.view;
#if ERUI_GRADIENT
                er_gradient_render(vp, px, py, w, h);
#endif
                render_view_bg(vp, px, py, w, h);
                break;
            }
            case ER_NODE_TEXT:
            {
                const ERTextProps* tp = &n->props.text;
                ERTextRenderParams par;
                memset(&par, 0, sizeof(par));
                par.text = tp->text;
                par.clip = (ERRect){px, py, w, h};
                par.color = tp->color ? tp->color : 0xFFFFFFFFU;
                par.font_size = tp->font_size;
                par.font_family = tp->font_family;
                par.text_align = tp->text_align;
                par.number_of_lines = tp->number_of_lines;
                par.ellipsize_mode = tp->ellipsize_mode;
                par.text_decoration = tp->text_decoration;
                par.font_weight = tp->font_weight;
                par.font_style = tp->font_style;
                par.line_height = tp->line_height;
                par.letter_spacing = tp->letter_spacing;
                par.span_count = tp->span_count;
                par.spans = (tp->span_count > 0) ? tp->spans : NULL;
                er_text_render(&par);
                break;
            }
            case ER_NODE_IMAGE:
                er_image_render(&n->props.image, px, py, w, h);
                break;
            case ER_NODE_VECTOR:
                if (n->vector_slot >= 0)
                {
                    int no = 0, np = 0, ng = 0;
                    const float* vops = er_vector_slot_ops(n->vector_slot, &no);
                    const ERVectorPaint* vpa = er_vector_slot_paints(n->vector_slot, &np);
                    const ERVectorGradient* vg = er_vector_slot_grads(n->vector_slot, &ng);
                    if (vops && no > 0)
                    {
                        /* Clip the rasterize to the CURRENT DAMAGE REGION (the active scissor), not just
                         * this node's own sub-rect: the background under the vector is repainted across
                         * the whole damage clip (which may be larger — e.g. unioned with the readout's
                         * box), so the vector must recompute + repaint everywhere the background was
                         * erased, or its content (e.g. the track ring) goes missing there. Intersect with
                         * the node box. With no scissor (full repaint), this is just the node box. */
                        int clx0 = px, cly0 = py, clx1 = px + w, cly1 = py + h;
                        int gx, gy, gw, gh;
                        if (er_get_clip_rect(&gx, &gy, &gw, &gh))
                        {
                            if (gx > clx0)
                                clx0 = gx;
                            if (gy > cly0)
                                cly0 = gy;
                            if (gx + gw < clx1)
                                clx1 = gx + gw;
                            if (gy + gh < cly1)
                                cly1 = gy + gh;
                        }
                        er_vector_render(vops, no, vpa, np, vg, ng, px, py, clx0, cly0, clx1, cly1);
                    }
                }
                n->vec_has_dirty = false; /* one-shot: consumed by this commit */
                break;
            case ER_NODE_ACTIVITY_INDICATOR:
                render_activity_indicator(n, px, py, w, h);
                break;
            case ER_NODE_SWITCH:
            {
                const ERSwitchProps* sp = &n->props.sw;
                const float t = n->switch_thumb_t;

                /* Track: pill-shaped rectangle, color lerped between off/on. */
                const uint32_t off_c = sp->track_color_false ? sp->track_color_false : 0xFF767577U;
                const uint32_t on_c = sp->track_color_true ? sp->track_color_true : 0xFF81B0FFU;
                er_rrect_fill_bordered(lerp_color32(off_c, on_c, t), 0x00000000U, 0, px, py, w, h, h / 2);

                /* Thumb: circular knob that slides along the track. */
                const int margin = 2;
                const int thumb_size = h - 2 * margin;
                const int travel = w - thumb_size - 2 * margin;
                const int thumb_x = px + margin + (int)(t * (float)travel + 0.5f);
                const uint32_t tc = sp->thumb_color ? sp->thumb_color : 0xFFFFFFFFU;
                er_rrect_fill_bordered(
                    tc, 0x00000000U, 0, thumb_x, py + margin, thumb_size, thumb_size, thumb_size / 2);
                break;
            }
            case ER_NODE_TEXT_INPUT:
            {
                const ERTextInputProps* tip = &n->props.text_input;
                const int pad_h = tip->border_width + 4;
                const int pad_v = tip->border_width + 3;

                /* Background + border. When focused we draw the border in cursor_color
                 * directly via the bordered fill helper. Doing it this way (rather than a
                 * second highlight pass with bg=0) avoids overwriting the field interior
                 * with the border color, which would hide both the text and the cursor. */
                const uint32_t border_c = (n->is_focused && tip->border_width > 0)
                                              ? (tip->cursor_color ? tip->cursor_color : 0xFF4488FFU)
                                              : tip->border_color;
                er_rrect_fill_bordered(
                    tip->background_color, border_c, tip->border_width, px, py, w, h, tip->border_radius);

                /* Text content or placeholder. */
                const bool show_ph = (n->input_text[0] == '\0');
                ERTextRenderParams par;
                memset(&par, 0, sizeof(par));
                par.text = show_ph ? tip->placeholder : n->input_text;
                par.clip = (ERRect){px + pad_h, py + pad_v, w - 2 * pad_h, h - 2 * pad_v};
                par.color = show_ph ? (tip->placeholder_color ? tip->placeholder_color : 0xFF888888U)
                                    : (tip->color ? tip->color : 0xFFFFFFFFU);
                par.font_size = tip->font_size ? tip->font_size : 16U;
                par.font_family = tip->font_family;
                par.number_of_lines = 1;
                par.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
                er_text_render(&par);

                /* Blinking cursor when focused and not showing placeholder. */
                if (n->is_focused && !show_ph && (s_now_ms % 1000U < 500U))
                {
                    int text_w = 0, text_h = 0;
                    er_text_measure(
                        n->input_text, par.font_size, tip->font_family, 0, par.font_weight, &text_w, &text_h);
                    int cursor_x = px + pad_h + text_w;
                    const int max_cx = px + w - pad_h - 2;
                    if (cursor_x > max_cx)
                        cursor_x = max_cx;
                    const uint32_t cc = tip->cursor_color ? tip->cursor_color : (tip->color ? tip->color : 0xFFFFFFFFU);
                    er_rrect_fill_bordered(cc, 0x00000000U, 0, cursor_x, py + pad_v, 2, h - 2 * pad_v, 0);
                }
                else if (n->is_focused && show_ph && (s_now_ms % 1000U < 500U))
                {
                    /* Cursor at start when field is empty. */
                    const uint32_t cc = tip->cursor_color ? tip->cursor_color : (tip->color ? tip->color : 0xFFFFFFFFU);
                    er_rrect_fill_bordered(cc, 0x00000000U, 0, px + pad_h, py + pad_v, 2, h - 2 * pad_v, 0);
                }
                break;
            }
            default:
                break;
        }
    }

    /* Overflow clipping: push a scissor rect so children cannot draw outside this node. A scroller
     * (ScrollView / FlatList) ALWAYS clips to its viewport — that is its defining behaviour — regardless
     * of an explicit overflow style, so scrolled children can't escape past the top or bottom edge. */
    const bool clips = (n->layout.overflow == ER_OVERFLOW_HIDDEN || n->layout.overflow == ER_OVERFLOW_SCROLL
                        || n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_FLAT_LIST);
    if (clips)
        er_push_clip_rect(px, py, w, h);

    /* Scroll offset translation: ScrollView and FlatList children are shifted by the current offset. */
    const bool is_scroller = (n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_FLAT_LIST);
    const int child_tx = is_scroller ? translate_x + (int)n->scroll_offset_x : translate_x;
    const int child_ty = is_scroller ? translate_y + (int)n->scroll_offset_y : translate_y;

    uint16_t child_tags[ERUI_MAX_NODES];
    const int child_count = collect_children(n, child_tags, ERUI_MAX_NODES);
    sort_children_by_z_index(child_tags, child_count);

    for (int i = 0; i < child_count; i++)
    {
        ERNode* child = er_get_node(child_tags[i]);
        if (!child)
            continue;
        render_tree(child, should_render, child_tx, child_ty);
    }

    if (clips)
        er_pop_clip_rect();
}

/**
 * @brief Recomputes the bounding content size of a ScrollView's children after layout.
 *
 * scroll_content_w and scroll_content_h store the rightmost and bottommost extents of
 * all direct children relative to the ScrollView's own computed origin.  These values
 * are used to clamp scroll offsets in er_scroll_view_set_offset().
 *
 * @param[in,out] node  ScrollView node to update.
 */
static void update_scroll_content_size(ERNode* node)
{
    int16_t max_w = 0;
    int16_t max_h = 0;

    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        const ERNode* child = er_get_node(child_tag);
        if (child)
        {
            const int16_t right = (int16_t)(child->computed.x - node->computed.x + child->computed.w);
            const int16_t bottom = (int16_t)(child->computed.y - node->computed.y + child->computed.h);
            if (right > max_w)
                max_w = right;
            if (bottom > max_h)
                max_h = bottom;
        }
        child_tag = child ? child->next_sibling_tag : ER_INVALID_TAG;
    }

    node->scroll_content_w = max_w;
    node->scroll_content_h = max_h;
}

/**
 * @brief Walks the subtree and updates scroll_content_w / scroll_content_h on all ScrollViews.
 *
 * Called after the layout pass so content-size clamping is based on freshly-computed rects.
 *
 * @param[in] node  Subtree root to walk.
 */
static void refresh_scroll_content_sizes(ERNode* node)
{
    if (!node)
        return;

    if (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST)
        update_scroll_content_size(node);

    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        refresh_scroll_content_sizes(child);
        child_tag = child->next_sibling_tag;
    }
}

/**
 * @brief Walks the subtree and fires ER_EVENT_LAYOUT for every node whose computed
 *        rectangle changed since the previous commit.
 *
 * @param[in] node  Subtree root to check.
 */
static void dispatch_layout_events(ERNode* node)
{
    if (!node)
        return;

    const ERLayoutRect cur = node->computed;
    const ERLayoutRect prev = node->prev_computed;

    if (cur.x != prev.x || cur.y != prev.y || cur.w != prev.w || cur.h != prev.h)
    {
        const EREventHandler* h = &node->events[ER_EVENT_LAYOUT];
        if (h->fn)
        {
            EREventData data = {0};
            data.layout_rect.x = (int)cur.x;
            data.layout_rect.y = (int)cur.y;
            data.layout_rect.w = (int)cur.w;
            data.layout_rect.h = (int)cur.h;
            h->fn(node, &data, h->user_data);
        }
        node->prev_computed = cur;
    }

    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        dispatch_layout_events(child);
        child_tag = child->next_sibling_tag;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

ERNode* er_get_node(uint16_t tag)
{
    if (tag == ER_INVALID_TAG || tag >= (uint16_t)ERUI_MAX_NODES)
        return NULL;
    return s_nodes[tag].in_use ? &s_nodes[tag] : NULL;
}

ERNode* er_get_root_node(void)
{
    return er_get_node(s_root_tag);
}

int er_node_in_use_count(void)
{
    int n = 0;
    for (int i = 0; i < (int)ERUI_MAX_NODES; i++)
        if (s_nodes[i].in_use)
            n++;
    return n;
}

ERNode* er_node_first_child(const ERNode* node)
{
    /* er_get_node returns NULL for ER_INVALID_TAG, so no-children resolves to NULL. */
    return node ? er_get_node(node->first_child_tag) : NULL;
}

ERNode* er_node_next_sibling(const ERNode* node)
{
    return node ? er_get_node(node->next_sibling_tag) : NULL;
}

ERNode* er_node_create(ERNodeType type)
{
    uint16_t tag;
    if (s_free_count > 0)
    {
        tag = s_free_list[--s_free_count];
    }
    else
    {
        if (s_next_tag >= (uint16_t)ERUI_MAX_NODES)
            return NULL;
        tag = s_next_tag++;
    }

    ERNode* n = &s_nodes[tag];
    memset(n, 0, sizeof(ERNode));

    n->tag = tag;
    n->parent_tag = ER_INVALID_TAG;
    n->first_child_tag = ER_INVALID_TAG;
    n->next_sibling_tag = ER_INVALID_TAG;
    n->type = type;
    n->in_use = true;
    n->dirty = true;
    n->vector_slot = -1; /* memset cleared it to 0, which is a valid slot; -1 = "no geometry". */

    init_layout_defaults(&n->layout);

    /* View-type nodes default to fully opaque. */
    if (type == ER_NODE_VIEW || type == ER_NODE_SCROLL_VIEW || type == ER_NODE_PRESSABLE || type == ER_NODE_MODAL)
    {
        n->props.view.opacity = 255U;
    }

    /* ActivityIndicator starts spinning immediately. */
    if (type == ER_NODE_ACTIVITY_INDICATOR)
    {
        n->props.act.animating = 1U;
        ERAnimConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.type = ER_ANIM_TIMING;
        cfg.duration_ms = 1000U;
        cfg.loop = true;
        er_anim_start(n, ER_PROP_ROTATE_Z, 360.0f, &cfg);
    }

    /* TextInput: default to editable. */
    if (type == ER_NODE_TEXT_INPUT)
        n->props.text_input.editable = 1U;

    return n;
}

void er_node_destroy(ERNode* node)
{
    if (!node || !node->in_use)
        return;
    /* Tags are recycled: never let the fade cache survive its owner (or a subtree member). */
    fade_cache_invalidate();
    /* Erase the freed node's pixels on the next commit (damage-clipped, not a full repaint). */
    if (node->has_last_paint)
        damage_union(&s_removed_damage,
                     &s_have_removed_damage,
                     (int)node->last_paint_rect.x,
                     (int)node->last_paint_rect.y,
                     (int)node->last_paint_rect.w,
                     (int)node->last_paint_rect.h);

    node->in_use = false;
    node->dirty = false;
    /* Release the vector storage slot so it can be reused (the binding lives on the node side). */
    if (node->vector_slot >= 0)
    {
        er_vector_free(node->vector_slot);
        node->vector_slot = -1;
    }
    /* Drop any animated-value bindings to this node — its tag is about to be recycled onto the free list,
     * and a stale binding would drive whatever node next reuses it (see er_anim_unbind_node). */
    er_anim_unbind_node(node->tag);
    /* A destroyed node that was still linked into the tree changes its siblings' layout. */
    mark_layout_dirty();
    /* Guard against overflow (would only occur on a double-free bug in the caller). */
    if (s_free_count < (uint16_t)ERUI_MAX_NODES)
        s_free_list[s_free_count++] = node->tag;
}

/** @brief 64-bit FNV-1a hash of an ERProps (zero-initialised by the bridge, so identical props hash equal). */
static uint64_t props_hash(const ERProps* p)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof(ERProps); i++)
    {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void er_props_default(ERProps* props)
{
    if (!props)
        return;
    memset(props, 0, sizeof(*props));

    /* Layout fields default to the AUTO sentinel ("not set", size from flex). flex_grow/flex_shrink
     * are deliberately NOT in this list — their default is 0 (RN's "no grow / no shrink"), and any
     * non-zero value (including ER_LAYOUT_AUTO) is read as a real flex factor. flex_basis stays AUTO. */
    int16_t* const auto_fields[] = {
        &props->left,
        &props->top,
        &props->right,
        &props->bottom,
        &props->width,
        &props->height,
        &props->min_width,
        &props->max_width,
        &props->min_height,
        &props->max_height,
        &props->padding,
        &props->padding_left,
        &props->padding_top,
        &props->padding_right,
        &props->padding_bottom,
        &props->margin,
        &props->margin_left,
        &props->margin_top,
        &props->margin_right,
        &props->margin_bottom,
        &props->gap,
        &props->row_gap,
        &props->column_gap,
        &props->flex_basis,
        &props->margin_horizontal,
        &props->margin_vertical,
        &props->padding_horizontal,
        &props->padding_vertical,
    };
    for (size_t i = 0; i < sizeof(auto_fields) / sizeof(auto_fields[0]); i++)
        *auto_fields[i] = ER_LAYOUT_AUTO;

    props->opacity = 255;

    /* RN defaults the transform pivot to the node centre; the engine treats 0.0 as a literal top-left
       pivot, so seed the fractional origin to 0.5/0.5 (overridden by transformOrigin). */
    props->transform_origin_x = 0.5f;
    props->transform_origin_y = 0.5f;

    /* Type-specific fields whose engine default is non-zero. Harmless for other node types, since
       er_node_set_props applies only the fields relevant to the node's type. */
    props->editable = 1;               /* TextInput editable by default. */
    props->animating = 1;              /* ActivityIndicator spins by default. */
    props->shadow_color = 0xFF000000U; /* Opaque black shadow unless overridden. */
}

void er_node_set_props(ERNode* node, const ERProps* props)
{
    if (!node || !props)
        return;

    /* Skip the layout/repaint invalidation when the props are byte-identical to what's already
     * applied (e.g. React re-running a render with freshly-allocated but equal inline-style objects).
     * The field copies below still run so all derived state stays correct; only the expensive dirty
     * marking is gated, so an unchanged node doesn't drag the whole screen into a repaint. */
    const uint64_t h = props_hash(props);
    const bool props_changed = !node->has_props_hash || h != node->props_hash;
    node->props_hash = h;
    node->has_props_hash = true;

    /* Copy all layout fields. */
    ERLayoutSpec* L = &node->layout;
    L->left = props->left;
    L->top = props->top;
    L->right = props->right;
    L->bottom = props->bottom;
    L->width = props->width;
    L->height = props->height;
    L->min_width = props->min_width;
    L->max_width = props->max_width;
    L->min_height = props->min_height;
    L->max_height = props->max_height;
    L->padding = props->padding;
    /* paddingHorizontal/Vertical expand into per-edge; per-edge wins over shorthand. */
    L->padding_left = (props->padding_left != ER_LAYOUT_AUTO)         ? props->padding_left
                      : (props->padding_horizontal != ER_LAYOUT_AUTO) ? props->padding_horizontal
                                                                      : ER_LAYOUT_AUTO;
    L->padding_right = (props->padding_right != ER_LAYOUT_AUTO)        ? props->padding_right
                       : (props->padding_horizontal != ER_LAYOUT_AUTO) ? props->padding_horizontal
                                                                       : ER_LAYOUT_AUTO;
    L->padding_top = (props->padding_top != ER_LAYOUT_AUTO)        ? props->padding_top
                     : (props->padding_vertical != ER_LAYOUT_AUTO) ? props->padding_vertical
                                                                   : ER_LAYOUT_AUTO;
    L->padding_bottom = (props->padding_bottom != ER_LAYOUT_AUTO)     ? props->padding_bottom
                        : (props->padding_vertical != ER_LAYOUT_AUTO) ? props->padding_vertical
                                                                      : ER_LAYOUT_AUTO;
    L->margin = props->margin;
    /* marginHorizontal/Vertical expand into per-edge; per-edge wins over shorthand. */
    L->margin_left = (props->margin_left != ER_LAYOUT_AUTO)         ? props->margin_left
                     : (props->margin_horizontal != ER_LAYOUT_AUTO) ? props->margin_horizontal
                                                                    : ER_LAYOUT_AUTO;
    L->margin_right = (props->margin_right != ER_LAYOUT_AUTO)        ? props->margin_right
                      : (props->margin_horizontal != ER_LAYOUT_AUTO) ? props->margin_horizontal
                                                                     : ER_LAYOUT_AUTO;
    L->margin_top = (props->margin_top != ER_LAYOUT_AUTO)        ? props->margin_top
                    : (props->margin_vertical != ER_LAYOUT_AUTO) ? props->margin_vertical
                                                                 : ER_LAYOUT_AUTO;
    L->margin_bottom = (props->margin_bottom != ER_LAYOUT_AUTO)     ? props->margin_bottom
                       : (props->margin_vertical != ER_LAYOUT_AUTO) ? props->margin_vertical
                                                                    : ER_LAYOUT_AUTO;
    L->gap = props->gap;
    L->row_gap = props->row_gap;
    L->column_gap = props->column_gap;
    L->flex_grow = props->flex_grow;
    L->flex_shrink = props->flex_shrink;
    L->flex_basis = props->flex_basis;
    L->flex_direction = props->flex_direction;
    L->flex_wrap = props->flex_wrap;
    L->align_items = props->align_items;
    L->align_self = props->align_self;
    L->align_content = props->align_content;
    L->justify_content = props->justify_content;
    L->position = props->position;
    L->display = props->display;
    L->overflow = props->overflow;
    L->aspect_ratio = props->aspect_ratio;
    L->flex_basis_pct = props->flex_basis_pct;
    L->width_pct = props->width_pct;
    L->height_pct = props->height_pct;
    node->z_index = props->z_index;
    node->pointer_events = props->pointer_events;
    node->hit_slop_left = props->hit_slop_left;
    node->hit_slop_top = props->hit_slop_top;
    node->hit_slop_right = props->hit_slop_right;
    node->hit_slop_bottom = props->hit_slop_bottom;

    /* Copy transform props. */
    node->tp_translate_x = props->transform_translate_x;
    node->tp_translate_y = props->transform_translate_y;
    node->tp_scale_x = props->transform_scale_x;
    node->tp_scale_y = props->transform_scale_y;
    node->tp_rotate_z = props->transform_rotate_z;
    node->tp_origin_x = props->transform_origin_x;
    node->tp_origin_y = props->transform_origin_y;
    node->tp_rotate_x = props->transform_rotate_x;
    node->tp_rotate_y = props->transform_rotate_y;
    node->tp_perspective = props->transform_perspective;
    node->has_transform = (props->transform_translate_x != 0.0f || props->transform_translate_y != 0.0f
                           || props->transform_scale_x != 0.0f || props->transform_scale_y != 0.0f
                           || props->transform_rotate_z != 0.0f || props->transform_rotate_x != 0.0f
                           || props->transform_rotate_y != 0.0f || props->transform_perspective != 0.0f);

    /* Copy type-specific visual props. */
    switch (node->type)
    {
        case ER_NODE_VIEW:
        case ER_NODE_SCROLL_VIEW:
        case ER_NODE_PRESSABLE:
        case ER_NODE_FLAT_LIST:
            node->props.view.background_color = props->background_color;
            node->props.view.border_color = props->border_color;
            node->props.view.border_width = props->border_width;
            node->props.view.border_radius = props->border_radius;
            node->props.view.border_tl_radius = props->border_top_left_radius;
            node->props.view.border_tr_radius = props->border_top_right_radius;
            node->props.view.border_br_radius = props->border_bottom_right_radius;
            node->props.view.border_bl_radius = props->border_bottom_left_radius;
            node->props.view.border_left_width = props->border_left_width;
            node->props.view.border_top_width = props->border_top_width;
            node->props.view.border_right_width = props->border_right_width;
            node->props.view.border_bottom_width = props->border_bottom_width;
            node->props.view.border_left_color = props->border_left_color;
            node->props.view.border_top_color = props->border_top_color;
            node->props.view.border_right_color = props->border_right_color;
            node->props.view.border_bottom_color = props->border_bottom_color;
            node->props.view.border_style = props->border_style;
            node->props.view.opacity = props->opacity;
            node->props.view.shadow_color = props->shadow_color;
            node->props.view.shadow_offset_x = props->shadow_offset_x;
            node->props.view.shadow_offset_y = props->shadow_offset_y;
            node->props.view.shadow_opacity = props->shadow_opacity;
            node->props.view.shadow_radius = props->shadow_radius;
            node->props.view.elevation = props->elevation;
            node->props.view.gradient_type = props->gradient_type;
            node->props.view.gradient_angle = props->gradient_angle;
            node->props.view.gradient_stop_count = props->gradient_stop_count;
            for (int gi = 0; gi < ER_GRADIENT_MAX_STOPS; gi++)
                node->props.view.gradient_stops[gi] = props->gradient_stops[gi];
            break;
        case ER_NODE_TEXT:
            strncpy(node->props.text.text, props->text, ER_TEXT_MAX);
            node->props.text.text[ER_TEXT_MAX] = '\0';
            strncpy(node->props.text.font_family, props->font_family, ER_FONT_FAMILY_MAX);
            node->props.text.font_family[ER_FONT_FAMILY_MAX] = '\0';
            node->props.text.color = props->color;
            node->props.text.font_size = props->font_size;
            node->props.text.font_weight = props->font_weight;
            node->props.text.font_style = props->font_style;
            node->props.text.text_align = props->text_align;
            node->props.text.number_of_lines = props->number_of_lines;
            node->props.text.ellipsize_mode = props->ellipsize_mode;
            node->props.text.text_decoration = props->text_decoration;
            node->props.text.line_height = props->line_height;
            node->props.text.letter_spacing = props->letter_spacing;
            {
                const uint8_t sc =
                    (props->span_count < ER_TEXT_MAX_SPANS) ? props->span_count : (uint8_t)ER_TEXT_MAX_SPANS;
                node->props.text.span_count = sc;
                for (uint8_t si = 0; si < sc; si++)
                {
                    ERTextSpan* dst = &node->props.text.spans[si];
                    const ERTextSpan* src = &props->spans[si];
                    strncpy(dst->text, src->text, ER_SPAN_TEXT_MAX);
                    dst->text[ER_SPAN_TEXT_MAX] = '\0';
                    dst->color = src->color;
                    dst->font_size = src->font_size;
                    dst->font_weight = src->font_weight;
                    dst->font_style = src->font_style;
                    dst->text_decoration = src->text_decoration;
                    dst->letter_spacing = src->letter_spacing;
                }
            }
            break;
        case ER_NODE_IMAGE:
            strncpy(node->props.image.image_name, props->image_name, ER_IMAGE_NAME_MAX);
            node->props.image.image_name[ER_IMAGE_NAME_MAX] = '\0';
            node->props.image.resize_mode = props->resize_mode;
            node->props.image.tint_color = props->tint_color;
            break;
        case ER_NODE_ACTIVITY_INDICATOR:
        {
            const uint8_t was_animating = node->props.act.animating;
            node->props.act.color = props->indicator_color;
            node->props.act.animating = props->animating;
            if (props->animating && !was_animating)
            {
                /* (Re-)start the looping spin animation. */
                ERAnimConfig cfg;
                memset(&cfg, 0, sizeof(cfg));
                cfg.type = ER_ANIM_TIMING;
                cfg.duration_ms = 1000U;
                cfg.loop = true;
                er_anim_start(node, ER_PROP_ROTATE_Z, 360.0f, &cfg);
            }
            else if (!props->animating && was_animating)
            {
                er_anim_cancel(node, ER_PROP_ROTATE_Z);
            }
            break;
        }
        case ER_NODE_SWITCH:
        {
            const uint8_t old_value = node->props.sw.value;
            node->props.sw.track_color_false = props->track_color_false;
            node->props.sw.track_color_true = props->track_color_true;
            node->props.sw.thumb_color = props->thumb_color;
            node->props.sw.value = props->switch_value;

            /* Animate the thumb when the value changes. */
            if (props->switch_value != old_value)
            {
                ERAnimConfig cfg;
                memset(&cfg, 0, sizeof(cfg));
                cfg.type = ER_ANIM_TIMING;
                cfg.easing = ER_EASE_EASE_IN_OUT;
                cfg.duration_ms = 200U;
                er_anim_start(node, ER_PROP_SWITCH_THUMB, props->switch_value ? 1.0f : 0.0f, &cfg);
            }
            break;
        }
        case ER_NODE_TEXT_INPUT:
            node->props.text_input.background_color = props->background_color;
            node->props.text_input.border_color = props->border_color;
            node->props.text_input.border_width = props->border_width;
            node->props.text_input.border_radius = props->border_radius;
            node->props.text_input.opacity = props->opacity ? props->opacity : 255U;
            node->props.text_input.color = props->color;
            node->props.text_input.font_size = props->font_size;
            strncpy(node->props.text_input.font_family, props->font_family, ER_FONT_FAMILY_MAX);
            node->props.text_input.font_family[ER_FONT_FAMILY_MAX] = '\0';
            strncpy(node->props.text_input.placeholder, props->placeholder, ER_PLACEHOLDER_MAX);
            node->props.text_input.placeholder[ER_PLACEHOLDER_MAX] = '\0';
            node->props.text_input.placeholder_color = props->placeholder_color;
            node->props.text_input.cursor_color = props->cursor_color;
            node->props.text_input.editable = props->editable ? props->editable : 1U;
            /* If 'text' is provided, set it as the current input value. */
            if (props->text[0] != '\0')
                er_text_input_set_text(node, props->text);
            break;
        case ER_NODE_MODAL:
            node->props.view.background_color = props->background_color;
            node->props.view.border_color = props->border_color;
            node->props.view.border_width = props->border_width;
            node->props.view.border_radius = props->border_radius;
            node->props.view.opacity = props->opacity ? props->opacity : 255U;
            node->modal_visible = props->modal_visible;
            node->modal_backdrop_color = props->backdrop_color;
            /* Propagate visibility to the layout so the modal takes no space when hidden. */
            node->layout.display = props->modal_visible ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE;
            node->props.view.shadow_color = props->shadow_color;
            node->props.view.shadow_offset_x = props->shadow_offset_x;
            node->props.view.shadow_offset_y = props->shadow_offset_y;
            node->props.view.shadow_opacity = props->shadow_opacity;
            node->props.view.shadow_radius = props->shadow_radius;
            node->props.view.elevation = props->elevation;
            node->props.view.gradient_type = props->gradient_type;
            node->props.view.gradient_angle = props->gradient_angle;
            node->props.view.gradient_stop_count = props->gradient_stop_count;
            for (int gi = 0; gi < ER_GRADIENT_MAX_STOPS; gi++)
                node->props.view.gradient_stops[gi] = props->gradient_stops[gi];
            break;
        default:
            break;
    }

    /* Native-driver props (opacity/transform/color animated via an ERAnimValue) are owned by the
     * animation, not this declarative update — restore them so a React commitUpdate doesn't snap
     * the node back to its static value mid-animation. No-op when nothing is bound to this node. */
    er_anim_reapply_bound(node);

    /* Props may change layout inputs (size, flex, margins, text content/font). Conservatively
     * request a layout pass; only when something actually changed (see the props_hash gate above) —
     * an identical setProps invalidates nothing. */
    if (props_changed)
    {
        mark_layout_dirty();
        er_mark_dirty_upward(node);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public — TextInput
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Gives keyboard focus to a TextInput node.
 *
 * @param[in] node  TextInput node to focus, or NULL to blur the current input.
 */
void er_text_input_focus(ERNode* node)
{
    if (node && node->type != ER_NODE_TEXT_INPUT)
        return;

    s_kbd_dirty = true; /* focus is changing → repaint the on-screen keyboard strip (show or hide) */
    s_kbd_layer = 0;    /* a freshly focused input starts on the lowercase layer */

    /* Blur the currently focused node first. */
    if (s_focused_input_tag != ER_INVALID_TAG)
    {
        ERNode* old = er_get_node(s_focused_input_tag);
        if (old)
        {
            old->is_focused = false;
            er_mark_dirty_upward(old);
            const EREventHandler* h = &old->events[ER_EVENT_BLUR];
            if (h->fn)
            {
                EREventData d = {0};
                h->fn(old, &d, h->user_data);
            }
        }
        s_focused_input_tag = ER_INVALID_TAG;
    }

    if (!node)
        return;

    node->is_focused = true;
    s_focused_input_tag = node->tag;
    er_mark_dirty_upward(node);

    const EREventHandler* h = &node->events[ER_EVENT_FOCUS];
    if (h->fn)
    {
        EREventData d = {0};
        h->fn(node, &d, h->user_data);
    }
}

/**
 * @brief Removes focus from the currently focused TextInput, if any.
 */
void er_text_input_blur(void)
{
    er_text_input_focus(NULL);
}

/**
 * @brief Returns the current text content of a TextInput node.
 *
 * @param[in] node  TextInput node.
 *
 * @return Pointer to the null-terminated text buffer, or NULL if not a TextInput.
 */
const char* er_text_input_get_text(const ERNode* node)
{
    if (!node || node->type != ER_NODE_TEXT_INPUT)
        return NULL;
    return node->input_text;
}

/**
 * @brief Sets the text content of a TextInput node.
 *
 * @param[in] node  TextInput node.
 * @param[in] text  Null-terminated UTF-8 string.
 */
void er_text_input_set_text(ERNode* node, const char* text)
{
    if (!node || node->type != ER_NODE_TEXT_INPUT || !text)
        return;
    strncpy(node->input_text, text, ER_TEXT_MAX);
    node->input_text[ER_TEXT_MAX] = '\0';
    er_mark_dirty_upward(node);
}

/**
 * @brief Sets inline text spans on a Text node.
 *
 * @param[in] node   Text node to configure.
 * @param[in] spans  Span descriptors; NULL to revert to single-string rendering.
 * @param[in] count  Number of spans; clamped to ER_TEXT_MAX_SPANS.
 */
void er_node_set_text_spans(ERNode* node, const ERTextSpan* spans, uint8_t count)
{
    if (!node || node->type != ER_NODE_TEXT)
        return;
    const uint8_t n = (count < (uint8_t)ER_TEXT_MAX_SPANS) ? count : (uint8_t)ER_TEXT_MAX_SPANS;
    node->props.text.span_count = (spans && n > 0) ? n : 0U;
    for (uint8_t i = 0; i < node->props.text.span_count; i++)
    {
        ERTextSpan* dst = &node->props.text.spans[i];
        strncpy(dst->text, spans[i].text, ER_SPAN_TEXT_MAX);
        dst->text[ER_SPAN_TEXT_MAX] = '\0';
        dst->color = spans[i].color;
        dst->font_size = spans[i].font_size;
        dst->font_weight = spans[i].font_weight;
        dst->font_style = spans[i].font_style;
        dst->text_decoration = spans[i].text_decoration;
        dst->letter_spacing = spans[i].letter_spacing;
    }
    /* Span text feeds the Text node's intrinsic-width measurement during layout. */
    mark_layout_dirty();
    er_mark_dirty_upward(node);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Vector damage diffing
 *
 * When a state-driven <Svg> re-uploads its op-tape (e.g. a thermostat dial each value change), most of
 * it is unchanged — the static track arc, and a moving value arc that only shifts its END ANGLE. The
 * vector rasterizer's cost is CLIP-AREA bound, so damaging just the changed sub-region instead of the
 * whole node box is the difference between a cheap and an expensive redraw. We diff the new tape against
 * the stored one and emit a tight node-local damage rect. CONSERVATIVE: any structural change (first
 * upload, different length/opcodes/paint-index, or a paint-table change) falls back to a full-box repaint,
 * so the rect can never be too SMALL — no stale-pixel artifacts. A paint-only change (same geometry, e.g.
 * a mode recolor) also falls back to full, which is correct (every pixel of the shape changes colour).
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Grows a bbox with points sampled along a circle arc [a0,a1] (radians) — the changed sub-sweep. */
static void
vec_bbox_arc(float cx, float cy, float r, float a0, float a1, float* minx, float* miny, float* maxx, float* maxy)
{
    float span = a1 - a0;
    if (span < 0.0f)
        span = -span;
    int n = (int)(span / 0.196f) + 1; /* ~ one sample per 11.25 deg */
    if (n > 64)
        n = 64;
    for (int k = 0; k <= n; k++)
    {
        const float a = a0 + (a1 - a0) * (float)k / (float)n;
        const float x = cx + r * cosf(a);
        const float y = cy + r * sinf(a);
        if (x < *minx)
            *minx = x;
        if (y < *miny)
            *miny = y;
        if (x > *maxx)
            *maxx = x;
        if (y > *maxy)
            *maxy = y;
    }
}

/**
 * @brief Diffs old vs new op-tape (+ paints) and writes a node-local damage rect of the changed geometry.
 * @return true if a tight rect was computed; false → caller must repaint the full node box.
 */
static bool vec_diff_dirty_rect(const float* o,
                                int on,
                                const ERVectorPaint* op,
                                int onp,
                                const float* nw,
                                int nn,
                                const ERVectorPaint* np,
                                int nnp,
                                int* rx,
                                int* ry,
                                int* rw,
                                int* rh)
{
    if (!o || on <= 0 || on != nn || onp != nnp)
        return false;
    if (onp > 0 && (!op || !np || memcmp(op, np, (size_t)onp * sizeof(ERVectorPaint)) != 0))
        return false; /* paint table changed → every pixel of affected shapes recolours; full repaint */

    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    bool any = false;
#define VADD(X, Y)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        const float _x = (X), _y = (Y);                                                                                \
        if (_x < minx)                                                                                                 \
            minx = _x;                                                                                                 \
        if (_y < miny)                                                                                                 \
            miny = _y;                                                                                                 \
        if (_x > maxx)                                                                                                 \
            maxx = _x;                                                                                                 \
        if (_y > maxy)                                                                                                 \
            maxy = _y;                                                                                                 \
        any = true;                                                                                                    \
    } while (0)

    int i = 0;
    while (i < on)
    {
        if (o[i] != nw[i])
            return false; /* opcode / shape structure differs */
        const int code = (int)o[i];
        if (code == (int)ER_VOP_SHAPE)
        {
            i++;
            if (i < on)
            {
                if (o[i] != nw[i])
                    return false; /* paint-index swap on a shape */
                i++;
            }
            continue;
        }
        i++; /* consume opcode (identical in both tapes) */
        if (code == (int)ER_VOP_MOVE || code == (int)ER_VOP_LINE)
        {
            if (i + 2 > on)
                return false;
            if (o[i] != nw[i] || o[i + 1] != nw[i + 1])
            {
                VADD(o[i], o[i + 1]);
                VADD(nw[i], nw[i + 1]);
            }
            i += 2;
        }
        else if (code == (int)ER_VOP_QUAD)
        {
            if (i + 4 > on)
                return false;
            bool ch = false;
            for (int k = 0; k < 4; k++)
                if (o[i + k] != nw[i + k])
                    ch = true;
            if (ch)
            {
                VADD(o[i], o[i + 1]);
                VADD(o[i + 2], o[i + 3]);
                VADD(nw[i], nw[i + 1]);
                VADD(nw[i + 2], nw[i + 3]);
            }
            i += 4;
        }
        else if (code == (int)ER_VOP_CUBIC)
        {
            if (i + 6 > on)
                return false;
            bool ch = false;
            for (int k = 0; k < 6; k++)
                if (o[i + k] != nw[i + k])
                    ch = true;
            if (ch)
                for (int k = 0; k < 6; k += 2)
                {
                    VADD(o[i + k], o[i + k + 1]);
                    VADD(nw[i + k], nw[i + k + 1]);
                }
            i += 6;
        }
        else if (code == (int)ER_VOP_ARC)
        {
            if (i + 6 > on)
                return false;
            const float ocx = o[i], ocy = o[i + 1], orr = o[i + 2], oa0 = o[i + 3], oa1 = o[i + 4], occw = o[i + 5];
            const float ncx = nw[i], ncy = nw[i + 1], nrr = nw[i + 2], na0 = nw[i + 3], na1 = nw[i + 4],
                        nccw = nw[i + 5];
            if (ocx != ncx || ocy != ncy || orr != nrr || oa0 != na0 || oa1 != na1 || occw != nccw)
            {
                if (ocx == ncx && ocy == ncy && orr == nrr && occw == nccw)
                {
                    /* Same circle, only swept angles moved (the value arc) → damage just the changed sub-arcs. */
                    if (oa0 != na0)
                        vec_bbox_arc(ocx, ocy, orr, oa0, na0, &minx, &miny, &maxx, &maxy);
                    if (oa1 != na1)
                        vec_bbox_arc(ocx, ocy, orr, oa1, na1, &minx, &miny, &maxx, &maxy);
                    any = true;
                }
                else
                {
                    /* Centre/radius moved (e.g. the handle knob) → full-circle bbox of old + new. */
                    VADD(ocx - orr, ocy - orr);
                    VADD(ocx + orr, ocy + orr);
                    VADD(ncx - nrr, ncy - nrr);
                    VADD(ncx + nrr, ncy + nrr);
                }
            }
            i += 6;
        }
        else if (code == (int)ER_VOP_CLOSE)
        {
            /* no coordinates */
        }
        else
        {
            return false; /* unknown opcode — bail to a full repaint */
        }
    }
#undef VADD

    if (!any)
        return false; /* geometry identical (paint-only or no-op change) → full box is simplest & correct */

    /* Pad by the widest stroke half-width + AA so round caps and coverage fringes are covered. */
    float pad = 1.5f;
    for (int p = 0; p < nnp; p++)
    {
        const float sw = np[p].stroke_w;
        if (sw > 0.0f)
        {
            const float hp = sw * 0.5f + 1.5f;
            if (hp > pad)
                pad = hp;
        }
    }
    minx -= pad;
    miny -= pad;
    maxx += pad;
    maxy += pad;
    *rx = (int)floorf(minx);
    *ry = (int)floorf(miny);
    *rw = (int)ceilf(maxx) - *rx;
    *rh = (int)ceilf(maxy) - *ry;
    return (*rw > 0 && *rh > 0);
}

void er_node_set_vector_ops(ERNode* node,
                            const float* ops,
                            int n_ops,
                            const ERVectorPaint* paints,
                            int n_paints,
                            const ERVectorGradient* grads,
                            int n_grads)
{
    if (!node || node->type != ER_NODE_VECTOR)
        return;
    /* Default to a full-box repaint; the diff below (or er_node_set_vector_dirty_rect, if called after) narrows it. */
    node->vec_has_dirty = false;
    if (!ops || n_ops <= 0)
    {
        /* Clearing geometry: release the slot and repaint the (now empty) box. */
        if (node->vector_slot >= 0)
        {
            er_vector_free(node->vector_slot);
            node->vector_slot = -1;
        }
        er_mark_dirty_upward(node);
        return;
    }

    /* Diff the incoming tape against the stored one to damage only the changed sub-region (a cheap redraw
     * for a moving dial). Read the old tape BEFORE er_vector_store overwrites the slot. */
    int dx = 0, dy = 0, dw = 0, dh = 0;
    bool tight = false;
    if (node->vector_slot >= 0)
    {
        int old_n = 0, old_np = 0, old_ng = 0;
        const float* old_ops = er_vector_slot_ops(node->vector_slot, &old_n);
        const ERVectorPaint* old_paints = er_vector_slot_paints(node->vector_slot, &old_np);
        const ERVectorGradient* old_grads = er_vector_slot_grads(node->vector_slot, &old_ng);
        /* Identical re-upload (e.g. a held finger below the drag deadband re-running app_update with the same
         * state) → nothing changed, so skip the repaint entirely. */
        if (old_ops && old_n == n_ops && old_np == n_paints && memcmp(old_ops, ops, (size_t)n_ops * sizeof(float)) == 0
            && (n_paints <= 0
                || (old_paints && paints && memcmp(old_paints, paints, (size_t)n_paints * sizeof(ERVectorPaint)) == 0))
            && old_ng == n_grads
            && (n_grads <= 0
                || (old_grads && grads && memcmp(old_grads, grads, (size_t)n_grads * sizeof(ERVectorGradient)) == 0)))
        {
            return;
        }
        tight =
            vec_diff_dirty_rect(old_ops, old_n, old_paints, old_np, ops, n_ops, paints, n_paints, &dx, &dy, &dw, &dh);
    }

    node->vector_slot = er_vector_store(node->vector_slot, ops, n_ops, paints, n_paints, grads, n_grads);
    if (tight)
    {
        node->vec_dirty_x = (int16_t)dx;
        node->vec_dirty_y = (int16_t)dy;
        node->vec_dirty_w = (int16_t)dw;
        node->vec_dirty_h = (int16_t)dh;
        node->vec_has_dirty = true;
    }
    /* Geometry is visual-only (the box comes from layout/style), so no layout pass is needed. */
    er_mark_dirty_upward(node);
}

void er_node_set_vector_dirty_rect(ERNode* node, int x, int y, int w, int h)
{
    if (!node || node->type != ER_NODE_VECTOR)
        return;
    node->vec_dirty_x = (int16_t)x;
    node->vec_dirty_y = (int16_t)y;
    node->vec_dirty_w = (int16_t)w;
    node->vec_dirty_h = (int16_t)h;
    node->vec_has_dirty = true;
    er_mark_dirty_upward(node);
}

/**
 * @brief Processes a keyboard event for the currently focused TextInput node.
 *
 * @param[in] keycode    Control key code (ER_KEY_*) or 0 for printable characters.
 * @param[in] utf8_char  Character to insert, or NULL for control keys.
 */
void er_text_input_key(uint32_t keycode, const char* utf8_char)
{
    if (s_focused_input_tag == ER_INVALID_TAG)
        return;
    ERNode* node = er_get_node(s_focused_input_tag);
    if (!node || !node->props.text_input.editable)
        return;

    const size_t len = strlen(node->input_text);
    bool changed = false;

    if (keycode == ER_KEY_BACKSPACE || keycode == ER_KEY_DELETE)
    {
        if (len > 0)
        {
            /* Remove the last UTF-8 character (back up past any continuation bytes). */
            size_t i = len - 1;
            while (i > 0 && ((unsigned char)node->input_text[i] & 0xC0U) == 0x80U)
                i--;
            node->input_text[i] = '\0';
            changed = true;
        }
    }
    else if (keycode == ER_KEY_RETURN)
    {
        const EREventHandler* h = &node->events[ER_EVENT_SUBMIT_EDITING];
        if (h->fn)
        {
            EREventData d = {0};
            d.changed_text = node->input_text;
            h->fn(node, &d, h->user_data);
        }
    }
    else if (keycode == ER_KEY_ESCAPE)
    {
        er_text_input_blur();
    }
    else if (utf8_char && utf8_char[0] != '\0')
    {
        /* Append the UTF-8 character if there is room. */
        const size_t char_len = strlen(utf8_char);
        if (len + char_len < ER_TEXT_MAX)
        {
            memcpy(node->input_text + len, utf8_char, char_len);
            node->input_text[len + char_len] = '\0';
            changed = true;
        }
    }

    if (changed)
    {
        er_mark_dirty_upward(node);
        const EREventHandler* h = &node->events[ER_EVENT_CHANGE_TEXT];
        if (h->fn)
        {
            EREventData d = {0};
            d.changed_text = node->input_text;
            h->fn(node, &d, h->user_data);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - On-screen keyboard — compiled in only when ERUI_ONSCREEN_KEYBOARD=1 (touch-only devices). The layout and
 - appearance are entirely data-driven (ERKeyboardConfig): an app overrides them via er_keyboard_set_config()
 - without touching the engine. Rows are laid out span-proportionally, so one config flexes to any screen.
 ---------------------------------------------------------------------------------------------------------------------*/

#ifndef ERUI_ONSCREEN_KEYBOARD
#define ERUI_ONSCREEN_KEYBOARD 0
#endif

#if ERUI_ONSCREEN_KEYBOARD

/* ---- Built-in default layout: lowercase / UPPERCASE / numbers / symbols, the iOS-style QWERTY pages. ---- */
#define KCH(s) {(s), (s), ER_KBD_KEY_CHAR, 0, 2, 0xFFU}   /* char key, span 2 */
#define KSPACE {NULL, " ", ER_KBD_KEY_CHAR, 0, 12, 0xFFU} /* blank space bar */
#define KBKSP {"<", NULL, ER_KBD_KEY_BACKSPACE, 0, 3, 0xFFU}
#define KDONE {"OK", NULL, ER_KBD_KEY_DONE, 0, 4, 0xFFU}
#define KSW3(lbl, tgt, hl) {(lbl), NULL, ER_KBD_KEY_LAYER, (tgt), 3, (hl)} /* row-2 left switch */
#define KSW4(lbl, tgt) {(lbl), NULL, ER_KBD_KEY_LAYER, (tgt), 4, 0xFFU}    /* row-3 left switch */

static const ERKeyboardKey L0r0[] = {
    KCH("q"), KCH("w"), KCH("e"), KCH("r"), KCH("t"), KCH("y"), KCH("u"), KCH("i"), KCH("o"), KCH("p")};
static const ERKeyboardKey L0r1[] = {
    KCH("a"), KCH("s"), KCH("d"), KCH("f"), KCH("g"), KCH("h"), KCH("j"), KCH("k"), KCH("l")};
static const ERKeyboardKey L0r2[] = {
    KSW3("^", 1, 1), KCH("z"), KCH("x"), KCH("c"), KCH("v"), KCH("b"), KCH("n"), KCH("m"), KBKSP};
static const ERKeyboardKey L0r3[] = {KSW4("123", 2), KSPACE, KDONE};
static const ERKeyboardRow L0rows[] = {{L0r0, 10}, {L0r1, 9}, {L0r2, 9}, {L0r3, 3}};

static const ERKeyboardKey L1r0[] = {
    KCH("Q"), KCH("W"), KCH("E"), KCH("R"), KCH("T"), KCH("Y"), KCH("U"), KCH("I"), KCH("O"), KCH("P")};
static const ERKeyboardKey L1r1[] = {
    KCH("A"), KCH("S"), KCH("D"), KCH("F"), KCH("G"), KCH("H"), KCH("J"), KCH("K"), KCH("L")};
static const ERKeyboardKey L1r2[] = {
    KSW3("^", 0, 1), KCH("Z"), KCH("X"), KCH("C"), KCH("V"), KCH("B"), KCH("N"), KCH("M"), KBKSP};
static const ERKeyboardRow L1rows[] = {{L1r0, 10}, {L1r1, 9}, {L1r2, 9}, {L0r3, 3}};

static const ERKeyboardKey L2r0[] = {
    KCH("1"), KCH("2"), KCH("3"), KCH("4"), KCH("5"), KCH("6"), KCH("7"), KCH("8"), KCH("9"), KCH("0")};
static const ERKeyboardKey L2r1[] = {
    KCH("-"), KCH("/"), KCH(":"), KCH(";"), KCH("("), KCH(")"), KCH("$"), KCH("&"), KCH("@"), KCH("\"")};
static const ERKeyboardKey L2r2[] = {KSW3("#+=", 3, 0xFFU), KCH("."), KCH(","), KCH("?"), KCH("!"), KCH("'"), KBKSP};
static const ERKeyboardKey L2r3[] = {KSW4("ABC", 0), KSPACE, KDONE};
static const ERKeyboardRow L2rows[] = {{L2r0, 10}, {L2r1, 10}, {L2r2, 7}, {L2r3, 3}};

static const ERKeyboardKey L3r0[] = {
    KCH("["), KCH("]"), KCH("{"), KCH("}"), KCH("#"), KCH("%"), KCH("^"), KCH("*"), KCH("+"), KCH("=")};
static const ERKeyboardKey L3r1[] = {KCH("_"), KCH("\\"), KCH("|"), KCH("~"), KCH("<"), KCH(">"), KCH("$"), KCH("`")};
static const ERKeyboardKey L3r2[] = {KSW3("123", 2, 0xFFU), KCH("."), KCH(","), KCH("?"), KCH("!"), KCH("'"), KBKSP};
static const ERKeyboardRow L3rows[] = {{L3r0, 10}, {L3r1, 8}, {L3r2, 7}, {L2r3, 3}};

#undef KCH
#undef KSPACE
#undef KBKSP
#undef KDONE
#undef KSW3
#undef KSW4

static const ERKeyboardLayer s_kbd_default_layers[4] = {{L0rows, 4}, {L1rows, 4}, {L2rows, 4}, {L3rows, 4}};
static const ERKeyboardConfig s_kbd_default_cfg = {s_kbd_default_layers, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const ERKeyboardConfig* s_kbd_cfg = NULL; /**< app override; NULL = built-in default */

/** @brief The active keyboard config (app-supplied or the built-in default). */
static const ERKeyboardConfig* er_kbd_cfg(void)
{
    return s_kbd_cfg ? s_kbd_cfg : &s_kbd_default_cfg;
}

/** @brief The active layout — falls back to the built-in QWERTY when a config leaves `layers` NULL, so an
 *         app can override only colours/sizes and keep the default keys. Returns the layer array + count. */
static const ERKeyboardLayer* er_kbd_layers(uint8_t* count)
{
    const ERKeyboardConfig* c = er_kbd_cfg();
    if (c->layers && c->layer_count)
    {
        *count = c->layer_count;
        return c->layers;
    }
    *count = 4;
    return s_kbd_default_layers;
}

void er_keyboard_set_config(const ERKeyboardConfig* cfg)
{
    s_kbd_cfg = cfg;
    s_kbd_layer = 0;
    s_kbd_dirty = true;
}

bool er_keyboard_active(void)
{
    return s_focused_input_tag != ER_INVALID_TAG;
}

/** @brief Largest row count across the config's layers (so the strip rect is stable across layer switches). */
static int er_kbd_rows(void)
{
    uint8_t lc;
    const ERKeyboardLayer* layers = er_kbd_layers(&lc);
    int m = 0;
    for (uint8_t i = 0; i < lc; i++)
        if ((int)layers[i].count > m)
            m = layers[i].count;
    return m ? m : 1;
}

/** @brief Per-row height: configured, else ≈1/11 of screen height; clamped so the strip stays ≤ half-screen. */
static int er_kbd_row_h(int screen_h)
{
    const ERKeyboardConfig* c = er_kbd_cfg();
    int rh = c->row_height_px ? (int)c->row_height_px : (screen_h / 11);
    if (rh < 24)
        rh = 24;
    const int rows = er_kbd_rows();
    if (rh * rows > screen_h / 2)
        rh = (screen_h / 2) / rows;
    return rh;
}

/** @brief Bottom strip the keyboard occupies. */
static void er_keyboard_rect(int screen_w, int screen_h, ERRect* out)
{
    const int kh = er_kbd_row_h(screen_h) * er_kbd_rows();
    out->x = 0;
    out->y = screen_h - kh;
    out->w = screen_w;
    out->h = kh;
}

/** One laid-out key: its screen rect, the source key descriptor, and whether to draw it highlighted. */
typedef struct
{
    int x, y, w, h;
    const ERKeyboardKey* key;
    bool highlight;
} ERKbdHit1;
typedef void (*er_kbd_key_fn)(const ERKbdHit1* key, void* ud);

/** Invokes `fn` for every key of the active layer, positioned span-proportionally and centred per row. */
static void er_kbd_foreach(int screen_w, int screen_h, er_kbd_key_fn fn, void* ud)
{
    const ERKeyboardConfig* c = er_kbd_cfg();
    uint8_t lc;
    const ERKeyboardLayer* layers = er_kbd_layers(&lc);
    if (s_kbd_layer >= lc)
        return;
    ERRect kr;
    er_keyboard_rect(screen_w, screen_h, &kr);
    const int grid = c->grid_cols ? (int)c->grid_cols : 20;
    const int cw = kr.w / grid;
    const int rh = er_kbd_row_h(screen_h);
    const int pad = c->key_gap_px ? (int)c->key_gap_px : 2;
    const ERKeyboardLayer* L = &layers[s_kbd_layer];

    for (uint8_t r = 0; r < L->count; r++)
    {
        const ERKeyboardRow* row = &L->rows[r];
        int total = 0;
        for (uint8_t i = 0; i < row->count; i++)
            total += row->keys[i].span ? row->keys[i].span : 1;
        int xc = (grid - total) / 2;
        if (xc < 0)
            xc = 0;
        for (uint8_t i = 0; i < row->count; i++)
        {
            const ERKeyboardKey* kk = &row->keys[i];
            const int span = kk->span ? kk->span : 1;
            ERKbdHit1 lk;
            lk.x = kr.x + xc * cw + pad;
            lk.y = kr.y + r * rh + pad;
            lk.w = span * cw - 2 * pad;
            lk.h = rh - 2 * pad;
            lk.key = kk;
            lk.highlight = (kk->type == ER_KBD_KEY_LAYER && kk->highlight_layer == s_kbd_layer);
            fn(&lk, ud);
            xc += span;
        }
    }
}

/** Draws one key: a rounded fill (brighter when highlighted) + its centred label. */
static void er_kbd_draw_key(const ERKbdHit1* k, void* ud)
{
    const ERKeyboardConfig* c = (const ERKeyboardConfig*)ud;
    const uint32_t key_bg = c->key_color ? c->key_color : 0xFF2A3340U;
    const uint32_t act_bg = c->key_active_color ? c->key_active_color : 0xFF4A6488U;
    const uint32_t label_color = c->label_color ? c->label_color : 0xFFE7EDF5U;
    const int radius = c->key_radius_px ? (int)c->key_radius_px : 5;
    const int fs = c->font_size_px ? (int)c->font_size_px : 16;
    er_rrect_fill_bordered(k->highlight ? act_bg : key_bg, 0xFF000000U, 0, k->x, k->y, k->w, k->h, radius);
    const char* label = k->key->label ? k->key->label : k->key->text;
    if (label && label[0] && label[0] != ' ') /* the space bar (text " ") stays blank */
    {
        ERTextRenderParams par;
        memset(&par, 0, sizeof(par));
        par.text = label;
        /* Keep the glyph top where it reads centred, but extend the clip to the key's bottom edge so
         * descenders (p g q y j) are not cut. */
        const int top = k->y + (k->h - fs) / 2;
        par.clip = (ERRect){k->x, top, k->w, (k->y + k->h) - top};
        par.color = label_color;
        par.font_size = (uint16_t)fs;
        par.text_align = ER_TEXT_ALIGN_CENTER;
        par.number_of_lines = 1;
        er_text_render(&par);
    }
}

void er_keyboard_draw(int screen_w, int screen_h)
{
    if (!er_keyboard_active())
        return;
    const ERKeyboardConfig* c = er_kbd_cfg();
    ERRect kr;
    er_keyboard_rect(screen_w, screen_h, &kr);
    er_blit_fill(c->panel_color ? c->panel_color : 0xFF12181FU, kr.x, kr.y, kr.w, kr.h);
    er_kbd_foreach(screen_w, screen_h, er_kbd_draw_key, (void*)c);
}

/** Hit-test accumulator: the first key under (x, y). */
typedef struct
{
    int x, y;
    const ERKeyboardKey* key;
} ERKbdHit;

static void er_kbd_hit_one(const ERKbdHit1* k, void* ud)
{
    ERKbdHit* h = (ERKbdHit*)ud;
    if (h->key)
        return;
    if (h->x >= k->x && h->x < k->x + k->w && h->y >= k->y && h->y < k->y + k->h)
        h->key = k->key;
}

bool er_keyboard_dispatch_touch(ERTouchPhase phase, int x, int y)
{
    if (!er_keyboard_active())
        return false;
    ERNode* root = er_get_node(s_root_tag);
    if (!root)
        return false;
    const int sw = (int)root->computed.w;
    const int sh = (int)root->computed.h;
    ERRect kr;
    er_keyboard_rect(sw, sh, &kr);
    if (x < kr.x || x >= kr.x + kr.w || y < kr.y || y >= kr.y + kr.h)
        return false; /* outside the keyboard → let the scene handle the touch */
    if (phase == ER_TOUCH_DOWN)
    {
        ERKbdHit h;
        h.x = x;
        h.y = y;
        h.key = NULL;
        er_kbd_foreach(sw, sh, er_kbd_hit_one, &h);
        if (h.key)
        {
            switch (h.key->type)
            {
                case ER_KBD_KEY_LAYER:
                    s_kbd_layer = h.key->layer;
                    s_kbd_dirty = true; /* the whole keyboard relabels → repaint its strip next commit */
                    break;
                case ER_KBD_KEY_BACKSPACE:
                    er_text_input_key(ER_KEY_BACKSPACE, NULL);
                    break;
                case ER_KBD_KEY_DONE:
                    er_text_input_key(ER_KEY_ESCAPE, NULL);
                    break;
                case ER_KBD_KEY_CHAR:
                default:
                    er_text_input_key(0, h.key->text ? h.key->text : "");
                    break;
            }
        }
    }
    return true; /* consume all phases inside the keyboard so they never reach the scene */
}

#else /* ERUI_ONSCREEN_KEYBOARD: compiled out — trivial stubs so callers need no #ifs. */

bool er_keyboard_active(void)
{
    return false;
}
void er_keyboard_draw(int screen_w, int screen_h)
{
    (void)screen_w;
    (void)screen_h;
}
bool er_keyboard_dispatch_touch(ERTouchPhase phase, int x, int y)
{
    (void)phase;
    (void)x;
    (void)y;
    return false;
}
void er_keyboard_set_config(const ERKeyboardConfig* cfg)
{
    (void)cfg;
}

#endif /* ERUI_ONSCREEN_KEYBOARD */

/** @brief Pixels the scene is currently shifted up for keyboard avoidance (0 when none). Hit-testing adds
 *         this to a touch's Y to map a screen point back to the shifted scene. Always defined (0 when the
 *         keyboard is compiled out), so hit_test.c can call it unconditionally. */
int er_keyboard_avoid_offset(void)
{
    return s_kbd_avoid_y;
}

/**
 * @brief Unlinks a child from its current parent's sibling list, if it has one.
 *
 * Leaves child->parent_tag set (callers reassign it). Makes append/insert move-safe: a child that
 * is already attached is removed from its old position before being re-spliced, so re-appending or
 * reordering an existing node cannot corrupt the sibling chain.
 *
 * @param[in] child  Node to detach from its parent's child list.
 */
static void tree_detach(ERNode* child)
{
    if (child->parent_tag == ER_INVALID_TAG)
        return;
    ERNode* parent = er_get_node(child->parent_tag);
    if (!parent)
        return;

    if (parent->first_child_tag == child->tag)
    {
        parent->first_child_tag = child->next_sibling_tag;
    }
    else
    {
        uint16_t cur = parent->first_child_tag;
        while (cur != ER_INVALID_TAG)
        {
            ERNode* cur_n = er_get_node(cur);
            if (!cur_n)
                break;
            if (cur_n->next_sibling_tag == child->tag)
            {
                cur_n->next_sibling_tag = child->next_sibling_tag;
                break;
            }
            cur = cur_n->next_sibling_tag;
        }
    }
    child->next_sibling_tag = ER_INVALID_TAG;
}

void er_tree_append_child(ERNode* parent, ERNode* child)
{
    if (!parent || !child)
        return;

    /* Move-safe: detach from any current position so re-appending an existing child (React
     * reorders keyed children with appendChild) doesn't corrupt the sibling chain. */
    tree_detach(child);
    child->parent_tag = parent->tag;

    if (parent->first_child_tag == ER_INVALID_TAG)
    {
        parent->first_child_tag = child->tag;
    }
    else
    {
        uint16_t cur = parent->first_child_tag;
        while (cur != ER_INVALID_TAG)
        {
            ERNode* cur_n = er_get_node(cur);
            if (!cur_n)
                break;
            if (cur_n->next_sibling_tag == ER_INVALID_TAG)
            {
                cur_n->next_sibling_tag = child->tag;
                break;
            }
            cur = cur_n->next_sibling_tag;
        }
    }

    child->next_sibling_tag = ER_INVALID_TAG;
    parent->dirty = true;
    mark_layout_dirty();
}

void er_tree_insert_before(ERNode* parent, ERNode* child, ERNode* before)
{
    if (!parent || !child)
        return;

    /* No anchor → append (also covers inserting before a non-child). */
    if (!before)
    {
        er_tree_append_child(parent, child);
        return;
    }

    /* Move-safe: unlink from any current position (this or another parent) before splicing. */
    tree_detach(child);
    child->parent_tag = parent->tag;

    /* Splice immediately before `before`. */
    if (parent->first_child_tag == before->tag)
    {
        child->next_sibling_tag = before->tag;
        parent->first_child_tag = child->tag;
    }
    else
    {
        uint16_t cur = parent->first_child_tag;
        while (cur != ER_INVALID_TAG)
        {
            ERNode* cur_n = er_get_node(cur);
            if (!cur_n)
                break;
            if (cur_n->next_sibling_tag == before->tag)
            {
                child->next_sibling_tag = before->tag;
                cur_n->next_sibling_tag = child->tag;
                break;
            }
            cur = cur_n->next_sibling_tag;
        }
        /* `before` is not a child of parent → append instead. */
        if (cur == ER_INVALID_TAG)
        {
            er_tree_append_child(parent, child);
            return;
        }
    }

    parent->dirty = true;
    mark_layout_dirty();
}

void er_tree_remove_child(ERNode* parent, ERNode* child)
{
    if (!parent || !child)
        return;

    /* Record the removed subtree's footprint (while it's still intact) so the next commit erases
     * those pixels — damage-clipped, not a full-screen repaint. */
    note_removed_subtree(child);

    tree_detach(child);
    child->parent_tag = ER_INVALID_TAG;
    parent->dirty = true;
    mark_layout_dirty();
}

void er_tree_set_root(ERNode* root)
{
    if (!root)
    {
        s_root_tag = ER_INVALID_TAG;
        mark_layout_dirty();
        return;
    }
    s_root_tag = root->tag;
    mark_layout_dirty();
    er_force_full_repaint(); /* whole new scene: repaint everything */
}

void er_reset(void)
{
    fade_cache_invalidate();
    /* Empty the node pool and clear the scene root. er_node_create pops the free list or bumps
     * s_next_tag and memsets each slot on allocation, so resetting the counters is a complete reset —
     * nothing scans s_nodes for stale in_use flags. */
    s_next_tag = 0;
    s_free_count = 0;
    s_root_tag = ER_INVALID_TAG;
    s_focused_input_tag = ER_INVALID_TAG;
    s_last_cursor_phase = 2U;

    /* Drop dirty/damage tracking and force the next commit to fully repaint and re-run layout. */
    s_has_dirty = false;
    s_have_removed_damage = false;
    s_force_full_repaint = true;
    s_layout_dirty = true;

    /* Reset the multi-buffer debt: every rotating buffer owes a full frame again (new scene). */
    s_cur_buf = 0;
    debt_reset_all_full();

    /* Reset the per-scene subsystems. The render backend, registered images, and registered/built-in
     * fonts are kept; the monotonic clock (s_now_ms) is preserved so time never runs backwards across
     * a reload. */
    er_anim_reset();
    er_layout_anim_reset();
    er_vector_reset();
    er_input_reset();
}

void er_commit(void)
{
    if (s_root_tag == ER_INVALID_TAG)
        return;

    ERNode* root = er_get_node(s_root_tag);
    if (!root)
        return;

    s_commit_seq++; /* per-commit sequence for the fade cache's one-capture-per-commit gate */

    /* Reset dirty-rect accumulator for this commit. */
    s_has_dirty = false;

    /* Blinking cursor: if there is a focused TextInput, mark it dirty whenever the
     * 500 ms blink phase has changed since the last commit. This keeps the render
     * cost negligible (one re-render every half-second) instead of every frame. */
    if (s_focused_input_tag != ER_INVALID_TAG)
    {
        ERNode* focus = er_get_node(s_focused_input_tag);
        if (focus && focus->type == ER_NODE_TEXT_INPUT && focus->is_focused)
        {
            const uint8_t cursor_phase = (s_now_ms % 1000U < 500U) ? 1U : 0U;
            if (cursor_phase != s_last_cursor_phase)
            {
                er_mark_dirty_upward(focus);
                s_last_cursor_phase = cursor_phase;
            }
        }
        else
        {
            s_focused_input_tag = ER_INVALID_TAG;
            s_last_cursor_phase = 2U;
        }
    }
    else
    {
        s_last_cursor_phase = 2U;
    }

#if ERUI_ONSCREEN_KEYBOARD
    /* Keyboard avoidance: shift the whole scene UP just enough that the focused input clears the on-screen
     * keyboard (0 when no input is focused or it is already above the strip). A change moves the whole scene,
     * so force a full repaint that frame; while stable, node_screen_rect applies the same offset so damage
     * tracking stays exact. */
    {
        int want = 0;
        if (er_keyboard_active())
        {
            ERNode* inp = er_get_node(s_focused_input_tag);
            if (inp)
            {
                ERRect kr;
                er_keyboard_rect((int)root->computed.w, (int)root->computed.h, &kr);
                const int input_bottom = (int)inp->animated.y + (int)inp->animated.h; /* unshifted layout pos */
                const int needed = input_bottom + 8 - kr.y;                           /* 8px above the strip */
                if (needed > 0)
                    want = needed;
            }
        }
        if (want != s_kbd_avoid_y)
        {
            s_kbd_avoid_y = want;
            er_force_full_repaint(); /* the whole scene shifts → repaint everything once */
        }
    }
#endif

    /* Layout fast path: the flex solver and per-Text-node measurement only run when something
     * that affects a computed rect changed since the last commit (mark_layout_dirty), or when a
     * LayoutAnimation config is pending and must be evaluated against fresh rects. Animations
     * mutate render-only props and never move computed rects, so a static or animation-only frame
     * skips this whole block — the computed rects from the previous layout remain valid, and the
     * post-layout passes that read them would produce identical results, so they are simply not
     * re-run. render_tree() below still runs every commit to repaint dirty nodes. */
    const bool layout_ran = (s_layout_dirty || er_layout_anim_has_pending());
    if (layout_ran)
    {
        const int16_t rw = (root->layout.width != ER_LAYOUT_AUTO) ? root->layout.width : 0;
        const int16_t rh = (root->layout.height != ER_LAYOUT_AUTO) ? root->layout.height : 0;

        er_layout_compute(s_root_tag, rw, rh);
        refresh_scroll_content_sizes(root);
        er_layout_anim_post_layout(root);
        dispatch_layout_events(root);
        compute_subtree_bounds(root); /* refresh cached prune bounds; stay valid through static frames */

        s_layout_dirty = false;
        s_layout_pass_count++;
    }

    /* Damage-clipped render. Unless we must repaint everything (first frame, an invalidated
     * framebuffer, a removed node, or a changing node with a transform we can't bound), scissor
     * render_tree() to the union of every changed-or-moved node's new screen rect and the rect it was
     * painted at last commit (so a moved node's trail is erased). A node counts if it was directly
     * dirtied (source_dirty) or its screen rect differs from where it was last painted (layout reflow
     * or a translate animation) — propagated-dirty ancestors are excluded so the damage stays tight.
     * The persistent framebuffer keeps the untouched pixels, so the compositor and the backend's flush
     * both shrink from full-screen to just the changed region. */
    /* Compute this commit's repaint region in screen space. Default is the whole root rect (a full
     * repaint); the damage pre-pass below narrows it to the union of changed/moved node rects whenever
     * change tracking is possible. The region then drives EITHER a single damage-clipped render_tree
     * (full-framebuffer backend) OR a per-strip banded render (backend->band_height > 0). */
    int rb_x0 = root->computed.x;
    int rb_y0 = root->computed.y;
    int rb_x1 = rb_x0 + root->computed.w;
    int rb_y1 = rb_y0 + root->computed.h;
    bool render_full = true;    /* repaint the whole root rect */
    bool nothing_dirty = false; /* tracked, but nothing changed (or changes clamped off-screen) */
    if (s_force_full_repaint)
    {
        /* Full repaint requested: this commit's OWN damage is the whole screen. For multi-buffer that is
         * folded into every buffer's debt below (so each rotating buffer repaints fully when next
         * rendered); for single-buffer it simply repaints the one framebuffer. */
        root->dirty = true;
    }
    else
    {
        ERRect clip = {0, 0, 0, 0};
        bool have = false;
        bool trackable = true;
        /* Seed with any pixels vacated by removed/destroyed nodes since the last commit. */
        if (s_have_removed_damage)
            damage_union(&clip, &have, s_removed_damage.x, s_removed_damage.y, s_removed_damage.w, s_removed_damage.h);
#if ERUI_ONSCREEN_KEYBOARD
        /* On-screen keyboard show/hide/layer-switch: repaint its bottom strip once (then GRAM retains it). */
        if (s_kbd_dirty)
        {
            ERRect kr;
            er_keyboard_rect((int)root->computed.w, (int)root->computed.h, &kr);
            damage_union(&clip, &have, kr.x, kr.y, kr.w, kr.h);
        }
#endif
        for (uint16_t tag = 0U; tag < (uint16_t)ERUI_MAX_NODES; tag++)
        {
            ERNode* n = er_get_node(tag);
            if (!n)
                continue;
            int rx, ry, rw, rh;
            if (!node_screen_rect(n, &rx, &ry, &rw, &rh))
            {
                /* Complex transform (scale/rotate). Bound the damage to the node's transformed AABB —
                 * current box plus where it was painted last commit, so a shrinking pulse erases its
                 * trail — instead of forcing a full-screen repaint.
                 */
                int tx, ty, tw, th;
                if (node_transformed_screen_rect(n, &tx, &ty, &tw, &th))
                {
                    const bool moved = n->has_last_paint
                                       && (tx != (int)n->last_paint_rect.x || ty != (int)n->last_paint_rect.y
                                           || tw != (int)n->last_paint_rect.w || th != (int)n->last_paint_rect.h);
                    if (n->source_dirty || moved)
                    {
                        int nx = tx, ny = ty, nw = tw, nh = th;
                        clip_rect_to_clippers(n, &nx, &ny, &nw, &nh);
                        if (nw > 0 && nh > 0)
                            damage_union(&clip, &have, nx, ny, nw, nh); /* new transformed footprint */
                        if (n->has_last_paint)
                        {
                            int ox = (int)n->last_paint_rect.x, oy = (int)n->last_paint_rect.y,
                                ow = (int)n->last_paint_rect.w, oh = (int)n->last_paint_rect.h;
                            clip_rect_to_clippers(n, &ox, &oy, &ow, &oh);
                            if (ow > 0 && oh > 0)
                                damage_union(&clip, &have, ox, oy, ow, oh); /* old footprint (erase trail) */
                        }
                    }
                    continue;
                }
                /*
                 * Could not bound it (3D / oversized): only forces a full repaint if actually changing.
                 * TODO: A moved-but-not-source_dirty node here (e.g. a 3D-transformed node shifted by reflow) is still missed — that needs the 3D AABB path ().
                 */
                if (n->source_dirty)
                {
                    trackable = false;
                    break;
                }
                continue;
            }
            const bool moved = n->has_last_paint
                               && (rx != (int)n->last_paint_rect.x || ry != (int)n->last_paint_rect.y
                                   || rw != (int)n->last_paint_rect.w || rh != (int)n->last_paint_rect.h);
            if (!n->source_dirty && !moved)
                continue; /* unchanged and in place: contributes nothing to the damage */
            if (n->type == ER_NODE_VECTOR && n->vec_has_dirty && !moved)
            {
                /* Sub-region vector update: damage only the app-supplied changed rect (node-local →
                 * screen), not the whole box. The caller's rect already covers old+new content, so the
                 * full last_paint_rect is intentionally NOT unioned (it would balloon back to the box). */
                damage_union(&clip,
                             &have,
                             rx + (int)n->vec_dirty_x,
                             ry + (int)n->vec_dirty_y,
                             (int)n->vec_dirty_w,
                             (int)n->vec_dirty_h);
                continue;
            }
            /* Clip both contributions to any clipping ancestor (ScrollView / overflow:hidden) so a scrolled
             * child's damage can't reach outside the list and pull a sibling (e.g. a title above) into the
             * repaint, where it would be cleared but not restored for a frame. */
            int nx = rx, ny = ry, nw = rw, nh = rh;
            clip_rect_to_clippers(n, &nx, &ny, &nw, &nh);
            if (nw > 0 && nh > 0)
                damage_union(&clip, &have, nx, ny, nw, nh); /* new position */
            if (n->has_last_paint)
            {
                int ox = (int)n->last_paint_rect.x, oy = (int)n->last_paint_rect.y, ow = (int)n->last_paint_rect.w,
                    oh = (int)n->last_paint_rect.h;
                clip_rect_to_clippers(n, &ox, &oy, &ow, &oh);
                if (ow > 0 && oh > 0)
                    damage_union(&clip, &have, ox, oy, ow, oh); /* old position (erase trail) */
            }
        }
        if (trackable && have)
        {
            /* Pad for anti-aliased / sub-pixel edges, then clamp to the root rect. */
            const int margin = 2;
            int cx0 = clip.x - margin;
            int cy0 = clip.y - margin;
            int cx1 = clip.x + clip.w + margin;
            int cy1 = clip.y + clip.h + margin;
            if (cx0 < rb_x0)
                cx0 = rb_x0;
            if (cy0 < rb_y0)
                cy0 = rb_y0;
            if (cx1 > rb_x1)
                cx1 = rb_x1;
            if (cy1 > rb_y1)
                cy1 = rb_y1;
            if (cx1 > cx0 && cy1 > cy0)
            {
                rb_x0 = cx0;
                rb_y0 = cy0;
                rb_x1 = cx1;
                rb_y1 = cy1;
                render_full = false; /* narrowed to the damage region */
            }
            else
            {
                nothing_dirty = true; /* damage collapsed to empty after clamping (off-screen change) */
            }
        }
        else if (trackable)
        {
            nothing_dirty = true; /* nothing changed this commit */
        }
        /* !trackable: render_full stays true (repaint the whole root rect). */
    }

    /* --- Multi-buffer (page-flip) damage debt --------------------------------------------------------------
     * At this point render_full / nothing_dirty / rb_* describe THIS commit's OWN damage. With >1 rotating
     * display buffers, the buffer we render into was last painted (count-1) PRESENTS ago — and, crucially,
     * commits are driven by React (a burst at mount, none when idle), NOT by the host's present loop. So we
     * cannot count commits. Instead each buffer carries a "debt": the union of every commit's damage since
     * that buffer was last rendered. We (1) fold this commit's own damage into every buffer's debt, then
     * (2) render the CURRENT buffer over its full debt (bringing it current) and clear it. er_display_present()
     * advances s_cur_buf at each flip, so a buffer's debt accumulates exactly the commits it missed while a
     * different buffer was on screen — converging regardless of how many commits fall between presents. */
    if (s_display_buffer_count > 1)
    {
        /* (1) Fold this commit's own damage into every buffer's debt. nothing_dirty is checked first because
         * render_full defaults to true and is left true in the nothing-changed case (there it means "nothing",
         * not "full"). */
        if (nothing_dirty)
        {
            /* no new damage to record */
        }
        else if (render_full)
        {
            for (int b = 0; b < s_display_buffer_count; b++)
            {
                s_buf_debt[b].full = true;
                s_buf_debt[b].has = false;
            }
        }
        else
        {
            for (int b = 0; b < s_display_buffer_count; b++)
                if (!s_buf_debt[b].full)
                    damage_union(&s_buf_debt[b].rect, &s_buf_debt[b].has, rb_x0, rb_y0, rb_x1 - rb_x0,
                                 rb_y1 - rb_y0);
        }

        /* (2) Derive this commit's render region from the CURRENT buffer's debt (what it still owes). */
        ERDamageSlot* d = &s_buf_debt[s_cur_buf];
        if (d->full)
        {
            rb_x0 = root->computed.x;
            rb_y0 = root->computed.y;
            rb_x1 = rb_x0 + root->computed.w;
            rb_y1 = rb_y0 + root->computed.h;
            render_full = true;
            nothing_dirty = false;
            root->dirty = true; /* full recomposite of the whole screen into the stale buffer */
        }
        else if (d->has)
        {
            rb_x0 = d->rect.x;
            rb_y0 = d->rect.y;
            rb_x1 = d->rect.x + d->rect.w;
            rb_y1 = d->rect.y + d->rect.h;
            render_full = false;
            nothing_dirty = false;
        }
        else
        {
            render_full = false;
            nothing_dirty = true;
        }

        /* The current buffer is being brought fully current this commit, so it owes nothing afterward.
         * (Cleared here, before render, is fine: the region to paint is captured in rb_* above.) */
        d->full = false;
        d->has = false;
        d->rect.x = d->rect.y = d->rect.w = d->rect.h = 0;
    }

    /* Enable subtree pruning only when no layout animation is interpolating positions (which would
     * leave the cached computed-space bounds stale). A full repaint pushes no clip, so render_tree()
     * pruning self-disables there regardless. */
    s_prune_ok = !er_layout_anim_has_pending() && !er_layout_anim_is_active();

    const EmbeddedRenderBackend* backend = er_backend();
    if (backend && backend->band_height > 0)
    {
        /* Banded render: split the repaint region into horizontal strips no taller than the band
         * buffer. Each strip is fully recomposited (parent_dirty = true) into the backend's band buffer
         * — which starts blank, so every overlapping layer must repaint, unlike the retained-framebuffer
         * path — then flushed to the panel. Pixels outside the region are retained by the panel's GRAM.
         *
         * The whole repaint region is pushed as a SINGLE clip for the duration (enabling subtree pruning
         * and, crucially, keeping transform/opacity scratch sources complete — the region contains every
         * changed node in full). The per-strip row range is set via er_set_band(), which clamps only the
         * backend emit; render_tree additionally culls subtrees that fall entirely outside the strip. */
        if (!nothing_dirty)
        {
            /* Strips span the FULL screen width (only the dirty ROWS are narrowed). A band backend
             * flushes each strip as one tightly-packed full-width block straight from the band buffer,
             * so partial-width strips are intentionally not used; on the narrow panels this targets the
             * extra horizontal fill is negligible. The damage box still bounds the dirty rows. */
            const int bh = backend->band_height;
            const int fx = root->computed.x;
            const int fw = root->computed.w;
            er_push_clip_rect(fx, rb_y0, fw, rb_y1 - rb_y0);
            for (int sy = rb_y0; sy < rb_y1; sy += bh)
            {
                const int sh = (rb_y1 - sy < bh) ? (rb_y1 - sy) : bh;
                if (backend->band_begin)
                    backend->band_begin(fx, sy, fw, sh, backend->ctx);
                er_set_band(sy, sh);
                render_tree(root, true, 0, s_kbd_avoid_y);   /* whole scene shifted up to clear the keyboard */
                er_keyboard_draw(fw, (int)root->computed.h); /* overlay (no-op for bands above the strip) */
                if (backend->band_flush)
                    backend->band_flush(backend->ctx);
            }
            er_set_band(0, 0);
            er_pop_clip_rect();
        }
    }
    else
    {
        /* Full-framebuffer render: one damage-clipped pass; the persistent framebuffer retains the
         * untouched pixels. (nothing_dirty → no clip, render_tree repaints nothing; render_full → no
         * clip, the whole tree repaints; otherwise scissor to the damage region.) */
        bool clipped = false;
        if (!render_full && !nothing_dirty)
        {
            er_push_clip_rect(rb_x0, rb_y0, rb_x1 - rb_x0, rb_y1 - rb_y0);
            clipped = true;
        }
        /* Single buffer: rely on the propagated dirty flags inside the clip (parent_dirty = false) so only
         * the changed nodes repaint. Multi-buffer replay must instead FULLY recomposite the clipped region
         * (parent_dirty = true) — the replayed history covers nodes with no live dirty flag this frame, and
         * the stale buffer needs them all repainted. Matches the banded path, which always recomposites. */
        const bool full_recomposite = (s_display_buffer_count > 1) && (clipped || render_full);
        render_tree(root, full_recomposite, 0, s_kbd_avoid_y); /* whole scene shifted up to clear the keyboard */
        er_keyboard_draw((int)root->computed.w, (int)root->computed.h); /* overlay, clipped to the damage */
        if (clipped)
            er_pop_clip_rect();
    }

    /* Multi-buffer: er_get_dirty_rect() must report the region actually painted (the replayed union), not
     * just this frame's source-dirty nodes, so a host using it for a secondary transfer sees the truth.
     * (Single buffer keeps the tight source_dirty union accumulated by render_tree.) */
    if (s_display_buffer_count > 1)
    {
        if (render_full)
        {
            s_dirty_rect.x = root->computed.x;
            s_dirty_rect.y = root->computed.y;
            s_dirty_rect.w = root->computed.w;
            s_dirty_rect.h = root->computed.h;
            s_has_dirty = (root->computed.w > 0 && root->computed.h > 0);
        }
        else if (!nothing_dirty)
        {
            s_dirty_rect.x = rb_x0;
            s_dirty_rect.y = rb_y0;
            s_dirty_rect.w = rb_x1 - rb_x0;
            s_dirty_rect.h = rb_y1 - rb_y0;
            s_has_dirty = true;
        }
        else
        {
            s_has_dirty = false;
        }
    }

    s_force_full_repaint = false;
    s_kbd_dirty = false;           /* the keyboard strip (if any) was repainted this commit */
    s_have_removed_damage = false; /* consumed (or covered by a full repaint) this commit */

#if ER_PROF
    {
        static uint32_t s_prof_commits = 0;
        if (++s_prof_commits >= 30U)
        {
            printf("ERPROF: passes=%u composites=%u push_us=%u content_us=%u blend_us=%u (per 30 commits)\n",
                   (unsigned)s_prof_passes,
                   (unsigned)s_prof_composites,
                   (unsigned)s_prof_push_us,
                   (unsigned)s_prof_content_us,
                   (unsigned)s_prof_blend_us);
            s_prof_commits = 0;
            s_prof_passes = s_prof_composites = 0;
            s_prof_push_us = s_prof_content_us = s_prof_blend_us = 0;
        }
    }
#endif
}

uint32_t er_layout_pass_count(void)
{
    return s_layout_pass_count;
}

uint32_t er_now_ms(void)
{
    return s_now_ms;
}

bool er_get_dirty_rect(ERRect* out)
{
    if (out)
    {
        if (s_has_dirty)
            *out = s_dirty_rect;
        else
        {
            out->x = 0;
            out->y = 0;
            out->w = 0;
            out->h = 0;
        }
    }
    return s_has_dirty;
}

void er_tick(uint32_t delta_ms)
{
    s_now_ms += delta_ms;
}

void er_scroll_view_set_offset(ERNode* node, float x, float y)
{
    if (!node || (node->type != ER_NODE_SCROLL_VIEW && node->type != ER_NODE_FLAT_LIST))
        return;

    /* Clamp to valid scroll range.  The maximum offset is content_size − viewport_size,
     * floored at 0 so we never scroll past the start or beyond the end. */
    float max_x = (float)(node->scroll_content_w - node->computed.w);
    float max_y = (float)(node->scroll_content_h - node->computed.h);
    if (max_x < 0.0f)
        max_x = 0.0f;
    if (max_y < 0.0f)
        max_y = 0.0f;

    if (x < 0.0f)
        x = 0.0f;
    if (x > max_x)
        x = max_x;
    if (y < 0.0f)
        y = 0.0f;
    if (y > max_y)
        y = max_y;

    if (x == node->scroll_offset_x && y == node->scroll_offset_y)
        return;

    node->scroll_offset_x = x;
    node->scroll_offset_y = y;
    er_mark_dirty_upward(node);

    const EREventHandler* h = &node->events[ER_EVENT_SCROLL];
    if (h->fn)
    {
        EREventData data = {0};
        data.scroll_x = x;
        data.scroll_y = y;
        h->fn(node, &data, h->user_data);
    }
}
