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

#include "er_scene.h"

#include <stdio.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_fail = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            printf("FAIL: %s\n", (msg));                                                                               \
            s_fail = 1;                                                                                                \
        }                                                                                                              \
    } while (0)

/**
 * @brief Returns true when two floats are within a small epsilon.
 *
 * @param[in] a  First value.
 * @param[in] b  Second value.
 *
 * @return Non-zero when |a - b| < 1e-4.
 */
static int approx(float a, float b)
{
    float d = a - b;
    if (d < 0.0f)
        d = -d;
    return d < 1e-4f;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: er_interpolate (pure mapping)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies linear two-point mapping at endpoints and midpoint.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_two_point_linear(void)
{
    const float in[2] = {0.0f, 1.0f};
    const float out[2] = {0.0f, 100.0f};
    CHECK(approx(er_interpolate(0.0f, in, out, 2, ER_EXTRAPOLATE_EXTEND, ER_EXTRAPOLATE_EXTEND), 0.0f),
          "two-point: start");
    CHECK(approx(er_interpolate(0.5f, in, out, 2, ER_EXTRAPOLATE_EXTEND, ER_EXTRAPOLATE_EXTEND), 50.0f),
          "two-point: midpoint");
    CHECK(approx(er_interpolate(1.0f, in, out, 2, ER_EXTRAPOLATE_EXTEND, ER_EXTRAPOLATE_EXTEND), 100.0f),
          "two-point: end");
    return s_fail;
}

/**
 * @brief Verifies a three-point range with a non-monotonic output (0 -> 80 -> 0).
 *
 * @return 0 on success, 1 on failure.
 */
static int test_three_point_range(void)
{
    const float in[3] = {0.0f, 0.5f, 1.0f};
    const float out[3] = {0.0f, 80.0f, 0.0f};
    CHECK(approx(er_interpolate(0.25f, in, out, 3, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP), 40.0f),
          "three-point: rising segment");
    CHECK(approx(er_interpolate(0.5f, in, out, 3, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP), 80.0f),
          "three-point: peak");
    CHECK(approx(er_interpolate(0.75f, in, out, 3, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP), 40.0f),
          "three-point: falling segment");
    return s_fail;
}

/**
 * @brief Verifies the three extrapolation policies below and above the range.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_extrapolation(void)
{
    const float in[2] = {0.0f, 1.0f};
    const float out[2] = {10.0f, 20.0f};

    /* CLAMP: pinned to the nearest endpoint. */
    CHECK(approx(er_interpolate(-1.0f, in, out, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP), 10.0f), "clamp: below");
    CHECK(approx(er_interpolate(2.0f, in, out, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP), 20.0f), "clamp: above");

    /* EXTEND: continue the segment slope (here slope 10). */
    CHECK(approx(er_interpolate(-1.0f, in, out, 2, ER_EXTRAPOLATE_EXTEND, ER_EXTRAPOLATE_EXTEND), 0.0f),
          "extend: below");
    CHECK(approx(er_interpolate(2.0f, in, out, 2, ER_EXTRAPOLATE_EXTEND, ER_EXTRAPOLATE_EXTEND), 30.0f),
          "extend: above");

    /* IDENTITY: pass the raw input through outside the range. */
    CHECK(approx(er_interpolate(-5.0f, in, out, 2, ER_EXTRAPOLATE_IDENTITY, ER_EXTRAPOLATE_IDENTITY), -5.0f),
          "identity: below");
    CHECK(approx(er_interpolate(7.0f, in, out, 2, ER_EXTRAPOLATE_IDENTITY, ER_EXTRAPOLATE_IDENTITY), 7.0f),
          "identity: above");

    /* Left and right policies are independent. */
    CHECK(approx(er_interpolate(-1.0f, in, out, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_EXTEND), 10.0f),
          "split: clamp left");
    CHECK(approx(er_interpolate(2.0f, in, out, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_EXTEND), 30.0f),
          "split: extend right");
    return s_fail;
}

/**
 * @brief Verifies degenerate inputs are handled safely.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_degenerate(void)
{
    const float in[2] = {0.0f, 1.0f};
    const float out[2] = {5.0f, 9.0f};
    /* point_count < 2 -> input returned unchanged. */
    CHECK(approx(er_interpolate(0.5f, in, out, 1, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP), 0.5f),
          "degenerate: single point returns input");
    /* NULL arrays -> input returned unchanged. */
    CHECK(approx(er_interpolate(0.5f, NULL, out, 2, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP), 0.5f),
          "degenerate: null input_range");
    /* Zero-width leading segment -> lower output (no divide-by-zero). */
    {
        const float flat_in[3] = {0.0f, 0.0f, 1.0f};
        const float flat_out[3] = {2.0f, 4.0f, 8.0f};
        float v = er_interpolate(0.0f, flat_in, flat_out, 3, ER_EXTRAPOLATE_CLAMP, ER_EXTRAPOLATE_CLAMP);
        CHECK(approx(v, 2.0f), "degenerate: zero-width segment");
    }
    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Entry point
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Runs all interpolation tests.
 *
 * @return 0 when every test passes, 1 otherwise.
 */
int main(void)
{
    test_two_point_linear();
    test_three_point_range();
    test_extrapolation();
    test_degenerate();

    if (s_fail)
    {
        printf("test_interpolate: FAILED\n");
        return 1;
    }
    printf("test_interpolate: OK\n");
    return 0;
}
