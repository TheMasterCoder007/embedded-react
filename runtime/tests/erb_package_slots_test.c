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
 * The package slots: which one is active, what a bad package falls back to, and which one comes next.
 */

#include "er_container.h"
#include "er_error_screen.h"
#include "erb_internal.h"
#include "erb_package.h"
#include "erb_package_slots.h"
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
#define NO_INVALIDATE 0xFFu

/** @brief An order past which a plain comparison of sequences would be wrong. */
#define SEQUENCE_BEFORE_WRAP 0xFFFFFFFFu
#define SEQUENCE_AFTER_WRAP 1u

/** @brief What a slot's copy of the baseline package has wrong with it. */
enum
{
    GOOD,
    BAD_CRC,
    BOOT_TRAP,
    TOO_BIG,
};

static uint32_t s_flash[ERB_PACKAGE_SLOT_COUNT][SLOT_WORDS];
static uint32_t s_arena[1024];
static TPkg s_pkg;
static ErbConfig s_config;
static ErbPackageSlots s_slots;
static ErbPackageSlotBoot s_boot;
static unsigned s_invalidated;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        s_checked++;                                                                                                   \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, (message));                                                         \
            s_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool sensors_read(const int32_t* args, int32_t* result, void* context)
{
    (void)context;
    *result = args[0] + 1;
    return true;
}

static const ErbNative s_natives[] = {{"sensors.read", 1, "i:i", sensors_read}};

static void null_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/** @brief Clears a header as a board does: programming zeros over it, with no erase. */
static void invalidate(unsigned slot, void* context)
{
    (void)context;
    s_invalidated = slot;
    memset(s_flash[slot], 0, ERB_PACKAGE_SLOT_HEADER_BYTES);
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
    s_invalidated = NO_INVALIDATE;
    for (unsigned slot = 0; slot < ERB_PACKAGE_SLOT_COUNT; slot++)
    {
        s_slots.slots[slot].base = s_flash[slot];
        s_slots.slots[slot].bytes = SLOT_BYTES;
    }
    s_slots.invalidate = invalidate;
    s_slots.context = NULL;
}

