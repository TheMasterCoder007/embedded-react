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
