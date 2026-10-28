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
 * Container load test: an asset pack that fails to load fails the whole container, before anything is
 * registered or run.
 */

#include "er_container.h"
#include "er_runtime.h"
#include "er_scene.h"
#include "erpk_pack.h"
#include "host_harness.h"
#include "native_renderer.h"

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

/** @brief The QuickJS tag er_runtime accepts. */
static const char QJS_TAG[] = "v0.15.0";

/* Word-aligned, so where a section lands depends only on its offset. */
static uint32_t s_container_words[512];
static uint8_t* const s_container = (uint8_t*)s_container_words;
static size_t s_container_length;
static size_t s_pack_offset; /* where build_with_pack put the pack */

static void put_u32(uint8_t* destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFu);
    destination[1] = (uint8_t)((value >> 8) & 0xFFu);
    destination[2] = (uint8_t)((value >> 16) & 0xFFu);
    destination[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void container_begin(uint32_t section_count)
{
    const uint16_t tag_length = (uint16_t)(sizeof(QJS_TAG) - 1u);
    memcpy(s_container, "ERCF", 4);
    put_u32(s_container + 4, ER_CONTAINER_FORMAT_VERSION);
    s_container_length = 12u;
    s_container[s_container_length++] = (uint8_t)(tag_length & 0xFFu);
    s_container[s_container_length++] = (uint8_t)(tag_length >> 8);
    memcpy(s_container + s_container_length, QJS_TAG, tag_length);
    s_container_length += tag_length;
    put_u32(s_container + s_container_length, section_count);
    s_container_length += 4u;
}

static void container_section(uint32_t type, const void* data, size_t length)
{
    put_u32(s_container + s_container_length, type);
    put_u32(s_container + s_container_length + 4u, (uint32_t)length);
    s_container_length += 8u;
    memcpy(s_container + s_container_length, data, length);
    s_container_length += length;
}

static void container_seal(void)
{
    put_u32(s_container + 8, er_crc32(s_container + 12, s_container_length - 12u));
}

static const uint8_t s_bytecode[] = {0u};

/** @brief A container with one asset pack, then bytecode that throws if it runs. */
static void build_with_pack(const TestPack* pack)
{
    container_begin(2u);
    s_pack_offset = s_container_length + 8u;
    container_section(ER_SECTION_ASSET_PACK, pack->bytes, pack->length);
    container_section(ER_SECTION_BYTECODE, s_bytecode, sizeof(s_bytecode));
    container_seal();
}

/** @brief A container with a good pack, then a pack with an unknown image format, then bytecode. */
static void build_with_bad_pack(void)
{
    static TestPack good_pack;
    static TestPack bad_pack;
    tpack_begin(&good_pack, 1u, 0u);
    tpack_image(&good_pack, "good", 0u);
    tpack_begin(&bad_pack, 2u, 0u);
    tpack_image(&bad_pack, "early", 0u);
    tpack_image(&bad_pack, "late", TPACK_UNKNOWN_IMAGE_FORMAT);

    container_begin(3u);
    container_section(ER_SECTION_ASSET_PACK, good_pack.bytes, good_pack.length);
    container_section(ER_SECTION_ASSET_PACK, bad_pack.bytes, bad_pack.length);
    container_section(ER_SECTION_BYTECODE, s_bytecode, sizeof(s_bytecode));
    container_seal();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_bad_pack_fails_the_load(void)
{
    er_image_clear();
    build_with_bad_pack();
    CHECK(er_runtime_load_container(s_container, s_container_length) == ER_CONTAINER_BAD_ASSETS,
          "a bad pack fails the load with its own status");
    CHECK(!er_image_exists("good") && !er_image_exists("early"), "no pack in the container is registered");
    CHECK(er_runtime_load_container_ex(s_container, s_container_length, true) == ER_CONTAINER_BAD_ASSETS,
          "the same with copied assets");
    CHECK(!er_image_exists("good") && !er_image_exists("early"), "no pack is registered from the copy");
    CHECK(strcmp(er_runtime_container_status_str(ER_CONTAINER_BAD_ASSETS), "unknown") != 0,
          "the status has a reason to show");
}

static void test_too_many_font_families_fails_the_load(void)
{
    ERPoolLimits limits;
    er_pool_limits(&limits);
    static TestPack pack;
    const uint32_t family_count = limits.font_families + 1u;
    tpack_begin(&pack, 1u, family_count);
    tpack_image(&pack, "early", 0u);
    for (uint32_t family_index = 0; family_index < family_count; family_index++)
    {
        const char family[] = {(char)('a' + family_index), '\0'};
        tpack_font(&pack, family, ERUI_FONT_FMT_1BIT);
    }
    er_image_clear();
    build_with_pack(&pack);
    CHECK(er_runtime_load_container(s_container, s_container_length) == ER_CONTAINER_BAD_ASSETS,
          "more font families than the registry holds fails the load");
    CHECK(!er_image_exists("early"), "nothing from it is registered");
}

static void test_odd_rgb565_pixels_fail_the_load(void)
{
    static TestPack pack;
    tpack_begin(&pack, 1u, 0u);
    tpack_image(&pack, "rgb565", 1u);
    er_image_clear();
    build_with_pack(&pack);
    CHECK((uintptr_t)(s_container + s_pack_offset) & 1u, "the pack starts on an odd address");
    CHECK(er_runtime_load_container(s_container, s_container_length) == ER_CONTAINER_BAD_ASSETS,
          "RGB565 pixels on an odd address fail the load");
    CHECK(!er_image_exists("rgb565"), "the image isn't registered");
    /* The bytecode throws, so a pack that passes ends in LOAD_FAILED. */
    CHECK(er_runtime_load_container_ex(s_container, s_container_length, true) == ER_CONTAINER_LOAD_FAILED,
          "a copy is aligned, so the same pack loads");
    CHECK(er_image_exists("rgb565"), "the copy's image is registered");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    /* A runtime to load into, so a pack that wrongly passes reaches the bytecode instead of crashing. */
    static const EmbeddedRenderBackend backend = ER_HOST_NOOP_BACKEND;
    embedded_renderer_set_backend(&backend);
    ErRuntimeConfig config = {0};
    config.screen_width = 320;
    config.screen_height = 240;
    config.screen_scale = 1.0f;
    if (!er_runtime_init(&config))
    {
        printf("FAIL: er_runtime_init\n");
        return 1;
    }

    test_bad_pack_fails_the_load();
    test_too_many_font_families_fails_the_load();
    test_odd_rgb565_pixels_fail_the_load();

    if (s_failed)
    {
        return 1;
    }
    printf("container load: all tests passed\n");
    return 0;
}
