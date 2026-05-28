#ifndef EMBEDDED_REACT_TEXT_RENDERER_H
#define EMBEDDED_REACT_TEXT_RENDERER_H

#include "er_scene.h"
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Renders a UTF-8 text string into a clipping rectangle.
 *
 * Words are wrapped when a glyph advance exceeds the right edge of the clip.
 * Rendering stops when the next line falls below the bottom of the clip.
 * Codepoints outside the font's coverage fall back to '?'.
 *
 * @param[in] text        Null-terminated UTF-8 string to render.
 * @param[in] clip        Clipping and layout rectangle in framebuffer coordinates.
 * @param[in] color       Text color as straight-alpha ARGB8888.
 * @param[in] font_size   Desired font size in pixels (clamped to [8, 96]).
 * @param[in] font_family Null-terminated font family name, or NULL for the default.
 */
void er_text_render(const char* text, ERRect clip, uint32_t color, uint8_t font_size, const char* font_family);

/**
 * @brief Measures the pixel dimensions of a UTF-8 string without rendering it.
 *
 * The returned width is the sum of glyph advances for all codepoints on a single
 * line (newlines are ignored). The height is the font's line height.
 *
 * @param[in]  text        Null-terminated UTF-8 string to measure.
 * @param[in]  font_size   Desired font size in pixels (clamped to [8, 96]).
 * @param[in]  font_family Null-terminated font family name, or NULL for the default.
 * @param[out] out_width   Receives the measured width in pixels, or 0 if the text is NULL.
 * @param[out] out_height  Receives the font's line height in pixels.
 */
void er_text_measure(const char* text, uint8_t font_size, const char* font_family, int* out_width, int* out_height);

#endif
