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
 * The error screen: it paints over the whole screen without the scene, once per display buffer, in strips
 * on a banded backend, drops input while it is up, and gives the scene back when it is hidden. Also the
 * no-init fault record.
 */

#include "er_error_screen.h"
#include "er_fault_cortex_m.h"
#include "er_scene.h"
#include "native_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 280
#define BAND_ROWS 40
#define SCENE_COLOR 0xFF204060u
#define ERROR_BACKGROUND 0xFF7F1D1Du

/*----------------------------------------------------------------------------------------------------------------------
 - Backend recorder
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    int operation_count;
    int full_screen_error_fills;
    int last_error_fill_width;
    int last_error_fill_height;
    int scene_fills;
    int band_begins;
    int band_flushes;
} Recorded;

static Recorded s_recorded;
static int s_failures;
static int s_touch_starts;
static int s_touch_moves;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s\n", message);                                                                    \
            s_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static void record_fill(uint32_t argb, int x, int y, int width, int height, void* context)
{
    (void)context;
    s_recorded.operation_count++;
    if (argb == ERROR_BACKGROUND && x == 0 && width == SCREEN_WIDTH && height > 0)
    {
        s_recorded.full_screen_error_fills += (y == 0 && height == SCREEN_HEIGHT) ? 1 : 0;
    }
    if (argb == ERROR_BACKGROUND && x == 0 && y == 0)
    {
        s_recorded.last_error_fill_width = width;
        s_recorded.last_error_fill_height = height;
    }
    if (argb == SCENE_COLOR)
    {
        s_recorded.scene_fills++;
    }
}

static void record_copy(const void* source, int stride, int x, int y, int width, int height, void* context)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)context;
    s_recorded.operation_count++;
}

static void
record_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* context)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)context;
    s_recorded.operation_count++;
}

static void record_band_begin(int x, int y, int width, int height, void* context)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)context;
    s_recorded.band_begins++;
}

static void record_band_flush(void* context)
{
    (void)context;
    s_recorded.band_flushes++;
}

static void on_touch_start(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_touch_starts++;
}

static void on_touch_move(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    (void)data;
    (void)user_data;
    s_touch_moves++;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static void use_backend(int band_rows)
{
    static EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = record_fill;
    backend.copy_rect = record_copy;
    backend.blend_rect = record_blend;
    if (band_rows > 0)
    {
        backend.band_height = band_rows;
        backend.band_begin = record_band_begin;
        backend.band_flush = record_band_flush;
    }
    embedded_renderer_set_backend(&backend);
}

/** @brief A full-screen opaque view that counts touches. */
static void build_scene(void)
{
    er_reset();
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps props;
    er_props_default(&props);
    props.width = SCREEN_WIDTH;
    props.height = SCREEN_HEIGHT;
    props.background_color = SCENE_COLOR;
    er_node_set_props(root, &props);
    er_event_set(root, ER_EVENT_TOUCH_START, on_touch_start, NULL);
    er_event_set(root, ER_EVENT_TOUCH_MOVE, on_touch_move, NULL);
    er_tree_set_root(root);
}

static void commit(void)
{
    memset(&s_recorded, 0, sizeof(s_recorded));
    er_commit();
}

static const ERErrorScreen s_screen = {
    .title = "App stopped",
    .reason = "needs more than the firmware reserves",
    .amounts = "arena: needs 41,210 B, firmware reserves 32,768 B",
    .location = "App.jsx:41",
    .detail = "the arena region table is past the arena",
};

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_paints_over_the_scene(void)
{
    use_backend(0);
    build_scene();
    commit();
    CHECK(s_recorded.scene_fills > 0, "the scene paints before the error");

    er_error_screen_show(SCREEN_WIDTH, SCREEN_HEIGHT, &s_screen);
    CHECK(er_error_screen_active(), "the screen is up");
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the error fills the whole screen");
    CHECK(s_recorded.scene_fills == 0, "the scene doesn't paint under it");
    CHECK(s_recorded.operation_count > 1, "the text draws");
    ERRect dirty;
    CHECK(er_get_dirty_rect(&dirty) && dirty.width == SCREEN_WIDTH && dirty.height == SCREEN_HEIGHT,
          "the whole screen is reported");

    commit();
    CHECK(s_recorded.operation_count == 0, "a second commit paints nothing");

    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);
    CHECK(s_touch_starts == 0, "the app gets no touches");

    er_error_screen_hide();
    CHECK(!er_error_screen_active(), "the screen is down");
    commit();
    CHECK(s_recorded.scene_fills > 0 && s_recorded.full_screen_error_fills == 0, "the scene repaints after it");
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_UP, 10, 10);
    CHECK(s_touch_starts == 1, "and gets touches again");
}

