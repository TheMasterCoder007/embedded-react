#ifndef EMBEDDED_REACT_ER_SCENE_H
#define EMBEDDED_REACT_ER_SCENE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

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
    ER_NODE_VIEW               = 0, /**< Generic container (View). */
    ER_NODE_TEXT,                   /**< Text label (Text). */
    ER_NODE_IMAGE,                  /**< Bitmap image (Image). */
    ER_NODE_SCROLL_VIEW,            /**< Scrollable container (ScrollView). */
    ER_NODE_FLAT_LIST,              /**< Virtualised list (FlatList). */
    ER_NODE_PRESSABLE,              /**< Touchable area (Pressable). */
    ER_NODE_TEXT_INPUT,             /**< Single-line text input (TextInput). */
    ER_NODE_ACTIVITY_INDICATOR,     /**< Spinning activity indicator. */
    ER_NODE_SWITCH,                 /**< Boolean toggle switch. */
    ER_NODE_MODAL,                  /**< Full-screen modal overlay. */
} ERNodeType;

/**
 * @brief Animatable node properties.
 */
typedef enum
{
    ER_PROP_OPACITY          = 0, /**< Transparency (0.0–1.0). */
    ER_PROP_TRANSLATE_X,          /**< Horizontal translation in pixels. */
    ER_PROP_TRANSLATE_Y,          /**< Vertical translation in pixels. */
    ER_PROP_SCALE_X,              /**< Horizontal scale factor. */
    ER_PROP_SCALE_Y,              /**< Vertical scale factor. */
    ER_PROP_ROTATE_Z,             /**< Rotation around the Z axis in degrees. */
    ER_PROP_BACKGROUND_COLOR,     /**< Background ARGB8888 color packed as float bits. */
    ER_PROP_COLOR,                /**< Foreground ARGB8888 color packed as float bits. */
} ERAnimProp;

/**
 * @brief Event types that can be wired to a callback via er_event_set().
 */
typedef enum
{
    ER_EVENT_PRESS       = 0, /**< Tap/click completed. */
    ER_EVENT_LONG_PRESS,      /**< Press held beyond the long-press threshold. */
    ER_EVENT_PRESS_IN,        /**< Finger/pointer entered the hit area. */
    ER_EVENT_PRESS_OUT,       /**< Finger/pointer left the hit area. */
    ER_EVENT_TOUCH_START,     /**< Raw touch-down on this node. */
    ER_EVENT_TOUCH_MOVE,      /**< Raw touch-move on this node. */
    ER_EVENT_TOUCH_END,       /**< Raw touch-up on this node. */
    ER_EVENT_SCROLL,          /**< Scroll offset changed (ScrollView). */
    ER_EVENT_LAYOUT,          /**< Computed layout rectangle changed. */
} EREventType;

/** @brief Opaque node property bag. Defined by the compositor implementation. */
typedef struct ERProps ERProps;

/** @brief Opaque animation configuration. Defined by the animation implementation. */
typedef struct ERAnimConfig ERAnimConfig;

/** @brief Opaque event payload delivered to EREventFn callbacks. */
typedef struct EREventData EREventData;

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
 * @param[in] type  Node type to create.
 *
 * @return Pointer to the new node, or NULL if the pool is exhausted.
 */
ERNode* er_node_create(ERNodeType type);

/**
 * @brief Destroys a node and releases its resources back to the pool.
 *
 * The node must not be part of any tree when this is called.
 *
 * @param[in] node  Node to destroy.
 */
void er_node_destroy(ERNode* node);

/**
 * @brief Applies a property bag to a node.
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
 * @brief Sets the root of the scene tree.
 *
 * The root node is rendered as a full-screen container on every commit.
 *
 * @param[in] root  Node to use as the scene root.
 */
void er_tree_set_root(ERNode* root);

/**
 * @brief Commits the current scene tree and triggers a layout + render pass.
 */
void er_commit(void);

/**
 * @brief Returns the number of milliseconds elapsed since the renderer started.
 *
 * @return Monotonic timestamp in milliseconds.
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
