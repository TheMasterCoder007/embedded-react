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
 */

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Configuration
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Most parameters a native function or event may take. */
#define ERB_NATIVE_MAX_PARAMETERS 16u

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief A native function an app may call.
 *
 * @param[in]  arguments  Arguments as 32-bit cells, as the signature lists them.
 * @param[out] result     The result cell (unused when the signature returns nothing).
 * @param[in]  context    ErbConfig::context, or ErRuntimeConfig::native_context.
 *
 * @return false to stop the app, naming the module.
 */
typedef bool (*ErbNativeFn)(const int32_t* arguments, int32_t* result, void* context);

/**
 * @brief One native function or event the firmware offers. The signature is the result, ':', then the parameters,
 *        each one of i (int), b (bool) or f (float); v as the result means none. "i:if" takes an int and a
 *        float and returns an int. An event has no callback and returns nothing: "v:f" is posted with a float.
 */
typedef struct
{
    const char* name;
    uint16_t version;
    const char* signature;
    ErbNativeFn callback; /**< NULL for an event. */
} ErbNative;

#endif
