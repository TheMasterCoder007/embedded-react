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

void er_mark_dirty_upward(ERNode* node)
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
    *ry = (int)n->animated.y - sy + (int)n->tp_translate_y;
    *rw = (int)n->animated.w;
    *rh = (int)n->animated.h;
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

            if (er_transform_homography_invert(H, xf_inv_H) && er_transform_source_begin(px, py, w, h))
            {
                doing_affine = true;
                doing_3d = true;
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

            if (er_transform_invert(a, b, c, d, ftx, fty, &xf_ia, &xf_ib, &xf_ic, &xf_id, &xf_itx, &xf_ity)
                && er_transform_source_begin(px, py, w, h))
            {
                doing_affine = true;
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
    }

    /* Opacity compositing: View-family nodes with opacity < 255 render into an off-screen
     * scratch slot which is then blended at the node's alpha.  er_scratch_push returns
     * false when the pool is exhausted or the node is too large; in that case the subtree
     * renders at full opacity (graceful degradation). */
    const uint8_t node_opacity =
        (n->type == ER_NODE_VIEW || n->type == ER_NODE_SCROLL_VIEW || n->type == ER_NODE_PRESSABLE
         || (n->type == ER_NODE_MODAL && n->modal_visible) || n->type == ER_NODE_TEXT_INPUT)
            ? n->props.view.opacity
            : 255U;
    const bool use_scratch = (node_opacity < 255U) && should_render && er_scratch_push(px, py, w, h);

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
                    int no = 0, np = 0;
                    const float* vops = er_vector_slot_ops(n->vector_slot, &no);
                    const ERVectorPaint* vpa = er_vector_slot_paints(n->vector_slot, &np);
                    if (vops && no > 0)
                        er_vector_render(vops, no, vpa, np, px, py, w, h);
                }
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

    n->dirty = false;

    /* Overflow clipping: push a scissor rect so children cannot draw outside this node. */
    const bool clips = (n->layout.overflow == ER_OVERFLOW_HIDDEN || n->layout.overflow == ER_OVERFLOW_SCROLL);
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

    /* Blend the scratch slot back at this node's opacity. */
    if (use_scratch)
        er_scratch_pop_blend(node_opacity, px, py, w, h);

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
    }
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

void er_node_set_vector_ops(ERNode* node, const float* ops, int n_ops, const ERVectorPaint* paints, int n_paints)
{
    if (!node || node->type != ER_NODE_VECTOR)
        return;
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
    node->vector_slot = er_vector_store(node->vector_slot, ops, n_ops, paints, n_paints);
    /* Geometry is visual-only (the box comes from layout/style), so no layout pass is needed. */
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

void er_commit(void)
{
    if (s_root_tag == ER_INVALID_TAG)
        return;

    ERNode* root = er_get_node(s_root_tag);
    if (!root)
        return;

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
    bool clipped = false;
    if (s_force_full_repaint)
    {
        root->dirty = true; /* force the whole tree to repaint into the fresh/invalidated framebuffer */
    }
    else
    {
        ERRect clip = {0, 0, 0, 0};
        bool have = false;
        bool trackable = true;
        /* Seed with any pixels vacated by removed/destroyed nodes since the last commit. */
        if (s_have_removed_damage)
            damage_union(&clip, &have, s_removed_damage.x, s_removed_damage.y, s_removed_damage.w, s_removed_damage.h);
        for (uint16_t tag = 0U; tag < (uint16_t)ERUI_MAX_NODES; tag++)
        {
            ERNode* n = er_get_node(tag);
            if (!n)
                continue;
            int rx, ry, rw, rh;
            if (!node_screen_rect(n, &rx, &ry, &rw, &rh))
            {
                /* Complex transform: only forces a full repaint if this node is actually changing. */
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
                continue;                               /* unchanged and in place: contributes nothing to the damage */
            damage_union(&clip, &have, rx, ry, rw, rh); /* new position */
            if (n->has_last_paint)
                damage_union(&clip,
                             &have,
                             (int)n->last_paint_rect.x,
                             (int)n->last_paint_rect.y,
                             (int)n->last_paint_rect.w,
                             (int)n->last_paint_rect.h); /* old position (erase trail) */
        }
        if (trackable && have)
        {
            /* Pad for anti-aliased / sub-pixel edges, then clamp to the root rect. */
            const int margin = 2;
            int cx0 = clip.x - margin;
            int cy0 = clip.y - margin;
            int cx1 = clip.x + clip.w + margin;
            int cy1 = clip.y + clip.h + margin;
            const int rx0 = root->computed.x;
            const int ry0 = root->computed.y;
            const int rx1 = root->computed.x + root->computed.w;
            const int ry1 = root->computed.y + root->computed.h;
            if (cx0 < rx0)
                cx0 = rx0;
            if (cy0 < ry0)
                cy0 = ry0;
            if (cx1 > rx1)
                cx1 = rx1;
            if (cy1 > ry1)
                cy1 = ry1;
            if (cx1 > cx0 && cy1 > cy0)
            {
                er_push_clip_rect(cx0, cy0, cx1 - cx0, cy1 - cy0);
                clipped = true;
            }
        }
        /* trackable && !have: nothing is dirty this commit, render_tree() repaints nothing anyway.
         * !trackable: leave unclipped for a full repaint. */
    }

    render_tree(root, false, 0, 0);

    if (clipped)
        er_pop_clip_rect();

    s_force_full_repaint = false;
    s_have_removed_damage = false; /* consumed (or covered by a full repaint) this commit */
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
