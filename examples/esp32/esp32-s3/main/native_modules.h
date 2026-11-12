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

#ifndef NATIVE_MODULES_H
#define NATIVE_MODULES_H

/*
 * The native modules the S3 offers apps, both flows alike: the chip (its name, free heap and a once-a-second event)
 * and its temperature sensor. The watch face demo uses them (demos/watch-face/modules).
 */

#include "erb_native.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Posts an event by its index in the table: erb_native_post() or er_runtime_native_post(). */
typedef bool (*NativePost)(int native, const int32_t* arguments);

/** @brief The table, for ErbConfig::natives or ErRuntimeConfig::natives. */
extern const ErbNative native_modules[];

/** @brief How many entries it has. */
extern const uint16_t native_module_count;

/** @brief Starts the temperature sensor. Without it, a reading never comes. */
void native_modules_init(void);

/**
 * @brief Posts what is due: the chip's second, and a reading measure() asked for. Call it every frame, from the
 *        frame loop.
 *
 * @param[in] uptime_ms  Milliseconds since boot.
 * @param[in] post       How this flow posts an event.
 */
void native_modules_frame(uint32_t uptime_ms, NativePost post);

#endif
