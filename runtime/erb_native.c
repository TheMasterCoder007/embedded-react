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
 * erb_native — what both flows share for native calls and events: the reading of signatures, and the strings they
 * pass. A string cell is its text's place in this table plus one, so it fits a 32-bit cell on any host. A flow notes
 * how many are held before a call or post and releases back to that after it.
 */

#include "erb_native.h"

#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

static const char* s_texts[ERB_NATIVE_MAX_TEXTS];
static uint32_t s_held;

/*----------------------------------------------------------------------------------------------------------------------
 - Signatures
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Whether @p code is a value's code. */
static bool is_code(char code)
{
    return code == 'i' || code == 'b' || code == 'f' || code == 'l' || code == 's';
}

/** @brief Whether @p character may be part of a field's name; a digit only after the first. */
static bool is_name_character(char character, bool first)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_'
           || (!first && character >= '0' && character <= '9');
}

/** @brief Reads a field's name and the ':' after it. */
static bool read_field_name(const char** cursor)
{
    const char* at = *cursor;
    if (!is_name_character(*at, true))
    {
        return false;
    }
    while (is_name_character(*at, false))
    {
        at++;
    }
    if (*at != ':')
    {
        return false;
    }
    *cursor = at + 1;
    return true;
}

/**
 * @brief Reads one value, a record's fields one by one, into @p codes from @p count.
 *
 * @return false when it isn't a value, or there are more than ERB_NATIVE_MAX_PARAMETERS.
 */
static bool read_value(const char** cursor, char* codes, uint32_t* count)
{
    const char* at = *cursor;
    uint32_t depth = 0;
    do
    {
        if (*at == '{')
        {
            at++;
            depth++;
            if (!read_field_name(&at))
            {
                return false;
            }
            continue;
        }
        if (!is_code(*at) || *count == ERB_NATIVE_MAX_PARAMETERS)
        {
            return false;
        }
        codes[(*count)++] = *at++;

        /* The fields that follow, and the records this value ends. */
        while (depth)
        {
            if (*at == '}')
            {
                at++;
                depth--;
                continue;
            }
            if (*at != ',')
            {
                return false;
            }
            at++;
            if (!read_field_name(&at))
            {
                return false;
            }
            break;
        }
    } while (depth);
    *cursor = at;
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public API
 ---------------------------------------------------------------------------------------------------------------------*/

bool erb_native_codes(const char* signature, ErbNativeCodes* codes)
{
    if (!signature || !codes)
    {
        return false;
    }
    const char* at = signature;
    uint32_t count = 0;
    codes->record = *at == '{';
    if (*at == 'v')
    {
        at++;
    }
    else if (!read_value(&at, codes->results, &count))
    {
        return false;
    }
    codes->results[count] = '\0';
    if (*at != ':')
    {
        return false;
    }
    at++;
    count = 0;
    while (*at)
    {
        if (!read_value(&at, codes->params, &count))
        {
            return false;
        }
    }
    codes->params[count] = '\0';
    return true;
}

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
