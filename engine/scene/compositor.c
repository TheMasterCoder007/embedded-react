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
#include "er_error_screen.h"
#include "er_limits.h"
#include "er_node_internal.h"
#include "er_perf.h"
#include "font_registry.h"
#include "gradient.h"
#include "image_registry.h"
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

/* Occlusion culling: skip layers that a fully opaque node painted on top of them completely covers.
 * On by default (the engine CMake plumbs the option through); the fallback keeps consumers that
 * compile the sources directly — the ESP-IDF / Pico components — from having to know about it. */
#ifndef ERUI_OCCLUSION_CULLING
#define ERUI_OCCLUSION_CULLING 1
#endif

/**
 * @brief Text-input cursor blink period in milliseconds; the cursor shows for the first half of each.
 *
 * The half is derived, not written down twice, because the two uses are far apart and must agree
 * exactly: er_commit dirties the focused input when the phase FLIPS, and the render pass decides
 * whether to draw the caret. If they ever disagreed the caret would change state on frames nothing
 * repainted (or repaint on frames it did not change) — which surfaces as flicker, not as a blink
 * with the wrong timing.
 */
#define ER_CURSOR_BLINK_MS 1000U

/** @brief A cursor blink phase that matches neither 0 nor 1, so the next commit sees a change. */
#define ER_CURSOR_PHASE_UNKNOWN 2U

