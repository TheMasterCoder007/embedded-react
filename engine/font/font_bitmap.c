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

#include "font_bitmap.h"

/** @brief Drawn for a codepoint the font has no glyph for, when the font has this one. */
#define FONT_FALLBACK_CODEPOINT ((uint32_t)'?')

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

const GlyphInfo* font_glyph(const BitmapFont* font, uint32_t codepoint)
{
    if (codepoint >= font->first && codepoint <= font->last)
        return &font->glyphs[codepoint - font->first];

    if (font->extras_count > 0U && font->extras)
    {
        uint32_t low = 0U;
        uint32_t high = font->extras_count;
        while (low < high)
        {
            const uint32_t middle = low + ((high - low) >> 1);
            const uint32_t middle_codepoint = font->extras[middle].codepoint;
            if (middle_codepoint == codepoint)
                return &font->extras[middle].info;
            if (middle_codepoint < codepoint)
                low = middle + 1U;
            else
                high = middle;
        }
    }

    if (FONT_FALLBACK_CODEPOINT >= font->first && FONT_FALLBACK_CODEPOINT <= font->last)
        return &font->glyphs[FONT_FALLBACK_CODEPOINT - font->first];
    return &font->glyphs[0];
}
