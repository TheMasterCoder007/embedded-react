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
 * The headless Flow B frame host: the Flow B runtime loading a package. It renders through the software backend
 * into a framebuffer in RAM, runs scripted taps, and writes the final frame, so
 * packages/embedded-react/test/erb-frames/erb-frames.mts can compare it with a golden image.
 *
 *   erb-frames <out.raw> <width> <height> <package.erpkg> [taps [settle_ms]]
 *
 * taps is "x,y x,y ...": each is a press and release, with a 16 ms tick after each half. "x,y>x2,y2" is a drag
 * instead: a press at x,y, DRAG_STEPS moves to x2,y2 with a tick after each, and a release there. The frame is
 * written as width * height ARGB8888 words, little-endian, after the taps and a settle of settle_ms (SETTLE_MS when
 * it is left out), so a scenario can take its frame in the middle of an animation. It registers a probe native
 * module, which the native-calls scenarios call.
 */

#include "er_scene.h"
#include "erb_runtime.h"
#include "native_renderer.h"
#include "software_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Each tick, in ms, as a 60 Hz display loop runs. */
#define FRAME_MS 16u

/** @brief The moves a drag takes from its press to its release. */
#define DRAG_STEPS 8

/** @brief How long the app runs after the last tap before the frame is taken, unless the scenario says. */
#define SETTLE_MS 500u

/** @brief Date.now() as the app sees it, so a run doesn't depend on the host's clock. */
#define WALL_CLOCK_MS 1767225600000LL

/** @brief The finger every tap and drag uses. */
#define TOUCH_FINGER 0

/** @brief Room for an error's message. */
#define ERROR_MESSAGE_BYTES 256

/** @brief A package's alignment in memory, as a board places it. */
#define PACKAGE_ALIGNMENT 4u

/** @brief Exit codes: the app failed, or the host couldn't run it. */
#define EXIT_APP_FAILED 1
#define EXIT_HOST_FAILED 2

/** @brief The firmware's arena. */
#define ARENA_BYTES (64u * 1024u)
static uint32_t s_arena[ARENA_BYTES / sizeof(uint32_t)];

/*----------------------------------------------------------------------------------------------------------------------
 - The probe module, which the native-calls scenarios call
 ---------------------------------------------------------------------------------------------------------------------*/

static int32_t s_probe_count;
static float s_probe_stored;

static float probe_float(int32_t cell)
{
    float value;
    memcpy(&value, &cell, sizeof(value));
    return value;
}

static int32_t probe_cell(float value)
{
    int32_t cell;
    memcpy(&cell, &value, sizeof(cell));
    return cell;
}

static bool probe_add(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = arguments[0] + arguments[1];
    return true;
}

static bool probe_half(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = probe_cell((float)arguments[0] / 2.0f);
    return true;
}

static bool probe_even(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = arguments[0] % 2 == 0;
    return true;
}

static bool probe_flip(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = !arguments[0];
    return true;
}

static bool probe_next(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = ++s_probe_count;
    return true;
}

static bool probe_store(const int32_t* arguments, int32_t* result, void* context)
{
    (void)result;
    (void)context;
    s_probe_stored = probe_float(arguments[0]);
    return true;
}

static bool probe_stored(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = probe_cell(s_probe_stored);
    return true;
}

static const ErbNative s_natives[] = {
    {"probe.add", 1, "i:ii", probe_add},
    {"probe.half", 1, "f:i", probe_half},
    {"probe.even", 1, "b:i", probe_even},
    {"probe.flip", 1, "b:b", probe_flip},
    {"probe.next", 1, "i:", probe_next},
    {"probe.store", 1, "v:f", probe_store},
    {"probe.stored", 1, "f:", probe_stored},
};

static void log_line(const char* line, void* context)
{
    (void)context;
    fprintf(stderr, "ERB: %s\n", line);
}

