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

#include "chip_module.h"
#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "thermometer_module.h"

#include <stddef.h>

#define MS_PER_SECOND 1000u

/* The range the sensor is set up for, which a chip's own temperature stays in. */
#define TEMPERATURE_MIN_CELSIUS (-10)
#define TEMPERATURE_MAX_CELSIUS 80

/* Each entry's place in the table, which an event is posted by. */
enum
{
    CHIP_NAME,
    CHIP_FREE_MEMORY,
    CHIP_SECOND,
    THERMOMETER_MEASURE,
    THERMOMETER_READING,
    NATIVE_COUNT
};

static const char* TAG = "native-modules";

static temperature_sensor_handle_t s_thermometer;
static uint32_t s_posted_seconds;
static bool s_measure_asked;
static bool s_reading_ready;
static float s_reading;

static bool chip_name(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = erb_native_text("ESP32-S3");
    return true;
}

static bool chip_free_memory(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = (int32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
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
    [CHIP_NAME] = {CHIP_NAME_ENTRY, chip_name},
    [CHIP_FREE_MEMORY] = {CHIP_FREE_MEMORY_ENTRY, chip_free_memory},
    [CHIP_SECOND] = {CHIP_SECOND_ENTRY, NULL},
    [THERMOMETER_MEASURE] = {THERMOMETER_MEASURE_ENTRY, thermometer_measure},
    [THERMOMETER_READING] = {THERMOMETER_READING_ENTRY, NULL},
};

const uint16_t native_module_count = NATIVE_COUNT;

void native_modules_init(void)
{
    const temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(TEMPERATURE_MIN_CELSIUS, TEMPERATURE_MAX_CELSIUS);
    if (temperature_sensor_install(&config, &s_thermometer) != ESP_OK
        || temperature_sensor_enable(s_thermometer) != ESP_OK)
    {
        ESP_LOGW(TAG, "the temperature sensor didn't start; readings won't come");
        s_thermometer = NULL;
    }
}

void native_modules_frame(uint32_t uptime_ms, NativePost post)
{
    /* A post the runtime can't take now goes again next frame. */
    const uint32_t seconds = uptime_ms / MS_PER_SECOND;
    if (seconds != s_posted_seconds)
    {
        int32_t uptime[CHIP_SECOND_ARG_CELLS];
        erb_native_set_int64(&uptime[CHIP_SECOND_ARG_UPTIME_MS], (int64_t)uptime_ms);
        if (post(CHIP_SECOND, uptime))
        {
            s_posted_seconds = seconds;
        }
    }

    if (s_measure_asked && !s_reading_ready && s_thermometer)
    {
        s_reading_ready = temperature_sensor_get_celsius(s_thermometer, &s_reading) == ESP_OK;
        s_measure_asked = false;
    }
    if (s_reading_ready)
    {
        int32_t reading[THERMOMETER_READING_ARG_CELLS];
        erb_native_set_float(&reading[THERMOMETER_READING_ARG_CELSIUS], s_reading);
        if (post(THERMOMETER_READING, reading))
        {
            s_reading_ready = false;
        }
    }
}
