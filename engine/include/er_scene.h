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

/** @brief Maximum length of ERProps::placeholder, excluding the null terminator. */
#define ER_PLACEHOLDER_MAX 63

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
        ER_NODE_VECTOR,             /**< Vector graphics surface (Svg): rasterized path op-tape. */
    } ERNodeType;

    /*
     * Vector node contract (ER_NODE_VECTOR). A vector node carries a flat float "op-tape" plus a paint
     * table; the JS/SVG layer compiles its shapes into this form and the engine rasterizes it. The JS
     * mirror of these opcodes must stay in sync. Tape coordinates are node-local pixels; each op is an
     * opcode float followed by a fixed argument count:
     *   ER_VOP_SHAPE  paint_index            begin a shape; following path ops use paints[index]
     *   ER_VOP_MOVE   x y                     start a subpath
     *   ER_VOP_LINE   x y                     line to
     *   ER_VOP_QUAD   cx cy x y               quadratic bezier
     *   ER_VOP_CUBIC  c1x c1y c2x c2y x y     cubic bezier
     *   ER_VOP_ARC    cx cy r a0 a1 ccw       circular arc (radians, clockwise-from-+X; ccw!=0 reverses)
     *   ER_VOP_CLOSE                          close the current subpath
     */
#define ER_VOP_SHAPE 0.0f
#define ER_VOP_MOVE 1.0f
#define ER_VOP_LINE 2.0f
#define ER_VOP_QUAD 3.0f
#define ER_VOP_CUBIC 4.0f
#define ER_VOP_ARC 5.0f
#define ER_VOP_CLOSE 6.0f