/** @brief Reads a file into a 4-aligned buffer, as a board places a package. */
static uint32_t* read_package(const char* path, size_t* package_bytes)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint32_t* words = malloc(((size_t)length + (PACKAGE_ALIGNMENT - 1u)) & ~(size_t)(PACKAGE_ALIGNMENT - 1u));
    if (words && fread(words, 1, (size_t)length, file) != (size_t)length)
    {
        free(words);
        words = NULL;
    }
    fclose(file);
    *package_bytes = (size_t)length;
    return words;
}

/** @brief One frame: the engine's clock and animations, the app's timers, then a commit. */
static void frame(void)
{
    embedded_renderer_tick(FRAME_MS);
    erb_tick(FRAME_MS);
    er_commit();
}

/** @brief Runs the taps and drags in @p taps ("x,y x,y>x2,y2 ..."). */
static void run_taps(const char* taps)
{
    const char* cursor = taps;
    while (*cursor)
    {
        int x;
        int y;
        int characters_read;
        if (sscanf(cursor, " %d,%d%n", &x, &y, &characters_read) != 2)
        {
            break;
        }
        cursor += characters_read;
        int end_x = x;
        int end_y = y;
        const bool drag = sscanf(cursor, ">%d,%d%n", &end_x, &end_y, &characters_read) == 2;
        if (drag)
        {
            cursor += characters_read;
        }
        embedded_renderer_touch(TOUCH_FINGER, ER_TOUCH_DOWN, x, y);
        frame();
        for (int step = 1; drag && step <= DRAG_STEPS; step++)
        {
            embedded_renderer_touch(
                TOUCH_FINGER, ER_TOUCH_MOVE, x + (end_x - x) * step / DRAG_STEPS, y + (end_y - y) * step / DRAG_STEPS);
            frame();
        }
        embedded_renderer_touch(TOUCH_FINGER, ER_TOUCH_UP, end_x, end_y);
        frame();
    }
}

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <out.raw> <width> <height> <package> [taps [settle_ms]]\n", argv[0]);
        return EXIT_HOST_FAILED;
    }
    const char* frame_path = argv[1];
    const int width = atoi(argv[2]);
    const int height = atoi(argv[3]);
    const char* taps = argc > 5 ? argv[5] : "";
    const uint32_t settle_ms = argc > 6 ? (uint32_t)atoi(argv[6]) : SETTLE_MS;
    if (!er_software_backend_init(width, height))
    {
        fprintf(stderr, "can't make a %dx%d framebuffer\n", width, height);
        return EXIT_HOST_FAILED;
    }

    size_t package_bytes = 0;
    uint32_t* package = read_package(argv[4], &package_bytes);
    if (!package)
    {
        fprintf(stderr, "can't read %s\n", argv[4]);
        return EXIT_HOST_FAILED;
    }
    erb_set_wall_clock(WALL_CLOCK_MS);
    const ErbConfig config = {
        .arena = s_arena,
        .arena_bytes = sizeof(s_arena),
        .screen_width = (uint16_t)width,
        .screen_height = (uint16_t)height,
        .natives = s_natives,
        .native_count = sizeof(s_natives) / sizeof(s_natives[0]),
        .log = log_line,
    };
    ErbError error;
    if (!erb_load(package, package_bytes, &config, &error))
    {
        char message[ERROR_MESSAGE_BYTES];
        erb_error_message(&error, message, sizeof(message));
        fprintf(stderr, "the package didn't load: %s\n", message);
        return EXIT_APP_FAILED;
    }

    er_commit();
    run_taps(taps);
    for (uint32_t elapsed_ms = 0; elapsed_ms < settle_ms; elapsed_ms += FRAME_MS)
    {
        frame();
    }
    if (!erb_running())
    {
        char message[ERROR_MESSAGE_BYTES];
        erb_error_message(erb_last_error(), message, sizeof(message));
        fprintf(stderr, "the app stopped: %s\n", message);
        return EXIT_APP_FAILED;
    }

    FILE* frame_file = fopen(frame_path, "wb");
    if (!frame_file)
    {
        fprintf(stderr, "can't write %s\n", frame_path);
        return EXIT_HOST_FAILED;
    }
    fwrite(er_software_framebuffer(), sizeof(uint32_t), (size_t)width * (size_t)height, frame_file);
    fclose(frame_file);
    return 0;
}