/** @brief Whether the text cursor is in the visible half of its blink cycle at @p now_ms. */
static inline bool cursor_blink_on(uint64_t now_ms)
{
    /* A 32-bit modulo, which an MCU does in hardware; the phase slips once per 49.7-day wrap. */
    return ((uint32_t)now_ms % ER_CURSOR_BLINK_MS) < (ER_CURSOR_BLINK_MS / 2U);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERNode s_nodes[ERUI_MAX_NODES];
static uint16_t s_next_tag = 0;
static uint16_t s_free_list[ERUI_MAX_NODES]; /**< LIFO stack of destroyed node slots available for reuse. */
static uint16_t s_free_count = 0;            /**< Number of entries currently in s_free_list. */
static uint32_t s_node_serial = 1;           /**< Next ERNode::serial, never 0; er_reset keeps it. */
static uint16_t s_root_tag = ER_INVALID_TAG;
static int16_t s_bounds_width = 0; /**< er_set_screen_bounds(): the largest the root may be; 0 = no limit. */
static int16_t s_bounds_height = 0;
static uint64_t s_now_ms = 0;
static uint16_t s_focused_input_tag = ER_INVALID_TAG; /**< Currently focused TextInput node. */
static uint8_t s_last_cursor_phase = ER_CURSOR_PHASE_UNKNOWN; /**< Last cursor blink phase (0/1) seen by er_commit. */
static bool s_keyboard_dirty =
    false; /**< Set when the keyboard shows/hides/switches layer so the next commit repaints it. */
static uint8_t s_keyboard_layer = 0; /**< Active key layer index into the (default or app-supplied) layout. */
static int s_keyboard_avoid_y = 0;   /**< Pixels the whole scene is shifted UP so a focused input clears the keyboard
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
/* Cached subtree paint bounds (sub_* / subtree_prunable) need recomputing, but nothing moved: set by
 * er_request_subtree_bounds_pass() so an animation-only frame can refresh them without a layout pass. */
static bool s_subtree_bounds_dirty = false;

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
 * er_display_present() advances s_current_buffer, so a buffer accumulates every commit that lands while some
 * OTHER buffer is being shown — exactly what it must replay when it next comes around. This is robust to
 * any commit:present cadence (React drives commits in bursts; the host drives presents), unlike a
 * commit-counting ring. Index [0] is the sole slot used when count == 1 (pure incremental). */
static ERDamageSlot s_buffer_debt[ER_DISPLAY_BUFFERS_MAX];

/* Index of the buffer currently being rendered; advanced by er_display_present() at each page-flip. */
static int s_current_buffer = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Fills an ERLayoutSpec with Yoga-compatible defaults (all fields auto).
 *
 * @param[in,out] spec  Layout spec to initialise.
 */
static void init_layout_defaults(ERLayoutSpec* spec)
{
    spec->left = spec->top = spec->right = spec->bottom = ER_LAYOUT_AUTO;
    spec->width = spec->height = ER_LAYOUT_AUTO;
    spec->min_width = spec->max_width = ER_LAYOUT_AUTO;
    spec->min_height = spec->max_height = ER_LAYOUT_AUTO;
    spec->padding = ER_LAYOUT_AUTO;
    spec->padding_left = spec->padding_top = spec->padding_right = spec->padding_bottom = ER_LAYOUT_AUTO;
    spec->margin = ER_LAYOUT_AUTO;
    spec->margin_left = spec->margin_top = spec->margin_right = spec->margin_bottom = ER_LAYOUT_AUTO;
    spec->gap = spec->row_gap = spec->column_gap = ER_LAYOUT_AUTO;
    spec->flex_grow = 0;
    spec->flex_shrink = 0;
    spec->flex_basis = ER_LAYOUT_AUTO;
    spec->flex_direction = ER_FLEX_COLUMN;
    spec->flex_wrap = ER_WRAP_NOWRAP;
    spec->align_items = ER_ALIGN_STRETCH;
    spec->align_self = ER_ALIGN_AUTO;
    spec->align_content = ER_ALIGN_CONTENT_FLEX_START;
    spec->justify_content = ER_JUSTIFY_FLEX_START;
    spec->position = ER_POSITION_RELATIVE;
    spec->aspect_ratio = 0.0f;
    spec->flex_basis_percent = 0.0f;
    spec->width_percent = 0.0f;
    spec->height_percent = 0.0f;
    spec->left_percent = spec->top_percent = spec->right_percent = spec->bottom_percent = 0.0f;
}

/**
 * @brief Returns the sibling after child, or NULL at the end of the list or at a tag that no longer resolves.
 *
 * @param[in] child  Current child.
 *
 * @return The next sibling, or NULL.
 */
static ERNode* sibling_after(const ERNode* child)
{
    return er_get_node(child->next_sibling_tag);
}

/* Documented in er_node_internal.h. */
ERNode* er_child_first(ERChildCursor* cursor, const ERNode* parent)
{
    cursor->parent = parent;
    cursor->node = er_get_node(parent->first_child_tag);
    cursor->z_index = 0;
    cursor->mixed_z = false;
    if (!cursor->node)
        return NULL;

    /* The walk starts at the lowest zIndex; finding it also tells whether there is more than one. */
    ERNode* lowest = cursor->node;
    for (ERNode* sibling = sibling_after(lowest); sibling; sibling = sibling_after(sibling))
    {
        if (sibling->z_index != lowest->z_index)
            cursor->mixed_z = true;
        if (sibling->z_index < lowest->z_index)
            lowest = sibling;
    }
    cursor->node = lowest;
    cursor->z_index = lowest->z_index;
    return lowest;
}

/* Documented in er_node_internal.h. */
ERNode* er_child_next(ERChildCursor* cursor)
{
    if (!cursor->node)
        return NULL;

    for (ERNode* sibling = sibling_after(cursor->node); sibling; sibling = sibling_after(sibling))
    {
        if (sibling->z_index == cursor->z_index)
        {
            cursor->node = sibling;
            return sibling;
        }
    }

    /* This zIndex is done: continue at the first child of the lowest zIndex above it. */
    ERNode* next = NULL;
    if (cursor->mixed_z)
    {
        for (ERNode* sibling = er_get_node(cursor->parent->first_child_tag); sibling; sibling = sibling_after(sibling))
        {
            if (sibling->z_index > cursor->z_index && (!next || sibling->z_index < next->z_index))
                next = sibling;
        }
    }
    cursor->node = next;
    if (next)
        cursor->z_index = next->z_index;
    return next;
}

/**
 * @brief Linearly interpolates a single ARGB8888 color channel.
 *
 * @param[in] from      From channel value.
 * @param[in] to        To channel value.
 * @param[in] fraction  Interpolation fraction [0.0, 1.0].
 *
 * @return Interpolated channel value.
 */
static uint8_t lerp_ch(uint8_t from, uint8_t to, float fraction)
{
    return (uint8_t)((float)from + ((float)to - (float)from) * fraction + 0.5f);
}

/**
 * @brief Linearly interpolates between two ARGB8888 colors per-channel.
 *
 * @param[in] from      From color.
 * @param[in] to        To color.
 * @param[in] fraction  Interpolation fraction [0.0, 1.0].
 *
 * @return Interpolated ARGB8888 color.
 */
static uint32_t lerp_color32(uint32_t from, uint32_t to, float fraction)
{
    const uint8_t alpha = lerp_ch((uint8_t)(from >> 24), (uint8_t)(to >> 24), fraction);
    const uint8_t red = lerp_ch((uint8_t)(from >> 16), (uint8_t)(to >> 16), fraction);
    const uint8_t green = lerp_ch((uint8_t)(from >> 8), (uint8_t)(to >> 8), fraction);
    const uint8_t blue = lerp_ch((uint8_t)from, (uint8_t)to, fraction);
    return ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

/**
 * @brief Renders an ActivityIndicator as a ring of 8 fading dots.
 *
 * The current spin angle is read from node->transform_rotate_z (driven by a looping rotate_z
 * animation started when animating=1).  Each dot's opacity fades from the leading
 * dot (full) to the trailing dot (~15%), creating the classic spinner illusion.
 *
 * @param[in] node    ActivityIndicator node to render.
 * @param[in] box_x   Left edge of the node in framebuffer pixels.
 * @param[in] box_y   Top edge of the node in framebuffer pixels.
 * @param[in] width   Width of the node in pixels.
 * @param[in] height  Height of the node in pixels.
 */
static void render_activity_indicator(const ERNode* node, int box_x, int box_y, int width, int height)
{
#define ACTIVITY_DOT_COUNT 8
#define ACTIVITY_DOT_DIVISOR 5    /* dot diameter is the spinner's over this */
#define ACTIVITY_RING_INSET 2     /* px between the dots and the box edge */
#define ACTIVITY_TRAIL_FADE 0.85f /* opacity the trailing dot loses */
#define ACTIVITY_DEFAULT_COLOR 0xFFFFFFFFU
    const int diameter = width < height ? width : height;
    const int dot_size = diameter / ACTIVITY_DOT_DIVISOR;
    if (dot_size < 1)
        return;

    const int center_x = box_x + width / 2;
    const int center_y = box_y + height / 2;
    const int ring_radius = diameter / 2 - dot_size / 2 - ACTIVITY_RING_INSET;

    const uint32_t base_color =
        node->props.activity_indicator.color ? node->props.activity_indicator.color : ACTIVITY_DEFAULT_COLOR;
    const float base_alpha = (float)((base_color >> 24) & 0xFFU);
    /* The spin angle is the node's rotation, which app math can make infinite (a NaN one is already 0): every
     * dot's position would then be NaN before its int cast. It is drawn as at 0 instead. */
    const float angle_offset_deg = fabsf(node->transform_rotate_z) < INFINITY ? node->transform_rotate_z : 0.0f;

    for (int dot_index = 0; dot_index < ACTIVITY_DOT_COUNT; dot_index++)
    {
        const float angle_deg = angle_offset_deg + (float)dot_index * (360.0f / ACTIVITY_DOT_COUNT);
        const float angle_rad = angle_deg * ER_DEG2RAD;
        const int dot_center_x = center_x + (int)((float)ring_radius * cosf(angle_rad) + 0.5f);
        const int dot_center_y = center_y + (int)((float)ring_radius * sinf(angle_rad) + 0.5f);
        const int dot_left = dot_center_x - dot_size / 2;
        const int dot_top = dot_center_y - dot_size / 2;

        /* Leading dot (dot_index 0) is full opacity; trailing dots fade to ~15%. */
        const float fade = 1.0f - (float)dot_index / ACTIVITY_DOT_COUNT * ACTIVITY_TRAIL_FADE;
        const uint8_t alpha = (uint8_t)(base_alpha * fade + 0.5f);
        const uint32_t dot_color = ((uint32_t)alpha << 24) | (base_color & 0x00FFFFFFu);

        er_rrect_fill_bordered(dot_color, 0x00000000U, 0, dot_left, dot_top, dot_size, dot_size, dot_size / 2);
    }
#undef ACTIVITY_DOT_COUNT
#undef ACTIVITY_DOT_DIVISOR
#undef ACTIVITY_RING_INSET
#undef ACTIVITY_TRAIL_FADE
#undef ACTIVITY_DEFAULT_COLOR
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
    int tile_width;
    int tile_height;
    uint32_t band_generation_counter;
    uint32_t current_loop_generation;
    bool tile_first;
    ERRect dirty_rect; /**< This worker's union of painted-node rects this commit. */
    bool has_dirty;
    bool transform_capturing;             /**< A transformed ancestor's scratch capture is active: everything rendering
                                        below it is in SOURCE space and reaches the screen only through that
                                        ancestor's inverse-map blit. */
    ERRect transform_capture_destination; /**< Where that blit writes — the ancestor's transformed screen AABB. */
} CompCtx;

static CompCtx s_composite_contexts[ERUI_RENDER_WORKERS];

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
static uint32_t s_fade_capture_wanted_generation = 0;

/** @brief The calling worker's composite context (constant &s_composite_contexts[0] in single-worker builds). */
static inline CompCtx* cc(void)
{
    return &s_composite_contexts[er_render_worker_id()];
}

/**
 * @brief Expands the per-commit dirty rectangle to include the given screen-space rect.
 *
 * Called from render_tree() whenever a node is actually repainted. The union grows
 * monotonically within each commit and is reset at the start of the next one.
 *
 * @param[in] x       Left edge of the repainted region in framebuffer pixels.
 * @param[in] y       Top edge of the repainted region in framebuffer pixels.
 * @param[in] width   Width of the repainted region in pixels.
 * @param[in] height  Height of the repainted region in pixels.
 */
static void union_dirty_rect(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    if (!cc()->has_dirty)
    {
        cc()->dirty_rect.x = x;
        cc()->dirty_rect.y = y;
        cc()->dirty_rect.width = width;
        cc()->dirty_rect.height = height;
        cc()->has_dirty = true;
        return;
    }

    const int right = cc()->dirty_rect.x + cc()->dirty_rect.width;
    const int bottom = cc()->dirty_rect.y + cc()->dirty_rect.height;
    const int new_right = x + width;
    const int new_bottom = y + height;

    if (x < cc()->dirty_rect.x)
        cc()->dirty_rect.x = x;
    if (y < cc()->dirty_rect.y)
        cc()->dirty_rect.y = y;
    cc()->dirty_rect.width = (new_right > right ? new_right : right) - cc()->dirty_rect.x;
    cc()->dirty_rect.height = (new_bottom > bottom ? new_bottom : bottom) - cc()->dirty_rect.y;
}

void er_force_full_repaint(void)
{
    s_force_full_repaint = true;
}

/* Mark every rotating buffer as owing a full-screen repaint (first frame, reset, or a full-repaint
 * request): each buffer repaints fully the next time it is rendered, then reverts to incremental. */
static void debt_reset_all_full(void)
{
    for (int buffer_index = 0; buffer_index < ER_DISPLAY_BUFFERS_MAX; buffer_index++)
    {
        s_buffer_debt[buffer_index].full = true;
        er_damage_set_clear(&s_buffer_debt[buffer_index].set);
    }
}

void er_set_display_buffer_count(int count)
{
    if (count < 1)
        count = 1;
    if (count > ER_DISPLAY_BUFFERS_MAX)
        count = ER_DISPLAY_BUFFERS_MAX;
    s_display_buffer_count = count;
    s_current_buffer = 0;
    debt_reset_all_full(); /* every rotating buffer must start from a full frame */
}

int er_get_display_buffer_count(void)
{
    return s_display_buffer_count;
}

void er_display_present(void)
{
    /* One page-flip has occurred: the next commit renders the next rotating buffer. Wrapping by the
     * buffer count keeps s_current_buffer a valid debt index; for count == 1 this is a no-op. */
    if (s_display_buffer_count > 1)
        s_current_buffer = (s_current_buffer + 1) % s_display_buffer_count;
}

/* Global content generation: advanced by every mutation that changes rendered subtree content
 * (er_mark_dirty_upward and friends). The fade cache stores the generation it captured at; any
 * bump invalidates it. Deliberately coarse — a content change anywhere invalidates the one
 * cached subtree — which keeps the check O(1) and always correct. */
static uint32_t s_content_generation = 1U;

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
    s_content_generation++;
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
    s_content_generation++; /* a reflow does change rendered content, so the fade cache must drop */
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

void er_request_subtree_bounds_pass(void)
{
    s_subtree_bounds_dirty = true;
}

/**
 * @brief Renders the background and border of a View-family node.
 *
 * Resolves per-corner radii, per-edge widths/colors, and border style from ERViewProps,
 * then dispatches to er_rrect_fill_bordered (fast uniform path) or the general per-corner/per-edge
 * path using er_rrect_fill_corners and er_rrect_fill_ring_edges.
 *
 * @param[in] view_props  Visual properties of the node.
 * @param[in] box_x       Left edge of the node in framebuffer pixels (after scroll offset).
 * @param[in] box_y       Top edge of the node in framebuffer pixels.
 * @param[in] width       Node width in pixels.
 * @param[in] height      Node height in pixels.
 */
static void render_view_bg(const ERViewProps* view_props, int box_x, int box_y, int width, int height)
{
    /* Resolve per-corner radii (0 = fall back to uniform border_radius). */
    const int radius_top_left = view_props->border_top_left_radius > 0 ? (int)view_props->border_top_left_radius
                                                                       : (int)view_props->border_radius;
    const int radius_top_right = view_props->border_top_right_radius > 0 ? (int)view_props->border_top_right_radius
                                                                         : (int)view_props->border_radius;
    const int radius_bottom_right = view_props->border_bottom_right_radius > 0
                                        ? (int)view_props->border_bottom_right_radius
                                        : (int)view_props->border_radius;
    const int radius_bottom_left = view_props->border_bottom_left_radius > 0
                                       ? (int)view_props->border_bottom_left_radius
                                       : (int)view_props->border_radius;

    /* Resolve per-edge widths (0 = fall back to uniform border_width). */
    const int border_width_left =
        view_props->border_left_width > 0 ? (int)view_props->border_left_width : (int)view_props->border_width;
    const int border_width_top =
        view_props->border_top_width > 0 ? (int)view_props->border_top_width : (int)view_props->border_width;
    const int border_width_right =
        view_props->border_right_width > 0 ? (int)view_props->border_right_width : (int)view_props->border_width;
    const int border_width_bottom =
        view_props->border_bottom_width > 0 ? (int)view_props->border_bottom_width : (int)view_props->border_width;

    /* Resolve per-edge colors (0 = fall back to uniform border_color). */
    const uint32_t border_color_left =
        view_props->border_left_color ? view_props->border_left_color : view_props->border_color;
    const uint32_t border_color_top =
        view_props->border_top_color ? view_props->border_top_color : view_props->border_color;
    const uint32_t border_color_right =
        view_props->border_right_color ? view_props->border_right_color : view_props->border_color;
    const uint32_t border_color_bottom =
        view_props->border_bottom_color ? view_props->border_bottom_color : view_props->border_color;

    /* When a gradient is active (type != NONE and at least 2 stops) it fills the background;
     * skip background_color so gradient pixels remain visible beneath the border. */
#if ERUI_GRADIENT
    const uint32_t background_color =
        (view_props->gradient_type != ER_GRADIENT_NONE && view_props->gradient_stop_count >= 2u)
            ? 0u
            : view_props->background_color;
#else
    const uint32_t background_color = view_props->background_color;
#endif

    /* Fast path: uniform border width, color, radius, and solid style. */
    if (border_width_left == border_width_top && border_width_top == border_width_right
        && border_width_right == border_width_bottom && border_color_left == border_color_top
        && border_color_top == border_color_right && border_color_right == border_color_bottom
        && radius_top_left == radius_top_right && radius_top_right == radius_bottom_right
        && radius_bottom_right == radius_bottom_left && view_props->border_style == 0)
    {
        er_rrect_fill_bordered(
            background_color, border_color_left, border_width_left, box_x, box_y, width, height, radius_top_left);
        return;
    }

    const bool has_border =
        (border_width_left > 0 || border_width_top > 0 || border_width_right > 0 || border_width_bottom > 0);
    const bool uniform_width = (border_width_left == border_width_top && border_width_top == border_width_right
                                && border_width_right == border_width_bottom);
    const bool uniform_color = (border_color_left == border_color_top && border_color_top == border_color_right
                                && border_color_right == border_color_bottom);

    if (has_border && uniform_width && uniform_color && view_props->border_style == 0 && (border_color_left >> 24))
    {
        const int inner_x = box_x + border_width_left;
        const int inner_y = box_y + border_width_top;
        const int inner_width = width - border_width_left - border_width_right;
        const int inner_height = height - border_width_top - border_width_bottom;
        /* Inset radii: shrink each corner by the adjacent border width. */
        const int inner_radius_top_left =
            (radius_top_left > border_width_left) ? radius_top_left - border_width_left : 0;
        const int inner_radius_top_right =
            (radius_top_right > border_width_right) ? radius_top_right - border_width_right : 0;
        const int inner_radius_bottom_right =
            (radius_bottom_right > border_width_right) ? radius_bottom_right - border_width_right : 0;
        const int inner_radius_bottom_left =
            (radius_bottom_left > border_width_left) ? radius_bottom_left - border_width_left : 0;

        if ((background_color >> 24) == 0xFFu)
        {
            /* Uniform solid border with per-corner radii, opaque background: outer → background
             * inset. The background hides every border pixel it needs to. */
            er_rrect_fill_corners(border_color_left,
                                  box_x,
                                  box_y,
                                  width,
                                  height,
                                  radius_top_left,
                                  radius_top_right,
                                  radius_bottom_right,
                                  radius_bottom_left);
            if (inner_width > 0 && inner_height > 0)
                er_rrect_fill_corners(background_color,
                                      inner_x,
                                      inner_y,
                                      inner_width,
                                      inner_height,
                                      inner_radius_top_left,
                                      inner_radius_top_right,
                                      inner_radius_bottom_right,
                                      inner_radius_bottom_left);
        }
        else
        {
            /* Transparent, translucent, or gradient-backed: a filled border shape would cover what
             * is behind it (see er_rrect_fill_ring), so stroke the band only. */
            er_rrect_fill_ring(border_color_left,
                               box_x,
                               box_y,
                               width,
                               height,
                               radius_top_left,
                               radius_top_right,
                               radius_bottom_right,
                               radius_bottom_left,
                               border_width_left);
            if ((background_color >> 24) != 0 && inner_width > 0 && inner_height > 0)
                er_rrect_fill_corners(background_color,
                                      inner_x,
                                      inner_y,
                                      inner_width,
                                      inner_height,
                                      inner_radius_top_left,
                                      inner_radius_top_right,
                                      inner_radius_bottom_right,
                                      inner_radius_bottom_left);
        }
    }
    else
    {
        /* Per-edge widths/colours and/or a dashed or dotted style. The background fills the whole
         * rounded shape (a transparent or translucent edge shows it through, as it does on the web),
         * then the band is stroked on top following the same corners — including the dash pattern,
         * which is stepped by arc length so it flows through them. Drawn as four straight rects — as
         * this used to be — the border ignored borderRadius entirely and squared off every corner. */
        er_rrect_fill_corners(background_color,
                              box_x,
                              box_y,
                              width,
                              height,
                              radius_top_left,
                              radius_top_right,
                              radius_bottom_right,
                              radius_bottom_left);
        const ERRRectBorder border = {border_width_left,
                                      border_width_top,
                                      border_width_right,
                                      border_width_bottom,
                                      border_color_left,
                                      border_color_top,
                                      border_color_right,
                                      border_color_bottom,
                                      view_props->border_style};
        if (has_border)
            er_rrect_fill_ring_edges(box_x,
                                     box_y,
                                     width,
                                     height,
                                     radius_top_left,
                                     radius_top_right,
                                     radius_bottom_right,
                                     radius_bottom_left,
                                     &border);
    }
}

/**
 * @brief Pads a damage rect for anti-aliased / sub-pixel edges, clamps it to the root rect, adds it.
 *
 * Padding happens HERE, before insertion, so the set's disjointness invariant holds for the final
 * geometry — two rects 3 px apart that would overlap only after padding are coalesced instead of
 * stored "disjoint" and later double-painted. (Rects within 2×margin of each other auto-merge.)
 *
 * @param[in,out] set                     Damage set to add to.
 * @param[in]     x,y,width,height        Raw damage rectangle (screen space).
 * @param[in]     root_left,root_top      Root rect top-left (clamp bounds).
 * @param[in]     root_right,root_bottom  Root rect bottom-right exclusive (clamp bounds).
 */
static void add_damage(
    ERDamageSet* set, int x, int y, int width, int height, int root_left, int root_top, int root_right, int root_bottom)
{
    if (width <= 0 || height <= 0)
        return;
    const int margin = 2;
    int clamped_left = x - margin;
    int clamped_top = y - margin;
    int clamped_right = x + width + margin;
    int clamped_bottom = y + height + margin;
    if (clamped_left < root_left)
        clamped_left = root_left;
    if (clamped_top < root_top)
        clamped_top = root_top;
    if (clamped_right > root_right)
        clamped_right = root_right;
    if (clamped_bottom > root_bottom)
        clamped_bottom = root_bottom;
    er_damage_set_add(set,
                      clamped_left,
                      clamped_top,
                      clamped_right - clamped_left,
                      clamped_bottom - clamped_top); /* empty after clamping: ignored */
}

/**
 * @brief Sums the scroll offsets of every ScrollView / FlatList above a node, less their translates.
 *
 * render_tree carries this down the walk as its translation; the flat per-node damage passes have no
 * parent context, so they re-derive it here.
 *
 * @param[in]  node               Node to measure from (its own scroll offset and translate are NOT included).
 * @param[out] scroll_x,scroll_y  Receive the accumulated ancestor scroll, minus the accumulated ancestor translate.
 */
static void node_ancestor_scroll(const ERNode* node, int* scroll_x, int* scroll_y)
{
    *scroll_x = 0;
    *scroll_y = 0;
    const ERNode* ancestor = er_get_node(node->parent_tag);
    while (ancestor)
    {
        if (ancestor->type == ER_NODE_SCROLL_VIEW || ancestor->type == ER_NODE_FLAT_LIST)
        {
            *scroll_x += (int)ancestor->scroll_offset_x;
            *scroll_y += (int)ancestor->scroll_offset_y;
        }
        if (er_node_has_offset_translate(ancestor))
        {
            *scroll_x -= (int)ancestor->transform_translate_x;
            *scroll_y -= (int)ancestor->transform_translate_y;
        }
        ancestor = er_get_node(ancestor->parent_tag);
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
 * @param[in] node  Node whose ancestors to search.
 *
 * @return The capturing ancestor, or NULL when this node paints straight to the screen.
 */
static ERNode* capturing_transform_ancestor(const ERNode* node)
{
#if ERUI_TRANSFORMS_FULL
    ERNode* capturing = NULL;
    for (ERNode* ancestor = er_get_node(node->parent_tag); ancestor; ancestor = er_get_node(ancestor->parent_tag))
    {
        if (!er_node_can_capture_transform(ancestor))
            continue;
        /* The same pre-transform origin render_tree hands the matrix: layout position minus ancestor
         * scroll and the keyboard shift. Only the 3D homography's pivot actually depends on it, but
         * deriving it any other way here would be a second rule to keep in step. */
        int scroll_x, scroll_y;
        node_ancestor_scroll(ancestor, &scroll_x, &scroll_y);
        if (er_transform_is_invertible(ancestor,
                                       (int)ancestor->animated.x - scroll_x,
                                       (int)ancestor->animated.y - scroll_y - s_keyboard_avoid_y,
                                       (int)ancestor->animated.width,
                                       (int)ancestor->animated.height))
            capturing = ancestor;
    }
    return capturing;
#else
    (void)node;
    return NULL;
#endif
}

#if ERUI_TRANSFORMS_FULL
/**
 * @brief Whether any ancestor carries a transform that could take the capture away from this node.
 *
 * The cheap half of capturing_transform_ancestor(): the same walk, the same type/translate/size
 * filters, but no matrix and so no trig. It answers a weaker question — "could the capture belong to
 * someone above me?" rather than "who holds it?" — and errs the safe way, naming an ancestor whose
 * matrix turns out not to invert. That only ever widens a hedge, never narrows one.
 *
 * It exists because the answer decides whether a node's own footprint is knowable HERE. Only the
 * outermost admitted transform captures, so a node with such an ancestor paints its raw box in that
 * ancestor's source space, and one without paints its transformed AABB on screen — and nothing about
 * the node itself distinguishes the two. Asked of every complex-transformed node on every commit, so
 * the expensive invertibility test is deliberately left to the callers that need the exact answer.
 *
 * Defined only for ERUI_TRANSFORMS=FULL, the only build with a capture to take.
 *
 * @param[in] node  Node whose ancestors to search.
 *
 * @return true when some ancestor's transform could hold the capture.
 */
static bool capture_candidate_above(const ERNode* node)
{
    for (const ERNode* ancestor = er_get_node(node->parent_tag); ancestor; ancestor = er_get_node(ancestor->parent_tag))
    {
        if (er_node_can_capture_transform(ancestor))
            return true;
    }
    return false;
}
#endif /* ERUI_TRANSFORMS_FULL */

/**
 * @brief Whether a change ABOVE this node is about to repaint it.
 *
 * source_dirty marks the node a mutation actually landed on, so this is "something above me changed
 * its own content" — which is exactly when render_tree repaints this whole subtree for a reason the
 * node cannot see in itself. `dirty` would not do: it propagates to the root, so on any commit at all
 * every node would answer yes.
 *
 * Only asked of a node whose predicted footprint is uncertain (issue #139): the thing that flips a
 * nested transform between its AABB and its raw box is an ANCESTOR gaining or losing a transform, and
 * that leaves the node itself clean and its layout box exactly where it was.
 *
 * @param[in] node  Node whose ancestors to search.
 *
 * @return true when some ancestor was directly dirtied this commit.
 */
static bool source_dirty_above(const ERNode* node)
{
    for (const ERNode* ancestor = er_get_node(node->parent_tag); ancestor; ancestor = er_get_node(ancestor->parent_tag))
    {
        if (ancestor->source_dirty)
            return true;
    }
    return false;
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
 * @param[in] node              Node vacating the pixels (used to find its capturing ancestor, if any).
 * @param[in] x,y,width,height  Screen rect being vacated, as measured for @p node itself.
 */
static void note_vacated_rect(const ERNode* node, int x, int y, int width, int height)
{
    const ERNode* capturing = capturing_transform_ancestor(node);
    if (capturing && capturing->has_last_paint && !capturing->last_paint_untransformed)
    {
        x = (int)capturing->last_paint_rect.x;
        y = (int)capturing->last_paint_rect.y;
        width = (int)capturing->last_paint_rect.width;
        height = (int)capturing->last_paint_rect.height;
    }
    er_damage_set_add(&s_removed_set, x, y, width, height);
}

/**
 * @brief Accumulates a removed subtree's last-painted rects into the pending removal damage.
 *
 * Called while the subtree is still intact (before detach). The next er_commit() seeds its damage set
 * from this so the vacated pixels are repainted (erased) without forcing a full-screen redraw.
 *
 * @param[in] node  Root of the subtree being removed.
 */
static void note_removed_subtree(ERNode* node)
{
    if (!node)
        return;
    if (node->has_last_paint)
    {
        note_vacated_rect(node,
                          (int)node->last_paint_rect.x,
                          (int)node->last_paint_rect.y,
                          (int)node->last_paint_rect.width,
                          (int)node->last_paint_rect.height);
        node->has_last_paint = false;
    }
    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        note_removed_subtree(child);
        child_tag = child->next_sibling_tag;
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
 * @param[in,out] node             Subtree root to walk (NULL is ignored).
 * @param[in]     ancestor_hidden  Whether an ancestor of @p node is (or is inside) a display:none node.
 */
static void propagate_hidden(ERNode* node, bool ancestor_hidden)
{
    if (!node)
        return;

    const bool hidden = ancestor_hidden || (node->layout.display == ER_DISPLAY_NONE);
    if (hidden != node->subtree_hidden)
    {
        node->subtree_hidden = hidden;
        if (hidden)
        {
            if (node->has_last_paint)
            {
                note_vacated_rect(node,
                                  (int)node->last_paint_rect.x,
                                  (int)node->last_paint_rect.y,
                                  (int)node->last_paint_rect.width,
                                  (int)node->last_paint_rect.height);
                node->has_last_paint = false;
            }
        }
        else
        {
            node->dirty = true;
            node->source_dirty = true;
        }
    }

    /* Recurse unconditionally: an unchanged flag here says nothing about a subtree that was just
     * spliced in from somewhere else (append/insert re-runs this from the new parent). */
    for (uint16_t child_tag = node->first_child_tag; child_tag != ER_INVALID_TAG;)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        propagate_hidden(child, hidden);
        child_tag = child->next_sibling_tag;
    }
}

/** @brief True when @p node sits under a display:none ancestor (false for a node with no parent). */
static bool parent_hidden(const ERNode* node)
{
    const ERNode* parent = er_get_node(node->parent_tag);
    return parent && parent->subtree_hidden;
}

/**
 * @brief True when @p node confines its descendants' paint to its own box.
 *
 * Either an explicit clipping overflow, or a node type that always scrolls its content behind a
 * viewport. The distinction drives three separate things — the scissor render_tree pushes, the
 * subtree paint bounds, and how far a descendant's damage may reach — which must agree exactly, so
 * they all ask here rather than each spelling the predicate out.
 */
static bool node_clips_children(const ERNode* node)
{
    return node->layout.overflow == ER_OVERFLOW_HIDDEN || node->layout.overflow == ER_OVERFLOW_SCROLL
           || node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST;
}

/**
 * @brief Offset from a node's border-box origin to the origin its vec_dirty_* rect is measured from.
 *
 * The two rects are stored in different spaces, and both land on screen through this. An ER_NODE_VECTOR
 * records its tight sub-rect in TAPE coordinates, whose origin is the content box the tape now paints
 * from — so the padding has to be added back. An Arc's comes out of er_arc_geom() called at (0, 0),
 * which already resolves inside the content box, so its rect is border-box relative and needs nothing.
 *
 * @param[in]  node       Node holding the rect.
 * @param[out] offset_x,  offset_y  Offset to add to the node's screen origin.
 */
static void vec_dirty_offset(const ERNode* node, int* offset_x, int* offset_y)
{
    if (node->type != ER_NODE_VECTOR)
    {
        *offset_x = 0;
        *offset_y = 0;
        return;
    }
    const ERPadding padding = er_layout_padding(&node->layout);
    *offset_x = padding.left;
    *offset_y = padding.top;
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
 * @param[in]     node              Node to measure; its cached ERNode::arc_overhang is refreshed as a side effect.
 * @param[in,out] x,y,width,height  Rect grown in place by the knob's reach on every side.
 */
static void expand_for_arc(ERNode* node, int* x, int* y, int* width, int* height)
{
    if (node->type != ER_NODE_ARC)
        return;
    const int overhang = er_arc_refresh_overhang(node);
    *x -= overhang;
    *y -= overhang;
    *width += 2 * overhang;
    *height += 2 * overhang;
}

#if ERUI_SHADOWS
/**
 * @brief Grows a rect by however far this node's shadow bleeds past its layout box.
 *
 * A shadow is painted outside the box, so any rect meant to bound what the node PUTS ON SCREEN — its
 * damage, its erase trail, its reported repaint — has to include the bleed or the old shadow is never
 * erased and the new one is clipped at the damage edge. No-op for a node that casts none.
 *
 * Read from the node's CURRENT props, so it answers for the paint that is about to happen. What a
 * PREVIOUS paint put on screen is not re-derived from here — last_paint_rect records it already
 * inflated (see render_tree), because these props can change between the two commits and then this
 * function no longer describes what is on the panel.
 */
static void expand_for_shadow(const ERNode* node, int* x, int* y, int* width, int* height)
{
    if (!(node->type == ER_NODE_VIEW || node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_PRESSABLE
          || (node->type == ER_NODE_MODAL && node->modal_visible)))
        return;
    if (!(node->props.view.shadow_opacity > 0.0f || node->props.view.elevation > 0))
        return;
    const int radius = (int)node->props.view.shadow_radius;
    const int offset_x = (int)(fabsf(node->props.view.shadow_offset_x) + 0.5f);
    const int offset_y = (int)(fabsf(node->props.view.shadow_offset_y) + 0.5f);
    const int reach = radius + (offset_x > offset_y ? offset_x : offset_y);
    *x -= reach;
    *y -= reach;
    *width += 2 * reach;
    *height += 2 * reach;
}
#endif

/**
 * @brief Clips one axis of a rect, [*start, *start + *length), to ±ER_PAINT_RECT_MAX, keeping what it covers.
 *
 * Every rect the engine keeps in int16 fields goes through this first: a node's last paint and a vector node's
 * dirty rect. A cast alone would wrap a far edge (a shadow offset near 32767 widens a small node that far) into
 * a rect off to the side, which then erases or damages none of what it should.
 *
 * @param[in,out] start   Start; clipped.
 * @param[in,out] length  Length, where a negative one covers nothing; clipped, never negative.
 */
static void clip_rect_axis(int* start, int* length)
{
    int64_t near_edge = *start;
    int64_t far_edge = (int64_t)*start + *length; /* in 64 bits: the far edge can pass the int range */
    near_edge = near_edge < -ER_PAINT_RECT_MAX ? -ER_PAINT_RECT_MAX
                                               : (near_edge > ER_PAINT_RECT_MAX ? ER_PAINT_RECT_MAX : near_edge);
    far_edge = far_edge < -ER_PAINT_RECT_MAX ? -ER_PAINT_RECT_MAX
                                             : (far_edge > ER_PAINT_RECT_MAX ? ER_PAINT_RECT_MAX : far_edge);
    *start = (int)near_edge;
    *length = (int)(far_edge > near_edge ? far_edge - near_edge : 0);
}

/**
 * @brief Whether a clipped footprint spans the whole ±ER_PAINT_RECT_MAX window along either axis.
 *
 * Two such footprints clip to the same rect wherever the node sits, so comparing one with the record cannot tell
 * that the node moved. The pre-pass counts it as moved every commit instead: it covers the whole screen, so that
 * repaints what a move would.
 *
 * @param[in] x,y,width,height  Clipped footprint.
 *
 * @return true when the footprint reaches both ends of the window on some axis.
 */
static bool spans_paint_window(int x, int y, int width, int height)
{
    return (x <= -ER_PAINT_RECT_MAX && x + width >= ER_PAINT_RECT_MAX)
           || (y <= -ER_PAINT_RECT_MAX && y + height >= ER_PAINT_RECT_MAX);
}

/**
 * @brief Computes a node's current screen rect for damage tracking.
 *
 * Mirrors render_tree's position math: absolute layout position minus accumulated ancestor scroll,
 * plus the translate-transform offset. Returns false for non-translate transforms (rotate/scale/3D)
 * whose painted bounding box this fast path can't reproduce — the caller then repaints in full.
 *
 * The ActivityIndicator is the one node that answers here whatever its transform props say, because it
 * is the one node render_tree never captures: er_node_has_complex_transform() excludes it, so its
 * transform block always falls through to the translate fast path and it paints untransformed at
 * box+translate, which is exactly what this helper measures. Its tp_rotate_z is an internal spin angle that
 * render_activity_indicator() bakes into the ring of dots, not an affine render — so it is non-zero on
 * every commit of a spinning indicator, and without this exception a spinner that also carries a real
 * transform (a translate is enough) reads as a non-translate transform, is refused by both rect helpers,
 * and repaints the whole screen once per spin frame.
 *
 * @param[in]  node                                  Node to measure.
 * @param[out] rect_x,rect_y,rect_width,rect_height  Receive the node's screen rectangle.
 *
 * @return true if the rect was computed; false if the node needs a full repaint instead.
 */
static bool node_screen_rect(const ERNode* node, int* rect_x, int* rect_y, int* rect_width, int* rect_height)
{
#if ERUI_TRANSFORMS_FULL
    if (er_node_has_complex_transform(node))
        return false;
#endif
    int scroll_x, scroll_y;
    node_ancestor_scroll(node, &scroll_x, &scroll_y);
    *rect_x = (int)node->animated.x - scroll_x + (int)node->transform_translate_x;
    *rect_y = (int)node->animated.y - scroll_y + (int)node->transform_translate_y - s_keyboard_avoid_y;
    *rect_width = (int)node->animated.width;
    *rect_height = (int)node->animated.height;
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
 * `invertible` rides along because the matrix this already builds is the one render_tree hands to
 * er_transform_invert() / er_transform_homography_invert() before it starts a capture, and a matrix
 * too singular to invert has no inverse-map blit — so the node paints untransformed at its raw box
 * and the AABB measured here is not what reaches the screen (issue #138). Answering it here costs a
 * determinant on the matrix already in hand (the 3D path pays a 3x3 cofactor inverse for it, since the
 * epsilon rule lives inside er_transform_homography_invert and is not worth a second copy); asking
 * er_transform_is_invertible() instead would rebuild the matrix itself, trig and all. It is set on
 * every return, including the zero-area one, because the caller has to read it BEFORE it decides
 * whether a missing AABB matters.
 *
 * @param[in]  node                                  Node to measure.
 * @param[in]  scroll_x,scroll_y                     Accumulated ancestor scroll, from node_ancestor_scroll().
 * @param[out] rect_x,rect_y,rect_width,rect_height  Receive the node's transformed screen AABB.
 * @param[out] invertible                            Receives whether the matrix behind that AABB can be inverted, i.e.
 * whether the AABB is a footprint the paint can actually use.
 *
 * @return true if a finite AABB was produced; false to fall back to a full repaint.
 */
static bool node_transformed_screen_rect(const ERNode* node,
                                         int scroll_x,
                                         int scroll_y,
                                         int* rect_x,
                                         int* rect_y,
                                         int* rect_width,
                                         int* rect_height,
                                         bool* invertible)
{
    const int box_x = (int)node->animated.x - scroll_x;
    const int box_y = (int)node->animated.y - scroll_y - s_keyboard_avoid_y;
    const int width = (int)node->animated.width;
    const int height = (int)node->animated.height;
#if ERUI_3D_TRANSFORMS
    if (er_transform_is_3d(node))
    {
        /* 3D/perspective: project the node's box through the same homography render_tree paints with,
         * so the damage bounds exactly match the painted dst rect. A node whose corners all fall behind
         * the perspective plane yields a zero-area AABB → false → full-repaint fallback. */
        float homography[9], inverse_homography[9];
        er_transform_compute_homography_3d(node, box_x, box_y, width, height, homography);
        *invertible = er_transform_homography_invert(homography, inverse_homography);
        er_transform_aabb_3d(box_x, box_y, width, height, homography, rect_x, rect_y, rect_width, rect_height);
        return (*rect_width > 0 && *rect_height > 0);
    }
#endif
    float matrix_a, matrix_b, matrix_c, matrix_d, matrix_translate_x, matrix_translate_y;
    er_transform_compute_matrix(node,
                                box_x,
                                box_y,
                                width,
                                height,
                                &matrix_a,
                                &matrix_b,
                                &matrix_c,
                                &matrix_d,
                                &matrix_translate_x,
                                &matrix_translate_y);
    float inverse_a, inverse_b, inverse_c, inverse_d, inverse_translate_x, inverse_translate_y;
    *invertible = er_transform_invert(matrix_a,
                                      matrix_b,
                                      matrix_c,
                                      matrix_d,
                                      matrix_translate_x,
                                      matrix_translate_y,
                                      &inverse_a,
                                      &inverse_b,
                                      &inverse_c,
                                      &inverse_d,
                                      &inverse_translate_x,
                                      &inverse_translate_y);
    er_transform_aabb(box_x,
                      box_y,
                      width,
                      height,
                      matrix_a,
                      matrix_b,
                      matrix_c,
                      matrix_d,
                      matrix_translate_x,
                      matrix_translate_y,
                      rect_x,
                      rect_y,
                      rect_width,
                      rect_height);
    return (*rect_width > 0 && *rect_height > 0);
}

/**
 * @brief Computes the raw, untransformed screen box of a transformed node.
 *
 * The footprint render_tree degrades to when it cannot start the node's scratch capture: same origin as
 * node_transformed_screen_rect (layout position minus ancestor scroll and the keyboard shift), but the
 * node's own w×h rather than a projected AABB, and no translate folded in — the fallback paints the node
 * exactly where it would sit with no transform at all. An Arc's knob reach and a shadow's bleed are added
 * for the same reason render_tree adds them to last_paint_rect on that path: they are part of what the
 * fallback puts on screen. (A node that DOES capture casts no shadow — render_tree gates the shadow on
 * `!doing_affine` because it would be rasterised into the source and distorted by the blit — which is
 * exactly why the bleed belongs here, on the raw-box helper, rather than at the callers.)
 *
 * @param[in]  node                                  Node to measure.
 * @param[in]  scroll_x,scroll_y                     Accumulated ancestor scroll, from node_ancestor_scroll().
 * @param[out] rect_x,rect_y,rect_width,rect_height  Receive the node's untransformed screen box.
 */
static void node_untransformed_screen_rect(
    ERNode* node, int scroll_x, int scroll_y, int* rect_x, int* rect_y, int* rect_width, int* rect_height)
{
    *rect_x = (int)node->animated.x - scroll_x;
    *rect_y = (int)node->animated.y - scroll_y - s_keyboard_avoid_y;
    *rect_width = (int)node->animated.width;
    *rect_height = (int)node->animated.height;
    expand_for_arc(node, rect_x, rect_y, rect_width, rect_height);
#if ERUI_SHADOWS
    expand_for_shadow(node, rect_x, rect_y, rect_width, rect_height);
#endif
}
#endif /* ERUI_TRANSFORMS_FULL */

/** @brief What the damage pre-pass expects a full-transform node's next paint to cover. */
typedef struct
{
    int footprint_x, footprint_y, footprint_width,
        footprint_height; /**< The footprint itself: either the transformed AABB or the raw box, the
latter already grown by the arc/shadow reach the fallback paints. */
    int hedge_x, hedge_y, hedge_width,
        hedge_height;   /**< The OTHER candidate footprint — meaningful only when `hedge`. */
    bool hedge;         /**< Damage both candidates: which one the paint uses is not decided here. */
} NodeTransformDamage;

#if ERUI_TRANSFORMS_FULL
/**
 * @brief Clips a NodeTransformDamage's footprints as render_tree clips the paint it records.
 *
 * The AABB is clipped already, but a raw-box fallback is not: one past ±ER_PAINT_RECT_MAX would differ from its
 * clipped record on every commit, so an idle node would read as moved and repaint itself forever.
 *
 * @param[in,out] damage  Footprints to clip; the other candidate only when `hedge`.
 */
static void clip_transform_damage(NodeTransformDamage* damage)
{
    clip_rect_axis(&damage->footprint_x, &damage->footprint_width);
    clip_rect_axis(&damage->footprint_y, &damage->footprint_height);
    if (damage->hedge)
    {
        clip_rect_axis(&damage->hedge_x, &damage->hedge_width);
        clip_rect_axis(&damage->hedge_y, &damage->hedge_height);
    }
}
#endif /* ERUI_TRANSFORMS_FULL */

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
 * Three things stop a capture, and two of them are visible from here:
 *   - the node is larger than the transform source (or degenerate) — er_transform_source_fits() is the
 *     exact test er_transform_source_begin() admits on, so this is a prediction, not a guess;
 *   - its matrix does not invert — the blit that puts the capture back on screen is an inverse map, so
 *     render_tree abandons the capture at exactly the threshold er_transform_invert() (and, in 3D,
 *     er_transform_homography_invert()) refuses at. Also a prediction, not a guess: the matrix behind
 *     the AABB is the one that gets tested, so node_transformed_screen_rect() reports the verdict
 *     alongside the rect (issue #138);
 *   - a capture is already active for a transformed ancestor — not a property of this node's geometry.
 *     The flag the last paint left behind stands in for it. That is a guess: an ancestor's transform
 *     may have appeared or vanished since, so `hedge` asks the caller to damage the other candidate
 *     footprint alongside the predicted one.
 *
 * The first two are decisive and hedge nothing: whichever of them fires, this node paints its raw box
 * whether or not an ancestor would also have refused it the capture.
 *
 * The hedge is symmetric, and has to be (issue #139). The flag is equally stale in both directions: it
 * says "raw" for a node whose ancestor has since dropped its transform and is about to let this one
 * capture, and it says "captured" for a node whose ancestor has since gained one and is about to
 * refuse it. The second direction is asked of capture_candidate_above() rather than assumed, so an
 * ordinary transformed node with no transformed ancestor — the common case — keeps its tight AABB and
 * pays nothing: the capture cannot be taken from it, so the AABB is not a guess at all.
 *
 * The size test is four integer compares and it is asked FIRST, because when it fails the AABB is dead:
 * the footprint is the raw box and neither hedge can fire. Computing the matrix before asking spent two
 * sinf/cosf (six, plus up to eight projective divides, in 3D) per node per commit — including on fully
 * idle commits — to produce a rect that was then thrown away. The invert test cannot be hoisted the
 * same way — it IS the matrix — so it rides on the one the AABB already needed.
 *
 * @param[in]  node    Node to measure.
 * @param[out] damage  Receives the footprint (and, when `hedge`, the other candidate to damage with it).
 *
 * @return true if the paint could be bounded; false to leave the caller on its full-repaint fallback.
 */
static bool node_transform_damage(ERNode* node, NodeTransformDamage* damage)
{
#if ERUI_TRANSFORMS_FULL
    /* Not a transform this path owns. The ActivityIndicator is excluded first and deliberately: its
     * rotate_z is an internal spin, not an affine render, so it must never be measured by an AABB it
     * does not paint. It is node_screen_rect()'s node — it paints untransformed at box+translate — so
     * in practice the pre-pass has already bounded it and never reaches here; this keeps the helper's
     * contract true for any other caller. */
    if (!er_node_has_complex_transform(node))
        return false;

    /* One walk, shared by both helpers below. */
    int scroll_x, scroll_y;
    node_ancestor_scroll(node, &scroll_x, &scroll_y);

    damage->hedge = false;
    if (!er_transform_source_fits((int)node->animated.width, (int)node->animated.height))
    {
        /* Decisive: er_transform_source_begin() admits on size alone, so this node paints its raw box.
         * No matrix, no AABB — and no full-repaint fallback either, even for a degenerate transform,
         * because bounded damage matching the actual paint beats a conservative whole-screen repaint. */
        node_untransformed_screen_rect(node,
                                       scroll_x,
                                       scroll_y,
                                       &damage->footprint_x,
                                       &damage->footprint_y,
                                       &damage->footprint_width,
                                       &damage->footprint_height);
        clip_transform_damage(damage);
        return true;
    }

    int transformed_x, transformed_y, transformed_width, transformed_height;
    bool invertible = false;
    const bool bounded = node_transformed_screen_rect(
        node, scroll_x, scroll_y, &transformed_x, &transformed_y, &transformed_width, &transformed_height, &invertible);
    if (!invertible)
    {
        /* Decisive, exactly like the size test above: the capture reaches the screen through an
         * inverse-map blit, and a matrix render_tree cannot invert has no such map, so it falls
         * through to painting the raw box. Nothing an ancestor does can change that, so there is
         * nothing to hedge. Asked BEFORE the AABB is required to be finite, because a near-singular
         * matrix is precisely the one that collapses its own AABB to nothing: taking the full-repaint
         * fallback there would be conservative but wrong-headed, since the footprint IS known.
         *
         * Without this the pre-pass damaged the sub-pixel AABB the paint never used, the fallback
         * paint recorded the raw box, and from the next commit the (stale) prediction agreed with it —
         * `moved` stayed false and the torn node was never source-dirty again (issue #138). */
        node_untransformed_screen_rect(node,
                                       scroll_x,
                                       scroll_y,
                                       &damage->footprint_x,
                                       &damage->footprint_y,
                                       &damage->footprint_width,
                                       &damage->footprint_height);
        clip_transform_damage(damage);
        return true;
    }
    if (!bounded)
        return false;

    if (node->has_last_paint && node->last_paint_untransformed)
    {
        /* Carried over from the last paint, not predicted from size, so the capture may succeed this
         * commit and paint the AABB instead. Damage both rather than risk scissoring away its own paint. */
        damage->hedge = true;
        node_untransformed_screen_rect(node,
                                       scroll_x,
                                       scroll_y,
                                       &damage->footprint_x,
                                       &damage->footprint_y,
                                       &damage->footprint_width,
                                       &damage->footprint_height);
        damage->hedge_x = transformed_x;
        damage->hedge_y = transformed_y;
        damage->hedge_width = transformed_width;
        damage->hedge_height = transformed_height;
    }
    else
    {
        damage->footprint_x = transformed_x;
        damage->footprint_y = transformed_y;
        damage->footprint_width = transformed_width;
        damage->footprint_height = transformed_height;
        /* The mirror image: the flag says this node held the capture, and only an ancestor can have
         * taken it since. When one could have, the raw box is the other candidate — a node about to be
         * captured from above paints it in the ancestor's source space, and the AABB predicted here is
         * abandoned. Without a transformed ancestor there is nothing to take it, so no hedge. */
        damage->hedge = capture_candidate_above(node);
        if (damage->hedge)
            node_untransformed_screen_rect(node,
                                           scroll_x,
                                           scroll_y,
                                           &damage->hedge_x,
                                           &damage->hedge_y,
                                           &damage->hedge_width,
                                           &damage->hedge_height);
    }
    clip_transform_damage(damage);
    return true;
#else
    (void)node;
    (void)damage;
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
 * @param[in]  node                                  Node whose subtree bounds to measure.
 * @param[out] rect_x,rect_y,rect_width,rect_height  Receive the subtree's screen rectangle.
 *
 * @return true if the rect was computed; false for a transform node_screen_rect() cannot bound.
 */
static bool node_subtree_screen_rect(const ERNode* node, int* rect_x, int* rect_y, int* rect_width, int* rect_height)
{
    int box_x, box_y, box_width, box_height;
    if (!node_screen_rect(node, &box_x, &box_y, &box_width, &box_height))
        return false;
    *rect_x = (int)node->subtree_x + (box_x - (int)node->computed.x);
    *rect_y = (int)node->subtree_y + (box_y - (int)node->computed.y);
    *rect_width = (int)node->subtree_width;
    *rect_height = (int)node->subtree_height;
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
static void clip_rect_to_clippers(const ERNode* node, int* rect_x, int* rect_y, int* rect_width, int* rect_height)
{
    int left = *rect_x, top = *rect_y, right = *rect_x + *rect_width, bottom = *rect_y + *rect_height;
    const ERNode* ancestor = er_get_node(node->parent_tag);
    while (ancestor)
    {
        const bool clips = node_clips_children(ancestor);
        int ancestor_x, ancestor_y, ancestor_width, ancestor_height;
        if (clips && node_screen_rect(ancestor, &ancestor_x, &ancestor_y, &ancestor_width, &ancestor_height))
        {
            if (ancestor_x > left)
                left = ancestor_x;
            if (ancestor_y > top)
                top = ancestor_y;
            if (ancestor_x + ancestor_width < right)
                right = ancestor_x + ancestor_width;
            if (ancestor_y + ancestor_height < bottom)
                bottom = ancestor_y + ancestor_height;
        }
        ancestor = er_get_node(ancestor->parent_tag);
    }
    *rect_x = left;
    *rect_y = top;
    *rect_width = (right > left) ? (right - left) : 0;
    *rect_height = (bottom > top) ? (bottom - top) : 0;
}

/**
 * @brief Reports a rect as repainted, clamped to the root the way add_damage clamps its insert.
 *
 * For the contributions render_tree's own accumulator cannot make. That accumulator unions only
 * SOURCE-dirty nodes, so anything repainted for a reason other than its own content changing —
 * pixels a removed node vacated, a modal scrim, a node that merely MOVED — has to report itself
 * here, or a host that transfers only er_get_dirty_rect() leaves those pixels stale on the panel.
 */
static void report_repaint_clamped(
    int x, int y, int width, int height, int root_left, int root_top, int root_right, int root_bottom)
{
    const int clamped_left = (x > root_left) ? x : root_left;
    const int clamped_top = (y > root_top) ? y : root_top;
    const int clamped_right = (x + width < root_right) ? (x + width) : root_right;
    const int clamped_bottom = (y + height < root_bottom) ? (y + height) : root_bottom;
    union_dirty_rect(clamped_left, clamped_top, clamped_right - clamped_left, clamped_bottom - clamped_top);
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
 * @param[in]     node                                       Modal node, already known to be source-dirty or moved.
 * @param[in,out] damage                                     Damage set to add the root rect to.
 * @param[in]     root_left,root_top,root_right,root_bottom  Root bounds — both the rect damaged and the clamp.
 */
static void
modal_scrim_damage(ERNode* node, ERDamageSet* damage, int root_left, int root_top, int root_right, int root_bottom)
{
    add_damage(damage,
               root_left,
               root_top,
               root_right - root_left,
               root_bottom - root_top,
               root_left,
               root_top,
               root_right,
               root_bottom);
    union_dirty_rect(root_left, root_top, root_right - root_left, root_bottom - root_top);
    if (!node->modal_visible)
        node->modal_scrim_shown = 0U;
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
 * The DESCENDANT's trail is, though, in the one case where the ancestor's footprint cannot stand in
 * for it: a node whose own last paint went straight to the screen — it held the capture itself, and
 * an ancestor has taken it since (issue #139). The rect it blitted is abandoned this commit and lies
 * wherever its own matrix put it, which is not bounded by the ancestor's box. A source-space trail
 * (the ordinary case: last_paint_untransformed) is not a screen rect at all and is exactly what the
 * ancestor's footprint does cover, so it is left alone.
 *
 * @param[in]     node                                       Descendant whose damage is being escalated.
 * @param[in]     capturing                                  Capturing ancestor, from capturing_transform_ancestor().
 * @param[in,out] damage                                     Damage set to add to.
 * @param[in]     report                                     Report the region as repainted too (for a descendant that
 * is not source_dirty, so render_tree's accumulator will never report it).
 * @param[in]     root_left,root_top,root_right,root_bottom  Root bounds the insert is clamped to.
 *
 * @return true when the ancestor's footprint was damaged; false when it could not be bounded, in
 *         which case the caller falls back to measuring the descendant itself.
 */
static bool escalate_damage_to_capture(ERNode* node,
                                       ERNode* capturing,
                                       ERDamageSet* damage,
                                       bool report,
                                       int root_left,
                                       int root_top,
                                       int root_right,
                                       int root_bottom)
{
    NodeTransformDamage transform_damage = {0};
    if (!node_transform_damage(capturing, &transform_damage))
        return false;

    int new_x = transform_damage.footprint_x, new_y = transform_damage.footprint_y,
        new_width = transform_damage.footprint_width, new_height = transform_damage.footprint_height;
    clip_rect_to_clippers(capturing, &new_x, &new_y, &new_width, &new_height);
    add_damage(damage, new_x, new_y, new_width, new_height, root_left, root_top, root_right, root_bottom);
    if (report)
        report_repaint_clamped(new_x, new_y, new_width, new_height, root_left, root_top, root_right, root_bottom);

    if (transform_damage.hedge)
    {
        int hedge_x = transform_damage.hedge_x, hedge_y = transform_damage.hedge_y,
            hedge_width = transform_damage.hedge_width, hedge_height = transform_damage.hedge_height;
        clip_rect_to_clippers(capturing, &hedge_x, &hedge_y, &hedge_width, &hedge_height);
        add_damage(damage, hedge_x, hedge_y, hedge_width, hedge_height, root_left, root_top, root_right, root_bottom);
        if (report)
            report_repaint_clamped(
                hedge_x, hedge_y, hedge_width, hedge_height, root_left, root_top, root_right, root_bottom);
    }

    /* Always reported, like every other trail erase: what repaints those pixels is an ancestor's
     * background, so render_tree's accumulator would never contribute them for this node. */
    if (node->has_last_paint && !node->last_paint_untransformed)
    {
        int old_x = (int)node->last_paint_rect.x, old_y = (int)node->last_paint_rect.y,
            old_width = (int)node->last_paint_rect.width, old_height = (int)node->last_paint_rect.height;
        clip_rect_to_clippers(node, &old_x, &old_y, &old_width, &old_height);
        add_damage(damage, old_x, old_y, old_width, old_height, root_left, root_top, root_right, root_bottom);
        report_repaint_clamped(old_x, old_y, old_width, old_height, root_left, root_top, root_right, root_bottom);
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
 * box — so render_tree() never prunes such a subtree. subtree_complex_xf is set whenever the subtree
 * holds a transform that needs the capture path, clipped children included, so render_node_content()
 * never buries one under an occluder.
 *
 * Bounds are in computed (pre-scroll) space; render_tree() subtracts the running scroll
 * translation before testing them against the damage clip. Refreshed on layout commits and whenever
 * er_request_subtree_bounds_pass() asks; otherwise the previous layout's bounds remain valid because
 * positions do not move.
 *
 * @param[in,out] node  Node whose subtree bounds to (re)compute.
 */
static void compute_subtree_bounds(ERNode* node)
{
    if (!node)
        return;

    int left = node->computed.x;
    int top = node->computed.y;
    int right = left + node->computed.width;
    int bottom = top + node->computed.height;
    bool prunable = !node->has_transform;
#if ERUI_TRANSFORMS_FULL
    bool complex_transform = er_node_has_complex_transform(node);
#else
    bool complex_transform = false;
#endif
    /* A knob wider than the ring paints past the box: widen the prune bounds by its reach. */
    int width = right - left, height = bottom - top;
    expand_for_arc(node, &left, &top, &width, &height);
    right = left + width;
    bottom = top + height;

    const bool clips = node_clips_children(node);

    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        if (child->layout.display == ER_DISPLAY_NONE)
        {
            /* A hidden subtree paints nothing, so it must not widen this node's prune bounds — its
             * own box has collapsed to the origin and unioning that in would stretch the parent's
             * bounds back to (0,0) and defeat the pruning for everything around it. */
            child_tag = child->next_sibling_tag;
            continue;
        }
        compute_subtree_bounds(child);
        if (child->subtree_complex_transform)
            complex_transform = true;
        if (!clips)
        {
            /* Non-clipping parent: a child may paint past this box, so grow to cover its subtree. */
            if (child->subtree_x < left)
                left = child->subtree_x;
            if (child->subtree_y < top)
                top = child->subtree_y;
            if (child->subtree_x + child->subtree_width > right)
                right = child->subtree_x + child->subtree_width;
            if (child->subtree_y + child->subtree_height > bottom)
                bottom = child->subtree_y + child->subtree_height;
            if (!child->subtree_prunable)
                prunable = false;
        }
        child_tag = child->next_sibling_tag;
    }

    node->subtree_x = (int16_t)left;
    node->subtree_y = (int16_t)top;
    node->subtree_width = (int16_t)(right - left);
    node->subtree_height = (int16_t)(bottom - top);
    node->subtree_prunable = prunable;
    node->subtree_complex_transform = complex_transform;
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
 * (coarse but O(1) and always safe). Sized by ERUI_FADE_CACHE_WIDTH/HEIGHT; 0 disables the feature. */
#ifndef ERUI_FADE_CACHE_WIDTH
#define ERUI_FADE_CACHE_WIDTH 0
#endif
#ifndef ERUI_FADE_CACHE_HEIGHT
#define ERUI_FADE_CACHE_HEIGHT 0
#endif
#define ER_FADE_CACHE_ENABLED (ERUI_FADE_CACHE_WIDTH > 0 && ERUI_FADE_CACHE_HEIGHT > 0)
#if ER_FADE_CACHE_ENABLED
static uint32_t s_fade_cache[(size_t)ERUI_FADE_CACHE_HEIGHT * ERUI_FADE_CACHE_WIDTH];
static uint16_t s_fade_cache_tag = ER_INVALID_TAG; /* owning node */
static uint32_t s_fade_cache_generation = 0;       /* s_content_generation at capture */
static int s_fade_cache_width = 0;
static int s_fade_cache_height = 0;
static uint32_t s_fade_capture_commit = 0; /* anti-thrash: at most one capture per commit */
#endif
static uint32_t s_commit_sequence = 0;

/** @brief Drops the fade cache (owner destroyed, scene reset, or tag about to be recycled). */
static void fade_cache_invalidate(void)
{
#if ER_FADE_CACHE_ENABLED
    s_fade_cache_tag = ER_INVALID_TAG;
#endif
}

void er_mark_dirty_upward_opacity(ERNode* node)
{
#if ER_FADE_CACHE_ENABLED
    /* The cache holds its owner's subtree as painted, so a descendant's new alpha is new content to it. */
    for (ERNode* ancestor = node ? er_get_node(node->parent_tag) : NULL; ancestor;
         ancestor = er_get_node(ancestor->parent_tag))
    {
        if (ancestor->tag == s_fade_cache_tag)
        {
            s_content_generation++;
            break;
        }
    }
#endif
    er_mark_dirty_upward_visual(node);
}

#if ER_PROF
/* TEMP on-device profiling: phase accumulators printed every ER_PROF_INTERVAL commits (host provides
 * the clock). The interval is named because the line it prints states it — a literal in both the
 * condition and the format string is one edit away from a log that lies about its own window. */
#define ER_PROF_INTERVAL 30U
#include <stdio.h>
extern uint32_t er_prof_now_us(void);
static uint32_t s_prof_content_us = 0; /* subtree render time inside composites */
static uint32_t s_prof_blend_us = 0;   /* strip pop_blend time */
static uint32_t s_prof_push_us = 0;    /* strip push (clear) time */
static uint32_t s_prof_passes = 0;     /* band passes */
static uint32_t s_prof_composites = 0; /* composite_with_opacity calls that composited */
#define ER_PROF_MARK(name) const uint32_t name = er_prof_now_us()
#define ER_PROF_ACC(total, start) (total += er_prof_now_us() - (start))
#else
#define ER_PROF_MARK(name)
#define ER_PROF_ACC(total, start)
#endif

/* Frame instrumentation (er_perf.h): the commit reports its own layout/raster split and the region it
 * repainted, so a spike can be attributed without the host guessing. The entry points are no-op stubs
 * when ER_PERF_STATS == 0, but the macros drop even the calls so a disabled build is byte-identical. */
#if ER_PERF_STATS
#define ER_PERF_BEGIN(phase) er_perf_phase_begin(phase)
#define ER_PERF_END(phase) er_perf_phase_end(phase)
#define ER_PERF_REPAINT(x, y, width, height) er_perf_note_repaint((x), (y), (width), (height))
#define ER_PERF_RASTER_BEGIN(subphase) er_perf_raster_begin(subphase)
#define ER_PERF_RASTER_END(subphase) er_perf_raster_end(subphase)
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
#define ER_PERF_REPAINT(x, y, width, height) ((void)0)
#define ER_PERF_RASTER_BEGIN(subphase) ((void)0)
#define ER_PERF_RASTER_END(subphase) ((void)0)
#define ER_PERF_BLIT_RESET() ((void)0)
#define ER_PERF_BLIT_COLLECT() ((void)0)
#endif

static void render_tree(ERNode* node, bool parent_dirty, bool occluded, int translate_x, int translate_y);

/**
 * @brief Renders a node's own content and recurses into its children.
 *
 * This is the repeatable "body" of render_tree: the per-type draw switch, the overflow
 * scissor push, and the z-sorted child recursion. It has no side effects on node's dirty
 * flags (except one-shot consumption of vec_has_dirty by the vector rasterizer), so the
 * banded opacity compositor can invoke it once per strip tile.
 *
 * @param[in] node         Node to render.
 * @param[in] needs_paint  true when the node itself (not only descendants) must repaint.
 * @param[in] occluded     true when an opaque node painted later already covers this whole
 *                         region, so nothing here can be seen. The subtree is still walked (its
 *                         paint bookkeeping has to stay in step) but emits no pixels.
 * @param[in] box_x        Screen-space left edge (scroll translation applied).
 * @param[in] box_y        Screen-space top edge.
 * @param[in] width        Node width in pixels.
 * @param[in] height       Node height in pixels.
 * @param[in] translate_x  Accumulated horizontal scroll offset for children.
 * @param[in] translate_y  Accumulated vertical scroll offset for children.
 */
static void render_node_content(ERNode* node,
                                bool needs_paint,
                                bool occluded,
                                int box_x,
                                int box_y,
                                int width,
                                int height,
                                int translate_x,
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
static bool composite_from_cache(
    ERNode* node, uint8_t alpha, int box_x, int box_y, int width, int height, int translate_x, int translate_y)
{
#if ER_FADE_CACHE_ENABLED
    if (cc()->tile_active || !er_scratch_idle() || er_get_draw_alpha() != ER_ALPHA_OPAQUE)
        return false;
    if (width <= 0 || height <= 0 || width > ERUI_FADE_CACHE_WIDTH || height > ERUI_FADE_CACHE_HEIGHT)
        return false;

    /* The cache buffer is the node BOX. A subtree that paints past it does not fit, and capturing it
     * anyway would clip those descendants away — the defect composite_with_opacity's region union
     * fixes. Such a group takes the strip pool instead. */
    if ((int)node->subtree_x - translate_x < box_x || (int)node->subtree_y - translate_y < box_y
        || (int)node->subtree_x - translate_x + (int)node->subtree_width > box_x + width
        || (int)node->subtree_y - translate_y + (int)node->subtree_height > box_y + height)
        return false;

    /* Cache HIT: a read-only blend, clipped by the active scissor — safe under any scissor
     * (a partial repaint blends just the damaged part) and from any render worker (slices blend
     * disjoint rows of the same cached content). Checked before the full-coverage gate below,
     * which only capture needs. */
    if (s_fade_cache_tag == node->tag && s_fade_cache_generation == s_content_generation && s_fade_cache_width == width
        && s_fade_cache_height == height)
    {
        er_blit_blend(s_fade_cache, ERUI_FADE_CACHE_WIDTH * (int)sizeof(uint32_t), alpha, box_x, box_y, width, height);
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
            s_fade_capture_wanted_generation = s_content_generation;
        }
        return false;
    }
    int clip_x, clip_y, clip_width, clip_height;
    if (er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height)
        && (clip_x > box_x || clip_y > box_y || clip_x + clip_width < box_x + width
            || clip_y + clip_height < box_y + height))
        return false;

    if (s_fade_capture_commit == s_commit_sequence)
        return false;
    s_fade_capture_commit = s_commit_sequence;

    for (int row = 0; row < height; row++)
        memset(s_fade_cache + (size_t)row * ERUI_FADE_CACHE_WIDTH, 0, (size_t)width * sizeof(uint32_t));
    er_scratch_push_base(s_fade_cache, ERUI_FADE_CACHE_WIDTH, ERUI_FADE_CACHE_HEIGHT, box_x, box_y);
    render_node_content(node, true, false, box_x, box_y, width, height, translate_x, translate_y);
    er_scratch_pop_base();

    s_fade_cache_tag = node->tag;
    s_fade_cache_generation = s_content_generation;
    s_fade_cache_width = width;
    s_fade_cache_height = height;
    er_blit_blend(s_fade_cache, ERUI_FADE_CACHE_WIDTH * (int)sizeof(uint32_t), alpha, box_x, box_y, width, height);
    return true;
#else
    (void)node;
    (void)alpha;
    (void)box_x;
    (void)box_y;
    (void)width;
    (void)height;
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
 * @param[in] node         Node whose subtree is composited.
 * @param[in] alpha        Group opacity 0–255.
 * @param[in] box_x        Screen-space left edge of the node.
 * @param[in] box_y        Screen-space top edge.
 * @param[in] width        Node width in pixels.
 * @param[in] height       Node height in pixels.
 * @param[in] translate_x  Accumulated scroll offset for children.
 * @param[in] translate_y  Accumulated scroll offset for children.
 *
 * @return true when the subtree was composited; false when no slot was available or the
 *         region is empty (caller falls back to a direct render).
 */
static bool composite_with_opacity(
    ERNode* node, uint8_t alpha, int box_x, int box_y, int width, int height, int translate_x, int translate_y)
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
    int region_left = box_x, region_top = box_y, region_right = box_x + width, region_bottom = box_y + height;
    {
        const int subtree_left = (int)node->subtree_x - translate_x;
        const int subtree_top = (int)node->subtree_y - translate_y;
        const int subtree_right = subtree_left + (int)node->subtree_width;
        const int subtree_bottom = subtree_top + (int)node->subtree_height;
        if (subtree_left < region_left)
            region_left = subtree_left;
        if (subtree_top < region_top)
            region_top = subtree_top;
        if (subtree_right > region_right)
            region_right = subtree_right;
        if (subtree_bottom > region_bottom)
            region_bottom = subtree_bottom;
    }
    int clip_x, clip_y, clip_width, clip_height;
    if (er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height))
    {
        if (clip_x > region_left)
            region_left = clip_x;
        if (clip_y > region_top)
            region_top = clip_y;
        if (clip_x + clip_width < region_right)
            region_right = clip_x + clip_width;
        if (clip_y + clip_height < region_bottom)
            region_bottom = clip_y + clip_height;
    }
    if (cc()->tile_active)
    {
        if (cc()->tile_x > region_left)
            region_left = cc()->tile_x;
        if (cc()->tile_y > region_top)
            region_top = cc()->tile_y;
        if (cc()->tile_x + cc()->tile_width < region_right)
            region_right = cc()->tile_x + cc()->tile_width;
        if (cc()->tile_y + cc()->tile_height < region_bottom)
            region_bottom = cc()->tile_y + cc()->tile_height;
    }
    const int region_width = region_right - region_left;
    const int region_height = region_bottom - region_top;
    if (region_width <= 0 || region_height <= 0)
        return false;

    const int strip_width = er_scratch_strip_w();
    const int strip_height = er_scratch_strip_h();

    if (region_width <= strip_width && region_height <= strip_height)
    {
        ER_PROF_MARK(push_start);
        if (!er_scratch_push(region_left, region_top, region_width, region_height))
            return false;
        ER_PROF_ACC(s_prof_push_us, push_start);
        /* Scissor to the composite region so rasterisers do region-sized work, not node-sized. */
        er_push_clip_rect(region_left, region_top, region_width, region_height);
        ER_PROF_MARK(content_start);
        render_node_content(node, true, false, box_x, box_y, width, height, translate_x, translate_y);
        ER_PROF_ACC(s_prof_content_us, content_start);
        er_pop_clip_rect();
        ER_PROF_MARK(blend_start);
        er_scratch_pop_blend(alpha, region_left, region_top, region_width, region_height);
        ER_PROF_ACC(s_prof_blend_us, blend_start);
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
    const int outer_x = cc()->tile_x, outer_y = cc()->tile_y, outer_width = cc()->tile_width,
              outer_height = cc()->tile_height;
    const uint32_t outer_generation = cc()->current_loop_generation;
    const bool outer_first = cc()->tile_first;
    cc()->current_loop_generation = ++cc()->band_generation_counter;
    bool first = true;
    for (int band_y = region_top; band_y < region_bottom; band_y += strip_height)
    {
        const int band_height = (region_bottom - band_y) < strip_height ? (region_bottom - band_y) : strip_height;
        for (int band_x = region_left; band_x < region_right; band_x += strip_width)
        {
            const int band_width = (region_right - band_x) < strip_width ? (region_right - band_x) : strip_width;
            ER_PROF_MARK(push_start);
            if (!er_scratch_push(band_x, band_y, band_width, band_height))
            {
                /* Defensive fallback: avoid leaving holes if the scratch pool is unexpectedly exhausted. */
                const uint8_t saved_alpha = er_get_draw_alpha();
                er_set_draw_alpha((uint8_t)((uint32_t)saved_alpha * alpha / ER_ALPHA_OPAQUE));

                cc()->tile_active = true;
                cc()->tile_x = band_x;
                cc()->tile_y = band_y;
                cc()->tile_width = band_width;
                cc()->tile_height = band_height;
                cc()->tile_first = first;
                er_push_clip_rect(band_x, band_y, band_width, band_height);
                render_node_content(node, true, false, box_x, box_y, width, height, translate_x, translate_y);
                er_pop_clip_rect();

                cc()->tile_active = outer_active;
                cc()->tile_x = outer_x;
                cc()->tile_y = outer_y;
                cc()->tile_width = outer_width;
                cc()->tile_height = outer_height;
                er_set_draw_alpha(saved_alpha);

                first = false;
                continue;
            }
            ER_PROF_ACC(s_prof_push_us, push_start);
            cc()->tile_active = true;
            cc()->tile_x = band_x;
            cc()->tile_y = band_y;
            cc()->tile_width = band_width;
            cc()->tile_height = band_height;
            cc()->tile_first = first;
            er_push_clip_rect(band_x, band_y, band_width, band_height);
            ER_PROF_MARK(content_start);
            render_node_content(node, true, false, box_x, box_y, width, height, translate_x, translate_y);
            ER_PROF_ACC(s_prof_content_us, content_start);
            er_pop_clip_rect();
            cc()->tile_active = outer_active;
            cc()->tile_x = outer_x;
            cc()->tile_y = outer_y;
            cc()->tile_width = outer_width;
            cc()->tile_height = outer_height;
            ER_PROF_MARK(blend_start);
            er_scratch_pop_blend(alpha, band_x, band_y, band_width, band_height);
            ER_PROF_ACC(s_prof_blend_us, blend_start);
#if ER_PROF
            s_prof_passes++;
#endif
            first = false;
        }
    }
    cc()->current_loop_generation = outer_generation;
    cc()->tile_first = outer_first;
#if ER_PROF
    s_prof_composites++;
#endif
    return true;
}

/** @brief How far one edge's opaque fill is set in: the corner radius, or a partly opaque border band. */
static int edge_opaque_inset(int border_width, uint32_t border_color, int radius)
{
    const uint32_t alpha = border_color >> 24;
    const int band = (border_width > 0 && alpha != 0U && alpha != 0xFFU) ? border_width : 0;
    return band > radius ? band : radius;
}

/* A Modal's backdrop when it sets none (backdrop_color 0, er_scene.h): a translucent dim. */
#define ER_MODAL_DEFAULT_BACKDROP 0x99000000U

/**
 * @brief Whether a shown Modal's backdrop paints every pixel of a screen rect at full alpha.
 *
 * The backdrop fills the whole root before anything of the Modal's own, so when it is opaque everything
 * painted before the Modal is overwritten: a settings sheet over a busy screen then repaints only itself,
 * not the screen it hides. A translucent backdrop hides nothing.
 *
 * @param[in] modal                                 Modal node.
 * @param[in] rect_x,rect_y,rect_width,rect_height  Screen rect to test coverage of.
 *
 * @return true when the rect lies inside the root and the backdrop is opaque.
 */
static bool modal_backdrop_covers(const ERNode* modal, int rect_x, int rect_y, int rect_width, int rect_height)
{
    const uint32_t backdrop = modal->modal_backdrop_color ? modal->modal_backdrop_color : ER_MODAL_DEFAULT_BACKDROP;
    if (!modal->modal_visible || modal->props.view.opacity != ER_ALPHA_OPAQUE || (backdrop >> 24) != 0xFFU)
        return false;
    const ERNode* root = er_get_root_node();
    if (!root)
        return false;
    const int root_x = (int)root->computed.x;
    const int root_y = (int)root->computed.y;
    return rect_x >= root_x && rect_y >= root_y && rect_x + rect_width <= root_x + (int)root->computed.width
           && rect_y + rect_height <= root_y + (int)root->computed.height;
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
 * @param[in] node         Candidate occluder.
 * @param[in] translate_x  Scroll translation in effect for node (its parent's child translation).
 * @param[in] translate_y  Vertical counterpart.
 * @param[in] rect_x       Screen rect to test coverage of.
 * @param[in] rect_y       Screen rect top.
 * @param[in] rect_width   Screen rect width.
 * @param[in] rect_height  Screen rect height.
 *
 * @return true when the rect lies entirely inside node's opaque fill.
 */
static bool node_covers_opaque(
    const ERNode* node, int translate_x, int translate_y, int rect_x, int rect_y, int rect_width, int rect_height)
{
    if (!node || rect_width <= 0 || rect_height <= 0)
        return false;
    if (node->layout.display == ER_DISPLAY_NONE || node->subtree_hidden || node->has_transform)
        return false;

    switch (node->type)
    {
        case ER_NODE_VIEW:
        case ER_NODE_SCROLL_VIEW:
        case ER_NODE_PRESSABLE:
        case ER_NODE_FLAT_LIST:
            break;
        case ER_NODE_MODAL:
            return modal_backdrop_covers(node, rect_x, rect_y, rect_width, rect_height); /* its backdrop, not its box */
        default:
            return false; /* Text/Image/Vector/Arc/Switch leave gaps. */
    }

    const ERViewProps* view_props = &node->props.view;
    if (view_props->opacity != ER_ALPHA_OPAQUE)
        return false;
    if ((view_props->background_color >> 24) != 0xFFU)
        return false;
#if ERUI_GRADIENT
    if (view_props->gradient_type != ER_GRADIENT_NONE && view_props->gradient_stop_count >= 2U)
        return false;
#endif

    /* Largest corner radius, starting from 0 so a negative value can never widen the tested area —
     * the rasteriser clamps a negative radius to a square corner, and reading it literally here would
     * push x1/y1 past the node's actual box. */
    int radius = 0;
    if ((int)view_props->border_radius > radius)
        radius = (int)view_props->border_radius;
    if ((int)view_props->border_top_left_radius > radius)
        radius = (int)view_props->border_top_left_radius;
    if ((int)view_props->border_top_right_radius > radius)
        radius = (int)view_props->border_top_right_radius;
    if ((int)view_props->border_bottom_right_radius > radius)
        radius = (int)view_props->border_bottom_right_radius;
    if ((int)view_props->border_bottom_left_radius > radius)
        radius = (int)view_props->border_bottom_left_radius;

    /* Per-edge border inset (see the note above): only a PARTLY opaque band hides nothing. */
    const int border_width_left =
        view_props->border_left_width > 0 ? (int)view_props->border_left_width : (int)view_props->border_width;
    const int border_width_top =
        view_props->border_top_width > 0 ? (int)view_props->border_top_width : (int)view_props->border_width;
    const int border_width_right =
        view_props->border_right_width > 0 ? (int)view_props->border_right_width : (int)view_props->border_width;
    const int border_width_bottom =
        view_props->border_bottom_width > 0 ? (int)view_props->border_bottom_width : (int)view_props->border_width;
    const uint32_t border_color_left =
        view_props->border_left_color ? view_props->border_left_color : view_props->border_color;
    const uint32_t border_color_top =
        view_props->border_top_color ? view_props->border_top_color : view_props->border_color;
    const uint32_t border_color_right =
        view_props->border_right_color ? view_props->border_right_color : view_props->border_color;
    const uint32_t border_color_bottom =
        view_props->border_bottom_color ? view_props->border_bottom_color : view_props->border_color;

    const int inset_left = edge_opaque_inset(border_width_left, border_color_left, radius);
    const int inset_top = edge_opaque_inset(border_width_top, border_color_top, radius);
    const int inset_right = edge_opaque_inset(border_width_right, border_color_right, radius);
    const int inset_bottom = edge_opaque_inset(border_width_bottom, border_color_bottom, radius);

    const int left = node->animated.x - translate_x + inset_left;
    const int top = node->animated.y - translate_y + inset_top;
    const int right = node->animated.x - translate_x + node->animated.width - inset_right;
    const int bottom = node->animated.y - translate_y + node->animated.height - inset_bottom;

    return rect_x >= left && rect_y >= top && (rect_x + rect_width) <= right && (rect_y + rect_height) <= bottom;
}

/**
 * @brief Draws a TextInput's text as one dot per character (secureTextEntry), on the line the text uses.
 *
 * Only whole dots inside the content box are drawn.
 *
 * @param[in] text    The input's text (UTF-8).
 * @param[in] params  Its text params: the content box as the clip, the color and the font.
 *
 * @return Width of the dots, which is where the cursor goes.
 */
static int paint_masked_text(const char* text, const ERTextRenderParams* params)
{
    const uint8_t size = er_text_clamp_font_size(params->font_size);
    int line_height = 0;
    er_text_measure("", size, params->font_family, 0, 0, NULL, &line_height);
    const int dot_size = (size * 3 + 4) / 8; /* 6 px at 16 px */
    const int dot_gap = (size / 4 > 2) ? size / 4 : 2;
    const int y = params->clip.y + (line_height - dot_size) / 2;
    const bool fits = y >= params->clip.y && y + dot_size <= params->clip.y + params->clip.height;
    const int right = params->clip.x + params->clip.width;
    int x = params->clip.x + dot_gap / 2;
    int count = 0;
    for (const char* cursor = text; *cursor; cursor++)
    {
        if (((unsigned char)*cursor & 0xC0U) == 0x80U)
            continue; /* a continuation byte: the same character */
        if (fits && x + dot_size <= right)
            er_rrect_fill_bordered(params->color, 0x00000000U, 0, x, y, dot_size, dot_size, dot_size / 2);
        x += dot_size + dot_gap;
        count++;
    }
    return count * (dot_size + dot_gap);
}

/**
 * @brief Recursively renders a node and its children depth-first.
 *
 * translate_x / translate_y accumulate the total scroll offset contributed by all
 * ScrollView ancestors, less ancestor translates.  Each node's actual screen position is computed by subtracting
 * the accumulated translation from its layout-computed position.
 *
 * overflow:hidden and overflow:scroll nodes push a scissor clip rectangle around their
 * children so that out-of-bounds content is not visible.  ScrollView nodes also add
 * their current scroll offset to the running translation for their children.
 *
 * @param[in] node          Node to render.
 * @param[in] parent_dirty  true when an ancestor was dirty this frame.
 * @param[in] occluded      true when an opaque node painted later already covers this whole region.
 *                          The subtree is still walked — its paint bookkeeping (painted_seq,
 *                          last_paint_rect) has to stay in step or a hidden node damages the same
 *                          pixels forever — but it emits nothing.
 * @param[in] translate_x   Accumulated horizontal scroll offset from ancestor ScrollViews, less ancestor translates.
 * @param[in] translate_y   Accumulated vertical scroll offset from ancestor ScrollViews, less ancestor translates.
 */
static void render_tree(ERNode* node, bool parent_dirty, bool occluded, int translate_x, int translate_y)
{
    if (node->layout.display == ER_DISPLAY_NONE)
        return;

    /* Subtree-bounds pruning: if this whole subtree's cached paint bounds fall entirely outside the
     * active damage clip it cannot contribute to the changed region, so skip it (and every descendant)
     * without touching their structs — the win that turns the per-commit walk from O(all nodes) into
     * O(nodes near the change). Only applies when bounds are trustworthy (s_prune_ok), the subtree has
     * no transform (subtree_prunable), and a clip is active (a full repaint pushes none, so this is a
     * no-op then). Bounds are computed-space; subtract the scroll translation to compare in screen space. */
    if (s_prune_ok && node->subtree_prunable)
    {
        const int bounds_left = (int)node->subtree_x - translate_x;
        const int bounds_top = (int)node->subtree_y - translate_y;
        const int bounds_right = bounds_left + (int)node->subtree_width;
        const int bounds_bottom = bounds_top + (int)node->subtree_height;
        int clip_x, clip_y, clip_width, clip_height;
        if (er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height))
        {
            if (bounds_right <= clip_x || bounds_bottom <= clip_y || bounds_left >= clip_x + clip_width
                || bounds_top >= clip_y + clip_height)
                return;
        }
        /* Banded render: skip subtrees entirely outside the strip currently being emitted. Transformed
         * subtrees are never prunable, so they still render in full each strip — keeping their offscreen
         * scratch source complete across strip seams. */
        int band_y, band_height;
        if (er_band_active(&band_y, &band_height) && (bounds_bottom <= band_y || bounds_top >= band_y + band_height))
            return;
        /* Banded opacity compositing: skip subtrees entirely outside the strip tile currently being
         * composited (same never-prune-transformed rule as above). */
        if (cc()->tile_active
            && (bounds_right <= cc()->tile_x || bounds_bottom <= cc()->tile_y
                || bounds_left >= cc()->tile_x + cc()->tile_width || bounds_top >= cc()->tile_y + cc()->tile_height))
            return;
    }

    /* needs_paint is the classic painter's-algorithm answer: this node's pixels are part of the
     * region being recomposited. should_render additionally asks whether they would be VISIBLE —
     * an occluded node is walked for its bookkeeping but draws nothing (see the cull in
     * render_node_content). needs_paint, not should_render, is what children inherit and what
     * refreshes last_paint_rect, so an occluded subtree stays exactly as damage-tracked as a
     * painted one. */
    const bool needs_paint = node->dirty || parent_dirty;
    const bool should_render = needs_paint && !occluded;

    /* Actual screen position after applying all ancestor scroll offsets.
     * Use node->animated rather than node->computed so that LayoutAnimation
     * transitions show intermediate positions during the animation. */
    int box_x = node->animated.x - translate_x;
    int box_y = node->animated.y - translate_y;
    const int width = node->animated.width;
    const int height = node->animated.height;

    /* --- 2D/3D transform application --- */
    bool doing_affine = false;
    bool doing_replay = false;
    /* An occluded node whose capture would have been admitted: it captures nothing, but records and
     * scopes its subtree exactly as doing_affine does (see the transform block below). */
    bool doing_bookkeeping_capture = false;
    int destination_x = 0, destination_y = 0, destination_width = 0, destination_height = 0;
    float inverse_a = 1.0f, inverse_b = 0.0f, inverse_c = 0.0f, inverse_d = 1.0f, inverse_translate_x = 0.0f,
          inverse_translate_y = 0.0f;
#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
    bool doing_3d = false;
    float inverse_homography[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
#endif

    /* Only the CAPTURE is skipped for the nodes er_node_has_complex_transform() excludes: a translate
     * still moves them, the way node_screen_rect() and hit-testing both already place them. Gating the
     * whole block on the type dropped the offset instead, so a translated spinner painted at its raw box
     * while the pre-pass measured it at the offset one — the two never agreed, `moved` latched, and it
     * re-damaged itself on every commit for as long as it existed. */
#if ERUI_TRANSFORMS_FULL
    const bool complex_transform = er_node_has_complex_transform(node);
#endif
    /* A plain translate moves the children with the node. */
    int child_translate_x = translate_x;
    int child_translate_y = translate_y;
    if (er_node_has_offset_translate(node))
    {
        box_x += (int)node->transform_translate_x;
        box_y += (int)node->transform_translate_y;
        child_translate_x -= (int)node->transform_translate_x;
        child_translate_y -= (int)node->transform_translate_y;
    }
#if ERUI_TRANSFORMS_FULL
    /* An occluded node never captures a transform source: the scratch render would be thrown away. It
     * still works out the footprint, because last_paint_rect must say where its pixels ARE, which is
     * where its last real paint put them: the transformed AABB whenever the capture would have been
     * admitted. The cull never buries a subtree holding one (subtree_complex_xf), so an occluded node
     * gets here only on the bookkeeping-only walk of a commit whose every change clamped off-screen —
     * and since the dirty chain reaches the root, that walk refreshes the record of every node on screen.
     * A raw box recorded there would have the next move erase it and leave the transformed corners behind.
     *
     * Admission is predicted the way er_transform_source_begin() decides it: the source fits and no
     * capture is running. xf_capturing stands in for the latter, real or predicted, so a nested node
     * records the raw box its real paint would fall back to. */
    else if (complex_transform)
    {
#if ERUI_3D_TRANSFORMS
        if (er_transform_is_3d(node))
        {
            /* 3D perspective path: compute homography, render into scratch, back-project blit. */
            float homography[9];
            er_transform_compute_homography_3d(node, box_x, box_y, width, height, homography);
            er_transform_aabb_3d(box_x,
                                 box_y,
                                 width,
                                 height,
                                 homography,
                                 &destination_x,
                                 &destination_y,
                                 &destination_width,
                                 &destination_height);

            if (er_transform_homography_invert(homography, inverse_homography))
            {
                if (occluded)
                    doing_bookkeeping_capture = !cc()->transform_capturing && er_transform_source_fits(width, height);
                else if (cc()->tile_active && !cc()->tile_first
                         && er_transform_source_is_cached(node->tag, cc()->current_loop_generation))
                {
                    doing_replay = true;
                    doing_3d = true;
                }
                else if (er_transform_source_begin(box_x, box_y, width, height))
                {
                    doing_affine = true;
                    doing_3d = true;
                    er_transform_source_note(node->tag, cc()->tile_active ? cc()->current_loop_generation : 0U);
                }
            }
        }
        else
#endif
        {
            /* Full affine: render into scratch, then inverse-map blit. */
            float matrix_a, matrix_b, matrix_c, matrix_d, matrix_translate_x, matrix_translate_y;
            er_transform_compute_matrix(node,
                                        box_x,
                                        box_y,
                                        width,
                                        height,
                                        &matrix_a,
                                        &matrix_b,
                                        &matrix_c,
                                        &matrix_d,
                                        &matrix_translate_x,
                                        &matrix_translate_y);
            er_transform_aabb(box_x,
                              box_y,
                              width,
                              height,
                              matrix_a,
                              matrix_b,
                              matrix_c,
                              matrix_d,
                              matrix_translate_x,
                              matrix_translate_y,
                              &destination_x,
                              &destination_y,
                              &destination_width,
                              &destination_height);

            if (er_transform_invert(matrix_a,
                                    matrix_b,
                                    matrix_c,
                                    matrix_d,
                                    matrix_translate_x,
                                    matrix_translate_y,
                                    &inverse_a,
                                    &inverse_b,
                                    &inverse_c,
                                    &inverse_d,
                                    &inverse_translate_x,
                                    &inverse_translate_y))
            {
                if (occluded)
                    doing_bookkeeping_capture = !cc()->transform_capturing && er_transform_source_fits(width, height);
                else if (cc()->tile_active && !cc()->tile_first
                         && er_transform_source_is_cached(node->tag, cc()->current_loop_generation))
                {
                    doing_replay = true;
                }
                else if (er_transform_source_begin(box_x, box_y, width, height))
                {
                    doing_affine = true;
                    er_transform_source_note(node->tag, cc()->tile_active ? cc()->current_loop_generation : 0U);
                }
            }
            /* On failure (too large or singular matrix) fall through to normal render at
             * the untransformed position as a graceful degradation. */
        }
    }
#endif

    /* Banded-composite replay: later tiles of the same band pass reuse the transform source
     * captured in the first tile — emit only, bounded to the tile scissor. The subtree
     * re-render, damage recording, and dirty bookkeeping all happened in the first pass. */
    if (doing_replay)
    {
#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
        if (doing_3d)
        {
            er_transform_source_replay_blit_3d(box_x,
                                               box_y,
                                               width,
                                               height,
                                               inverse_homography,
                                               destination_x,
                                               destination_y,
                                               destination_width,
                                               destination_height);
            return;
        }
#endif
#if ERUI_TRANSFORMS_FULL
        er_transform_source_replay_blit(box_x,
                                        box_y,
                                        width,
                                        height,
                                        inverse_a,
                                        inverse_b,
                                        inverse_c,
                                        inverse_d,
                                        inverse_translate_x,
                                        inverse_translate_y,
                                        destination_x,
                                        destination_y,
                                        destination_width,
                                        destination_height);
#endif
        return;
    }

    /* A transform capture renders in SOURCE space: suspend the composite tile for its
     * duration (the tile bounds the transformed OUTPUT via the clip stack at emit time), so
     * pruning and nested composite regions inside the capture aren't bounded by a
     * post-transform rectangle. er_transform_source_begin pushed the matching clip reset. */
    const bool saved_tile_active = cc()->tile_active;
    /* Whether an OUTER capture was already running when this node was entered — read below to report
     * this node's change at the position its pixels actually reach the screen. Sampled before the
     * assignment, so a node that starts its own capture still reports its own transformed AABB. */
    const bool outer_capturing = cc()->transform_capturing;
    const ERRect outer_destination = cc()->transform_capture_destination;
    /* The node's pixels are (or, occluded, were last painted as) its capture's inverse-map blit. */
    const bool transformed_footprint = doing_affine || doing_bookkeeping_capture;
    if (doing_bookkeeping_capture)
        er_push_clip_reset(); /* what er_transform_source_begin() pushes: the subtree is walked in source space */
    if (transformed_footprint)
    {
        cc()->tile_active = false;
        cc()->transform_capturing = true;
        cc()->transform_capture_destination.x = destination_x;
        cc()->transform_capture_destination.y = destination_y;
        cc()->transform_capture_destination.width = destination_width;
        cc()->transform_capture_destination.height = destination_height;
    }

    /* Record where this node is painted so the next commit can damage-clip a move: the old rect
     * (stored here) unioned with the new rect erases the node's trail without a full-screen repaint.
     * Keyed off needs_paint rather than should_render so an OCCLUDED node still retires its move:
     * leaving last_paint_rect stale would make the damage pre-pass read it as moved on every
     * subsequent commit, re-damaging the same pixels for as long as it stays hidden. */
    if (needs_paint)
    {
        int paint_x = transformed_footprint ? destination_x : box_x;
        int paint_y = transformed_footprint ? destination_y : box_y;
        int paint_width = transformed_footprint ? destination_width : width;
        int paint_height = transformed_footprint ? destination_height : height;
        /* RECORD WHAT WAS PAINTED, not the bare box: the knob's reach past the box and the shadow's
         * bleed around it are on the panel too, so every reader of this trail — the move erase below,
         * note_removed_subtree(), propagate_hidden(), er_node_destroy() — must erase them with it.
         * Re-deriving the bleed at those read sites instead was the bug in issue #140: they read the
         * node's CURRENT props, so the very commit that clears shadow_opacity reconstructed a bleed of
         * zero and left the old ring on screen, and the three retire paths never expanded at all.
         *
         * Neither is added on the affine path, which bounds itself by the transformed layout box: a
         * captured node's knob is clipped by the scratch and it casts no shadow at all (see the
         * shadow block below, gated on the same `!doing_affine`). */
        if (!transformed_footprint)
        {
            expand_for_arc(node, &paint_x, &paint_y, &paint_width, &paint_height);
#if ERUI_SHADOWS
            expand_for_shadow(node, &paint_x, &paint_y, &paint_width, &paint_height);
#endif
        }
        clip_rect_axis(&paint_x, &paint_width);
        clip_rect_axis(&paint_y, &paint_height);
        node->last_paint_rect.x = (int16_t)paint_x;
        node->last_paint_rect.y = (int16_t)paint_y;
        node->last_paint_rect.width = (int16_t)paint_width;
        node->last_paint_rect.height = (int16_t)paint_height;
        node->last_paint_untransformed = !transformed_footprint;
        node->has_last_paint = true;
    }

    /* Shadow: rendered before opacity scratch so the shadow lands in the outer destination
     * (framebuffer or an ancestor opacity slot) rather than inside this node's own composite.
     * Affine-transformed nodes are skipped for v1 — the shadow would otherwise be rasterised
     * into the transform scratch and distorted by the inverse-map blit. */
    if (should_render && !doing_affine)
    {
        switch (node->type)
        {
            case ER_NODE_VIEW:
            case ER_NODE_SCROLL_VIEW:
            case ER_NODE_PRESSABLE:
                er_shadow_render(&node->props.view, box_x, box_y, width, height);
                break;
            case ER_NODE_MODAL:
                if (node->modal_visible)
                    er_shadow_render(&node->props.view, box_x, box_y, width, height);
                break;
            default:
                break;
        }
    }

    /* Dirty-rect accumulation: only the node that was DIRECTLY dirtied (source_dirty)
     * contributes to the dirty rect, not ancestors that re-render merely to clear
     * backgrounds.  This keeps the reported rect tight around the actually-changed
     * pixels so MCU display drivers can restrict partial DMA transfers. */
    if (node->source_dirty && node->painted_sequence != s_commit_sequence)
    {
        if (outer_capturing)
        {
            union_dirty_rect(
                outer_destination.x, outer_destination.y, outer_destination.width, outer_destination.height);
        }
        else if (transformed_footprint)
        {
            union_dirty_rect(destination_x, destination_y, destination_width, destination_height);
        }
        else
        {
            int damage_x = box_x, damage_y = box_y, damage_width = width, damage_height = height;
            if (node->type == ER_NODE_MODAL && node->modal_visible)
            {
                /* The backdrop covers the root, so a host flushing er_get_dirty_rect() has to send
                 * all of it — the node's own box would leave the scrim's edges untransferred. */
                const ERNode* root = er_get_root_node();
                if (root)
                {
                    damage_x = root->computed.x;
                    damage_y = root->computed.y;
                    damage_width = root->computed.width;
                    damage_height = root->computed.height;
                }
            }
            if ((node->type == ER_NODE_VECTOR || node->type == ER_NODE_ARC) && node->vector_has_dirty)
            {
                /* Match the sub-region damage so the engine's dirty-rect tracker stays tight too. */
                int dirty_offset_x, dirty_offset_y;
                vec_dirty_offset(node, &dirty_offset_x, &dirty_offset_y);
                damage_x = box_x + dirty_offset_x + (int)node->vector_dirty_x;
                damage_y = box_y + dirty_offset_y + (int)node->vector_dirty_y;
                damage_width = (int)node->vector_dirty_width;
                damage_height = (int)node->vector_dirty_height;
            }
            else if (node->type == ER_NODE_ARC)
            {
                const int overhang = (int)node->arc_overhang;
                damage_x -= overhang;
                damage_y -= overhang;
                damage_width += 2 * overhang;
                damage_height += 2 * overhang;
            }
#if ERUI_SHADOWS
            /* Expand conservatively for shadow bleed outside the node layout rect. */
            expand_for_shadow(node, &damage_x, &damage_y, &damage_width, &damage_height);
#endif
            union_dirty_rect(damage_x, damage_y, damage_width, damage_height);
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
    /* A TextInput keeps its opacity in ITS OWN union member — props.view.opacity is a different offset
     * in the same union, landing inside ERTextInputProps::font_family — a NUL for any font name short
     * enough not to reach that byte, i.e. essentially always. Read through the wrong member, every
     * field composited at alpha 0 and painted nothing at all. */
    uint8_t node_opacity = ER_ALPHA_OPAQUE;
    if (node->type == ER_NODE_VIEW || node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_PRESSABLE
        || (node->type == ER_NODE_MODAL && node->modal_visible))
        node_opacity = node->props.view.opacity;
    else if (node->type == ER_NODE_TEXT_INPUT)
        node_opacity = node->props.text_input.opacity;
    else if (node->type == ER_NODE_TEXT)
        node_opacity = node->props.text.opacity;
    else if (node->type == ER_NODE_IMAGE)
        node_opacity = node->props.image.opacity;

    /* A Text or Image is one leaf draw, so the draw-alpha path below fades it without a scratch composite. */
    const bool leaf_fade = node->type == ER_NODE_TEXT || node->type == ER_NODE_IMAGE;

    bool composited = false;
    if (node_opacity < ER_ALPHA_OPAQUE && should_render && !leaf_fade)
    {
        composited =
            composite_from_cache(node, node_opacity, box_x, box_y, width, height, child_translate_x, child_translate_y);
        if (!composited)
            composited = composite_with_opacity(
                node, node_opacity, box_x, box_y, width, height, child_translate_x, child_translate_y);
    }
    if (!composited)
    {
        /* A leaf fade, or graceful degradation: when the group cannot be composited (scratch pool
         * exhausted), multiply its opacity into every primitive draw instead of dropping it. Exact
         * wherever siblings don't overlap; far closer to correct than rendering fully opaque. */
        const uint8_t saved_alpha = er_get_draw_alpha();
        if (node_opacity < ER_ALPHA_OPAQUE && should_render)
            er_set_draw_alpha((uint8_t)((uint32_t)saved_alpha * node_opacity / ER_ALPHA_OPAQUE));
        render_node_content(
            node, needs_paint, occluded, box_x, box_y, width, height, child_translate_x, child_translate_y);
        er_set_draw_alpha(saved_alpha);
    }

    node->painted_sequence = s_commit_sequence; /* flags cleared in er_commit's sequential post-pass */

    /* Affine/perspective transform: end source capture and blit the transformed result. */
    if (doing_affine)
    {
#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
        if (doing_3d)
            er_transform_source_end_blit_3d(box_x,
                                            box_y,
                                            width,
                                            height,
                                            inverse_homography,
                                            destination_x,
                                            destination_y,
                                            destination_width,
                                            destination_height);
        else
#endif
            er_transform_source_end_blit(box_x,
                                         box_y,
                                         width,
                                         height,
                                         inverse_a,
                                         inverse_b,
                                         inverse_c,
                                         inverse_d,
                                         inverse_translate_x,
                                         inverse_translate_y,
                                         destination_x,
                                         destination_y,
                                         destination_width,
                                         destination_height);
    }
    else if (doing_bookkeeping_capture)
    {
        er_pop_clip_rect(); /* the clip reset pushed above; nothing was captured, so nothing is blitted */
    }
    if (transformed_footprint)
    {
        cc()->tile_active = saved_tile_active;
        cc()->transform_capturing = outer_capturing;
        cc()->transform_capture_destination = outer_destination;
    }
}

/**
 * @brief Draws a node's own pixels (its background, glyphs, image, vector, ...), not its children.
 *
 * Not inlined: its locals (text params, clip math) would otherwise sit in render_node_content()'s frame,
 * which stays live across the recursion into every child.
 *
 * @param[in] node    Node to draw.
 * @param[in] box_x   Screen-space left edge (scroll translation applied).
 * @param[in] box_y   Screen-space top edge.
 * @param[in] width   Node width in pixels.
 * @param[in] height  Node height in pixels.
 */
/* Defaults for unset colours and sizes, mirrored from the ERProps docs. */
#define ER_DEFAULT_TEXT_COLOR 0xFFFFFFFFU
#define ER_SWITCH_TRACK_OFF_COLOR 0xFF767577U
#define ER_SWITCH_TRACK_ON_COLOR 0xFF81B0FFU
#define ER_SWITCH_THUMB_COLOR 0xFFFFFFFFU
#define ER_SWITCH_THUMB_MARGIN 2 /* px between the thumb and the track edge */
#define ER_INPUT_FOCUS_BORDER_COLOR 0xFF4488FFU
#define ER_INPUT_PLACEHOLDER_COLOR 0xFF888888U
#define ER_INPUT_FONT_SIZE 16U
#define ER_INPUT_PADDING_X 4 /* TextInput's built-in inset when no padding is set */
#define ER_INPUT_PADDING_Y 3
#define ER_INPUT_CURSOR_WIDTH 2

static ER_NOINLINE void paint_node(ERNode* node, int box_x, int box_y, int width, int height)
{
    switch (node->type)
    {
        case ER_NODE_VIEW:
        case ER_NODE_SCROLL_VIEW:
        case ER_NODE_PRESSABLE:
        case ER_NODE_FLAT_LIST:
        {
#if ERUI_GRADIENT
            er_gradient_render(&node->props.view, box_x, box_y, width, height);
#endif
            render_view_bg(&node->props.view, box_x, box_y, width, height);
            break;
        }
        case ER_NODE_MODAL:
        {
            if (!node->modal_visible)
                break;
            /* Draw backdrop over the entire root before the modal's own background. */
            ERNode* root = er_get_root_node();
            if (root)
            {
                const uint32_t backdrop =
                    node->modal_backdrop_color ? node->modal_backdrop_color : ER_MODAL_DEFAULT_BACKDROP;
                er_blit_fill(backdrop, root->computed.x, root->computed.y, root->computed.width, root->computed.height);
                node->modal_scrim_shown = 1U;
            }
            const ERViewProps* view_props = &node->props.view;
#if ERUI_GRADIENT
            er_gradient_render(view_props, box_x, box_y, width, height);
#endif
            render_view_bg(view_props, box_x, box_y, width, height);
            break;
        }
        case ER_NODE_TEXT:
        {
            const ERTextProps* text_props = &node->props.text;
            /* Glyphs go in the node's CONTENT BOX, not its border box. par.clip is both the clip
             * and the layout rect, so this one inset moves the origin, narrows the wrap width and
             * re-anchors text_align in a single step — and it is the same padding measure_content()
             * grew the auto-sized node by, so a padded <Text> fits its own glyph run exactly. */
            int text_x = box_x, text_y = box_y, text_width = width, text_height = height;
            er_layout_content_box(&node->layout, &text_x, &text_y, &text_width, &text_height);
            ERTextRenderParams params;
            memset(&params, 0, sizeof(params));
            params.text = text_props->text;
            params.clip = (ERRect){text_x, text_y, text_width, text_height};
            params.color = text_props->color ? text_props->color : ER_DEFAULT_TEXT_COLOR;
            params.font_size = text_props->font_size;
            params.font_family = text_props->font_family;
            params.text_align = text_props->text_align;
            params.number_of_lines = text_props->number_of_lines;
            params.ellipsize_mode = text_props->ellipsize_mode;
            params.text_decoration = text_props->text_decoration;
            params.font_weight = text_props->font_weight;
            params.font_style = text_props->font_style;
            params.line_height = text_props->line_height;
            params.letter_spacing = text_props->letter_spacing;
            params.span_count = text_props->span_count;
            params.spans = (text_props->span_count > 0) ? text_props->spans : NULL;
            er_text_render(&params);
            break;
        }
        case ER_NODE_IMAGE:
        {
            /* A replaced element draws inside its padding, exactly as CSS and RN put a bitmap in
             * the box within border + padding. er_image_render() is entirely destination-rect
             * relative, so every resizeMode follows from insetting the rect alone: `cover` and
             * `contain` fit the CONTENT box, and `repeat` tiles from the content origin — which is
             * what `background-origin: padding-box`, the CSS default, does. The corner radii clip
             * it to the border box, the silhouette a background with those radii would fill. */
            int image_x = box_x, image_y = box_y, image_width = width, image_height = height;
            er_layout_content_box(&node->layout, &image_x, &image_y, &image_width, &image_height);
            const ERRect box = {box_x, box_y, width, height};
            er_image_render(&node->props.image, image_x, image_y, image_width, image_height, &box);
            break;
        }
        case ER_NODE_VECTOR:
            if (node->vector_slot >= 0)
            {
                /* Clip the rasterize to the CURRENT DAMAGE REGION (the active scissor), not just
                 * this node's own sub-rect: the background under the vector is repainted across
                 * the whole damage clip (which may be larger — e.g. unioned with the readout's
                 * box), so the vector must recompute + repaint everywhere the background was
                 * erased, or its content (e.g. the track ring) goes missing there. Intersect with
                 * the node box. With no scissor (full repaint), this is just the node box.
                 *
                 * Padding moves the tape's ORIGIN and shrinks the clip: the same replaced-element
                 * inset as Image. It does not RESCALE the geometry — a tape arrives already flat,
                 * with the <Svg> viewBox baked into its coordinates on the JS side, and the engine
                 * has no paint-time scale for one — so a padded <Svg> shifts in and clips at the
                 * content box rather than shrinking to fit it. */
                int vector_x = box_x, vector_y = box_y, vector_width = width, vector_height = height;
                er_layout_content_box(&node->layout, &vector_x, &vector_y, &vector_width, &vector_height);
                int clip_left = vector_x, clip_top = vector_y, clip_right = vector_x + vector_width,
                    clip_bottom = vector_y + vector_height;
                int clip_x, clip_y, clip_width, clip_height;
                if (er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height))
                {
                    if (clip_x > clip_left)
                        clip_left = clip_x;
                    if (clip_y > clip_top)
                        clip_top = clip_y;
                    if (clip_x + clip_width < clip_right)
                        clip_right = clip_x + clip_width;
                    if (clip_y + clip_height < clip_bottom)
                        clip_bottom = clip_y + clip_height;
                }
                /* Slot-keyed entry: an unchanged node replays its cached edge lists instead of
                 * re-tessellating the tape (ERUI_VECTOR_EDGE_CACHE). */
                er_vector_render_slot(
                    node->vector_slot, vector_x, vector_y, clip_left, clip_top, clip_right, clip_bottom);
            }
            node->vector_has_dirty = false; /* one-shot: consumed by this commit */
            break;
        case ER_NODE_ACTIVITY_INDICATOR:
        {
            /* Ring diameter comes straight off the box, so the padding shrinks the spinner and
             * keeps it centred — the same thing the author would get by shrinking the node, but
             * without giving up the space they reserved around it. */
            int content_x = box_x, content_y = box_y, content_width = width, content_height = height;
            er_layout_content_box(&node->layout, &content_x, &content_y, &content_width, &content_height);
            render_activity_indicator(node, content_x, content_y, content_width, content_height);
            break;
        }
        case ER_NODE_ARC:
            er_arc_render(node, box_x, box_y, width, height);
            node->vector_has_dirty = false; /* one-shot, like the vector sub-rect */
            break;
        case ER_NODE_SWITCH:
        {
            const ERSwitchProps* switch_props = &node->props.switch_props;
            const float progress = node->switch_thumb_progress;

            /* Track and thumb are both proportional to the box, so the whole control is drawn in
             * the content box: padding is breathing room around the pill, not inside it. The 2 px
             * thumb margin below is the control's own geometry and stays relative to the track. */
            int track_x = box_x, track_y = box_y, track_width = width, track_height = height;
            er_layout_content_box(&node->layout, &track_x, &track_y, &track_width, &track_height);

            /* Track: pill-shaped rectangle, color lerped between off/on. */
            const uint32_t off_color =
                switch_props->track_color_false ? switch_props->track_color_false : ER_SWITCH_TRACK_OFF_COLOR;
            const uint32_t on_color =
                switch_props->track_color_true ? switch_props->track_color_true : ER_SWITCH_TRACK_ON_COLOR;
            er_rrect_fill_bordered(lerp_color32(off_color, on_color, progress),
                                   0x00000000U,
                                   0,
                                   track_x,
                                   track_y,
                                   track_width,
                                   track_height,
                                   track_height / 2);

            /* Thumb: circular knob that slides along the track. */
            const int margin = ER_SWITCH_THUMB_MARGIN;
            const int thumb_size = track_height - 2 * margin;
            const int travel = track_width - thumb_size - 2 * margin;
            const int thumb_x = track_x + margin + (int)(progress * (float)travel + 0.5f);
            const uint32_t thumb_color = switch_props->thumb_color ? switch_props->thumb_color : ER_SWITCH_THUMB_COLOR;
            er_rrect_fill_bordered(
                thumb_color, 0x00000000U, 0, thumb_x, track_y + margin, thumb_size, thumb_size, thumb_size / 2);
            break;
        }
        case ER_NODE_TEXT_INPUT:
        {
            const ERTextInputProps* input_props = &node->props.text_input;
            const ERLayoutSpec* input_layout = &node->layout;
            const int padding_left =
                input_props->border_width
                + er_layout_pad_edge(input_layout->padding_left, input_layout->padding, ER_INPUT_PADDING_X);
            const int padding_right =
                input_props->border_width
                + er_layout_pad_edge(input_layout->padding_right, input_layout->padding, ER_INPUT_PADDING_X);
            const int padding_top =
                input_props->border_width
                + er_layout_pad_edge(input_layout->padding_top, input_layout->padding, ER_INPUT_PADDING_Y);
            const int padding_bottom =
                input_props->border_width
                + er_layout_pad_edge(input_layout->padding_bottom, input_layout->padding, ER_INPUT_PADDING_Y);

            /* Background + border. When focused we draw the border in cursor_color
             * directly via the bordered fill helper. Doing it this way (rather than a
             * second highlight pass with bg=0) avoids overwriting the field interior
             * with the border color, which would hide both the text and the cursor. */
            const uint32_t border_color =
                (node->is_focused && input_props->border_width > 0)
                    ? (input_props->cursor_color ? input_props->cursor_color : ER_INPUT_FOCUS_BORDER_COLOR)
                    : input_props->border_color;
            er_rrect_fill_bordered(input_props->background_color,
                                   border_color,
                                   input_props->border_width,
                                   box_x,
                                   box_y,
                                   width,
                                   height,
                                   input_props->border_radius);

            /* Text content or placeholder. */
            const bool show_placeholder = (node->input_text[0] == '\0');
            ERTextRenderParams params;
            memset(&params, 0, sizeof(params));
            params.text = show_placeholder ? input_props->placeholder : node->input_text;
            int inner_width = width - padding_left - padding_right;
            int inner_height = height - padding_top - padding_bottom;
            if (inner_width < 0)
                inner_width = 0;
            if (inner_height < 0)
                inner_height = 0;
            params.clip = (ERRect){box_x + padding_left, box_y + padding_top, inner_width, inner_height};
            params.color = show_placeholder ? (input_props->placeholder_color ? input_props->placeholder_color
                                                                              : ER_INPUT_PLACEHOLDER_COLOR)
                                            : (input_props->color ? input_props->color : ER_DEFAULT_TEXT_COLOR);
            params.font_size = input_props->font_size ? input_props->font_size : ER_INPUT_FONT_SIZE;
            params.font_family = input_props->font_family;
            params.number_of_lines = 1;
            params.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
            const bool masked = input_props->secure && !show_placeholder;
            int typed_width = 0;
            if (masked)
                typed_width = paint_masked_text(node->input_text, &params);
            else
                er_text_render(&params);

            /* Blinking cursor when focused and not showing placeholder. */
            if (node->is_focused && !show_placeholder && cursor_blink_on(s_now_ms))
            {
                int typed_height = 0;
                if (!masked)
                    er_text_measure(node->input_text,
                                    params.font_size,
                                    input_props->font_family,
                                    0,
                                    params.font_weight,
                                    &typed_width,
                                    &typed_height);
                int cursor_x = box_x + padding_left + typed_width;
                const int max_cursor_x = box_x + width - padding_right - ER_INPUT_CURSOR_WIDTH;
                if (cursor_x > max_cursor_x)
                    cursor_x = max_cursor_x;
                const uint32_t caret_color = input_props->cursor_color
                                                 ? input_props->cursor_color
                                                 : (input_props->color ? input_props->color : ER_DEFAULT_TEXT_COLOR);
                er_rrect_fill_bordered(
                    caret_color, 0x00000000U, 0, cursor_x, box_y + padding_top, ER_INPUT_CURSOR_WIDTH, inner_height, 0);
            }
            else if (node->is_focused && show_placeholder && cursor_blink_on(s_now_ms))
            {
                /* Cursor at start when field is empty. */
                const uint32_t caret_color = input_props->cursor_color
                                                 ? input_props->cursor_color
                                                 : (input_props->color ? input_props->color : ER_DEFAULT_TEXT_COLOR);
                er_rrect_fill_bordered(caret_color,
                                       0x00000000U,
                                       0,
                                       box_x + padding_left,
                                       box_y + padding_top,
                                       ER_INPUT_CURSOR_WIDTH,
                                       inner_height,
                                       0);
            }
            break;
        }
        default:
            break;
    }
}

static void render_node_content(ERNode* node,
                                bool needs_paint,
                                bool occluded,
                                int box_x,
                                int box_y,
                                int width,
                                int height,
                                int translate_x,
                                int translate_y)
{
    /* Overflow clipping: children cannot draw outside this node. A scroller (ScrollView / FlatList)
     * ALWAYS clips to its viewport — that is its defining behaviour — regardless of an explicit
     * overflow style, so scrolled children can't escape past the top or bottom edge. */
    const bool clips = node_clips_children(node);

    /* Scroll offset translation: ScrollView and FlatList children are shifted by the current offset. */
    const bool is_scroller = (node->type == ER_NODE_SCROLL_VIEW || node->type == ER_NODE_FLAT_LIST);
    const int child_translate_x = is_scroller ? translate_x + (int)node->scroll_offset_x : translate_x;
    const int child_translate_y = is_scroller ? translate_y + (int)node->scroll_offset_y : translate_y;

    ERChildCursor cursor;

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
     * paints as usual and the occluder covers it a moment later. A clipping container's
     * subtree_prunable leaves its children's transforms out, so subtree_complex_xf, which counts them,
     * gates the burial too. A translated node can still be walked occluded inside a clipping container;
     * render_tree applies its offset there, so its record stays right.
     *
     * The children are scanned front to back (the cursor only walks forward), keeping the last match. */
    int occluder_index = -1;
#if ERUI_OCCLUSION_CULLING
    if (!occluded && er_child_first(&cursor, node) && er_get_draw_alpha() == ER_ALPHA_OPAQUE)
    {
        int region_x, region_y, region_width, region_height;
        if (!er_get_clip_rect(&region_x, &region_y, &region_width, &region_height))
        {
            const ERNode* root = er_get_root_node(); /* unclipped full repaint: the region is the screen */
            if (!root)
                region_width = region_height = 0;
            else
            {
                region_x = root->computed.x;
                region_y = root->computed.y;
                region_width = root->computed.width;
                region_height = root->computed.height;
            }
        }
        if (clips)
        {
            /* Children are about to be scissored to this node's box; the node's own background fills
             * exactly the same area, so one region covers both decisions. */
            const int clipped_right =
                (region_x + region_width < box_x + width) ? region_x + region_width : box_x + width;
            const int clipped_bottom =
                (region_y + region_height < box_y + height) ? region_y + region_height : box_y + height;
            if (box_x > region_x)
                region_x = box_x;
            if (box_y > region_y)
                region_y = box_y;
            region_width = clipped_right - region_x;
            region_height = clipped_bottom - region_y;
        }
        int child_index = 0;
        for (const ERNode* child = er_child_first(&cursor, node); child; child = er_child_next(&cursor), child_index++)
        {
            if (node_covers_opaque(
                    child, child_translate_x, child_translate_y, region_x, region_y, region_width, region_height))
                occluder_index = child_index;
        }
    }
#endif

    /* A Modal's backdrop covers the whole root rather than this node's box, so a child covering the
     * box proves nothing about the pixels the scrim owns — never skip its own draw. */
    const bool self_covered = (occluder_index >= 0) && (node->type != ER_NODE_MODAL);
    const bool should_render = needs_paint && !occluded && !self_covered;

    if (should_render)
        paint_node(node, box_x, box_y, width, height);

    /* A one-shot sub-region damage rect that was never consumed (the node is buried this commit)
     * must still be retired, or the next commit would narrow that node's repaint to a stale rect. */
    if (needs_paint && !should_render && (node->type == ER_NODE_VECTOR || node->type == ER_NODE_ARC))
        node->vector_has_dirty = false;

    if (clips)
        er_push_clip_rect(box_x, box_y, width, height);

    int child_index = 0;
    for (ERNode* child = er_child_first(&cursor, node); child; child = er_child_next(&cursor), child_index++)
    {
        /* Children before the occluder are buried by it; children from the occluder on still paint.
         * A subtree carrying a transform is never buried — see the note on the cull above. */
        const bool buried =
            (child_index < occluder_index) && child->subtree_prunable && !child->subtree_complex_transform;
        render_tree(child, needs_paint, occluded || buried, child_translate_x, child_translate_y);
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
    int16_t max_right = 0;
    int16_t max_bottom = 0;

    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        const ERNode* child = er_get_node(child_tag);
        if (child)
        {
            const int16_t right = (int16_t)(child->computed.x - node->computed.x + child->computed.width);
            const int16_t bottom = (int16_t)(child->computed.y - node->computed.y + child->computed.height);
            if (right > max_right)
                max_right = right;
            if (bottom > max_bottom)
                max_bottom = bottom;
        }
        child_tag = child ? child->next_sibling_tag : ER_INVALID_TAG;
    }

    node->scroll_content_width = max_right;
    node->scroll_content_height = max_bottom;
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
 * @brief Walks the subtree and fires ER_EVENT_LAYOUT for every node whose rectangle relative to its
 *        parent changed since it was last reported.
 *
 * The payload is parent-relative, as in React Native, so a node whose parent moved without moving it
 * doesn't fire. A display:none node reports an empty box at 0,0, and its subtree is skipped: those
 * rects are stale until it shows again.
 *
 * @param[in] node         Subtree root to check.
 * @param[in] parent_rect  The parent's computed rect from this commit.
 */
static void dispatch_layout_events(ERNode* node, ERLayoutRect parent_rect)
{
    if (!node)
        return;

    const bool hidden = node->layout.display == ER_DISPLAY_NONE;
    const ERLayoutRect rect = node->computed;
    ERLayoutRect relative = rect;
    if (hidden)
    {
        relative.x = 0;
        relative.y = 0;
    }
    else
    {
        relative.x = (int16_t)(rect.x - parent_rect.x);
        relative.y = (int16_t)(rect.y - parent_rect.y);
    }

    const ERLayoutRect previous = node->previous_layout;
    if (relative.x != previous.x || relative.y != previous.y || relative.width != previous.width
        || relative.height != previous.height)
    {
        node->previous_layout = relative;
        const EREventHandler* handler = &node->events[ER_EVENT_LAYOUT];
        if (handler->callback)
        {
            EREventData data = {0};
            data.layout_rect.x = relative.x;
            data.layout_rect.y = relative.y;
            data.layout_rect.width = relative.width;
            data.layout_rect.height = relative.height;
            handler->callback(node, &data, handler->user_data);
        }
    }

    if (hidden)
        return;

    uint16_t child_tag = node->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        dispatch_layout_events(child, rect);
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

ERNodeRef er_node_ref(const ERNode* node)
{
    ERNodeRef ref = {ER_INVALID_TAG, 0U};
    if (node)
    {
        ref.tag = node->tag;
        ref.serial = node->serial;
    }
    return ref;
}

ERNode* er_node_deref(ERNodeRef ref)
{
    ERNode* node = er_get_node(ref.tag);
    return (node && node->serial == ref.serial) ? node : NULL;
}

void er_pool_limits(ERPoolLimits* limits)
{
    memset(limits, 0, sizeof(*limits));
    limits->nodes = (uint16_t)ERUI_MAX_NODES;
    limits->vector_nodes = (uint16_t)er_vector_capacity();
    limits->images = (uint16_t)ERUI_IMAGE_REGISTRY_MAX;
    limits->font_families = (uint16_t)FONT_REGISTRY_MAX;
    limits->font_sizes = (uint16_t)FONT_FAMILY_MAX_SIZES;
    er_animation_pool_limits(limits);
}

ERNode* er_get_root_node(void)
{
    return er_get_node(s_root_tag);
}

int er_node_in_use_count(void)
{
    int count = 0;
    for (int tag = 0; tag < (int)ERUI_MAX_NODES; tag++)
        if (s_nodes[tag].in_use)
            count++;
    return count;
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

ERNode* er_node_parent(const ERNode* node)
{
    return node ? er_get_node(node->parent_tag) : NULL;
}

ERNodeType er_node_get_type(const ERNode* node)
{
    return node ? node->type : ER_NODE_VIEW;
}

float er_arc_get_value(const ERNode* node)
{
    return (node && node->type == ER_NODE_ARC) ? node->arc_value : 0.0f;
}

/** @brief One turn of an ActivityIndicator's spinner, in milliseconds. */
#define ER_ACTIVITY_SPIN_MS 1000U

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

    ERNode* node = &s_nodes[tag];
    memset(node, 0, sizeof(ERNode));

    node->tag = tag;
    node->serial = s_node_serial;
    s_node_serial = er_node_next_serial(s_node_serial);
    node->parent_tag = ER_INVALID_TAG;
    node->first_child_tag = ER_INVALID_TAG;
    node->next_sibling_tag = ER_INVALID_TAG;
    node->type = type;
    node->in_use = true;
    node->dirty = true;
    node->vector_slot = -1; /* memset cleared it to 0, which is a valid slot; -1 = "no geometry". */

    if (type == ER_NODE_VECTOR || type == ER_NODE_ARC)
        s_parallel_unsafe++; /* vector rasterizer / arc span cache use shared scratch — see s_parallel_unsafe */

    /* No finger owns a fresh arc. memset left this 0, which would read as "finger 0 is dragging". */
    if (type == ER_NODE_ARC)
        node->arc_drag_finger = -1;

    init_layout_defaults(&node->layout);

    /* Nodes with an opacity default to fully opaque. */
    if (type == ER_NODE_VIEW || type == ER_NODE_SCROLL_VIEW || type == ER_NODE_PRESSABLE || type == ER_NODE_MODAL)
    {
        node->props.view.opacity = ER_ALPHA_OPAQUE;
    }
    else if (type == ER_NODE_TEXT)
    {
        node->props.text.opacity = ER_ALPHA_OPAQUE;
    }
    else if (type == ER_NODE_IMAGE)
    {
        node->props.image.opacity = ER_ALPHA_OPAQUE;
    }
    else if (type == ER_NODE_TEXT_INPUT)
    {
        node->props.text_input.opacity = ER_ALPHA_OPAQUE;
    }

    /* ActivityIndicator starts spinning immediately. */
    if (type == ER_NODE_ACTIVITY_INDICATOR)
    {
        node->props.activity_indicator.animating = 1U;
        ERAnimationConfig config;
        memset(&config, 0, sizeof(config));
        config.type = ER_ANIMATION_TIMING;
        config.duration_ms = ER_ACTIVITY_SPIN_MS;
        config.loop = true;
        er_animation_start(node, ER_PROP_ROTATE_Z, 360.0f, &config);
    }

    /* TextInput: default to editable. */
    if (type == ER_NODE_TEXT_INPUT)
        node->props.text_input.editable = 1U;

    return node;
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
                          (int)node->last_paint_rect.width,
                          (int)node->last_paint_rect.height);

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
    /* Cut every animation loose from this node — its tag is about to be recycled onto the free list, and
     * anything still pointing at it would drive whatever node next reuses it. Three separate tables hold
     * such a reference: ERAnimationValue bindings (er_animation_unbind_node), animations keyed by tag
     * (er_animation_cancel_node, groups included) and layout animations (er_layout_animation_cancel_node). The tick
     * functions each drop a reference whose node has gone, but only while the tag is still unclaimed —
     * a node created before the next tick inherits it. */
    er_animation_unbind_node(node->tag);
    er_animation_cancel_node(node->tag);
    er_layout_animation_cancel_node(node->tag);
    /* A touch handler destroying nodes mid-dispatch must not let the rest of the dispatch reach their slots. */
    er_input_forget_node(node->tag);
    /* The focus goes with the input, or the next node in its slot would take the typing. No blur: the node is
     * gone. The keyboard it brought up still has to be erased. */
    if (node->tag == s_focused_input_tag)
    {
        s_focused_input_tag = ER_INVALID_TAG;
        s_keyboard_dirty = true;
        s_keyboard_layer = 0;
    }
    /* A destroyed node that was still linked into the tree changes its siblings' layout. */
    mark_layout_dirty();
    /* Guard against overflow (would only occur on a double-free bug in the caller). */
    if (s_free_count < (uint16_t)ERUI_MAX_NODES)
        s_free_list[s_free_count++] = node->tag;
}

/** @brief FNV-1a 32-bit parameters, as published (the mixing below is a derivative — see props_hash32). */
#define FNV1A32_OFFSET_BASIS 2166136261U
#define FNV1A32_PRIME 16777619U

/** @brief Independent accumulators in the word-wise loop, and the bytes one pass therefore consumes. */
#define FNV1A32_LANES 2U
#define FNV1A32_PASS_BYTES (FNV1A32_LANES * sizeof(uint32_t))

/** @brief Mixes one value into an FNV-1a accumulator. */
#define FNV1A32_MIX(hash, value) (((hash) ^ (uint32_t)(value)) * FNV1A32_PRIME)

/**
 * @brief Change-detection hash over a byte range (ERProps is zero-initialised by the bridge, so equal
 *        props hash equal).
 *
 * NOT the published FNV-1a, despite using its constants. It consumes native-endian 32-bit words and
 * folds two accumulators, so its output differs from canonical FNV-1a and from itself across
 * endiannesses. That is fine here because a hash is only ever compared against one this same build
 * produced — but it must not be persisted, put on a wire, or compared across architectures.
 *
 * Word-at-a-time because ERProps is ~1 KB and this runs twice on every setProps, so the byte-wise loop
 * was the bulk of an unchanged node's update cost. Two accumulators keep the multiplies off one
 * dependency chain; trailing bytes fold into the first.
 *
 * The word is read through a uint32_t*, guarded by an alignment test, rather than memcpy'd: on a
 * target without unaligned loads (Xtensa, Cortex-M0+) the compiler turns a 4-byte memcpy into an
 * out-of-line call, and the wide version measured 1.8x SLOWER on an ESP32-S3 than the byte loop it
 * replaced.
 */
static uint32_t props_hash32(const void* data, size_t length)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t first_lane = FNV1A32_OFFSET_BASIS;
    uint32_t second_lane = FNV1A32_OFFSET_BASIS;
    /* Always aligned for ERProps; the tail loop below covers any range that is not. */
    if (((uintptr_t)bytes % sizeof(uint32_t)) == 0U)
    {
        const uint32_t* words = (const uint32_t*)(const void*)bytes;
        size_t passes = length / FNV1A32_PASS_BYTES;
        length -= passes * FNV1A32_PASS_BYTES;
        while (passes-- > 0U)
        {
            first_lane = FNV1A32_MIX(first_lane, words[0]);
            second_lane = FNV1A32_MIX(second_lane, words[1]);
            words += FNV1A32_LANES;
        }
        bytes = (const uint8_t*)(const void*)words;
    }
    while (length-- > 0U)
    {
        first_lane = FNV1A32_MIX(first_lane, *bytes++);
    }
    /* Fold the lanes together so a change in either one always moves the result. */
    return FNV1A32_MIX(first_lane, second_lane * FNV1A32_PRIME);
}

/* ERProps is laid out as one contiguous Yoga layout block followed by everything else, so the
 * boundary is a single offset: background_color is the first field past it. Hashing the two sides
 * separately costs exactly what hashing the whole struct did, and answers the question the damage
 * pre-pass actually needs — did this update change how the node LOOKS, or only where things sit? */
#define ER_PROPS_LAYOUT_BYTES offsetof(ERProps, background_color)

#define ER_DEFAULT_SHADOW_COLOR 0xFF000000U

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
    for (size_t field_index = 0; field_index < sizeof(auto_fields) / sizeof(auto_fields[0]); field_index++)
        *auto_fields[field_index] = ER_LAYOUT_AUTO;

    props->opacity = ER_ALPHA_OPAQUE;

    /* RN defaults the transform pivot to the node centre; the engine treats 0.0 as a literal top-left
       pivot, so seed the fractional origin to 0.5/0.5 (overridden by transformOrigin). */
    props->transform_origin_x = 0.5f;
    props->transform_origin_y = 0.5f;

    /* Type-specific fields whose engine default is non-zero. Harmless for other node types, since
       er_node_set_props applies only the fields relevant to the node's type. */
    props->editable = 1;               /* TextInput editable by default. */
    props->animating = 1;              /* ActivityIndicator spins by default. */
    props->shadow_color = ER_DEFAULT_SHADOW_COLOR; /* Opaque black shadow unless overridden. */
}

/**
 * @brief Copies the shadow and gradient props into a node's view props, keeping the parallel-render count true.
 *
 * Every node type that stores ERViewProps goes through here — the View family and Modal — because the two
 * halves are not independent. s_parallel_unsafe counts shadow CASTERS as a running transition tally, so a
 * branch that copied the shadow props without it either leaks a count (multi-worker rendering stays off for
 * good) or under-counts one (a shadow-casting modal renders racily in parallel). Sharing the block also means
 * a gradient prop added here reaches <Modal> instead of quietly applying only to <View>.
 *
 * @param[in,out] node   Node whose view props receive the values.
 * @param[in]     props  Incoming props.
 */
static void copy_view_shadow_and_gradient(ERNode* node, const ERProps* props)
{
    node->props.view.shadow_color = props->shadow_color;
    node->props.view.shadow_offset_x = er_px_offset(props->shadow_offset_x);
    node->props.view.shadow_offset_y = er_px_offset(props->shadow_offset_y);
#if ERUI_SHADOWS
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
    for (int stop_index = 0; stop_index < ER_GRADIENT_MAX_STOPS; stop_index++)
        node->props.view.gradient_stops[stop_index] = props->gradient_stops[stop_index];
}

void er_node_set_props(ERNode* node, const ERProps* props)
{
    if (!node || !props)
        return;

    /* Skip the layout/repaint invalidation when the props are byte-identical to what's already
     * applied (e.g. React re-running a render with freshly-allocated but equal inline-style objects).
     * The field copies below still run so all derived state stays correct; only the expensive dirty
     * marking is gated, so an unchanged node doesn't drag the whole screen into a repaint. */
    const uint32_t layout_hash = props_hash32(props, ER_PROPS_LAYOUT_BYTES);
    const uint32_t visual_hash =
        props_hash32((const uint8_t*)props + ER_PROPS_LAYOUT_BYTES, sizeof(ERProps) - ER_PROPS_LAYOUT_BYTES);
    const bool first_update = !node->has_props_hash;
    const bool layout_changed = first_update || layout_hash != node->layout_props_hash;
    const bool visual_changed = first_update || visual_hash != node->visual_props_hash;
    const bool props_changed = layout_changed || visual_changed;
    node->layout_props_hash = layout_hash;
    node->visual_props_hash = visual_hash;
    node->has_props_hash = true;

    /* Remembered across the layout copy below so a display:none toggle can be detected and the
     * subtree's hidden state settled once, after this node's own dirty marking (see propagate_hidden). */
    const uint8_t previous_display = node->layout.display;

    /* Same idea for overflow, and read HERE because the cached subtree paint bounds it is compared
     * against are still the pre-change ones (see the settle below). */
    const bool previous_clips = node_clips_children(node);

    /* Copy all layout fields. */
    ERLayoutSpec* spec = &node->layout;
    spec->left = props->left;
    spec->top = props->top;
    spec->right = props->right;
    spec->bottom = props->bottom;
    spec->width = props->width;
    spec->height = props->height;
    spec->min_width = props->min_width;
    spec->max_width = props->max_width;
    spec->min_height = props->min_height;
    spec->max_height = props->max_height;
    spec->padding = props->padding;
    /* paddingHorizontal/Vertical expand into per-edge; per-edge wins over shorthand. */
    spec->padding_left = (props->padding_left != ER_LAYOUT_AUTO)         ? props->padding_left
                         : (props->padding_horizontal != ER_LAYOUT_AUTO) ? props->padding_horizontal
                                                                         : ER_LAYOUT_AUTO;
    spec->padding_right = (props->padding_right != ER_LAYOUT_AUTO)        ? props->padding_right
                          : (props->padding_horizontal != ER_LAYOUT_AUTO) ? props->padding_horizontal
                                                                          : ER_LAYOUT_AUTO;
    spec->padding_top = (props->padding_top != ER_LAYOUT_AUTO)        ? props->padding_top
                        : (props->padding_vertical != ER_LAYOUT_AUTO) ? props->padding_vertical
                                                                      : ER_LAYOUT_AUTO;
    spec->padding_bottom = (props->padding_bottom != ER_LAYOUT_AUTO)     ? props->padding_bottom
                           : (props->padding_vertical != ER_LAYOUT_AUTO) ? props->padding_vertical
                                                                         : ER_LAYOUT_AUTO;
    spec->margin = props->margin;
    /* marginHorizontal/Vertical expand into per-edge; per-edge wins over shorthand. */
    spec->margin_left = (props->margin_left != ER_LAYOUT_AUTO)         ? props->margin_left
                        : (props->margin_horizontal != ER_LAYOUT_AUTO) ? props->margin_horizontal
                                                                       : ER_LAYOUT_AUTO;
    spec->margin_right = (props->margin_right != ER_LAYOUT_AUTO)        ? props->margin_right
                         : (props->margin_horizontal != ER_LAYOUT_AUTO) ? props->margin_horizontal
                                                                        : ER_LAYOUT_AUTO;
    spec->margin_top = (props->margin_top != ER_LAYOUT_AUTO)        ? props->margin_top
                       : (props->margin_vertical != ER_LAYOUT_AUTO) ? props->margin_vertical
                                                                    : ER_LAYOUT_AUTO;
    spec->margin_bottom = (props->margin_bottom != ER_LAYOUT_AUTO)     ? props->margin_bottom
                          : (props->margin_vertical != ER_LAYOUT_AUTO) ? props->margin_vertical
                                                                       : ER_LAYOUT_AUTO;
    spec->gap = props->gap;
    spec->row_gap = props->row_gap;
    spec->column_gap = props->column_gap;
    spec->flex_grow = props->flex_grow;
    spec->flex_shrink = props->flex_shrink;
    spec->flex_basis = props->flex_basis;
    spec->flex_direction = props->flex_direction;
    spec->flex_wrap = props->flex_wrap;
    spec->align_items = props->align_items;
    spec->align_self = props->align_self;
    spec->align_content = props->align_content;
    spec->justify_content = props->justify_content;
    spec->position = props->position;
    spec->display = props->display;
    spec->overflow = props->overflow;
    spec->aspect_ratio = props->aspect_ratio;
    spec->flex_basis_percent = props->flex_basis_percent;
    spec->width_percent = props->width_percent;
    spec->height_percent = props->height_percent;
    spec->left_percent = props->left_percent;
    spec->top_percent = props->top_percent;
    spec->right_percent = props->right_percent;
    spec->bottom_percent = props->bottom_percent;
    node->z_index = props->z_index;
    node->pointer_events = props->pointer_events;
    node->hit_slop_left = props->hit_slop_left;
    node->hit_slop_top = props->hit_slop_top;
    node->hit_slop_right = props->hit_slop_right;
    node->hit_slop_bottom = props->hit_slop_bottom;
    node->long_press_ms = props->long_press_ms;

    /* Copy transform props. A NaN component is unset (a NaN origin is the centre), where it would reach the
     * integer casts that place and bound the node. */
    node->transform_translate_x = er_px_offset(props->transform_translate_x);
    node->transform_translate_y = er_px_offset(props->transform_translate_y);
    node->transform_scale_x = er_nan_or(props->transform_scale_x, 0.0f);
    node->transform_scale_y = er_nan_or(props->transform_scale_y, 0.0f);
    node->transform_rotate_z = er_nan_or(props->transform_rotate_z, 0.0f);
    node->transform_origin_x = er_nan_or(props->transform_origin_x, 0.5f);
    node->transform_origin_y = er_nan_or(props->transform_origin_y, 0.5f);
    node->transform_rotate_x = er_nan_or(props->transform_rotate_x, 0.0f);
    node->transform_rotate_y = er_nan_or(props->transform_rotate_y, 0.0f);
    node->transform_perspective = er_nan_or(props->transform_perspective, 0.0f);
    node->has_transform =
        (node->transform_translate_x != 0.0f || node->transform_translate_y != 0.0f || node->transform_scale_x != 0.0f
         || node->transform_scale_y != 0.0f || node->transform_rotate_z != 0.0f || node->transform_rotate_x != 0.0f
         || node->transform_rotate_y != 0.0f || node->transform_perspective != 0.0f);

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
            node->props.view.border_top_left_radius = props->border_top_left_radius;
            node->props.view.border_top_right_radius = props->border_top_right_radius;
            node->props.view.border_bottom_right_radius = props->border_bottom_right_radius;
            node->props.view.border_bottom_left_radius = props->border_bottom_left_radius;
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
            copy_view_shadow_and_gradient(node, props);
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
            node->props.text.opacity = props->opacity;
            {
                const uint8_t span_count =
                    (props->span_count < ER_TEXT_MAX_SPANS) ? props->span_count : (uint8_t)ER_TEXT_MAX_SPANS;
                node->props.text.span_count = span_count;
                for (uint8_t span_index = 0; span_index < span_count; span_index++)
                {
                    ERTextSpan* destination = &node->props.text.spans[span_index];
                    const ERTextSpan* source = &props->spans[span_index];
                    strncpy(destination->text, source->text, ER_SPAN_TEXT_MAX);
                    destination->text[ER_SPAN_TEXT_MAX] = '\0';
                    destination->color = source->color;
                    destination->font_size = source->font_size;
                    destination->font_weight = source->font_weight;
                    destination->font_style = source->font_style;
                    destination->text_decoration = source->text_decoration;
                    destination->letter_spacing = source->letter_spacing;
                }
            }
            break;
        case ER_NODE_IMAGE:
            strncpy(node->props.image.image_name, props->image_name, ER_IMAGE_NAME_MAX);
            node->props.image.image_name[ER_IMAGE_NAME_MAX] = '\0';
            node->props.image.resize_mode = props->resize_mode;
            node->props.image.tint_color = props->tint_color;
            node->props.image.opacity = props->opacity;
            node->props.image.border_top_left_radius =
                props->border_top_left_radius > 0 ? props->border_top_left_radius : props->border_radius;
            node->props.image.border_top_right_radius =
                props->border_top_right_radius > 0 ? props->border_top_right_radius : props->border_radius;
            node->props.image.border_bottom_right_radius =
                props->border_bottom_right_radius > 0 ? props->border_bottom_right_radius : props->border_radius;
            node->props.image.border_bottom_left_radius =
                props->border_bottom_left_radius > 0 ? props->border_bottom_left_radius : props->border_radius;
            break;
        case ER_NODE_ACTIVITY_INDICATOR:
        {
            const uint8_t was_animating = node->props.activity_indicator.animating;
            node->props.activity_indicator.color = props->indicator_color;
            node->props.activity_indicator.animating = props->animating;
            if (props->animating && !was_animating)
            {
                /* (Re-)start the looping spin animation. */
                ERAnimationConfig config;
                memset(&config, 0, sizeof(config));
                config.type = ER_ANIMATION_TIMING;
                config.duration_ms = ER_ACTIVITY_SPIN_MS;
                config.loop = true;
                er_animation_start(node, ER_PROP_ROTATE_Z, 360.0f, &config);
            }
            else if (!props->animating && was_animating)
            {
                er_animation_cancel(node, ER_PROP_ROTATE_Z);
            }
            break;
        }
        case ER_NODE_SWITCH:
        {
            const uint8_t old_value = node->props.switch_props.value;
            node->props.switch_props.track_color_false = props->track_color_false;
            node->props.switch_props.track_color_true = props->track_color_true;
            node->props.switch_props.thumb_color = props->thumb_color;
            node->props.switch_props.value = props->switch_value;

            /* Animate the thumb when the value changes. */
            if (props->switch_value != old_value)
            {
                ERAnimationConfig config;
                memset(&config, 0, sizeof(config));
                config.type = ER_ANIMATION_TIMING;
                config.easing = ER_EASE_EASE_IN_OUT;
                config.duration_ms = ER_SWITCH_THUMB_MS;
                er_animation_start(node, ER_PROP_SWITCH_THUMB, props->switch_value ? 1.0f : 0.0f, &config);
            }
            break;
        }
        case ER_NODE_ARC:
        {
            ERArcProps* arc_props = &node->props.arc;
            ERArcProps before;
            memcpy(&before, arc_props, sizeof(before));
            arc_props->min = props->arc_min;
            arc_props->max = props->arc_max;
            arc_props->start_angle = props->arc_start_angle;
            arc_props->sweep_angle = props->arc_sweep_angle;
            arc_props->step = props->arc_step;
            arc_props->gap_angle = props->arc_gap_angle;
            arc_props->width = props->arc_width;
            arc_props->band_width = props->arc_band_width;
            arc_props->knob_size = props->arc_knob_size;
            arc_props->knob_border_width = props->arc_knob_border_width;
            arc_props->track_color = props->arc_track_color;
            arc_props->indicator_color = props->arc_indicator_color;
            arc_props->band_color = props->arc_band_color;
            arc_props->knob_color = props->arc_knob_color;
            arc_props->knob_border_color = props->arc_knob_border_color;
            arc_props->segments = props->arc_segments;
            arc_props->cap = props->arc_cap;
            arc_props->knob = props->arc_knob;
            arc_props->adjustable = props->arc_adjustable;
            arc_props->range = props->arc_range;
            arc_props->value_start = props->arc_value_start;
            arc_props->min_span = props->arc_min_span;
            arc_props->gradient_type = props->gradient_type;
            arc_props->gradient_stop_count = props->gradient_stop_count;
            for (int stop_index = 0; stop_index < ER_GRADIENT_MAX_STOPS; stop_index++)
                arc_props->gradient_stops[stop_index] = props->gradient_stops[stop_index];
            strncpy(arc_props->image_name, props->image_name, ER_IMAGE_NAME_MAX);
            arc_props->image_name[ER_IMAGE_NAME_MAX] = '\0';
            const bool shape_changed = (memcmp(&before, arc_props, sizeof(before)) != 0);
            /* A native drag owns the value while the finger is down: a React re-render mid-drag (e.g. the
             * readout updating from onChange) must not snap the knob back to the value it rendered with. */
            if (node->arc_drag_finger < 0)
            {
                /* Order matters in RANGE mode: each end clamps against the other, so widening the band
                 * needs the far end moved first or it would clamp itself to the stale neighbour. */
                if (props->arc_value >= node->arc_value)
                {
                    (void)er_arc_apply_value(node, props->arc_value);
                    if (arc_props->range)
                        (void)er_arc_apply_value_start(node, props->arc_value_start);
                }
                else
                {
                    if (arc_props->range)
                        (void)er_arc_apply_value_start(node, props->arc_value_start);
                    (void)er_arc_apply_value(node, props->arc_value);
                }
            }
            if (shape_changed)
            {
                /* Anything but the value changed → every pixel may differ: repaint the whole box. */
                node->vector_has_dirty = false;
                er_arc_refresh_overhang(node);
            }
            break;
        }
        case ER_NODE_TEXT_INPUT:
            node->props.text_input.background_color = props->background_color;
            node->props.text_input.border_color = props->border_color;
            node->props.text_input.border_width = props->border_width;
            node->props.text_input.border_radius = props->border_radius;
            node->props.text_input.opacity = props->opacity;
            node->props.text_input.color = props->color;
            node->props.text_input.font_size = props->font_size;
            strncpy(node->props.text_input.font_family, props->font_family, ER_FONT_FAMILY_MAX);
            node->props.text_input.font_family[ER_FONT_FAMILY_MAX] = '\0';
            strncpy(node->props.text_input.placeholder, props->placeholder, ER_PLACEHOLDER_MAX);
            node->props.text_input.placeholder[ER_PLACEHOLDER_MAX] = '\0';
            node->props.text_input.placeholder_color = props->placeholder_color;
            node->props.text_input.cursor_color = props->cursor_color;
            node->props.text_input.editable = props->editable ? 1U : 0U;
            node->props.text_input.secure = props->secure_text_entry ? 1U : 0U;
            /* If 'text' is provided, set it as the current input value. */
            if (props->text[0] != '\0')
                er_text_input_set_text(node, props->text);
            break;
        case ER_NODE_MODAL:
            node->props.view.background_color = props->background_color;
            node->props.view.border_color = props->border_color;
            node->props.view.border_width = props->border_width;
            node->props.view.border_radius = props->border_radius;
            node->props.view.opacity = props->opacity;
            node->modal_visible = props->modal_visible;
            node->modal_backdrop_color = props->backdrop_color;
            /* Propagate visibility to the layout so the modal takes no space when hidden. */
            node->layout.display = props->modal_visible ? ER_DISPLAY_FLEX : ER_DISPLAY_NONE;
            copy_view_shadow_and_gradient(node, props);
            break;
        default:
            break;
    }

    /* Native-driver props (opacity/transform/color animated via an ERAnimationValue) are owned by the
     * animation, not this declarative update — restore them so a React commitUpdate doesn't snap
     * the node back to its static value mid-animation. No-op when nothing is bound to this node. */
    er_animation_reapply_bound(node);

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
    if (node_clips_children(node) != previous_clips)
    {
        int bounds_x, bounds_y, bounds_width, bounds_height;
        if (node_subtree_screen_rect(node, &bounds_x, &bounds_y, &bounds_width, &bounds_height))
        {
            clip_rect_to_clippers(node, &bounds_x, &bounds_y, &bounds_width, &bounds_height);
            note_vacated_rect(node, bounds_x, bounds_y, bounds_width, bounds_height);
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
    if (node->layout.display != previous_display)
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

    s_keyboard_dirty = true; /* focus is changing → repaint the on-screen keyboard strip (show or hide) */
    s_keyboard_layer = 0;    /* a freshly focused input starts on the lowercase layer */

    /* Blur the currently focused node first. */
    if (s_focused_input_tag != ER_INVALID_TAG)
    {
        ERNode* previous = er_get_node(s_focused_input_tag);
        if (previous)
        {
            previous->is_focused = false;
            er_mark_dirty_upward(previous);
            const EREventHandler* handler = &previous->events[ER_EVENT_BLUR];
            if (handler->callback)
            {
                EREventData data = {0};
                handler->callback(previous, &data, handler->user_data);
            }
        }
        s_focused_input_tag = ER_INVALID_TAG;
    }

    if (!node)
        return;

    node->is_focused = true;
    s_focused_input_tag = node->tag;
    er_mark_dirty_upward(node);

    const EREventHandler* handler = &node->events[ER_EVENT_FOCUS];
    if (handler->callback)
    {
        EREventData data = {0};
        handler->callback(node, &data, handler->user_data);
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
    const uint8_t kept_count = (count < (uint8_t)ER_TEXT_MAX_SPANS) ? count : (uint8_t)ER_TEXT_MAX_SPANS;
    node->props.text.span_count = (spans && kept_count > 0) ? kept_count : 0U;
    for (uint8_t span_index = 0; span_index < node->props.text.span_count; span_index++)
    {
        ERTextSpan* destination = &node->props.text.spans[span_index];
        strncpy(destination->text, spans[span_index].text, ER_SPAN_TEXT_MAX);
        destination->text[ER_SPAN_TEXT_MAX] = '\0';
        destination->color = spans[span_index].color;
        destination->font_size = spans[span_index].font_size;
        destination->font_weight = spans[span_index].font_weight;
        destination->font_style = spans[span_index].font_style;
        destination->text_decoration = spans[span_index].text_decoration;
        destination->letter_spacing = spans[span_index].letter_spacing;
    }

    const bool pinned_width = (node->layout.width != ER_LAYOUT_AUTO) || (node->layout.width_percent > 0.0f);
    const bool pinned_height = (node->layout.height != ER_LAYOUT_AUTO) || (node->layout.height_percent > 0.0f);
    const bool single_line = (node->props.text.number_of_lines == 1U);
    if (!(pinned_width && (single_line || pinned_height)))
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
 * upload, different length/opcodes/paint-index, or a paint- or gradient-table change) falls back to a
 * full-box repaint, so the rect can never be too SMALL — no stale-pixel artifacts. A paint-only change
 * (same geometry, e.g. a mode recolor) also falls back to full, which is correct (every pixel of the shape
 * changes colour).
 *
 * A segment is bounded by its ANCHOR (the pen position it starts from) as well as by its own points: a
 * rotating needle `M cx cy L tip` moves only `tip` in the tape, but the whole line sweeps, so damaging
 * the two tip positions alone repaints the tip and leaves the body stale — the needle's end visibly
 * detaches. Curves lie inside the convex hull of anchor + control points, so the same rule bounds them.
 * An arc is the exception worth keeping tight: its leading segment runs from the pen to the arc's start
 * point, which does not move when only the swept END angle changes (the value-arc case above), so the
 * anchor is folded in only when the arc's start actually moves.
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Opcodes are small integers; a tape value at or past this cannot be one. */
#define VEC_OPCODE_LIMIT 256.0f

/** @brief Antialiasing pad (px) on vector damage, past each stroke's half-width. */
#define VEC_DAMAGE_PAD 1.5f

/**
 * @brief Samples along an arc at this angular step (radians) to bound it — a full turn in
 *        VEC_BBOX_ARC_STEPS samples.
 *
 * Sets how tight the damage around an animated arc is: too coarse and the bbox cuts inside the curve,
 * leaving under-damaged pixels behind a moving needle. 32 samples per turn keeps the chord sag under a
 * pixel for the radii a panel this size can hold.
 */
#define VEC_BBOX_ARC_STEPS 32
#define VEC_BBOX_ARC_STEP (2.0f * ER_PI / (float)VEC_BBOX_ARC_STEPS)

/**
 * @brief Grows a bbox with points sampled along a circle arc [start_angle,end_angle] (radians) — the changed sub-sweep.
 *
 * @return false for an arc no sample can bound (a NaN or infinite input); the caller repaints the whole node.
 */
static bool vec_bbox_arc(float center_x,
                         float center_y,
                         float radius,
                         float start_angle,
                         float end_angle,
                         float* min_x,
                         float* min_y,
                         float* max_x,
                         float* max_y)
{
    float span = end_angle - start_angle;
    if (span < 0.0f)
        span = -span;
    if (!(fabsf(center_x) < INFINITY && fabsf(center_y) < INFINITY && fabsf(radius) < INFINITY && span < INFINITY))
        return false;
    if (span >= 2.0f * ER_PI)
    {
        /* A turn or more sweeps the whole circle, however many turns it is (and past the int range, the sample
         * count's cast would be undefined): bound the circle rather than sample it. */
        const float abs_radius = fabsf(radius);
        if (center_x - abs_radius < *min_x)
            *min_x = center_x - abs_radius;
        if (center_y - abs_radius < *min_y)
            *min_y = center_y - abs_radius;
        if (center_x + abs_radius > *max_x)
            *max_x = center_x + abs_radius;
        if (center_y + abs_radius > *max_y)
            *max_y = center_y + abs_radius;
        return true;
    }
    const int step_count = (int)(span / VEC_BBOX_ARC_STEP) + 1; /* under a turn, so at most VEC_BBOX_ARC_STEPS + 1 */
    for (int step = 0; step <= step_count; step++)
    {
        const float angle = start_angle + (end_angle - start_angle) * (float)step / (float)step_count;
        const float x = center_x + radius * cosf(angle);
        const float y = center_y + radius * sinf(angle);
        if (x < *min_x)
            *min_x = x;
        if (y < *min_y)
            *min_y = y;
        if (x > *max_x)
            *max_x = x;
        if (y > *max_y)
            *max_y = y;
    }
    return true;
}

/**
 * @brief Diffs old vs new op-tape (+ paints) and writes a node-local damage rect of the changed geometry.
 * @return true if a tight rect was computed; false → caller must repaint the full node box.
 */
static bool vec_diff_dirty_rect(const float* old_tape,
                                int old_count,
                                const ERVectorPaint* old_paints,
                                int old_paint_count,
                                const float* new_tape,
                                int new_count,
                                const ERVectorPaint* new_paints,
                                int new_paint_count,
                                int* rect_x,
                                int* rect_y,
                                int* rect_width,
                                int* rect_height)
{
    if (!old_tape || old_count <= 0 || old_count != new_count || old_paint_count != new_paint_count)
        return false;
    if (old_paint_count > 0
        && (!old_paints || !new_paints
            || memcmp(old_paints, new_paints, (size_t)old_paint_count * sizeof(ERVectorPaint)) != 0))
        return false; /* paint table changed → every pixel of affected shapes recolours; full repaint */

    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    bool any = false;
    bool finite = true; /* false once a changed point is NaN or infinite, which no rect can bound */
#define VEC_ADD_POINT(X, Y)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        const float point_x_ = (X), point_y_ = (Y);                                                                    \
        if (!(fabsf(point_x_) < INFINITY && fabsf(point_y_) < INFINITY))                                               \
            finite = false;                                                                                            \
        if (point_x_ < min_x)                                                                                          \
            min_x = point_x_;                                                                                          \
        if (point_y_ < min_y)                                                                                          \
            min_y = point_y_;                                                                                          \
        if (point_x_ > max_x)                                                                                          \
            max_x = point_x_;                                                                                          \
        if (point_y_ > max_y)                                                                                          \
            max_y = point_y_;                                                                                          \
        any = true;                                                                                                    \
    } while (0)

    /* The pen, tracked through BOTH tapes: a moved segment is only bounded once the point it starts
     * from is in the box too. `have_pen` is false until the shape's first MOVE. */
    float old_pen_x = 0.0f, old_pen_y = 0.0f, new_pen_x = 0.0f, new_pen_y = 0.0f;         /* current point */
    float old_start_x = 0.0f, old_start_y = 0.0f, new_start_x = 0.0f, new_start_y = 0.0f; /* subpath start, for CLOSE */
    bool have_pen = false;
#define VEC_ADD_PEN()                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (have_pen)                                                                                                  \
        {                                                                                                              \
            VEC_ADD_POINT(old_pen_x, old_pen_y);                                                                       \
            VEC_ADD_POINT(new_pen_x, new_pen_y);                                                                       \
        }                                                                                                              \
    } while (0)

    int tape_index = 0;
    while (tape_index < old_count)
    {
        if (old_tape[tape_index] != new_tape[tape_index])
            return false; /* opcode / shape structure differs */
        const int code = fabsf(old_tape[tape_index]) < VEC_OPCODE_LIMIT
                             ? (int)old_tape[tape_index]
                             : -1; /* a package's VSET can write any float here */
        if (code == (int)ER_VECTOR_OP_SHAPE)
        {
            tape_index++;
            if (tape_index < old_count)
            {
                if (old_tape[tape_index] != new_tape[tape_index])
                    return false; /* paint-index swap on a shape */
                tape_index++;
            }
            have_pen = false; /* a new shape starts a new pen path */
            continue;
        }
        tape_index++; /* consume opcode (identical in both tapes) */
        const bool pen_moved = have_pen && (old_pen_x != new_pen_x || old_pen_y != new_pen_y);
        if (code == (int)ER_VECTOR_OP_MOVE || code == (int)ER_VECTOR_OP_LINE)
        {
            if (tape_index + 2 > old_count)
                return false;

            const bool changed =
                (old_tape[tape_index] != new_tape[tape_index] || old_tape[tape_index + 1] != new_tape[tape_index + 1]);
            if (changed || (code == (int)ER_VECTOR_OP_LINE && pen_moved))
            {
                if (code == (int)ER_VECTOR_OP_LINE)
                    VEC_ADD_PEN(); /* the segment sweeps from the pen, not just between the endpoints */
                VEC_ADD_POINT(old_tape[tape_index], old_tape[tape_index + 1]);
                VEC_ADD_POINT(new_tape[tape_index], new_tape[tape_index + 1]);
            }
            old_pen_x = old_tape[tape_index];
            old_pen_y = old_tape[tape_index + 1];
            new_pen_x = new_tape[tape_index];
            new_pen_y = new_tape[tape_index + 1];
            if (code == (int)ER_VECTOR_OP_MOVE)
            {
                old_start_x = old_pen_x;
                old_start_y = old_pen_y;
                new_start_x = new_pen_x;
                new_start_y = new_pen_y;
            }
            have_pen = true;
            tape_index += 2;
        }
        else if (code == (int)ER_VECTOR_OP_QUAD)
        {
            if (tape_index + 4 > old_count)
                return false;
            bool changed = pen_moved; /* the pen is the curve's first hull point: moving it moves the curve */
            for (int offset = 0; offset < 4; offset++)
                if (old_tape[tape_index + offset] != new_tape[tape_index + offset])
                    changed = true;
            if (changed)
            {
                VEC_ADD_PEN(); /* the curve lies inside the hull of pen + control points */
                VEC_ADD_POINT(old_tape[tape_index], old_tape[tape_index + 1]);
                VEC_ADD_POINT(old_tape[tape_index + 2], old_tape[tape_index + 3]);
                VEC_ADD_POINT(new_tape[tape_index], new_tape[tape_index + 1]);
                VEC_ADD_POINT(new_tape[tape_index + 2], new_tape[tape_index + 3]);
            }
            old_pen_x = old_tape[tape_index + 2];
            old_pen_y = old_tape[tape_index + 3];
            new_pen_x = new_tape[tape_index + 2];
            new_pen_y = new_tape[tape_index + 3];
            have_pen = true;
            tape_index += 4;
        }
        else if (code == (int)ER_VECTOR_OP_CUBIC)
        {
            if (tape_index + 6 > old_count)
                return false;
            bool changed = pen_moved;
            for (int offset = 0; offset < 6; offset++)
                if (old_tape[tape_index + offset] != new_tape[tape_index + offset])
                    changed = true;
            if (changed)
            {
                VEC_ADD_PEN();
                for (int offset = 0; offset < 6; offset += 2)
                {
                    VEC_ADD_POINT(old_tape[tape_index + offset], old_tape[tape_index + offset + 1]);
                    VEC_ADD_POINT(new_tape[tape_index + offset], new_tape[tape_index + offset + 1]);
                }
            }
            old_pen_x = old_tape[tape_index + 4];
            old_pen_y = old_tape[tape_index + 5];
            new_pen_x = new_tape[tape_index + 4];
            new_pen_y = new_tape[tape_index + 5];
            have_pen = true;
            tape_index += 6;
        }
        else if (code == (int)ER_VECTOR_OP_ARC)
        {
            if (tape_index + 6 > old_count)
                return false;
            const float old_center_x = old_tape[tape_index], old_center_y = old_tape[tape_index + 1],
                        old_radius = old_tape[tape_index + 2], old_start_angle = old_tape[tape_index + 3],
                        old_end_angle = old_tape[tape_index + 4], old_counter_clockwise = old_tape[tape_index + 5];
            const float new_center_x = new_tape[tape_index], new_center_y = new_tape[tape_index + 1],
                        new_radius = new_tape[tape_index + 2], new_start_angle = new_tape[tape_index + 3],
                        new_end_angle = new_tape[tape_index + 4], new_counter_clockwise = new_tape[tape_index + 5];
            const bool same_circle = (old_center_x == new_center_x && old_center_y == new_center_y
                                      && old_radius == new_radius && old_counter_clockwise == new_counter_clockwise);
            const bool arc_moved = !same_circle || old_start_angle != new_start_angle || old_end_angle != new_end_angle;
            const bool lead_in_moved = pen_moved || !same_circle || old_start_angle != new_start_angle;
            if (arc_moved || pen_moved)
            {
                if (same_circle)
                {
                    /* Same circle, only swept angles moved (the value arc) → damage just the changed
                     * sub-arcs, which is what keeps a dial's repaint small. */
                    if (lead_in_moved)
                    {
                        VEC_ADD_PEN();
                        VEC_ADD_POINT(old_center_x + old_radius * cosf(old_start_angle),
                                      old_center_y + old_radius * sinf(old_start_angle));
                        VEC_ADD_POINT(new_center_x + new_radius * cosf(new_start_angle),
                                      new_center_y + new_radius * sinf(new_start_angle));
                    }
                    if (old_start_angle != new_start_angle
                        && !vec_bbox_arc(old_center_x,
                                         old_center_y,
                                         old_radius,
                                         old_start_angle,
                                         new_start_angle,
                                         &min_x,
                                         &min_y,
                                         &max_x,
                                         &max_y))
                        return false;
                    if (old_end_angle != new_end_angle
                        && !vec_bbox_arc(old_center_x,
                                         old_center_y,
                                         old_radius,
                                         old_end_angle,
                                         new_end_angle,
                                         &min_x,
                                         &min_y,
                                         &max_x,
                                         &max_y))
                        return false;
                    any = true;
                }
                else
                {
                    /* Centre/radius moved (e.g. the handle knob) → full-circle bbox of old + new. */
                    VEC_ADD_PEN();
                    VEC_ADD_POINT(old_center_x - old_radius, old_center_y - old_radius);
                    VEC_ADD_POINT(old_center_x + old_radius, old_center_y + old_radius);
                    VEC_ADD_POINT(new_center_x - new_radius, new_center_y - new_radius);
                    VEC_ADD_POINT(new_center_x + new_radius, new_center_y + new_radius);
                }
            }
            old_pen_x = old_center_x + old_radius * cosf(old_end_angle);
            old_pen_y = old_center_y + old_radius * sinf(old_end_angle);
            new_pen_x = new_center_x + new_radius * cosf(new_end_angle);
            new_pen_y = new_center_y + new_radius * sinf(new_end_angle);
            have_pen = true;
            tape_index += 6;
        }
        else if (code == (int)ER_VECTOR_OP_CLOSE)
        {
            /* No coordinates of its own, but it draws pen -> subpath start, which sweeps when either end
             * moved — the `Z` on a rotating `M cx cy L tip Z` needle. */
            if (have_pen
                && (old_pen_x != new_pen_x || old_pen_y != new_pen_y || old_start_x != new_start_x
                    || old_start_y != new_start_y))
            {
                VEC_ADD_POINT(old_pen_x, old_pen_y);
                VEC_ADD_POINT(new_pen_x, new_pen_y);
                VEC_ADD_POINT(old_start_x, old_start_y);
                VEC_ADD_POINT(new_start_x, new_start_y);
            }
            old_pen_x = old_start_x;
            old_pen_y = old_start_y;
            new_pen_x = new_start_x;
            new_pen_y = new_start_y;
        }
        else
        {
            return false; /* unknown opcode — bail to a full repaint */
        }
    }
#undef VEC_ADD_PEN
#undef VEC_ADD_POINT

    if (!finite)
        return false; /* a NaN or infinite point is bounded by nothing: repaint the whole node */
    if (!any)
        return false; /* geometry identical (paint-only or no-op change) → full box is simplest & correct */

    /* Pad by the widest stroke half-width + AA so round caps and coverage fringes are covered. */
    float fringe = VEC_DAMAGE_PAD;
    for (int paint_index = 0; paint_index < new_paint_count; paint_index++)
    {
        const float stroke_width = new_paints[paint_index].stroke_width;
        if (stroke_width > 0.0f)
        {
            const float reach = stroke_width * 0.5f + VEC_DAMAGE_PAD;
            if (reach > fringe)
                fringe = reach;
        }
    }
    min_x -= fringe;
    min_y -= fringe;
    max_x += fringe;
    max_y += fringe;
    /* The tape is app geometry: a rect past ±ER_PAINT_RECT_MAX cannot be kept in the node's int16 fields, and
     * one past the int range (or NaN) cannot even be converted. Repaint the whole node instead, which is always
     * right. */
    const float limit = (float)ER_PAINT_RECT_MAX;
    if (!(min_x >= -limit && max_x <= limit && min_y >= -limit && max_y <= limit && min_x <= max_x && min_y <= max_y))
        return false;
    *rect_x = (int)floorf(min_x);
    *rect_y = (int)floorf(min_y);
    *rect_width = (int)ceilf(max_x) - *rect_x;
    *rect_height = (int)ceilf(max_y) - *rect_y;
    return (*rect_width > 0 && *rect_height > 0);
}

void er_node_set_vector_ops(ERNode* node,
                            const float* ops,
                            int op_count,
                            const ERVectorPaint* paints,
                            int paint_count,
                            const ERVectorGradient* gradients,
                            int gradient_count)
{
    if (!node || node->type != ER_NODE_VECTOR)
        return;
    /* Default to a full-box repaint; the diff below (or er_node_set_vector_dirty_rect, if called after) narrows it. */
    node->vector_has_dirty = false;
    if (!ops || op_count <= 0)
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
    int dirty_x = 0, dirty_y = 0, dirty_width = 0, dirty_height = 0;
    bool tight = false;
    if (node->vector_slot >= 0)
    {
        int old_op_count = 0, old_paint_count = 0, old_gradient_count = 0;
        const float* old_ops = er_vector_slot_ops(node->vector_slot, &old_op_count);
        const ERVectorPaint* old_paints = er_vector_slot_paints(node->vector_slot, &old_paint_count);
        const ERVectorGradient* old_gradients = er_vector_slot_grads(node->vector_slot, &old_gradient_count);
        const bool same_gradients =
            old_gradient_count == gradient_count
            && (gradient_count <= 0
                || (old_gradients && gradients
                    && memcmp(old_gradients, gradients, (size_t)gradient_count * sizeof(ERVectorGradient)) == 0));
        /* Identical re-upload (e.g. a held finger below the drag deadband re-running a handler with the same
         * state) → nothing changed, so skip the repaint entirely. */
        if (old_ops && old_op_count == op_count && old_paint_count == paint_count
            && memcmp(old_ops, ops, (size_t)op_count * sizeof(float)) == 0
            && (paint_count <= 0
                || (old_paints && paints
                    && memcmp(old_paints, paints, (size_t)paint_count * sizeof(ERVectorPaint)) == 0))
            && same_gradients)
        {
            return;
        }
        /* The diff sees only ops and paints. A changed gradient recolours every shape that uses it, wherever the
         * geometry changed, so it repaints the whole node. */
        tight = same_gradients
                && vec_diff_dirty_rect(old_ops,
                                       old_op_count,
                                       old_paints,
                                       old_paint_count,
                                       ops,
                                       op_count,
                                       paints,
                                       paint_count,
                                       &dirty_x,
                                       &dirty_y,
                                       &dirty_width,
                                       &dirty_height);
    }

    node->vector_slot =
        er_vector_store(node->vector_slot, ops, op_count, paints, paint_count, gradients, gradient_count);
    if (tight)
    {
        clip_rect_axis(&dirty_x, &dirty_width);
        clip_rect_axis(&dirty_y, &dirty_height);
        node->vector_dirty_x = (int16_t)dirty_x;
        node->vector_dirty_y = (int16_t)dirty_y;
        node->vector_dirty_width = (int16_t)dirty_width;
        node->vector_dirty_height = (int16_t)dirty_height;
        node->vector_has_dirty = true;
    }
    /* Geometry is visual-only (the box comes from layout/style), so no layout pass is needed. */
    er_mark_dirty_upward(node);
}

void er_node_set_vector_dirty_rect(ERNode* node, int x, int y, int width, int height)
{
    if (!node || node->type != ER_NODE_VECTOR)
        return;
    clip_rect_axis(&x, &width);
    clip_rect_axis(&y, &height);
    node->vector_dirty_x = (int16_t)x;
    node->vector_dirty_y = (int16_t)y;
    node->vector_dirty_width = (int16_t)width;
    node->vector_dirty_height = (int16_t)height;
    node->vector_has_dirty = true;
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

    const size_t length = strlen(node->input_text);
    bool changed = false;

    if (keycode == ER_KEY_BACKSPACE || keycode == ER_KEY_DELETE)
    {
        if (length > 0)
        {
            /* Remove the last UTF-8 character (back up past any continuation bytes). */
            size_t end = length - 1;
            while (end > 0 && ((unsigned char)node->input_text[end] & 0xC0U) == 0x80U)
                end--;
            node->input_text[end] = '\0';
            changed = true;
        }
    }
    else if (keycode == ER_KEY_RETURN)
    {
        const EREventHandler* handler = &node->events[ER_EVENT_SUBMIT_EDITING];
        if (handler->callback)
        {
            EREventData data = {0};
            data.changed_text = node->input_text;
            handler->callback(node, &data, handler->user_data);
        }
    }
    else if (keycode == ER_KEY_ESCAPE)
    {
        er_text_input_blur();
    }
    else if (utf8_char && utf8_char[0] != '\0')
    {
        /* Append the UTF-8 character if there is room. */
        const size_t char_length = strlen(utf8_char);
        if (length + char_length < ER_TEXT_MAX)
        {
            memcpy(node->input_text + length, utf8_char, char_length);
            node->input_text[length + char_length] = '\0';
            changed = true;
        }
    }

    if (changed)
    {
        er_mark_dirty_upward(node);
        const EREventHandler* handler = &node->events[ER_EVENT_CHANGE_TEXT];
        if (handler->callback)
        {
            EREventData data = {0};
            data.changed_text = node->input_text;
            handler->callback(node, &data, handler->user_data);
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
#define KEY_CHAR(text) {(text), (text), ER_KEYBOARD_KEY_CHAR, 0, 2, 0xFFU} /* char key, span 2 */
#define KEY_SPACE {NULL, " ", ER_KEYBOARD_KEY_CHAR, 0, 12, 0xFFU}          /* blank space bar */
#define KEY_BACKSPACE {"<", NULL, ER_KEYBOARD_KEY_BACKSPACE, 0, 3, 0xFFU}
#define KEY_DONE {"OK", NULL, ER_KEYBOARD_KEY_DONE, 0, 4, 0xFFU}
#define KEY_SWITCH_ROW2(label, target, highlight)                                                                      \
    {(label), NULL, ER_KEYBOARD_KEY_LAYER, (target), 3, (highlight)} /* row-2 left switch */
#define KEY_SWITCH_ROW3(label, target)                                                                                 \
    {(label), NULL, ER_KEYBOARD_KEY_LAYER, (target), 4, 0xFFU} /* row-3 left switch */

static const ERKeyboardKey s_lower_row0[] = {KEY_CHAR("q"),
                                             KEY_CHAR("w"),
                                             KEY_CHAR("e"),
                                             KEY_CHAR("r"),
                                             KEY_CHAR("t"),
                                             KEY_CHAR("y"),
                                             KEY_CHAR("u"),
                                             KEY_CHAR("i"),
                                             KEY_CHAR("o"),
                                             KEY_CHAR("p")};
static const ERKeyboardKey s_lower_row1[] = {KEY_CHAR("a"),
                                             KEY_CHAR("s"),
                                             KEY_CHAR("d"),
                                             KEY_CHAR("f"),
                                             KEY_CHAR("g"),
                                             KEY_CHAR("h"),
                                             KEY_CHAR("j"),
                                             KEY_CHAR("k"),
                                             KEY_CHAR("l")};
static const ERKeyboardKey s_lower_row2[] = {KEY_SWITCH_ROW2("^", 1, 1),
                                             KEY_CHAR("z"),
                                             KEY_CHAR("x"),
                                             KEY_CHAR("c"),
                                             KEY_CHAR("v"),
                                             KEY_CHAR("b"),
                                             KEY_CHAR("n"),
                                             KEY_CHAR("m"),
                                             KEY_BACKSPACE};
static const ERKeyboardKey s_letters_row3[] = {KEY_SWITCH_ROW3("123", 2), KEY_SPACE, KEY_DONE};
static const ERKeyboardRow s_lower_rows[] = {
    {s_lower_row0, 10}, {s_lower_row1, 9}, {s_lower_row2, 9}, {s_letters_row3, 3}};

static const ERKeyboardKey s_upper_row0[] = {KEY_CHAR("Q"),
                                             KEY_CHAR("W"),
                                             KEY_CHAR("E"),
                                             KEY_CHAR("R"),
                                             KEY_CHAR("T"),
                                             KEY_CHAR("Y"),
                                             KEY_CHAR("U"),
                                             KEY_CHAR("I"),
                                             KEY_CHAR("O"),
                                             KEY_CHAR("P")};
static const ERKeyboardKey s_upper_row1[] = {KEY_CHAR("A"),
                                             KEY_CHAR("S"),
                                             KEY_CHAR("D"),
                                             KEY_CHAR("F"),
                                             KEY_CHAR("G"),
                                             KEY_CHAR("H"),
                                             KEY_CHAR("J"),
                                             KEY_CHAR("K"),
                                             KEY_CHAR("L")};
static const ERKeyboardKey s_upper_row2[] = {KEY_SWITCH_ROW2("^", 0, 1),
                                             KEY_CHAR("Z"),
                                             KEY_CHAR("X"),
                                             KEY_CHAR("C"),
                                             KEY_CHAR("V"),
                                             KEY_CHAR("B"),
                                             KEY_CHAR("N"),
                                             KEY_CHAR("M"),
                                             KEY_BACKSPACE};
static const ERKeyboardRow s_upper_rows[] = {
    {s_upper_row0, 10}, {s_upper_row1, 9}, {s_upper_row2, 9}, {s_letters_row3, 3}};

static const ERKeyboardKey s_number_row0[] = {KEY_CHAR("1"),
                                              KEY_CHAR("2"),
                                              KEY_CHAR("3"),
                                              KEY_CHAR("4"),
                                              KEY_CHAR("5"),
                                              KEY_CHAR("6"),
                                              KEY_CHAR("7"),
                                              KEY_CHAR("8"),
                                              KEY_CHAR("9"),
                                              KEY_CHAR("0")};
static const ERKeyboardKey s_number_row1[] = {KEY_CHAR("-"),
                                              KEY_CHAR("/"),
                                              KEY_CHAR(":"),
                                              KEY_CHAR(";"),
                                              KEY_CHAR("("),
                                              KEY_CHAR(")"),
                                              KEY_CHAR("$"),
                                              KEY_CHAR("&"),
                                              KEY_CHAR("@"),
                                              KEY_CHAR("\"")};
static const ERKeyboardKey s_number_row2[] = {KEY_SWITCH_ROW2("#+=", 3, 0xFFU),
                                              KEY_CHAR("."),
                                              KEY_CHAR(","),
                                              KEY_CHAR("?"),
                                              KEY_CHAR("!"),
                                              KEY_CHAR("'"),
                                              KEY_BACKSPACE};
static const ERKeyboardKey s_symbols_row3[] = {KEY_SWITCH_ROW3("ABC", 0), KEY_SPACE, KEY_DONE};
static const ERKeyboardRow s_number_rows[] = {
    {s_number_row0, 10}, {s_number_row1, 10}, {s_number_row2, 7}, {s_symbols_row3, 3}};

static const ERKeyboardKey s_symbol_row0[] = {KEY_CHAR("["),
                                              KEY_CHAR("]"),
                                              KEY_CHAR("{"),
                                              KEY_CHAR("}"),
                                              KEY_CHAR("#"),
                                              KEY_CHAR("%"),
                                              KEY_CHAR("^"),
                                              KEY_CHAR("*"),
                                              KEY_CHAR("+"),
                                              KEY_CHAR("=")};
static const ERKeyboardKey s_symbol_row1[] = {KEY_CHAR("_"),
                                              KEY_CHAR("\\"),
                                              KEY_CHAR("|"),
                                              KEY_CHAR("~"),
                                              KEY_CHAR("<"),
                                              KEY_CHAR(">"),
                                              KEY_CHAR("$"),
                                              KEY_CHAR("`")};
static const ERKeyboardKey s_symbol_row2[] = {KEY_SWITCH_ROW2("123", 2, 0xFFU),
                                              KEY_CHAR("."),
                                              KEY_CHAR(","),
                                              KEY_CHAR("?"),
                                              KEY_CHAR("!"),
                                              KEY_CHAR("'"),
                                              KEY_BACKSPACE};
static const ERKeyboardRow s_symbol_rows[] = {
    {s_symbol_row0, 10}, {s_symbol_row1, 8}, {s_symbol_row2, 7}, {s_symbols_row3, 3}};

#undef KEY_CHAR
#undef KEY_SPACE
#undef KEY_BACKSPACE
#undef KEY_DONE
#undef KEY_SWITCH_ROW2
#undef KEY_SWITCH_ROW3

static const ERKeyboardLayer s_keyboard_default_layers[4] = {
    {s_lower_rows, 4}, {s_upper_rows, 4}, {s_number_rows, 4}, {s_symbol_rows, 4}};
static const ERKeyboardConfig s_keyboard_default_config = {s_keyboard_default_layers, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const ERKeyboardConfig* s_keyboard_config = NULL; /**< app override; NULL = built-in default */

/** @brief The active keyboard config (app-supplied or the built-in default). */
static const ERKeyboardConfig* er_kbd_cfg(void)
{
    return s_keyboard_config ? s_keyboard_config : &s_keyboard_default_config;
}

/** @brief The active layout — falls back to the built-in QWERTY when a config leaves `layers` NULL, so an
 *         app can override only colours/sizes and keep the default keys. Returns the layer array + count. */
static const ERKeyboardLayer* er_kbd_layers(uint8_t* count)
{
    const ERKeyboardConfig* config = er_kbd_cfg();
    if (config->layers && config->layer_count)
    {
        *count = config->layer_count;
        return config->layers;
    }
    *count = 4;
    return s_keyboard_default_layers;
}

void er_keyboard_set_config(const ERKeyboardConfig* config)
{
    s_keyboard_config = config;
    s_keyboard_layer = 0;
    s_keyboard_dirty = true;
}

bool er_keyboard_active(void)
{
    return s_focused_input_tag != ER_INVALID_TAG;
}

/** @brief Largest row count across the config's layers (so the strip rect is stable across layer switches). */
static int er_kbd_rows(void)
{
    uint8_t layer_count;
    const ERKeyboardLayer* layers = er_kbd_layers(&layer_count);
    int max_rows = 0;
    for (uint8_t layer_index = 0; layer_index < layer_count; layer_index++)
        if ((int)layers[layer_index].count > max_rows)
            max_rows = layers[layer_index].count;
    return max_rows ? max_rows : 1;
}

/* Keyboard row sizing, in the order the clamps apply. The default divisor leaves a four-row keyboard a
 * little over a third of the screen, which is roughly where a phone keyboard sits. The floor is a
 * fingertip: below it the keys stop being reliably hittable on the small panels this runs on, and it
 * wins over the divisor on a short screen. The ceiling then wins over both — a keyboard past half the
 * screen leaves nothing to type into. */
#define ER_KEYBOARD_ROW_HEIGHT_DIVISOR 11
#define ER_KEYBOARD_ROW_HEIGHT_MIN 24
#define ER_KEYBOARD_MAX_SCREEN_FRACTION 2 /* strip height <= screen_height / this */

/* Defaults for the ERKeyboardConfig fields left 0. */
#define ER_KEYBOARD_GRID_COLUMNS 20
#define ER_KEYBOARD_KEY_GAP 2
#define ER_KEYBOARD_KEY_RADIUS 5
#define ER_KEYBOARD_FONT_SIZE 16
#define ER_KEYBOARD_KEY_COLOR 0xFF2A3340U
#define ER_KEYBOARD_KEY_ACTIVE_COLOR 0xFF4A6488U
#define ER_KEYBOARD_LABEL_COLOR 0xFFE7EDF5U
#define ER_KEYBOARD_PANEL_COLOR 0xFF12181FU

/** @brief Gap (px) kept between a focused input and the keyboard strip above it. */
#define ER_KEYBOARD_INPUT_CLEARANCE 8

/** @brief Per-row height: configured, else a fraction of the screen; floored for touch, capped for room. */
static int er_kbd_row_h(int screen_height)
{
    const ERKeyboardConfig* config = er_kbd_cfg();
    int row_height =
        config->row_height_px ? (int)config->row_height_px : (screen_height / ER_KEYBOARD_ROW_HEIGHT_DIVISOR);
    if (row_height < ER_KEYBOARD_ROW_HEIGHT_MIN)
        row_height = ER_KEYBOARD_ROW_HEIGHT_MIN;
    const int rows = er_kbd_rows();
    const int max_strip = screen_height / ER_KEYBOARD_MAX_SCREEN_FRACTION;
    if (row_height * rows > max_strip)
        row_height = max_strip / rows;
    return row_height;
}

/** @brief Bottom strip the keyboard occupies. */
static void er_keyboard_rect(int screen_width, int screen_height, ERRect* out)
{
    const int strip_height = er_kbd_row_h(screen_height) * er_kbd_rows();
    out->x = 0;
    out->y = screen_height - strip_height;
    out->width = screen_width;
    out->height = strip_height;
}

/** One laid-out key: its screen rect, the source key descriptor, and whether to draw it highlighted. */
typedef struct
{
    int x, y, width, height;
    const ERKeyboardKey* key;
    bool highlight;
} ERKbdHit1;
typedef void (*er_kbd_key_fn)(const ERKbdHit1* key, void* user_data);

/** Invokes `fn` for every key of the active layer, positioned span-proportionally and centred per row. */
static void er_kbd_foreach(int screen_width, int screen_height, er_kbd_key_fn callback, void* user_data)
{
    const ERKeyboardConfig* config = er_kbd_cfg();
    uint8_t layer_count;
    const ERKeyboardLayer* layers = er_kbd_layers(&layer_count);
    if (s_keyboard_layer >= layer_count)
        return;
    ERRect strip;
    er_keyboard_rect(screen_width, screen_height, &strip);
    const int grid = config->grid_columns ? (int)config->grid_columns : ER_KEYBOARD_GRID_COLUMNS;
    const int column_width = strip.width / grid;
    const int row_height = er_kbd_row_h(screen_height);
    const int gap = config->key_gap_px ? (int)config->key_gap_px : ER_KEYBOARD_KEY_GAP;
    const ERKeyboardLayer* layer = &layers[s_keyboard_layer];

    for (uint8_t row_index = 0; row_index < layer->count; row_index++)
    {
        const ERKeyboardRow* row = &layer->rows[row_index];
        int total = 0;
        for (uint8_t key_index = 0; key_index < row->count; key_index++)
            total += row->keys[key_index].span ? row->keys[key_index].span : 1;
        int column = (grid - total) / 2;
        if (column < 0)
            column = 0;
        for (uint8_t key_index = 0; key_index < row->count; key_index++)
        {
            const ERKeyboardKey* key = &row->keys[key_index];
            const int span = key->span ? key->span : 1;
            ERKbdHit1 hit;
            hit.x = strip.x + column * column_width + gap;
            hit.y = strip.y + row_index * row_height + gap;
            hit.width = span * column_width - 2 * gap;
            hit.height = row_height - 2 * gap;
            hit.key = key;
            hit.highlight = (key->type == ER_KEYBOARD_KEY_LAYER && key->highlight_layer == s_keyboard_layer);
            callback(&hit, user_data);
            column += span;
        }
    }
}

/** Draws one key: a rounded fill (brighter when highlighted) + its centred label. */
static void er_kbd_draw_key(const ERKbdHit1* hit, void* user_data)
{
    const ERKeyboardConfig* config = (const ERKeyboardConfig*)user_data;
    const uint32_t key_color = config->key_color ? config->key_color : ER_KEYBOARD_KEY_COLOR;
    const uint32_t active_color = config->key_active_color ? config->key_active_color : ER_KEYBOARD_KEY_ACTIVE_COLOR;
    const uint32_t label_color = config->label_color ? config->label_color : ER_KEYBOARD_LABEL_COLOR;
    const int radius = config->key_radius_px ? (int)config->key_radius_px : ER_KEYBOARD_KEY_RADIUS;
    const int font_size = config->font_size_px ? (int)config->font_size_px : ER_KEYBOARD_FONT_SIZE;
    er_rrect_fill_bordered(
        hit->highlight ? active_color : key_color, 0xFF000000U, 0, hit->x, hit->y, hit->width, hit->height, radius);
    const char* label = hit->key->label ? hit->key->label : hit->key->text;
    if (label && label[0] && label[0] != ' ') /* the space bar (text " ") stays blank */
    {
        ERTextRenderParams params;
        memset(&params, 0, sizeof(params));
        params.text = label;
        /* Keep the glyph top where it reads centred, but extend the clip to the key's bottom edge so
         * descenders (p g q y j) are not cut. */
        const int top = hit->y + (hit->height - font_size) / 2;
        params.clip = (ERRect){hit->x, top, hit->width, (hit->y + hit->height) - top};
        params.color = label_color;
        params.font_size = (uint16_t)font_size;
        params.text_align = ER_TEXT_ALIGN_CENTER;
        params.number_of_lines = 1;
        er_text_render(&params);
    }
}

void er_keyboard_draw(int screen_width, int screen_height)
{
    if (!er_keyboard_active())
        return;
    const ERKeyboardConfig* config = er_kbd_cfg();
    ERRect strip;
    er_keyboard_rect(screen_width, screen_height, &strip);
    er_blit_fill(config->panel_color ? config->panel_color : ER_KEYBOARD_PANEL_COLOR,
                 strip.x,
                 strip.y,
                 strip.width,
                 strip.height);
    er_kbd_foreach(screen_width, screen_height, er_kbd_draw_key, (void*)config);
}

/** Hit-test accumulator: the first key under (x, y). */
typedef struct
{
    int x, y;
    const ERKeyboardKey* key;
} ERKbdHit;

static void er_kbd_hit_one(const ERKbdHit1* hit, void* user_data)
{
    ERKbdHit* search = (ERKbdHit*)user_data;
    if (search->key)
        return;
    if (search->x >= hit->x && search->x < hit->x + hit->width && search->y >= hit->y
        && search->y < hit->y + hit->height)
        search->key = hit->key;
}

bool er_keyboard_dispatch_touch(ERTouchPhase phase, int x, int y)
{
    if (!er_keyboard_active())
        return false;
    ERNode* root = er_get_node(s_root_tag);
    if (!root)
        return false;
    const int screen_width = (int)root->computed.width;
    const int screen_height = (int)root->computed.height;
    ERRect strip;
    er_keyboard_rect(screen_width, screen_height, &strip);
    if (x < strip.x || x >= strip.x + strip.width || y < strip.y || y >= strip.y + strip.height)
        return false; /* outside the keyboard → let the scene handle the touch */
    if (phase == ER_TOUCH_DOWN)
    {
        ERKbdHit search;
        search.x = x;
        search.y = y;
        search.key = NULL;
        er_kbd_foreach(screen_width, screen_height, er_kbd_hit_one, &search);
        if (search.key)
        {
            switch (search.key->type)
            {
                case ER_KEYBOARD_KEY_LAYER:
                    s_keyboard_layer = search.key->layer;
                    s_keyboard_dirty = true; /* the whole keyboard relabels → repaint its strip next commit */
                    break;
                case ER_KEYBOARD_KEY_BACKSPACE:
                    er_text_input_key(ER_KEY_BACKSPACE, NULL);
                    break;
                case ER_KEYBOARD_KEY_DONE:
                {
                    /* Submit, then blur, unless the app's handler moved the focus itself. A ref, since a node
                       made in the handler can reuse a destroyed input's tag. */
                    const ERNodeRef submitted = er_node_ref(er_get_node(s_focused_input_tag));
                    er_text_input_key(ER_KEY_RETURN, NULL);
                    const ERNode* still_there = er_node_deref(submitted);
                    if (still_there && still_there->tag == s_focused_input_tag)
                    {
                        er_text_input_key(ER_KEY_ESCAPE, NULL);
                    }
                    break;
                }
                case ER_KEYBOARD_KEY_CHAR:
                default:
                    er_text_input_key(0, search.key->text ? search.key->text : "");
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
void er_keyboard_draw(int screen_width, int screen_height)
{
    (void)screen_width;
    (void)screen_height;
}
bool er_keyboard_dispatch_touch(ERTouchPhase phase, int x, int y)
{
    (void)phase;
    (void)x;
    (void)y;
    return false;
}
void er_keyboard_set_config(const ERKeyboardConfig* config)
{
    (void)config;
}

#endif /* ERUI_ONSCREEN_KEYBOARD */

/** @brief Pixels the scene is currently shifted up for keyboard avoidance (0 when none). Hit-testing adds
 *         this to a touch's Y to map a screen point back to the shifted scene. Always defined (0 when the
 *         keyboard is compiled out), so hit_test.c can call it unconditionally. */
int er_keyboard_avoid_offset(void)
{
    return s_keyboard_avoid_y;
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
        uint16_t sibling_tag = parent->first_child_tag;
        while (sibling_tag != ER_INVALID_TAG)
        {
            ERNode* sibling = er_get_node(sibling_tag);
            if (!sibling)
                break;
            if (sibling->next_sibling_tag == child->tag)
            {
                sibling->next_sibling_tag = child->next_sibling_tag;
                break;
            }
            sibling_tag = sibling->next_sibling_tag;
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
        uint16_t sibling_tag = parent->first_child_tag;
        while (sibling_tag != ER_INVALID_TAG)
        {
            ERNode* sibling = er_get_node(sibling_tag);
            if (!sibling)
                break;
            if (sibling->next_sibling_tag == ER_INVALID_TAG)
            {
                sibling->next_sibling_tag = child->tag;
                break;
            }
            sibling_tag = sibling->next_sibling_tag;
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
        uint16_t sibling_tag = parent->first_child_tag;
        while (sibling_tag != ER_INVALID_TAG)
        {
            ERNode* sibling = er_get_node(sibling_tag);
            if (!sibling)
                break;
            if (sibling->next_sibling_tag == before->tag)
            {
                child->next_sibling_tag = before->tag;
                sibling->next_sibling_tag = child->tag;
                break;
            }
            sibling_tag = sibling->next_sibling_tag;
        }
        /* `before` is not a child of parent → append instead. */
        if (sibling_tag == ER_INVALID_TAG)
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

void er_set_screen_bounds(int width, int height)
{
    /* A size past int16 is the largest limit there is. 0 would mean no limit at all. */
    s_bounds_width = width <= 0 ? 0 : width > INT16_MAX ? INT16_MAX : (int16_t)width;
    s_bounds_height = height <= 0 ? 0 : height > INT16_MAX ? INT16_MAX : (int16_t)height;
    mark_layout_dirty();
    er_force_full_repaint();
}

void er_reset(void)
{
    fade_cache_invalidate();
    /* Empty the node pool and clear the scene root. Every slot is marked unused, not just the allocator
     * rewound: the per-frame sweeps walk the pool by in_use, and a tag or ERNodeRef held from before the
     * reset (by a handler that reset the scene mid-dispatch, say) must stop resolving to its old node. */
    for (int tag = 0; tag < (int)ERUI_MAX_NODES; tag++)
        s_nodes[tag].in_use = false;
    s_next_tag = 0;
    s_free_count = 0;
    s_parallel_unsafe = 0; /* the old scene's vectors, arcs and shadow casters are gone with it */
    s_root_tag = ER_INVALID_TAG;
    s_focused_input_tag = ER_INVALID_TAG;
    s_last_cursor_phase = ER_CURSOR_PHASE_UNKNOWN;

    /* Drop dirty/damage tracking and force the next commit to fully repaint and re-run layout. */
    s_has_dirty = false;
    for (int worker = 0; worker < ERUI_RENDER_WORKERS; worker++)
        s_composite_contexts[worker].has_dirty = false;
    er_damage_set_clear(&s_removed_set);
    er_damage_set_clear(&s_last_paint_set);
    s_force_full_repaint = true;
    s_layout_dirty = true;
    s_subtree_bounds_dirty = false; /* the next commit's layout pass recomputes them */

    /* Reset the multi-buffer debt: every rotating buffer owes a full frame again (new scene). */
    s_current_buffer = 0;
    debt_reset_all_full();

    /* Reset the per-scene subsystems. The render backend, registered images, and registered/built-in
     * fonts are kept; the monotonic clock (s_now_ms) is preserved so time never runs backwards across
     * a reload. */
    er_animation_reset();
    er_layout_animation_reset();
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
    int left;              /**< Repaint region bounds (screen space). */
    int right;
    int top;
    int bottom;
    int slice_count;
    int keyboard_avoid_y;
} ParallelRenderJob;

/** @brief Fewest rows per worker that is worth splitting a repaint for. */
#define ER_PARALLEL_MIN_ROWS 16

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
    const ParallelRenderJob* job = (const ParallelRenderJob*)arg;
    const int rows = job->bottom - job->top;
    const int slice_top = job->top + rows * worker / job->slice_count;
    const int slice_bottom = job->top + rows * (worker + 1) / job->slice_count;
    if (slice_top >= slice_bottom)
        return;
    er_push_clip_rect(job->left, slice_top, job->right - job->left, slice_bottom - slice_top);
    render_tree(job->root, job->full_recomposite, false, 0, job->keyboard_avoid_y);
    er_pop_clip_rect();
}

/**
 * @brief er_commit() while the error screen is up: paints it into each display buffer once, in strips on a
 *        banded backend, and reports the whole screen as this commit's repaint.
 */
static void commit_error_screen(void)
{
    if (s_force_full_repaint)
    {
        debt_reset_all_full();
        s_force_full_repaint = false;
    }
    ERDamageSlot* debt = &s_buffer_debt[s_display_buffer_count > 1 ? s_current_buffer : 0];
    if (!debt->full)
        return;

    int screen_width = 0;
    int screen_height = 0;
    er_error_screen_size(&screen_width, &screen_height);
    const EmbeddedRenderBackend* backend = er_backend();
    if (backend && backend->band_height > 0)
    {
        for (int strip_top = 0; strip_top < screen_height; strip_top += backend->band_height)
        {
            const int strip_height =
                screen_height - strip_top < backend->band_height ? screen_height - strip_top : backend->band_height;
            if (backend->band_begin)
                backend->band_begin(0, strip_top, screen_width, strip_height, backend->context);
            er_set_band(strip_top, strip_height);
            er_error_screen_draw();
            if (backend->band_flush)
                backend->band_flush(backend->context);
        }
        er_set_band(0, 0);
    }
    else
    {
        er_error_screen_draw();
    }

    debt->full = false;
    er_damage_set_clear(&debt->set);
    s_dirty_rect.x = 0;
    s_dirty_rect.y = 0;
    s_dirty_rect.width = screen_width;
    s_dirty_rect.height = screen_height;
    s_has_dirty = screen_width > 0 && screen_height > 0;
    er_damage_set_clear(&s_last_paint_set);
    er_damage_set_add(&s_last_paint_set, 0, 0, screen_width, screen_height);
}

void er_commit(void)
{
    if (er_error_screen_active())
    {
        commit_error_screen();
        return;
    }

    er_input_flush_moves();

    if (s_root_tag == ER_INVALID_TAG)
        return;

    ERNode* root = er_get_node(s_root_tag);
    if (!root)
        return;

    s_commit_sequence++; /* per-commit sequence for the fade cache's one-capture-per-commit gate */

    /* Reset every worker's dirty-rect accumulator for this commit. The PUBLISHED results
     * (s_dirty_rect / s_has_dirty / s_last_paint_set) are deliberately not touched here: a commit that
     * paints nothing leaves the last painting commit's rects readable — see er_get_dirty_rect(). */
    for (int worker = 0; worker < ERUI_RENDER_WORKERS; worker++)
    {
        s_composite_contexts[worker].has_dirty = false;
        /* render_tree saves and restores this around each capture, so it is already false — cleared
         * anyway so one unbalanced render can't make every later node report an ancestor's stale AABB. */
        s_composite_contexts[worker].transform_capturing = false;
    }

    /* Blinking cursor: if there is a focused TextInput, mark it dirty whenever the blink phase has
     * changed since the last commit. This keeps the render cost negligible (one re-render per half
     * period) instead of every frame. */
    if (s_focused_input_tag != ER_INVALID_TAG)
    {
        ERNode* focus = er_get_node(s_focused_input_tag);
        if (focus && focus->type == ER_NODE_TEXT_INPUT && focus->is_focused)
        {
            const uint8_t cursor_phase = cursor_blink_on(s_now_ms) ? 1U : 0U;
            if (cursor_phase != s_last_cursor_phase)
            {
                er_mark_dirty_upward(focus);
                s_last_cursor_phase = cursor_phase;
            }
        }
        else
        {
            s_focused_input_tag = ER_INVALID_TAG;
            s_last_cursor_phase = ER_CURSOR_PHASE_UNKNOWN;
        }
    }
    else
    {
        s_last_cursor_phase = ER_CURSOR_PHASE_UNKNOWN;
    }

#if ERUI_ONSCREEN_KEYBOARD
    /* Keyboard avoidance: shift the whole scene UP just enough that the focused input clears the on-screen
     * keyboard (0 when no input is focused or it is already above the strip). A change moves the whole scene,
     * so force a full repaint that frame; while stable, node_screen_rect applies the same offset so damage
     * tracking stays exact. */
    {
        int avoid_y = 0;
        if (er_keyboard_active())
        {
            ERNode* input = er_get_node(s_focused_input_tag);
            if (input)
            {
                ERRect strip;
                er_keyboard_rect((int)root->computed.width, (int)root->computed.height, &strip);
                const int input_bottom =
                    (int)input->animated.y + (int)input->animated.height; /* unshifted layout pos */
                const int needed = input_bottom + ER_KEYBOARD_INPUT_CLEARANCE - strip.y;
                if (needed > 0)
                    avoid_y = needed;
            }
        }
        if (avoid_y != s_keyboard_avoid_y)
        {
            s_keyboard_avoid_y = avoid_y;
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
    const bool layout_ran = (s_layout_dirty || er_layout_animation_has_pending());
    if (layout_ran)
    {
        ER_PERF_BEGIN(ER_PERF_PHASE_LAYOUT);
        int16_t root_width = (root->layout.width != ER_LAYOUT_AUTO) ? root->layout.width : 0;
        int16_t root_height = (root->layout.height != ER_LAYOUT_AUTO) ? root->layout.height : 0;
        if (s_bounds_width > 0 && root_width > s_bounds_width)
            root_width = s_bounds_width;
        if (s_bounds_height > 0 && root_height > s_bounds_height)
            root_height = s_bounds_height;

        er_layout_compute(s_root_tag, root_width, root_height);
        anchor_arc_children(root);
        refresh_scroll_content_sizes(root);
        er_layout_animation_post_layout(root);
        dispatch_layout_events(root, (ERLayoutRect){0});
        compute_subtree_bounds(root); /* refresh cached prune bounds; stay valid through static frames */
        s_subtree_bounds_dirty = false;

        s_layout_dirty = false;
        s_layout_pass_count++;
        ER_PERF_END(ER_PERF_PHASE_LAYOUT);
    }
    else if (s_subtree_bounds_dirty)
    {
        /* An animation added or removed a transform without moving a computed rect, so the layout pass
         * above is skipped. subtree_prunable still has to be refreshed, or render_tree keeps pruning by
         * a box the node no longer paints in.
         *
         * Timed as layout — it is the layout pass's own bookkeeping and walks every node — but the pass
         * COUNT deliberately does not move: that counts solver runs, and the solver did not run. */
        ER_PERF_BEGIN(ER_PERF_PHASE_LAYOUT);
        compute_subtree_bounds(root);
        s_subtree_bounds_dirty = false;
        ER_PERF_END(ER_PERF_PHASE_LAYOUT);
    }

    /* Everything from here to the end of the commit is the paint pipeline: the damage pre-pass that
     * decides what to repaint, the composite itself, and the dirty-flag sweep. Attributed as one
     * RASTER phase — the pre-pass walks the whole node pool, so it belongs with the paint cost it is
     * there to reduce rather than in an unaccounted gap. Within it, the sub-steps that can
     * independently blow up are marked separately (ERPerfRasterSubphase): the pre-pass and the flag sweep
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
    int render_left = root->computed.x;
    int render_top = root->computed.y;
    int render_right = render_left + root->computed.width;
    int render_bottom = render_top + root->computed.height;
    bool render_full = true;    /* repaint the whole root rect */
    bool nothing_dirty = false; /* tracked, but nothing changed (or changes clamped off-screen) */
    ERDamageSet damage;         /* this commit's repaint rects (only meaningful when narrowed) */
    er_damage_set_clear(&damage);
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
        for (uint8_t rect_index = 0U; rect_index < s_removed_set.count; rect_index++)
        {
            const ERRect* removed = &s_removed_set.rects[rect_index];
            add_damage(&damage,
                       removed->x,
                       removed->y,
                       removed->width,
                       removed->height,
                       render_left,
                       render_top,
                       render_right,
                       render_bottom);
            /* Clamped to the root the same way add_damage clamps its insert, so a footprint that lies
             * (partly) off-screen is never reported as repainted when it was not. */
            report_repaint_clamped(removed->x,
                                   removed->y,
                                   removed->width,
                                   removed->height,
                                   render_left,
                                   render_top,
                                   render_right,
                                   render_bottom);
        }
#if ERUI_ONSCREEN_KEYBOARD
        /* On-screen keyboard show/hide/layer-switch: repaint its bottom strip once (then GRAM retains it). */
        if (s_keyboard_dirty)
        {
            ERRect strip;
            er_keyboard_rect((int)root->computed.width, (int)root->computed.height, &strip);
            add_damage(&damage,
                       strip.x,
                       strip.y,
                       strip.width,
                       strip.height,
                       render_left,
                       render_top,
                       render_right,
                       render_bottom);
        }
#endif
        for (uint16_t tag = 0U; tag < (uint16_t)ERUI_MAX_NODES; tag++)
        {
            ERNode* node = er_get_node(tag);
            if (!node)
                continue;

            if (node->subtree_hidden && !(node->type == ER_NODE_MODAL && node->modal_scrim_shown))
                continue;
            if (node->overflow_toggled)
            {
                /* Under a transformed ancestor the subtree bounds below are source-space, like every
                 * other measurement of this node — the ancestor's blit is what puts the toggled
                 * region on screen, and its AABB already covers the whole subtree. */
                ERNode* const overflow_capturing = capturing_transform_ancestor(node);
                if (overflow_capturing
                    && escalate_damage_to_capture(
                        node, overflow_capturing, &damage, false, render_left, render_top, render_right, render_bottom))
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
                int bounds_x, bounds_y, bounds_width, bounds_height;
                if (!node_subtree_screen_rect(node, &bounds_x, &bounds_y, &bounds_width, &bounds_height))
                {
                    trackable = false; /* transform we cannot bound: repaint everything */
                    break;
                }
                clip_rect_to_clippers(node, &bounds_x, &bounds_y, &bounds_width, &bounds_height);
                add_damage(&damage,
                           bounds_x,
                           bounds_y,
                           bounds_width,
                           bounds_height,
                           render_left,
                           render_top,
                           render_right,
                           render_bottom);
                report_repaint_clamped(bounds_x,
                                       bounds_y,
                                       bounds_width,
                                       bounds_height,
                                       render_left,
                                       render_top,
                                       render_right,
                                       render_bottom);
            }
            /* Asked before the rect is measured, because the answer must not depend on which helper
             * manages to measure it: the backdrop covers the root either way. Both branches below
             * hand a changed scrim modal straight to modal_scrim_damage(). */
            const bool scrim_modal = (node->type == ER_NODE_MODAL && (node->modal_visible || node->modal_scrim_shown));
            int rect_x, rect_y, rect_width, rect_height;
            if (!node_screen_rect(node, &rect_x, &rect_y, &rect_width, &rect_height))
            {
                /* Complex transform (scale/rotate). Bound the damage to the node's transformed AABB —
                 * current box plus where it was painted last commit, so a shrinking pulse erases its
                 * trail — instead of forcing a full-screen repaint.
                 */
                NodeTransformDamage transform_damage = {0};
                if (node_transform_damage(node, &transform_damage))
                {
                    const bool moved = node->has_last_paint
                                       && (transform_damage.footprint_x != (int)node->last_paint_rect.x
                                           || transform_damage.footprint_y != (int)node->last_paint_rect.y
                                           || transform_damage.footprint_width != (int)node->last_paint_rect.width
                                           || transform_damage.footprint_height != (int)node->last_paint_rect.height
                                           || spans_paint_window(transform_damage.footprint_x,
                                                                 transform_damage.footprint_y,
                                                                 transform_damage.footprint_width,
                                                                 transform_damage.footprint_height));
                    /* The third way this node's pixels can move: an ANCESTOR gained or lost a transform,
                     * which hands the capture to a different node and flips this one between its AABB and
                     * its raw box (issue #139). It leaves the node clean and its layout box where it was,
                     * so neither test above sees it — and `moved` never will, because the prediction that
                     * has to be checked against is the STALE flag's, which by definition still agrees with
                     * last_paint_rect. `hedge` is exactly the state of not knowing which footprint the
                     * paint will use, so it is the state in which a change above has to be listened for;
                     * a node whose footprint is certain stays as idle as it was. */
                    if (node->source_dirty || moved || (transform_damage.hedge && source_dirty_above(node)))
                    {
                        /* Asked before the ancestor-capture escalation below, for the same reason the
                         * translate-only branch asks it before its own: the scrim covers the root
                         * regardless of what any ancestor does to this node's pixels. */
                        if (scrim_modal)
                        {
                            modal_scrim_damage(node, &damage, render_left, render_top, render_right, render_bottom);
                            continue;
                        }
                        /* Refused the scratch because an ancestor holds it: this node painted into
                         * that ancestor's capture in source space, so td measures a region its pixels
                         * never reach. The ancestor's AABB is where they actually land. */
                        ERNode* const capturing = capturing_transform_ancestor(node);
                        if (capturing
                            && escalate_damage_to_capture(node,
                                                          capturing,
                                                          &damage,
                                                          !node->source_dirty,
                                                          render_left,
                                                          render_top,
                                                          render_right,
                                                          render_bottom))
                            continue;
                        int new_x = transform_damage.footprint_x, new_y = transform_damage.footprint_y,
                            new_width = transform_damage.footprint_width,
                            new_height = transform_damage.footprint_height;
                        clip_rect_to_clippers(node, &new_x, &new_y, &new_width, &new_height);
                        add_damage(&damage,
                                   new_x,
                                   new_y,
                                   new_width,
                                   new_height,
                                   render_left,
                                   render_top,
                                   render_right,
                                   render_bottom); /* new footprint */
                        if (!node->source_dirty)
                            report_repaint_clamped(new_x,
                                                   new_y,
                                                   new_width,
                                                   new_height,
                                                   render_left,
                                                   render_top,
                                                   render_right,
                                                   render_bottom);
                        if (transform_damage.hedge)
                        {
                            /* Which footprint the paint lands on rests on the ancestor chain, not on
                             * anything measurable here, so damage the other candidate too rather than
                             * risk scissoring the node's own paint away. */
                            int hedge_x = transform_damage.hedge_x, hedge_y = transform_damage.hedge_y,
                                hedge_width = transform_damage.hedge_width,
                                hedge_height = transform_damage.hedge_height;
                            clip_rect_to_clippers(node, &hedge_x, &hedge_y, &hedge_width, &hedge_height);
                            add_damage(&damage,
                                       hedge_x,
                                       hedge_y,
                                       hedge_width,
                                       hedge_height,
                                       render_left,
                                       render_top,
                                       render_right,
                                       render_bottom);
                            if (!node->source_dirty)
                                report_repaint_clamped(hedge_x,
                                                       hedge_y,
                                                       hedge_width,
                                                       hedge_height,
                                                       render_left,
                                                       render_top,
                                                       render_right,
                                                       render_bottom);
                        }
                        if (node->has_last_paint)
                        {
                            int old_x = (int)node->last_paint_rect.x, old_y = (int)node->last_paint_rect.y,
                                old_width = (int)node->last_paint_rect.width,
                                old_height = (int)node->last_paint_rect.height;
                            clip_rect_to_clippers(node, &old_x, &old_y, &old_width, &old_height);
                            add_damage(&damage,
                                       old_x,
                                       old_y,
                                       old_width,
                                       old_height,
                                       render_left,
                                       render_top,
                                       render_right,
                                       render_bottom); /* old (erase trail) */
                            report_repaint_clamped(old_x,
                                                   old_y,
                                                   old_width,
                                                   old_height,
                                                   render_left,
                                                   render_top,
                                                   render_right,
                                                   render_bottom);
                        }
                    }
                    continue;
                }
                /*
                 * Could not bound it (a transform that projects to nothing): only forces a full repaint
                 * if actually changing. An oversized node no longer lands here — node_transform_damage()
                 * settles it on size and returns the raw box; neither does a spinning ActivityIndicator,
                 * which node_screen_rect() bounds at the box+translate it actually paints at.
                 * TODO: A moved-but-not-source_dirty node here (e.g. a 3D-transformed node shifted by reflow) is still
                 * missed — that needs the 3D AABB path ().
                 */
                if (node->source_dirty)
                {
                    /* Bounded after all, and by the only rect that was ever right for it: a modal that
                     * cannot be measured still scrims exactly the root. Cheaper than the full-repaint
                     * fallback below, and unlike it, retires the scrim flag. */
                    if (scrim_modal)
                    {
                        modal_scrim_damage(node, &damage, render_left, render_top, render_right, render_bottom);
                        continue;
                    }
                    /* Inside an ancestor's capture this is bounded after all: whatever the node's own
                     * transform does, it does it in source space and reaches the screen only through
                     * the ancestor's blit. Beats a full-screen repaint per animated frame. */
                    ERNode* const capturing = capturing_transform_ancestor(node);
                    if (capturing
                        && escalate_damage_to_capture(
                            node, capturing, &damage, false, render_left, render_top, render_right, render_bottom))
                        continue;
                    trackable = false;
                    break;
                }
                continue;
            }
            const int box_x = rect_x, box_y = rect_y; /* the layout box, before any arc inflation */
            /* Measure the node by its painted footprint — the box plus the arc knob's reach past it and
             * the shadow's bleed around it — which is what last_paint_rect records, so a steady node is
             * not read as "moved" every commit. Both are folded in HERE, ahead of the comparison, and
             * not again below: the new footprint and the recorded trail have to be the same measurement
             * or they can never agree. */
            expand_for_arc(node, &rect_x, &rect_y, &rect_width, &rect_height);
#if ERUI_SHADOWS
            expand_for_shadow(node, &rect_x, &rect_y, &rect_width, &rect_height);
#endif
            /* Clipped as the record is, or a footprint past the clip would never match it. */
            clip_rect_axis(&rect_x, &rect_width);
            clip_rect_axis(&rect_y, &rect_height);
            const bool moved = node->has_last_paint
                               && (rect_x != (int)node->last_paint_rect.x || rect_y != (int)node->last_paint_rect.y
                                   || rect_width != (int)node->last_paint_rect.width
                                   || rect_height != (int)node->last_paint_rect.height
                                   || spans_paint_window(rect_x, rect_y, rect_width, rect_height));
            if (!node->source_dirty && !moved)
                continue; /* unchanged and in place: contributes nothing to the damage */
            if (scrim_modal)
            {
                modal_scrim_damage(node, &damage, render_left, render_top, render_right, render_bottom);
                continue;
            }
            /* Everything below measures this node in plain layout-minus-scroll space. Under a
             * transformed ancestor that space is the ancestor's CAPTURE, not the screen: the node's
             * pixels are inverse-mapped out at the ancestor's transformed AABB, so that AABB — and
             * not any rect derived from the node's own box — is what has to be repainted.
             * (Asked after the Modal case, whose scrim covers the root regardless of any ancestor.) */
            {
                ERNode* const capturing = capturing_transform_ancestor(node);
                if (capturing
                    && escalate_damage_to_capture(node,
                                                  capturing,
                                                  &damage,
                                                  !node->source_dirty,
                                                  render_left,
                                                  render_top,
                                                  render_right,
                                                  render_bottom))
                    continue;
            }
            if ((node->type == ER_NODE_VECTOR || node->type == ER_NODE_ARC) && node->vector_has_dirty && !moved)
            {
                /* Sub-region vector update: damage only the app-supplied changed rect (node-local →
                 * screen), not the whole box. The caller's rect already covers old+new content, so the
                 * full last_paint_rect is intentionally NOT added (it would balloon back to the box). */
                int dirty_offset_x, dirty_offset_y;
                vec_dirty_offset(node, &dirty_offset_x, &dirty_offset_y);
                add_damage(&damage,
                           box_x + dirty_offset_x + (int)node->vector_dirty_x,
                           box_y + dirty_offset_y + (int)node->vector_dirty_y,
                           (int)node->vector_dirty_width,
                           (int)node->vector_dirty_height,
                           render_left,
                           render_top,
                           render_right,
                           render_bottom);
                continue;
            }
            /* Clip both contributions to any clipping ancestor (ScrollView / overflow:hidden) so a scrolled
             * child's damage can't reach outside the list and pull a sibling (e.g. a title above) into the
             * repaint, where it would be cleared but not restored for a frame. */
            int new_x = rect_x, new_y = rect_y, new_width = rect_width, new_height = rect_height;
            clip_rect_to_clippers(node, &new_x, &new_y, &new_width, &new_height);
            add_damage(&damage,
                       new_x,
                       new_y,
                       new_width,
                       new_height,
                       render_left,
                       render_top,
                       render_right,
                       render_bottom); /* new position */
            /* A node here that is NOT source_dirty is contributing purely because it MOVED, and
             * render_tree's accumulator only ever unions source-dirty nodes — so it reports itself,
             * exactly like the vacated footprints above. Without this a pure reflow repaints the
             * framebuffer correctly and then tells a partial-update host that nothing changed. */
            if (!node->source_dirty)
                report_repaint_clamped(
                    new_x, new_y, new_width, new_height, render_left, render_top, render_right, render_bottom);
            if (node->has_last_paint)
            {
                int old_x = (int)node->last_paint_rect.x, old_y = (int)node->last_paint_rect.y,
                    old_width = (int)node->last_paint_rect.width, old_height = (int)node->last_paint_rect.height;
                clip_rect_to_clippers(node, &old_x, &old_y, &old_width, &old_height);
                add_damage(&damage,
                           old_x,
                           old_y,
                           old_width,
                           old_height,
                           render_left,
                           render_top,
                           render_right,
                           render_bottom); /* old position (erase trail) */
                /* Reported unconditionally, unlike the new position: render_tree's accumulator only
                 * ever sees where a node is NOW, so nothing else can report the trail it vacated. */
                report_repaint_clamped(
                    old_x, old_y, old_width, old_height, render_left, render_top, render_right, render_bottom);

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
                if (new_width <= 0 || new_height <= 0)
                    node->has_last_paint = false;
            }
        }
        /* Padding + clamping happened per-insert (add_damage), so an empty set covers both "nothing
         * changed" and "every change clamped off-screen". rb_* stays the set's bounding box — the
         * banded clip, the parallel-height gate, and diagnostics all still want one covering box. */
        if (trackable && damage.count > 0U)
        {
            ERRect bounds;
            er_damage_set_bounds(&damage, &bounds);
            render_left = bounds.x;
            render_top = bounds.y;
            render_right = bounds.x + bounds.width;
            render_bottom = bounds.y + bounds.height;
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
     * advances s_current_buffer at each flip, so a buffer's debt accumulates exactly the commits it missed while a
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
            for (int buffer = 0; buffer < s_display_buffer_count; buffer++)
            {
                s_buffer_debt[buffer].full = true;
                er_damage_set_clear(&s_buffer_debt[buffer].set);
            }
        }
        else
        {
            /* Each rect stays a separate debt entry (the slot's set merges/saturates on its own), so
             * disjoint damage replays as disjoint rects instead of ballooning into one span. */
            for (int buffer = 0; buffer < s_display_buffer_count; buffer++)
                if (!s_buffer_debt[buffer].full)
                    for (uint8_t rect_index = 0U; rect_index < damage.count; rect_index++)
                        er_damage_set_add(&s_buffer_debt[buffer].set,
                                          damage.rects[rect_index].x,
                                          damage.rects[rect_index].y,
                                          damage.rects[rect_index].width,
                                          damage.rects[rect_index].height);
        }

        /* (2) Derive this commit's render set from the CURRENT buffer's debt (what it still owes). */
        ERDamageSlot* debt = &s_buffer_debt[s_current_buffer];
        if (debt->full)
        {
            render_left = root->computed.x;
            render_top = root->computed.y;
            render_right = render_left + root->computed.width;
            render_bottom = render_top + root->computed.height;
            render_full = true;
            nothing_dirty = false;
            root->dirty = true; /* full recomposite of the whole screen into the stale buffer */
        }
        else if (debt->set.count > 0U)
        {
            ERRect bounds;
            damage = debt->set; /* the replayed debt becomes this commit's render set */
            er_damage_set_bounds(&damage, &bounds);
            render_left = bounds.x;
            render_top = bounds.y;
            render_right = bounds.x + bounds.width;
            render_bottom = bounds.y + bounds.height;
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
        debt->full = false;
        er_damage_set_clear(&debt->set);
    }

    /* Enable subtree pruning only when no layout animation is interpolating positions (which would
     * leave the cached computed-space bounds stale). A full repaint pushes no clip, so render_tree()
     * pruning self-disables there regardless. */
    s_prune_ok = !er_layout_animation_has_pending() && !er_layout_animation_is_active();

    /* Rect count is a trade: each rect is a separate clipped pass, and a pass costs a tree walk plus
     * the pixels it touches. With pruning on, the walk is O(nodes near that rect), so many small rects
     * are close to free and the tight clips are what keep a vector node rasterizing only its own
     * changed sub-region. With pruning off the walk visits EVERY node, so the per-pass cost is fixed
     * and the full budget would multiply it — trim back to a handful of coarser rects there. */
    if (!render_full && !nothing_dirty && !s_prune_ok)
        er_damage_set_limit(&damage, UNPRUNED_PASS_RECTS_MAX);

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
        er_damage_set_add(
            &s_last_paint_set, render_left, render_top, render_right - render_left, render_bottom - render_top);
        ER_PERF_REPAINT(render_left, render_top, render_right - render_left, render_bottom - render_top);
    }
    else
    {
        s_last_paint_set = damage;
        for (uint8_t rect_index = 0U; rect_index < damage.count; rect_index++)
            ER_PERF_REPAINT(damage.rects[rect_index].x,
                            damage.rects[rect_index].y,
                            damage.rects[rect_index].width,
                            damage.rects[rect_index].height);
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
            int range_tops[ER_DAMAGE_RECTS_MAX];
            int range_bottoms[ER_DAMAGE_RECTS_MAX];
            int range_count = 0;
            if (render_full)
            {
                range_tops[0] = render_top;
                range_bottoms[0] = render_bottom;
                range_count = 1;
            }
            else
            {
                /* Collect each rect's row interval (insertion sort by y0 — count <= ER_DAMAGE_RECTS_MAX),
                 * then merge overlapping/adjacent intervals: disjoint rects can still share rows (side by
                 * side). */
                for (uint8_t rect_index = 0U; rect_index < damage.count; rect_index++)
                {
                    const int rect_top = damage.rects[rect_index].y;
                    const int rect_bottom = damage.rects[rect_index].y + damage.rects[rect_index].height;
                    int range_index = range_count++;
                    while (range_index > 0 && range_tops[range_index - 1] > rect_top)
                    {
                        range_tops[range_index] = range_tops[range_index - 1];
                        range_bottoms[range_index] = range_bottoms[range_index - 1];
                        range_index--;
                    }
                    range_tops[range_index] = rect_top;
                    range_bottoms[range_index] = rect_bottom;
                }
                int merged = 0;
                for (int range_index = 1; range_index < range_count; range_index++)
                {
                    if (range_tops[range_index] <= range_bottoms[merged])
                    {
                        if (range_bottoms[range_index] > range_bottoms[merged])
                            range_bottoms[merged] = range_bottoms[range_index];
                    }
                    else
                    {
                        merged++;
                        range_tops[merged] = range_tops[range_index];
                        range_bottoms[merged] = range_bottoms[range_index];
                    }
                }
                range_count = merged + 1;
            }

            const int max_band_height = backend->band_height;
            const int frame_x = root->computed.x;
            const int frame_width = root->computed.width;
            /* ONE clip over the whole repaint bbox for the duration (enabling subtree pruning and,
             * crucially, keeping transform/opacity scratch sources complete — it contains every
             * changed node in full). Row narrowing happens purely via the strip loop + er_set_band. */
            er_push_clip_rect(frame_x, render_top, frame_width, render_bottom - render_top);
            for (int range_index = 0; range_index < range_count; range_index++)
            {
                for (int band_y = range_tops[range_index]; band_y < range_bottoms[range_index];
                     band_y += max_band_height)
                {
                    const int band_height = (range_bottoms[range_index] - band_y < max_band_height)
                                                ? (range_bottoms[range_index] - band_y)
                                                : max_band_height;
                    if (backend->band_begin)
                    {
                        ER_PERF_RASTER_BEGIN(ER_PERF_RASTER_BLIT);
                        backend->band_begin(frame_x, band_y, frame_width, band_height, backend->context);
                        ER_PERF_RASTER_END(ER_PERF_RASTER_BLIT);
                    }
                    er_set_band(band_y, band_height);
                    render_tree(
                        root, true, false, 0, s_keyboard_avoid_y); /* whole scene shifted up to clear the keyboard */
                    er_keyboard_draw(frame_width,
                                     (int)root->computed.height); /* overlay (no-op for bands above the strip) */
                    if (backend->band_flush)
                    {
                        ER_PERF_RASTER_BEGIN(ER_PERF_RASTER_BLIT);
                        backend->band_flush(backend->context);
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
        const int worker_count = er_render_workers_active();
        /* One single-core frame when a parallel frame wanted a fade-cache capture and the content
         * has not changed since — the capture happens below, and following frames hit the cache
         * from inside the parallel fork. Consumed ONCE per commit, before any per-rect pass. */
        const bool serial_for_capture =
            s_fade_capture_wanted && (s_fade_capture_wanted_generation == s_content_generation);
        s_fade_capture_wanted = false;
        /* Parallel gate minus the region-height test, which is per-region below. */
        const bool can_parallel = worker_count > 1 && !serial_for_capture && s_parallel_unsafe == 0;
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
            render_tree(root, false, true, 0, s_keyboard_avoid_y);
            er_keyboard_draw((int)root->computed.width, (int)root->computed.height);
        }
        else if (render_full)
        {
            /* Full repaint: one unclipped pass over the whole tree (sliced across workers when the
             * screen is tall enough to pay for the fork). */
            if (can_parallel && (render_bottom - render_top) >= worker_count * ER_PARALLEL_MIN_ROWS)
            {
                ParallelRenderJob job;
                job.root = root;
                job.full_recomposite = full_recomposite;
                job.left = render_left;
                job.right = render_right;
                job.top = render_top;
                job.bottom = render_bottom;
                job.slice_count = worker_count;
                job.keyboard_avoid_y = s_keyboard_avoid_y;
                s_parallel_render = true;
                er_parallel_for(render_slice_job, &job);
                s_parallel_render = false;
                s_parallel_frames++;
                er_push_clip_rect(render_left, render_top, render_right - render_left, render_bottom - render_top);
                er_keyboard_draw((int)root->computed.width, (int)root->computed.height); /* overlay, sequential */
                er_pop_clip_rect();
            }
            else
            {
                /* With bounds set, nothing is drawn past them: a painter that works per pixel otherwise
                 * only stops at its node's box, however far off screen that is. An axis with no limit
                 * gets the largest extent a node can reach. */
                const bool bounded = s_bounds_width > 0 || s_bounds_height > 0;
                if (bounded)
                    er_push_clip_rect(0,
                                      0,
                                      s_bounds_width > 0 ? s_bounds_width : INT16_MAX,
                                      s_bounds_height > 0 ? s_bounds_height : INT16_MAX);
                render_tree(root, full_recomposite, false, 0, s_keyboard_avoid_y);
                er_keyboard_draw((int)root->computed.width, (int)root->computed.height);
                if (bounded)
                    er_pop_clip_rect();
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
            for (uint8_t rect_index = 0U; rect_index < damage.count; rect_index++)
            {
                const ERRect* rect = &damage.rects[rect_index];
                if (can_parallel && rect->height >= worker_count * ER_PARALLEL_MIN_ROWS)
                {
                    ParallelRenderJob job;
                    job.root = root;
                    job.full_recomposite = full_recomposite;
                    job.left = rect->x;
                    job.right = rect->x + rect->width;
                    job.top = rect->y;
                    job.bottom = rect->y + rect->height;
                    job.slice_count = worker_count;
                    job.keyboard_avoid_y = s_keyboard_avoid_y;
                    s_parallel_render = true;
                    er_parallel_for(render_slice_job, &job);
                    s_parallel_render = false;
                    any_parallel = true;
                }
                else
                {
                    er_push_clip_rect(rect->x, rect->y, rect->width, rect->height);
                    render_tree(
                        root, full_recomposite, false, 0, s_keyboard_avoid_y); /* scene shifted up for keyboard */
                    er_pop_clip_rect();
                }
                er_push_clip_rect(rect->x, rect->y, rect->width, rect->height);
                er_keyboard_draw((int)root->computed.width, (int)root->computed.height); /* overlay, per-rect clip */
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
    for (int worker = 0; worker < ERUI_RENDER_WORKERS; worker++)
    {
        const CompCtx* context = &s_composite_contexts[worker];
        if (!context->has_dirty)
            continue;
        if (!painted_has)
        {
            painted_rect = context->dirty_rect;
            painted_has = true;
        }
        else
        {
            const int right = painted_rect.x + painted_rect.width > context->dirty_rect.x + context->dirty_rect.width
                                  ? painted_rect.x + painted_rect.width
                                  : context->dirty_rect.x + context->dirty_rect.width;
            const int bottom = painted_rect.y + painted_rect.height > context->dirty_rect.y + context->dirty_rect.height
                                   ? painted_rect.y + painted_rect.height
                                   : context->dirty_rect.y + context->dirty_rect.height;
            if (context->dirty_rect.x < painted_rect.x)
                painted_rect.x = context->dirty_rect.x;
            if (context->dirty_rect.y < painted_rect.y)
                painted_rect.y = context->dirty_rect.y;
            painted_rect.width = right - painted_rect.x;
            painted_rect.height = bottom - painted_rect.y;
        }
    }

    /* Sequential post-pass: clear the dirty flags of every node painted this commit. The paint
     * traversal itself only stamps painted_seq (an idempotent same-value write), so concurrent
     * workers never race on flag mutation, and a dirty node that was NOT painted (e.g. scrolled
     * offscreen) keeps its flags — exactly the previous clear-during-paint semantics. */
    for (int tag = 0; tag < (int)ERUI_MAX_NODES; tag++)
    {
        ERNode* pool_node = &s_nodes[tag];
        if (!pool_node->in_use)
            continue;

        pool_node->overflow_toggled = false;
        if (pool_node->painted_sequence == s_commit_sequence || pool_node->subtree_hidden)
        {
            pool_node->dirty = false;
            pool_node->source_dirty = false;
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
            painted_rect.x = render_left;
            painted_rect.y = render_top;
            painted_rect.width = render_right - render_left;
            painted_rect.height = render_bottom - render_top;
            painted_has = (painted_rect.width > 0 && painted_rect.height > 0);
        }
        s_dirty_rect = painted_rect;
        s_has_dirty = painted_has;
    }

    s_force_full_repaint = false;
    s_keyboard_dirty = false;            /* the keyboard strip (if any) was repainted this commit */
    er_damage_set_clear(&s_removed_set); /* consumed (or covered by a full repaint) this commit */

    ER_PERF_RASTER_END(ER_PERF_RASTER_SWEEP);
    ER_PERF_END(ER_PERF_PHASE_RASTER);

#if ER_PROF
    {
        static uint32_t s_prof_commits = 0;
        if (++s_prof_commits >= ER_PROF_INTERVAL)
        {
            printf("ERPROF: passes=%u composites=%u push_us=%u content_us=%u blend_us=%u (per %u commits)\n",
                   (unsigned)s_prof_passes,
                   (unsigned)s_prof_composites,
                   (unsigned)s_prof_push_us,
                   (unsigned)s_prof_content_us,
                   (unsigned)s_prof_blend_us,
                   (unsigned)ER_PROF_INTERVAL);
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
    return (uint32_t)s_now_ms;
}

uint64_t er_now_ms64(void)
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
            out->width = 0;
            out->height = 0;
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
        for (int rect_index = 0; rect_index < count; rect_index++)
            out[rect_index] = s_last_paint_set.rects[rect_index];
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

    /* NaN passes every clamp below and has no whole-pixel value: leave that axis where it is. */
    x = er_nan_or(x, node->scroll_offset_x);
    y = er_nan_or(y, node->scroll_offset_y);

    /* Clamp to valid scroll range.  The maximum offset is content_size − viewport_size,
     * floored at 0 so we never scroll past the start or beyond the end. */
    float max_x = (float)(node->scroll_content_width - node->computed.width);
    float max_y = (float)(node->scroll_content_height - node->computed.height);
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

    /* Content is placed at the whole-pixel offset, so a step that stays within the same pixel (the slow tail
     * of a fling) changes nothing on screen and needs no repaint. */
    const bool moved = (int)x != (int)node->scroll_offset_x || (int)y != (int)node->scroll_offset_y;
    node->scroll_offset_x = x;
    node->scroll_offset_y = y;
    if (moved)
        er_mark_dirty_upward(node);

    const EREventHandler* handler = &node->events[ER_EVENT_SCROLL];
    if (handler->callback)
    {
        EREventData data = {0};
        data.scroll_x = x;
        data.scroll_y = y;
        handler->callback(node, &data, handler->user_data);
    }
}

uint32_t er_parallel_frames(void)
{
    return s_parallel_frames;
}
