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
 * The headless frame host: the Flow B runtime loading a package, or, given a .js bundle, Flow A's QuickJS runtime
 * running it. It renders through the software backend into a framebuffer in RAM, runs scripted taps, and writes the
 * final frame, so packages/embedded-react/test/erb-frames/erb-frames.mts can compare it with a golden image.
 *
 *   erb-frames <out.raw> <width> <height> <package.erpkg | app.js> [taps [settle_ms]]
 *
 * taps is "x,y x,y ...": each is a press and release, with a 16 ms tick after each half. "x,y>x2,y2" is a drag
 * instead: a press at x,y, DRAG_STEPS moves to x2,y2 with a tick after each, and a release there. The frame is
 * written as width * height ARGB8888 words, little-endian, after the taps and a settle of settle_ms (SETTLE_MS when
 * it is left out), so a scenario can take its frame in the middle of an animation. Both flows get the same probe
 * native module, which the native-calls and native-events scenarios call. Its events are posted at the start of a
 * frame, as a board's frame loop posts them.
 */

#include "er_runtime.h"
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

/** @brief The app is a Flow A bundle rather than a Flow B package. */
static bool s_flow_a;

/*----------------------------------------------------------------------------------------------------------------------
 - The probe module, which the native-calls, native-events and native-strings scenarios call
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The probe's events, by their index in s_natives. */
#define PROBE_DONE 9
#define PROBE_IDLE 10
#define PROBE_NAMED 15

static int32_t s_probe_count;
static float s_probe_stored;
static int32_t s_probe_refused;
static bool s_probe_pending;
static int32_t s_probe_pending_value;
static bool s_probe_announced;
static int32_t s_probe_announced_id;

/** @brief Posts one of the probe's events through the flow that runs the app. */
static bool probe_post(int native, const int32_t* arguments)
{
    return s_flow_a ? er_runtime_native_post(native, arguments) : erb_native_post(native, arguments);
}

/** @brief Posts done with the pending value, its half and whether it is even, once it is taken. */
static void probe_post_done(void)
{
    if (!s_probe_pending)
    {
        return;
    }
    const float half = (float)s_probe_pending_value / 2.0f;
    int32_t values[3] = {s_probe_pending_value, 0, s_probe_pending_value % 2 == 0};
    memcpy(&values[1], &half, sizeof(half));
    if (probe_post(PROBE_DONE, values))
    {
        s_probe_pending = false;
    }
    else
    {
        s_probe_refused++;
    }
}

/** @brief Posts named with the announced id's name and stamp, once it is taken. */
static void probe_post_named(void)
{
    if (!s_probe_announced)
    {
        return;
    }
    static char name[32];
    snprintf(name, sizeof(name), "probe-%d", (int)s_probe_announced_id);
    int32_t values[3];
    values[0] = erb_native_text(name);
    erb_native_set_int64(&values[1], 5000000000LL + s_probe_announced_id);
    if (probe_post(PROBE_NAMED, values))
    {
        s_probe_announced = false;
    }
}

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

/** @brief Posts done on the next frame. The post from inside this call is refused, since the app's code runs. */
static bool probe_start(const int32_t* arguments, int32_t* result, void* context)
{
    (void)result;
    (void)context;
    s_probe_pending = true;
    s_probe_pending_value = arguments[0];
    probe_post_done();
    return true;
}

static bool probe_refused(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = s_probe_refused;
    return true;
}

static bool probe_greet(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    static char greeting[64];
    snprintf(greeting, sizeof(greeting), "hi %s", erb_native_string(arguments[0]));
    *result = erb_native_text(greeting);
    return true;
}

static bool probe_length(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = (int32_t)strlen(erb_native_string(arguments[0]));
    return true;
}

static bool probe_shift(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    erb_native_set_int64(result, erb_native_int64(&arguments[1]) + (int64_t)arguments[0] * 1000);
    return true;
}

