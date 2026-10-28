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

#ifndef ERPK_PACK_H
#define ERPK_PACK_H

/*
 * A tiny ERPK v2 writer for tests: 1x1 images and a one-glyph font, in the layout the emitter writes.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief An image format no pack version defines. */
#define TPACK_UNKNOWN_IMAGE_FORMAT 7u

/** @brief A font format the engine can't draw. */
#define TPACK_UNKNOWN_FONT_FORMAT 9u

typedef struct
{
    uint8_t bytes[512];
    size_t length;
} TestPack;

static void tpack_u8(TestPack* pack, uint8_t value)
{
    pack->bytes[pack->length++] = value;
}

static void tpack_u16(TestPack* pack, uint16_t value)
{
    tpack_u8(pack, (uint8_t)(value & 0xFFu));
    tpack_u8(pack, (uint8_t)(value >> 8));
}

static void tpack_u32(TestPack* pack, uint32_t value)
{
    tpack_u16(pack, (uint16_t)(value & 0xFFFFu));
    tpack_u16(pack, (uint16_t)(value >> 16));
}

static void tpack_name(TestPack* pack, const char* name)
{
    const uint16_t name_length = (uint16_t)strlen(name);
    tpack_u16(pack, name_length);
    memcpy(pack->bytes + pack->length, name, name_length);
    pack->length += name_length;
}

/** @brief Starts a version 2 pack that says it holds @p image_count images and @p font_count fonts. */
static void tpack_begin(TestPack* pack, uint32_t image_count, uint32_t font_count)
{
    memcpy(pack->bytes, "ERPK", 4);
    pack->length = 4u;
    tpack_u32(pack, 2u);
    tpack_u32(pack, image_count);
    tpack_u32(pack, font_count);
}

/** @brief Adds a 1x1 image (format 0 = ARGB8888, 1 = RGB565). */
static void tpack_image(TestPack* pack, const char* name, uint8_t format)
{
    tpack_name(pack, name);
    tpack_u32(pack, 1u);
    tpack_u32(pack, 1u);
    tpack_u8(pack, format);
    while (pack->length & 3u)
    {
        tpack_u8(pack, 0u);
    }
    const size_t pixel_bytes = format == 1u ? 2u : 4u;
    memset(pack->bytes + pack->length, 0xFF, pixel_bytes);
    pack->length += pixel_bytes;
}

/** @brief Adds a font of family @p family with one 1x1 glyph for 'A'. */
static void tpack_font(TestPack* pack, const char* family, uint8_t format)
{
    tpack_name(pack, family);
    tpack_u8(pack, 16u); /* pixel size */
    tpack_u8(pack, 18u); /* line height */
    tpack_u8(pack, 14u); /* baseline */
    tpack_u8(pack, format);
    tpack_u16(pack, 'A');
    tpack_u16(pack, 'A');
    tpack_u16(pack, 1u); /* glyphs */
    tpack_u16(pack, 0u); /* extras */
    tpack_u32(pack, 1u); /* bitmap bytes */
    tpack_u32(pack, 0u); /* glyph: bitmap offset, width, height, x and y offsets, advance */
    tpack_u8(pack, 1u);
    tpack_u8(pack, 1u);
    tpack_u8(pack, 0u);
    tpack_u8(pack, 0u);
    tpack_u8(pack, 1u);
    tpack_u8(pack, 0xFFu); /* bitmap */
}

#endif
