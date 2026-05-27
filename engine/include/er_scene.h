#ifndef EMBEDDED_REACT_ER_SCENE_H
#define EMBEDDED_REACT_ER_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{


#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Sentinel value for layout fields that were not specified by the caller ("auto").
 *
 * Assign this to any int16_t layout field in ERProps to let the layout engine derive
 * the value automatically (e.g. size from flex grow, margin/padding from shorthand).
 */
#define ER_LAYOUT_AUTO INT16_MIN

/** @brief Maximum length of ERProps::text, excluding the null terminator. */
#define ER_TEXT_MAX 255

/** @brief Maximum length of ERProps::font_family, excluding the null terminator. */
#define ER_FONT_FAMILY_MAX 31

/** @brief Maximum length of ERProps::image_name, excluding the null terminator. */
#define ER_IMAGE_NAME_MAX 63

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Opaque scene graph node. Obtain via er_node_create(). */
typedef struct ERNode ERNode;

/**
 * @brief Axis-aligned rectangle used for clipping and layout.
 */
typedef struct
{
    int x; /**< Left edge in pixels. */
    int y; /**< Top edge in pixels. */
    int w; /**< Width in pixels. */
    int h; /**< Height in pixels. */
} ERRect;

/**
 * @brief Supported node types, mirroring core React Native primitives.
 */
typedef enum
{
    ER_NODE_VIEW = 0, /**< Generic container (View). */
    ER_NODE_TEXT, /**< Text label (Text). */
    ER_NODE_IMAGE, /**< Bitmap image (Image). */
    ER_NODE_SCROLL_VIEW, /**< Scrollable container (ScrollView). */
    ER_NODE_FLAT_LIST, /**< Virtualised list (FlatList). */
    ER_NODE_PRESSABLE, /**< Touchable area (Pressable). */
    ER_NODE_TEXT_INPUT, /**< Single-line text input (TextInput). */
    ER_NODE_ACTIVITY_INDICATOR, /**< Spinning activity indicator. */
    ER_NODE_SWITCH, /**< Boolean toggle switch. */
    ER_NODE_MODAL, /**< Full-screen modal overlay. */
} ERNodeType;

/**
 * @brief Main-axis direction for a flex container.
 */
typedef enum
{
    ER_FLEX_COL = 0, /**< Children stacked top-to-bottom (default). */
    ER_FLEX_ROW = 1, /**< Children laid out left-to-right. */
    ER_FLEX_ROW_REVERSE = 2, /**< Children laid out right-to-left. */
    ER_FLEX_COL_REVERSE = 3, /**< Children stacked bottom-to-top. */
} ERFlexDirection;

/**
 * @brief Cross-axis alignment for flex items.
 */
typedef enum
{
    ER_ALIGN_AUTO = 0, /**< Inherit from parent's align_items (align_self default). */
    ER_ALIGN_STRETCH = 1, /**< Stretch to fill cross-axis (align_items default). */
    ER_ALIGN_FLEX_START = 2, /**< Align to the start of the cross-axis. */
    ER_ALIGN_CENTER = 3, /**< Center on the cross-axis. */
    ER_ALIGN_FLEX_END = 4, /**< Align to the end of the cross-axis. */
} ERFlexAlign;

/**
 * @brief Main-axis distribution of space among flex items.
 */
typedef enum
{
    ER_JUSTIFY_FLEX_START = 0, /**< Pack items toward the start (default). */
    ER_JUSTIFY_CENTER = 1, /**< Center items along the main axis. */
    ER_JUSTIFY_FLEX_END = 2, /**< Pack items toward the end. */
    ER_JUSTIFY_SPACE_BETWEEN = 3, /**< Equal space between items; none at edges. */
    ER_JUSTIFY_SPACE_AROUND = 4, /**< Equal space around each item. */
    ER_JUSTIFY_SPACE_EVENLY = 5, /**< Equal space between items and at edges. */
} ERFlexJustify;

/**
 * @brief Line-wrapping behaviour for flex containers.
 */