/** @brief Posts named on the next frame. */
static bool probe_announce(const int32_t* arguments, int32_t* result, void* context)
{
    (void)result;
    (void)context;
    s_probe_announced = true;
    s_probe_announced_id = arguments[0];
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
    {"probe.start", 1, "v:i", probe_start},
    {"probe.refused", 1, "i:", probe_refused},
    [PROBE_DONE] = {"probe.done", 1, "v:ifb", NULL},
    [PROBE_IDLE] = {"probe.idle", 1, "v:", NULL},
    {"probe.greet", 1, "s:s", probe_greet},
    {"probe.length", 1, "i:s", probe_length},
    {"probe.shift", 1, "l:il", probe_shift},
    {"probe.announce", 1, "v:i", probe_announce},
    [PROBE_NAMED] = {"probe.named", 1, "v:sl", NULL},
};

static void log_line(const char* line, void* context)
{
    (void)context;
    fprintf(stderr, "ERB: %s\n", line);
}

/** @brief Reads a file into a 4-aligned buffer, as a board places a package, with a NUL after it for a bundle. */
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
    uint32_t* words = calloc(((size_t)length + PACKAGE_ALIGNMENT) & ~(size_t)(PACKAGE_ALIGNMENT - 1u), 1);
    if (words && fread(words, 1, (size_t)length, file) != (size_t)length)
    {
        free(words);
        words = NULL;
    }
    fclose(file);
    *package_bytes = (size_t)length;
    return words;
}

/** @brief One frame: the probe's events, the engine's clock and animations, the app's timers, then a commit. */
static void frame(void)
{
    probe_post_done();
    probe_post_named();
    probe_post(PROBE_IDLE, NULL);
    embedded_renderer_tick(FRAME_MS);
    if (s_flow_a)
    {
        er_runtime_pump();
    }
    else
    {
        erb_tick(FRAME_MS);
    }
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

/** @brief Lets the app settle after its taps. */
static void settle(uint32_t settle_ms)
{
    for (uint32_t elapsed_ms = 0; elapsed_ms < settle_ms; elapsed_ms += FRAME_MS)
    {
        frame();
    }
}

/** @brief Loads a Flow B package and runs it. @return 0, or an exit code. */
static int
run_flow_b(const uint32_t* package, size_t package_bytes, int width, int height, const char* taps, uint32_t settle_ms)
{
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
    settle(settle_ms);
    if (!erb_running())
    {
        char message[ERROR_MESSAGE_BYTES];
        erb_error_message(erb_last_error(), message, sizeof(message));
        fprintf(stderr, "the app stopped: %s\n", message);
        return EXIT_APP_FAILED;
    }
    return 0;
}

/** @brief Runs a Flow A bundle. @return 0, or an exit code. */
static int
run_flow_a(const uint32_t* bundle, size_t bundle_bytes, int width, int height, const char* taps, uint32_t settle_ms)
{
    const ErRuntimeConfig config = {
        .screen_width = width,
        .screen_height = height,
        .natives = s_natives,
        .native_count = sizeof(s_natives) / sizeof(s_natives[0]),
    };
    if (!er_runtime_init(&config))
    {
        fprintf(stderr, "can't start QuickJS\n");
        return EXIT_HOST_FAILED;
    }
    er_runtime_set_wall_clock(WALL_CLOCK_MS);
    if (!er_runtime_load_source((const char*)bundle, bundle_bytes, "app.js"))
    {
        fprintf(stderr, "the bundle didn't load: %s\n", er_runtime_last_error());
        return EXIT_APP_FAILED;
    }

    er_commit();
    run_taps(taps);
    settle(settle_ms);
    if (er_runtime_last_error()[0])
    {
        fprintf(stderr, "the app stopped: %s\n", er_runtime_last_error());
        return EXIT_APP_FAILED;
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <out.raw> <width> <height> <package | app.js> [taps [settle_ms]]\n", argv[0]);
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
    const size_t path_length = strlen(argv[4]);
    s_flow_a = path_length > 3 && strcmp(argv[4] + path_length - 3, ".js") == 0;
    const int status = s_flow_a ? run_flow_a(package, package_bytes, width, height, taps, settle_ms)
                                : run_flow_b(package, package_bytes, width, height, taps, settle_ms);
    if (status != 0)
    {
        return status;
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
