#include "er_node_internal.h"
#include "image_scaler.h"
#include "layout_engine.h"
#include "renderer_internal.h"
#include "rrect.h"
#include "scratch_pool.h"
#include "shadow.h"
#include "text_renderer.h"
#include "transform.h"
#include <string.h>

#ifndef ERUI_MAX_NODES
#define ERUI_MAX_NODES 512
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERNode s_nodes[ERUI_MAX_NODES];
static uint16_t s_next_tag = 0;
static uint16_t s_root_tag = ER_INVALID_TAG;
static uint32_t s_now_ms = 0;

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
    L->justify_content = ER_JUSTIFY_FLEX_START;
    L->position = ER_POS_RELATIVE;
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
 * @brief Marks a node and all ancestors dirty so stale child pixels are repainted.
 *
 * The renderer currently paints into a persistent framebuffer. If a child changes
 * shape or text, the parent background must be redrawn before the child is painted
 * again, otherwise old pixels can remain visible.
 *
 * @param[in,out] node  Node whose ancestor chain should be invalidated.
 */
void er_mark_dirty_upward(ERNode* node)
{
    while (node)
    {
        node->dirty = true;
        node = er_get_node(node->parent_tag);
    }
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

    /* Actual screen position after applying all ancestor scroll offsets. */
    int px = n->computed.x - translate_x;
    int py = n->computed.y - translate_y;
    const int w = n->computed.w;
    const int h = n->computed.h;

    /* --- 2D transform application --- */
    bool doing_affine = false;
    int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
    float xf_ia = 1.0f, xf_ib = 0.0f, xf_ic = 0.0f, xf_id = 1.0f, xf_itx = 0.0f, xf_ity = 0.0f;

    if (n->has_transform)
    {
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
            case ER_NODE_MODAL:
                er_shadow_render(&n->props.view, px, py, w, h);
                break;
            default:
                break;
        }
    }

    /* Opacity compositing: View-family nodes with opacity < 255 render into an off-screen
     * scratch slot which is then blended at the node's alpha.  er_scratch_push returns
     * false when the pool is exhausted or the node is too large; in that case the subtree
     * renders at full opacity (graceful degradation). */
    const uint8_t node_opacity = (n->type == ER_NODE_VIEW || n->type == ER_NODE_SCROLL_VIEW
                                  || n->type == ER_NODE_PRESSABLE || n->type == ER_NODE_MODAL)
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
            case ER_NODE_MODAL:
            {
                const ERViewProps* vp = &n->props.view;
                er_rrect_fill_bordered(
                    vp->background_color, vp->border_color, vp->border_width, px, py, w, h, vp->border_radius);
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
                par.line_height = tp->line_height;
                par.letter_spacing = tp->letter_spacing;
                er_text_render(&par);
                break;
            }
            case ER_NODE_IMAGE:
                er_image_render(&n->props.image, px, py, w, h);
                break;
            case ER_NODE_FLAT_LIST:
            case ER_NODE_TEXT_INPUT:
            case ER_NODE_ACTIVITY_INDICATOR:
            case ER_NODE_SWITCH:
            default:
                break;
        }
    }

    n->dirty = false;

    /* Overflow clipping: push a scissor rect so children cannot draw outside this node. */
    const bool clips = (n->layout.overflow == ER_OVERFLOW_HIDDEN || n->layout.overflow == ER_OVERFLOW_SCROLL);
    if (clips)
        er_push_clip_rect(px, py, w, h);

    /* Scroll offset translation: ScrollView children are shifted by the current offset. */
    const int child_tx = (n->type == ER_NODE_SCROLL_VIEW) ? translate_x + (int)n->scroll_offset_x : translate_x;
    const int child_ty = (n->type == ER_NODE_SCROLL_VIEW) ? translate_y + (int)n->scroll_offset_y : translate_y;

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

    /* Affine transform: end source capture and blit the transformed result. */
    if (doing_affine)
        er_transform_source_end_blit(
            px, py, w, h, xf_ia, xf_ib, xf_ic, xf_id, xf_itx, xf_ity, dst_x, dst_y, dst_w, dst_h);
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

    if (node->type == ER_NODE_SCROLL_VIEW)
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
    if (s_next_tag >= (uint16_t)ERUI_MAX_NODES)
        return NULL;

    const uint16_t tag = s_next_tag++;
    ERNode* n = &s_nodes[tag];
    memset(n, 0, sizeof(ERNode));

    n->tag = tag;
    n->parent_tag = ER_INVALID_TAG;
    n->first_child_tag = ER_INVALID_TAG;
    n->next_sibling_tag = ER_INVALID_TAG;
    n->type = type;
    n->in_use = true;
    n->dirty = true;

    init_layout_defaults(&n->layout);

    /* View-type nodes default to fully opaque */
    if (type == ER_NODE_VIEW || type == ER_NODE_SCROLL_VIEW || type == ER_NODE_PRESSABLE || type == ER_NODE_MODAL)
    {
        n->props.view.opacity = 255U;
    }

    return n;
}

