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
    uint8_t justify_content; /**< ERFlexJustify    */
    uint8_t position;        /**< ERPositionType   */
    uint8_t display;         /**< ERDisplayMode    */
    uint8_t overflow;        /**< EROverflow       */
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
    uint32_t border_color;     /**< ARGB8888. */
    int16_t border_width;
    int16_t border_radius;
    uint8_t opacity; /**< 0–255. */
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
} ERTextProps;

/**
 * @brief Visual properties for an Image node.
 */
typedef struct
{
    char image_name[ER_IMAGE_NAME_MAX + 1];
} ERImageProps;

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
    int16_t z_index;
    uint8_t pointer_events;  /**< ERPointerEvents — controls which parts of the node receive touch events. */
    int16_t hit_slop_left;   /**< Pixels by which the left hit edge extends beyond the computed rect. */
    int16_t hit_slop_top;    /**< Pixels by which the top hit edge extends beyond the computed rect. */
    int16_t hit_slop_right;  /**< Pixels by which the right hit edge extends beyond the computed rect. */
    int16_t hit_slop_bottom; /**< Pixels by which the bottom hit edge extends beyond the computed rect. */
    ERLayoutSpec layout;
    ERLayoutRect computed;
    ERLayoutRect prev_computed; /**< Computed rect from the previous commit, used to detect layout changes. */
    EREventHandler events[ER_EVENT_LAYOUT + 1U];
    ERResponderQueryHandler queries[ER_RESPONDER_QUERY_COUNT]; /**< Gesture negotiation callbacks. */
    union
    {
        ERViewProps view;
        ERTextProps text;
        ERImageProps image;
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
