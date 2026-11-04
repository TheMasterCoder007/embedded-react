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
 * On-device hot reload: a package coming in over frames, into the staging slot, and what a bad one leaves behind.
 * Flash here behaves as it does on a board: it erases whole sectors and programs whole pages, which only clear bits.
 */

#include "er_container.h"
#include "er_error_screen.h"
#include "erb_internal.h"
#include "erb_package.h"
#include "erb_package_reload.h"
#include "native_renderer.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Harness
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_failed = 0;
static int s_checked = 0;

#define SLOT_WORDS 8192u
#define SLOT_BYTES ((uint32_t)(SLOT_WORDS * sizeof(uint32_t)))
#define CHUNK_BYTES 512u
#define PACKAGE_MAX_BYTES 65536u
#define FRAME_MAX_BYTES (ER_HR_HEADER_SIZE + ERB_PACKAGE_RELOAD_BUFFER_BYTES(CHUNK_BYTES))
#define LAST_LINE_BYTES 256u

/** @brief A sequence past which the next one wraps. */
#define SEQUENCE_LAST 0xFFFFFFFFu

/** @brief What a package has wrong with it. */
enum
{
    GOOD,
    BAD_CRC,
    BOOT_TRAP,
    TOO_BIG,
};

static uint32_t s_flash[ERB_PACKAGE_SLOT_COUNT][SLOT_WORDS];
static uint32_t s_arena[TEST_ARENA_WORDS];
static uint32_t s_buffer[ERB_PACKAGE_RELOAD_BUFFER_BYTES(CHUNK_BYTES) / sizeof(uint32_t)];
static uint8_t s_package[PACKAGE_MAX_BYTES];
static TPkg s_package_spec;
static ErbConfig s_config;
static ErbPackageSlots s_slots;
static ErbPackageSlotBoot s_boot;
static ErbPackageReload s_reload;
static char s_last_line[LAST_LINE_BYTES];
static char s_outcome[LAST_LINE_BYTES];
static unsigned s_got_lines;
static unsigned s_dropped_lines;
static unsigned s_ready_lines;
static uint32_t s_last_got;
static bool s_flash_fails;
static bool s_header_fails;
static unsigned s_erases;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        s_checked++;                                                                                                   \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL line %d: %s (last: %s)\n", __LINE__, (message), s_last_line);                                 \
            s_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool sensors_read(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = arguments[0] + 1;
    return true;
}

static const ErbNative s_natives[] = {{"sensors.read", 1, "i:i", sensors_read}};

static void null_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

static bool flash_erase(unsigned slot, uint32_t offset, uint32_t bytes, void* context)
{
    (void)context;
    s_erases++;
    CHECK(offset % ERB_PACKAGE_SLOT_SECTOR_BYTES == 0u && bytes % ERB_PACKAGE_SLOT_SECTOR_BYTES == 0u,
          "erases are whole sectors");
    CHECK(offset + bytes <= SLOT_BYTES, "and inside the slot");
    if (s_flash_fails)
    {
        return false;
    }
    memset((uint8_t*)s_flash[slot] + offset, 0xFF, bytes);
    return true;
}

static bool flash_program(unsigned slot, uint32_t offset, const void* data, uint32_t bytes, void* context)
{
    (void)context;
    CHECK(offset % ERB_PACKAGE_SLOT_PAGE_BYTES == 0u && bytes % ERB_PACKAGE_SLOT_PAGE_BYTES == 0u,
          "programs are whole pages");
    CHECK(offset + bytes <= SLOT_BYTES, "and inside the slot");
    if (s_flash_fails || (s_header_fails && offset == 0u))
    {
        return false;
    }
    uint8_t* flash = (uint8_t*)s_flash[slot] + offset;
    const uint8_t* source = (const uint8_t*)data;
    for (uint32_t index = 0; index < bytes; index++)
    {
        CHECK((source[index] & flash[index]) == source[index], "programming only clears bits, so it was erased first");
        flash[index] &= source[index];
    }
    return true;
}

static void reload_log(const char* line, void* context)
{
    (void)context;
    snprintf(s_last_line, sizeof(s_last_line), "%s", line);
    uint32_t got = 0u;
    if (sscanf(line, "erb reload: got %u", &got) == 1)
    {
        s_got_lines++;
        s_last_got = got;
    }
    else if (strncmp(line, "erb reload: ready", 17) == 0)
    {
        s_ready_lines++;
    }
    else if (strncmp(line, "dropped frame", 13) == 0)
    {
        s_dropped_lines++;
    }
    else
    {
        snprintf(s_outcome, sizeof(s_outcome), "%s", line);
    }
}