static void test_ends_a_gesture_in_progress(void)
{
    use_backend(0);
    build_scene();
    commit();
    embedded_renderer_touch(0, ER_TOUCH_DOWN, 10, 10);
    embedded_renderer_touch(0, ER_TOUCH_MOVE, 30, 30);
    CHECK(er_touch_active_count() == 1, "a finger is down with a move parked");

    er_error_screen_show(SCREEN_WIDTH, SCREEN_HEIGHT, &s_screen);
    CHECK(er_touch_active_count() == 0, "the error screen ends the gesture");
    er_error_screen_hide();
    s_touch_moves = 0;
    commit();
    CHECK(s_touch_moves == 0, "and the parked move never reaches the app");
}

static void test_paints_without_a_scene(void)
{
    use_backend(0);
    er_reset();
    er_error_screen_show(SCREEN_WIDTH, SCREEN_HEIGHT, NULL);
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the error paints with no scene at all");
    er_error_screen_hide();
}

static void test_takes_a_new_size(void)
{
    use_backend(0);
    build_scene();
    er_error_screen_resize(SCREEN_HEIGHT, SCREEN_WIDTH);
    CHECK(!er_error_screen_active(), "a resize with no error up shows none");

    er_error_screen_show(SCREEN_WIDTH, SCREEN_HEIGHT, &s_screen);
    commit();
    er_error_screen_resize(SCREEN_HEIGHT, SCREEN_WIDTH);
    commit();
    CHECK(s_recorded.last_error_fill_width == SCREEN_HEIGHT && s_recorded.last_error_fill_height == SCREEN_WIDTH,
          "a resized error fills its new size");
    CHECK(s_recorded.operation_count > 1, "and draws its text again");
    er_error_screen_hide();
}

static void test_paints_each_display_buffer(void)
{
    use_backend(0);
    build_scene();
    er_set_display_buffer_count(2);
    er_error_screen_show(SCREEN_WIDTH, SCREEN_HEIGHT, &s_screen);
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the first buffer gets the error");
    commit();
    CHECK(s_recorded.operation_count == 0, "it isn't painted twice before a present");
    er_display_present();
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the second buffer gets it after the flip");
    er_display_present();
    commit();
    CHECK(s_recorded.operation_count == 0, "then both buffers hold it");
    er_error_screen_hide();
    er_set_display_buffer_count(1);
}

static void test_paints_in_bands(void)
{
    use_backend(BAND_ROWS);
    build_scene();
    er_error_screen_show(SCREEN_WIDTH, SCREEN_HEIGHT, &s_screen);
    commit();
    const int strips = (SCREEN_HEIGHT + BAND_ROWS - 1) / BAND_ROWS;
    CHECK(s_recorded.band_begins == strips && s_recorded.band_flushes == strips, "one strip per band");
    CHECK(s_recorded.scene_fills == 0, "no strip paints the scene");
    er_error_screen_hide();
}

