#ifndef EMBEDDED_REACT_ER_NODE_INTERNAL_H
#define EMBEDDED_REACT_ER_NODE_INTERNAL_H

#include "er_scene.h"
#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Sentinel tag value meaning "no node" / end of list. */
#define ER_INVALID_TAG UINT16_MAX

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Internal Yoga layout specification stored per node.
 *
 * All int16_t fields use ER_LAYOUT_AUTO (== INT16_MIN) as the "not specified" sentinel.
 * Per-edge padding/margin fields override the corresponding shorthand when set.
 */
typedef struct
{
    int16_t left, top, right, bottom;
    int16_t width, height;
    int16_t min_width, max_width;
    int16_t min_height, max_height;
    int16_t padding;
    int16_t padding_left, padding_top, padding_right, padding_bottom;
    int16_t margin;
    int16_t margin_left, margin_top, margin_right, margin_bottom;
    int16_t gap, row_gap, column_gap;
    int16_t flex_grow, flex_shrink, flex_basis;
    uint8_t flex_direction;  /**< ERFlexDirection  */
    uint8_t flex_wrap;       /**< ERFlexWrap       */
    uint8_t align_items;     /**< ERFlexAlign      */
    uint8_t align_self;      /**< ERFlexAlign      */
    uint8_t align_content;   /**< ERAlignContent   */
    uint8_t justify_content; /**< ERFlexJustify    */
    uint8_t position;        /**< ERPositionType   */
    uint8_t display;         /**< ERDisplayMode    */
    uint8_t overflow;        /**< EROverflow       */
    float aspect_ratio;      /**< Width/height ratio; 0.0 = not set. */
    float flex_basis_pct;    /**< flex_basis as % of parent main-axis; 0.0 = not set. */
    float width_pct;         /**< width as % of parent content width; 0.0 = not set. */
    float height_pct;        /**< height as % of parent content height; 0.0 = not set. */
} ERLayoutSpec;

/**
 * @brief Computed layout rectangle produced by the layout pass.
 */
typedef struct
{
    int16_t x, y, w, h;
} ERLayoutRect;

/**
 * @brief Visual properties for a View node.
 */
typedef struct
{
    uint32_t background_color; /**< ARGB8888; 0x00000000 = transparent. */
    uint32_t border_color;     /**< ARGB8888 uniform border color. */
    int16_t border_width;      /**< Uniform border width in pixels. */
    int16_t border_radius;     /**< Uniform corner radius in pixels. */
    /* Per-corner radii (0 = fall back to border_radius). */
    int16_t border_tl_radius;
    int16_t border_tr_radius;
    int16_t border_br_radius;
    int16_t border_bl_radius;
    /* Per-edge border widths (0 = fall back to border_width). */
    int16_t border_left_width;
    int16_t border_top_width;
    int16_t border_right_width;
    int16_t border_bottom_width;
    /* Per-edge border colors (0 = fall back to border_color). */
    uint32_t border_left_color;
    uint32_t border_top_color;
    uint32_t border_right_color;
    uint32_t border_bottom_color;
    uint8_t border_style; /**< ERBorderStyle: 0=solid, 1=dashed, 2=dotted. */
    uint8_t opacity;      /**< 0–255. */

    /* Shadow */
    uint32_t shadow_color; /**< ARGB8888; default 0xFF000000 (black). */
    float shadow_offset_x; /**< Shadow X offset in pixels. */
    float shadow_offset_y; /**< Shadow Y offset in pixels. */
    float shadow_opacity;  /**< 0.0–1.0; 0 means no shadow. */
    uint8_t shadow_radius; /**< Blur radius in pixels. */
    uint8_t elevation;     /**< Android-style elevation in dp; synthesises a shadow when shadow_opacity is 0. */

    /* Gradient (requires ERUI_GRADIENT) */
    uint8_t gradient_type;       /**< ERGradientType. */
    float gradient_angle;        /**< Angle in degrees (0 = top→bottom, 90 = left→right). */
    uint8_t gradient_stop_count; /**< Number of active stops [0–ER_GRADIENT_MAX_STOPS]. */
    ERGradientStop gradient_stops[ER_GRADIENT_MAX_STOPS]; /**< Color stops in order of ascending position. */
} ERViewProps;

/**
 * @brief Visual properties for a Text node.
 */
typedef struct
{
    char text[ER_TEXT_MAX + 1];
    char font_family[ER_FONT_FAMILY_MAX + 1];
    uint32_t color; /**< ARGB8888. */
    uint8_t font_size;
    uint8_t font_weight; /**< 0 = normal, 1 = bold. */
    uint8_t font_style;  /**< ERFontStyle — 0 = normal, 1 = italic. */
    uint8_t text_align;  /**< ERTextAlign. */
    uint8_t number_of_lines;
    uint8_t ellipsize_mode;  /**< ERTextEllipsize. */
    uint8_t text_decoration; /**< ERTextDecoration. */
    int16_t line_height;     /**< 0 = font default. */
    int16_t letter_spacing;
    uint8_t span_count;                  /**< 0 = use text[]; >0 = use spans[]. */
    ERTextSpan spans[ER_TEXT_MAX_SPANS]; /**< Inline span descriptors. */
} ERTextProps;

/**
 * @brief Visual properties for an Image node.
 */
typedef struct
{
    char image_name[ER_IMAGE_NAME_MAX + 1];
    uint8_t resize_mode; /**< ERResizeMode */
    uint32_t tint_color; /**< Straight-alpha ARGB8888 tint; 0 = no tint. */
} ERImageProps;

/**
 * @brief Visual properties for an ActivityIndicator node.
 */
