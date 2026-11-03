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

#include "er_hotreload.h"

#include "er_container.h" /* er_crc32 */

#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Where each field sits in the header. */
#define VERSION_OFFSET 4u
#define MSG_TYPE_OFFSET 5u
#define PAYLOAD_LENGTH_OFFSET 8u
#define PAYLOAD_CRC_OFFSET 12u

/** @brief Parser phases. */
#define PHASE_MAGIC 0u
#define PHASE_HEADER 1u
#define PHASE_PAYLOAD 2u

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t er_hr_rd_le32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* The magic, as 4 bytes (avoids embedding a NUL-terminated string to memcmp against). */
static const uint8_t ER_HR_MAGIC[ER_HR_MAGIC_SIZE] = {'E', 'R', 'H', 'R'};

/** @brief Returns the parser to the magic-scan phase, discarding any partial frame. */
static void er_hr_rescan(ErHotReload* parser)
{
    parser->phase = PHASE_MAGIC;
    parser->magic_window_length = 0;
    parser->header_received = 0;
    parser->payload_length = 0;
    parser->payload_crc = 0;
    parser->payload_received = 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_hotreload_init(ErHotReload* parser, uint8_t* buffer, size_t capacity)
{
    memset(parser, 0, sizeof(*parser));
    parser->buf = buffer;
    parser->cap = capacity;
    er_hr_rescan(parser);
}

const char* er_hotreload_status_str(ErHotReloadStatus status)
{
    switch (status)
    {
        case ER_HR_NEED_MORE:
            return "need more";
        case ER_HR_FRAME_READY:
            return "frame ready";
        case ER_HR_BAD_CRC:
            return "transport CRC mismatch";
        case ER_HR_OVERFLOW:
            return "payload exceeds buffer";
        case ER_HR_BAD_VERSION:
            return "unsupported frame version";
        default:
            return "unknown";
    }
}

ErHotReloadStatus
er_hotreload_feed(ErHotReload* parser, const uint8_t* data, size_t data_length, size_t* consumed, size_t* out_length)
{
    size_t byte_index = 0;
    for (; byte_index < data_length; byte_index++)
    {
        const uint8_t byte = data[byte_index];

        if (parser->phase == PHASE_MAGIC) /* scan for magic via a rolling 4-byte window (matches at any offset) */
        {
            if (parser->magic_window_length < ER_HR_MAGIC_SIZE)
            {
                parser->magic_window[parser->magic_window_length++] = byte;
            }
            else
            {
                parser->magic_window[0] = parser->magic_window[1];
                parser->magic_window[1] = parser->magic_window[2];
                parser->magic_window[2] = parser->magic_window[3];
                parser->magic_window[3] = byte;
            }
            if (parser->magic_window_length == ER_HR_MAGIC_SIZE
                && memcmp(parser->magic_window, ER_HR_MAGIC, ER_HR_MAGIC_SIZE) == 0)
            {
                memcpy(parser->header, ER_HR_MAGIC, ER_HR_MAGIC_SIZE);
                parser->header_received = ER_HR_MAGIC_SIZE;
                parser->magic_window_length = 0u;
                parser->phase = PHASE_HEADER;
            }
            continue;
        }

        if (parser->phase == PHASE_HEADER) /* collect the rest of the 16-byte header */
        {
            parser->header[parser->header_received++] = byte;
            if (parser->header_received < ER_HR_HEADER_SIZE)
            {
                continue;
            }
            const uint8_t version = parser->header[VERSION_OFFSET];
            parser->msg_type = parser->header[MSG_TYPE_OFFSET];
            parser->payload_length = er_hr_rd_le32(parser->header + PAYLOAD_LENGTH_OFFSET);
            parser->payload_crc = er_hr_rd_le32(parser->header + PAYLOAD_CRC_OFFSET);

            if (version != ER_HR_PROTO_VERSION || parser->payload_length == 0u)
            {
                er_hr_rescan(parser);
                *consumed = byte_index + 1u;
                return ER_HR_BAD_VERSION;
            }
            if (parser->payload_length > parser->cap)
            {
                /* Too big for our buffer (or a false magic claiming a bogus length): drop and resync
                 * from the next byte rather than skipping payload_len bytes, which could swallow a real
                 * frame that follows a bogus header. */
                er_hr_rescan(parser);
                *consumed = byte_index + 1u;
                return ER_HR_OVERFLOW;
            }
            parser->payload_received = 0u;
            parser->phase = PHASE_PAYLOAD;
            continue;
        }

        /* PHASE_PAYLOAD */
        parser->buf[parser->payload_received++] = byte;
        if (parser->payload_received < parser->payload_length)
        {
            continue;
        }
        const uint32_t computed_crc = er_crc32(parser->buf, parser->payload_length);
        const size_t payload_length = parser->payload_length;
        const bool crc_matches = (computed_crc == parser->payload_crc);
        er_hr_rescan(parser);
        *consumed = byte_index + 1u;
        if (!crc_matches)
        {
            return ER_HR_BAD_CRC;
        }
        if (out_length)
        {
            *out_length = payload_length;
        }
        return ER_HR_FRAME_READY;
    }

    *consumed = byte_index; /* == data_length: all input processed without finishing a frame */
    return ER_HR_NEED_MORE;
}
