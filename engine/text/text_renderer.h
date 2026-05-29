#ifndef EMBEDDED_REACT_TEXT_RENDERER_H
#define EMBEDDED_REACT_TEXT_RENDERER_H

#include "er_scene.h"
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Parameters passed to er_text_render().
 *
 * Zero-initialise and set only the fields you need.  Defaults produce left-aligned
 * word-wrapped text at the font's natural line height with no decoration.
 */
typedef struct ERTextRenderParams
{
    const char* text;        /**< Null-terminated UTF-8 string. May be NULL (no-op). */
    ERRect clip;             /**< Clipping and layout rectangle in framebuffer coordinates. */
    uint32_t color;          /**< Straight-alpha ARGB8888 text color. */
    uint8_t font_size;       /**< Desired font size in pixels (clamped to [8, 96]). */
    const char* font_family; /**< Font family name, or NULL for the built-in default. */
    uint8_t text_align;      /**< ERTextAlign — default ER_TEXT_ALIGN_LEFT. */
    uint8_t number_of_lines; /**< Maximum rendered lines; 0 = unlimited. */
    uint8_t ellipsize_mode;  /**< ERTextEllipsize — applied when number_of_lines truncates. */
    uint8_t text_decoration; /**< ERTextDecoration — default none. */
    int16_t line_height;     /**< Line height in pixels; 0 = use the font's natural value. */
    int16_t letter_spacing;  /**< Extra pixels added to each glyph advance; may be negative. */
} ERTextRenderParams;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Renders UTF-8 text into a clipping rectangle with full layout control.
 *
 * Text wraps on word boundaries when adding the next word would exceed the right edge
 * of clip.  Falls back to character-boundary wrapping for single words wider than clip.
 * Rendering stops when the next line falls below the bottom of clip or number_of_lines
 * is reached.  Codepoints outside the font's coverage fall back to '?'.
 *
 * @param[in] params  Rendering parameters; must not be NULL.
 */
void er_text_render(const ERTextRenderParams* params);

/**
 * @brief Measures the pixel dimensions of a UTF-8 string without rendering it.
 *
 * Computes the width of all codepoints on a single line (newlines are ignored) plus
 * letter_spacing for each glyph.  The height is the font's natural line height.
 *
 * @param[in]  text           Null-terminated UTF-8 string to measure.
 * @param[in]  font_size      Desired font size in pixels (clamped to [8, 96]).
 * @param[in]  font_family    Font family name, or NULL for the built-in default.
 * @param[in]  letter_spacing Extra pixels per glyph advance (same as ERTextRenderParams).
 * @param[out] out_width      Receives the measured width in pixels.
 * @param[out] out_height     Receives the font's line height in pixels.
 */
void er_text_measure(const char* text,
                     uint8_t font_size,
                     const char* font_family,
                     int16_t letter_spacing,
                     int* out_width,
                     int* out_height);

#endif