/** @brief Empty flash and a config the baseline package fits. */
static void reset(void)
{
    erb_unload();
    er_error_screen_hide();
    memset(s_flash, 0xFF, sizeof(s_flash));
    memset(&s_config, 0, sizeof(s_config));
    s_config.arena = s_arena;
    s_config.arena_bytes = sizeof(s_arena);
    s_config.screen_w = TPKG_SCREEN;
    s_config.screen_h = TPKG_SCREEN;
    s_config.natives = s_natives;
    s_config.native_count = 1;
    memset(&s_slots, 0, sizeof(s_slots));
    for (unsigned slot = 0; slot < ERB_PACKAGE_SLOT_COUNT; slot++)
    {
        s_slots.slots[slot].base = s_flash[slot];
        s_slots.slots[slot].bytes = SLOT_BYTES;
    }
    s_slots.erase = flash_erase;
    s_slots.program = flash_program;
    s_last_line[0] = '\0';
    s_outcome[0] = '\0';
    s_got_lines = 0u;
    s_dropped_lines = 0u;
    s_ready_lines = 0u;
    s_flash_fails = false;
    s_header_fails = false;
    s_erases = 0u;
}

/** @brief Boots whatever flash holds, and readies the receiver as a board does. */
static void boot(void)
{
    erb_package_slots_boot(&s_slots, &s_config, &s_boot);
    er_error_screen_hide();
    erb_package_reload_init(&s_reload, &s_slots, &s_config, &s_boot, s_buffer, CHUNK_BYTES, reload_log, NULL);
}

/** @brief Builds a package of the given kind into s_package. */
static uint32_t build(int kind)
{
    tpkg_base(&s_package_spec);
    if (kind == BOOT_TRAP)
    {
        const uint32_t trap[] = {OP_ABX(TRAP, 0, STRING_BOOM)};
        const TFunction keep = s_package_spec.functions[FUNCTION_BOOT];
        tpkg_set_code(&s_package_spec, FUNCTION_BOOT, s_package_spec.types + keep.types, keep.registers, trap, 1);
    }
    if (kind == TOO_BIG)
    {
        s_package_spec.panels[0].arena_bytes = sizeof(s_arena) * 2u;
    }
    const uint32_t length = tpkg_build(&s_package_spec, s_package);
    if (kind == BAD_CRC)
    {
        s_package[length - 1u] ^= 0xFFu;
    }
    return length;
}

/** @brief Writes a slot as `embedded-react flash` does. */
static void install(unsigned slot, uint32_t sequence, int kind)
{
    const uint32_t length = build(kind);
    uint8_t* image = (uint8_t*)s_flash[slot];
    memcpy(image + ERB_PACKAGE_SLOT_HEADER_BYTES, s_package, length);
    uint32_t header[4] = {ERB_PACKAGE_SLOT_MAGIC, sequence, length, 0u};
    header[3] = er_crc32((const uint8_t*)header, 12u);
    memcpy(image, header, sizeof(header));
}

/** @brief Encodes a frame. */
static size_t frame(uint8_t* out, uint8_t type, const void* payload, uint32_t bytes)
{
    memset(out, 0, ER_HR_HEADER_SIZE);
    memcpy(out, "ERHR", 4u);
    out[4] = ER_HR_PROTO_VERSION;
    out[5] = type;
    memcpy(out + 8, &bytes, sizeof(bytes));
    const uint32_t checksum = er_crc32((const uint8_t*)payload, bytes);
    memcpy(out + 12, &checksum, sizeof(checksum));
    memcpy(out + ER_HR_HEADER_SIZE, payload, bytes);
    return ER_HR_HEADER_SIZE + bytes;
}

/** @brief Sends a package's begin frame. */
static bool send_begin(uint32_t length)
{
    uint8_t out[ER_HR_HEADER_SIZE + sizeof(length)];
    const size_t bytes = frame(out, ER_HR_MSG_PACKAGE_BEGIN, &length, sizeof(length));
    return erb_package_reload_feed(&s_reload, out, bytes);
}

