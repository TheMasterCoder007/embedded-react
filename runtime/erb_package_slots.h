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

#ifndef ERB_PACKAGE_SLOTS_H
#define ERB_PACKAGE_SLOTS_H

/*
 * erb_package_slots — the two flash slots a board keeps packages in (website/docs/internals/flow-b-packages.md).
 *
 * A slot holds a header in an erase sector of its own, then a package:
 *   magic "ERBS" | sequence u32 | package_bytes u32 | crc32 u32 (over the first 12 bytes) | 0xFF padding
 * The header is written after the package, so a valid header means the whole package was written. The valid slot with the newest sequence is the active one, and the other is staging, where the next
 * package goes. A package that fails to load is given up on and the other slot runs, so the last good
 * package stays until a new one passes verification.
 *
 * The writer is packages/embedded-react/flash.mjs. The board maps the slots and clears a header.
 */

#include "erb_runtime.h"

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define ERB_PACKAGE_SLOT_COUNT 2u

/** @brief Bytes before a slot's package: one erase sector, so the header can be written, or cleared, on its own. */
#define ERB_PACKAGE_SLOT_HEADER_BYTES 4096u

#define ERB_PACKAGE_SLOT_MAGIC 0x53425245u

/** @brief No slot. */
#define ERB_PACKAGE_SLOT_NONE (-1)

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief One slot, memory-mapped. */
typedef struct
{
    const void* base; /**< The slot's first byte, 4-byte aligned. */
    uint32_t bytes;   /**< The slot's size, header included. */
} ErbPackageSlot;

/** @brief The board's slots. */
typedef struct
{
    ErbPackageSlot slots[ERB_PACKAGE_SLOT_COUNT];
    void (*invalidate)(unsigned slot, void* context); /**< Clears a slot's header in flash. */
    void* context;                                    /**< Passed to invalidate. */
} ErbPackageSlots;

/** @brief What erb_package_slots_boot() did. */
typedef struct
{
    int active;     /**< The slot running, or ERB_PACKAGE_SLOT_NONE. */
    int staging;    /**< The slot the next package goes into. */
    int rejected;   /**< The newest slot, when it failed to load; else ERB_PACKAGE_SLOT_NONE. */
    ErbError error; /**< Why it failed, or ERB_ERR_NO_PACKAGE. Status ERB_OK when neither happened. */
} ErbPackageSlotBoot;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reads a slot's header. It says nothing of the package, which erb_load() checks whole.
 *
 * @param[in]  slot           The slot.
 * @param[out] package        Receives the package's first byte; may be NULL.
 * @param[out] package_bytes  Receives its length; may be NULL.
 * @param[out] sequence       Receives its sequence; may be NULL.
 *
 * @return true when the slot has a valid header.
 */
bool erb_package_slot_read(const ErbPackageSlot* slot,
                           const uint8_t** package,
                           uint32_t* package_bytes,
                           uint32_t* sequence);

/**
 * @brief Loads the newest package that passes, at boot, and gives up on a newer one that didn't.
 *
 * When the newest package fails and the other runs, the failed slot's header is cleared, so the last good
 * package is the newest again and the next one is written over the failed one. When neither loads, both are
 * kept, and each boot shows the same reason.
 *
 * @param[in]  slots   The board's slots.
 * @param[in]  config  As for erb_load().
 * @param[out] boot    Receives what happened.
 *
 * @return true when an app is running.
 */
bool erb_package_slots_boot(const ErbPackageSlots* slots, const ErbConfig* config, ErbPackageSlotBoot* boot);

/**
 * @brief Puts up the error screen for a boot with an error, saying what to do next. Over a running app it
 *        stays until er_error_screen_hide().
 *
 * @param[in] boot  From erb_package_slots_boot().
 */
void erb_package_slots_show_error(const ErbPackageSlotBoot* boot);

#endif