static void test_fault_record(void)
{
    use_backend(0);
    er_reset();
    ERFaultRecord record;
    memset(&record, 0xA5, sizeof(record));
    CHECK(!er_fault_recorded(&record), "power-on garbage isn't a fault");
    CHECK(!er_fault_show_recorded(&record, SCREEN_WIDTH, SCREEN_HEIGHT), "and shows nothing");
    char text[ER_FAULT_TEXT_BYTES];
    CHECK(er_fault_text(&record, text, sizeof(text)) == 0u && text[0] == '\0', "and has no text");

    er_fault_record(&record, "hard fault in a very long reason that is cut to the record's size", 0x10001234u, 0u);
    CHECK(er_fault_recorded(&record), "a recorded fault is kept");
    CHECK(strlen(record.reason) == ER_FAULT_REASON_BYTES - 1u, "its reason is cut to fit");
    CHECK(record.stack_count == 0u && record.flags == 0u && record.link_register == 0u, "with nothing else set");

    record.program_counter ^= 1u;
    CHECK(!er_fault_recorded(&record), "a changed record fails its check");
    record.program_counter ^= 1u;
    record.stack[ER_FAULT_STACK_WORDS - 1] ^= 1u;
    CHECK(!er_fault_recorded(&record), "in any field");
    record.stack[ER_FAULT_STACK_WORDS - 1] ^= 1u;

    const uint32_t status[] = {0x00008200u, 0x40000000u, 0u, 0x20u, 0xDEADu};
    er_fault_record_registers(&record, ER_FAULT_CORE_CORTEX_M, 0x10000235u, 0x20001000u, status, 5u);
    CHECK(er_fault_recorded(&record), "registers leave the record whole");
    CHECK(record.core == ER_FAULT_CORE_CORTEX_M && record.link_register == 0x10000235u
              && record.stack_pointer == 0x20001000u,
          "and are kept");
    CHECK(record.status[0] == 0x00008200u && record.status[3] == 0x20u, "with the status words that fit");

    uint32_t words[ER_FAULT_STACK_WORDS + 4];
    for (unsigned index = 0u; index < ER_FAULT_STACK_WORDS + 4u; index++)
    {
        words[index] = 0x1000u + index;
    }
    er_fault_record_stack(&record, words, ER_FAULT_STACK_WORDS + 4u, false);
    CHECK(er_fault_recorded(&record) && record.stack_count == ER_FAULT_STACK_WORDS, "stack words are cut to fit");
    CHECK(record.stack[0] == 0x1000u && record.stack[ER_FAULT_STACK_WORDS - 1] == 0x1000u + ER_FAULT_STACK_WORDS - 1u,
          "and kept in order");
    er_fault_record_stack(&record, words, 3u, true);
    CHECK(record.stack_count == 3u && (record.flags & ER_FAULT_FLAG_CALLERS), "callers are flagged");

    const ERFaultApp app = {
        .function = 7u,
        .panel = 1u,
        .offset = 42u,
        .build_id = 0x1A2B3C4Du,
        .file = "a/long/path/that/does/not/fit/in/the/record/App.tsx",
        .line = 41u,
    };
    er_fault_record_app(&record, &app);
    CHECK(er_fault_recorded(&record) && (record.flags & ER_FAULT_FLAG_APP), "the app's place is kept");
    CHECK(record.app_function == 7u && record.app_panel == 1u && record.app_offset == 42u
              && record.app_build_id == 0x1A2B3C4Du && record.app_line == 41u,
          "with its numbers");
    CHECK(strlen(record.app_file) == ER_FAULT_FILE_BYTES - 1u
              && strcmp(record.app_file + ER_FAULT_FILE_BYTES - 1u - strlen("App.tsx"), "App.tsx") == 0,
          "and the end of a long file name");

    /* A null address counts when the core says it is one. */
    ERFaultRecord null_access;
    er_fault_record(&null_access, "hard fault", 0x10001234u, 0u);
    CHECK(!er_fault_has_address(&null_access), "a record with no core has no address at zero");
    const uint32_t bus_fault[] = {0x00008200u, 0u, 0u, 0u};
    er_fault_record_registers(&null_access, ER_FAULT_CORE_CORTEX_M, 0u, 0u, bus_fault, 4u);
    CHECK(er_fault_has_address(&null_access), "a Cortex-M's address-valid bit makes zero an address");
    const uint32_t usage_fault[] = {0x00010000u, 0u, 0u, 0u};
    er_fault_record_registers(&null_access, ER_FAULT_CORE_CORTEX_M, 0u, 0u, usage_fault, 4u);
    CHECK(!er_fault_has_address(&null_access), "without it there is none");
    er_fault_record_registers(&null_access, ER_FAULT_CORE_XTENSA, 0u, 0u, NULL, 0u);
    CHECK(er_fault_has_address(&null_access), "an Xtensa always reports one");

    char small[ER_FAULT_TEXT_BYTES - 1u];
    CHECK(er_fault_text(&record, small, sizeof(small)) == 0u, "text needs room for the whole record");
    CHECK(er_fault_text(&record, text, sizeof(text)) == 2u * sizeof(record), "the text is the record in hex");
    CHECK(strncmp(text, "5446524501", 10) == 0, "from its magic and version");

    CHECK(er_fault_show_recorded(&record, SCREEN_WIDTH, SCREEN_HEIGHT), "a recorded fault shows");
    CHECK(er_error_screen_active(), "on the error screen");
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "which paints");
    CHECK(!er_fault_recorded(&record), "and the record is cleared");
    CHECK(!er_fault_show_recorded(&record, SCREEN_WIDTH, SCREEN_HEIGHT), "so it shows once");
    er_error_screen_hide();
}

/* A stack as a fault leaves it: the stacked registers, then the words the handler records. */
#define FAULT_STACK_WORDS 64u

