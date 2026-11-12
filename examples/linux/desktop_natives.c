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

#include "desktop_natives.h"

#include <stddef.h>

#define MS_PER_SECOND 1000u

/* A desktop has no heap to speak of, so it reports a board's, the same in both flows. */
#define FREE_MEMORY_BYTES (256 * 1024)

enum
{
    CHIP_NAME,
    CHIP_FREE_MEMORY,
    CHIP_SECOND,
    NATIVE_COUNT
};

static uint32_t s_posted_seconds;

static bool chip_name(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = erb_native_text("Desktop");
    return true;
}

static bool chip_free_memory(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = FREE_MEMORY_BYTES;
    return true;
}

const ErbNative desktop_natives[NATIVE_COUNT] = {
    [CHIP_NAME] = {"chip.name", 1, "s:", chip_name},
    [CHIP_FREE_MEMORY] = {"chip.freeMemory", 1, "i:", chip_free_memory},
    [CHIP_SECOND] = {"chip.second", 1, "v:l", NULL},
};

const uint16_t desktop_native_count = NATIVE_COUNT;

void desktop_natives_frame(uint32_t uptime_ms, DesktopNativePost post)
{
    const uint32_t seconds = uptime_ms / MS_PER_SECOND;
    if (seconds == s_posted_seconds)
    {
        return;
    }
    /* A post the runtime can't take now goes again next frame. */
    int32_t uptime[2];
    erb_native_set_int64(uptime, (int64_t)uptime_ms);
    if (post(CHIP_SECOND, uptime))
    {
        s_posted_seconds = seconds;
    }
}
