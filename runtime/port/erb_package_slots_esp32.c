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

#include "erb_package_slots_esp32.h"

#include "esp_log.h"
#include "esp_partition.h"

#include <string.h>

static const char* TAG = "erb-slots";

/** @brief The bytes of a slot's header that make it valid: clearing them takes no erase. */
#define HEADER_VALID_BYTES 16u

static const char* const k_labels[ERB_PACKAGE_SLOT_COUNT] = {"erb_a", "erb_b"};
static const esp_partition_t* s_partitions[ERB_PACKAGE_SLOT_COUNT];

/** @brief Programs a slot's header to zeros, so it no longer reads as valid. */
static void invalidate(unsigned slot, void* context)
{
    (void)context;
    static const uint8_t zeros[HEADER_VALID_BYTES] = {0};
    const esp_err_t err = esp_partition_write(s_partitions[slot], 0u, zeros, sizeof(zeros));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "couldn't clear slot %s: %s", k_labels[slot], esp_err_to_name(err));
    }
}

bool erb_package_slots_esp32_open(ErbPackageSlots* slots)
{
    memset(slots, 0, sizeof(*slots));
    slots->invalidate = invalidate;
    bool opened = true;
    for (unsigned slot = 0; slot < ERB_PACKAGE_SLOT_COUNT; slot++)
    {
        s_partitions[slot] =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, k_labels[slot]);
        if (!s_partitions[slot])
        {
            ESP_LOGE(TAG, "no '%s' partition: add it to partitions.csv", k_labels[slot]);
            opened = false;
            continue;
        }
        const void* mapped = NULL;
        esp_partition_mmap_handle_t handle;
        const esp_err_t err = esp_partition_mmap(
            s_partitions[slot], 0u, s_partitions[slot]->size, ESP_PARTITION_MMAP_DATA, &mapped, &handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "mapping '%s' failed: %s", k_labels[slot], esp_err_to_name(err));
            opened = false;
            continue;
        }
        slots->slots[slot].base = mapped;
        slots->slots[slot].bytes = s_partitions[slot]->size;
    }
    return opened;
}
