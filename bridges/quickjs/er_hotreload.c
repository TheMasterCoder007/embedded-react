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

#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/* Standard CRC-32 (IEEE / zlib polynomial 0xEDB88320), branchless inner loop. Byte-for-byte identical
 * to crc32_bytes() in er_runtime.c and crc32() in emit-container.mjs/protocol.mjs — all three CRCs in
 * the pipeline (host frame, host container, device container) are the one algorithm. */
static uint32_t er_hr_crc32(const uint8_t* p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t er_hr_rd_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The magic, as 4 bytes (avoids embedding a NUL-terminated string to memcmp against). */
static const uint8_t ER_HR_MAGIC[4] = {'E', 'R', 'H', 'R'};

/** @brief Returns the parser to the magic-scan phase, discarding any partial frame. */
static void er_hr_rescan(ErHotReload* hr)
{
    hr->phase = 0;
    hr->win_len = 0;
    hr->hdr_got = 0;
    hr->payload_len = 0;
    hr->payload_crc = 0;
    hr->payload_got = 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_hotreload_init(ErHotReload* hr, uint8_t* buf, size_t cap)
{
    memset(hr, 0, sizeof(*hr));
    hr->buf = buf;
    hr->cap = cap;
    er_hr_rescan(hr);
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

ErHotReloadStatus er_hotreload_feed(ErHotReload* hr, const uint8_t* data, size_t n, size_t* consumed, size_t* out_len)
{
    size_t i = 0;
    for (; i < n; i++)
    {
        const uint8_t b = data[i];

        if (hr->phase == 0) /* scan for magic via a rolling 4-byte window (matches at any offset) */
        {
            if (hr->win_len < 4u)
            {
                hr->win[hr->win_len++] = b;
            }
            else
            {
                hr->win[0] = hr->win[1];
                hr->win[1] = hr->win[2];
                hr->win[2] = hr->win[3];
                hr->win[3] = b;
            }
            if (hr->win_len == 4u && memcmp(hr->win, ER_HR_MAGIC, 4) == 0)
            {
                memcpy(hr->hdr, ER_HR_MAGIC, 4);
                hr->hdr_got = 4u;
                hr->win_len = 0u;
                hr->phase = 1u;
            }
            continue;
        }

        if (hr->phase == 1u) /* collect the rest of the 16-byte header */
        {
            hr->hdr[hr->hdr_got++] = b;
            if (hr->hdr_got < ER_HR_HEADER_SIZE)
            {
                continue;
            }
            const uint8_t version = hr->hdr[4];
            hr->msg_type = hr->hdr[5];
            hr->payload_len = er_hr_rd_le32(hr->hdr + 8);
            hr->payload_crc = er_hr_rd_le32(hr->hdr + 12);

            if (version != ER_HR_PROTO_VERSION || hr->payload_len == 0u)
            {
                er_hr_rescan(hr);
                *consumed = i + 1u;
                return ER_HR_BAD_VERSION;
            }
            if (hr->payload_len > hr->cap)
            {
                /* Too big for our buffer (or a false magic claiming a bogus length): drop and resync
                 * from the next byte rather than skipping payload_len bytes, which could swallow a real
                 * frame that follows a bogus header. */
                er_hr_rescan(hr);
                *consumed = i + 1u;
                return ER_HR_OVERFLOW;
            }
            hr->payload_got = 0u;
            hr->phase = 2u;
            continue;
        }

        /* phase 2: payload */
        hr->buf[hr->payload_got++] = b;
        if (hr->payload_got < hr->payload_len)
        {
            continue;
        }
        const uint32_t got = er_hr_crc32(hr->buf, hr->payload_len);
        const size_t len = hr->payload_len;
        const bool ok = (got == hr->payload_crc);
        er_hr_rescan(hr);
        *consumed = i + 1u;
        if (!ok)
        {
            return ER_HR_BAD_CRC;
        }
        if (out_len)
        {
            *out_len = len;
        }
        return ER_HR_FRAME_READY;
    }

    *consumed = i; /* == n: all input processed without finishing a frame */
    return ER_HR_NEED_MORE;
}

ErContainerStatus er_hotreload_apply(const void* erpkg, size_t len)
{
    /* Tear the previous app down to a clean slate, then load the new container — copying its assets
     * (copy_assets=true) since @p erpkg is a staging buffer the receiver will reuse for the next upload.
     * The copy is what lets the receiver refill the buffer without first tearing the running app down, so
     * the old UI stays on screen until the new one mounts. On failure, paint the on-screen panel. */
    er_runtime_reset();
    const ErContainerStatus status = er_runtime_load_container_ex(erpkg, len, true);
    if (status != ER_CONTAINER_OK)
    {
        er_runtime_show_message(
            "Hot reload failed", er_runtime_container_status_str(status), "Fix the app and save again to re-send.");
    }
    return status;
}
