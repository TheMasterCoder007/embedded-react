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
 * Accelerometer step detection.
 *
 * Walking shows up as a ~1.5–2.5 Hz oscillation in the acceleration magnitude (each footfall is a peak).
 * The detector: (1) tracks gravity with a slow low-pass baseline and subtracts it to get the AC "bounce";
 * (2) smooths that with a faster low-pass to kill sensor noise; (3) counts a step on each RISING crossing
 * of an upper threshold, but only after the signal has dipped below a lower threshold since the last step
 * (hysteresis) AND at least PEDO_MIN_STEP_MS has passed (refractory) — so a single stride can't double
 * count and hand-jitter below the threshold is ignored. Thresholds are in ±4g LSB (1 g ≈ 8192).
 */

#include "pedometer.h"

#include <math.h>

#define PEDOMETER_BASELINE_RATE 0.02f    /**< Baseline (gravity) tracking rate — slow. */
#define PEDOMETER_SMOOTHING_RATE 0.30f   /**< AC smoothing rate — faster, denoises footfall peaks. */
#define PEDOMETER_HIGH_THRESHOLD 1400.0f /**< Upper threshold (~0.17 g): a peak above this arms a step. */
#define PEDOMETER_LOW_THRESHOLD 500.0f   /**< Lower threshold (~0.06 g): must dip below this to re-arm. */
#define PEDOMETER_MIN_STEP_MS 260u       /**< Refractory: fastest plausible cadence (~230 steps/min). */
#define PEDOMETER_ONE_G 8192.0f          /**< 1 g in accelerometer LSB at ±4g full scale. */

void pedometer_reset(Pedometer* pedometer)
{
    pedometer->baseline =
        PEDOMETER_ONE_G; /* ~1 g at ±4g full-scale — a warm start so the first strides aren't missed */
    pedometer->smoothed = 0.0f;
    pedometer->armed = 1;
    pedometer->last_step_ms = 0;
    pedometer->steps = 0;
}

uint32_t pedometer_update(Pedometer* pedometer, int16_t accel_x, int16_t accel_y, int16_t accel_z, uint32_t now_ms)
{
    const float magnitude = sqrtf((float)accel_x * accel_x + (float)accel_y * accel_y + (float)accel_z * accel_z);

    /* Track gravity slowly; the AC component is what walking modulates. */
    pedometer->baseline += (magnitude - pedometer->baseline) * PEDOMETER_BASELINE_RATE;
    const float ac_component = magnitude - pedometer->baseline;
    pedometer->smoothed += (ac_component - pedometer->smoothed) * PEDOMETER_SMOOTHING_RATE;

    /* Re-arm once we've clearly dipped below the low threshold since the last peak. */
    if (pedometer->smoothed < PEDOMETER_LOW_THRESHOLD)
    {
        pedometer->armed = 1;
    }

    /* A step = armed + rising above the high threshold + past the refractory window. */
    if (pedometer->armed && pedometer->smoothed > PEDOMETER_HIGH_THRESHOLD
        && (uint32_t)(now_ms - pedometer->last_step_ms) >= PEDOMETER_MIN_STEP_MS)
    {
        pedometer->steps++;
        pedometer->armed = 0;
        pedometer->last_step_ms = now_ms;
    }
    return pedometer->steps;
}
