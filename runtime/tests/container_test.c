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

#include "er_container.h"

#include <stdint.h>
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

/** @brief The tag every test container carries. */
static const char TEST_TAG[] = "v0.15.0";
#define TEST_TAG_LENGTH ((uint16_t)(sizeof(TEST_TAG) - 1u))

/** @brief Offset of the first section's length field: header (12) + tag length (2) + tag + section count (4) +
 *         the section's type (4). */
#define FIRST_SECTION_LENGTH_OFFSET (12u + 2u + TEST_TAG_LENGTH + 4u + 4u)

/**
 * @brief Writes a little-endian uint32_t into a byte buffer.
 *
 * @param[out] destination  Where to write (4 bytes).
 * @param[in]  value        Value to write.
 */
static void put_u32_le(uint8_t* destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16) & 0xFFU);
    destination[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/**
 * @brief Stamps the CRC over everything after the 12-byte header, as the emitter does.
 *
 * @param[in,out] container       Container bytes.
 * @param[in]     container_size  Container size in bytes.
 */
static void seal(uint8_t* container, size_t container_size)
{
    put_u32_le(container + 8, er_crc32(container + 12, container_size - 12u));
}

/**
 * @brief Builds a sealed container tagged TEST_TAG. Section N (from 0) has type N + 1 and holds N + 1 bytes of value
 *        N, so each section's contents say which one it is.
 *
 * @param[out] container      Destination (must be large enough).
 * @param[in]  section_count  Number of sections to write (the header says the same).
 *
 * @return The container size in bytes.
 */
static size_t build(uint8_t* container, uint32_t section_count)
{
    memcpy(container, "ERCF", 4);
    put_u32_le(container + 4, 1u);
    size_t offset = 12u;
    container[offset++] = (uint8_t)(TEST_TAG_LENGTH & 0xFFU);
    container[offset++] = (uint8_t)(TEST_TAG_LENGTH >> 8);
    memcpy(container + offset, TEST_TAG, TEST_TAG_LENGTH);
    offset += TEST_TAG_LENGTH;
    put_u32_le(container + offset, section_count);
    offset += 4u;
    for (uint32_t section_index = 0; section_index < section_count; section_index++)
    {
        const uint32_t section_length = section_index + 1u;
        put_u32_le(container + offset, section_index + 1u);
        put_u32_le(container + offset + 4u, section_length);
        offset += 8u;
        memset(container + offset, (int)section_index, section_length);
        offset += section_length;
    }
    seal(container, offset);
    return offset;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_crc_check_vector(void)
{
    CHECK(er_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "CRC-32 check vector");
}

static void test_reads_tag_and_sections(void)
{
    uint8_t buffer[256];
    const size_t container_size = build(buffer, 3);
    ErContainer container;
    CHECK(er_container_read(buffer, container_size, &container) == ER_CONTAINER_OK, "valid container reads");
    CHECK(container.size == container_size, "size is the container's exact length");
    CHECK(container.tag_length == TEST_TAG_LENGTH && memcmp(container.tag, TEST_TAG, TEST_TAG_LENGTH) == 0,
          "tag points at the tag bytes");
    CHECK(container.section_count == 3u, "three sections");
    for (uint32_t section_index = 0; section_index < container.section_count; section_index++)
    {
        const ErContainerSection* section = &container.sections[section_index];
        CHECK(section->type == section_index + 1u, "section type");
        CHECK(section->length == section_index + 1u, "section length");
        CHECK(section->data[0] == (uint8_t)section_index, "section data points into the buffer");
    }
}

static void test_length_is_an_upper_bound(void)
{
    uint8_t buffer[256];
    const size_t container_size = build(buffer, 2);
    memset(buffer + container_size, 0xFF, sizeof(buffer) - container_size);
    ErContainer container;
    CHECK(er_container_read(buffer, sizeof(buffer), &container) == ER_CONTAINER_OK,
          "trailing partition bytes are ignored");
    CHECK(container.size == container_size, "size stops at the last section");
}

static void test_rejects_bad_header(void)
{
    uint8_t buffer[256];
    size_t container_size = build(buffer, 1);
    ErContainer container;
    CHECK(er_container_read(NULL, container_size, &container) == ER_CONTAINER_BAD_MAGIC, "NULL buffer");
    CHECK(er_container_read(buffer, 11, &container) == ER_CONTAINER_BAD_MAGIC, "shorter than the header");
    buffer[0] = 'X';
    CHECK(er_container_read(buffer, container_size, &container) == ER_CONTAINER_BAD_MAGIC, "wrong magic");

    container_size = build(buffer, 1);
    put_u32_le(buffer + 4, 2u);
    CHECK(er_container_read(buffer, container_size, &container) == ER_CONTAINER_BAD_VERSION, "unknown format version");
}

static void test_rejects_bad_crc(void)
{
    uint8_t buffer[256];
    const size_t container_size = build(buffer, 2);
    buffer[container_size - 1u] ^= 0x01u;
    ErContainer container;
    CHECK(er_container_read(buffer, container_size, &container) == ER_CONTAINER_BAD_CRC, "flipped payload bit");
    CHECK(er_container_scan(buffer, container_size, &container) == ER_CONTAINER_OK, "scan skips the CRC");
}

static void test_rejects_truncation(void)
{
    uint8_t buffer[256];
    const size_t container_size = build(buffer, 3);
    ErContainer container;
    for (size_t truncated_length = 12u; truncated_length < container_size; truncated_length++)
    {
        if (er_container_scan(buffer, truncated_length, &container) != ER_CONTAINER_BAD_VERSION)
        {
            printf("FAIL: truncated to %u bytes\n", (unsigned)truncated_length);
            s_failed = 1;
        }
    }
}

static void test_rejects_oversized_lengths(void)
{
    uint8_t buffer[256];
    size_t container_size = build(buffer, 1);
    ErContainer container;

    /* The tag length, then the section length, claim more than the buffer holds. */
    buffer[12] = 0xFFu;
    buffer[13] = 0xFFu;
    seal(buffer, container_size);
    CHECK(er_container_read(buffer, container_size, &container) == ER_CONTAINER_BAD_VERSION, "tag past the end");

    container_size = build(buffer, 1);
    put_u32_le(buffer + FIRST_SECTION_LENGTH_OFFSET, 0xFFFFFFF0u);
    seal(buffer, container_size);
    CHECK(er_container_read(buffer, container_size, &container) == ER_CONTAINER_BAD_VERSION,
          "section length near 4 GB");
}

static void test_rejects_too_many_sections(void)
{
    uint8_t buffer[512];
    const size_t over_limit_size = build(buffer, ER_CONTAINER_MAX_SECTIONS + 1u);
    ErContainer container;
    CHECK(er_container_read(buffer, over_limit_size, &container) == ER_CONTAINER_BAD_VERSION,
          "more sections than the limit");

    const size_t at_limit_size = build(buffer, ER_CONTAINER_MAX_SECTIONS);
    CHECK(er_container_read(buffer, at_limit_size, &container) == ER_CONTAINER_OK, "exactly the limit");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    test_crc_check_vector();
    test_reads_tag_and_sections();
    test_length_is_an_upper_bound();
    test_rejects_bad_header();
    test_rejects_bad_crc();
    test_rejects_truncation();
    test_rejects_oversized_lengths();
    test_rejects_too_many_sections();

    if (s_failed)
    {
        return 1;
    }
    printf("container: all tests passed\n");
    return 0;
}