static void test_cortex_m_fault(void)
{
    uint32_t stack[FAULT_STACK_WORDS];
    for (unsigned index = 0u; index < FAULT_STACK_WORDS; index++)
    {
        stack[index] = 0xC0DE0000u + index;
    }
    ERFaultRecord record;
    uint32_t other_stack[16] = {0};
    const ERCortexMFaultStack stacks[] = {
        {(uintptr_t)other_stack, (uintptr_t)(other_stack + 16)},
        {(uintptr_t)stack, (uintptr_t)(stack + FAULT_STACK_WORDS)},
    };
    const ERCortexMFaultConfig config = {
        .record = &record,
        .stacks = stacks,
        .stack_count = 2u,
    };

    /* A basic frame 12 words below the top: lr, pc and xpsr in its words 5 to 7. */
    uint32_t* frame = stack + FAULT_STACK_WORDS - 20u;
    frame[5] = 0x08001235u;
    frame[6] = 0x08004320u;
    frame[7] = 0x01000000u;
    const uint32_t status[] = {0x00008200u, 0x40000000u, 0x11u, 0x24u};
    er_cortex_m_fault_fill(&config, frame, 0xFFFFFFF9u, status);
    CHECK(er_fault_recorded(&record), "a fault is recorded");
    CHECK(record.program_counter == 0x08004320u && record.link_register == 0x08001235u, "with its pc and lr");
    CHECK(record.address == 0x24u && strcmp(record.reason, "hard fault: bus error") == 0,
          "a bus fault's address and reason");
    CHECK(record.status[0] == status[0] && record.status[1] == status[1], "and the status registers");
    CHECK(record.stack_pointer == (uint32_t)(uintptr_t)(frame + 8), "the stack pointer is past the frame");
    CHECK(record.stack_count == 12u && record.stack[0] == frame[8] && record.stack[11] == stack[FAULT_STACK_WORDS - 1u],
          "the stack words stop at the end of the stack");

    /* An aligned frame with float registers: 18 more words, and one of padding. */
    frame = stack + 4u;
    frame[6] = 0x08004320u;
    frame[7] = 0x01000200u;
    er_cortex_m_fault_fill(&config, frame, 0xFFFFFFE9u, NULL);
    CHECK(record.stack_pointer == (uint32_t)(uintptr_t)(frame + 27), "float registers and padding are stepped over");
    CHECK(record.stack_count == ER_FAULT_STACK_WORDS && record.stack[0] == frame[27], "a deep stack fills the record");
    CHECK(strcmp(record.reason, "hard fault") == 0 && record.status[0] == 0u, "a core without status has none");

    /* A stack pointer outside the stack is never read. */
    er_cortex_m_fault_fill(&config, stack + FAULT_STACK_WORDS - 4u, 0xFFFFFFF9u, NULL);
    CHECK(er_fault_recorded(&record) && record.program_counter == 0u && record.stack_count == 0u,
          "a frame that runs off the stack is not read");
    CHECK(strcmp(record.reason, "hard fault: bad stack pointer") == 0, "and says why");
    er_cortex_m_fault_fill(&config, (const uint32_t*)((uintptr_t)stack + 2u), 0xFFFFFFF9u, NULL);
    CHECK(record.program_counter == 0u, "nor is a misaligned one");

    /* A frame on another stack is read to that stack's end. */
    other_stack[6] = 0x08005550u;
    other_stack[7] = 0x01000000u;
    other_stack[8] = 0xAAAA0001u;
    er_cortex_m_fault_fill(&config, other_stack, 0xFFFFFFF9u, NULL);
    CHECK(record.program_counter == 0x08005550u && record.stack_count == 8u && record.stack[0] == 0xAAAA0001u,
          "each stack is read to its own end");

    /* A frame at the very top leaves no words to record. */
    frame = stack + FAULT_STACK_WORDS - 8u;
    frame[7] = 0x01000000u;
    er_cortex_m_fault_fill(&config, frame, 0xFFFFFFF9u, NULL);
    CHECK(er_fault_recorded(&record) && record.program_counter == frame[6] && record.stack_count == 0u,
          "a frame at the top of the stack has no words after it");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    test_paints_over_the_scene();
    test_ends_a_gesture_in_progress();
    test_paints_without_a_scene();
    test_takes_a_new_size();
    test_paints_each_display_buffer();
    test_paints_in_bands();
    test_fault_record();
    test_cortex_m_fault();
    if (s_failures)
    {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    printf("error screen: all checks passed\n");
    return EXIT_SUCCESS;
}