typedef enum
{
    ER_WRAP_NOWRAP = 0, /**< All items on one line; may overflow (default). */
    ER_WRAP_WRAP = 1, /**< Items wrap onto additional lines as needed. */
    ER_WRAP_WRAP_REVERSE = 2, /**< Items wrap onto additional lines in reverse. */
} ERFlexWrap;

/**
 * @brief Position type for a node within its parent.
 */
typedef enum
{
    ER_POS_RELATIVE = 0, /**< In-flow; left/top/right/bottom offset from flex position. */
    ER_POS_ABSOLUTE = 1, /**< Out of flow; positioned against the parent's padding box. */
} ERPositionType;

/**
 * @brief Animatable node properties.
 */
typedef enum
{
    ER_PROP_OPACITY = 0, /**< Transparency (0.0–1.0). */
    ER_PROP_TRANSLATE_X, /**< Horizontal translation in pixels. */
    ER_PROP_TRANSLATE_Y, /**< Vertical translation in pixels. */
    ER_PROP_SCALE_X, /**< Horizontal scale factor. */
    ER_PROP_SCALE_Y, /**< Vertical scale factor. */
    ER_PROP_ROTATE_Z, /**< Rotation around the Z axis in degrees. */
    ER_PROP_BACKGROUND_COLOR, /**< Background ARGB8888 color packed as float bits. */
    ER_PROP_COLOR, /**< Foreground ARGB8888 color packed as float bits. */
} ERAnimProp;

/**
 * @brief Animation algorithm used by ERAnimConfig.
 */
typedef enum
{
    ER_ANIM_TIMING = 0, /**< Time-based easing curve. */
    ER_ANIM_SPRING = 1, /**< Damped harmonic oscillator. */
    ER_ANIM_DECAY = 2, /**< Exponential friction decay. */
} ERAnimType;

/**
 * @brief Event types that can be wired to a callback via er_event_set().
 */
typedef enum
{
    ER_EVENT_PRESS = 0, /**< Tap/click completed. */
    ER_EVENT_LONG_PRESS, /**< Press held beyond the long-press threshold. */
    ER_EVENT_PRESS_IN, /**< Finger/pointer entered the hit area. */
    ER_EVENT_PRESS_OUT, /**< Finger/pointer left the hit area. */
    ER_EVENT_TOUCH_START, /**< Raw touch-down on this node. */
    ER_EVENT_TOUCH_MOVE, /**< Raw touch-move on this node. */
    ER_EVENT_TOUCH_END, /**< Raw touch-up on this node. */
    ER_EVENT_TOUCH_CANCEL, /**< Raw touch sequence canceled. */
    ER_EVENT_SCROLL, /**< Scroll offset changed (ScrollView). */
    ER_EVENT_LAYOUT, /**< Computed layout rectangle changed. */
} EREventType;

/**
 * @brief Flat property bag passed to er_node_set_props().
 *
 * Initialise all int16_t layout fields to ER_LAYOUT_AUTO and all other fields to zero
 * before setting only the props you need. A zero-initialised ERProps is a valid
 * transparent View with default Yoga layout (flex-column, stretch, no padding/margin).
 *
 * Layout fields use int16_t and accept ER_LAYOUT_AUTO as "not specified — use auto".
 */
