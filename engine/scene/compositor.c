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

#include "arc.h"
#include "arc_widget.h"
#include "er_damage_internal.h"
#include "er_node_internal.h"
#include "er_perf.h"
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
#include <stddef.h>
#include <string.h>

#ifndef ERUI_MAX_NODES
#define ERUI_MAX_NODES 512
#endif

/* Occlusion culling: skip layers that a fully opaque node painted on top of them completely covers.
 * On by default (the engine CMake plumbs the option through); the fallback keeps consumers that
 * compile the sources directly — the ESP-IDF / Pico components — from having to know about it. */
#ifndef ERUI_OCCLUSION_CULLING
#define ERUI_OCCLUSION_CULLING 1
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

/* Dirty-rect tracking: union of all screen rects repainted by the last er_commit() that PAINTED.
 * A commit that finds nothing dirty leaves these alone (er_reset() clears them), so a Flow A host —
 * whose own er_commit() runs after the reconciler already committed inside er_runtime_pump() — still
 * reads the frame's damage instead of the empty no-op commit. */
static ERRect s_dirty_rect;
static bool s_has_dirty = false;

/* The disjoint repaint rects of the last er_commit() that painted (post-replay, exactly what was
 * scissored and painted). Backs er_get_dirty_rects(); a full repaint records one root-sized rect. */
static ERDamageSet s_last_paint_set;

/* Damage-clipped rendering: when true the next commit repaints the whole screen (first frame, an
 * invalidated framebuffer, or a new root). Otherwise render_tree is scissored to just the rects that
 * actually changed, so a small animation flushes a small region instead of all 800x480. */
static bool s_force_full_repaint = true;

/* Pending damage from removed/destroyed nodes (their last painted rects): seeded into the next
 * commit's damage set so the vacated pixels are erased without a full-screen repaint. A set rather
 * than one bbox, so two far-apart unmounts (a common React conditional-render pattern) don't drag
 * the span between them into the repaint. */
static ERDamageSet s_removed_set;

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
 * absorbed — either a set of disjoint rects, the whole screen (full), or nothing (empty set, already
 * current). Slot saturation degrades toward one bbox, i.e. exactly the pre-rect-set behaviour. */
