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

#include "er_assets.h"
#include "erpk_pack.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_failed = 0;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL: %s\n", (message));                                                                           \
            s_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static TestPack s_pack;

static void clear_registries(void)
{
    er_image_clear();
    er_font_clear();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_loads_a_good_pack(void)
{
    clear_registries();
    tpack_begin(&s_pack, 2u, 1u);
    tpack_image(&s_pack, "argb", 0u);
    tpack_image(&s_pack, "rgb565", 1u);
    tpack_font(&s_pack, "f", ERUI_FONT_FMT_1BIT);
    CHECK(er_assets_load_pack(s_pack.bytes, s_pack.length), "a good pack loads");
    CHECK(er_image_exists("argb") && er_image_exists("rgb565"), "both images are registered");
}

static void test_unknown_image_format_registers_nothing(void)
{
    clear_registries();
    tpack_begin(&s_pack, 2u, 0u);
    tpack_image(&s_pack, "early", 0u);
    tpack_image(&s_pack, "late", TPACK_UNKNOWN_IMAGE_FORMAT);
    CHECK(!er_assets_load_pack(s_pack.bytes, s_pack.length), "an unknown image format fails the load");
    CHECK(!er_image_exists("early"), "the image before it isn't registered");
}

static void test_bad_font_registers_nothing(void)
{
    clear_registries();
    tpack_begin(&s_pack, 1u, 1u);
    tpack_image(&s_pack, "early", 0u);
    tpack_font(&s_pack, "f", TPACK_UNKNOWN_FONT_FORMAT);
    CHECK(!er_assets_load_pack(s_pack.bytes, s_pack.length), "an unknown font format fails the load");
    CHECK(!er_image_exists("early"), "the image before it isn't registered");
}

static void test_truncated_pack_registers_nothing(void)
{
    clear_registries();
    tpack_begin(&s_pack, 1u, 1u);
    tpack_image(&s_pack, "early", 0u);
    tpack_font(&s_pack, "f", ERUI_FONT_FMT_1BIT);
    CHECK(!er_assets_load_pack(s_pack.bytes, s_pack.length - 1u), "a record past the end fails the load");
    CHECK(!er_image_exists("early"), "the image before it isn't registered");
}

static void test_copy_registers_nothing_on_failure(void)
{
    clear_registries();
    tpack_begin(&s_pack, 2u, 0u);
    tpack_image(&s_pack, "early", 0u);
    tpack_image(&s_pack, "late", TPACK_UNKNOWN_IMAGE_FORMAT);
    CHECK(!er_assets_load_pack_ex(s_pack.bytes, s_pack.length, true), "a bad pack fails the copying load");
    CHECK(!er_image_exists("early"), "nothing is registered from the copy");

    tpack_begin(&s_pack, 1u, 1u);
    tpack_image(&s_pack, "copied", 0u);
    tpack_font(&s_pack, "f", ERUI_FONT_FMT_1BIT);
    CHECK(er_assets_load_pack_ex(s_pack.bytes, s_pack.length, true), "a good pack loads from a copy");
    CHECK(er_image_exists("copied"), "the copy's image is registered");
}

static void test_storage_that_doesnt_fit_registers_nothing(void)
{
    clear_registries();
    tpack_begin(&s_pack, 1u, 1u);
    tpack_image(&s_pack, "early", 0u);
    tpack_font(&s_pack, "f", ERUI_FONT_FMT_1BIT);

    static BitmapFont fonts[1];
    static GlyphInfo glyphs[1];
    ErAssetsStorage no_glyph_room = {fonts, 1u, glyphs, 0u, NULL, 0u, 0u, 0u, 0u};
    CHECK(!er_assets_load_pack_into(s_pack.bytes, s_pack.length, &no_glyph_room), "no room for the glyphs");
    CHECK(!er_image_exists("early"), "the image before the font isn't registered");

    ErAssetsStorage room = {fonts, 1u, glyphs, 1u, NULL, 0u, 0u, 0u, 0u};
    CHECK(er_assets_load_pack_into(s_pack.bytes, s_pack.length, &room), "the same pack with room");
    CHECK(er_image_exists("early"), "the image is registered");
    CHECK(room.fonts_used == 1u && room.glyphs_used == 1u && room.extras_used == 0u, "the storage it took");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    test_loads_a_good_pack();
    test_unknown_image_format_registers_nothing();
    test_bad_font_registers_nothing();
    test_truncated_pack_registers_nothing();
    test_copy_registers_nothing_on_failure();
    test_storage_that_doesnt_fit_registers_nothing();

    if (s_failed)
    {
        return 1;
    }
    printf("assets: all tests passed\n");
    return 0;
}
