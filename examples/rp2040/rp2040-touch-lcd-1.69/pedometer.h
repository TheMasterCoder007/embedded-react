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

#ifndef PEDOMETER_H
#define PEDOMETER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief A simple accelerometer step detector (magnitude peak counter).
     *
     * Feed it raw accelerometer samples at a roughly fixed rate; it tracks the gravity baseline, extracts
     * the low-frequency "bounce" of walking, and counts a step on each qualifying peak (with hysteresis and
     * a refractory period so one stride is never double-counted). Platform-neutral C — no board deps.
     */
    typedef struct
    {
        float baseline;        /**< Slow-tracked gravity magnitude (LSB). */
        float smoothed;        /**< Low-pass of the AC (gravity-removed) magnitude. */
        int armed;             /**< 1 once the signal dropped below the low threshold (ready for the next peak). */
        uint32_t last_step_ms; /**< Timestamp of the last counted step (refractory gate). */
        uint32_t steps;        /**< Total steps counted since reset. */
    } Pedometer;

    /** @brief Resets a pedometer to zero steps and a cold baseline. */
    void pedometer_reset(Pedometer* p);

    /**
     * @brief Feeds one accelerometer sample and returns the running step total.
     *
     * @param[in] p        The pedometer state.
     * @param[in] ax,ay,az Raw accelerometer axes (any consistent unit; ±4g → 1 g ≈ 8192 LSB).
     * @param[in] now_ms   Monotonic millisecond timestamp of this sample.
     *
     * @return The total step count after processing this sample.
     */
    uint32_t pedometer_update(Pedometer* p, int16_t ax, int16_t ay, int16_t az, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