/** @brief Sends the chunk at @p offset of s_package. */
static bool send_chunk(uint32_t offset, uint32_t bytes)
{
    uint8_t payload[ERB_PACKAGE_RELOAD_BUFFER_BYTES(CHUNK_BYTES)];
    memcpy(payload, &offset, sizeof(offset));
    memcpy(payload + sizeof(offset), s_package + offset, bytes);
    uint8_t out[FRAME_MAX_BYTES];
    const size_t frame_bytes = frame(out, ER_HR_MSG_PACKAGE_CHUNK, payload, bytes + sizeof(offset));
    return erb_package_reload_feed(&s_reload, out, frame_bytes);
}

/** @brief Sends the whole of s_package as the host does. */
static bool send_package(uint32_t length)
{
    bool started = send_begin(length);
    for (uint32_t offset = 0; offset < length; offset += CHUNK_BYTES)
    {
        const uint32_t remaining = length - offset;
        started = send_chunk(offset, remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES) || started;
    }
    return started;
}

static bool slot_valid(unsigned slot)
{
    return erb_package_slot_read(&s_slots.slots[slot], NULL, NULL, NULL);
}

static uint32_t slot_sequence(unsigned slot)
{
    uint32_t sequence = 0u;
    erb_package_slot_read(&s_slots.slots[slot], NULL, NULL, &sequence);
    return sequence;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_first_package(void)
{
    reset();
    boot();
    CHECK(!erb_running() && s_reload.staging == 0, "an empty board runs nothing, and the first slot is next");

    const uint32_t length = build(GOOD);
    CHECK(send_package(length), "a whole package starts");
    CHECK(s_ready_lines == 1u, "the board says the chunk size");
    CHECK(s_last_got == length, "and how far it has got");
    CHECK(strncmp(s_outcome, "hot reload: ok", 14) == 0, "and then that it runs");
    CHECK(erb_running(), "the app runs");
    CHECK(!er_error_screen_active(), "with the no-package screen gone");
    CHECK(slot_valid(0u) && slot_sequence(0u) == 1u, "its slot has a header");
    CHECK(s_reload.active == 0 && s_reload.staging == 1, "and the next package goes in the other");
    CHECK(memcmp((const uint8_t*)s_flash[0] + ERB_PACKAGE_SLOT_HEADER_BYTES, s_package, length) == 0,
          "with the package whole in flash");

    ErbPackageSlotBoot after;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &after) && after.active == 0, "a reset runs it from flash");
}

static void test_swaps_slots(void)
{
    reset();
    install(0, 5u, GOOD);
    boot();
    CHECK(erb_running() && s_reload.active == 0, "the board runs its package");

    const uint32_t length = build(GOOD);
    CHECK(send_package(length), "a new one starts");
    CHECK(slot_sequence(1u) == 6u, "with the next sequence");
    CHECK(s_reload.active == 1 && s_reload.staging == 0, "the slots swap");
    CHECK(slot_valid(0u) && slot_sequence(0u) == 5u, "and the old package stays to fall back on");

    CHECK(send_package(length), "and another");
    CHECK(slot_sequence(0u) == 7u && s_reload.active == 0, "goes back in the first slot");

    ErbPackageSlotBoot after;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &after) && after.active == 0, "a reset runs the newest");
}

static void test_sequence_wraps(void)
{
    reset();
    install(0, SEQUENCE_LAST, GOOD);
    boot();
    CHECK(send_package(build(GOOD)), "a package loads");
    ErbPackageSlotBoot after;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &after) && after.active == 1, "and is newest across the wrap");
}

static void test_rejected_keeps_running(void)
{
    static const struct
    {
        int kind;
        ErbStatus status;
    } cases[] = {
        {BAD_CRC, ERB_ERR_CORRUPT},
        {TOO_BIG, ERB_ERR_TOO_BIG},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        reset();
        install(0, 5u, GOOD);
        boot();
        const int input = erb_input_find("steps");
        CHECK(input >= 0, "the running app has its input");
        CHECK(erb_input_set_int(input, 42), "and takes values");
        static uint32_t arena_before[sizeof(s_arena) / sizeof(s_arena[0])];
        memcpy(arena_before, s_arena, sizeof(s_arena));

        CHECK(!send_package(build(cases[index].kind)), "a bad package doesn't start");
        CHECK(erb_last_error()->status == cases[index].status, "with the reason kept");
        CHECK(strncmp(s_outcome, "hot reload: ", 12) == 0 && strncmp(s_outcome, "hot reload: ok", 14) != 0,
              "the host hears why");
        CHECK(erb_running(), "the running app is untouched");
        CHECK(memcmp(s_arena, arena_before, sizeof(s_arena)) == 0, "with its state");
        CHECK(er_error_screen_active(), "and the panel shows why");
        CHECK(!slot_valid(1u), "its slot has no header, so a reset won't try it");
        CHECK(slot_valid(0u) && s_reload.active == 0 && s_reload.staging == 1, "and the slots stay as they were");

        erb_package_reload_tap(&s_reload);
        CHECK(!er_error_screen_active() && erb_running(), "a tap goes back to the app");

        CHECK(!send_package(build(cases[index].kind)), "a second bad one is the same");
        CHECK(er_error_screen_active(), "with the reason up");
        CHECK(send_package(build(GOOD)), "and a good one starts");
        CHECK(!er_error_screen_active(), "taking the reason down");
        CHECK(slot_valid(1u) && slot_sequence(1u) == 6u, "in the same slot");
    }
}

