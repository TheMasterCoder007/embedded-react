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
 * The no-init fault record: filling it, sealing it and reading it back.
 *
 * This file is apart from the error screen because a fault handler runs it. A firmware that moves cold code out of
 * internal flash must leave this file where the core can run it during a hard fault: with the MPU off, as it is
 * there by default, external RAM is not executable.
 */

#include "er_error_screen.h"

#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/* The fault record's check word. */
#define FAULT_MAGIC 0x45524654u /* "ERFT" */
#define FAULT_HASH_SEED 2166136261u
#define FAULT_HASH_PRIME 16777619u

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The check word for a fault record's contents: every byte before it. */
static uint32_t fault_check(const ERFaultRecord* record)
{
    const unsigned char* bytes = (const unsigned char*)record;
    uint32_t hash = FAULT_HASH_SEED;
    for (size_t index = 0u; index < offsetof(ERFaultRecord, check); index++)
    {
        hash = (hash ^ bytes[index]) * FAULT_HASH_PRIME;
    }
    return hash;
}

/**
 * @brief Writes a record's check word, which makes it whole.
 *
 * RAM with error correction can hold the last write back until the next one, and a reset then loses it. So the
 * check word is never the last write: the magic is written again after it, where losing the write changes nothing.
 */
static void fault_seal(ERFaultRecord* record)
{
    record->check = fault_check(record);
    *(volatile uint32_t*)&record->magic = FAULT_MAGIC;
}

/**
 * @brief Copies @p source into @p destination and fills the rest with NULs, with no library call. A source that
 *        doesn't fit keeps its start, or its end when @p keep_end is set.
 */
static void fault_copy_text(char* destination, size_t capacity, const char* source, bool keep_end)
{
    size_t length = 0u;
    while (source && source[length])
    {
        length++;
    }
    const size_t skipped = keep_end && length >= capacity ? length - (capacity - 1u) : 0u;
    size_t index = 0u;
    for (; index + 1u < capacity && skipped + index < length; index++)
    {
        destination[index] = source[skipped + index];
    }
    for (; index < capacity; index++)
    {
        destination[index] = '\0';
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_fault_record(ERFaultRecord* record, const char* reason, uint32_t program_counter, uint32_t address)
{
    unsigned char* bytes = (unsigned char*)record;
    for (size_t index = 0u; index < sizeof(*record); index++)
    {
        bytes[index] = 0u;
    }
    record->magic = FAULT_MAGIC;
    record->version = ER_FAULT_VERSION;
    record->program_counter = program_counter;
    record->address = address;
    fault_copy_text(record->reason, sizeof(record->reason), reason, false);
    fault_seal(record);
}

void er_fault_record_registers(ERFaultRecord* record,
                               uint8_t core,
                               uint32_t link_register,
                               uint32_t stack_pointer,
                               const uint32_t* status,
                               unsigned status_count)
{
    record->core = core;
    record->link_register = link_register;
    record->stack_pointer = stack_pointer;
    for (unsigned index = 0u; index < ER_FAULT_STATUS_WORDS; index++)
    {
        record->status[index] = status && index < status_count ? status[index] : 0u;
    }
    fault_seal(record);
}

void er_fault_record_stack(ERFaultRecord* record, const uint32_t* words, unsigned count, bool callers)
{
    if (count > ER_FAULT_STACK_WORDS)
    {
        count = ER_FAULT_STACK_WORDS;
    }
    /* One word at a time, with the record whole after each: reading the next may be what faults. */
    record->stack_count = 0u;
    record->flags = (uint8_t)(callers ? record->flags | ER_FAULT_FLAG_CALLERS : record->flags & ~ER_FAULT_FLAG_CALLERS);
    fault_seal(record);
    for (unsigned index = 0u; index < count; index++)
    {
        record->stack[index] = words[index];
        record->stack_count = (uint8_t)(index + 1u);
        fault_seal(record);
    }
}

void er_fault_record_app(ERFaultRecord* record, const ERFaultApp* app)
{
    record->flags |= ER_FAULT_FLAG_APP;
    record->app_function = app->function;
    record->app_panel = app->panel;
    record->app_offset = app->offset;
    record->app_build_id = app->build_id;
    record->app_line = app->file ? app->line : 0u;
    fault_copy_text(record->app_file, sizeof(record->app_file), app->file, true);
    fault_seal(record);
}

size_t er_fault_text(const ERFaultRecord* record, char* buffer, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    if (!buffer || capacity == 0u)
    {
        return 0u;
    }
    buffer[0] = '\0';
    if (!er_fault_recorded(record) || capacity < ER_FAULT_TEXT_BYTES)
    {
        return 0u;
    }
    const unsigned char* bytes = (const unsigned char*)record;
    for (size_t index = 0u; index < sizeof(*record); index++)
    {
        buffer[2u * index] = digits[bytes[index] >> 4];
        buffer[2u * index + 1u] = digits[bytes[index] & 0x0Fu];
    }
    buffer[2u * sizeof(*record)] = '\0';
    return 2u * sizeof(*record);
}

bool er_fault_recorded(const ERFaultRecord* record)
{
    return record->magic == FAULT_MAGIC && record->version == ER_FAULT_VERSION && record->check == fault_check(record);
}

void er_fault_clear(ERFaultRecord* record)
{
    record->magic = 0u;
}
