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

#include "erb_package_slots.h"

#include "er_container.h"

#include <stddef.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define HINT_NO_PACKAGE "Install one with: embedded-react flash"
#define HINT_LAST_GOOD "Running the last good package."

/** @brief The header bytes its CRC covers. */
#define HEADER_CHECKED_BYTES offsetof(SlotHeader, crc)

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A slot's header, little-endian like the rest of the format. */
typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t package_bytes;
    uint32_t crc;
} SlotHeader;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

bool erb_package_slot_read(const ErbPackageSlot* slot,
                           const uint8_t** package,
                           uint32_t* package_bytes,
                           uint32_t* sequence)
{
    if (!slot->base || ((uintptr_t)slot->base & 3u) != 0u || slot->bytes <= ERB_PACKAGE_SLOT_HEADER_BYTES)
    {
        return false;
    }
    SlotHeader header;
    memcpy(&header, slot->base, sizeof(header));
    if (header.magic != ERB_PACKAGE_SLOT_MAGIC || header.crc != er_crc32((const uint8_t*)&header, HEADER_CHECKED_BYTES)
        || header.package_bytes == 0u || header.package_bytes > slot->bytes - ERB_PACKAGE_SLOT_HEADER_BYTES)
    {
        return false;
    }
    if (package)
    {
        *package = (const uint8_t*)slot->base + ERB_PACKAGE_SLOT_HEADER_BYTES;
    }
    if (package_bytes)
    {
        *package_bytes = header.package_bytes;
    }
    if (sequence)
    {
        *sequence = header.sequence;
    }
    return true;
}

bool erb_package_slots_boot(const ErbPackageSlots* slots, const ErbConfig* config, ErbPackageSlotBoot* boot)
{
    memset(boot, 0, sizeof(*boot));
    boot->active = ERB_PACKAGE_SLOT_NONE;
    boot->rejected = ERB_PACKAGE_SLOT_NONE;

    const uint8_t* packages[ERB_PACKAGE_SLOT_COUNT] = {NULL, NULL};
    uint32_t lengths[ERB_PACKAGE_SLOT_COUNT] = {0u, 0u};
    uint32_t sequences[ERB_PACKAGE_SLOT_COUNT] = {0u, 0u};
    bool valid[ERB_PACKAGE_SLOT_COUNT];
    for (unsigned slot = 0; slot < ERB_PACKAGE_SLOT_COUNT; slot++)
    {
        valid[slot] = erb_package_slot_read(&slots->slots[slot], &packages[slot], &lengths[slot], &sequences[slot]);
    }

    /* A signed difference, so the order survives the sequence wrapping. */
    const bool second_is_newer = valid[1] && (!valid[0] || (int32_t)(sequences[1] - sequences[0]) > 0);
    const int newest = second_is_newer ? 1 : 0;
    const int older = second_is_newer ? 0 : 1;
    boot->staging = valid[newest] ? older : 0;

    if (!valid[newest])
    {
        erb_load(NULL, 0u, config, &boot->error); /* no package: remembers the panel, for the error screen */
        return false;
    }
    if (erb_load(packages[newest], lengths[newest], config, &boot->error))
    {
        boot->active = newest;
        return true;
    }

    boot->rejected = newest;
    if (!valid[older])
    {
        return false;
    }
    ErbError older_error;
    if (!erb_load(packages[older], lengths[older], config, &older_error))
    {
        return false;
    }
    boot->active = older;
    boot->staging = newest;
    if (slots->invalidate)
    {
        slots->invalidate((unsigned)newest, slots->context);
    }
    return true;
}

void erb_package_slots_show_error(const ErbPackageSlotBoot* boot)
{
    if (boot->error.status == ERB_OK)
    {
        return;
    }
    const char* hint = NULL;
    if (boot->active != ERB_PACKAGE_SLOT_NONE)
    {
        hint = HINT_LAST_GOOD;
    }
    else if (boot->error.status == ERB_ERR_NO_PACKAGE)
    {
        hint = HINT_NO_PACKAGE;
    }
    erb_show_error_hint(&boot->error, hint);
}
