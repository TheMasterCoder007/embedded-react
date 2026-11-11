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
 * erb_native — the native functions the firmware offers an app's native modules. One table serves both flows: Flow B
 * takes it in ErbConfig, and Flow A in ErRuntimeConfig.
 */

#include <stdbool.h>
#include <stdint.h>

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
 * @brief One native function the firmware offers. The signature is the result, ':', then the parameters,
 *        each one of i (int), b (bool) or f (float); v as the result means none. "i:if" takes an int and a
 *        float and returns an int.
 */
typedef struct
{
    const char* name;
    uint16_t version;
    const char* signature;
    ErbNativeFn callback;
} ErbNative;

#endif