static void test_boot_fault(void)
{
    reset();
    install(0, 5u, GOOD);
    boot();
    CHECK(!send_package(build(BOOT_TRAP)), "a package that faults at boot doesn't start");
    CHECK(erb_last_error()->status == ERB_ERR_TRAP, "with the reason kept");
    CHECK(!erb_running(), "the old app is gone by then");
    CHECK(er_error_screen_active(), "and the panel shows why");
    erb_package_reload_tap(&s_reload);
    CHECK(er_error_screen_active(), "a tap has nothing to go back to");
    CHECK(!slot_valid(1u) && s_reload.staging == 1, "its slot has no header");

    CHECK(send_package(build(GOOD)), "the next save fixes it");
    CHECK(erb_running() && !er_error_screen_active() && slot_sequence(1u) == 6u, "and runs");

    /* Rejected at boot the same way, so a reset goes to the last package that loaded. */
    reset();
    install(0, 5u, GOOD);
    boot();
    send_package(build(BOOT_TRAP));
    ErbPackageSlotBoot after;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &after) && after.active == 0, "a reset runs the last good");
    CHECK(after.error.status == ERB_OK, "with nothing to report");
}

static void test_too_big_for_slot(void)
{
    reset();
    install(0, 5u, GOOD);
    boot();
    send_begin(SLOT_BYTES);
    CHECK(s_ready_lines == 0u && s_erases == 0u, "a package past the slot's end touches nothing");
    CHECK(strstr(s_outcome, "package slot") != NULL, "the host hears why");
    CHECK(erb_running() && er_error_screen_active(), "the app runs on, with the reason up");
    CHECK(slot_valid(0u), "and its slot whole");

    s_outcome[0] = '\0';
    send_begin(0u);
    CHECK(s_ready_lines == 0u && s_outcome[0] != '\0', "nor does an empty one get anywhere");
}

static void test_fills_the_slot(void)
{
    /* A short last chunk is padded to a page, which still has to land inside the slot. */
    const uint32_t slot_room = SLOT_BYTES - ERB_PACKAGE_SLOT_HEADER_BYTES;
    const uint32_t lengths[] = {slot_room, slot_room - 1u};
    for (unsigned index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++)
    {
        reset();
        install(0, 5u, GOOD);
        boot();
        memset(s_package, 0x5A, lengths[index]);
        send_package(lengths[index]);
        CHECK(s_last_got == lengths[index], "every chunk is written");
        CHECK(strstr(s_outcome, "package slot") == NULL, "with flash taking all of it");
        CHECK(erb_running() && slot_valid(0u), "and the app runs on past the bad package");
    }
}

static void test_missing_slot(void)
{
    /* A board that can't open a slot leaves it with no bytes, and has no flash to erase behind it. */
    reset();
    install(0, 5u, GOOD);
    s_slots.slots[1].base = NULL;
    s_slots.slots[1].bytes = 0u;
    boot();
    send_begin(build(GOOD));
    CHECK(s_ready_lines == 0u && s_erases == 0u, "a missing staging slot touches no flash");
    CHECK(strstr(s_outcome, "package slot") != NULL, "the host hears why");
    CHECK(erb_running(), "and the app runs on");

    reset();
    s_slots.slots[0].base = NULL;
    s_slots.slots[0].bytes = 0u;
    s_slots.slots[1].base = NULL;
    s_slots.slots[1].bytes = 0u;
    boot();
    send_begin(build(GOOD));
    CHECK(s_ready_lines == 0u && s_erases == 0u, "nor does a board with no slots at all");
}

