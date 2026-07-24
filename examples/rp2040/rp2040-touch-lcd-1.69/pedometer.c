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

#define PEDO_BASE_A 0.02f      /**< Baseline (gravity) tracking rate — slow. */
#define PEDO_SMOOTH_A 0.30f    /**< AC smoothing rate — faster, denoises footfall peaks. */
#define PEDO_HI 1400.0f        /**< Upper threshold (~0.17 g): a peak above this arms a step. */
#define PEDO_LO 500.0f         /**< Lower threshold (~0.06 g): must dip below this to re-arm. */
#define PEDO_MIN_STEP_MS 260u  /**< Refractory: fastest plausible cadence (~230 steps/min). */

void pedometer_reset(Pedometer* p)
{
    p->baseline = 8192.0f; /* ~1 g at ±4g full-scale — a warm start so the first strides aren't missed */
    p->smoothed = 0.0f;
    p->armed = 1;
    p->last_step_ms = 0;
    p->steps = 0;
}

uint32_t pedometer_update(Pedometer* p, int16_t ax, int16_t ay, int16_t az, uint32_t now_ms)
{
    const float mag = sqrtf((float)ax * ax + (float)ay * ay + (float)az * az);

    /* Track gravity slowly; the AC component is what walking modulates. */
    p->baseline += (mag - p->baseline) * PEDO_BASE_A;
    const float ac = mag - p->baseline;
    p->smoothed += (ac - p->smoothed) * PEDO_SMOOTH_A;

    /* Re-arm once we've clearly dipped below the low threshold since the last peak. */
    if (p->smoothed < PEDO_LO)
    {
        p->armed = 1;
    }

    /* A step = armed + rising above the high threshold + past the refractory window. */
    if (p->armed && p->smoothed > PEDO_HI && (uint32_t)(now_ms - p->last_step_ms) >= PEDO_MIN_STEP_MS)
    {
        p->steps++;
        p->armed = 0;
        p->last_step_ms = now_ms;
    }
    return p->steps;
}
