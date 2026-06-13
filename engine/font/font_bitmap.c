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

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

const GlyphInfo* font_glyph(const BitmapFont* font, uint32_t codepoint)
{
    if (codepoint >= font->first && codepoint <= font->last)
        return &font->glyphs[codepoint - font->first];

    if (font->extras_count > 0U && font->extras)
    {
        uint32_t lo = 0U;
        uint32_t hi = font->extras_count;
        while (lo < hi)
        {
            const uint32_t mid = lo + ((hi - lo) >> 1);
            const uint32_t mcp = font->extras[mid].codepoint;
            if (mcp == codepoint)
                return &font->extras[mid].info;
            if (mcp < codepoint)
                lo = mid + 1U;
            else
                hi = mid;
        }
    }

    if ((uint32_t)'?' >= font->first && (uint32_t)'?' <= font->last)
        return &font->glyphs[(uint32_t)'?' - font->first];
    return &font->glyphs[0];
}