static void test_interrupted(void)
{
    reset();
    install(0, 5u, GOOD);
    install(1, 4u, GOOD);
    boot();
    const uint32_t length = build(GOOD);
    send_begin(length);
    CHECK(!slot_valid(1u), "starting clears the staging slot's header");
    send_chunk(0u, CHUNK_BYTES);
    CHECK(erb_running() && slot_valid(0u), "the app runs on while it comes in");

    ErbPackageSlotBoot after;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &after) && after.active == 0, "a reset now runs the active slot");

    boot();
    CHECK(s_reload.active == 0 && s_reload.staging == 1, "and the partial slot is next");
    CHECK(send_package(length), "the next package goes over it");
    CHECK(slot_sequence(1u) == 6u, "with the next sequence");
}

static void test_flaky_link(void)
{
    reset();
    install(0, 5u, GOOD);
    boot();
    const uint32_t length = build(GOOD);
    send_begin(length);

    send_chunk(0u, CHUNK_BYTES);
    CHECK(s_last_got == CHUNK_BYTES, "a chunk lands");
    const unsigned erases = s_erases;
    send_chunk(0u, CHUNK_BYTES);
    CHECK(s_last_got == CHUNK_BYTES && s_erases == erases, "a resend of it changes nothing");
    send_chunk(CHUNK_BYTES * 2u, CHUNK_BYTES);
    CHECK(s_last_got == CHUNK_BYTES, "a chunk from ahead asks for the right one");

    /* A corrupt frame is dropped and asks for a resend, and noise in the stream is skipped. */
    uint8_t payload[ERB_PACKAGE_RELOAD_BUFFER_BYTES(CHUNK_BYTES)];
    const uint32_t offset = CHUNK_BYTES;
    memcpy(payload, &offset, sizeof(offset));
    memcpy(payload + sizeof(offset), s_package + offset, CHUNK_BYTES);
    uint8_t out[FRAME_MAX_BYTES];
    const size_t bytes = frame(out, ER_HR_MSG_PACKAGE_CHUNK, payload, sizeof(payload));
    out[bytes - 1u] ^= 0xFFu;
    static const uint8_t noise[] = {'E', 'R', 'x', 0x00, 0xFF};
    erb_package_reload_feed(&s_reload, noise, sizeof(noise));
    erb_package_reload_feed(&s_reload, out, bytes);
    CHECK(s_dropped_lines == 1u && s_last_got == CHUNK_BYTES, "a corrupt chunk is dropped");

    /* The rest arrives a byte at a time, as a slow link hands it over. */
    bool started = false;
    for (uint32_t chunk_start = CHUNK_BYTES; chunk_start < length; chunk_start += CHUNK_BYTES)
    {
        const uint32_t remaining = length - chunk_start;
        const uint32_t take = remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES;
        memcpy(payload, &chunk_start, sizeof(chunk_start));
        memcpy(payload + sizeof(chunk_start), s_package + chunk_start, take);
        const size_t frame_bytes = frame(out, ER_HR_MSG_PACKAGE_CHUNK, payload, take + sizeof(chunk_start));
        for (size_t index = 0; index < frame_bytes; index++)
        {
            started = erb_package_reload_feed(&s_reload, out + index, 1u) || started;
        }
    }
    CHECK(started && erb_running() && slot_sequence(1u) == 6u, "and the package still starts");

    /* The load's answer can pass a resend of the last chunk, which has nothing left to do. */
    const unsigned got = s_got_lines;
    CHECK(!send_chunk(0u, CHUNK_BYTES) && s_got_lines == got, "a chunk with no package coming in is ignored");
}

