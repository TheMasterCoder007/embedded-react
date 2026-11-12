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
 * The native modules this board offers apps: the chip (its name, free heap and a once-a-second event), its
 * temperature sensor, and the IMU's steps and tilt. The watch face demo uses them (demos/watch-face/modules).
 */

#include "erb_native.h"

#include <stdint.h>

/** @brief The table, for ErbConfig::natives. */
extern const ErbNative native_modules[];

/** @brief How many entries it has. */
extern const uint16_t native_module_count;

/** @brief Starts the temperature sensor. */
void native_modules_init(void);

/** @brief The pedometer's count, posted as motion.steps when it changes. */
void native_modules_set_steps(uint32_t steps);

/** @brief The level's dot offset in pixels, posted as motion.tilt when it changes. */
void native_modules_set_tilt(int x, int y);

/** @brief Posts the steps and tilt again, for an app that a hot reload just started. */
void native_modules_repost(void);

/**
 * @brief Posts what is due: the chip's second, a reading measure() asked for, and the steps and tilt. Call it every
 *        frame, from the frame loop.
 *
 * @param[in] uptime_ms  Milliseconds since boot.
 */
void native_modules_frame(uint32_t uptime_ms);

#endif
