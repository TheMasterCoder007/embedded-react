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

#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief "ERCF" | format_version u32 | crc32 u32. The CRC covers everything after it. */
#define HEADER_SIZE 12u
#define FORMAT_VERSION_OFFSET 4u
#define CRC_OFFSET 8u

/** @brief Each section starts with type u32 | length u32. */
#define SECTION_HEADER_SIZE 8u

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static uint16_t read_le16(const uint8_t* bytes)
{
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t read_le32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public
 ---------------------------------------------------------------------------------------------------------------------*/

uint32_t er_crc32(const uint8_t* bytes, size_t byte_count)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t byte_index = 0; byte_index < byte_count; byte_index++)
    {
        crc ^= bytes[byte_index];
        for (int bit_index = 0; bit_index < 8; bit_index++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

const char* er_container_status_str(ErContainerStatus status)
{
    switch (status)
    {
        case ER_CONTAINER_OK:
            return "ok";
        case ER_CONTAINER_BAD_MAGIC:
            return "not an ERCF container";
        case ER_CONTAINER_BAD_VERSION:
            return "unsupported or malformed container";
        case ER_CONTAINER_BAD_CRC:
            return "CRC mismatch (corrupt or partially written)";
        case ER_CONTAINER_BAD_QJS:
            return "built for a different QuickJS version";
        case ER_CONTAINER_LOAD_FAILED:
            return "app load failed";
        default:
            return "unknown";
    }
}

ErContainerStatus er_container_scan(const void* buffer, size_t buffer_length, ErContainer* container)
{
    const uint8_t* bytes = (const uint8_t*)buffer;
    if (!bytes || buffer_length < HEADER_SIZE || memcmp(bytes, "ERCF", 4) != 0)
    {
        return ER_CONTAINER_BAD_MAGIC;
    }
    if (read_le32(bytes + FORMAT_VERSION_OFFSET) != ER_CONTAINER_FORMAT_VERSION)
    {
        return ER_CONTAINER_BAD_VERSION;
    }

    /* The length fields are unverified until the CRC is checked, so every one is bounds-checked against
     * buffer_length: a malformed blob is rejected, never over-read. offset never passes buffer_length, so
     * each check compares with the bytes left instead of adding to offset, which could wrap a 32-bit size_t
     * when buffer_length is a loose upper bound. */
    size_t offset = HEADER_SIZE;
    if (buffer_length - offset < 2u)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    container->tag_length = read_le16(bytes + offset);
    offset += 2u;
    if (buffer_length - offset < (size_t)container->tag_length + 4u)
    {
        return ER_CONTAINER_BAD_VERSION;
    }
    container->tag = bytes + offset;
    offset += container->tag_length;
    container->section_count = read_le32(bytes + offset);
    offset += 4u;
    if (container->section_count > ER_CONTAINER_MAX_SECTIONS)
    {
        return ER_CONTAINER_BAD_VERSION;
    }

    for (uint32_t section_index = 0; section_index < container->section_count; section_index++)
    {
        if (buffer_length - offset < SECTION_HEADER_SIZE)
        {
            return ER_CONTAINER_BAD_VERSION;
        }
        ErContainerSection* section = &container->sections[section_index];
        const uint32_t section_length = read_le32(bytes + offset + 4u);
        section->type = read_le32(bytes + offset);
        offset += SECTION_HEADER_SIZE;
        if (buffer_length - offset < section_length)
        {
            return ER_CONTAINER_BAD_VERSION;
        }
        section->data = bytes + offset;
        section->length = section_length;
        offset += section_length;
    }
    container->size = offset;
    return ER_CONTAINER_OK;
}

ErContainerStatus er_container_read(const void* buffer, size_t buffer_length, ErContainer* container)
{
    const ErContainerStatus status = er_container_scan(buffer, buffer_length, container);
    if (status != ER_CONTAINER_OK)
    {
        return status;
    }
    const uint8_t* bytes = (const uint8_t*)buffer;
    const uint32_t stored_crc = read_le32(bytes + CRC_OFFSET);
    if (stored_crc != er_crc32(bytes + HEADER_SIZE, container->size - HEADER_SIZE))
    {
        return ER_CONTAINER_BAD_CRC;
    }
    return ER_CONTAINER_OK;
}