typedef struct ERProps
{
    /* --- Layout (Yoga-compatible) --- */
    int16_t left; /**< Offset from flow position / absolute left anchor. */
    int16_t top; /**< Offset from flow position / absolute top anchor. */
    int16_t right; /**< Offset from flow position / absolute right anchor. */
    int16_t bottom; /**< Offset from flow position / absolute bottom anchor. */
    int16_t width; /**< Explicit width (ER_LAYOUT_AUTO = size from flex). */
    int16_t height; /**< Explicit height (ER_LAYOUT_AUTO = size from flex). */
    int16_t min_width; /**< Minimum width constraint. */
    int16_t max_width; /**< Maximum width constraint. */
    int16_t min_height; /**< Minimum height constraint. */
    int16_t max_height; /**< Maximum height constraint. */
    int16_t padding; /**< Uniform padding shorthand. */
    int16_t padding_left;
    int16_t padding_top;
    int16_t padding_right;
    int16_t padding_bottom;
    int16_t margin; /**< Uniform margin shorthand. */
    int16_t margin_left;
    int16_t margin_top;
    int16_t margin_right;
    int16_t margin_bottom;
    int16_t gap; /**< Gap between children (shorthand). */
    int16_t row_gap; /**< Gap between rows (overrides gap). */
    int16_t column_gap; /**< Gap between columns (overrides gap). */
    int16_t flex_grow; /**< How much to grow relative to siblings (0 = no grow). */
    int16_t flex_shrink; /**< How much to shrink relative to siblings (0 = no shrink). */
    int16_t flex_basis; /**< Base size before grow/shrink (ER_LAYOUT_AUTO = use width/height). */
    uint8_t flex_direction; /**< ERFlexDirection — default ER_FLEX_COL. */
    uint8_t flex_wrap; /**< ERFlexWrap     — default ER_WRAP_NOWRAP. */
    uint8_t align_items; /**< ERFlexAlign    — default ER_ALIGN_STRETCH. */
    uint8_t align_self; /**< ERFlexAlign    — default ER_ALIGN_AUTO. */
    uint8_t justify_content; /**< ERFlexJustify  — default ER_JUSTIFY_FLEX_START. */
    uint8_t position; /**< ERPositionType — default ER_POS_RELATIVE. */

    /* --- View visual --- */
    uint32_t background_color; /**< ARGB8888; 0x00000000 = transparent. */
    uint32_t border_color; /**< ARGB8888. */
    int16_t border_width; /**< Border width in pixels. */
    int16_t border_radius; /**< Corner radius in pixels. */
    uint8_t opacity; /**< Node opacity 0–255 (255 = fully opaque). */

    /* --- Text --- */
    char text[ER_TEXT_MAX + 1]; /**< Null-terminated UTF-8 string. */
    char font_family[ER_FONT_FAMILY_MAX + 1]; /**< Font family name; "" = built-in Inter. */
    uint32_t color; /**< Text ARGB8888; 0 defaults to white. */
    uint8_t font_size; /**< Font size in pixels. */
    uint8_t font_weight; /**< 0 = normal, 1 = bold. */

    /* --- Image --- */
    char image_name[ER_IMAGE_NAME_MAX + 1]; /**< Asset name registered via er_image_load(). */
} ERProps;

/**
 * @brief Animation timing configuration passed to er_anim_start().
 */
typedef struct ERAnimConfig
{
    ERAnimType type; /**< Animation algorithm. */
    uint32_t duration_ms; /**< Duration for ER_ANIM_TIMING; ignored for spring/decay. */
    float stiffness; /**< Spring stiffness (ER_ANIM_SPRING). */
    float damping; /**< Spring damping coefficient (ER_ANIM_SPRING). */
    float mass; /**< Spring mass (ER_ANIM_SPRING). */
    float velocity; /**< Initial velocity (ER_ANIM_DECAY). */
    float deceleration; /**< Friction coefficient (ER_ANIM_DECAY). */
    bool loop; /**< Repeat the animation indefinitely. */
} ERAnimConfig;

/**
 * @brief Payload delivered to an EREventFn callback.
 */
typedef struct EREventData
{
    int x; /**< Touch X coordinate in framebuffer pixels. */
    int y; /**< Touch Y coordinate in framebuffer pixels. */
    float scroll_x; /**< Scroll offset X (ER_EVENT_SCROLL). */
    float scroll_y; /**< Scroll offset Y (ER_EVENT_SCROLL). */
} EREventData;

/**
 * @brief Callback signature for node event handlers.
 *
 * @param[in] node       The node that fired the event.
 * @param[in] data       Event-specific payload (type depends on EREventType).
 * @param[in] user_data  Opaque pointer supplied when the handler was registered.
 */
