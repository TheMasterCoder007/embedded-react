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

#ifndef ERB_NATIVE_H
#define ERB_NATIVE_H

/*
 * erb_native — the native functions and events the firmware offers an app's native modules. One table serves both
 * flows: Flow B takes it in ErbConfig, and Flow A in ErRuntimeConfig.
 *
 * An entry with no callback is an event. The firmware posts it by its index in the table, from the frame-loop task,
 * with erb_native_post() in Flow B or er_runtime_native_post() in Flow A. The app's subscribed handlers run at once.
 *
 * Values pass as 32-bit cells. An int64 takes two, low word first. A string takes one, which holds it by reference:
 * erb_native_string() reads an argument's, and erb_native_text() makes one for a result or an event's argument.
 */

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Configuration
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Most parameters a native function or event may take. */
#define ERB_NATIVE_MAX_PARAMETERS 16u

/** @brief Most cells they take: two for each int64. */
#define ERB_NATIVE_MAX_CELLS (2u * ERB_NATIVE_MAX_PARAMETERS)

/** @brief Most strings held at once: an event's, then those of a call its handler makes, with room to spare. */
#define ERB_NATIVE_MAX_TEXTS 48u

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief A native function an app may call.
 *
 * @param[in]  arguments  Arguments as 32-bit cells, as the signature lists them.
 * @param[out] result     The result's cells: two for an int64, one otherwise, and none when it returns nothing.
 * @param[in]  context    ErbConfig::context, or ErRuntimeConfig::native_context.
 *
 * @return false to stop the app, naming the module.
 */
typedef bool (*ErbNativeFn)(const int32_t* arguments, int32_t* result, void* context);

/**
 * @brief One native function or event the firmware offers. The signature is the result, ':', then the parameters,
 *        each one of i (int), b (bool), f (float), l (int64) or s (string); v as the result means none. "i:if" takes
 *        an int and a float and returns an int. An event has no callback and returns nothing: "v:f" is posted with a
 *        float.
 */
typedef struct
{
    const char* name;
    uint16_t version;
    const char* signature;
    ErbNativeFn callback; /**< NULL for an event. */
} ErbNative;

/*----------------------------------------------------------------------------------------------------------------------
 - Values
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief An int64 from its two cells, low word first. */
static inline int64_t erb_native_int64(const int32_t* cells)
{
    return (int64_t)((uint64_t)(uint32_t)cells[0] | ((uint64_t)(uint32_t)cells[1] << 32));
}

/** @brief Writes an int64 into two cells, low word first. */
static inline void erb_native_set_int64(int32_t* cells, int64_t value)
{
    cells[0] = (int32_t)(uint32_t)(uint64_t)value;
    cells[1] = (int32_t)(uint32_t)((uint64_t)value >> 32);
}

/**
 * @brief The text a string cell holds: an argument's, read in place while the call runs, or one erb_native_text()
 *        made. Copy it to keep it.
 *
 * @param[in] cell  The cell.
 *
 * @return The text; "" for a cell that holds none.
 */
const char* erb_native_string(int32_t cell);

/**
 * @brief A string cell for @p text: a native function's result, or an argument of an event the firmware posts. The
 *        text isn't copied, so it must stay as it is until the call or post returns. Call it from the frame-loop
 *        task, as natives run and events are posted.
 *
 * @param[in] text  The text, UTF-8; NULL is "".
 *
 * @return The cell. One that reads as "" when ERB_NATIVE_MAX_TEXTS are held.
 */
int32_t erb_native_text(const char* text);

/** @brief For the flows: how many strings are held, to release back to with erb_native_texts_release(). */
uint32_t erb_native_texts_held(void);

/** @brief For the flows: drops the strings held since erb_native_texts_held() returned @p held. */
void erb_native_texts_release(uint32_t held);

#endif
