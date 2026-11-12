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
 * erb_native — the strings native calls and events pass, shared by both flows. A string cell is its text's place in
 * this table plus one, so it fits a 32-bit cell on any host. A flow notes how many are held before a call or post
 * and releases back to that after it.
 */

#include "erb_native.h"

#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

static const char* s_texts[ERB_NATIVE_MAX_TEXTS];
static uint32_t s_held;

/*----------------------------------------------------------------------------------------------------------------------
 - Public API
 ---------------------------------------------------------------------------------------------------------------------*/

const char* erb_native_string(int32_t cell)
{
    return cell > 0 && (uint32_t)cell <= s_held ? s_texts[cell - 1] : "";
}

int32_t erb_native_text(const char* text)
{
    if (!text || !text[0] || s_held == ERB_NATIVE_MAX_TEXTS)
    {
        return 0;
    }
    s_texts[s_held++] = text;
    return (int32_t)s_held;
}

uint32_t erb_native_texts_held(void)
{
    return s_held;
}

void erb_native_texts_release(uint32_t held)
{
    if (held < s_held)
    {
        s_held = held;
    }
}