/** @brief Writes a slot as `embedded-react flash` does. */
static void install(unsigned slot, uint32_t sequence, int kind)
{
    tpkg_base(&s_pkg);
    if (kind == BOOT_TRAP)
    {
        const uint32_t trap[] = {OP_ABX(TRAP, 0, STR_BOOM)};
        const TFunction keep = s_pkg.functions[F_BOOT];
        tpkg_set_code(&s_pkg, F_BOOT, s_pkg.types + keep.types, keep.registers, trap, 1);
    }
    if (kind == TOO_BIG)
    {
        s_pkg.panels[0].arena_bytes = sizeof(s_arena) * 2u;
    }
    uint8_t* image = (uint8_t*)s_flash[slot];
    const uint32_t length = tpkg_build(&s_pkg, image + ERB_PACKAGE_SLOT_HEADER_BYTES);
    if (kind == BAD_CRC)
    {
        image[ERB_PACKAGE_SLOT_HEADER_BYTES + length - 1u] ^= 0xFFu;
    }
    uint32_t header[4] = {ERB_PACKAGE_SLOT_MAGIC, sequence, length, 0u};
    header[3] = er_crc32((const uint8_t*)header, 12u);
    memcpy(image, header, sizeof(header));
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/* The same bytes as flash.unit.test.mjs, so the host's writer and this reader can't drift apart. */
static void test_header_format(void)
{
    reset();
    static const uint8_t golden[16] = {
        0x45, 0x52, 0x42, 0x53, 0x07, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0xB5, 0xD9, 0x79, 0x48};
    memcpy(s_flash[0], golden, sizeof(golden));
    const uint8_t* package = NULL;
    uint32_t length = 0u;
    uint32_t sequence = 0u;
    CHECK(erb_package_slot_read(&s_slots.slots[0], &package, &length, &sequence), "the host's header reads");
    CHECK(sequence == 7u && length == 0x1234u, "with its sequence and length");
    CHECK(package == (const uint8_t*)s_flash[0] + ERB_PACKAGE_SLOT_HEADER_BYTES, "and the package after the header");
}

static void test_bad_headers(void)
{
    reset();
    CHECK(!erb_package_slot_read(&s_slots.slots[0], NULL, NULL, NULL), "erased flash has no header");

    install(0, 1u, GOOD);
    CHECK(erb_package_slot_read(&s_slots.slots[0], NULL, NULL, NULL), "an installed slot has one");
    ((uint8_t*)s_flash[0])[4] ^= 1u;
    CHECK(!erb_package_slot_read(&s_slots.slots[0], NULL, NULL, NULL), "a damaged header doesn't read");

    uint32_t header[4] = {ERB_PACKAGE_SLOT_MAGIC, 1u, SLOT_BYTES, 0u};
    header[3] = er_crc32((const uint8_t*)header, 12u);
    memcpy(s_flash[0], header, sizeof(header));
    CHECK(!erb_package_slot_read(&s_slots.slots[0], NULL, NULL, NULL), "a package past the slot's end doesn't read");

    header[2] = 0u;
    header[3] = er_crc32((const uint8_t*)header, 12u);
    memcpy(s_flash[0], header, sizeof(header));
    CHECK(!erb_package_slot_read(&s_slots.slots[0], NULL, NULL, NULL), "nor does an empty package");

    const ErbPackageSlot unaligned = {(const uint8_t*)s_flash[1] + 1, SLOT_BYTES - 1u};
    CHECK(!erb_package_slot_read(&unaligned, NULL, NULL, NULL), "a slot must be 4-byte aligned");
    const ErbPackageSlot tiny = {s_flash[1], ERB_PACKAGE_SLOT_HEADER_BYTES};
    CHECK(!erb_package_slot_read(&tiny, NULL, NULL, NULL), "and hold more than a header");
    const ErbPackageSlot missing = {NULL, SLOT_BYTES};
    CHECK(!erb_package_slot_read(&missing, NULL, NULL, NULL), "and exist");
}

static void test_no_package(void)
{
    reset();
    CHECK(!erb_package_slots_boot(&s_slots, &s_config, &s_boot), "empty slots don't boot");
    CHECK(s_boot.active == ERB_PACKAGE_SLOT_NONE && s_boot.rejected == ERB_PACKAGE_SLOT_NONE,
          "and nothing was rejected");
    CHECK(s_boot.error.status == ERB_ERR_NO_PACKAGE, "the reason is no package");
    CHECK(s_boot.staging == 0, "and the first package goes in the first slot");
    CHECK(s_invalidated == NO_INVALIDATE, "flash isn't touched");
    erb_package_slots_show_error(&s_boot);
    CHECK(er_error_screen_active(), "the first boot shows the error screen");
}

static void test_newest_wins(void)
{
    reset();
    install(1, 1u, GOOD);
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot), "one package boots, in either slot");
    CHECK(s_boot.active == 1 && s_boot.staging == 0, "and the other slot is staging");
    CHECK(s_boot.error.status == ERB_OK, "with no error");

    reset();
    install(0, 5u, GOOD);
    install(1, 6u, GOOD);
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot) && s_boot.active == 1, "the newest runs");
    CHECK(s_boot.staging == 0, "and the older is written over next");
    CHECK(s_invalidated == NO_INVALIDATE, "and stays in flash for now");

    reset();
    install(0, 6u, GOOD);
    install(1, 5u, GOOD);
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot) && s_boot.active == 0, "in either order");

    reset();
    install(0, SEQUENCE_BEFORE_WRAP, GOOD);
    install(1, SEQUENCE_AFTER_WRAP, GOOD);
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot) && s_boot.active == 1, "and past the sequence wrapping");
}

