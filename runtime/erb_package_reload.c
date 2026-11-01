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

#include "erb_package_reload.h"

#include "er_error_screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define HINT_STILL_RUNNING "The last app is still running. Tap to go back to it."
#define HINT_SAVE_AGAIN "Fix it and save again."

/** @brief Longest line to the host, with its NUL. */
#define LINE_BYTES 224u

/** @brief Room for one line of erb_error_message(). */
#define ERROR_MESSAGE_BYTES 160u

/** @brief A PACKAGE_BEGIN frame's payload. */
#define BEGIN_PAYLOAD_BYTES 4u

/** @brief ErbError::function when no function was running. */
#define NO_FUNCTION 0xFFFFu

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Sends the host one line.
 *
 * @param[in] reload  The receiver.
 * @param[in] format  As for printf; its arguments follow.
 */
static void __attribute__((format(printf, 2, 3))) say(const ErbPackageReload* reload, const char* format, ...)
{
    char line[LINE_BYTES];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    reload->log(line, reload->log_context);
}

/** @brief A little-endian u32 from the host's bytes. */
static uint32_t read_u32(const void* bytes)
{
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

/**
 * @brief Tells the host and the panel why a package didn't make it.
 *
 * @param[in] reload  The receiver.
 * @param[in] error   The reason.
 */
static void report_failure(ErbPackageReload* reload, const ErbError* error)
{
    char message[ERROR_MESSAGE_BYTES];
    erb_error_message(error, message, sizeof(message));
    say(reload, "hot reload: %s", message);
    const bool running = erb_running();
    erb_show_error_hint(error, running ? HINT_STILL_RUNNING : HINT_SAVE_AGAIN);
    reload->reason_up = running;
    reload->receiving = false;
}

/**
 * @brief Reports a failure that isn't from loading, which has only a status and what went wrong.
 *
 * @param[in] reload  The receiver.
 * @param[in] status  The reason.
 * @param[in] detail  Which step failed.
 */
static void report_status(ErbPackageReload* reload, ErbStatus status, const char* detail)
{
    ErbError error;
    memset(&error, 0, sizeof(error));
    error.status = status;
    error.detail = detail;
    error.function = NO_FUNCTION;
    report_failure(reload, &error);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Frames
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Starts on a package: clears the staging slot's header first, so it can't read as whole until it is.
 *
 * @param[in] reload         The receiver.
 * @param[in] package_bytes  The package's length.
 */
static void begin(ErbPackageReload* reload, uint32_t package_bytes)
{
    const uint32_t slot_bytes = reload->slots.slots[reload->staging].bytes;
    if (package_bytes == 0u || slot_bytes <= ERB_PACKAGE_SLOT_HEADER_BYTES
        || package_bytes > slot_bytes - ERB_PACKAGE_SLOT_HEADER_BYTES)
    {
        ErbError error;
        memset(&error, 0, sizeof(error));
        error.status = ERB_ERR_TOO_BIG;
        error.needed = package_bytes;
        error.available = slot_bytes > ERB_PACKAGE_SLOT_HEADER_BYTES ? slot_bytes - ERB_PACKAGE_SLOT_HEADER_BYTES : 0u;
        error.resource = ERB_RESOURCE_SLOT;
        error.function = NO_FUNCTION;
        report_failure(reload, &error);
        return;
    }
    if (!reload->slots.erase((unsigned)reload->staging, 0u, ERB_PACKAGE_SLOT_HEADER_BYTES, reload->slots.context))
    {
        report_status(reload, ERB_ERR_SLOT, "erasing the staging slot's header failed");
        return;
    }
    reload->package_bytes = package_bytes;
    reload->received = 0u;
    reload->erased_to = ERB_PACKAGE_SLOT_HEADER_BYTES;
    reload->receiving = true;
    say(reload, "erb reload: ready %lu", (unsigned long)reload->chunk_bytes);
}

/**
 * @brief Loads the whole package, and gives its slot a header once it has.
 *
 * @param[in] reload  The receiver.
 *
 * @return true when the new app is running.
 */
static bool apply(ErbPackageReload* reload)
{
    reload->receiving = false;
    const unsigned slot = (unsigned)reload->staging;
    const uint8_t* package = (const uint8_t*)reload->slots.slots[slot].base + ERB_PACKAGE_SLOT_HEADER_BYTES;
    ErbError error;
    if (!erb_load(package, reload->package_bytes, &reload->config, &error))
    {
        report_failure(reload, &error);
        return false;
    }
    reload->reason_up = false;

    const unsigned other = slot == 0u ? 1u : 0u;
    uint32_t newest = 0u;
    erb_package_slot_read(&reload->slots.slots[other], NULL, NULL, &newest);
    const bool committed = erb_package_slot_commit(&reload->slots, slot, reload->package_bytes, newest + 1u);
    reload->active = (int)slot;
    reload->staging = (int)other;
    say(reload,
        "hot reload: ok (slot %c, %lu bytes%s)",
        'a' + slot,
        (unsigned long)reload->package_bytes,
        committed ? "" : "; couldn't write its header, so a reset runs the last package");
    return true;
}

/**
 * @brief Writes one chunk, erasing what it lands on first, and loads the package after the last.
 *
 * @param[in] reload       The receiver.
 * @param[in] frame_bytes  The frame's payload length.
 *
 * @return true when a new app started.
 */
static bool chunk(ErbPackageReload* reload, size_t frame_bytes)
{
    /* The app's load already answered, so this is a resend of a last chunk whose reply went missing. */
    if (!reload->receiving || frame_bytes <= ERB_PACKAGE_RELOAD_CHUNK_HEADER_BYTES)
    {
        return false;
    }
    const uint32_t offset = read_u32(reload->buffer);
    const uint32_t length = (uint32_t)(frame_bytes - ERB_PACKAGE_RELOAD_CHUNK_HEADER_BYTES);
    if (offset != reload->received)
    {
        say(reload, "erb reload: got %lu", (unsigned long)reload->received);
        return false;
    }
    const uint32_t remaining = reload->package_bytes - reload->received;
    if (length > reload->chunk_bytes || length > remaining
        || (length % ERB_PACKAGE_SLOT_PAGE_BYTES != 0u && length != remaining))
    {
        report_status(reload, ERB_ERR_CORRUPT, "a chunk doesn't fit the package");
        return false;
    }

    /* Flash takes whole pages, so a short last chunk is padded as erased flash. */
    uint8_t* data = (uint8_t*)reload->buffer + ERB_PACKAGE_RELOAD_CHUNK_HEADER_BYTES;
    const uint32_t over = length % ERB_PACKAGE_SLOT_PAGE_BYTES;
    const uint32_t padded = over ? length + ERB_PACKAGE_SLOT_PAGE_BYTES - over : length;
    memset(data + length, 0xFF, padded - length);

    const unsigned slot = (unsigned)reload->staging;
    const uint32_t at = ERB_PACKAGE_SLOT_HEADER_BYTES + offset;
    while (reload->erased_to < at + padded)
    {
        if (!reload->slots.erase(slot, reload->erased_to, ERB_PACKAGE_SLOT_SECTOR_BYTES, reload->slots.context))
        {
            report_status(reload, ERB_ERR_SLOT, "erasing the staging slot failed");
            return false;
        }
        reload->erased_to += ERB_PACKAGE_SLOT_SECTOR_BYTES;
    }
    if (!reload->slots.program(slot, at, data, padded, reload->slots.context))
    {
        report_status(reload, ERB_ERR_SLOT, "programming the staging slot failed");
        return false;
    }
    reload->received += length;
    say(reload, "erb reload: got %lu", (unsigned long)reload->received);
    return reload->received == reload->package_bytes ? apply(reload) : false;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public
 ---------------------------------------------------------------------------------------------------------------------*/

void erb_package_reload_init(ErbPackageReload* reload,
                             const ErbPackageSlots* slots,
                             const ErbConfig* config,
                             const ErbPackageSlotBoot* boot,
                             uint32_t* buffer,
                             uint32_t chunk_bytes,
                             void (*log)(const char* line, void* context),
                             void* log_context)
{
    memset(reload, 0, sizeof(*reload));
    reload->slots = *slots;
    reload->config = *config;
    reload->buffer = buffer;
    reload->chunk_bytes = chunk_bytes;
    reload->log = log;
    reload->log_context = log_context;
    reload->active = boot->active;
    reload->staging = boot->staging;
    er_hotreload_init(&reload->parser, (uint8_t*)buffer, ERB_PACKAGE_RELOAD_BUFFER_BYTES(chunk_bytes));
}

bool erb_package_reload_feed(ErbPackageReload* reload, const uint8_t* data, size_t length)
{
    bool started = false;
    size_t done = 0u;
    while (done < length)
    {
        size_t consumed = 0u;
        size_t frame_bytes = 0u;
        const ErHotReloadStatus status =
            er_hotreload_feed(&reload->parser, data + done, length - done, &consumed, &frame_bytes);
        done += consumed;
        if (status == ER_HR_NEED_MORE)
        {
            break;
        }
        if (status != ER_HR_FRAME_READY)
        {
            say(reload, "dropped frame: %s", er_hotreload_status_str(status));
            continue;
        }
        switch (reload->parser.msg_type)
        {
            case ER_HR_MSG_PACKAGE_BEGIN:
                if (frame_bytes == BEGIN_PAYLOAD_BYTES)
                {
                    begin(reload, read_u32(reload->buffer));
                }
                break;
            case ER_HR_MSG_PACKAGE_CHUNK:
                started = chunk(reload, frame_bytes) || started;
                break;
            default:
                say(reload, "hot reload: this firmware runs Flow B packages: use dev --device --aot");
                break;
        }
    }
    return started;
}

void erb_package_reload_tap(ErbPackageReload* reload)
{
    if (reload->reason_up && erb_running())
    {
        er_error_screen_hide();
    }
    reload->reason_up = false;
}
