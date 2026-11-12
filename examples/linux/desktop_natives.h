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

#ifndef ER_DESKTOP_NATIVES_H
#define ER_DESKTOP_NATIVES_H

/*
 * The native modules the desktop hosts offer, both flows alike: the chip of the boards' device package
 * (../device), which every board has. Its thermometer and motion sensor are optional, and a desktop has neither.
 */

#include "erb_native.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Posts an event by its index in the table: erb_native_post() or er_runtime_native_post(). */
typedef bool (*DesktopNativePost)(int native, const int32_t* arguments);

/** @brief The table, for ErbConfig::natives or ErRuntimeConfig::natives. */
extern const ErbNative desktop_natives[];

/** @brief How many entries it has. */
extern const uint16_t desktop_native_count;

/**
 * @brief Posts the chip's `second` event once a second. Call it every frame.
 *
 * @param[in] uptime_ms  Milliseconds since the host started.
 * @param[in] post       How this flow posts an event.
 */
void desktop_natives_frame(uint32_t uptime_ms, DesktopNativePost post);

#endif