static void test_rollback(void)
{
    static const struct
    {
        int kind;
        ErbStatus status;
        const char* what;
    } cases[] = {
        {BAD_CRC, ERB_ERR_CORRUPT, "a corrupt package falls back"},
        {TOO_BIG, ERB_ERR_TOO_BIG, "a package too big for the arena falls back"},
        {BOOT_TRAP, ERB_ERR_TRAP, "a package that faults at boot falls back"},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        reset();
        install(0, 5u, GOOD);
        install(1, 6u, cases[index].kind);
        CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot), cases[index].what);
        CHECK(erb_running() && s_boot.active == 0, "and the last good package runs");
        CHECK(s_boot.rejected == 1 && s_boot.error.status == cases[index].status, "with the reason kept");
        CHECK(s_boot.staging == 1, "and the rejected slot is next");
        CHECK(s_invalidated == 1u, "its header is cleared");
        CHECK(!er_error_screen_active(), "the good load took any error screen down");

        /* The next boot sees only the last good package, as the host's next flash does. */
        CHECK(!erb_package_slot_read(&s_slots.slots[1], NULL, NULL, NULL), "so it no longer reads");
        ErbPackageSlotBoot next;
        CHECK(erb_package_slots_boot(&s_slots, &s_config, &next) && next.active == 0,
              "the next boot runs the last good");
        CHECK(next.error.status == ERB_OK && next.rejected == ERB_PACKAGE_SLOT_NONE, "with nothing to report");
    }

    reset();
    install(0, 5u, GOOD);
    install(1, 6u, BAD_CRC);
    erb_package_slots_boot(&s_slots, &s_config, &s_boot);
    erb_package_slots_show_error(&s_boot);
    CHECK(er_error_screen_active() && erb_running(), "the reason can be shown over the running app");
    er_error_screen_hide();
    CHECK(!er_error_screen_active() && erb_running(), "and taken down again");
}

/** @brief A board whose flash won't take the clear. */
static void invalidate_fails(unsigned slot, void* context)
{
    (void)context;
    s_invalidated = slot;
}

static void test_clear_fails(void)
{
    reset();
    install(0, 5u, GOOD);
    install(1, 6u, BAD_CRC);
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot) && s_boot.active == 0, "the last good package runs");
    CHECK(!s_boot.clear_failed, "and a cleared header is no failure");

    reset();
    install(0, 5u, GOOD);
    install(1, 6u, BAD_CRC);
    s_slots.invalidate = invalidate_fails;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot) && s_boot.active == 0,
          "it runs when the clear fails too");
    CHECK(s_invalidated == 1u && s_boot.clear_failed, "and the boot says the header is still there");
    erb_package_slots_show_error(&s_boot);
    CHECK(er_error_screen_active(), "with the error screen up");
    er_error_screen_hide();

    reset();
    install(0, 5u, GOOD);
    install(1, 6u, BAD_CRC);
    s_slots.invalidate = NULL;
    CHECK(erb_package_slots_boot(&s_slots, &s_config, &s_boot) && s_boot.clear_failed, "as when nothing can clear it");
}

static void test_nothing_loads(void)
{
    reset();
    install(0, 5u, BAD_CRC);
    install(1, 6u, TOO_BIG);
    CHECK(!erb_package_slots_boot(&s_slots, &s_config, &s_boot), "neither package loads");
    CHECK(!erb_running() && s_boot.active == ERB_PACKAGE_SLOT_NONE, "so no app runs");
    CHECK(s_boot.rejected == 1 && s_boot.error.status == ERB_ERR_TOO_BIG, "the reason is the newest package's");
    CHECK(s_boot.staging == 0, "and the older is written over next");
    CHECK(s_invalidated == NO_INVALIDATE, "both are kept: a later firmware may run them");
    erb_package_slots_show_error(&s_boot);
    CHECK(er_error_screen_active(), "the error screen is up");

    reset();
    install(0, 6u, BAD_CRC);
    CHECK(!erb_package_slots_boot(&s_slots, &s_config, &s_boot), "a lone bad package doesn't boot");
    CHECK(s_boot.rejected == 0 && s_boot.staging == 1, "the other slot is next");
    CHECK(s_invalidated == NO_INVALIDATE, "and it is kept too");
}

int main(void)
{
    EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = null_fill;
    embedded_renderer_set_backend(&backend);

    test_header_format();
    test_bad_headers();
    test_no_package();
    test_newest_wins();
    test_rollback();
    test_clear_fails();
    test_nothing_loads();

    if (s_failed)
    {
        return 1;
    }
    printf("All %d Flow B package slot checks passed\n", s_checked);
    return 0;
}