#define ER_VCAP_BUTT 0
#define ER_VCAP_ROUND 1
#define ER_VCAP_SQUARE 2
#define ER_VJOIN_MITER 0
#define ER_VJOIN_ROUND 1
#define ER_VJOIN_BEVEL 2
#define ER_VFILL_NONZERO 0
#define ER_VFILL_EVENODD 1

    /**
     * @brief Paint for one vector shape. Colors are exact uint32 ARGB8888 (never floats — a 32-bit
     *        float cannot hold a 32-bit color losslessly).
     */
    typedef struct
    {
        uint32_t fill;     /**< Fill ARGB8888; alpha 0 => no fill. */
        uint32_t stroke;   /**< Stroke ARGB8888; alpha 0 => no stroke. */
        float stroke_w;    /**< Stroke width in px; <= 0 => no stroke. */
        float miter;       /**< Miter limit; <= 0 => default 4. */
        uint8_t cap;       /**< ER_VCAP_*. */
        uint8_t join;      /**< ER_VJOIN_*. */
        uint8_t fill_rule; /**< ER_VFILL_*. */
    } ERVectorPaint;

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
     * @brief Cross-axis distribution of wrap lines within a multi-line flex container.
     *
     * Mirrors React Native / Yoga `alignContent`. Has no effect on a single-line (nowrap)
     * container or when the lines already fill the container's cross size. Default is
     * ER_ALIGN_CONTENT_FLEX_START (Yoga's default).
     */
    typedef enum
    {
        ER_ALIGN_CONTENT_FLEX_START = 0,    /**< Pack lines toward the cross-start (default). */
        ER_ALIGN_CONTENT_FLEX_END = 1,      /**< Pack lines toward the cross-end. */
        ER_ALIGN_CONTENT_CENTER = 2,        /**< Center lines on the cross axis. */
        ER_ALIGN_CONTENT_STRETCH = 3,       /**< Grow each line equally to fill the cross size. */
        ER_ALIGN_CONTENT_SPACE_BETWEEN = 4, /**< Equal space between lines; none at the edges. */
        ER_ALIGN_CONTENT_SPACE_AROUND = 5,  /**< Equal space around each line. */
    } ERAlignContent;

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
     * @brief Horizontal alignment of text lines within the layout rectangle.
     */
    typedef enum
    {
        ER_TEXT_ALIGN_LEFT = 0,   /**< Left-align each line (default). */
        ER_TEXT_ALIGN_CENTER = 1, /**< Centre each line horizontally. */
        ER_TEXT_ALIGN_RIGHT = 2,  /**< Right-align each line. */
    } ERTextAlign;

    /**
     * @brief Ellipsis placement when text is truncated by number_of_lines.
     */
    typedef enum
    {
        ER_TEXT_ELLIPSIZE_TAIL = 0,   /**< Truncate the end and append '…' (default). */
        ER_TEXT_ELLIPSIZE_HEAD = 1,   /**< Truncate the beginning and prepend '…'. */
        ER_TEXT_ELLIPSIZE_MIDDLE = 2, /**< Keep start and end; truncate the middle. */
        ER_TEXT_ELLIPSIZE_CLIP = 3,   /**< Clip at boundary without any ellipsis. */
    } ERTextEllipsize;

    /**
     * @brief Decoration line drawn over or under rendered glyphs.
     */
    typedef enum
    {
        ER_TEXT_DECORATION_NONE = 0,         /**< No decoration (default). */
        ER_TEXT_DECORATION_UNDERLINE = 1,    /**< Underline drawn one pixel below the baseline. */
        ER_TEXT_DECORATION_LINE_THROUGH = 2, /**< Strikethrough drawn at mid-cap height. */
    } ERTextDecoration;

    /**
     * @brief Font style applied to rendered glyphs.
     *
     * ER_FONT_STYLE_ITALIC produces a synthetic slant (horizontal shear) because the built-in
     * font blob does not ship a separate italic face.  The effect is a per-row rightward shift
     * proportional to distance from the glyph bottom, giving an ~11° slant.
     */
    typedef enum
    {
        ER_FONT_STYLE_NORMAL = 0, /**< Upright glyphs (default). */
        ER_FONT_STYLE_ITALIC = 1, /**< Synthetic italic via horizontal shear. */
    } ERFontStyle;

    /**
     * @brief Stroke pattern applied to rendered border edges.
     */
    typedef enum
    {
        ER_BORDER_SOLID = 0,  /**< Continuous solid border (default). */
        ER_BORDER_DASHED = 1, /**< Alternating filled and empty dashes (8 px on, 6 px off). */
        ER_BORDER_DOTTED = 2, /**< Small filled dots at regular intervals (3 px on, 3 px off). */
    } ERBorderStyle;

/** @brief Maximum number of color stops in a gradient. */
#define ER_GRADIENT_MAX_STOPS 4

/** @brief Maximum number of inline text spans in a Text node. */
#define ER_TEXT_MAX_SPANS 4

/** @brief Maximum length of a single text span's content, excluding the null terminator. */
#define ER_SPAN_TEXT_MAX 63

    /**
     * @brief Type of gradient fill applied to a View-family node background.
     *
     * Requires ERUI_GRADIENT to be non-zero at build time. When ERUI_GRADIENT is 0 the
     * gradient_type field is stored but has no rendering effect; background_color is used
     * instead. ER_GRADIENT_RADIAL additionally requires ERUI_GRADIENT_RADIAL.
     */
    typedef enum
    {
        ER_GRADIENT_NONE = 0,   /**< No gradient — background_color is used (default). */
        ER_GRADIENT_LINEAR = 1, /**< Linear gradient at a configurable angle. */
        ER_GRADIENT_RADIAL = 2, /**< Radial gradient from the rect centre outward (requires ERUI_GRADIENT_RADIAL). */
    } ERGradientType;

    /**
     * @brief One color stop in a gradient.
     *
     * Stops should be ordered by ascending position. The gradient is extrapolated with the
     * first stop's color below stops[0].position and the last stop's color above the last
     * stop's position.
     */
    typedef struct
    {
        uint32_t color; /**< Stop color in straight-alpha ARGB8888. */
        float position; /**< Stop position along the gradient axis [0.0–1.0]. */
    } ERGradientStop;

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
        ER_PROP_ROTATE_X,         /**< Rotation around the X axis in degrees (3D; requires ERUI_3D_TRANSFORMS). */
        ER_PROP_ROTATE_Y,         /**< Rotation around the Y axis in degrees (3D; requires ERUI_3D_TRANSFORMS). */
        ER_PROP_BACKGROUND_COLOR, /**< Background ARGB8888 color packed as float bits. */
        ER_PROP_COLOR,            /**< Foreground ARGB8888 color packed as float bits. */
        ER_PROP_SWITCH_THUMB,     /**< Switch thumb position 0.0 (off) – 1.0 (on). */
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
     * @brief Easing curve applied to ER_ANIM_TIMING animations.
     *
     * Mirrors the Easing module from React Native.  ER_EASE_BEZIER uses the four
     * bezier_* control-point fields in ERAnimConfig.  All other values are self-
     * contained.
     */
    typedef enum
    {
        ER_EASE_LINEAR = 0,   /**< Constant rate (default). */
        ER_EASE_EASE,         /**< Smooth ease-in-out (cubic-bezier 0.25,0.1,0.25,1). */
        ER_EASE_EASE_IN,      /**< Accelerate from zero (cubic-bezier 0.42,0,1,1). */
        ER_EASE_EASE_OUT,     /**< Decelerate to zero (cubic-bezier 0,0,0.58,1). */
        ER_EASE_EASE_IN_OUT,  /**< Accelerate then decelerate (cubic-bezier 0.42,0,0.58,1). */
        ER_EASE_QUAD_IN,      /**< Quadratic acceleration. */
        ER_EASE_QUAD_OUT,     /**< Quadratic deceleration. */
        ER_EASE_QUAD_IN_OUT,  /**< Quadratic in then out. */
        ER_EASE_CUBIC_IN,     /**< Cubic acceleration. */
        ER_EASE_CUBIC_OUT,    /**< Cubic deceleration. */
        ER_EASE_CUBIC_IN_OUT, /**< Cubic in then out. */
        ER_EASE_BOUNCE_OUT,   /**< Bouncing deceleration at the end. */
        ER_EASE_ELASTIC_OUT,  /**< Elastic overshoot with decay at the end. */
        ER_EASE_BEZIER,       /**< Custom cubic-bezier; set bezier_x1/y1/x2/y2. */
    } ERAnimEasing;

    /**
     * @brief Opaque handle returned by er_anim_start() and the group functions.
     *
     * Pass to er_anim_stop() to cancel before completion.  ER_ANIM_HANDLE_INVALID
     * (0) is the sentinel for "no animation".
     */
    typedef uint16_t ERAnimHandle;

/** @brief Sentinel value indicating no animation or an already-completed animation. */
#define ER_ANIM_HANDLE_INVALID 0U

    /**
     * @brief Opaque handle for a standalone animatable value (useNativeDriver binding model).
     *
     * A standalone value animates a single float that is decoupled from any specific node
     * property.  One or more node-property pairs can be bound to the value; when the value
     * changes every bound pair is updated automatically without re-entering any higher-level
     * layer per frame — the engine-side equivalent of React Native's useNativeDriver: true.
     *
     * Obtain via er_anim_value_create(), release via er_anim_value_destroy().
     */
    typedef uint16_t ERAnimValueHandle;

/** @brief Sentinel indicating an invalid or destroyed animatable value handle. */
#define ER_ANIM_VALUE_INVALID 0U

    /**
     * @brief Callback invoked when an animation or group finishes.
     *
     * @param[in] finished    true when the animation ran to completion; false when
     *                        it was stopped early via er_anim_stop().
     * @param[in] user_data   Opaque pointer supplied at registration.
     */
    typedef void (*ERAnimCompleteFn)(bool finished, void* user_data);

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
        ER_EVENT_CHANGE_TEXT,         /**< TextInput text changed; data->changed_text holds the new string. */
        ER_EVENT_SUBMIT_EDITING,      /**< TextInput Return/Enter key pressed. */
        ER_EVENT_FOCUS,               /**< TextInput gained keyboard focus. */
        ER_EVENT_BLUR,                /**< TextInput lost keyboard focus. */
        ER_EVENT_TYPE_COUNT_,         /**< Sentinel — not a real event; used for array sizing. */
    } EREventType;

    /**
     * @brief One inline text span with per-span style overrides.
     *
     * Sentinel values cause a field to inherit from the parent Text node.  Sentinels:
     * color == 0, font_size == 0, font_weight == 0xFF, font_style == 0xFF,
     * text_decoration == 0xFF, letter_spacing == ER_LAYOUT_AUTO (INT16_MIN).
     */
    typedef struct
    {
        char text[ER_SPAN_TEXT_MAX + 1]; /**< Null-terminated UTF-8 text fragment. */
        uint32_t color;                  /**< ARGB8888; 0 = inherit parent color. */
        uint8_t font_size;               /**< Font size in pixels; 0 = inherit parent font_size. */
        uint8_t font_weight;             /**< 0 = normal, 1 = bold, 0xFF = inherit. */
        uint8_t font_style;              /**< ERFontStyle; 0xFF = inherit. */
        uint8_t text_decoration;         /**< ERTextDecoration; 0xFF = inherit. */
        int16_t letter_spacing;          /**< Extra pixels per glyph advance; ER_LAYOUT_AUTO = inherit. */
    } ERTextSpan;

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
        uint8_t align_content;   /**< ERAlignContent — default ER_ALIGN_CONTENT_FLEX_START. */
        uint8_t justify_content; /**< ERFlexJustify  — default ER_JUSTIFY_FLEX_START. */
        uint8_t position;        /**< ERPositionType — default ER_POS_RELATIVE. */
        uint8_t display;         /**< ERDisplayMode  — default ER_DISPLAY_FLEX. */
        float aspect_ratio; /**< Width/height ratio (0.0 = not set). When one dimension is auto the other is derived. */
        int16_t
            margin_horizontal;   /**< Shorthand for margin_left and margin_right; overridden by the per-edge fields. */
        int16_t margin_vertical; /**< Shorthand for margin_top and margin_bottom; overridden by the per-edge fields. */
        int16_t
            padding_horizontal; /**< Shorthand for padding_left and padding_right; overridden by the per-edge fields. */
        int16_t
            padding_vertical; /**< Shorthand for padding_top and padding_bottom; overridden by the per-edge fields. */
        float flex_basis_pct; /**< flex_basis as a percentage of the parent's main-axis size [0.0–100.0]; 0.0 = not set.
                                 Takes precedence over flex_basis. */
        float width_pct;      /**< width as a percentage of the parent's content width [0.0–100.0]; 0.0 = not set.
                                 Takes precedence over width. */
        float height_pct;     /**< height as a percentage of the parent's content height [0.0–100.0]; 0.0 = not set.
                                 Takes precedence over height. */

        /* --- View visual --- */
        uint32_t background_color;          /**< ARGB8888; 0x00000000 = transparent. */
        uint32_t border_color;              /**< ARGB8888 border color for all edges. */
        int16_t border_width;               /**< Uniform border width in pixels; overridden by per-edge widths. */
        int16_t border_radius;              /**< Uniform corner radius in pixels; overridden by per-corner radii. */
        int16_t border_top_left_radius;     /**< Top-left corner radius in pixels; 0 = use border_radius. */
        int16_t border_top_right_radius;    /**< Top-right corner radius in pixels; 0 = use border_radius. */
        int16_t border_bottom_left_radius;  /**< Bottom-left corner radius in pixels; 0 = use border_radius. */
        int16_t border_bottom_right_radius; /**< Bottom-right corner radius in pixels; 0 = use border_radius. */
        int16_t border_left_width;          /**< Left border width in pixels; 0 = use border_width. */
        int16_t border_top_width;           /**< Top border width in pixels; 0 = use border_width. */
        int16_t border_right_width;         /**< Right border width in pixels; 0 = use border_width. */
        int16_t border_bottom_width;        /**< Bottom border width in pixels; 0 = use border_width. */
        uint32_t border_left_color;         /**< Left edge border ARGB8888; 0 = use border_color. */
        uint32_t border_top_color;          /**< Top edge border ARGB8888; 0 = use border_color. */
        uint32_t border_right_color;        /**< Right edge border ARGB8888; 0 = use border_color. */
        uint32_t border_bottom_color;       /**< Bottom edge border ARGB8888; 0 = use border_color. */
        uint8_t border_style;               /**< ERBorderStyle — default ER_BORDER_SOLID. */
        int16_t z_index;  /**< Sibling stacking order; higher values render and hit-test above lower values. */
        uint8_t opacity;  /**< Node opacity 0–255 (255 = fully opaque). */
        uint8_t overflow; /**< EROverflow — default ER_OVERFLOW_VISIBLE. */

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
        uint8_t font_style;                       /**< ERFontStyle — default ER_FONT_STYLE_NORMAL. */
        uint8_t text_align;                       /**< ERTextAlign — default ER_TEXT_ALIGN_LEFT. */
        uint8_t number_of_lines;                  /**< Maximum rendered lines; 0 = unlimited. */
        uint8_t ellipsize_mode;                   /**< ERTextEllipsize — used when number_of_lines truncates. */
        uint8_t text_decoration;                  /**< ERTextDecoration — default none. */
        int16_t line_height;                      /**< Line height in pixels; 0 = use font default. */
        int16_t letter_spacing;                   /**< Extra pixels added to each glyph advance; may be negative. */
        uint8_t span_count;                       /**< 0 = use text[]; >0 = render using spans[] instead. */
        ERTextSpan spans[ER_TEXT_MAX_SPANS];      /**< Per-span style overrides; active when span_count > 0. */

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
        float transform_rotate_x;    /**< Rotation around X axis in degrees (3D; requires ERUI_3D_TRANSFORMS). */
        float transform_rotate_y;    /**< Rotation around Y axis in degrees (3D; requires ERUI_3D_TRANSFORMS). */
        float transform_perspective; /**< Perspective distance in pixels; 0 = orthographic (3D; requires
                                        ERUI_3D_TRANSFORMS). */

        /* --- Shadow (View-family only; requires ERUI_SHADOWS at build time) --- */
        uint32_t shadow_color; /**< ARGB8888; default 0xFF000000 (opaque black). */
        float shadow_offset_x; /**< Shadow X offset in pixels (0 = directly behind node). */
        float shadow_offset_y; /**< Shadow Y offset in pixels (0 = directly behind node). */
        float shadow_opacity;  /**< 0.0–1.0; 0 = no shadow (default). */
        uint8_t shadow_radius; /**< Blur radius in pixels; 0 = hard edge. */
        uint8_t elevation;     /**< Android-style elevation in dp; synthesises a shadow when shadow_opacity is 0. */

        /* --- ActivityIndicator --- */
        uint32_t indicator_color; /**< Spinner dot color; 0 = 0xFFFFFFFF (white). */
        uint8_t animating;        /**< 1 = spinning (default when node created), 0 = stopped. */

        /* --- Switch --- */
        uint8_t switch_value;       /**< 0 = off, 1 = on. */
        uint32_t track_color_false; /**< Track color when off; 0 = system gray (0xFF767577). */
        uint32_t track_color_true;  /**< Track color when on;  0 = system blue (0xFF81B0FF). */
        uint32_t thumb_color;       /**< Thumb fill color; 0 = white (0xFFFFFFFF). */

        /* --- TextInput --- */
        char placeholder[ER_PLACEHOLDER_MAX + 1]; /**< Shown when the input is empty. */
        uint32_t placeholder_color;               /**< Placeholder color; 0 = dim gray (0xFF888888). */
        uint32_t cursor_color;                    /**< Cursor bar color; 0 = same as color field. */
        uint8_t editable;                         /**< 1 = editable (default), 0 = read-only. */

        /* --- Modal --- */
        uint8_t modal_visible;   /**< 1 = shown, 0 = hidden (default). */
        uint32_t backdrop_color; /**< Full-screen backdrop ARGB8888; 0 = 0x99000000. */

        /* --- Gradient (View-family; requires ERUI_GRADIENT at build time) --- */
        uint8_t gradient_type;       /**< ERGradientType — default ER_GRADIENT_NONE. */
        float gradient_angle;        /**< Angle in degrees: 0 = top→bottom, 90 = left→right. */
        uint8_t gradient_stop_count; /**< Number of valid entries in gradient_stops [0–ER_GRADIENT_MAX_STOPS]. */
        ERGradientStop gradient_stops[ER_GRADIENT_MAX_STOPS]; /**< Color stops in order of ascending position. */
    } ERProps;

    /**
     * @brief Animation configuration passed to er_anim_start() and group functions.
     *
     * Zero-initialise, then set only the fields relevant to the chosen type.
     * All fields that are not set default to sensible values (see field docs).
     */
    typedef struct ERAnimConfig
    {
        ERAnimType type;              /**< Animation algorithm (default ER_ANIM_TIMING). */
        ERAnimEasing easing;          /**< Easing curve for ER_ANIM_TIMING (default ER_EASE_LINEAR). */
        uint32_t duration_ms;         /**< Duration for ER_ANIM_TIMING; ignored for spring/decay. */
        uint32_t delay_ms;            /**< Delay before the animation begins; 0 = start immediately. */
        float stiffness;              /**< Spring stiffness k (ER_ANIM_SPRING; default 100). */
        float damping;                /**< Spring damping coefficient c (ER_ANIM_SPRING; default 10). */
        float mass;                   /**< Spring mass m (ER_ANIM_SPRING; default 1). */
        float velocity;               /**< Initial velocity in value/s for spring; value/ms for decay. */
        float deceleration;           /**< Per-ms friction [0,1) for ER_ANIM_DECAY (default 0.998). */
        float bezier_x1;              /**< First control point X for ER_EASE_BEZIER [0,1]. */
        float bezier_y1;              /**< First control point Y for ER_EASE_BEZIER. */
        float bezier_x2;              /**< Second control point X for ER_EASE_BEZIER [0,1]. */
        float bezier_y2;              /**< Second control point Y for ER_EASE_BEZIER. */
        bool loop;                    /**< Ping-pong repeat for ER_ANIM_TIMING; ignored otherwise. */
        ERAnimCompleteFn on_complete; /**< Called when the animation finishes; NULL = none. */
        void* on_complete_user_data;  /**< Forwarded to on_complete as user_data. */
    } ERAnimConfig;

    /**
     * @brief Configuration for a layout animation applied to nodes whose computed rect changes.
     *
     * Pass to er_layout_anim_configure_next() before calling er_commit().  On the next commit
     * every node whose computed layout rectangle changed will animate from its current display
     * position to the new computed position using this config.  The config is consumed by the
     * first commit after it is registered and has no effect on subsequent commits.
     *
     * Convenience presets: ER_LAYOUT_ANIM_EASE_IN_EASE_OUT, ER_LAYOUT_ANIM_LINEAR,
     * ER_LAYOUT_ANIM_SPRING.
     */
    typedef struct ERLayoutAnimConfig
    {
        ERAnimType type;      /**< ER_ANIM_TIMING or ER_ANIM_SPRING. */
        uint16_t duration_ms; /**< Duration for ER_ANIM_TIMING; ignored for spring. */
        ERAnimEasing easing;  /**< Easing curve for ER_ANIM_TIMING; ignored for spring. */
        float stiffness;      /**< Spring stiffness k (ER_ANIM_SPRING; 0 = default 100). */
        float damping;        /**< Spring damping c (ER_ANIM_SPRING; 0 = default 10). */
        float mass;           /**< Spring mass m (ER_ANIM_SPRING; 0 = default 1). */
        float bezier_x1;      /**< Bezier CP1 X for ER_EASE_BEZIER. */
        float bezier_y1;      /**< Bezier CP1 Y for ER_EASE_BEZIER. */
        float bezier_x2;      /**< Bezier CP2 X for ER_EASE_BEZIER. */
        float bezier_y2;      /**< Bezier CP2 Y for ER_EASE_BEZIER. */
    } ERLayoutAnimConfig;

    /**
     * @brief Single entry in an animation group (er_anim_sequence / parallel / stagger).
     */
    typedef struct
    {
        ERNode* node;     /**< Target scene node. */
        ERAnimProp prop;  /**< Property to animate. */
        float value;      /**< Target value (same encoding as er_anim_start). */
        ERAnimConfig cfg; /**< Per-entry animation configuration. */
    } ERAnimEntry;

    /**
     * @brief Payload delivered to an EREventFn callback.
     */
    typedef struct EREventData
    {
        int x;                    /**< Touch X coordinate in framebuffer pixels. */
        int y;                    /**< Touch Y coordinate in framebuffer pixels. */
        int dx;                   /**< X displacement from touch-down origin (responder events). */
        int dy;                   /**< Y displacement from touch-down origin (responder events). */
        float scroll_x;           /**< Scroll offset X (ER_EVENT_SCROLL). */
        float scroll_y;           /**< Scroll offset Y (ER_EVENT_SCROLL). */
        ERRect layout_rect;       /**< New computed rectangle (ER_EVENT_LAYOUT). */
        const char* changed_text; /**< Points into node's text buffer (ER_EVENT_CHANGE_TEXT). */
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
     * @brief Inserts a child into a parent's child list immediately before another child.
     *
     * If @p child is already a child of @p parent it is first detached from its current position,
     * so this also reorders (moves) an existing child. When @p before is NULL, or is not a child of
     * @p parent, the child is appended at the end (matching append semantics).
     *
     * @param[in] parent  Parent node.
     * @param[in] child   Child node to insert or move.
     * @param[in] before  Sibling to insert before, or NULL to append.
     */
    void er_tree_insert_before(ERNode* parent, ERNode* child, ERNode* before);

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
     *
     * The layout pass is recomputed only when a prop set or tree mutation changed something that
     * can affect a computed rect since the last commit (or when a LayoutAnimation is pending).
     * Static frames and frames that only advance animations skip the flex + text-measure pass and
     * just repaint dirty nodes. Use er_layout_pass_count() to observe how often layout actually ran.
     */
    void er_commit(void);

    /**
     * @brief Returns how many times er_commit() has actually run the layout pass.
     *
     * Increments once per commit that recomputes layout, and stays unchanged on commits that take
     * the static/animation-only fast path. Intended for diagnostics and profiling — e.g. confirming
     * that an idle screen is not re-solving flex every frame.
     *
     * @return Monotonic count of executed layout passes since process start.
     */
    uint32_t er_layout_pass_count(void);

    /**
     * @brief Returns the number of milliseconds elapsed since embedded_renderer_set_backend() was called.
     *
     * @return Monotonic timestamp in milliseconds, accumulated from embedded_renderer_tick() calls.
     */
    uint32_t er_now_ms(void);

    /**
     * @brief Registers a baked bitmap font blob under a name for use by Text nodes.
     *
     * The blob is copied into the engine's static font pool (sized by ERUI_FONT_POOL_BYTES;
     * when that is 0 this call is a no-op). Loading the same name with a new size adds that
     * size to the family. Loading the same name and size again replaces it: the pool bytes
     * are reused in place when the new blob fits the prior footprint, otherwise fresh bytes
     * are consumed and the old ones are not reclaimed. Load each font once where possible.
     *
     * @param[in] name  Null-terminated font family name (e.g. "Inter").
     * @param[in] buf   Pointer to the font blob data (FONT wire format v1).
     * @param[in] len   Size of the blob in bytes.
     */
    void er_font_load(const char* name, const void* buf, size_t len);

    /**
     * @brief Registers a premultiplied ARGB8888 image under a name for use by Image nodes.
     *
     * The pixel buffer is caller-owned and referenced by pointer, never copied, so it must
     * stay live for as long as any Image node uses the name. Registering an existing name
     * replaces its buffer/dimensions in place (no engine-side memory is leaked); each
     * distinct name occupies one of IMAGE_REGISTRY_MAX slots.
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
     * Any previously running animation on the same node+prop is cancelled before
     * the new one starts.  If cfg is NULL a zero-duration linear timing animation
     * is used, snapping the property to value immediately.
     *
     * @param[in] node   Node to animate.
     * @param[in] prop   Property to animate.
     * @param[in] value  Target value; for color props, pass the ARGB8888 word packed into
     *                   a float via memcpy (use the color_bits() pattern).
     * @param[in] cfg    Animation configuration, or NULL for an immediate snap.
     *
     * @return Handle identifying this animation; ER_ANIM_HANDLE_INVALID on failure.
     */
    ERAnimHandle er_anim_start(ERNode* node, ERAnimProp prop, float value, const ERAnimConfig* cfg);

    /**
     * @brief Cancels a running animation on a node property.
     *
     * @param[in] node  Node whose animation should be cancelled.
     * @param[in] prop  Property whose animation should be cancelled.
     */
    void er_anim_cancel(ERNode* node, ERAnimProp prop);

    /**
     * @brief Stops any animation (or group) identified by handle.
     *
     * The on_complete callback, if any, is called with finished = false.  Passing
     * ER_ANIM_HANDLE_INVALID is a no-op.
     *
     * @param[in] handle  Handle returned by er_anim_start() or a group function.
     */
    void er_anim_stop(ERAnimHandle handle);

    /**
     * @brief Runs a list of animations one after another (Animated.sequence equivalent).
     *
     * Each animation starts only after the previous one finishes.  on_complete is
     * called after the last entry completes.
     *
     * @param[in] entries      Array of animation entries.
     * @param[in] count        Number of entries (capped at ERUI_MAX_GROUP_ENTRIES).
     * @param[in] on_complete  Called when all entries finish; NULL = none.
     * @param[in] user_data    Forwarded to on_complete.
     *
     * @return Group handle, or ER_ANIM_HANDLE_INVALID if no group slot is free.
     */
    ERAnimHandle
    er_anim_sequence(const ERAnimEntry* entries, uint16_t count, ERAnimCompleteFn on_complete, void* user_data);

    /**
     * @brief Starts all animations simultaneously (Animated.parallel equivalent).
     *
     * on_complete is called once all entries have finished.
     *
     * @param[in] entries      Array of animation entries.
     * @param[in] count        Number of entries (capped at ERUI_MAX_GROUP_ENTRIES).
     * @param[in] on_complete  Called when every entry finishes; NULL = none.
     * @param[in] user_data    Forwarded to on_complete.
     *
     * @return Group handle, or ER_ANIM_HANDLE_INVALID if no group slot is free.
     */
    ERAnimHandle
    er_anim_parallel(const ERAnimEntry* entries, uint16_t count, ERAnimCompleteFn on_complete, void* user_data);

    /**
     * @brief Starts animations with a fixed delay between each start (Animated.stagger equivalent).
     *
     * Entry i starts after i × stagger_ms milliseconds.  on_complete is called once
     * all entries have finished.
     *
     * @param[in] entries      Array of animation entries.
     * @param[in] count        Number of entries (capped at ERUI_MAX_GROUP_ENTRIES).
     * @param[in] stagger_ms   Delay in milliseconds between consecutive entry starts.
     * @param[in] on_complete  Called when every entry finishes; NULL = none.
     * @param[in] user_data    Forwarded to on_complete.
     *
     * @return Group handle, or ER_ANIM_HANDLE_INVALID if no group slot is free.
     */
    ERAnimHandle er_anim_stagger(
        const ERAnimEntry* entries, uint16_t count, uint32_t stagger_ms, ERAnimCompleteFn on_complete, void* user_data);

    /**
     * @brief Creates a standalone animatable float value.
     *
     * The value is independent of any specific node or property until node-property pairs
     * are registered with er_anim_value_bind().  When animated, every bound pair is updated
     * on each engine tick without any higher-level involvement per frame.
     *
     * @param[in] initial_value  Starting float value.
     *
     * @return New handle, or ER_ANIM_VALUE_INVALID if the value pool is exhausted.
     */
    ERAnimValueHandle er_anim_value_create(float initial_value);

    /**
     * @brief Destroys a standalone animatable value and cancels any running animation on it.
     *
     * Any node-property bindings registered via er_anim_value_bind() are released.
     * Passing ER_ANIM_VALUE_INVALID is a no-op.
     *
     * @param[in] handle  Handle returned by er_anim_value_create().
     */
    void er_anim_value_destroy(ERAnimValueHandle handle);

    /**
     * @brief Binds a node property to this value.
     *
     * From this point on, whenever the value changes (via er_anim_value_animate() or
     * er_anim_value_set()), node->prop is updated automatically and the node is marked dirty.
     * Duplicate bindings for the same node+prop pair are silently ignored.  The binding
     * capacity per value is fixed at compile time (ERUI_MAX_VALUE_BINDINGS, default 4).
     *
     * @param[in] handle  Target animatable value.
     * @param[in] node    Scene node to receive updates.
     * @param[in] prop    Animatable property on the node to update.
     */
    void er_anim_value_bind(ERAnimValueHandle handle, ERNode* node, ERAnimProp prop);

    /**
     * @brief Removes all node-property bindings from this value.
     *
     * The value itself is not destroyed; call er_anim_value_destroy() to free the slot.
     *
     * @param[in] handle  Target animatable value.
     */
    void er_anim_value_unbind_all(ERAnimValueHandle handle);

    /**
     * @brief Starts an animation that drives a standalone value toward to_value.
     *
     * All bound node-property pairs are updated on each tick as the value interpolates.
     * Any previously running animation on this value is cancelled first.  If cfg is NULL a
     * zero-duration snap is applied immediately.
     *
     * Supports the same animation types (timing/spring/decay), easing, delay, loop, and
     * completion callback as er_anim_start().  Color properties are not supported for
     * standalone values.
     *
     * @param[in] handle    Standalone value to animate.
     * @param[in] to_value  Target float value.
     * @param[in] cfg       Animation configuration, or NULL for an immediate snap.
     *
     * @return Handle identifying this animation; ER_ANIM_HANDLE_INVALID on failure.
     */
    ERAnimHandle er_anim_value_animate(ERAnimValueHandle handle, float to_value, const ERAnimConfig* cfg);

    /**
     * @brief Immediately sets a standalone value and pushes it to all bound node properties.
     *
     * Any running animation on the value is cancelled before the new value is applied.
     * Equivalent to er_anim_value_animate(handle, value, NULL).
     *
     * @param[in] handle  Target animatable value.
     * @param[in] value   New float value to apply.
     */
    void er_anim_value_set(ERAnimValueHandle handle, float value);

    /**
     * @brief Returns the current float of a standalone animatable value.
     *
     * @param[in] handle  Target animatable value.
     *
     * @return Current value, or 0.0f if handle is invalid.
     */
    float er_anim_value_get(ERAnimValueHandle handle);

/*----------------------------------------------------------------------------------------------------------------------
 - Interpolation ranges
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum number of breakpoints (input/output pairs) in an interpolation mapping. */
#define ER_INTERPOLATE_MAX_POINTS 8

    /**
     * @brief Out-of-range behavior for an interpolation, mirroring React Native's `extrapolate`.
     */
    typedef enum ERExtrapolate
    {
        ER_EXTRAPOLATE_EXTEND = 0, /**< Continue the slope of the nearest segment past the range (default). */
        ER_EXTRAPOLATE_CLAMP,      /**< Clamp to the nearest endpoint of the output range. */
        ER_EXTRAPOLATE_IDENTITY    /**< Pass the raw input through unchanged outside the range. */
    } ERExtrapolate;

    /**
     * @brief Piecewise-linear mapping from an input range to an output range.
     *
     * @details Mirrors React Native's `value.interpolate({ inputRange, outputRange, extrapolate })`.
     * `input_range` must be monotonically increasing; `output_range` entries may be non-monotonic.
     * `point_count` must be >= 2.  Inputs between two breakpoints are linearly interpolated.
     * Inputs outside the range obey `extrapolate_left` / `extrapolate_right`.
     */
    typedef struct
    {
        float input_range[ER_INTERPOLATE_MAX_POINTS];  /**< Ascending input breakpoints. */
        float output_range[ER_INTERPOLATE_MAX_POINTS]; /**< Output value at each breakpoint. */
        uint8_t point_count;             /**< Number of valid breakpoints [2–ER_INTERPOLATE_MAX_POINTS]. */
        ERExtrapolate extrapolate_left;  /**< Behavior for inputs below input_range[0]. */
        ERExtrapolate extrapolate_right; /**< Behavior for inputs above the last breakpoint. */
    } ERInterpolation;

    /**
     * @brief Maps a single float through a piecewise-linear interpolation.
     *
     * @param[in] input             Value to map.
     * @param[in] input_range       Ascending input breakpoints (point_count entries).
     * @param[in] output_range      Output values at each breakpoint (point_count entries).
     * @param[in] point_count       Number of breakpoints; values < 2 return input unchanged.
     * @param[in] extrapolate_left  Behavior for inputs below input_range[0].
     * @param[in] extrapolate_right Behavior for inputs above the last breakpoint.
     *
     * @return The interpolated output value.
     */
    float er_interpolate(float input,
                         const float* input_range,
                         const float* output_range,
                         int point_count,
                         ERExtrapolate extrapolate_left,
                         ERExtrapolate extrapolate_right);

    /**
     * @brief Binds a shared animatable value to a node property through an interpolation mapping.
     *
     * @details Like er_anim_value_bind(), but on every value change the raw float is first mapped
     * through @p interp before being written to the property.  This is the engine-side equivalent
     * of `value.interpolate({ inputRange, outputRange })` in React Native's useNativeDriver path.
     *
     * @param[in] handle  Animatable value to bind.
     * @param[in] node    Target node.
     * @param[in] prop    Property to drive.
     * @param[in] interp  Interpolation mapping (copied; must have point_count >= 2).
     */
    void er_anim_value_bind_interpolated(ERAnimValueHandle handle,
                                         ERNode* node,
                                         ERAnimProp prop,
                                         const ERInterpolation* interp);

    /**
     * @brief Returns the axis-aligned bounding rectangle of all pixels repainted during the last er_commit().
     *
     * Useful for partial-update display drivers (SPI DMA, etc.) that can restrict
     * their transfer to only the changed region each frame.  The rect is in framebuffer
     * pixels and is guaranteed to cover every pixel modified by the last er_commit().
     * It may be slightly conservative (e.g. shadows expand it by their blur radius).
     *
     * The value is invalidated on the next er_commit() call, so read it immediately
     * after er_commit() returns and before the next one starts.
     *
     * @param[out] out  Populated with the dirty rectangle; set to {0,0,0,0} when
     *                  nothing was repainted.  May be NULL if only the return value
     *                  is needed.
     *
     * @return true when at least one node was repainted and @p out contains a valid
     *         non-empty rectangle; false when the scene was already clean this frame.
     */
    bool er_get_dirty_rect(ERRect* out);

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

    /**
     * @brief Gives keyboard focus to a TextInput node.
     *
     * Blurs any previously focused input, marks the node dirty to draw the cursor,
     * and fires ER_EVENT_FOCUS on the new node and ER_EVENT_BLUR on the old one.
     *
     * @param[in] node  TextInput node to focus, or NULL to only blur.
     */
    void er_text_input_focus(ERNode* node);

    /**
     * @brief Removes keyboard focus from whichever TextInput currently has it.
     *
     * Fires ER_EVENT_BLUR on the previously focused node and marks it dirty.
     * No-op when no input is focused.
     */
    void er_text_input_blur(void);

    /**
     * @brief Returns a pointer to the TextInput node's current text buffer.
     *
     * The returned pointer is valid until the node is destroyed or its text is
     * modified by er_text_input_set_text() or a key event.
     *
     * @param[in] node  TextInput node.
     *
     * @return Null-terminated UTF-8 string, or NULL if node is not a TextInput.
     */
    const char* er_text_input_get_text(const ERNode* node);

    /**
     * @brief Replaces the TextInput node's current text with the supplied string.
     *
     * Fires ER_EVENT_CHANGE_TEXT if the text changed.  Truncates at ER_TEXT_MAX
     * characters.
     *
     * @param[in] node  TextInput node.
     * @param[in] text  Null-terminated UTF-8 string.
     */
    void er_text_input_set_text(ERNode* node, const char* text);

    /**
     * @brief Sets inline text spans on a Text node, enabling per-span style overrides.
     *
     * When count > 0 the node renders using the provided span array instead of its
     * text[] string.  Each span contributes a UTF-8 text fragment; style fields left at
     * their sentinel values inherit from the node's base ERProps: color == 0,
     * font_size == 0, font_weight == 0xFF, font_style == 0xFF, text_decoration == 0xFF,
     * letter_spacing == ER_LAYOUT_AUTO.  Passing count == 0 or spans == NULL reverts to
     * single-string rendering.
     *
     * @param[in] node   Text node to configure.
     * @param[in] spans  Array of span descriptors; may be NULL when count == 0.
     * @param[in] count  Number of spans; clamped to ER_TEXT_MAX_SPANS.
     */
    void er_node_set_text_spans(ERNode* node, const ERTextSpan* spans, uint8_t count);

    /**
     * @brief Sets the vector op-tape and paint table on an ER_NODE_VECTOR (Svg) node.
     *
     * The node re-rasterizes its shapes into its layout box on the next paint. The op-tape and paints
     * are copied into engine-owned storage (bounded; excess is dropped). Passing n_ops == 0 clears the
     * node's geometry. The node takes its size from its layout/style (width/height), not the tape.
     *
     * @param[in] node      Vector node to configure (no-op for other node types).
     * @param[in] ops       Flat op-tape (see the ER_VOP_* contract above).
     * @param[in] n_ops     Number of floats in @p ops.
     * @param[in] paints    Paint table (one ERVectorPaint per shape).
     * @param[in] n_paints  Number of paint entries.
     */
    void er_node_set_vector_ops(ERNode* node, const float* ops, int n_ops, const ERVectorPaint* paints, int n_paints);

    /*----------------------------------------------------------------------------------------------------------------------
     - Layout animation
     ---------------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief 300 ms timing animation with ease-in-out cubic.
     *
     * Mirrors React Native's LayoutAnimation.Presets.easeInEaseOut.
     */
    extern const ERLayoutAnimConfig ER_LAYOUT_ANIM_EASE_IN_EASE_OUT;

    /**
     * @brief 300 ms timing animation with linear easing.
     *
     * Mirrors React Native's LayoutAnimation.Presets.linear.
     */
    extern const ERLayoutAnimConfig ER_LAYOUT_ANIM_LINEAR;

    /**
     * @brief Spring animation using the default React Native spring parameters.
     *
     * Mirrors React Native's LayoutAnimation.Presets.spring.
     */
    extern const ERLayoutAnimConfig ER_LAYOUT_ANIM_SPRING;

    /**
     * @brief Registers a layout animation config to apply on the next er_commit().
     *
     * On the next commit every node whose computed layout rectangle changed from the
     * previous commit will animate from its current display position to the new computed
     * position using this config.  The config is consumed after one commit and has no
     * effect on subsequent commits unless er_layout_anim_configure_next() is called again.
     *
     * Nodes that are appearing for the first time (prev computed rect was zero-size) are
     * snapped to their target immediately without animation; they are treated as creations
     * rather than updates.
     *
     * @param[in] cfg  Layout animation config. NULL is a no-op.
     */
    void er_layout_anim_configure_next(const ERLayoutAnimConfig* cfg);

    /**
     * @brief Reports whether a layout-animation config is armed for the next commit.
     *
     * Returns true between an er_layout_anim_configure_next() call and the er_commit() that
     * consumes it; false otherwise.  Useful for coordinating layout transitions (e.g. avoid
     * arming a second config before the first is consumed).
     *
     * @return true when a configured layout animation has not yet been consumed by a commit.
     */
    bool er_layout_anim_has_pending(void);

#ifdef __cplusplus
}
#endif

#endif