void er_node_destroy(ERNode* node)
{
    if (!node)
        return;
    node->in_use = false;
    node->dirty = false;
}

void er_node_set_props(ERNode* node, const ERProps* props)
{
    if (!node || !props)
        return;

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
    L->padding_left = props->padding_left;
    L->padding_top = props->padding_top;
    L->padding_right = props->padding_right;
    L->padding_bottom = props->padding_bottom;
    L->margin = props->margin;
    L->margin_left = props->margin_left;
    L->margin_top = props->margin_top;
    L->margin_right = props->margin_right;
    L->margin_bottom = props->margin_bottom;
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
    L->justify_content = props->justify_content;
    L->position = props->position;
    L->display = props->display;
    L->overflow = props->overflow;
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
    node->has_transform =
        (props->transform_translate_x != 0.0f || props->transform_translate_y != 0.0f
         || props->transform_scale_x != 0.0f || props->transform_scale_y != 0.0f || props->transform_rotate_z != 0.0f);

    /* Copy type-specific visual props. */
    switch (node->type)
    {
        case ER_NODE_VIEW:
        case ER_NODE_SCROLL_VIEW:
        case ER_NODE_PRESSABLE:
        case ER_NODE_MODAL:
            node->props.view.background_color = props->background_color;
            node->props.view.border_color = props->border_color;
            node->props.view.border_width = props->border_width;
            node->props.view.border_radius = props->border_radius;
            node->props.view.opacity = props->opacity;
            node->props.view.shadow_color = props->shadow_color;
            node->props.view.shadow_offset_x = props->shadow_offset_x;
            node->props.view.shadow_offset_y = props->shadow_offset_y;
            node->props.view.shadow_opacity = props->shadow_opacity;
            node->props.view.shadow_radius = props->shadow_radius;
            node->props.view.elevation = props->elevation;
            break;
        case ER_NODE_TEXT:
            strncpy(node->props.text.text, props->text, ER_TEXT_MAX);
            node->props.text.text[ER_TEXT_MAX] = '\0';
            strncpy(node->props.text.font_family, props->font_family, ER_FONT_FAMILY_MAX);
            node->props.text.font_family[ER_FONT_FAMILY_MAX] = '\0';
            node->props.text.color = props->color;
            node->props.text.font_size = props->font_size;
            node->props.text.font_weight = props->font_weight;
            node->props.text.text_align = props->text_align;
            node->props.text.number_of_lines = props->number_of_lines;
            node->props.text.ellipsize_mode = props->ellipsize_mode;
            node->props.text.text_decoration = props->text_decoration;
            node->props.text.line_height = props->line_height;
            node->props.text.letter_spacing = props->letter_spacing;
            break;
        case ER_NODE_IMAGE:
            strncpy(node->props.image.image_name, props->image_name, ER_IMAGE_NAME_MAX);
            node->props.image.image_name[ER_IMAGE_NAME_MAX] = '\0';
            node->props.image.resize_mode = props->resize_mode;
            node->props.image.tint_color = props->tint_color;
            break;
        default:
            break;
    }

    er_mark_dirty_upward(node);
}

void er_tree_append_child(ERNode* parent, ERNode* child)
{
    if (!parent || !child)
        return;

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
}

void er_tree_remove_child(ERNode* parent, ERNode* child)
{
    if (!parent || !child)
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

    child->parent_tag = ER_INVALID_TAG;
    child->next_sibling_tag = ER_INVALID_TAG;
    parent->dirty = true;
}

void er_tree_set_root(ERNode* root)
{
    if (!root)
    {
        s_root_tag = ER_INVALID_TAG;
        return;
    }
    s_root_tag = root->tag;
}

void er_commit(void)
{
    if (s_root_tag == ER_INVALID_TAG)
        return;

    ERNode* root = er_get_node(s_root_tag);
    if (!root)
        return;

    const int16_t rw = (root->layout.width != ER_LAYOUT_AUTO) ? root->layout.width : 0;
    const int16_t rh = (root->layout.height != ER_LAYOUT_AUTO) ? root->layout.height : 0;

    er_layout_compute(s_root_tag, rw, rh);
    refresh_scroll_content_sizes(root);
    dispatch_layout_events(root);
    render_tree(root, false, 0, 0);
}

uint32_t er_now_ms(void)
{
    return s_now_ms;
}

void er_tick(uint32_t delta_ms)
{
    s_now_ms += delta_ms;
}

void er_scroll_view_set_offset(ERNode* node, float x, float y)
{
    if (!node || node->type != ER_NODE_SCROLL_VIEW)
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