static void test_bad_chunks(void)
{
    reset();
    install(0, 5u, GOOD);
    boot();
    const uint32_t length = build(GOOD);
    send_begin(length);
    /* Not a whole page, and not the last chunk. */
    send_chunk(0u, ERB_PACKAGE_SLOT_PAGE_BYTES - 1u);
    CHECK(strncmp(s_outcome, "hot reload: ", 12) == 0, "a chunk that can't be programmed ends the transfer");
    CHECK(erb_running() && er_error_screen_active(), "with the app running, and the reason up");
    const unsigned got = s_got_lines;
    send_chunk(0u, CHUNK_BYTES);
    CHECK(s_got_lines == got, "more chunks wait for a new begin");

    send_begin(CHUNK_BYTES);
    uint8_t payload[ERB_PACKAGE_RELOAD_BUFFER_BYTES(CHUNK_BYTES)];
    const uint32_t offset = 0u;
    memcpy(payload, &offset, sizeof(offset));
    memset(payload + sizeof(offset), 0, CHUNK_BYTES);
    s_outcome[0] = '\0';
    uint8_t out[FRAME_MAX_BYTES];
    size_t bytes = frame(out, ER_HR_MSG_PACKAGE_CHUNK, payload, CHUNK_BYTES / 2u + sizeof(offset));
    erb_package_reload_feed(&s_reload, out, bytes);
    CHECK(s_outcome[0] == '\0', "a partial package isn't loaded");

    bytes = frame(out, ER_HR_MSG_LOAD_CONTAINER, payload, 8u);
    erb_package_reload_feed(&s_reload, out, bytes);
    CHECK(strstr(s_outcome, "--aot") != NULL, "a Flow A frame says what to do");
}

static void test_flash_fails(void)
{
    reset();
    install(0, 5u, GOOD);
    boot();
    const uint32_t length = build(GOOD);
    s_flash_fails = true;
    send_begin(length);
    CHECK(strstr(s_outcome, "package slot") != NULL, "flash that won't erase is the reason");
    CHECK(s_ready_lines == 0u, "and nothing comes next");
    CHECK(erb_running() && er_error_screen_active(), "with the app running, and the reason up");

    s_flash_fails = false;
    send_begin(length);
    s_flash_fails = true;
    send_chunk(0u, CHUNK_BYTES);
    CHECK(strstr(s_outcome, "package slot") != NULL, "so is flash that won't program");
}

static void test_header_write_fails(void)
{
    reset();
    install(0, 5u, GOOD);
    boot();
    const uint32_t length = build(GOOD);
    s_header_fails = true;
    CHECK(send_package(length), "the new app starts");
    CHECK(strstr(s_outcome, "couldn't write its header") != NULL, "and the host hears a reset won't run it");
    CHECK(!slot_valid(1u) && slot_valid(0u), "so only the last good package has a header");

    s_header_fails = false;
    s_outcome[0] = '\0';
    send_begin(length);
    CHECK(s_ready_lines == 1u, "the next package is refused");
    CHECK(strstr(s_outcome, "package slot") != NULL, "and the host hears why");
    CHECK(slot_valid(0u), "so a reset still has the last good package");
    CHECK(erb_running(), "and the new app runs on");
    ErbPackageSlotBoot after;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &after) && after.active == 0, "a reset runs it");

    /* The first package has nothing behind it to protect. */
    reset();
    boot();
    s_header_fails = true;
    CHECK(send_package(length), "a first package starts without a header");
    s_header_fails = false;
    CHECK(send_package(length), "and the next one still goes in");
    CHECK(s_reload.active == 1 && slot_valid(1u), "and gets a header");
}

static void test_clear_failed(void)
{
    /* The rejected newest slot still reads as valid, so the running app isn't in the newest slot. */
    reset();
    install(0, 5u, GOOD);
    install(1, 6u, BAD_CRC);
    boot();
    CHECK(erb_running() && s_reload.active == 0 && s_reload.staging == 1, "the last good package runs");
    CHECK(slot_valid(1u), "beside a rejected one no clear took away");
    const uint8_t* running = (const uint8_t*)s_flash[0] + ERB_PACKAGE_SLOT_HEADER_BYTES;
    uint8_t before[64];
    memcpy(before, running, sizeof(before));
    CHECK(send_package(build(GOOD)), "a new package starts");
    CHECK(memcmp(before, running, sizeof(before)) == 0, "written over the rejected one, not the running one");
    CHECK(s_reload.active == 1 && slot_sequence(1u) == 6u, "and newest");
}

int main(void)
{
    EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = null_fill;
    embedded_renderer_set_backend(&backend);

    test_first_package();
    test_swaps_slots();
    test_sequence_wraps();
    test_rejected_keeps_running();
    test_boot_fault();
    test_too_big_for_slot();
    test_fills_the_slot();
    test_missing_slot();
    test_interrupted();
    test_flaky_link();
    test_bad_chunks();
    test_flash_fails();
    test_header_write_fails();
    test_clear_failed();

    if (s_failed)
    {
        return 1;
    }
    printf("All %d Flow B hot reload checks passed\n", s_checked);
    return 0;
}
