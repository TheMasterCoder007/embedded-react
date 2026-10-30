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
#include "er_scene.h"
#include "native_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SCREEN_W 240
#define SCREEN_H 280
#define BAND_ROWS 40
#define SCENE_COLOR 0xFF204060u
#define ERROR_BACKGROUND 0xFF7F1D1Du

/*----------------------------------------------------------------------------------------------------------------------
 - Backend recorder
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    int ops;
    int full_screen_error_fills;
    int scene_fills;
    int band_begins;
    int band_flushes;
} Recorded;

static Recorded s_recorded;
static int s_failures;
static int s_touch_starts;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s\n", message);                                                                    \
            s_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static void record_fill(uint32_t argb, int x, int y, int w, int h, void* context)
{
    (void)context;
    s_recorded.ops++;
    if (argb == ERROR_BACKGROUND && x == 0 && w == SCREEN_W && h > 0)
    {
        s_recorded.full_screen_error_fills += (y == 0 && h == SCREEN_H) ? 1 : 0;
    }
    if (argb == SCENE_COLOR)
    {
        s_recorded.scene_fills++;
    }
}

static void record_copy(const void* source, int stride, int x, int y, int w, int h, void* context)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)context;
    s_recorded.ops++;
}

static void record_blend(const void* source, int stride, uint8_t alpha, int x, int y, int w, int h, void* context)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)context;
    s_recorded.ops++;
}

static void record_band_begin(int x, int y, int w, int h, void* context)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
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
    props.width = SCREEN_W;
    props.height = SCREEN_H;
    props.background_color = SCENE_COLOR;
    er_node_set_props(root, &props);
    er_event_set(root, ER_EVENT_TOUCH_START, on_touch_start, NULL);
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

    er_error_screen_show(SCREEN_W, SCREEN_H, &s_screen);
    CHECK(er_error_screen_active(), "the screen is up");
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the error fills the whole screen");
    CHECK(s_recorded.scene_fills == 0, "the scene doesn't paint under it");
    CHECK(s_recorded.ops > 1, "the text draws");
    ERRect dirty;
    CHECK(er_get_dirty_rect(&dirty) && dirty.w == SCREEN_W && dirty.h == SCREEN_H, "the whole screen is reported");

    commit();
    CHECK(s_recorded.ops == 0, "a second commit paints nothing");

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

static void test_paints_without_a_scene(void)
{
    use_backend(0);
    er_reset();
    er_error_screen_show(SCREEN_W, SCREEN_H, NULL);
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the error paints with no scene at all");
    er_error_screen_hide();
}

static void test_paints_each_display_buffer(void)
{
    use_backend(0);
    build_scene();
    er_set_display_buffer_count(2);
    er_error_screen_show(SCREEN_W, SCREEN_H, &s_screen);
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the first buffer gets the error");
    commit();
    CHECK(s_recorded.ops == 0, "it isn't painted twice before a present");
    er_display_present();
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "the second buffer gets it after the flip");
    er_display_present();
    commit();
    CHECK(s_recorded.ops == 0, "then both buffers hold it");
    er_error_screen_hide();
    er_set_display_buffer_count(1);
}

static void test_paints_in_bands(void)
{
    use_backend(BAND_ROWS);
    build_scene();
    er_error_screen_show(SCREEN_W, SCREEN_H, &s_screen);
    commit();
    const int strips = (SCREEN_H + BAND_ROWS - 1) / BAND_ROWS;
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
    CHECK(!er_fault_show_recorded(&record, SCREEN_W, SCREEN_H), "and shows nothing");

    er_fault_record(&record, "hard fault in a very long reason that is cut to the record's size", 0x10001234u, 0u);
    CHECK(er_fault_recorded(&record), "a recorded fault is kept");
    CHECK(strlen(record.reason) == ER_FAULT_REASON_BYTES - 1u, "its reason is cut to fit");

    record.pc ^= 1u;
    CHECK(!er_fault_recorded(&record), "a changed record fails its check");
    record.pc ^= 1u;

    CHECK(er_fault_show_recorded(&record, SCREEN_W, SCREEN_H), "a recorded fault shows");
    CHECK(er_error_screen_active(), "on the error screen");
    commit();
    CHECK(s_recorded.full_screen_error_fills == 1, "which paints");
    CHECK(!er_fault_recorded(&record), "and the record is cleared");
    CHECK(!er_fault_show_recorded(&record, SCREEN_W, SCREEN_H), "so it shows once");
    er_error_screen_hide();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    test_paints_over_the_scene();
    test_paints_without_a_scene();
    test_paints_each_display_buffer();
    test_paints_in_bands();
    test_fault_record();
    if (s_failures)
    {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return EXIT_FAILURE;
    }
    printf("error screen: all checks passed\n");
    return EXIT_SUCCESS;
}
