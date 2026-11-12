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
 * The native modules the CYD offers apps: the chip, with its name, free heap and a once-a-second event. The watch
 * face demo uses it (demos/watch-face/modules). Its ESP32 has no temperature sensor, so the demo's optional
 * thermometer isn't here.
 */

#include "erb_native.h"

#include <stdint.h>

/** @brief The table, for ErbConfig::natives. */
extern const ErbNative native_modules[];

/** @brief How many entries it has. */
extern const uint16_t native_module_count;

/**
 * @brief Posts the chip's second when it is due. Call it every frame, from the frame loop.
 *
 * @param[in] uptime_ms  Milliseconds since boot.
 */
void native_modules_frame(uint32_t uptime_ms);

#endif
