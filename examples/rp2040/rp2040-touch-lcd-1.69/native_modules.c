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

#include "native_modules.h"

#include "erb_runtime.h"
#include "hardware/adc.h"

#include <malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#define MS_PER_SECOND 1000u

/* The temperature sensor is the ADC's fifth input. Its voltage falls 1.721 mV a degree from 0.706 V at 27 °C. */
#define TEMPERATURE_INPUT 4u
#define ADC_VOLTS 3.3f
#define ADC_STEPS 4096.0f
#define SENSOR_VOLTS_AT_27 0.706f
#define SENSOR_VOLTS_PER_DEGREE 0.001721f
#define SENSOR_REFERENCE_CELSIUS 27.0f

enum
{
    CHIP_NAME,
    CHIP_FREE_MEMORY,
    CHIP_SECOND,
    THERMOMETER_MEASURE,
    THERMOMETER_READING,
    MOTION_STEPS,
    MOTION_TILT,
    NATIVE_COUNT
};

/* main.c's _sbrk hands out RAM up to the stack guard at the top of main RAM. */
extern char __HeapLimit;

static uint32_t s_posted_seconds;
static bool s_measure_asked;

static int32_t s_steps;
static bool s_steps_due;
static int32_t s_tilt[2];
static bool s_tilt_due;
static bool s_motion_known;

static bool chip_name(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = erb_native_text("RP2040");
    return true;
}

/* What malloc can still have: past the heap's end, and what was freed inside it. */
static bool chip_free_memory(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    const char* const limit = &__HeapLimit - STACK_GUARD_BYTES;
    const struct mallinfo info = mallinfo();
    *result = (int32_t)(limit - (const char*)sbrk(0)) + (int32_t)info.fordblks;
    return true;
}

/* A call must be quick, so this only asks: the frame loop reads the sensor and posts the reading. */
static bool thermometer_measure(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)result;
    (void)context;
    s_measure_asked = true;
    return true;
}

const ErbNative native_modules[NATIVE_COUNT] = {
    [CHIP_NAME] = {"chip.name", 1, "s:", chip_name},
    [CHIP_FREE_MEMORY] = {"chip.freeMemory", 1, "i:", chip_free_memory},
    [CHIP_SECOND] = {"chip.second", 1, "v:l", NULL},
    [THERMOMETER_MEASURE] = {"thermometer.measure", 1, "v:", thermometer_measure},
    [THERMOMETER_READING] = {"thermometer.reading", 1, "v:f", NULL},
    [MOTION_STEPS] = {"motion.steps", 1, "v:i", NULL},
    [MOTION_TILT] = {"motion.tilt", 1, "v:{x:i,y:i}", NULL},
};

const uint16_t native_module_count = NATIVE_COUNT;

void native_modules_init(void)
{
    adc_init();
    adc_set_temp_sensor_enabled(true);
}

void native_modules_set_steps(uint32_t steps)
{
    if (!s_motion_known || (int32_t)steps != s_steps)
    {
        s_steps = (int32_t)steps;
        s_steps_due = true;
    }
    s_motion_known = true;
}

void native_modules_set_tilt(int x, int y)
{
    if (x != s_tilt[0] || y != s_tilt[1])
    {
        s_tilt[0] = x;
        s_tilt[1] = y;
        s_tilt_due = true;
    }
}

void native_modules_repost(void)
{
    s_steps_due = s_motion_known;
    s_tilt_due = s_motion_known;
}

/** @brief Reads the chip's temperature: a couple of microseconds. */
static float read_celsius(void)
{
    adc_select_input(TEMPERATURE_INPUT);
    const float volts = (float)adc_read() * ADC_VOLTS / ADC_STEPS;
    return SENSOR_REFERENCE_CELSIUS - (volts - SENSOR_VOLTS_AT_27) / SENSOR_VOLTS_PER_DEGREE;
}

void native_modules_frame(uint32_t uptime_ms)
{
    /* A post the runtime can't take now goes again next frame. */
    const uint32_t seconds = uptime_ms / MS_PER_SECOND;
    if (seconds != s_posted_seconds)
    {
        int32_t uptime[2];
        erb_native_set_int64(uptime, (int64_t)uptime_ms);
        if (erb_native_post(CHIP_SECOND, uptime))
        {
            s_posted_seconds = seconds;
        }
    }

    if (s_measure_asked)
    {
        const float celsius = read_celsius();
        int32_t value;
        memcpy(&value, &celsius, sizeof(value));
        s_measure_asked = !erb_native_post(THERMOMETER_READING, &value);
    }
    if (s_steps_due)
    {
        s_steps_due = !erb_native_post(MOTION_STEPS, &s_steps);
    }
    if (s_tilt_due)
    {
        s_tilt_due = !erb_native_post(MOTION_TILT, s_tilt);
    }
}
