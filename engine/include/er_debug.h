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

/*
 * Where the engine keeps its pools and nodes, for a debugger reading a halted board. Nothing reads it on the board:
 * the GDB script finds it by its symbol, so it doesn't depend on the engine's private names or struct layouts.
 */

#ifndef ER_DEBUG_H
#define ER_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief ERDebug::magic: "ERDE". */
#define ER_DEBUG_MAGIC 0x45445245u

/** @brief Raised when a field of ERDebug or ERDebugPool changes, or a prop of ERDebugProp. */
#define ER_DEBUG_VERSION 3u

    /** @brief The engine's pools, in ERDebug::pools. */
    typedef enum
    {
        ER_DEBUG_POOL_NODES = 0,
        ER_DEBUG_POOL_VECTOR_NODES,
        ER_DEBUG_POOL_ANIMATIONS,
        ER_DEBUG_POOL_ANIMATION_GROUPS,
        ER_DEBUG_POOL_ANIMATION_VALUES,
        ER_DEBUG_POOL_IMAGES,
        ER_DEBUG_POOL_FONTS,
        ER_DEBUG_POOL_COUNT_,
    } ERDebugPoolId;

    /** @brief A pool: an array of records, each with a bool that is set while it is in use. */
    typedef struct
    {
        const void* records;
        uint16_t count;   /**< Records in the array. */
        uint16_t stride;  /**< Bytes from one record to the next. */
        uint16_t used_at; /**< The in-use bool's offset in a record. */
        uint16_t reserved;
    } ERDebugPool;

    /**
     * @brief The props a debugger shows for a node, in ERDebugNodeFields::props. A LAYOUT_ prop is an int16_t,
     *        ER_LAYOUT_AUTO when unset, or a uint8_t enum, and a _PERCENT one a float. The others are as their
     *        field in the node is. VIEW_ props are those of a View, ScrollView, FlatList, Pressable and Modal.
     */
    typedef enum
    {
        ER_DEBUG_PROP_LAYOUT_LEFT = 0,
        ER_DEBUG_PROP_LAYOUT_TOP,
        ER_DEBUG_PROP_LAYOUT_RIGHT,
        ER_DEBUG_PROP_LAYOUT_BOTTOM,
        ER_DEBUG_PROP_LAYOUT_WIDTH,
        ER_DEBUG_PROP_LAYOUT_HEIGHT,
        ER_DEBUG_PROP_LAYOUT_MIN_WIDTH,
        ER_DEBUG_PROP_LAYOUT_MAX_WIDTH,
        ER_DEBUG_PROP_LAYOUT_MIN_HEIGHT,
        ER_DEBUG_PROP_LAYOUT_MAX_HEIGHT,
        ER_DEBUG_PROP_LAYOUT_PADDING,
        ER_DEBUG_PROP_LAYOUT_PADDING_LEFT,
        ER_DEBUG_PROP_LAYOUT_PADDING_TOP,
        ER_DEBUG_PROP_LAYOUT_PADDING_RIGHT,
        ER_DEBUG_PROP_LAYOUT_PADDING_BOTTOM,
        ER_DEBUG_PROP_LAYOUT_MARGIN,
        ER_DEBUG_PROP_LAYOUT_MARGIN_LEFT,
        ER_DEBUG_PROP_LAYOUT_MARGIN_TOP,
        ER_DEBUG_PROP_LAYOUT_MARGIN_RIGHT,
        ER_DEBUG_PROP_LAYOUT_MARGIN_BOTTOM,
        ER_DEBUG_PROP_LAYOUT_GAP,
        ER_DEBUG_PROP_LAYOUT_ROW_GAP,
        ER_DEBUG_PROP_LAYOUT_COLUMN_GAP,
        ER_DEBUG_PROP_LAYOUT_FLEX_GROW,
        ER_DEBUG_PROP_LAYOUT_FLEX_SHRINK,
        ER_DEBUG_PROP_LAYOUT_FLEX_BASIS,
        ER_DEBUG_PROP_LAYOUT_FLEX_DIRECTION,
        ER_DEBUG_PROP_LAYOUT_FLEX_WRAP,
        ER_DEBUG_PROP_LAYOUT_ALIGN_ITEMS,
        ER_DEBUG_PROP_LAYOUT_ALIGN_SELF,
        ER_DEBUG_PROP_LAYOUT_ALIGN_CONTENT,
        ER_DEBUG_PROP_LAYOUT_JUSTIFY_CONTENT,
        ER_DEBUG_PROP_LAYOUT_POSITION,
        ER_DEBUG_PROP_LAYOUT_DISPLAY,
        ER_DEBUG_PROP_LAYOUT_OVERFLOW,
        ER_DEBUG_PROP_LAYOUT_ASPECT_RATIO,
        ER_DEBUG_PROP_LAYOUT_FLEX_BASIS_PERCENT,
        ER_DEBUG_PROP_LAYOUT_WIDTH_PERCENT,
        ER_DEBUG_PROP_LAYOUT_HEIGHT_PERCENT,
        ER_DEBUG_PROP_LAYOUT_LEFT_PERCENT,
        ER_DEBUG_PROP_LAYOUT_TOP_PERCENT,
        ER_DEBUG_PROP_LAYOUT_RIGHT_PERCENT,
        ER_DEBUG_PROP_LAYOUT_BOTTOM_PERCENT,
        ER_DEBUG_PROP_Z_INDEX,
        ER_DEBUG_PROP_TRANSLATE_X,
        ER_DEBUG_PROP_TRANSLATE_Y,
        ER_DEBUG_PROP_SCALE_X,
        ER_DEBUG_PROP_SCALE_Y,
        ER_DEBUG_PROP_ROTATE,
        ER_DEBUG_PROP_ROTATE_X,
        ER_DEBUG_PROP_ROTATE_Y,
        ER_DEBUG_PROP_PERSPECTIVE,
        ER_DEBUG_PROP_VIEW_BACKGROUND_COLOR,
        ER_DEBUG_PROP_VIEW_BACKGROUND_IMAGE,
        ER_DEBUG_PROP_VIEW_BORDER_COLOR,
        ER_DEBUG_PROP_VIEW_BORDER_WIDTH,
        ER_DEBUG_PROP_VIEW_BORDER_LEFT_WIDTH,
        ER_DEBUG_PROP_VIEW_BORDER_TOP_WIDTH,
        ER_DEBUG_PROP_VIEW_BORDER_RIGHT_WIDTH,
        ER_DEBUG_PROP_VIEW_BORDER_BOTTOM_WIDTH,
        ER_DEBUG_PROP_VIEW_BORDER_STYLE,
        ER_DEBUG_PROP_VIEW_BORDER_RADIUS,
        ER_DEBUG_PROP_VIEW_BORDER_TOP_LEFT_RADIUS,
        ER_DEBUG_PROP_VIEW_BORDER_TOP_RIGHT_RADIUS,
        ER_DEBUG_PROP_VIEW_BORDER_BOTTOM_RIGHT_RADIUS,
        ER_DEBUG_PROP_VIEW_BORDER_BOTTOM_LEFT_RADIUS,
        ER_DEBUG_PROP_VIEW_OPACITY,
        ER_DEBUG_PROP_VIEW_SHADOW_OPACITY,
        ER_DEBUG_PROP_VIEW_SHADOW_RADIUS,
        ER_DEBUG_PROP_VIEW_ELEVATION,
        ER_DEBUG_PROP_MODAL_VISIBLE,
        ER_DEBUG_PROP_MODAL_BACKDROP_COLOR,
        ER_DEBUG_PROP_TEXT_TEXT,
        ER_DEBUG_PROP_TEXT_COLOR,
        ER_DEBUG_PROP_TEXT_FONT_FAMILY,
        ER_DEBUG_PROP_TEXT_FONT_SIZE,
        ER_DEBUG_PROP_TEXT_FONT_WEIGHT,
        ER_DEBUG_PROP_TEXT_FONT_STYLE,
        ER_DEBUG_PROP_TEXT_TEXT_ALIGN,
        ER_DEBUG_PROP_TEXT_NUMBER_OF_LINES,
        ER_DEBUG_PROP_TEXT_LINE_HEIGHT,
        ER_DEBUG_PROP_TEXT_LETTER_SPACING,
        ER_DEBUG_PROP_TEXT_TEXT_DECORATION,
        ER_DEBUG_PROP_TEXT_OPACITY,
        ER_DEBUG_PROP_IMAGE_SOURCE,
        ER_DEBUG_PROP_IMAGE_RESIZE_MODE,
        ER_DEBUG_PROP_IMAGE_TINT_COLOR,
        ER_DEBUG_PROP_IMAGE_OPACITY,
        ER_DEBUG_PROP_INPUT_VALUE,
        ER_DEBUG_PROP_INPUT_PLACEHOLDER,
        ER_DEBUG_PROP_INPUT_COLOR,
        ER_DEBUG_PROP_INPUT_FONT_FAMILY,
        ER_DEBUG_PROP_INPUT_FONT_SIZE,
        ER_DEBUG_PROP_INPUT_BACKGROUND_COLOR,
        ER_DEBUG_PROP_INPUT_BORDER_COLOR,
        ER_DEBUG_PROP_INPUT_BORDER_WIDTH,
        ER_DEBUG_PROP_INPUT_BORDER_RADIUS,
        ER_DEBUG_PROP_INPUT_OPACITY,
        ER_DEBUG_PROP_SWITCH_VALUE,
        ER_DEBUG_PROP_SWITCH_TRACK_COLOR_FALSE,
        ER_DEBUG_PROP_SWITCH_TRACK_COLOR_TRUE,
        ER_DEBUG_PROP_SWITCH_THUMB_COLOR,
        ER_DEBUG_PROP_INDICATOR_COLOR,
        ER_DEBUG_PROP_INDICATOR_ANIMATING,
        ER_DEBUG_PROP_ARC_VALUE,
        ER_DEBUG_PROP_ARC_MIN,
        ER_DEBUG_PROP_ARC_MAX,
        ER_DEBUG_PROP_ARC_START_ANGLE,
        ER_DEBUG_PROP_ARC_SWEEP_ANGLE,
        ER_DEBUG_PROP_ARC_WIDTH,
        ER_DEBUG_PROP_ARC_TRACK_COLOR,
        ER_DEBUG_PROP_ARC_INDICATOR_COLOR,
        ER_DEBUG_PROP_COUNT_,
    } ERDebugProp;

    /** @brief Offsets in a node record (ER_DEBUG_POOL_NODES). Tags are uint16_t, ER_INVALID_TAG for none. */
    typedef struct
    {
        uint16_t tag_at;
        uint16_t parent_at;
        uint16_t first_child_at;
        uint16_t next_sibling_at;
        uint16_t serial_at; /**< uint32_t. */
        uint16_t type_at;   /**< An ERNodeType, type_bytes long. */
        uint16_t type_bytes;
        uint16_t rect_at;     /**< The laid-out rect: int16_t x, y, width, height, on the screen before scrolling. */
        uint16_t scroll_x_at; /**< float: a ScrollView's offset, which moves its children. */
        uint16_t scroll_y_at;
        uint16_t props[ER_DEBUG_PROP_COUNT_]; /**< Each ERDebugProp's offset. */
    } ERDebugNodeFields;

    /** @brief A framebuffer's pixel format, in ERDebugScreen::format. */
    typedef enum
    {
        ER_DEBUG_PIXELS_RGB565 = 0, /**< 2 bytes a pixel. */
        ER_DEBUG_PIXELS_RGB888,     /**< 3 bytes a pixel: blue, green, red. */
        ER_DEBUG_PIXELS_ARGB8888,   /**< 4 bytes a pixel, 0xAARRGGBB. */
    } ERDebugPixelFormat;

    /**
     * @brief The framebuffer the panel shows, for a debugger to draw on while the board is halted. A backend whose
     *        panel is scanned from memory by the chip itself, halted or not, keeps it with er_debug_set_screen(). For
     *        the rest pixels stays NULL.
     */
    typedef struct
    {
        void* pixels;      /**< The top left pixel of the buffer the panel shows, or NULL. */
        uint16_t width;    /**< The panel's width as it scans, in pixels. */
        uint16_t height;   /**< The panel's height as it scans. */
        uint16_t stride;   /**< Bytes from one row to the next. */
        uint16_t rotation; /**< An ERDisplayRotation: how far the app's picture is turned onto the panel. */
        uint8_t format;    /**< An ERDebugPixelFormat. */
    } ERDebugScreen;

    /** @brief The descriptor. */
    typedef struct
    {
        uint32_t magic;
        uint16_t version;
        uint16_t pool_count; /**< ER_DEBUG_POOL_COUNT_. */
        const ERDebugPool* pools[ER_DEBUG_POOL_COUNT_];
        ERDebugNodeFields node;
        const uint16_t* root_tag; /**< The root node's tag. */
        volatile bool* halted;    /**< Set by a debugger when the board halts: the next embedded_renderer_tick() counts
                                       no time. */
        const ERDebugScreen* screen;
    } ERDebug;

    extern const ERDebug er_debug;

    /**
     * @brief Says which framebuffer the panel shows. A backend calls it when it starts and at each page flip.
     *
     * @param[in] pixels        The top left pixel of the buffer the panel shows, or NULL for none.
     * @param[in] stride_bytes  Bytes from one row to the next.
     * @param[in] format        The buffer's pixel format.
     */
    void er_debug_set_screen(void* pixels, int stride_bytes, ERDebugPixelFormat format);

#ifdef __cplusplus
}
#endif

#endif /* ER_DEBUG_H */
