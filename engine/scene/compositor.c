#include "er_node_internal.h"
#include "layout_engine.h"
#include "renderer_internal.h"
#include "rrect.h"
#include "text_renderer.h"
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
 * @brief Recursively renders a node and its children depth-first.
 *
 * A node is painted when it or any ancestor is dirty. The dirty flag is cleared
 * after the node is drawn, before descending into children.
 *
 * @param[in] n             Node to render.
 * @param[in] parent_dirty  true when an ancestor was dirty this frame.
 */
static void render_tree(ERNode* n, bool parent_dirty)
{
    const bool should_render = n->dirty || parent_dirty;

    if (should_render)
    {
        const int x = n->computed.x;
        const int y = n->computed.y;
        const int w = n->computed.w;
        const int h = n->computed.h;

        switch (n->type)
        {
            case ER_NODE_VIEW:
            case ER_NODE_SCROLL_VIEW:
            case ER_NODE_PRESSABLE:
            case ER_NODE_MODAL:
            {
                const ERViewProps* vp = &n->props.view;
                er_rrect_fill_bordered(vp->background_color,
                                       vp->border_color,
                                       vp->border_width, x, y, w, h,
                                       vp->border_radius);
                break;
            }
            case ER_NODE_TEXT:
            {
                const uint32_t color = n->props.text.color ? n->props.text.color : 0xFFFFFFFFU;
                const ERRect clip = {x, y, w, h};
                er_text_render(n->props.text.text, clip, color, n->props.text.font_size, n->props.text.font_family);
                break;
            }
            case ER_NODE_IMAGE:
            case ER_NODE_FLAT_LIST:
            case ER_NODE_TEXT_INPUT:
            case ER_NODE_ACTIVITY_INDICATOR:
            case ER_NODE_SWITCH:
            default:
                break;
        }
    }

    n->dirty = false;

    uint16_t child_tag = n->first_child_tag;
    while (child_tag != ER_INVALID_TAG)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        render_tree(child, should_render);
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
            break;
        case ER_NODE_TEXT:
            strncpy(node->props.text.text, props->text, ER_TEXT_MAX);
            node->props.text.text[ER_TEXT_MAX] = '\0';
            strncpy(node->props.text.font_family, props->font_family, ER_FONT_FAMILY_MAX);
            node->props.text.font_family[ER_FONT_FAMILY_MAX] = '\0';
            node->props.text.color = props->color;
            node->props.text.font_size = props->font_size;
            node->props.text.font_weight = props->font_weight;
            break;
        case ER_NODE_IMAGE:
            strncpy(node->props.image.image_name, props->image_name, ER_IMAGE_NAME_MAX);
            node->props.image.image_name[ER_IMAGE_NAME_MAX] = '\0';
            break;
        default:
            break;
    }

    node->dirty = true;
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
    render_tree(root, false);
}

uint32_t er_now_ms(void)
{
    return s_now_ms;
}

void er_tick(uint32_t delta_ms)
{
    s_now_ms += delta_ms;
}