typedef struct
{
    uint32_t color;    /**< Spinner dot color; 0 = 0xFFFFFFFF (white). */
    uint8_t animating; /**< 1 = spinning, 0 = stopped. */
} ERActivityIndicatorProps;

/**
 * @brief Visual properties for a Switch node.
 */
typedef struct
{
    uint8_t value;              /**< 0 = off, 1 = on. */
    uint32_t track_color_false; /**< Track color when off. */
    uint32_t track_color_true;  /**< Track color when on. */
    uint32_t thumb_color;       /**< Thumb circle fill color. */
} ERSwitchProps;

/**
 * @brief Visual properties for a TextInput node.
 */
typedef struct
{
    uint32_t background_color;
    uint32_t border_color;
    int16_t border_width;
    int16_t border_radius;
    uint8_t opacity;
    uint32_t color; /**< Text color. */
    uint8_t font_size;
    char font_family[ER_FONT_FAMILY_MAX + 1];
    char placeholder[ER_PLACEHOLDER_MAX + 1];
    uint32_t placeholder_color;
    uint32_t cursor_color;
    uint8_t editable;
} ERTextInputProps;

/**
 * @brief Registered event handler and caller-owned context pointer.
 */
typedef struct
{
    EREventFn fn;
    void* user_data;
} EREventHandler;

/**
 * @brief Registered gesture responder query callback and caller-owned context pointer.
 */
typedef struct
{
    ERResponderQueryFn fn;
    void* user_data;
} ERResponderQueryHandler;

/**
 * @brief Full internal scene graph node.
 *
 * ERNode* in the public API points to one of these slots in the static pool.
 * The tag field doubles as the pool index.
 */
struct ERNode
{
    uint16_t tag;
    uint16_t parent_tag;
    uint16_t first_child_tag;
    uint16_t next_sibling_tag;
    ERNodeType type;
    bool in_use;
    bool dirty;
    bool source_dirty; /**< Set only on the node that was directly dirtied, not on propagated ancestors. Used for
                          dirty-rect accumulation. */
    int16_t z_index;
    uint8_t pointer_events;  /**< ERPointerEvents — controls which parts of the node receive touch events. */
    int16_t hit_slop_left;   /**< Pixels by which the left hit edge extends beyond the computed rect. */
    int16_t hit_slop_top;    /**< Pixels by which the top hit edge extends beyond the computed rect. */
    int16_t hit_slop_right;  /**< Pixels by which the right hit edge extends beyond the computed rect. */
    int16_t hit_slop_bottom; /**< Pixels by which the bottom hit edge extends beyond the computed rect. */
    ERLayoutSpec layout;
    ERLayoutRect computed;
    ERLayoutRect prev_computed; /**< Computed rect from the previous commit, used to detect layout changes. */
    ERLayoutRect animated;      /**< Current display rect; updated by layout animations; equals computed when idle. */
    float scroll_offset_x;      /**< Horizontal scroll position in pixels (ScrollView only). */
    float scroll_offset_y;      /**< Vertical scroll position in pixels (ScrollView only). */
    float scroll_vel_x;         /**< Momentum velocity X in px/ms (positive = content moving right). */
    float scroll_vel_y;         /**< Momentum velocity Y in px/ms (positive = content moving down). */
    int16_t scroll_content_w;   /**< Bounding width of all children; computed after layout (ScrollView only). */
    int16_t scroll_content_h;   /**< Bounding height of all children; computed after layout (ScrollView only). */
    /* Transform props: raw values copied from ERProps */
    float tp_translate_x; /**< X translation in pixels. */
    float tp_translate_y; /**< Y translation in pixels. */
    float tp_scale_x;     /**< X scale factor; 0.0 = identity (1.0). */
    float tp_scale_y;     /**< Y scale factor; 0.0 = identity (1.0). */
    float tp_rotate_z;    /**< Clockwise rotation in degrees. */
    float tp_origin_x;    /**< Fractional X pivot; negative = center (0.5). */
    float tp_origin_y;    /**< Fractional Y pivot; negative = center (0.5). */
    float tp_rotate_x;    /**< X-axis rotation in degrees (3D). */
    float tp_rotate_y;    /**< Y-axis rotation in degrees (3D). */
    float tp_perspective; /**< Perspective distance in pixels; 0 = orthographic (3D). */
    bool has_transform;   /**< True when any transform prop is non-identity. */
    EREventHandler events[ER_EVENT_TYPE_COUNT_];
    ERResponderQueryHandler queries[ER_RESPONDER_QUERY_COUNT]; /**< Gesture negotiation callbacks. */
    /* Switch: animated thumb position 0.0 (off) → 1.0 (on). */
    float switch_thumb_t;
    /* TextInput: current text content and focus state. */
    char input_text[ER_TEXT_MAX + 1];
    bool is_focused;
    /* Modal: visibility and backdrop color (stored here to avoid growing ERViewProps). */
    uint8_t modal_visible;
    uint32_t modal_backdrop_color;
    union
    {
        ERViewProps view;
        ERTextProps text;
        ERImageProps image;
        ERActivityIndicatorProps act;
        ERSwitchProps sw;
        ERTextInputProps text_input;
    } props;
};

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Looks up a node by tag in the static pool.
 *
 * @param[in] tag  Pool tag (index) to look up.
 *
 * @return Pointer to the node if the tag is valid and the node is in use, otherwise NULL.
 */
ERNode* er_get_node(uint16_t tag);

/**
 * @brief Returns the current scene root node.
 *
 * @return Root node, or NULL if no root is set.
 */
ERNode* er_get_root_node(void);

/**
 * @brief Marks a node and all ancestors dirty.
 *
 * @param[in,out] node  Node whose ancestor chain should be invalidated.
 */
void er_mark_dirty_upward(ERNode* node);

#endif
