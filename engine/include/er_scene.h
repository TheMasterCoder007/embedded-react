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
        ER_NODE_VIEW = 0,           /**< Generic container (View). */
        ER_NODE_TEXT,               /**< Text label (Text). */
        ER_NODE_IMAGE,              /**< Bitmap image (Image). */
        ER_NODE_SCROLL_VIEW,        /**< Scrollable container (ScrollView). */
        ER_NODE_FLAT_LIST,          /**< Virtualised list (FlatList). */
        ER_NODE_PRESSABLE,          /**< Touchable area (Pressable). */
        ER_NODE_TEXT_INPUT,         /**< Single-line text input (TextInput). */
        ER_NODE_ACTIVITY_INDICATOR, /**< Spinning activity indicator. */
        ER_NODE_SWITCH,             /**< Boolean toggle switch. */
        ER_NODE_MODAL,              /**< Full-screen modal overlay. */
    } ERNodeType;

    /**
     * @brief Main-axis direction for a flex container.
     */
    typedef enum
    {
        ER_FLEX_COL = 0,         /**< Children stacked top-to-bottom (default). */
        ER_FLEX_ROW = 1,         /**< Children laid out left-to-right. */
        ER_FLEX_ROW_REVERSE = 2, /**< Children laid out right-to-left. */
        ER_FLEX_COL_REVERSE = 3, /**< Children stacked bottom-to-top. */
    } ERFlexDirection;

    /**
     * @brief Cross-axis alignment for flex items.
     */
    typedef enum
    {
        ER_ALIGN_AUTO = 0,       /**< Inherit from parent's align_items (align_self default). */
        ER_ALIGN_STRETCH = 1,    /**< Stretch to fill cross-axis (align_items default). */
        ER_ALIGN_FLEX_START = 2, /**< Align to the start of the cross-axis. */
        ER_ALIGN_CENTER = 3,     /**< Center on the cross-axis. */
        ER_ALIGN_FLEX_END = 4,   /**< Align to the end of the cross-axis. */
    } ERFlexAlign;

    /**
     * @brief Main-axis distribution of space among flex items.
     */
    typedef enum
    {
        ER_JUSTIFY_FLEX_START = 0,    /**< Pack items toward the start (default). */
        ER_JUSTIFY_CENTER = 1,        /**< Center items along the main axis. */
        ER_JUSTIFY_FLEX_END = 2,      /**< Pack items toward the end. */
        ER_JUSTIFY_SPACE_BETWEEN = 3, /**< Equal space between items; none at edges. */
        ER_JUSTIFY_SPACE_AROUND = 4,  /**< Equal space around each item. */
        ER_JUSTIFY_SPACE_EVENLY = 5,  /**< Equal space between items and at edges. */
    } ERFlexJustify;

    /**
     * @brief Line-wrapping behaviour for flex containers.
     */
    typedef enum
    {
        ER_WRAP_NOWRAP = 0,       /**< All items on one line; may overflow (default). */
        ER_WRAP_WRAP = 1,         /**< Items wrap onto additional lines as needed. */
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
     * @brief Display mode controlling whether a node participates in layout and rendering.
     */
    typedef enum
    {
        ER_DISPLAY_FLEX = 0, /**< Node participates in layout (default). */
        ER_DISPLAY_NONE = 1, /**< Node and its entire subtree are excluded from layout, render, and hit-testing. */
    } ERDisplayMode;

    /**
     * @brief Overflow behaviour controlling how a node clips its content.
     */
    typedef enum
    {
        ER_OVERFLOW_VISIBLE = 0, /**< Content renders beyond node bounds (default). */
        ER_OVERFLOW_HIDDEN = 1,  /**< Content and hit-testing are clipped to node bounds. */
        ER_OVERFLOW_SCROLL = 2,  /**< Content clipped; scroll offset applied by ScrollView. */
    } EROverflow;

    /**
     * @brief How an Image node scales its source bitmap to fill the computed layout rect.
     *
     * Mirrors React Native's Image resizeMode prop.
     */
    typedef enum
    {
        ER_RESIZE_COVER = 0,   /**< Scale to fill, cropping the longer axis from the centre (default). */
        ER_RESIZE_CONTAIN = 1, /**< Scale to fit, letterboxing if aspect ratios differ. */
        ER_RESIZE_STRETCH = 2, /**< Stretch to exactly fill the layout rect, ignoring aspect ratio. */
        ER_RESIZE_REPEAT = 3,  /**< Tile at the image's original pixel size. */
        ER_RESIZE_CENTER = 4,  /**< Display at original size, centred; clip to bounds if larger. */
    } ERResizeMode;

    /**
     * @brief Controls which parts of a node and its subtree can receive pointer events.
     */
    typedef enum
    {
        ER_POINTER_EVENTS_AUTO = 0,     /**< Node and subtree participate in hit-testing (default). */
        ER_POINTER_EVENTS_NONE = 1,     /**< Neither this node nor its children receive touch events. */
        ER_POINTER_EVENTS_BOX_ONLY = 2, /**< Only this node is hittable; children are transparent to touches. */
        ER_POINTER_EVENTS_BOX_NONE = 3, /**< This node is transparent to touches; children are hittable. */
    } ERPointerEvents;

    /**
     * @brief Gesture responder query types used with er_responder_query_set().
     *
     * Capture queries (root→leaf) are evaluated before bubble queries (leaf→root).
     * The first node whose callback returns true claims the gesture responder.
     */
    typedef enum
    {
        ER_QUERY_START_SHOULD_SET = 0,         /**< Bubble: claim on touch-down. */
        ER_QUERY_START_SHOULD_SET_CAPTURE = 1, /**< Capture: claim on touch-down (wins over bubble). */
        ER_QUERY_MOVE_SHOULD_SET = 2,          /**< Bubble: claim on touch-move. */
        ER_QUERY_MOVE_SHOULD_SET_CAPTURE = 3,  /**< Capture: claim on touch-move (wins over bubble). */
        ER_QUERY_TERMINATION_REQUEST = 4,      /**< Can the current responder be taken? Return true to yield. */
    } ERResponderQuery;

    /** @brief Number of ERResponderQuery values; used to size the per-node query handler array. */
#define ER_RESPONDER_QUERY_COUNT 5U

    /**
     * @brief Animatable node properties.
     */
    typedef enum
    {
        ER_PROP_OPACITY = 0,      /**< Transparency (0.0–1.0). */
        ER_PROP_TRANSLATE_X,      /**< Horizontal translation in pixels. */
        ER_PROP_TRANSLATE_Y,      /**< Vertical translation in pixels. */
        ER_PROP_SCALE_X,          /**< Horizontal scale factor. */
        ER_PROP_SCALE_Y,          /**< Vertical scale factor. */
        ER_PROP_ROTATE_Z,         /**< Rotation around the Z axis in degrees. */
        ER_PROP_BACKGROUND_COLOR, /**< Background ARGB8888 color packed as float bits. */
        ER_PROP_COLOR,            /**< Foreground ARGB8888 color packed as float bits. */
    } ERAnimProp;

    /**
     * @brief Animation algorithm used by ERAnimConfig.
     */
    typedef enum
    {
        ER_ANIM_TIMING = 0, /**< Time-based easing curve. */
        ER_ANIM_SPRING = 1, /**< Damped harmonic oscillator. */
        ER_ANIM_DECAY = 2,  /**< Exponential friction decay. */
    } ERAnimType;

    /**
     * @brief Event types that can be wired to a callback via er_event_set().
     */
    typedef enum
    {
        ER_EVENT_PRESS = 0,           /**< Tap/click completed. */
        ER_EVENT_LONG_PRESS,          /**< Press held beyond the long-press threshold. */
        ER_EVENT_PRESS_IN,            /**< Finger/pointer entered the hit area. */
        ER_EVENT_PRESS_OUT,           /**< Finger/pointer left the hit area. */
        ER_EVENT_TOUCH_START,         /**< Raw touch-down on this node. */
        ER_EVENT_TOUCH_MOVE,          /**< Raw touch-move on this node. */
        ER_EVENT_TOUCH_END,           /**< Raw touch-up on this node. */
        ER_EVENT_TOUCH_CANCEL,        /**< Raw touch sequence canceled. */
        ER_EVENT_SCROLL,              /**< Scroll offset changed (ScrollView). */
        ER_EVENT_RESPONDER_GRANT,     /**< Node was granted the gesture responder. */
        ER_EVENT_RESPONDER_REJECT,    /**< Node's responder request was denied by the current responder. */
        ER_EVENT_RESPONDER_MOVE,      /**< Gesture moved while this node is the active responder. */
        ER_EVENT_RESPONDER_RELEASE,   /**< Touch ended while this node is the active responder. */
        ER_EVENT_RESPONDER_TERMINATE, /**< Responder was taken by another node or cancelled. */
        ER_EVENT_LAYOUT,              /**< Computed layout rectangle changed. */
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
        int16_t left;       /**< Offset from flow position / absolute left anchor. */
        int16_t top;        /**< Offset from flow position / absolute top anchor. */
        int16_t right;      /**< Offset from flow position / absolute right anchor. */
        int16_t bottom;     /**< Offset from flow position / absolute bottom anchor. */
        int16_t width;      /**< Explicit width (ER_LAYOUT_AUTO = size from flex). */
        int16_t height;     /**< Explicit height (ER_LAYOUT_AUTO = size from flex). */
        int16_t min_width;  /**< Minimum width constraint. */
        int16_t max_width;  /**< Maximum width constraint. */
        int16_t min_height; /**< Minimum height constraint. */
        int16_t max_height; /**< Maximum height constraint. */
        int16_t padding;    /**< Uniform padding shorthand. */
        int16_t padding_left;
        int16_t padding_top;
        int16_t padding_right;
        int16_t padding_bottom;
        int16_t margin; /**< Uniform margin shorthand. */
        int16_t margin_left;
        int16_t margin_top;
        int16_t margin_right;
        int16_t margin_bottom;
        int16_t gap;             /**< Gap between children (shorthand). */
        int16_t row_gap;         /**< Gap between rows (overrides gap). */
        int16_t column_gap;      /**< Gap between columns (overrides gap). */
        int16_t flex_grow;       /**< How much to grow relative to siblings (0 = no grow). */
        int16_t flex_shrink;     /**< How much to shrink relative to siblings (0 = no shrink). */
        int16_t flex_basis;      /**< Base size before grow/shrink (ER_LAYOUT_AUTO = use width/height). */
        uint8_t flex_direction;  /**< ERFlexDirection — default ER_FLEX_COL. */
        uint8_t flex_wrap;       /**< ERFlexWrap     — default ER_WRAP_NOWRAP. */
        uint8_t align_items;     /**< ERFlexAlign    — default ER_ALIGN_STRETCH. */
        uint8_t align_self;      /**< ERFlexAlign    — default ER_ALIGN_AUTO. */
        uint8_t justify_content; /**< ERFlexJustify  — default ER_JUSTIFY_FLEX_START. */
        uint8_t position;        /**< ERPositionType — default ER_POS_RELATIVE. */
        uint8_t display;         /**< ERDisplayMode  — default ER_DISPLAY_FLEX. */

        /* --- View visual --- */
        uint32_t background_color; /**< ARGB8888; 0x00000000 = transparent. */
        uint32_t border_color;     /**< ARGB8888. */
        int16_t border_width;      /**< Border width in pixels. */
        int16_t border_radius;     /**< Corner radius in pixels. */
        int16_t z_index;           /**< Sibling stacking order; higher values render and hit-test above lower values. */
        uint8_t opacity;           /**< Node opacity 0–255 (255 = fully opaque). */
        uint8_t overflow;          /**< EROverflow — default ER_OVERFLOW_VISIBLE. */

        /* --- Interaction --- */
        uint8_t pointer_events;  /**< ERPointerEvents — default ER_POINTER_EVENTS_AUTO. */
        int16_t hit_slop_left;   /**< Extend hit area beyond left edge in pixels. */
        int16_t hit_slop_top;    /**< Extend hit area beyond top edge in pixels. */
        int16_t hit_slop_right;  /**< Extend hit area beyond right edge in pixels. */
        int16_t hit_slop_bottom; /**< Extend hit area beyond bottom edge in pixels. */

        /* --- Text --- */
        char text[ER_TEXT_MAX + 1];               /**< Null-terminated UTF-8 string. */
        char font_family[ER_FONT_FAMILY_MAX + 1]; /**< Font family name; "" = built-in Inter. */
        uint32_t color;                           /**< Text ARGB8888; 0 defaults to white. */
        uint8_t font_size;                        /**< Font size in pixels. */
        uint8_t font_weight;                      /**< 0 = normal, 1 = bold. */

        /* --- Image --- */
        char image_name[ER_IMAGE_NAME_MAX + 1]; /**< Asset name registered via er_image_load(). */
        uint8_t resize_mode;                    /**< ERResizeMode — default ER_RESIZE_COVER. */
        uint32_t tint_color;                    /**< Straight-alpha ARGB8888; 0 = no tint. */

        /* --- Transform --- */
        float transform_translate_x; /**< X translation in pixels (0 = none). */
        float transform_translate_y; /**< Y translation in pixels (0 = none). */
        float transform_scale_x;     /**< X scale factor; 0.0 treated as identity (1.0). */
        float transform_scale_y;     /**< Y scale factor; 0.0 treated as identity (1.0). */
        float transform_rotate_z;    /**< Clockwise rotation in degrees (0 = none). */
        float transform_origin_x;    /**< Fractional X pivot [0.0–1.0]; negative = center (0.5). */
        float transform_origin_y;    /**< Fractional Y pivot [0.0–1.0]; negative = center (0.5). */
    } ERProps;

    /**
     * @brief Animation timing configuration passed to er_anim_start().
     */
    typedef struct ERAnimConfig
    {
        ERAnimType type;      /**< Animation algorithm. */
        uint32_t duration_ms; /**< Duration for ER_ANIM_TIMING; ignored for spring/decay. */
        float stiffness;      /**< Spring stiffness (ER_ANIM_SPRING). */
        float damping;        /**< Spring damping coefficient (ER_ANIM_SPRING). */
        float mass;           /**< Spring mass (ER_ANIM_SPRING). */
        float velocity;       /**< Initial velocity (ER_ANIM_DECAY). */
        float deceleration;   /**< Friction coefficient (ER_ANIM_DECAY). */
        bool loop;            /**< Repeat the animation indefinitely. */
    } ERAnimConfig;

    /**
     * @brief Payload delivered to an EREventFn callback.
     */
    typedef struct EREventData
    {
        int x;              /**< Touch X coordinate in framebuffer pixels. */
        int y;              /**< Touch Y coordinate in framebuffer pixels. */
        int dx;             /**< X displacement from touch-down origin (responder events). */
        int dy;             /**< Y displacement from touch-down origin (responder events). */
        float scroll_x;     /**< Scroll offset X (ER_EVENT_SCROLL). */
        float scroll_y;     /**< Scroll offset Y (ER_EVENT_SCROLL). */
        ERRect layout_rect; /**< New computed rectangle (ER_EVENT_LAYOUT). */
    } EREventData;

    /**
     * @brief Callback signature for node event handlers.
     *
     * @param[in] node       The node that fired the event.
     * @param[in] data       Event-specific payload (type depends on EREventType).
     * @param[in] user_data  Opaque pointer supplied when the handler was registered.
     */
    typedef void (*EREventFn)(ERNode* node, const EREventData* data, void* user_data);

    /**
     * @brief Callback signature for gesture responder negotiation queries.
     *
     * Returns true when the node wishes to become (or remain) the active gesture responder.
     *
     * @param[in] node       The node being queried.
     * @param[in] data       Current touch event payload (x, y, dx, dy populated).
     * @param[in] user_data  Opaque pointer supplied at registration.
     *
     * @return true to claim or retain the responder; false to pass.
     */
    typedef bool (*ERResponderQueryFn)(ERNode* node, const EREventData* data, void* user_data);

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
     * @brief Programmatically sets the scroll offset of a ScrollView node.
     *
     * The offset is clamped to the valid range [0, content_size − viewport_size] on each
     * axis.  If the new offset differs from the current one the node is marked dirty and
     * ER_EVENT_SCROLL is dispatched with the updated scroll_x / scroll_y values.
     *
     * @param[in] node  ScrollView node to update.
     * @param[in] x     Desired horizontal scroll offset in pixels.
     * @param[in] y     Desired vertical scroll offset in pixels.
     */
    void er_scroll_view_set_offset(ERNode* node, float x, float y);

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

    /**
     * @brief Registers a gesture responder query callback on a node.
     *
     * Query callbacks return a bool and participate in the capture/bubble negotiation
     * protocol that decides which node owns the active gesture. Replaces any previously
     * registered callback for the same query type.
     *
     * @param[in] node       Target node.
     * @param[in] query      Query type (see ERResponderQuery).
     * @param[in] fn         Callback to invoke during negotiation, or NULL to remove.
     * @param[in] user_data  Opaque pointer forwarded to the callback.
     */
    void er_responder_query_set(ERNode* node, ERResponderQuery query, ERResponderQueryFn fn, void* user_data);

#ifdef __cplusplus
}
#endif

#endif