typedef void (*EREventFn)(ERNode* node, const EREventData* data, void* user_data);

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Allocates a new scene graph node of the given type.
 *
 * All layout fields default to ER_LAYOUT_AUTO and all visual fields to zero.
 * The node is dirty on creation and will be rendered on the next er_commit().
 *
 * @param[in] type  Node type to create.
 *
 * @return Pointer to the new node, or NULL if the pool is exhausted.
 */
ERNode* er_node_create(ERNodeType type);

/**
 * @brief Destroys a node and returns its slot to the pool.
 *
 * The node must have been removed from its parent tree before this call.
 *
 * @param[in] node  Node to destroy.
 */
void er_node_destroy(ERNode* node);

/**
 * @brief Applies a property bag to a node, marking it dirty for re-render.
 *
 * Only the fields relevant to the node's type are copied; others are ignored.
 * All layout fields are always applied regardless of type.
 *
 * @param[in] node   Target node.
 * @param[in] props  Properties to apply.
 */
void er_node_set_props(ERNode* node, const ERProps* props);

/**
 * @brief Appends a child node to the end of a parent's child list.
 *
 * @param[in] parent  Parent node.
 * @param[in] child   Child node to append.
 */
void er_tree_append_child(ERNode* parent, ERNode* child);

/**
 * @brief Removes a child node from its parent's child list.
 *
 * @param[in] parent  Parent node.
 * @param[in] child   Child node to remove.
 */
void er_tree_remove_child(ERNode* parent, ERNode* child);

/**
 * @brief Sets the root of the scene tree for subsequent commits.
 *
 * @param[in] root  Node to use as the scene root.
 */
void er_tree_set_root(ERNode* root);

/**
 * @brief Commits all pending tree mutations: runs the Yoga layout pass then paints dirty nodes.
 */
void er_commit(void);

/**
 * @brief Returns the number of milliseconds elapsed since embedded_renderer_set_backend() was called.
 *
 * @return Monotonic timestamp in milliseconds, accumulated from embedded_renderer_tick() calls.
 */
uint32_t er_now_ms(void);

/**
 * @brief Registers a baked bitmap font blob under a name for use by Text nodes.
 *
 * @param[in] name  Null-terminated font family name (e.g. "Inter").
 * @param[in] buf   Pointer to the font blob data (FONT wire format v1).
 * @param[in] len   Size of the blob in bytes.
 */
void er_font_load(const char* name, const void* buf, size_t len);

/**
 * @brief Registers a premultiplied ARGB8888 image under a name for use by Image nodes.
 *
 * @param[in] name      Null-terminated image asset name.
 * @param[in] argb_buf  Pointer to the premultiplied ARGB8888 pixel data (row-major).
 * @param[in] w         Image width in pixels.
 * @param[in] h         Image height in pixels.
 */
void er_image_load(const char* name, const void* argb_buf, int w, int h);

/**
 * @brief Starts an animation on a node property.
 *
 * @param[in] node   Node to animate.
 * @param[in] prop   Property to animate.
 * @param[in] value  Target value to animate towards.
 * @param[in] cfg    Animation timing configuration.
 */
void er_anim_start(ERNode* node, ERAnimProp prop, float value, const ERAnimConfig* cfg);

/**
 * @brief Cancels a running animation on a node property.
 *
 * @param[in] node  Node whose animation should be cancelled.
 * @param[in] prop  Property whose animation should be cancelled.
 */
void er_anim_cancel(ERNode* node, ERAnimProp prop);

/**
 * @brief Registers an event handler on a node.
 *
 * Replaces any previously registered handler for the same event type.
 *
 * @param[in] node       Target node.
 * @param[in] event      Event type to listen for.
 * @param[in] fn         Callback to invoke when the event fires, or NULL to remove.
 * @param[in] user_data  Opaque pointer forwarded to the callback.
 */
void er_event_set(ERNode* node, EREventType event, EREventFn fn, void* user_data);

#ifdef __cplusplus
}
#endif

#endif
