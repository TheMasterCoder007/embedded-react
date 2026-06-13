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

#include "er_scene.h"
#include "font_blob.h"
#include "font_registry.h"
#include "image_registry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_fail = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            printf("FAIL: %s\n", (msg));                                                                               \
            s_fail = 1;                                                                                                \
        }                                                                                                              \
    } while (0)

/**
 * @brief Writes a little-endian uint16_t into a byte buffer.
 *
 * @param[out] p  Destination (2 bytes).
 * @param[in]  v  Value to write.
 */
static void put_u16_le(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

/**
 * @brief Writes a little-endian uint32_t into a byte buffer.
 *
 * @param[out] p  Destination (4 bytes).
 * @param[in]  v  Value to write.
 */
static void put_u32_le(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

/**
 * @brief Builds a minimal valid FONT wire-format blob into dst.
 *
 * Produces a single-glyph (or multi-glyph) version-2 font with a solid bitmap so the blob
 * passes font_blob_register()'s validation. The total size is returned via out_size.
 *
 * @param[out] dst         Destination buffer (must be large enough).
 * @param[in]  first       First character code.
 * @param[in]  last        Last character code (>= first).
 * @param[in]  pixel_size  Nominal pixel size.
 * @param[out] out_size    Receives the total blob size in bytes.
 */
static void build_blob(uint8_t* dst, uint8_t first, uint8_t last, uint8_t pixel_size, uint32_t* out_size)
{
    const uint16_t glyph_count = (uint16_t)(last - first + 1U);
    const uint32_t bitmap_size = 4U;

    dst[0] = 0x46U; /* 'F' */
    dst[1] = 0x4FU; /* 'O' */
    dst[2] = 0x4EU; /* 'N' */
    dst[3] = 0x54U; /* 'T' */
    dst[4] = 2U;    /* version */
    dst[5] = first;
    dst[6] = last;
    dst[7] = pixel_size;
    dst[8] = 18U; /* line_height */
    dst[9] = 14U; /* baseline */
    dst[10] = 0U; /* format */
    put_u16_le(&dst[11], glyph_count);
    put_u32_le(&dst[13], bitmap_size);

    uint8_t* g = &dst[17];
    for (uint16_t i = 0; i < glyph_count; i++)
    {
        put_u16_le(&g[0], 0U); /* bitmap_offset */
        g[2] = 2U;             /* width */
        g[3] = 2U;             /* height */
        g[4] = 0U;             /* x_offset */
        g[5] = 0U;             /* y_offset */
        g[6] = 3U;             /* advance */
        g[7] = 0U;             /* unused */
        g += 8;
    }
    for (uint32_t i = 0; i < bitmap_size; i++)
        g[i] = 0xFFU;

    *out_size = 17U + (uint32_t)glyph_count * 8U + bitmap_size;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Image registry replace
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies er_image_load replaces an existing name in place without consuming a new slot.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_image_replace(void)
{
    image_registry_init();

    static uint32_t buf_a[4] = {0xFF112233U, 0xFF112233U, 0xFF112233U, 0xFF112233U};
    static uint32_t buf_b[1] = {0xFFAABBCCU};

    er_image_load("logo", buf_a, 2, 2);
    const ImageEntry* e = image_registry_get("logo");
    CHECK(e != NULL && e->w == 2 && e->h == 2 && e->buf == buf_a, "image: initial registration");

    /* Re-register the same name with a different buffer and dimensions. */
    er_image_load("logo", buf_b, 1, 1);
    e = image_registry_get("logo");
    CHECK(e != NULL && e->w == 1 && e->h == 1 && e->buf == buf_b, "image: replace in place");

    /* Fill the registry to capacity, then re-register an existing name: must still succeed
     * (replace does not consume a free slot). */
    char name[16];
    for (int i = 1; i < (int)IMAGE_REGISTRY_MAX; i++) /* slot 0 holds "logo" */
    {
        snprintf(name, sizeof(name), "img%d", i);
        const bool ok = image_registry_store(name, buf_a, 1, 1);
        CHECK(ok, "image: fill to capacity");
    }
    CHECK(image_registry_store("logo", buf_a, 2, 2), "image: replace when registry is full");
    CHECK(!image_registry_store("overflow", buf_a, 1, 1), "image: new name rejected when full");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Font registry replace
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies font_registry_add replaces a same-pixel_size entry instead of appending.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_font_registry_replace(void)
{
    font_registry_init();

    BitmapFont f16a = {0};
    f16a.pixel_size = 16U;
    f16a.line_height = 20U;
    BitmapFont f16b = {0};
    f16b.pixel_size = 16U;
    f16b.line_height = 24U;
    BitmapFont f24 = {0};
    f24.pixel_size = 24U;
    f24.line_height = 30U;

    CHECK(font_registry_add("Fam", &f16a), "registry: add 16a");
    CHECK(font_registry_get_exact("Fam", 16U) == &f16a, "registry: exact returns 16a");

    /* Same name + same pixel_size replaces in place. */
    CHECK(font_registry_add("Fam", &f16b), "registry: replace 16");
    CHECK(font_registry_get_exact("Fam", 16U) == &f16b, "registry: exact returns 16b after replace");

    /* A different pixel_size is a distinct entry, not a replacement. */
    CHECK(font_registry_add("Fam", &f24), "registry: add 24");
    CHECK(font_registry_get_exact("Fam", 24U) == &f24, "registry: exact returns 24");
    CHECK(font_registry_get_exact("Fam", 16U) == &f16b, "registry: 16 still present after adding 24");

    /* Replacing the same (name, size) many times must never exhaust the size table. */
    for (int i = 0; i < 100; i++)
        CHECK(font_registry_add("Fam", &f16a), "registry: repeated replace must not overflow");
    CHECK(font_registry_get_exact("Fam", 16U) == &f16a, "registry: final replace wins");

    /* Unknown lookups return NULL (no closest-match, no fallback). */
    CHECK(font_registry_get_exact("Fam", 99U) == NULL, "registry: exact miss on size");
    CHECK(font_registry_get_exact("Nope", 16U) == NULL, "registry: exact miss on name");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Font blob pool reuse
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies that re-registering the same (family, size) reuses pool bytes when it fits.
 *
 * Meaningful only when the font is built with ERUI_FONT_POOL_BYTES > 0; when the pool is
 * disabled, font_blob_register returns an error and the byte-level assertions are skipped
 * (the test still passes). The registry-replace behavior is covered by test_font_registry_replace.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_font_blob_reuse(void)
{
    font_registry_init();
    font_blob_init(0U);

    /* Large enough for the biggest blob built below: 17 header + 49*8 glyph + 4 bitmap = 413. */
    uint8_t blob[512];
    uint32_t size = 0;
    build_blob(blob, 32U, 33U, 16U, &size); /* 2 glyphs at size 16 */

    const FontBlobStatus st = font_blob_register("ReuseFont", blob, size);
    if (st != FONT_BLOB_OK)
    {
        /* Pool disabled at build time — nothing to exercise here. */
        return s_fail;
    }

    const uint32_t used1 = font_blob_used_bytes();
    CHECK(used1 > 0U, "blob: first registration consumed pool bytes");

    /* Identical re-register of the same (name, size): must repack in place, no growth. */
    CHECK(font_blob_register("ReuseFont", blob, size) == FONT_BLOB_OK, "blob: re-register succeeds");
    const uint32_t used2 = font_blob_used_bytes();
    CHECK(used2 == used1, "blob: identical re-register must not grow the pool");
    CHECK(font_registry_get_exact("ReuseFont", 16U) != NULL, "blob: font retrievable after reuse");

    /* A smaller blob for the same (name, size) also fits within the prior footprint. */
    uint32_t small_size = 0;
    build_blob(blob, 32U, 32U, 16U, &small_size); /* 1 glyph */
    CHECK(font_blob_register("ReuseFont", blob, small_size) == FONT_BLOB_OK, "blob: smaller re-register succeeds");
    CHECK(font_blob_used_bytes() == used1, "blob: smaller re-register stays within reserved footprint");

    /* A larger blob for the same (name, size) cannot fit: fresh bytes are consumed. */
    uint32_t big_size = 0;
    build_blob(blob, 32U, 80U, 16U, &big_size); /* 49 glyphs */
    CHECK(font_blob_register("ReuseFont", blob, big_size) == FONT_BLOB_OK, "blob: larger re-register succeeds");
    CHECK(font_blob_used_bytes() > used1, "blob: larger re-register allocates fresh bytes");
    CHECK(font_registry_get_exact("ReuseFont", 16U) != NULL, "blob: font retrievable after fresh alloc");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Public er_font_register
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies the public er_font_register() registers a baked font by pointer (zero-copy) and is
 *        resolvable by closest-size lookup, with NULL arguments treated as safe no-ops.
 *
 * This is the build-time font path the converter's generated C uses (no font pool, no copy).
 *
 * @return 0 on success, 1 on failure.
 */
static int test_font_register_public(void)
{
    font_registry_init();

    BitmapFont brand16 = {0};
    brand16.pixel_size = 16U;
    brand16.line_height = 20U;
    BitmapFont brand24 = {0};
    brand24.pixel_size = 24U;
    brand24.line_height = 30U;

    er_font_register("Brand", &brand16);
    er_font_register("Brand", &brand24);

    /* The exact baked struct is returned by pointer (never copied). */
    CHECK(font_registry_get_exact("Brand", 16U) == &brand16, "er_font_register: exact 16 by pointer");
    CHECK(font_registry_get_exact("Brand", 24U) == &brand24, "er_font_register: exact 24 by pointer");

    /* A fontSize between baked sizes resolves to the nearest registered one (no scaling). */
    CHECK(font_registry_get("Brand", 17U) == &brand16, "er_font_register: 17 picks nearest (16)");
    CHECK(font_registry_get("Brand", 22U) == &brand24, "er_font_register: 22 picks nearest (24)");

    /* NULL family or font are safe no-ops and must not register anything or crash. */
    er_font_register(NULL, &brand16);
    er_font_register("Bogus", NULL);
    CHECK(font_registry_get_exact("Bogus", 16U) == NULL, "er_font_register: NULL font did not register");

    /* An unknown family falls back to the built-in font (never NULL). */
    CHECK(font_registry_get("Unknown", 16U) != NULL, "er_font_register: unknown family falls back to built-in");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point for resource registry teardown/replace behavior.
 *
 * @return 0 on success, 1 on any failure.
 */
int main(void)
{
    (void)test_image_replace();
    (void)test_font_registry_replace();
    (void)test_font_blob_reuse();
    (void)test_font_register_public();

    if (s_fail)
        return 1;

    printf("OK\n");
    return 0;
}