typedef struct
{
    ERDamageSet set;
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
 * @brief Per-worker composite-pass state (see er_render_worker_id).
 *
 * Tile bounds and band-pass generations are scoped to one worker's render traversal; the
 * dirty-rect accumulator collects that worker's painted region and er_commit merges all
 * workers' accumulators after the render. Generation tags only need to be unique within one
 * worker (they guard that worker's own transform-source cache), so the counter is per-worker
 * too; it starts at 0 so the first pass stamps generation 1, which the transform cache's
 * zero-initialized state can never false-match.
 */
typedef struct
{
    bool tile_active;
    int tile_x;
    int tile_y;
    int tile_w;
    int tile_h;
    uint32_t band_gen_counter;
    uint32_t cur_loop_gen;
    bool tile_first;
    ERRect dirty_rect; /**< This worker's union of painted-node rects this commit. */
    bool has_dirty;
    bool xf_capturing;     /**< A transformed ancestor's scratch capture is active: everything rendering
                                below it is in SOURCE space and reaches the screen only through that
                                ancestor's inverse-map blit. */
    ERRect xf_capture_dst; /**< Where that blit writes — the ancestor's transformed screen AABB. */
} CompCtx;

static CompCtx s_comp_ctx[ERUI_RENDER_WORKERS];

/* Multi-core render safety: vector rasterization and shadow blurs still run through large SHARED
 * static scratch (engine/rendering/vector.c, shadow.c — too big to duplicate per worker), so a
 * scene containing them renders single-core. Vector nodes and shadow-casting views are counted as
 * they come and go; the sliced fork below only engages while the count is zero. Every other
 * primitive (fills, borders, text, gradients, images, opacity strips, transforms) works from
 * per-worker state and is safe to render concurrently. */
static int s_parallel_unsafe = 0;

/* True while er_commit is rendering slices on multiple workers. The fade cache sits out those
 * frames (composite_from_cache checks this): its buffer is one shared global, and a capture from
 * inside one worker's slice would race other workers reading or re-tagging it. Cache validity is
 * generation-based, so it simply resumes on the next single-core frame. */
static bool s_parallel_render = false;

/* Commits rendered via the sliced fork since boot — a cheap engagement diagnostic (host frame
 * traces, tests asserting the fork really ran). */
static uint32_t s_parallel_frames = 0;

/* A parallel frame wanted a fade-cache capture (see composite_from_cache): er_commit renders the
 * next commit single-core so the capture can run — but only if the content generation is still
 * the one recorded here, so ever-changing content doesn't thrash between modes. Workers may set
 * this concurrently (same-value writes); er_commit consumes it sequentially. */
static bool s_fade_capture_wanted = false;
static uint32_t s_fade_capture_wanted_gen = 0;

/** @brief The calling worker's composite context (constant &s_comp_ctx[0] in single-worker builds). */
static inline CompCtx* cc(void)
{
    return &s_comp_ctx[er_render_worker_id()];
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

    if (!cc()->has_dirty)
    {
        cc()->dirty_rect.x = x;
        cc()->dirty_rect.y = y;
        cc()->dirty_rect.w = w;
        cc()->dirty_rect.h = h;
        cc()->has_dirty = true;
        return;
    }

    const int x2 = cc()->dirty_rect.x + cc()->dirty_rect.w;
    const int y2 = cc()->dirty_rect.y + cc()->dirty_rect.h;
    const int nx2 = x + w;
    const int ny2 = y + h;

    if (x < cc()->dirty_rect.x)
        cc()->dirty_rect.x = x;
    if (y < cc()->dirty_rect.y)
        cc()->dirty_rect.y = y;
    cc()->dirty_rect.w = (nx2 > x2 ? nx2 : x2) - cc()->dirty_rect.x;
    cc()->dirty_rect.h = (ny2 > y2 ? ny2 : y2) - cc()->dirty_rect.y;
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
        s_buf_debt[i].full = true;
        er_damage_set_clear(&s_buf_debt[i].set);
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
    /* A node inside a display:none subtree cannot paint, so changing it changes no pixels. Marking
     * it (or its ancestors) would be worse than useless: neither flag can be cleared by painting,
     * so a cached page that React keeps re-rendering would repaint its own rect on every commit —
     * and, once an ancestor up to the root is stuck dirty, the "nothing changed" commit path walks
     * unclipped and repaints the whole screen. propagate_hidden re-marks the subtree when it is
     * shown, which is when the change becomes visible. */
    if (node && node->subtree_hidden)
        return;

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
 * @brief Marks the chain for repaint WITHOUT claiming any damage for the node itself.
 *
 * The reflow case: the node's own appearance is unchanged and only its layout inputs moved, so it
 * has no pixels of its own to report. Skipping source_dirty is the whole point — the damage
 * pre-pass then bounds the repaint to whatever the layout pass actually moved (each node's new
 * screen rect versus where it was last painted) instead of the node's entire box.
 *
 * @param[in,out] node  Node whose ancestor chain should be invalidated.
 */
static void mark_reflow_upward(ERNode* node)
{
    s_content_gen++; /* a reflow does change rendered content, so the fade cache must drop */
    if (node && node->subtree_hidden)
        return; /* same reasoning as er_mark_dirty_upward_visual: a hidden node can never clear it */
    while (node)
    {
        node->dirty = true;
        node = er_get_node(node->parent_tag);
    }
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

void er_request_layout_pass(void)
{
    mark_layout_dirty();
}

/**
 * @brief Renders the background and border of a View-family node.
 *
 * Resolves per-corner radii, per-edge widths/colors, and border style from ERViewProps,
 * then dispatches to er_rrect_fill_bordered (fast uniform path) or the general per-corner/per-edge
 * path using er_rrect_fill_corners and er_rrect_fill_ring_edges.
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
        const int ix = px + bw_l;
        const int iy = py + bw_t;
        const int iw = w - bw_l - bw_r;
        const int ih = h - bw_t - bw_b;
        /* Inset radii: shrink each corner by the adjacent border width. */
        const int ir_tl = (r_tl > bw_l) ? r_tl - bw_l : 0;
        const int ir_tr = (r_tr > bw_r) ? r_tr - bw_r : 0;
        const int ir_br = (r_br > bw_r) ? r_br - bw_r : 0;
        const int ir_bl = (r_bl > bw_l) ? r_bl - bw_l : 0;

        if ((bg_color >> 24) == 0xFFu)
        {
            /* Uniform solid border with per-corner radii, opaque background: outer → background
             * inset. The background hides every border pixel it needs to. */
            er_rrect_fill_corners(bc_l, px, py, w, h, r_tl, r_tr, r_br, r_bl);
            if (iw > 0 && ih > 0)
                er_rrect_fill_corners(bg_color, ix, iy, iw, ih, ir_tl, ir_tr, ir_br, ir_bl);
        }
        else
        {
            /* Transparent, translucent, or gradient-backed: a filled border shape would cover what
             * is behind it (see er_rrect_fill_ring), so stroke the band only. */
            er_rrect_fill_ring(bc_l, px, py, w, h, r_tl, r_tr, r_br, r_bl, bw_l);
            if ((bg_color >> 24) != 0 && iw > 0 && ih > 0)
                er_rrect_fill_corners(bg_color, ix, iy, iw, ih, ir_tl, ir_tr, ir_br, ir_bl);
        }
    }
    else
    {
        /* Per-edge widths/colours and/or a dashed or dotted style. The background fills the whole
         * rounded shape (a transparent or translucent edge shows it through, as it does on the web),
         * then the band is stroked on top following the same corners — including the dash pattern,
         * which is stepped by arc length so it flows through them. Drawn as four straight rects — as
         * this used to be — the border ignored borderRadius entirely and squared off every corner. */
        er_rrect_fill_corners(bg_color, px, py, w, h, r_tl, r_tr, r_br, r_bl);
        const ERRRectBorder border = {bw_l, bw_t, bw_r, bw_b, bc_l, bc_t, bc_r, bc_b, vp->border_style};
        if (has_border)
            er_rrect_fill_ring_edges(px, py, w, h, r_tl, r_tr, r_br, r_bl, &border);
    }
}

/**
 * @brief Pads a damage rect for anti-aliased / sub-pixel edges, clamps it to the root rect, adds it.
 *
 * Padding happens HERE, before insertion, so the set's disjointness invariant holds for the final
 * geometry — two rects 3 px apart that would overlap only after padding are coalesced instead of
 * stored "disjoint" and later double-painted. (Rects within 2×margin of each other auto-merge.)
 *
 * @param[in,out] s    Damage set to add to.
 * @param[in]     x,y,w,h  Raw damage rectangle (screen space).
 * @param[in]     rx0,ry0  Root rect top-left (clamp bounds).
 * @param[in]     rx1,ry1  Root rect bottom-right exclusive (clamp bounds).
 */
static void add_damage(ERDamageSet* s, int x, int y, int w, int h, int rx0, int ry0, int rx1, int ry1)
{
    if (w <= 0 || h <= 0)
        return;
    const int margin = 2;
    int cx0 = x - margin;
    int cy0 = y - margin;
    int cx1 = x + w + margin;
    int cy1 = y + h + margin;
    if (cx0 < rx0)
        cx0 = rx0;
    if (cy0 < ry0)
        cy0 = ry0;
    if (cx1 > rx1)
        cx1 = rx1;
    if (cy1 > ry1)
        cy1 = ry1;
    er_damage_set_add(s, cx0, cy0, cx1 - cx0, cy1 - cy0); /* empty after clamping: ignored */
}

/**
 * @brief Sums the scroll offsets of every ScrollView / FlatList above a node.
 *
 * render_tree carries this down the walk as its translation; the flat per-node damage passes have no
 * parent context, so they re-derive it here.
 *
 * @param[in]  n       Node to measure from (its own scroll offset is NOT included).
 * @param[out] sx,sy   Receive the accumulated ancestor scroll.
 */
static void node_ancestor_scroll(const ERNode* n, int* sx, int* sy)
{
    *sx = 0;
    *sy = 0;
    const ERNode* a = er_get_node(n->parent_tag);
    while (a)
    {
        if (a->type == ER_NODE_SCROLL_VIEW || a->type == ER_NODE_FLAT_LIST)
        {
            *sx += (int)a->scroll_offset_x;
            *sy += (int)a->scroll_offset_y;
        }
        a = er_get_node(a->parent_tag);
    }
}

/**
 * @brief The transformed ancestor whose scratch capture this node's pixels are painted into, if any.
 *
 * A node under a full transform does NOT paint itself onto the screen: render_tree captures the
 * transformed ancestor's whole subtree into the transform scratch in SOURCE space, then inverse-maps
 * that scratch out at the ancestor's transformed AABB. Every measurement the damage pre-pass can make
 * of the inner node — its screen rect, and last_paint_rect itself — is plain layout-minus-scroll and
 * knows nothing of the ancestor's matrix, so damaging it scissors the re-capture and the blit to a
 * region the changed pixels never land in and the change is simply lost (issue #143). The fix is to
 * escalate such damage to the ancestor, which is the only node here that measures in screen space.
 *
 * Only ONE capture can be active at a time (er_transform_source_begin refuses a second), so of a
 * chain of transformed ancestors it is the OUTERMOST one render_tree ADMITS that captures — every
 * transform nested inside that one is refused and painted untransformed into its capture. Hence the
 * walk runs all the way to the root and keeps the last match rather than stopping at the first.
 *
 * "Admits" means BOTH halves of render_tree's test, not just the size one. A transform that does not
 * invert has no inverse-map blit, so it paints untransformed at its layout box and the capture passes
 * DOWN to the next transform inside it — and an outer ancestor counted as capturing on size alone
 * would take the escalation while the pixels landed at the inner one's AABB, which is the same class
 * of miss this path exists to fix. er_transform_is_invertible() answers with the exact rule the blit's
 * own inverse applies. Occlusion, the third thing that stops a capture, is deliberately not asked: an
 * occluded ancestor shows nothing, so escalating to it can only over-damage.
 *
 * Not a free predicate: it walks the ancestor chain, and pays for a matrix at each TRANSFORMED
 * ancestor on it (usually none at all). So it is asked only of nodes actually contributing damage this
 * commit, never of the idle majority the pre-pass sweeps past.
 *
 * @param[in] n  Node whose ancestors to search.
 *
 * @return The capturing ancestor, or NULL when this node paints straight to the screen.
 */
static ERNode* capturing_transform_ancestor(const ERNode* n)
{
#if ERUI_TRANSFORMS_FULL
    ERNode* cap = NULL;
    for (ERNode* a = er_get_node(n->parent_tag); a; a = er_get_node(a->parent_tag))
    {
        if (!a->has_transform || a->type == ER_NODE_ACTIVITY_INDICATOR || er_transform_is_translate_only(a)
            || !er_transform_source_fits((int)a->animated.w, (int)a->animated.h))
            continue;
        /* The same pre-transform origin render_tree hands the matrix: layout position minus ancestor
         * scroll and the keyboard shift. Only the 3D homography's pivot actually depends on it, but
         * deriving it any other way here would be a second rule to keep in step. */
        int sx, sy;
        node_ancestor_scroll(a, &sx, &sy);
        if (er_transform_is_invertible(a,
                                       (int)a->animated.x - sx,
                                       (int)a->animated.y - sy - s_kbd_avoid_y,
                                       (int)a->animated.w,
                                       (int)a->animated.h))
            cap = a;
    }
    return cap;
#else
    (void)n;
    return NULL;
#endif
}

/**
 * @brief Registers pixels a node is vacating, in the space they were actually painted in.
 *
 * The rect a node leaves behind is its last-painted one — except under a transformed ancestor, where
 * the node never painted to the screen at all: it painted into that ancestor's capture in source
 * space, and what reached the panel is the ancestor's inverse-map blit. Erasing the source-space rect
 * repaints an unrelated region and leaves the vacated pixels on screen (issue #143), so the region
 * that blit last wrote is vacated instead.
 *
 * Whether the ancestor really captured is not predicted here: last_paint_untransformed records what
 * the previous paint actually did, and it is the previous paint whose pixels are being vacated.
 *
 * @param[in] n         Node vacating the pixels (used to find its capturing ancestor, if any).
 * @param[in] x,y,w,h   Screen rect being vacated, as measured for @p n itself.
 */
static void note_vacated_rect(const ERNode* n, int x, int y, int w, int h)
{
    const ERNode* cap = capturing_transform_ancestor(n);
    if (cap && cap->has_last_paint && !cap->last_paint_untransformed)
    {
        x = (int)cap->last_paint_rect.x;
        y = (int)cap->last_paint_rect.y;
        w = (int)cap->last_paint_rect.w;
        h = (int)cap->last_paint_rect.h;
    }
    er_damage_set_add(&s_removed_set, x, y, w, h);
}

/**
 * @brief Accumulates a removed subtree's last-painted rects into the pending removal damage.
 *
 * Called while the subtree is still intact (before detach). The next er_commit() seeds its damage set
 * from this so the vacated pixels are repainted (erased) without forcing a full-screen redraw.
 *
 * @param[in] n  Root of the subtree being removed.
 */
static void note_removed_subtree(ERNode* n)
{
    if (!n)
        return;
    if (n->has_last_paint)
    {
        note_vacated_rect(n,
                          (int)n->last_paint_rect.x,
                          (int)n->last_paint_rect.y,
                          (int)n->last_paint_rect.w,
                          (int)n->last_paint_rect.h);
        n->has_last_paint = false;
    }
    uint16_t c = n->first_child_tag;
    while (c != ER_INVALID_TAG)
    {
        ERNode* ch = er_get_node(c);
        if (!ch)
            break;
        note_removed_subtree(ch);
        c = ch->next_sibling_tag;
    }
}

/**
 * @brief Recomputes ERNode::subtree_hidden for a subtree and settles its damage bookkeeping.
 *
 * A display:none node is pruned from layout, from render_tree and from hit-testing, so its
 * DESCENDANTS silently stop being maintained: layout leaves their computed rects frozen and the
 * paint walk never reaches them. The damage pre-pass, which measures each node on its own, then
 * reads them as unchanged-and-in-place and they contribute nothing — so a descendant that painted
 * outside the hidden node's own box would stay on screen forever, and one that was dirty when the
 * subtree went away would keep a flag it can never clear (a rect re-damaged on every commit for the
 * rest of the run). Both are settled here, once per transition rather than per frame:
 *
 *   - hiding: register each node's last painted rect as vacated — the same channel node removal
 *     uses — then drop the stale trail and retire the dirty flags,
 *   - showing: mark each node dirty so the pre-pass unions its rect back into the damage and the
 *     paint walk repaints it.
 *
 * Nothing about the nodes themselves changes: they keep their tags, props and geometry, which is
 * the whole point of hiding a page instead of unmounting it. Nested display:none subtrees stay
 * hidden when an ancestor is shown, because each level ORs in its own display.
 *
 * @param[in,out] n                Subtree root to walk (NULL is ignored).
 * @param[in]     ancestor_hidden  Whether an ancestor of @p n is (or is inside) a display:none node.
 */
static void propagate_hidden(ERNode* n, bool ancestor_hidden)
{
    if (!n)
        return;

    const bool hidden = ancestor_hidden || (n->layout.display == ER_DISPLAY_NONE);
    if (hidden != n->subtree_hidden)
    {
        n->subtree_hidden = hidden;
        if (hidden)
        {
            if (n->has_last_paint)
            {
                note_vacated_rect(n,
                                  (int)n->last_paint_rect.x,
                                  (int)n->last_paint_rect.y,
                                  (int)n->last_paint_rect.w,
                                  (int)n->last_paint_rect.h);
                n->has_last_paint = false;
            }
        }
        else
        {
            n->dirty = true;
            n->source_dirty = true;
        }
    }

    /* Recurse unconditionally: an unchanged flag here says nothing about a subtree that was just
     * spliced in from somewhere else (append/insert re-runs this from the new parent). */
    for (uint16_t c = n->first_child_tag; c != ER_INVALID_TAG;)
    {
        ERNode* ch = er_get_node(c);
        if (!ch)
            break;
        propagate_hidden(ch, hidden);
        c = ch->next_sibling_tag;
    }
}

/** @brief True when @p n sits under a display:none ancestor (false for a node with no parent). */
static bool parent_hidden(const ERNode* n)
{
    const ERNode* p = er_get_node(n->parent_tag);
    return p && p->subtree_hidden;
}

/**
 * @brief True when @p n confines its descendants' paint to its own box.
 *
 * Either an explicit clipping overflow, or a node type that always scrolls its content behind a
 * viewport. The distinction drives three separate things — the scissor render_tree pushes, the
 * subtree paint bounds, and how far a descendant's damage may reach — which must agree exactly, so
 * they all ask here rather than each spelling the predicate out.
 */
static bool node_clips_children(const ERNode* n)
{
    return n->layout.overflow == ER_OVERFLOW_HIDDEN || n->layout.overflow == ER_OVERFLOW_SCROLL
           || n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_FLAT_LIST;
}

/**
 * @brief Grows a rect by however far this node's Arc knob reaches past its layout box.
 *
 * A knob wider than the ring paints outside the box, so any rect meant to bound what the node PUTS ON
 * SCREEN — its damage, its prune bounds, its last-paint trail, the pre-pass footprint compared against
 * that trail — has to include the reach or the knob is clipped and a move leaves it behind. The pre-pass
 * compares its answer to last_paint_rect for exact equality, so every site must inflate identically:
 * that is why this lives in one place. No-op for anything but an Arc, or an Arc with no overhang.
 *
 * Not applied on the affine path: a rotated/scaled Arc renders through a `w × h` transform scratch and
 * is bounded by the transformed layout box, so its knob is clipped there (see engine/README.md).
 *
 * @param[in]     n        Node to measure; its cached ERNode::arc_overhang is refreshed as a side effect.
 * @param[in,out] x,y,w,h  Rect grown in place by the knob's reach on every side.
 */
static void expand_for_arc(ERNode* n, int* x, int* y, int* w, int* h)
{
    if (n->type != ER_NODE_ARC)
        return;
    const int over = er_arc_refresh_overhang(n);
    *x -= over;
    *y -= over;
    *w += 2 * over;
    *h += 2 * over;
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
    int sx, sy;
    node_ancestor_scroll(n, &sx, &sy);
    *rx = (int)n->animated.x - sx + (int)n->tp_translate_x;
    *ry = (int)n->animated.y - sy + (int)n->tp_translate_y - s_kbd_avoid_y;
    *rw = (int)n->animated.w;
    *rh = (int)n->animated.h;
    return true;
}

#if ERUI_TRANSFORMS_FULL
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
 * This is the expensive half of the pre-pass — two sinf/cosf on the affine path, six plus up to eight
 * projective divides on the 3D one — so node_transform_damage() reaches it only after the cheap size test
 * has said a capture can actually start. The gates that used to live at the top of this function (no
 * transform, translate-only, ActivityIndicator) are the caller's now, and the ancestor-scroll walk is
 * passed in so the two rect helpers share one.
 *
 * Returns false (leaving the caller on its full-repaint fallback) for a degenerate or off-screen-projected
 * (zero-area) result — including a 3D node whose corners all fall behind the perspective plane.
 *
 * @param[in]  n             Node to measure.
 * @param[in]  sx,sy         Accumulated ancestor scroll, from node_ancestor_scroll().
 * @param[out] rx,ry,rw,rh   Receive the node's transformed screen AABB.
 *
 * @return true if a finite AABB was produced; false to fall back to a full repaint.
 */
static bool node_transformed_screen_rect(const ERNode* n, int sx, int sy, int* rx, int* ry, int* rw, int* rh)
{
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
}

/**
 * @brief Computes the raw, untransformed screen box of a transformed node.
 *
 * The footprint render_tree degrades to when it cannot start the node's scratch capture: same origin as
 * node_transformed_screen_rect (layout position minus ancestor scroll and the keyboard shift), but the
 * node's own w×h rather than a projected AABB, and no translate folded in — the fallback paints the node
 * exactly where it would sit with no transform at all. An Arc's knob reach is added for the same reason
 * render_tree adds it to last_paint_rect on that path: it is part of what the fallback puts on screen.
 *
 * @param[in]  n             Node to measure.
 * @param[in]  sx,sy         Accumulated ancestor scroll, from node_ancestor_scroll().
 * @param[out] rx,ry,rw,rh   Receive the node's untransformed screen box.
 */
static void node_untransformed_screen_rect(ERNode* n, int sx, int sy, int* rx, int* ry, int* rw, int* rh)
{
    *rx = (int)n->animated.x - sx;
    *ry = (int)n->animated.y - sy - s_kbd_avoid_y;
    *rw = (int)n->animated.w;
    *rh = (int)n->animated.h;
    expand_for_arc(n, rx, ry, rw, rh);
}
#endif /* ERUI_TRANSFORMS_FULL */

/** @brief What the damage pre-pass expects a full-transform node's next paint to cover. */
typedef struct
{
    int fx, fy, fw, fh; /**< The footprint itself: either the transformed AABB or the raw box. */
    int ax, ay, aw, ah; /**< The transformed AABB — meaningful only when `hedge`. */
    bool raw;           /**< The footprint is the raw, untransformed box (so it carries a shadow). */
    bool hedge;         /**< Damage the AABB alongside the footprint: the raw prediction may be stale. */
} NodeTransformDamage;

/**
 * @brief Bounds the next paint of a node carrying a transform the fast path cannot express.
 *
 * render_tree paints such a node by capturing its subtree into the transform scratch and inverse-mapping
 * it out; when the capture cannot be started it falls back to painting the node untransformed at its
 * layout box. The damage pre-pass has to make the SAME call or it measures a footprint the paint never
 * uses: last_paint_rect then holds the box while the pre-pass computes an AABB, they can never agree,
 * `moved` latches true, and the node damages and re-reports its own region on every commit for as long
 * as it exists.
 *
 * Two things stop a capture, and only one of them is visible from here:
 *   - the node is larger than the transform source (or degenerate) — er_transform_source_fits() is the
 *     exact test er_transform_source_begin() admits on, so this is a prediction, not a guess;
 *   - a capture is already active for a transformed ancestor — not a property of this node's geometry.
 *     The flag the last paint left behind stands in for it. That is a guess: it can go stale (the
 *     ancestor's transform may be gone this commit), so `hedge` asks the caller to damage the
 *     transformed AABB alongside the box.
 *
 * The size test is four integer compares and it is asked FIRST, because when it fails the AABB is dead:
 * the footprint is the raw box and the uncertain-fallback hedge cannot fire. Computing the matrix before
 * asking spent two sinf/cosf (six, plus up to eight projective divides, in 3D) per node per commit —
 * including on fully idle commits — to produce a rect that was then thrown away.
 *
 * @param[in]  n   Node to measure.
 * @param[out] d   Receives the footprint (and, when `hedge`, the AABB to damage with it).
 *
 * @return true if the paint could be bounded; false to leave the caller on its full-repaint fallback.
 */
static bool node_transform_damage(ERNode* n, NodeTransformDamage* d)
{
#if ERUI_TRANSFORMS_FULL
    /* Not a transform this path owns. The ActivityIndicator is excluded first and deliberately: its
     * rotate_z is an internal spin, not an affine render, so it must still reach the full-repaint
     * fallback rather than be measured by either rect helper. */
    if (!n->has_transform || er_transform_is_translate_only(n) || n->type == ER_NODE_ACTIVITY_INDICATOR)
        return false;

    /* One walk, shared by both helpers below. */
    int sx, sy;
    node_ancestor_scroll(n, &sx, &sy);

    d->hedge = false;
    if (!er_transform_source_fits((int)n->animated.w, (int)n->animated.h))
    {
        /* Decisive: er_transform_source_begin() admits on size alone, so this node paints its raw box.
         * No matrix, no AABB — and no full-repaint fallback either, even for a degenerate transform,
         * because bounded damage matching the actual paint beats a conservative whole-screen repaint. */
        d->raw = true;
        node_untransformed_screen_rect(n, sx, sy, &d->fx, &d->fy, &d->fw, &d->fh);
        return true;
    }

    if (!node_transformed_screen_rect(n, sx, sy, &d->ax, &d->ay, &d->aw, &d->ah))
        return false;

    d->raw = n->has_last_paint && n->last_paint_untransformed;
    if (d->raw)
    {
        /* Carried over from the last paint, not predicted from size, so the capture may succeed this
         * commit and paint the AABB instead. Damage both rather than risk scissoring away its own paint. */
        d->hedge = true;
        node_untransformed_screen_rect(n, sx, sy, &d->fx, &d->fy, &d->fw, &d->fh);
    }
    else
    {
        d->fx = d->ax;
        d->fy = d->ay;
        d->fw = d->aw;
        d->fh = d->ah;
    }
    return true;
#else
    (void)n;
    (void)d;
    return false;
#endif
}

/**
 * @brief Computes the screen rect of a node's cached subtree paint bounds (sub_x/y/w/h).
 *
 * The bounds are kept in computed space by compute_subtree_bounds(), so they are re-anchored here by
 * the same offset that separates the node's own computed box from its screen box — ancestor scroll,
 * the translate transform and the keyboard shift, all of which apply to the descendants too.
 *
 * @param[in]  n             Node whose subtree bounds to measure.
 * @param[out] rx,ry,rw,rh   Receive the subtree's screen rectangle.
 *
 * @return true if the rect was computed; false for a transform node_screen_rect() cannot bound.
 */
static bool node_subtree_screen_rect(const ERNode* n, int* rx, int* ry, int* rw, int* rh)
{
    int bx, by, bw, bh;
    if (!node_screen_rect(n, &bx, &by, &bw, &bh))
        return false;
    *rx = (int)n->sub_x + (bx - (int)n->computed.x);
    *ry = (int)n->sub_y + (by - (int)n->computed.y);
    *rw = (int)n->sub_w;
    *rh = (int)n->sub_h;
    return true;
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
        const bool clips = node_clips_children(a);
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

#if ERUI_SHADOWS
/**
 * @brief Grows a rect by however far this node's shadow bleeds past its layout box.
 *
 * A shadow is painted outside the box, so any rect meant to bound what the node PUTS ON SCREEN — its
 * damage, its erase trail, its reported repaint — has to include the bleed or the old shadow is never
 * erased and the new one is clipped at the damage edge. No-op for a node that casts none.
 */
static void expand_for_shadow(const ERNode* n, int* x, int* y, int* w, int* h)
{
    if (!(n->type == ER_NODE_VIEW || n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_PRESSABLE
          || (n->type == ER_NODE_MODAL && n->modal_visible)))
        return;
    if (!(n->props.view.shadow_opacity > 0.0f || n->props.view.elevation > 0))
        return;
    const int r = (int)n->props.view.shadow_radius;
    const int ox = (int)(fabsf(n->props.view.shadow_offset_x) + 0.5f);
    const int oy = (int)(fabsf(n->props.view.shadow_offset_y) + 0.5f);
    const int exp = r + (ox > oy ? ox : oy);
    *x -= exp;
    *y -= exp;
    *w += 2 * exp;
    *h += 2 * exp;
}
#endif

/**
 * @brief Reports a rect as repainted, clamped to the root the way add_damage clamps its insert.
 *
 * For the contributions render_tree's own accumulator cannot make. That accumulator unions only
 * SOURCE-dirty nodes, so anything repainted for a reason other than its own content changing —
 * pixels a removed node vacated, a modal scrim, a node that merely MOVED — has to report itself
 * here, or a host that transfers only er_get_dirty_rect() leaves those pixels stale on the panel.
 */
static void report_repaint_clamped(int x, int y, int w, int h, int rb_x0, int rb_y0, int rb_x1, int rb_y1)
{
    const int cx0 = (x > rb_x0) ? x : rb_x0;
    const int cy0 = (y > rb_y0) ? y : rb_y0;
    const int cx1 = (x + w < rb_x1) ? (x + w) : rb_x1;
    const int cy1 = (y + h < rb_y1) ? (y + h) : rb_y1;
    union_dirty_rect(cx0, cy0, cx1 - cx0, cy1 - cy0);
}

/**
 * @brief Damages, reports and retires the root-wide backdrop of a changed Modal.
 *
 * A modal's scrim covers the whole ROOT, not the modal's own box, so that is what a changed modal
 * repaints — measuring it by either rect helper would scissor the scrim to the box and leave the rest
 * of the page uncovered on show, or still scrimmed after hide.
 *
 * Shared by BOTH pre-pass branches on purpose: which helper managed to measure the node's rect says
 * nothing about how far its backdrop reaches, so a modal carrying a scale/rotate transform owes
 * exactly the same root-wide damage as a plain one. It used to be inline in the translate-only
 * branch, which a transformed modal never reaches — it scrimmed only its own footprint on show, and
 * on hide erased only that footprint while the flag stayed latched for the node's lifetime.
 *
 * The scrim flag carries the hide case, where modal_visible has already gone but the old scrim is
 * still on screen; it is retired here because this damage is what erases it.
 *
 * Reported as well as damaged, and it has to happen HERE rather than in render_tree's accumulator:
 * on the hide commit the modal is display:none, so the walk never reaches it and nothing would
 * contribute at all. Callers reach this only for a modal that is source_dirty or moved, so a steady
 * on-screen modal still costs nothing and a change to one of its CHILDREN keeps its own tight damage.
 *
 * @param[in]     n    Modal node, already known to be source-dirty or moved.
 * @param[in,out] dmg  Damage set to add the root rect to.
 * @param[in]     rb_x0,rb_y0,rb_x1,rb_y1  Root bounds — both the rect damaged and the clamp.
 */
static void modal_scrim_damage(ERNode* n, ERDamageSet* dmg, int rb_x0, int rb_y0, int rb_x1, int rb_y1)
{
    add_damage(dmg, rb_x0, rb_y0, rb_x1 - rb_x0, rb_y1 - rb_y0, rb_x0, rb_y0, rb_x1, rb_y1);
    union_dirty_rect(rb_x0, rb_y0, rb_x1 - rb_x0, rb_y1 - rb_y0);
    if (!n->modal_visible)
        n->modal_scrim_shown = 0U;
}

/**
 * @brief Damages the region a capturing ancestor's blit writes, standing in for a descendant's own.
 *
 * The ancestor's transformed AABB bounds everything its subtree can put on screen — the capture is
 * exactly its w x h, so content laid out past that box is clipped away by the capture itself — which
 * makes one rect the correct (and only correct) damage for a change anywhere inside it. This is the
 * ancestor's own footprint, computed the same way node_transform_damage() computes it for the
 * ancestor's sake, hedge included: the capture may fail this commit and degrade to the raw box, and
 * damaging only one of the two would risk scissoring away the paint that actually happens.
 *
 * The ancestor's last-paint TRAIL is deliberately not added. It is only stale when the ancestor
 * itself moved, and then the ancestor contributes it through its own pass of the pre-pass.
 *
 * @param[in]     cap        Capturing ancestor, from capturing_transform_ancestor().
 * @param[in,out] dmg        Damage set to add to.
 * @param[in]     report     Report the region as repainted too (for a descendant that is not
 *                           source_dirty, so render_tree's accumulator will never report it).
 * @param[in]     rb_x0,rb_y0,rb_x1,rb_y1  Root bounds the insert is clamped to.
 *
 * @return true when the ancestor's footprint was damaged; false when it could not be bounded, in
 *         which case the caller falls back to measuring the descendant itself.
 */
static bool
escalate_damage_to_capture(ERNode* cap, ERDamageSet* dmg, bool report, int rb_x0, int rb_y0, int rb_x1, int rb_y1)
{
    NodeTransformDamage td = {0};
    if (!node_transform_damage(cap, &td))
        return false;

    int nx = td.fx, ny = td.fy, nw = td.fw, nh = td.fh;
#if ERUI_SHADOWS
    /* Same rule as the ancestor's own pass: only the raw-box fallback casts a shadow. */
    if (td.raw)
        expand_for_shadow(cap, &nx, &ny, &nw, &nh);
#endif
    clip_rect_to_clippers(cap, &nx, &ny, &nw, &nh);
    add_damage(dmg, nx, ny, nw, nh, rb_x0, rb_y0, rb_x1, rb_y1);
    if (report)
        report_repaint_clamped(nx, ny, nw, nh, rb_x0, rb_y0, rb_x1, rb_y1);

    if (td.hedge)
    {
        int ax = td.ax, ay = td.ay, aw = td.aw, ah = td.ah;
        clip_rect_to_clippers(cap, &ax, &ay, &aw, &ah);
        add_damage(dmg, ax, ay, aw, ah, rb_x0, rb_y0, rb_x1, rb_y1);
        if (report)
            report_repaint_clamped(ax, ay, aw, ah, rb_x0, rb_y0, rb_x1, rb_y1);
    }
    return true;
}

/* Set per-commit: true when the cached subtree bounds are trustworthy this frame (no layout
 * animation interpolating positions), so render_tree() may use them to skip untouched subtrees. */
static bool s_prune_ok = false;

/* Damage-rect budget used for the render passes when subtree pruning is off. Without pruning every
 * clipped pass walks the whole tree, so the fixed per-pass cost — not the damage area — dominates and
 * the full ER_DAMAGE_RECTS_MAX budget would multiply it. Coarser rects repaint a little extra area
 * instead, which is the cheaper half of that trade during a layout animation (where the whole scene
 * is moving anyway, so the damage is already broad). Trimming only ever lowers the count, and the
 * coarsened rects still cover every changed pixel. */
#define UNPRUNED_PASS_RECTS_MAX 4U

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
    /* A knob wider than the ring paints past the box: widen the prune bounds by its reach. */
    int aw = x1 - x0, ah = y1 - y0;
    expand_for_arc(n, &x0, &y0, &aw, &ah);
    x1 = x0 + aw;
    y1 = y0 + ah;

    const bool clips = node_clips_children(n);

    uint16_t child_tag = n->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* c = er_get_node(child_tag);
        if (!c)
            break;
        if (c->layout.display == ER_DISPLAY_NONE)
        {
            /* A hidden subtree paints nothing, so it must not widen this node's prune bounds — its
             * own box has collapsed to the origin and unioning that in would stretch the parent's
             * bounds back to (0,0) and defeat the pruning for everything around it. */
            child_tag = c->next_sibling_tag;
            continue;
        }
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
static uint32_t s_prof_content_us = 0; /* subtree render time inside composites */
static uint32_t s_prof_blend_us = 0;   /* strip pop_blend time */
static uint32_t s_prof_push_us = 0;    /* strip push (clear) time */
static uint32_t s_prof_passes = 0;     /* band passes */
static uint32_t s_prof_composites = 0; /* composite_with_opacity calls that composited */
#define ER_PROF_MARK(var) const uint32_t var = er_prof_now_us()
#define ER_PROF_ACC(acc, from) (acc += er_prof_now_us() - (from))
#else
#define ER_PROF_MARK(var)
#define ER_PROF_ACC(acc, from)
#endif

/* Frame instrumentation (er_perf.h): the commit reports its own layout/raster split and the region it
 * repainted, so a spike can be attributed without the host guessing. The entry points are no-op stubs
 * when ER_PERF_STATS == 0, but the macros drop even the calls so a disabled build is byte-identical. */
#if ER_PERF_STATS
#define ER_PERF_BEGIN(phase) er_perf_phase_begin(phase)
#define ER_PERF_END(phase) er_perf_phase_end(phase)
#define ER_PERF_REPAINT(x, y, w, h) er_perf_note_repaint((x), (y), (w), (h))
#define ER_PERF_RASTER_BEGIN(sub) er_perf_raster_begin(sub)
#define ER_PERF_RASTER_END(sub) er_perf_raster_end(sub)
/* Bracket the raster phase's backend-blit accounting: reset the per-worker accumulators when the
 * phase opens (dropping anything a host blitted between commits, e.g. its own overlay panel), and
 * report the sums once the render passes have joined — the only point where reading the worker
 * accumulators is race-free. */
#define ER_PERF_BLIT_RESET() er_blit_perf_reset()
#define ER_PERF_BLIT_COLLECT()                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        uint32_t blit_us_, blit_px_;                                                                                   \
        er_blit_perf_collect(&blit_us_, &blit_px_);                                                                    \
        er_perf_note_blit(blit_us_, blit_px_);                                                                         \
    } while (0)
#else
#define ER_PERF_BEGIN(phase) ((void)0)
#define ER_PERF_END(phase) ((void)0)
#define ER_PERF_REPAINT(x, y, w, h) ((void)0)
#define ER_PERF_RASTER_BEGIN(sub) ((void)0)
#define ER_PERF_RASTER_END(sub) ((void)0)
#define ER_PERF_BLIT_RESET() ((void)0)
#define ER_PERF_BLIT_COLLECT() ((void)0)
#endif

static void render_tree(ERNode* n, bool parent_dirty, bool occluded, int translate_x, int translate_y);

/**
 * @brief Renders a node's own content and recurses into its children.
 *
 * This is the repeatable "body" of render_tree: the per-type draw switch, the overflow
 * scissor push, and the z-sorted child recursion. It has no side effects on n's dirty
 * flags (except one-shot consumption of vec_has_dirty by the vector rasterizer), so the
 * banded opacity compositor can invoke it once per strip tile.
 *
 * @param[in] n              Node to render.
 * @param[in] needs_paint    true when the node itself (not only descendants) must repaint.
 * @param[in] occluded       true when an opaque node painted later already covers this whole
 *                           region, so nothing here can be seen. The subtree is still walked (its
 *                           paint bookkeeping has to stay in step) but emits no pixels.
 * @param[in] px             Screen-space left edge (scroll translation applied).
 * @param[in] py             Screen-space top edge.
 * @param[in] w              Node width in pixels.
 * @param[in] h              Node height in pixels.
 * @param[in] translate_x    Accumulated horizontal scroll offset for children.
 * @param[in] translate_y    Accumulated vertical scroll offset for children.
 */
static void render_node_content(
    ERNode* n, bool needs_paint, bool occluded, int px, int py, int w, int h, int translate_x, int translate_y);

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
static bool
composite_from_cache(ERNode* n, uint8_t alpha, int px, int py, int w, int h, int translate_x, int translate_y)
{
#if ER_FADE_CACHE_ENABLED
    if (cc()->tile_active || !er_scratch_idle() || er_get_draw_alpha() != 255U)
        return false;
    if (w <= 0 || h <= 0 || w > ERUI_FADE_CACHE_W || h > ERUI_FADE_CACHE_H)
        return false;

    /* The cache buffer is the node BOX. A subtree that paints past it does not fit, and capturing it
     * anyway would clip those descendants away — the defect composite_with_opacity's region union
     * fixes. Such a group takes the strip pool instead. */
    if ((int)n->sub_x - translate_x < px || (int)n->sub_y - translate_y < py
        || (int)n->sub_x - translate_x + (int)n->sub_w > px + w || (int)n->sub_y - translate_y + (int)n->sub_h > py + h)
        return false;

    /* Cache HIT: a read-only blend, clipped by the active scissor — safe under any scissor
     * (a partial repaint blends just the damaged part) and from any render worker (slices blend
     * disjoint rows of the same cached content). Checked before the full-coverage gate below,
     * which only capture needs. */
    if (s_fade_cache_tag == n->tag && s_fade_cache_gen == s_content_gen && s_fade_cache_w == w && s_fade_cache_h == h)
    {
        er_blit_blend(s_fade_cache, ERUI_FADE_CACHE_W * (int)sizeof(uint32_t), alpha, px, py, w, h);
        return true;
    }

    /* Capture writes the one shared cache buffer — never safe from inside a parallel slice.
     * Signal er_commit instead: if the content generation is still the same next commit (a pure
     * opacity animation), it renders that one frame single-core so the capture can happen, and
     * every following parallel frame takes the hit path above. Content that mutates every frame
     * (the generation moves) never requests the serial frame — those scenes gain more from
     * staying parallel than from a cache that would be stale immediately. */
    if (s_parallel_render)
    {
        /* Avoid concurrent writes: any worker may notice the need, but only worker 0 records it. */
        if (er_render_worker_id() == 0)
        {
            s_fade_capture_wanted = true;
            s_fade_capture_wanted_gen = s_content_gen;
        }
        return false;
    }
    int cx, cy, cw, ch;
    if (er_get_clip_rect(&cx, &cy, &cw, &ch) && (cx > px || cy > py || cx + cw < px + w || cy + ch < py + h))
        return false;

    if (s_fade_capture_commit == s_commit_seq)
        return false;
    s_fade_capture_commit = s_commit_seq;

    for (int row = 0; row < h; row++)
        memset(s_fade_cache + (size_t)row * ERUI_FADE_CACHE_W, 0, (size_t)w * sizeof(uint32_t));
    er_scratch_push_base(s_fade_cache, ERUI_FADE_CACHE_W, ERUI_FADE_CACHE_H, px, py);
    render_node_content(n, true, false, px, py, w, h, translate_x, translate_y);
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
static bool
composite_with_opacity(ERNode* n, uint8_t alpha, int px, int py, int w, int h, int translate_x, int translate_y)
{
    if (!er_scratch_avail())
        return false;

    /* The group's own footprint: the node box UNIONED with the subtree's cached paint bounds, so a
     * child that lays out past its parent's box is composited with the group instead of being clipped
     * out of existence. Bounding the region to the node box alone did exactly that — and because the
     * clipped-away descendants were then never reached by the walk, they also never had painted_seq
     * stamped, so their dirty flags survived every commit. A later commit in which the group itself was
     * clean would find such an orphan dirty on its own, with no dirty ancestor to composite it, and
     * paint it straight into the framebuffer at full alpha (see tests/rendering/test_opacity_equiv.c).
     * sub_* is computed-space (compute_subtree_bounds); the node box comes from `animated`, so union
     * both rather than trusting either alone while a layout animation interpolates. */
    int rx = px, ry = py, rx1 = px + w, ry1 = py + h;
    {
        const int sx0 = (int)n->sub_x - translate_x;
        const int sy0 = (int)n->sub_y - translate_y;
        const int sx1 = sx0 + (int)n->sub_w;
        const int sy1 = sy0 + (int)n->sub_h;
        if (sx0 < rx)
            rx = sx0;
        if (sy0 < ry)
            ry = sy0;
        if (sx1 > rx1)
            rx1 = sx1;
        if (sy1 > ry1)
            ry1 = sy1;
    }
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
    if (cc()->tile_active)
    {
        if (cc()->tile_x > rx)
            rx = cc()->tile_x;
        if (cc()->tile_y > ry)
            ry = cc()->tile_y;
        if (cc()->tile_x + cc()->tile_w < rx1)
            rx1 = cc()->tile_x + cc()->tile_w;
        if (cc()->tile_y + cc()->tile_h < ry1)
            ry1 = cc()->tile_y + cc()->tile_h;
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
        render_node_content(n, true, false, px, py, w, h, translate_x, translate_y);
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
    const bool outer_active = cc()->tile_active;
    const int ox = cc()->tile_x, oy = cc()->tile_y, ow = cc()->tile_w, oh = cc()->tile_h;
    const uint32_t outer_gen = cc()->cur_loop_gen;
    const bool outer_first = cc()->tile_first;
    cc()->cur_loop_gen = ++cc()->band_gen_counter;
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

                cc()->tile_active = true;
                cc()->tile_x = bx;
                cc()->tile_y = by;
                cc()->tile_w = bw;
                cc()->tile_h = bh;
                cc()->tile_first = first;
                er_push_clip_rect(bx, by, bw, bh);
                render_node_content(n, true, false, px, py, w, h, translate_x, translate_y);
                er_pop_clip_rect();

                cc()->tile_active = outer_active;
                cc()->tile_x = ox;
                cc()->tile_y = oy;
                cc()->tile_w = ow;
                cc()->tile_h = oh;
                er_set_draw_alpha(saved_alpha);

                first = false;
                continue;
            }
            ER_PROF_ACC(s_prof_push_us, t_push);
            cc()->tile_active = true;
            cc()->tile_x = bx;
            cc()->tile_y = by;
            cc()->tile_w = bw;
            cc()->tile_h = bh;
            cc()->tile_first = first;
            er_push_clip_rect(bx, by, bw, bh);
            ER_PROF_MARK(t_content);
            render_node_content(n, true, false, px, py, w, h, translate_x, translate_y);
            ER_PROF_ACC(s_prof_content_us, t_content);
            er_pop_clip_rect();
            cc()->tile_active = outer_active;
            cc()->tile_x = ox;
            cc()->tile_y = oy;
            cc()->tile_w = ow;
            cc()->tile_h = oh;
            ER_PROF_MARK(t_blend);
            er_scratch_pop_blend(alpha, bx, by, bw, bh);
            ER_PROF_ACC(s_prof_blend_us, t_blend);
#if ER_PROF
            s_prof_passes++;
#endif
            first = false;
        }
    }
    cc()->cur_loop_gen = outer_gen;
    cc()->tile_first = outer_first;
#if ER_PROF
    s_prof_composites++;
#endif
    return true;
}

/** @brief How far one edge's opaque fill is set in: the corner radius, or a partly opaque border band. */
static int edge_opaque_inset(int border_w, uint32_t border_c, int radius)
{
    const uint32_t a = border_c >> 24;
    const int band = (border_w > 0 && a != 0U && a != 0xFFU) ? border_w : 0;
    return band > radius ? band : radius;
}

/**
 * @brief Whether a node's own background paints every pixel of a screen rect at full alpha.
 *
 * The test the occlusion cull is built on: when this holds for a node painted LATER, everything
 * painted earlier inside that rect is overwritten and never needs to be drawn at all. It is
 * deliberately conservative — only the case it can prove pixel-for-pixel counts:
 *
 *   - a View-family box (the only node types that flat-fill their whole rect),
 *   - untransformed, since a scaled/rotated box lands somewhere this rect arithmetic cannot describe,
 *   - fully opaque: node opacity 255 and a background colour with alpha 255 and no active gradient
 *     (a gradient replaces the flat fill and may carry translucent stops),
 *   - inset by the largest corner radius, because rounded corners — and the anti-aliased pixels along
 *     them — leave the corner squares showing whatever is underneath,
 *   - inset again, per edge, by the width of any border whose colour is only PARTLY opaque. An opaque
 *     background does not imply an opaque box: for a uniform solid border, render_view_bg fills the
 *     whole shape in the BORDER colour and paints the background back over the inset (see
 *     er_rrect_fill_bordered), so a translucent border leaves a blended ring around an opaque middle.
 *     A fully opaque or fully transparent border needs no inset — the first covers the ring itself,
 *     the second routes to the path that fills the background across the whole shape.
 *
 * @param[in] c            Candidate occluder.
 * @param[in] translate_x  Scroll translation in effect for c (its parent's child translation).
 * @param[in] translate_y  Vertical counterpart.
 * @param[in] rx           Screen rect to test coverage of.
 * @param[in] ry           Screen rect top.
 * @param[in] rw           Screen rect width.
 * @param[in] rh           Screen rect height.
 *
 * @return true when the rect lies entirely inside c's opaque fill.
 */
static bool node_covers_opaque(const ERNode* c, int translate_x, int translate_y, int rx, int ry, int rw, int rh)
{
    if (!c || rw <= 0 || rh <= 0)
        return false;
    if (c->layout.display == ER_DISPLAY_NONE || c->subtree_hidden || c->has_transform)
        return false;

    switch (c->type)
    {
        case ER_NODE_VIEW:
        case ER_NODE_SCROLL_VIEW:
        case ER_NODE_PRESSABLE:
        case ER_NODE_FLAT_LIST:
            break;
        default:
            return false; /* Text/Image/Vector/Arc/Switch leave gaps; Modal paints past its own box. */
    }

    const ERViewProps* vp = &c->props.view;
    if (vp->opacity != 255U)
        return false;
    if ((vp->background_color >> 24) != 0xFFU)
        return false;
#if ERUI_GRADIENT
    if (vp->gradient_type != ER_GRADIENT_NONE && vp->gradient_stop_count >= 2U)
        return false;
#endif

    /* Largest corner radius, starting from 0 so a negative value can never widen the tested area —
     * the rasteriser clamps a negative radius to a square corner, and reading it literally here would
     * push x1/y1 past the node's actual box. */
    int r = 0;
    if ((int)vp->border_radius > r)
        r = (int)vp->border_radius;
    if ((int)vp->border_tl_radius > r)
        r = (int)vp->border_tl_radius;
    if ((int)vp->border_tr_radius > r)
        r = (int)vp->border_tr_radius;
    if ((int)vp->border_br_radius > r)
        r = (int)vp->border_br_radius;
    if ((int)vp->border_bl_radius > r)
        r = (int)vp->border_bl_radius;

    /* Per-edge border inset (see the note above): only a PARTLY opaque band hides nothing. */
    const int bw_l = vp->border_left_width > 0 ? (int)vp->border_left_width : (int)vp->border_width;
    const int bw_t = vp->border_top_width > 0 ? (int)vp->border_top_width : (int)vp->border_width;
    const int bw_r = vp->border_right_width > 0 ? (int)vp->border_right_width : (int)vp->border_width;
    const int bw_b = vp->border_bottom_width > 0 ? (int)vp->border_bottom_width : (int)vp->border_width;
    const uint32_t bc_l = vp->border_left_color ? vp->border_left_color : vp->border_color;
    const uint32_t bc_t = vp->border_top_color ? vp->border_top_color : vp->border_color;
    const uint32_t bc_r = vp->border_right_color ? vp->border_right_color : vp->border_color;
    const uint32_t bc_b = vp->border_bottom_color ? vp->border_bottom_color : vp->border_color;

    const int in_l = edge_opaque_inset(bw_l, bc_l, r);
    const int in_t = edge_opaque_inset(bw_t, bc_t, r);
    const int in_r = edge_opaque_inset(bw_r, bc_r, r);
    const int in_b = edge_opaque_inset(bw_b, bc_b, r);

    const int x0 = c->animated.x - translate_x + in_l;
    const int y0 = c->animated.y - translate_y + in_t;
    const int x1 = c->animated.x - translate_x + c->animated.w - in_r;
    const int y1 = c->animated.y - translate_y + c->animated.h - in_b;

    return rx >= x0 && ry >= y0 && (rx + rw) <= x1 && (ry + rh) <= y1;
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
 * @param[in] occluded      true when an opaque node painted later already covers this whole region.
 *                          The subtree is still walked — its paint bookkeeping (painted_seq,
 *                          last_paint_rect) has to stay in step or a hidden node damages the same
 *                          pixels forever — but it emits nothing.
 * @param[in] translate_x   Accumulated horizontal scroll offset from ancestor ScrollViews.
 * @param[in] translate_y   Accumulated vertical scroll offset from ancestor ScrollViews.
 */
static void render_tree(ERNode* n, bool parent_dirty, bool occluded, int translate_x, int translate_y)
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
        if (cc()->tile_active
            && (bx1 <= cc()->tile_x || by1 <= cc()->tile_y || bx0 >= cc()->tile_x + cc()->tile_w
                || by0 >= cc()->tile_y + cc()->tile_h))
            return;
    }

    /* needs_paint is the classic painter's-algorithm answer: this node's pixels are part of the
     * region being recomposited. should_render additionally asks whether they would be VISIBLE —
     * an occluded node is walked for its bookkeeping but draws nothing (see the cull in
     * render_node_content). needs_paint, not should_render, is what children inherit and what
     * refreshes last_paint_rect, so an occluded subtree stays exactly as damage-tracked as a
     * painted one. */
    const bool needs_paint = n->dirty || parent_dirty;
    const bool should_render = needs_paint && !occluded;

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
    /* An occluded node never captures a transform source: the scratch render would be thrown away,
     * and the cull only ever occludes transform-free subtrees anyway (see the subtree_prunable gate). */
    if (n->has_transform && n->type != ER_NODE_ACTIVITY_INDICATOR && !occluded)
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
                if (cc()->tile_active && !cc()->tile_first && er_transform_source_is_cached(n->tag, cc()->cur_loop_gen))
                {
                    doing_replay = true;
                    doing_3d = true;
                }
                else if (er_transform_source_begin(px, py, w, h))
                {
                    doing_affine = true;
                    doing_3d = true;
                    er_transform_source_note(n->tag, cc()->tile_active ? cc()->cur_loop_gen : 0U);
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
                if (cc()->tile_active && !cc()->tile_first && er_transform_source_is_cached(n->tag, cc()->cur_loop_gen))
                {
                    doing_replay = true;
                }
                else if (er_transform_source_begin(px, py, w, h))
                {
                    doing_affine = true;
                    er_transform_source_note(n->tag, cc()->tile_active ? cc()->cur_loop_gen : 0U);
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
    const bool xf_saved_tile_active = cc()->tile_active;
    /* Whether an OUTER capture was already running when this node was entered — read below to report
     * this node's change at the position its pixels actually reach the screen. Sampled before the
     * assignment, so a node that starts its own capture still reports its own transformed AABB. */
    const bool xf_outer_capturing = cc()->xf_capturing;
    const ERRect xf_outer_dst = cc()->xf_capture_dst;
    if (doing_affine)
    {
        cc()->tile_active = false;
        cc()->xf_capturing = true;
        cc()->xf_capture_dst.x = dst_x;
        cc()->xf_capture_dst.y = dst_y;
        cc()->xf_capture_dst.w = dst_w;
        cc()->xf_capture_dst.h = dst_h;
    }

    /* Record where this node is painted so the next commit can damage-clip a move: the old rect
     * (stored here) unioned with the new rect erases the node's trail without a full-screen repaint.
     * Keyed off needs_paint rather than should_render so an OCCLUDED node still retires its move:
     * leaving last_paint_rect stale would make the damage pre-pass read it as moved on every
     * subsequent commit, re-damaging the same pixels for as long as it stays hidden. */
    if (needs_paint)
    {
        int lp_x = doing_affine ? dst_x : px;
        int lp_y = doing_affine ? dst_y : py;
        int lp_w = doing_affine ? dst_w : w;
        int lp_h = doing_affine ? dst_h : h;
        /* The footprint must include the knob's reach past the box, or a move / removal leaves it.
         * Not on the affine path, which bounds itself by the transformed layout box. */
        if (!doing_affine)
            expand_for_arc(n, &lp_x, &lp_y, &lp_w, &lp_h);
        n->last_paint_rect.x = (int16_t)lp_x;
        n->last_paint_rect.y = (int16_t)lp_y;
        n->last_paint_rect.w = (int16_t)lp_w;
        n->last_paint_rect.h = (int16_t)lp_h;
        n->last_paint_untransformed = !doing_affine;
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
    if (n->source_dirty && n->painted_seq != s_commit_seq)
    {
        if (xf_outer_capturing)
        {
            union_dirty_rect(xf_outer_dst.x, xf_outer_dst.y, xf_outer_dst.w, xf_outer_dst.h);
        }
        else if (doing_affine)
        {
            union_dirty_rect(dst_x, dst_y, dst_w, dst_h);
        }
        else
        {
            int ux = px, uy = py, uw = w, uh = h;
            if (n->type == ER_NODE_MODAL && n->modal_visible)
            {
                /* The backdrop covers the root, so a host flushing er_get_dirty_rect() has to send
                 * all of it — the node's own box would leave the scrim's edges untransferred. */
                const ERNode* rt = er_get_root_node();
                if (rt)
                {
                    ux = rt->computed.x;
                    uy = rt->computed.y;
                    uw = rt->computed.w;
                    uh = rt->computed.h;
                }
            }
            if ((n->type == ER_NODE_VECTOR || n->type == ER_NODE_ARC) && n->vec_has_dirty)
            {
                /* Match the sub-region damage so the engine's dirty-rect tracker stays tight too. */
                ux = px + (int)n->vec_dirty_x;
                uy = py + (int)n->vec_dirty_y;
                uw = (int)n->vec_dirty_w;
                uh = (int)n->vec_dirty_h;
            }
            else if (n->type == ER_NODE_ARC)
            {
                const int over = (int)n->arc_overhang;
                ux -= over;
                uy -= over;
                uw += 2 * over;
                uh += 2 * over;
            }
#if ERUI_SHADOWS
            /* Expand conservatively for shadow bleed outside the node layout rect. */
            expand_for_shadow(n, &ux, &uy, &uw, &uh);
#endif
            union_dirty_rect(ux, uy, uw, uh);
        }
        /* NOT cleared here: er_commit's post-pass clears source_dirty for painted nodes. The
         * painted_seq gate above keeps this block first-visit-only (band strips revisit nodes). */
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
        render_node_content(n, needs_paint, occluded, px, py, w, h, translate_x, translate_y);
        er_set_draw_alpha(saved_alpha);
    }

    n->painted_seq = s_commit_seq; /* flags cleared in er_commit's sequential post-pass */

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
        cc()->tile_active = xf_saved_tile_active;
        cc()->xf_capturing = xf_outer_capturing;
        cc()->xf_capture_dst = xf_outer_dst;
    }
}

static void render_node_content(
    ERNode* n, bool needs_paint, bool occluded, int px, int py, int w, int h, int translate_x, int translate_y)
{
    /* Overflow clipping: children cannot draw outside this node. A scroller (ScrollView / FlatList)
     * ALWAYS clips to its viewport — that is its defining behaviour — regardless of an explicit
     * overflow style, so scrolled children can't escape past the top or bottom edge. */
    const bool clips = node_clips_children(n);

    /* Scroll offset translation: ScrollView and FlatList children are shifted by the current offset. */
    const bool is_scroller = (n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_FLAT_LIST);
    const int child_tx = is_scroller ? translate_x + (int)n->scroll_offset_x : translate_x;
    const int child_ty = is_scroller ? translate_y + (int)n->scroll_offset_y : translate_y;

    uint16_t child_tags[ERUI_MAX_NODES];
    const int child_count = collect_children(n, child_tags, ERUI_MAX_NODES);
    sort_children_by_z_index(child_tags, child_count);

    /* --- Occlusion cull -------------------------------------------------------------------------
     * Painting is bottom-up, so everything inside the repaint region is drawn even where a later,
     * opaque layer is about to bury it. Find the LAST child (in paint order) that fills the whole
     * region with opaque pixels: this node's own background and every child before that one cannot
     * contribute a single visible pixel, so none of them draws.
     *
     * The region tested is the active scissor — the damage rect being repainted, or the root when
     * there is none. Over-estimating it is always safe (it only makes the coverage test stricter),
     * which is what keeps this correct under banded rendering, where the clip is the whole repaint
     * bbox while each pass emits only one strip.
     *
     * Culled subtrees are still WALKED, with occluded = true, so their paint bookkeeping stays in
     * step; they just emit nothing. Only transform-free subtrees (subtree_prunable) are buried: a
     * transformed node records last_paint_rect as its TRANSFORMED AABB, which the occluded path —
     * which deliberately skips the scratch capture — cannot compute. Storing the raw box instead
     * would leave the damage pre-pass comparing a box against an AABB, reading `moved` on every
     * subsequent commit and re-damaging a subtree nobody can see, forever. Such a sibling simply
     * paints as usual and the occluder covers it a moment later. */
    int occ_idx = -1;
#if ERUI_OCCLUSION_CULLING
    if (!occluded && child_count > 0 && er_get_draw_alpha() == 255U)
    {
        int rx, ry, rw, rh;
        if (!er_get_clip_rect(&rx, &ry, &rw, &rh))
        {
            const ERNode* rt = er_get_root_node(); /* unclipped full repaint: the region is the screen */
            if (!rt)
                rw = rh = 0;
            else
            {
                rx = rt->computed.x;
                ry = rt->computed.y;
                rw = rt->computed.w;
                rh = rt->computed.h;
            }
        }
        if (clips)
        {
            /* Children are about to be scissored to this node's box; the node's own background fills
             * exactly the same area, so one region covers both decisions. */
            const int cx1 = (rx + rw < px + w) ? rx + rw : px + w;
            const int cy1 = (ry + rh < py + h) ? ry + rh : py + h;
            if (px > rx)
                rx = px;
            if (py > ry)
                ry = py;
            rw = cx1 - rx;
            rh = cy1 - ry;
        }
        for (int i = child_count - 1; i >= 0; i--)
        {
            const ERNode* c = er_get_node(child_tags[i]);
            if (node_covers_opaque(c, child_tx, child_ty, rx, ry, rw, rh))
            {
                occ_idx = i;
                break;
            }
        }
    }
#endif

    /* A Modal's backdrop covers the whole root rather than this node's box, so a child covering the
     * box proves nothing about the pixels the scrim owns — never skip its own draw. */
    const bool self_covered = (occ_idx >= 0) && (n->type != ER_NODE_MODAL);
    const bool should_render = needs_paint && !occluded && !self_covered;

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
                    n->modal_scrim_shown = 1U;
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
            case ER_NODE_ARC:
                er_arc_render(n, px, py, w, h);
                n->vec_has_dirty = false; /* one-shot, like the vector sub-rect */
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

    /* A one-shot sub-region damage rect that was never consumed (the node is buried this commit)
     * must still be retired, or the next commit would narrow that node's repaint to a stale rect. */
    if (needs_paint && !should_render && (n->type == ER_NODE_VECTOR || n->type == ER_NODE_ARC))
        n->vec_has_dirty = false;

    if (clips)
        er_push_clip_rect(px, py, w, h);

    for (int i = 0; i < child_count; i++)
    {
        ERNode* child = er_get_node(child_tags[i]);
        if (!child)
            continue;
        /* Children before the occluder are buried by it; children from the occluder on still paint.
         * A subtree carrying a transform is never buried — see the note on the cull above. */
        const bool buried = (i < occ_idx) && child->subtree_prunable;
        render_tree(child, needs_paint, occluded || buried, child_tx, child_ty);
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
 * @brief Walks the subtree and moves every knob-child Arc's first child onto its value point.
 *
 * Runs right after the flex pass, before layout animations and onLayout dispatch see the rects.
 *
 * @param[in] node  Subtree root to walk.
 */
static void anchor_arc_children(ERNode* node)
{
    if (!node)
        return;
    if (node->type == ER_NODE_ARC)
        er_arc_anchor_child(node);
    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        anchor_arc_children(child);
        child_tag = child->next_sibling_tag;
    }
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

ERNodeType er_node_get_type(const ERNode* node)
{
    return node ? node->type : ER_NODE_VIEW;
}

float er_arc_get_value(const ERNode* node)
{
    return (node && node->type == ER_NODE_ARC) ? node->arc_value : 0.0f;
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

    if (type == ER_NODE_VECTOR || type == ER_NODE_ARC)
        s_parallel_unsafe++; /* vector rasterizer / arc span cache use shared scratch — see s_parallel_unsafe */

    /* No finger owns a fresh arc. memset left this 0, which would read as "finger 0 is dragging". */
    if (type == ER_NODE_ARC)
        n->arc_drag_finger = -1;

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
        note_vacated_rect(node,
                          (int)node->last_paint_rect.x,
                          (int)node->last_paint_rect.y,
                          (int)node->last_paint_rect.w,
                          (int)node->last_paint_rect.h);

    node->in_use = false;
    node->dirty = false;
    if (node->type == ER_NODE_VECTOR || node->type == ER_NODE_ARC)
        s_parallel_unsafe--;
#if ERUI_SHADOWS
    if (node->casts_shadow)
    {
        node->casts_shadow = false;
        s_parallel_unsafe--;
    }
#endif
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

/** @brief 32-bit FNV-1a over a byte range (ERProps is zero-initialised by the bridge, so equal props hash equal). */
static uint32_t fnv1a32(const void* data, size_t len)
{
    const uint8_t* b = (const uint8_t*)data;
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < len; i++)
    {
        h ^= b[i];
        h *= 16777619U;
    }
    return h;
}

/* ERProps is laid out as one contiguous Yoga layout block followed by everything else, so the
 * boundary is a single offset: background_color is the first field past it. Hashing the two sides
 * separately costs exactly what hashing the whole struct did, and answers the question the damage
 * pre-pass actually needs — did this update change how the node LOOKS, or only where things sit? */
#define ER_PROPS_LAYOUT_BYTES offsetof(ERProps, background_color)

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
    const uint32_t lay_h = fnv1a32(props, ER_PROPS_LAYOUT_BYTES);
    const uint32_t vis_h =
        fnv1a32((const uint8_t*)props + ER_PROPS_LAYOUT_BYTES, sizeof(ERProps) - ER_PROPS_LAYOUT_BYTES);
    const bool first_update = !node->has_props_hash;
    const bool layout_changed = first_update || lay_h != node->layout_props_hash;
    const bool visual_changed = first_update || vis_h != node->visual_props_hash;
    const bool props_changed = layout_changed || visual_changed;
    node->layout_props_hash = lay_h;
    node->visual_props_hash = vis_h;
    node->has_props_hash = true;

    /* Remembered across the layout copy below so a display:none toggle can be detected and the
     * subtree's hidden state settled once, after this node's own dirty marking (see propagate_hidden). */
    const uint8_t prev_display = node->layout.display;

    /* Same idea for overflow, and read HERE because the cached subtree paint bounds it is compared
     * against are still the pre-change ones (see the settle below). */
    const bool prev_clips = node_clips_children(node);

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
#if ERUI_SHADOWS
            /* Track shadow-casting transitions for the multi-core safety count (see s_parallel_unsafe). */
            {
                const bool casts = (props->shadow_opacity > 0.0f || props->elevation > 0);
                if (casts != node->casts_shadow)
                {
                    s_parallel_unsafe += casts ? 1 : -1;
                    node->casts_shadow = casts;
                }
            }
#endif
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
        case ER_NODE_ARC:
        {
            ERArcProps* a = &node->props.arc;
            ERArcProps before;
            memcpy(&before, a, sizeof(before));
            a->min = props->arc_min;
            a->max = props->arc_max;
            a->start_angle = props->arc_start_angle;
            a->sweep_angle = props->arc_sweep_angle;
            a->step = props->arc_step;
            a->gap_angle = props->arc_gap_angle;
            a->width = props->arc_width;
            a->band_width = props->arc_band_width;
            a->knob_size = props->arc_knob_size;
            a->knob_border_width = props->arc_knob_border_width;
            a->track_color = props->arc_track_color;
            a->indicator_color = props->arc_indicator_color;
            a->band_color = props->arc_band_color;
            a->knob_color = props->arc_knob_color;
            a->knob_border_color = props->arc_knob_border_color;
            a->segments = props->arc_segments;
            a->cap = props->arc_cap;
            a->knob = props->arc_knob;
            a->adjustable = props->arc_adjustable;
            a->range = props->arc_range;
            a->value_start = props->arc_value_start;
            a->min_span = props->arc_min_span;
            a->gradient_type = props->gradient_type;
            a->gradient_stop_count = props->gradient_stop_count;
            for (int gi = 0; gi < ER_GRADIENT_MAX_STOPS; gi++)
                a->gradient_stops[gi] = props->gradient_stops[gi];
            strncpy(a->image_name, props->image_name, ER_IMAGE_NAME_MAX);
            a->image_name[ER_IMAGE_NAME_MAX] = '\0';
            const bool shape_changed = (memcmp(&before, a, sizeof(before)) != 0);
            /* A native drag owns the value while the finger is down: a React re-render mid-drag (e.g. the
             * readout updating from onChange) must not snap the knob back to the value it rendered with. */
            if (node->arc_drag_finger < 0)
            {
                /* Order matters in RANGE mode: each end clamps against the other, so widening the band
                 * needs the far end moved first or it would clamp itself to the stale neighbour. */
                if (props->arc_value >= node->arc_value)
                {
                    (void)er_arc_apply_value(node, props->arc_value);
                    if (a->range)
                        (void)er_arc_apply_value_start(node, props->arc_value_start);
                }
                else
                {
                    if (a->range)
                        (void)er_arc_apply_value_start(node, props->arc_value_start);
                    (void)er_arc_apply_value(node, props->arc_value);
                }
            }
            if (shape_changed)
            {
                /* Anything but the value changed → every pixel may differ: repaint the whole box. */
                node->vec_has_dirty = false;
                er_arc_refresh_overhang(node);
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
#if ERUI_SHADOWS
            /* Track shadow-casting transitions for the multi-core safety count (see s_parallel_unsafe). */
            {
                const bool casts = (props->shadow_opacity > 0.0f || props->elevation > 0);
                if (casts != node->casts_shadow)
                {
                    s_parallel_unsafe += casts ? 1 : -1;
                    node->casts_shadow = casts;
                }
            }
#endif
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
     * request a layout pass; only when something actually changed (see the hash gate above) —
     * an identical setProps invalidates nothing.
     *
     * How the node is DAMAGED, though, splits on which half of the props moved:
     *
     *   - appearance changed (colour, text, border, transform, …): the node's own pixels are
     *     different, so it damages its box, as always.
     *   - ONLY the layout inputs changed: the node still looks exactly the same. Whether any pixel
     *     moves is a question for the layout pass, and the damage pre-pass already answers it by
     *     comparing every node's new screen rect against where it was last painted. So mark the
     *     chain for repaint but claim no damage of its own — repaint what moved, not what was
     *     measured. A padding tweak on a big container then costs its children's old and new spots
     *     instead of the container's whole box, and a re-layout that lands everything back where it
     *     was costs nothing at all.
     *
     * A node that has never painted is always treated as a visual change: nothing can be "moved"
     * relative to a paint that never happened, so its box is the only thing that can put it on screen. */
    if (props_changed)
    {
        mark_layout_dirty();
        if (visual_changed || !node->has_last_paint)
            er_mark_dirty_upward(node);
        else
            mark_reflow_upward(node);
    }

    /* overflow clip/unclip. The node's own box is damaged like any other visual change, but the box is
     * not what moved: what changed is whether descendants may paint OUTSIDE it, and the pre-pass sees
     * only each node's own rect. Unclipping leaves the newly-reachable region un-painted (the children
     * did not move, so nothing damages it); clipping leaves what they painted there on screen. Both are
     * settled by damaging the subtree paint bounds from either side of the change — union, not
     * difference, since whichever side does not clip is the larger one and covers the other.
     *
     * The pre-change bounds are still cached on the node right now, so they go straight into the
     * vacated-pixel set (which both repaints and reports them, exactly as a removed node's footprint
     * does). The post-change bounds do not exist until the layout pass recomputes them, so the flag
     * hands that half to the pre-pass. A node whose own transform the damage tracker cannot bound has
     * no usable bounds on either side; that rare case repaints everything instead. */
    if (node_clips_children(node) != prev_clips)
    {
        int sx, sy, sw, sh;
        if (node_subtree_screen_rect(node, &sx, &sy, &sw, &sh))
        {
            clip_rect_to_clippers(node, &sx, &sy, &sw, &sh);
            note_vacated_rect(node, sx, sy, sw, sh);
            node->overflow_toggled = true;
        }
        else
        {
            er_force_full_repaint();
        }
        /* Independent of which half of the props hash caught the change: the subtree has to be
         * repainted, so the node must be source-dirty and the walk must reach it. */
        er_mark_dirty_upward(node);
    }

    /* display:none show/hide. Runs AFTER the dirty marking above so that hiding retires this node's
     * freshly-set flags too (the ancestors' propagated dirty stays, which is what repaints the
     * background the subtree vacated). ER_NODE_MODAL drives node->layout.display from modal_visible
     * further up, so its show/hide lands here as well. */
    if (node->layout.display != prev_display)
    {
        propagate_hidden(node, parent_hidden(node));

        if (!node->subtree_hidden)
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

    const bool pinned_w = (node->layout.width != ER_LAYOUT_AUTO) || (node->layout.width_pct > 0.0f);
    const bool pinned_h = (node->layout.height != ER_LAYOUT_AUTO) || (node->layout.height_pct > 0.0f);
    const bool single_line = (node->props.text.number_of_lines == 1U);
    if (!(pinned_w && (single_line || pinned_h)))
    {
        mark_layout_dirty();
    }
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
    mark_reflow_upward(parent);
    mark_layout_dirty();
    propagate_hidden(child, parent->subtree_hidden);
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

    mark_reflow_upward(parent); /* the chain above must repaint too — see er_tree_append_child */
    mark_layout_dirty();
    propagate_hidden(child, parent->subtree_hidden);
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
    mark_reflow_upward(parent); /* the chain above must repaint too — see er_tree_append_child */
    mark_layout_dirty();
    propagate_hidden(child, true);
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
    propagate_hidden(root, false); /* a new root is on screen: its subtree is no longer detached */
    er_force_full_repaint();       /* whole new scene: repaint everything */
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
    for (int i = 0; i < ERUI_RENDER_WORKERS; i++)
        s_comp_ctx[i].has_dirty = false;
    er_damage_set_clear(&s_removed_set);
    er_damage_set_clear(&s_last_paint_set);
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

/**
 * @brief One worker's share of a sliced parallel render: a horizontal band of the repaint region.
 */
typedef struct
{
    ERNode* root;
    bool full_recomposite; /**< Multi-buffer replay: repaint everything inside the clip. */
    int x0;                /**< Repaint region bounds (screen space). */
    int x1;
    int ry0;
    int ry1;
    int nslices;
    int kbd_y;
} ParallelRenderJob;

/**
 * @brief er_parallel_for job: renders slice `worker` of the repaint region.
 *
 * Each worker pushes its slice as a clip on its OWN clip stack (per-worker context) and walks the
 * whole tree; per-worker scratch (opacity slots, transform buffers, row buffers) keeps concurrent
 * composites isolated, and Phase 0's painted_seq deferral means the walk only reads shared node
 * flags. A transform subtree straddling a slice boundary is captured fully by both workers (the
 * capture pushes a clip reset — the seam-safety rule), each emitting only its slice's rows.
 */
static void render_slice_job(int worker, void* arg)
{
    const ParallelRenderJob* j = (const ParallelRenderJob*)arg;
    const int rows = j->ry1 - j->ry0;
    const int sy0 = j->ry0 + rows * worker / j->nslices;
    const int sy1 = j->ry0 + rows * (worker + 1) / j->nslices;
    if (sy0 >= sy1)
        return;
    er_push_clip_rect(j->x0, sy0, j->x1 - j->x0, sy1 - sy0);
    render_tree(j->root, j->full_recomposite, false, 0, j->kbd_y);
    er_pop_clip_rect();
}

void er_commit(void)
{
    er_input_flush_moves();

    if (s_root_tag == ER_INVALID_TAG)
        return;

    ERNode* root = er_get_node(s_root_tag);
    if (!root)
        return;

    s_commit_seq++; /* per-commit sequence for the fade cache's one-capture-per-commit gate */

    /* Reset every worker's dirty-rect accumulator for this commit. The PUBLISHED results
     * (s_dirty_rect / s_has_dirty / s_last_paint_set) are deliberately not touched here: a commit that
     * paints nothing leaves the last painting commit's rects readable — see er_get_dirty_rect(). */
    for (int i = 0; i < ERUI_RENDER_WORKERS; i++)
    {
        s_comp_ctx[i].has_dirty = false;
        /* render_tree saves and restores this around each capture, so it is already false — cleared
         * anyway so one unbalanced render can't make every later node report an ancestor's stale AABB. */
        s_comp_ctx[i].xf_capturing = false;
    }

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
        ER_PERF_BEGIN(ER_PERF_PHASE_LAYOUT);
        const int16_t rw = (root->layout.width != ER_LAYOUT_AUTO) ? root->layout.width : 0;
        const int16_t rh = (root->layout.height != ER_LAYOUT_AUTO) ? root->layout.height : 0;

        er_layout_compute(s_root_tag, rw, rh);
        anchor_arc_children(root);
        refresh_scroll_content_sizes(root);
        er_layout_anim_post_layout(root);
        dispatch_layout_events(root);
        compute_subtree_bounds(root); /* refresh cached prune bounds; stay valid through static frames */

        s_layout_dirty = false;
        s_layout_pass_count++;
        ER_PERF_END(ER_PERF_PHASE_LAYOUT);
    }

    /* Everything from here to the end of the commit is the paint pipeline: the damage pre-pass that
     * decides what to repaint, the composite itself, and the dirty-flag sweep. Attributed as one
     * RASTER phase — the pre-pass walks the whole node pool, so it belongs with the paint cost it is
     * there to reduce rather than in an unaccounted gap. Within it, the sub-steps that can
     * independently blow up are marked separately (ERPerfRasterSub): the pre-pass and the flag sweep
     * are ERUI_MAX_NODES-proportional floors paid even by an idle commit, while composite and blit
     * scale with the damage — the split is what tells a "the pool walk dominates" frame from a "the
     * framebuffer writes dominate" one. */
    ER_PERF_BEGIN(ER_PERF_PHASE_RASTER);
    ER_PERF_BLIT_RESET();
    ER_PERF_RASTER_BEGIN(ER_PERF_RASTER_PREPASS);

    /* Damage-clipped render. Unless we must repaint everything (first frame, an invalidated
     * framebuffer, a removed node, or a changing node with a transform we can't bound), scissor
     * render_tree() to the union of every changed-or-moved node's new screen rect and the rect it was
     * painted at last commit (so a moved node's trail is erased). A node counts if it was directly
     * dirtied (source_dirty) or its screen rect differs from where it was last painted (layout reflow
     * or a translate animation) — propagated-dirty ancestors are excluded so the damage stays tight.
     * The persistent framebuffer keeps the untouched pixels, so the compositor and the backend's flush
     * both shrink from full-screen to just the changed region. */
    /* Compute this commit's repaint region in screen space. Default is the whole root rect (a full
     * repaint); the damage pre-pass below narrows it to a SET of disjoint changed/moved rects whenever
     * change tracking is possible — so a change in one corner and another in the opposite corner
     * repaint two small areas, not the span between them. The set (with its bounding box in rb_*)
     * then drives EITHER per-rect damage-clipped render_tree passes (full-framebuffer backend) OR a
     * per-strip banded render over the set's dirty row ranges (backend->band_height > 0). */
    int rb_x0 = root->computed.x;
    int rb_y0 = root->computed.y;
    int rb_x1 = rb_x0 + root->computed.w;
    int rb_y1 = rb_y0 + root->computed.h;
    bool render_full = true;    /* repaint the whole root rect */
    bool nothing_dirty = false; /* tracked, but nothing changed (or changes clamped off-screen) */
    ERDamageSet dmg;            /* this commit's repaint rects (only meaningful when narrowed) */
    er_damage_set_clear(&dmg);
    if (s_force_full_repaint)
    {
        /* Full repaint requested: this commit's OWN damage is the whole screen. For multi-buffer that is
         * folded into every buffer's debt below (so each rotating buffer repaints fully when next
         * rendered); for single-buffer it simply repaints the one framebuffer. */
        root->dirty = true;
    }
    else
    {
        bool trackable = true;
        /* Seed with any pixels vacated by removed, destroyed or hidden nodes since the last commit.
         * They are REPORTED as well as repainted: the node that owned those pixels is gone from the
         * walk, so nothing downstream would contribute them to er_get_dirty_rect(), and a host that
         * flushes only the reported rect would leave the vacated content on the panel. (Same reason
         * the Modal scrim below reports its own erase explicitly.) */
        for (uint8_t ri = 0U; ri < s_removed_set.count; ri++)
        {
            const ERRect* v = &s_removed_set.r[ri];
            add_damage(&dmg, v->x, v->y, v->w, v->h, rb_x0, rb_y0, rb_x1, rb_y1);
            /* Clamped to the root the same way add_damage clamps its insert, so a footprint that lies
             * (partly) off-screen is never reported as repainted when it was not. */
            report_repaint_clamped(v->x, v->y, v->w, v->h, rb_x0, rb_y0, rb_x1, rb_y1);
        }
#if ERUI_ONSCREEN_KEYBOARD
        /* On-screen keyboard show/hide/layer-switch: repaint its bottom strip once (then GRAM retains it). */
        if (s_kbd_dirty)
        {
            ERRect kr;
            er_keyboard_rect((int)root->computed.w, (int)root->computed.h, &kr);
            add_damage(&dmg, kr.x, kr.y, kr.w, kr.h, rb_x0, rb_y0, rb_x1, rb_y1);
        }
#endif
        for (uint16_t tag = 0U; tag < (uint16_t)ERUI_MAX_NODES; tag++)
        {
            ERNode* n = er_get_node(tag);
            if (!n)
                continue;

            if (n->subtree_hidden && !(n->type == ER_NODE_MODAL && n->modal_scrim_shown))
                continue;
            if (n->overflow_toggled)
            {
                /* Under a transformed ancestor the subtree bounds below are source-space, like every
                 * other measurement of this node — the ancestor's blit is what puts the toggled
                 * region on screen, and its AABB already covers the whole subtree. */
                ERNode* const ov_cap = capturing_transform_ancestor(n);
                if (ov_cap && escalate_damage_to_capture(ov_cap, &dmg, false, rb_x0, rb_y0, rb_x1, rb_y1))
                    continue;
                /* This node clipped or unclipped its children since the last commit. Its own box is
                 * damaged below like any visual change; the half the box cannot express is the region
                 * its descendants painted (or are about to paint) outside it, which is this — the
                 * post-layout subtree bounds. The pre-change bounds came in through the vacated set at
                 * the top, so the union of the two covers the toggle in either direction.
                 *
                 * Reported as well as damaged: the descendants out there are not necessarily
                 * source-dirty, and on the clipping side what repaints the region is an ancestor's
                 * background, so render_tree's accumulator would never contribute it. */
                int sx, sy, sw, sh;
                if (!node_subtree_screen_rect(n, &sx, &sy, &sw, &sh))
                {
                    trackable = false; /* transform we cannot bound: repaint everything */
                    break;
                }
                clip_rect_to_clippers(n, &sx, &sy, &sw, &sh);
                add_damage(&dmg, sx, sy, sw, sh, rb_x0, rb_y0, rb_x1, rb_y1);
                report_repaint_clamped(sx, sy, sw, sh, rb_x0, rb_y0, rb_x1, rb_y1);
            }
            /* Asked before the rect is measured, because the answer must not depend on which helper
             * manages to measure it: the backdrop covers the root either way. Both branches below
             * hand a changed scrim modal straight to modal_scrim_damage(). */
            const bool scrim_modal = (n->type == ER_NODE_MODAL && (n->modal_visible || n->modal_scrim_shown));
            int rx, ry, rw, rh;
            if (!node_screen_rect(n, &rx, &ry, &rw, &rh))
            {
                /* Complex transform (scale/rotate). Bound the damage to the node's transformed AABB —
                 * current box plus where it was painted last commit, so a shrinking pulse erases its
                 * trail — instead of forcing a full-screen repaint.
                 */
                NodeTransformDamage td = {0};
                if (node_transform_damage(n, &td))
                {
                    const bool moved = n->has_last_paint
                                       && (td.fx != (int)n->last_paint_rect.x || td.fy != (int)n->last_paint_rect.y
                                           || td.fw != (int)n->last_paint_rect.w || td.fh != (int)n->last_paint_rect.h);
                    if (n->source_dirty || moved)
                    {
                        /* Asked before the ancestor-capture escalation below, for the same reason the
                         * translate-only branch asks it before its own: the scrim covers the root
                         * regardless of what any ancestor does to this node's pixels. */
                        if (scrim_modal)
                        {
                            modal_scrim_damage(n, &dmg, rb_x0, rb_y0, rb_x1, rb_y1);
                            continue;
                        }
                        /* Refused the scratch because an ancestor holds it: this node painted into
                         * that ancestor's capture in source space, so td measures a region its pixels
                         * never reach. The ancestor's AABB is where they actually land. */
                        ERNode* const cap = capturing_transform_ancestor(n);
                        if (cap && escalate_damage_to_capture(cap, &dmg, !n->source_dirty, rb_x0, rb_y0, rb_x1, rb_y1))
                            continue;
                        int nx = td.fx, ny = td.fy, nw = td.fw, nh = td.fh;
#if ERUI_SHADOWS
                        /* Only the FALLBACK footprint carries a shadow. render_tree gates the shadow on
                         * `!doing_affine`, so a node that captures its transform scratch genuinely casts
                         * none (it would be rasterised into the source and distorted by the inverse-map
                         * blit) and must not be grown — but one painted untransformed at its raw box
                         * draws its shadow like any other node, and a move that ignores the bleed leaves
                         * the old shadow on screen and clips the new one at the damage edge. */
                        if (td.raw)
                            expand_for_shadow(n, &nx, &ny, &nw, &nh);
#endif
                        clip_rect_to_clippers(n, &nx, &ny, &nw, &nh);
                        add_damage(&dmg, nx, ny, nw, nh, rb_x0, rb_y0, rb_x1, rb_y1); /* new footprint */
                        if (!n->source_dirty)
                            report_repaint_clamped(nx, ny, nw, nh, rb_x0, rb_y0, rb_x1, rb_y1);
                        if (td.hedge)
                        {
                            /* The fallback was carried over from the last paint, not predicted from
                             * size, so the capture may succeed this commit and paint the AABB instead.
                             * Damage both rather than risk scissoring the node's own paint away. */
                            int ax = td.ax, ay = td.ay, aw = td.aw, ah = td.ah;
                            clip_rect_to_clippers(n, &ax, &ay, &aw, &ah);
                            add_damage(&dmg, ax, ay, aw, ah, rb_x0, rb_y0, rb_x1, rb_y1);
                            if (!n->source_dirty)
                                report_repaint_clamped(ax, ay, aw, ah, rb_x0, rb_y0, rb_x1, rb_y1);
                        }
                        if (n->has_last_paint)
                        {
                            int ox = (int)n->last_paint_rect.x, oy = (int)n->last_paint_rect.y,
                                ow = (int)n->last_paint_rect.w, oh = (int)n->last_paint_rect.h;
#if ERUI_SHADOWS
                            /* Same rule, asked of the PREVIOUS paint: the trail carries a shadow only if
                             * that paint was the raw-box fallback, which is exactly what the flag records. */
                            if (n->last_paint_untransformed)
                                expand_for_shadow(n, &ox, &oy, &ow, &oh);
#endif
                            clip_rect_to_clippers(n, &ox, &oy, &ow, &oh);
                            add_damage(&dmg, ox, oy, ow, oh, rb_x0, rb_y0, rb_x1, rb_y1); /* old (erase trail) */
                            report_repaint_clamped(ox, oy, ow, oh, rb_x0, rb_y0, rb_x1, rb_y1);
                        }
                    }
                    continue;
                }
                /*
                 * Could not bound it (ActivityIndicator spin, or a transform that projects to nothing):
                 * only forces a full repaint if actually changing. An oversized node no longer lands here —
                 * node_transform_damage() settles it on size and returns the raw box.
                 * TODO: A moved-but-not-source_dirty node here (e.g. a 3D-transformed node shifted by reflow) is still
                 * missed — that needs the 3D AABB path ().
                 */
                if (n->source_dirty)
                {
                    /* Bounded after all, and by the only rect that was ever right for it: a modal that
                     * cannot be measured still scrims exactly the root. Cheaper than the full-repaint
                     * fallback below, and unlike it, retires the scrim flag. */
                    if (scrim_modal)
                    {
                        modal_scrim_damage(n, &dmg, rb_x0, rb_y0, rb_x1, rb_y1);
                        continue;
                    }
                    /* Inside an ancestor's capture this is bounded after all: whatever the node's own
                     * transform does, it does it in source space and reaches the screen only through
                     * the ancestor's blit. Beats a full-screen repaint per spinner frame. */
                    ERNode* const cap = capturing_transform_ancestor(n);
                    if (cap && escalate_damage_to_capture(cap, &dmg, false, rb_x0, rb_y0, rb_x1, rb_y1))
                        continue;
                    trackable = false;
                    break;
                }
                continue;
            }
            const int box_rx = rx, box_ry = ry; /* the layout box, before any arc inflation */
            /* Measure the arc by its painted footprint — the box plus the knob's reach past it — which is
             * what last_paint_rect records, so a steady arc is not read as "moved" every commit. */
            expand_for_arc(n, &rx, &ry, &rw, &rh);
            const bool moved = n->has_last_paint
                               && (rx != (int)n->last_paint_rect.x || ry != (int)n->last_paint_rect.y
                                   || rw != (int)n->last_paint_rect.w || rh != (int)n->last_paint_rect.h);
            if (!n->source_dirty && !moved)
                continue; /* unchanged and in place: contributes nothing to the damage */
            if (scrim_modal)
            {
                modal_scrim_damage(n, &dmg, rb_x0, rb_y0, rb_x1, rb_y1);
                continue;
            }
            /* Everything below measures this node in plain layout-minus-scroll space. Under a
             * transformed ancestor that space is the ancestor's CAPTURE, not the screen: the node's
             * pixels are inverse-mapped out at the ancestor's transformed AABB, so that AABB — and
             * not any rect derived from the node's own box — is what has to be repainted.
             * (Asked after the Modal case, whose scrim covers the root regardless of any ancestor.) */
            {
                ERNode* const cap = capturing_transform_ancestor(n);
                if (cap && escalate_damage_to_capture(cap, &dmg, !n->source_dirty, rb_x0, rb_y0, rb_x1, rb_y1))
                    continue;
            }
            if ((n->type == ER_NODE_VECTOR || n->type == ER_NODE_ARC) && n->vec_has_dirty && !moved)
            {
                /* Sub-region vector update: damage only the app-supplied changed rect (node-local →
                 * screen), not the whole box. The caller's rect already covers old+new content, so the
                 * full last_paint_rect is intentionally NOT added (it would balloon back to the box). */
                add_damage(&dmg,
                           box_rx + (int)n->vec_dirty_x,
                           box_ry + (int)n->vec_dirty_y,
                           (int)n->vec_dirty_w,
                           (int)n->vec_dirty_h,
                           rb_x0,
                           rb_y0,
                           rb_x1,
                           rb_y1);
                continue;
            }
            /* Clip both contributions to any clipping ancestor (ScrollView / overflow:hidden) so a scrolled
             * child's damage can't reach outside the list and pull a sibling (e.g. a title above) into the
             * repaint, where it would be cleared but not restored for a frame. */
            int nx = rx, ny = ry, nw = rw, nh = rh;
#if ERUI_SHADOWS
            /* The shadow is part of this node's footprint: a move that ignores it leaves the old
             * shadow on screen and clips the new one at the damage edge. */
            expand_for_shadow(n, &nx, &ny, &nw, &nh);
#endif
            clip_rect_to_clippers(n, &nx, &ny, &nw, &nh);
            add_damage(&dmg, nx, ny, nw, nh, rb_x0, rb_y0, rb_x1, rb_y1); /* new position */
            /* A node here that is NOT source_dirty is contributing purely because it MOVED, and
             * render_tree's accumulator only ever unions source-dirty nodes — so it reports itself,
             * exactly like the vacated footprints above. Without this a pure reflow repaints the
             * framebuffer correctly and then tells a partial-update host that nothing changed. */
            if (!n->source_dirty)
                report_repaint_clamped(nx, ny, nw, nh, rb_x0, rb_y0, rb_x1, rb_y1);
            if (n->has_last_paint)
            {
                int ox = (int)n->last_paint_rect.x, oy = (int)n->last_paint_rect.y, ow = (int)n->last_paint_rect.w,
                    oh = (int)n->last_paint_rect.h;
#if ERUI_SHADOWS
                expand_for_shadow(n, &ox, &oy, &ow, &oh);
#endif
                clip_rect_to_clippers(n, &ox, &oy, &ow, &oh);
                add_damage(&dmg, ox, oy, ow, oh, rb_x0, rb_y0, rb_x1, rb_y1); /* old position (erase trail) */
                /* Reported unconditionally, unlike the new position: render_tree's accumulator only
                 * ever sees where a node is NOW, so nothing else can report the trail it vacated. */
                report_repaint_clamped(ox, oy, ow, oh, rb_x0, rb_y0, rb_x1, rb_y1);

                /* A node with NO visible current position (scrolled out of its clipper, or otherwise
                 * clipped away entirely) owes exactly one thing: the erase just unioned above. Retire
                 * its footprint now, because it will never be able to retire it itself.
                 *
                 * Without it, such a node is a permanent damage source: a scroll shifts its screen rect
                 * (node_screen_rect folds in ancestor scroll offsets) while last_paint_rect still holds
                 * where it was drawn, so it counts as `moved` and the pre-pass re-unions this same
                 * footprint EVERY commit. Refreshing last_paint_rect requires painting it, painting
                 * requires render_tree to reach it, and render_tree prunes it precisely because it is
                 * invisible — so the loop sustains itself until reboot.
                 *
                 * Scrolling it back into view still repaints it: er_mark_dirty_upward marks the scroller
                 * source_dirty, so its viewport enters the damage and the walk reaches the child again. */
                if (nw <= 0 || nh <= 0)
                    n->has_last_paint = false;
            }
        }
        /* Padding + clamping happened per-insert (add_damage), so an empty set covers both "nothing
         * changed" and "every change clamped off-screen". rb_* stays the set's bounding box — the
         * banded clip, the parallel-height gate, and diagnostics all still want one covering box. */
        if (trackable && dmg.count > 0U)
        {
            ERRect bb;
            er_damage_set_bounds(&dmg, &bb);
            rb_x0 = bb.x;
            rb_y0 = bb.y;
            rb_x1 = bb.x + bb.w;
            rb_y1 = bb.y + bb.h;
            render_full = false; /* narrowed to the damage rects */
        }
        else if (trackable)
        {
            nothing_dirty = true; /* nothing changed this commit (or changes clamped off-screen) */
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
                er_damage_set_clear(&s_buf_debt[b].set);
            }
        }
        else
        {
            /* Each rect stays a separate debt entry (the slot's set merges/saturates on its own), so
             * disjoint damage replays as disjoint rects instead of ballooning into one span. */
            for (int b = 0; b < s_display_buffer_count; b++)
                if (!s_buf_debt[b].full)
                    for (uint8_t ri = 0U; ri < dmg.count; ri++)
                        er_damage_set_add(&s_buf_debt[b].set, dmg.r[ri].x, dmg.r[ri].y, dmg.r[ri].w, dmg.r[ri].h);
        }

        /* (2) Derive this commit's render set from the CURRENT buffer's debt (what it still owes). */
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
        else if (d->set.count > 0U)
        {
            ERRect bb;
            dmg = d->set; /* the replayed debt becomes this commit's render set */
            er_damage_set_bounds(&dmg, &bb);
            rb_x0 = bb.x;
            rb_y0 = bb.y;
            rb_x1 = bb.x + bb.w;
            rb_y1 = bb.y + bb.h;
            render_full = false;
            nothing_dirty = false;
        }
        else
        {
            render_full = false;
            nothing_dirty = true;
        }

        /* The current buffer is being brought fully current this commit, so it owes nothing afterward.
         * (Cleared here, before render, is fine: the set to paint was copied into dmg above.) */
        d->full = false;
        er_damage_set_clear(&d->set);
    }

    /* Enable subtree pruning only when no layout animation is interpolating positions (which would
     * leave the cached computed-space bounds stale). A full repaint pushes no clip, so render_tree()
     * pruning self-disables there regardless. */
    s_prune_ok = !er_layout_anim_has_pending() && !er_layout_anim_is_active();

    /* Rect count is a trade: each rect is a separate clipped pass, and a pass costs a tree walk plus
     * the pixels it touches. With pruning on, the walk is O(nodes near that rect), so many small rects
     * are close to free and the tight clips are what keep a vector node rasterizing only its own
     * changed sub-region. With pruning off the walk visits EVERY node, so the per-pass cost is fixed
     * and the full budget would multiply it — trim back to a handful of coarser rects there. */
    if (!render_full && !nothing_dirty && !s_prune_ok)
        er_damage_set_limit(&dmg, UNPRUNED_PASS_RECTS_MAX);

    /* The render set is final here (damage pre-pass + any multi-buffer debt replay): record it as
     * this frame's repaint rects — exactly what the passes below scissor to and what the backend
     * flushes, so the summed area is the number both the raster and present phases scale with.
     * Exposed via er_get_dirty_rects() and the perf counters. */
    if (nothing_dirty)
    {
        /* Nothing painted: no perf contribution (frame counters were reset at frame begin), and the set
         * is left holding the last commit that DID paint. */
    }
    else if (render_full)
    {
        er_damage_set_clear(&s_last_paint_set);
        er_damage_set_add(&s_last_paint_set, rb_x0, rb_y0, rb_x1 - rb_x0, rb_y1 - rb_y0);
        ER_PERF_REPAINT(rb_x0, rb_y0, rb_x1 - rb_x0, rb_y1 - rb_y0);
    }
    else
    {
        s_last_paint_set = dmg;
        for (uint8_t ri = 0U; ri < dmg.count; ri++)
            ER_PERF_REPAINT(dmg.r[ri].x, dmg.r[ri].y, dmg.r[ri].w, dmg.r[ri].h);
    }

    ER_PERF_RASTER_END(ER_PERF_RASTER_PREPASS);
    ER_PERF_RASTER_BEGIN(ER_PERF_RASTER_RENDER);

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
             * extra horizontal fill is negligible. The damage rects still bound the dirty rows: strips
             * run only over the set's merged row ranges, so two changes far apart vertically skip the
             * clean rows between them (retained by panel GRAM), instead of sweeping the whole span. */
            int ry0s[ER_DAMAGE_RECTS_MAX];
            int ry1s[ER_DAMAGE_RECTS_MAX];
            int nranges = 0;
            if (render_full)
            {
                ry0s[0] = rb_y0;
                ry1s[0] = rb_y1;
                nranges = 1;
            }
            else
            {
                /* Collect each rect's row interval (insertion sort by y0 — count <= ER_DAMAGE_RECTS_MAX),
                 * then merge overlapping/adjacent intervals: disjoint rects can still share rows (side by
                 * side). */
                for (uint8_t ri = 0U; ri < dmg.count; ri++)
                {
                    const int y0 = dmg.r[ri].y;
                    const int y1 = dmg.r[ri].y + dmg.r[ri].h;
                    int j = nranges++;
                    while (j > 0 && ry0s[j - 1] > y0)
                    {
                        ry0s[j] = ry0s[j - 1];
                        ry1s[j] = ry1s[j - 1];
                        j--;
                    }
                    ry0s[j] = y0;
                    ry1s[j] = y1;
                }
                int merged = 0;
                for (int j = 1; j < nranges; j++)
                {
                    if (ry0s[j] <= ry1s[merged])
                    {
                        if (ry1s[j] > ry1s[merged])
                            ry1s[merged] = ry1s[j];
                    }
                    else
                    {
                        merged++;
                        ry0s[merged] = ry0s[j];
                        ry1s[merged] = ry1s[j];
                    }
                }
                nranges = merged + 1;
            }

            const int bh = backend->band_height;
            const int fx = root->computed.x;
            const int fw = root->computed.w;
            /* ONE clip over the whole repaint bbox for the duration (enabling subtree pruning and,
             * crucially, keeping transform/opacity scratch sources complete — it contains every
             * changed node in full). Row narrowing happens purely via the strip loop + er_set_band. */
            er_push_clip_rect(fx, rb_y0, fw, rb_y1 - rb_y0);
            for (int r = 0; r < nranges; r++)
            {
                for (int sy = ry0s[r]; sy < ry1s[r]; sy += bh)
                {
                    const int sh = (ry1s[r] - sy < bh) ? (ry1s[r] - sy) : bh;
                    if (backend->band_begin)
                    {
                        ER_PERF_RASTER_BEGIN(ER_PERF_RASTER_BLIT);
                        backend->band_begin(fx, sy, fw, sh, backend->ctx);
                        ER_PERF_RASTER_END(ER_PERF_RASTER_BLIT);
                    }
                    er_set_band(sy, sh);
                    render_tree(root, true, false, 0, s_kbd_avoid_y); /* whole scene shifted up to clear the keyboard */
                    er_keyboard_draw(fw, (int)root->computed.h);      /* overlay (no-op for bands above the strip) */
                    if (backend->band_flush)
                    {
                        ER_PERF_RASTER_BEGIN(ER_PERF_RASTER_BLIT);
                        backend->band_flush(backend->ctx);
                        ER_PERF_RASTER_END(ER_PERF_RASTER_BLIT);
                    }
                }
            }
            er_set_band(0, 0);
            er_pop_clip_rect();
        }
    }
    else
    {
        const int nworkers = er_render_workers_active();
        /* One single-core frame when a parallel frame wanted a fade-cache capture and the content
         * has not changed since — the capture happens below, and following frames hit the cache
         * from inside the parallel fork. Consumed ONCE per commit, before any per-rect pass. */
        const bool serial_for_capture = s_fade_capture_wanted && (s_fade_capture_wanted_gen == s_content_gen);
        s_fade_capture_wanted = false;
        /* Parallel gate minus the region-height test, which is per-region below. */
        const bool can_parallel = nworkers > 1 && !serial_for_capture && s_parallel_unsafe == 0;
        /* Single buffer: rely on the propagated dirty flags inside the clip (parent_dirty = false) so only
         * the changed nodes repaint. Multi-buffer replay must instead FULLY recomposite the clipped region
         * (parent_dirty = true) — the replayed history covers nodes with no live dirty flag this frame, and
         * the stale buffer needs them all repainted. Matches the banded path, which always recomposites. */
        const bool full_recomposite = (s_display_buffer_count > 1);

        if (nothing_dirty)
        {
            /* Nothing to paint — but still walk once, unclipped, so any node whose damage clamped
             * off-screen (e.g. dirtied while scrolled out of view) is reached, stamped, and has its
             * stale flags cleared in the post-pass. The walk is marked occluded, which is exactly
             * "do the bookkeeping, emit nothing": parent_dirty = false alone would not do it, since
             * the dirty chain reaches the ROOT and its background would repaint the whole screen. */
            render_tree(root, false, true, 0, s_kbd_avoid_y);
            er_keyboard_draw((int)root->computed.w, (int)root->computed.h);
        }
        else if (render_full)
        {
            /* Full repaint: one unclipped pass over the whole tree (sliced across workers when the
             * screen is tall enough to pay for the fork). */
            if (can_parallel && (rb_y1 - rb_y0) >= nworkers * 16)
            {
                ParallelRenderJob job;
                job.root = root;
                job.full_recomposite = full_recomposite;
                job.x0 = rb_x0;
                job.x1 = rb_x1;
                job.ry0 = rb_y0;
                job.ry1 = rb_y1;
                job.nslices = nworkers;
                job.kbd_y = s_kbd_avoid_y;
                s_parallel_render = true;
                er_parallel_for(render_slice_job, &job);
                s_parallel_render = false;
                s_parallel_frames++;
                er_push_clip_rect(rb_x0, rb_y0, rb_x1 - rb_x0, rb_y1 - rb_y0);
                er_keyboard_draw((int)root->computed.w, (int)root->computed.h); /* overlay, sequential */
                er_pop_clip_rect();
            }
            else
            {
                render_tree(root, full_recomposite, false, 0, s_kbd_avoid_y);
                er_keyboard_draw((int)root->computed.w, (int)root->computed.h);
            }
        }
        else
        {
            /* Damage-clipped render: one pass PER DISJOINT RECT, so two changes in opposite corners
             * repaint two small areas instead of the span between them. Multi-pass is safe by
             * construction: passes only stamp painted_seq (flags clear in the sequential post-pass
             * below), the rects are pairwise disjoint so no pixel composites twice (which would darken
             * translucent blends), and each pass prunes to its own clip so a pass over a small rect
             * walks only that corner's subtrees. Each rect independently takes the sliced parallel
             * fork when it is tall enough to pay for it (see render_slice_job — slices behave exactly
             * like the serial damage clip). The persistent framebuffer retains untouched pixels. */
            bool any_parallel = false;
            for (uint8_t ri = 0U; ri < dmg.count; ri++)
            {
                const ERRect* R = &dmg.r[ri];
                if (can_parallel && R->h >= nworkers * 16)
                {
                    ParallelRenderJob job;
                    job.root = root;
                    job.full_recomposite = full_recomposite;
                    job.x0 = R->x;
                    job.x1 = R->x + R->w;
                    job.ry0 = R->y;
                    job.ry1 = R->y + R->h;
                    job.nslices = nworkers;
                    job.kbd_y = s_kbd_avoid_y;
                    s_parallel_render = true;
                    er_parallel_for(render_slice_job, &job);
                    s_parallel_render = false;
                    any_parallel = true;
                }
                else
                {
                    er_push_clip_rect(R->x, R->y, R->w, R->h);
                    render_tree(root, full_recomposite, false, 0, s_kbd_avoid_y); /* scene shifted up for keyboard */
                    er_pop_clip_rect();
                }
                er_push_clip_rect(R->x, R->y, R->w, R->h);
                er_keyboard_draw((int)root->computed.w, (int)root->computed.h); /* overlay, per-rect clip */
                er_pop_clip_rect();
            }
            if (any_parallel)
                s_parallel_frames++;
        }
    }

    ER_PERF_RASTER_END(ER_PERF_RASTER_RENDER);
    /* Workers have joined: fold their backend-blit time + pixel counts into the frame. (RENDER was
     * timed WITH the blits inside it; er_perf subtracts the blit total back out at frame end so the
     * buckets stay disjoint.) */
    ER_PERF_BLIT_COLLECT();
    ER_PERF_RASTER_BEGIN(ER_PERF_RASTER_SWEEP);

    /* Merge every worker's dirty-rect accumulator into this commit's union (single worker: a copy).
     * Kept local and published below, so a commit that paints nothing leaves the previous one's union
     * in place instead of clearing it. */
    ERRect painted_rect = {0, 0, 0, 0};
    bool painted_has = false;
    for (int i = 0; i < ERUI_RENDER_WORKERS; i++)
    {
        const CompCtx* wc = &s_comp_ctx[i];
        if (!wc->has_dirty)
            continue;
        if (!painted_has)
        {
            painted_rect = wc->dirty_rect;
            painted_has = true;
        }
        else
        {
            const int x2 = painted_rect.x + painted_rect.w > wc->dirty_rect.x + wc->dirty_rect.w
                               ? painted_rect.x + painted_rect.w
                               : wc->dirty_rect.x + wc->dirty_rect.w;
            const int y2 = painted_rect.y + painted_rect.h > wc->dirty_rect.y + wc->dirty_rect.h
                               ? painted_rect.y + painted_rect.h
                               : wc->dirty_rect.y + wc->dirty_rect.h;
            if (wc->dirty_rect.x < painted_rect.x)
                painted_rect.x = wc->dirty_rect.x;
            if (wc->dirty_rect.y < painted_rect.y)
                painted_rect.y = wc->dirty_rect.y;
            painted_rect.w = x2 - painted_rect.x;
            painted_rect.h = y2 - painted_rect.y;
        }
    }

    /* Sequential post-pass: clear the dirty flags of every node painted this commit. The paint
     * traversal itself only stamps painted_seq (an idempotent same-value write), so concurrent
     * workers never race on flag mutation, and a dirty node that was NOT painted (e.g. scrolled
     * offscreen) keeps its flags — exactly the previous clear-during-paint semantics. */
    for (int i = 0; i < (int)ERUI_MAX_NODES; i++)
    {
        ERNode* pn = &s_nodes[i];
        if (!pn->in_use)
            continue;

        pn->overflow_toggled = false;
        if (pn->painted_seq == s_commit_seq || pn->subtree_hidden)
        {
            pn->dirty = false;
            pn->source_dirty = false;
        }
    }

    /* Publish only when this commit painted, so the accessors keep reporting the last commit that did
     * (both stay in step with s_last_paint_set above).
     *
     * Two cases report the painted region rather than render_tree's accumulator, because that
     * accumulator only unions SOURCE-dirty nodes:
     *   - a full repaint, which may have no source-dirty node at all (a re-installed framebuffer, a
     *     reset) yet still paints every pixel of the root — it would otherwise report nothing;
     *   - multi-buffer, where the buffer is brought current over its whole replayed debt, so a host
     *     using the rect for a secondary transfer must see that, not just this frame's changes.
     * rb_* is exactly what s_last_paint_set recorded above, so the two accessors agree by construction.
     * A damage-clipped single-buffer commit keeps the tight source_dirty union. */
    if (!nothing_dirty)
    {
        if (render_full || s_display_buffer_count > 1)
        {
            painted_rect.x = rb_x0;
            painted_rect.y = rb_y0;
            painted_rect.w = rb_x1 - rb_x0;
            painted_rect.h = rb_y1 - rb_y0;
            painted_has = (painted_rect.w > 0 && painted_rect.h > 0);
        }
        s_dirty_rect = painted_rect;
        s_has_dirty = painted_has;
    }

    s_force_full_repaint = false;
    s_kbd_dirty = false;                 /* the keyboard strip (if any) was repainted this commit */
    er_damage_set_clear(&s_removed_set); /* consumed (or covered by a full repaint) this commit */

    ER_PERF_RASTER_END(ER_PERF_RASTER_SWEEP);
    ER_PERF_END(ER_PERF_PHASE_RASTER);

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

int er_get_dirty_rects(ERRect* out, int max_rects)
{
    const int count = (int)s_last_paint_set.count;
    if (!out || max_rects <= 0)
        return count; /* size query */
    if (count <= max_rects)
    {
        for (int i = 0; i < count; i++)
            out[i] = s_last_paint_set.r[i];
        return count;
    }
    /* Caller's buffer is too small: collapse to the covering bounding box so the guarantee ("the
     * returned rects cover every modified pixel") holds no matter what capacity was passed. */
    er_damage_set_bounds(&s_last_paint_set, &out[0]);
    return 1;
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

uint32_t er_parallel_frames(void)
{
    return s_parallel_frames;
}
