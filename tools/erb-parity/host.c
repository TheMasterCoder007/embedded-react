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
 * The headless Flow B parity host. Built twice from this file: around a generated app.gen.c (the C output),
 * and, with ERB_PARITY_VM, around the Flow B runtime loading a package. Both render through the software
 * backend into a framebuffer in RAM, run the same scripted taps, and write the final frame, so
 * packages/embedded-react/test/erb-parity/erb-parity.mjs can compare the two pixel for pixel.
 *
 *   erb-parity-c  <out.raw> <width> <height> [taps [settle_ms]]
 *   erb-parity-vm <out.raw> <width> <height> <package.erpkg> [taps [settle_ms]]
 *
 * taps is "x,y x,y ...": each is a press and release, with a 16 ms tick after each half. The frame is written
 * as width * height ARGB8888 words, little-endian, after the taps and a settle of settle_ms (SETTLE_MS when it is
 * left out), so a scenario can take its frame in the middle of an animation.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "software_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ERB_PARITY_VM
#include "erb_runtime.h"
#else
#include "app.gen.h"
#include "assets.generated.h"
#endif

/** @brief Each tick, in ms, as a 60 Hz display loop runs. */
#define FRAME_MS 16u

/** @brief How long the app runs after the last tap before the frame is taken, unless the scenario says. */
#define SETTLE_MS 500u

/** @brief Date.now() as both builds see it, so a run doesn't depend on the host's clock. */
#define WALL_CLOCK_MS 1767225600000LL

#ifdef ERB_PARITY_VM
/** @brief The firmware's arena. */
static uint32_t s_arena[(64u * 1024u) / 4u];

static void log_line(const char* line, void* context)
{
    (void)context;
    fprintf(stderr, "ERB: %s\n", line);
}

/** @brief Reads a file into a 4-aligned buffer, as a board places a package. */
static uint32_t* read_package(const char* path, size_t* bytes)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint32_t* words = malloc(((size_t)length + 3u) & ~(size_t)3u);
    if (words && fread(words, 1, (size_t)length, file) != (size_t)length)
    {
        free(words);
        words = NULL;
    }
    fclose(file);
    *bytes = (size_t)length;
    return words;
}
#endif

/** @brief One frame: the engine's clock and animations, the app's timers, then a commit. */
static void frame(void)
{
    embedded_renderer_tick(FRAME_MS);
#ifdef ERB_PARITY_VM
    erb_tick(FRAME_MS);
#else
    er_app_tick((int)FRAME_MS);
#endif
    er_commit();
}

/** @brief Runs the taps in @p taps ("x,y x,y ..."). */
static void run_taps(const char* taps)
{
    const char* cursor = taps;
    while (*cursor)
    {
        int x;
        int y;
        int used;
        if (sscanf(cursor, " %d,%d%n", &x, &y, &used) != 2)
        {
            break;
        }
        cursor += used;
        embedded_renderer_touch(0, ER_TOUCH_DOWN, x, y);
        frame();
        embedded_renderer_touch(0, ER_TOUCH_UP, x, y);
        frame();
    }
}

int main(int argc, char** argv)
{
#ifdef ERB_PARITY_VM
    const int fixed_args = 5;
#else
    const int fixed_args = 4;
#endif
    if (argc < fixed_args)
    {
        fprintf(stderr,
                "usage: %s <out.raw> <width> <height>%s [taps [settle_ms]]\n",
                argv[0],
                fixed_args == 5 ? " <package>" : "");
        return 2;
    }
    const char* out_path = argv[1];
    const int width = atoi(argv[2]);
    const int height = atoi(argv[3]);
    const char* taps = argc > fixed_args ? argv[fixed_args] : "";
    const uint32_t settle_ms = argc > fixed_args + 1 ? (uint32_t)atoi(argv[fixed_args + 1]) : SETTLE_MS;
    if (!er_software_backend_init(width, height))
    {
        fprintf(stderr, "can't make a %dx%d framebuffer\n", width, height);
        return 2;
    }

#ifdef ERB_PARITY_VM
    size_t package_bytes = 0;
    uint32_t* package = read_package(argv[4], &package_bytes);
    if (!package)
    {
        fprintf(stderr, "can't read %s\n", argv[4]);
        return 2;
    }
    erb_set_wall_clock(WALL_CLOCK_MS);
    const ErbConfig config = {
        .arena = s_arena,
        .arena_bytes = sizeof(s_arena),
        .screen_w = (uint16_t)width,
        .screen_h = (uint16_t)height,
        .log = log_line,
    };
    ErbError error;
    if (!erb_load(package, package_bytes, &config, &error))
    {
        char message[256];
        erb_error_message(&error, message, sizeof(message));
        fprintf(stderr, "the package didn't load: %s\n", message);
        return 1;
    }
#else
    er_register_assets();
    er_app_set_wall_clock(WALL_CLOCK_MS);
    if (!er_app_build(width, height))
    {
        fprintf(stderr, "app.gen.c was compiled for another panel than %dx%d\n", width, height);
        return 1;
    }
#endif

    er_commit();
    run_taps(taps);
    for (uint32_t elapsed = 0; elapsed < settle_ms; elapsed += FRAME_MS)
    {
        frame();
    }
#ifdef ERB_PARITY_VM
    if (!erb_running())
    {
        char message[256];
        erb_error_message(erb_last_error(), message, sizeof(message));
        fprintf(stderr, "the app stopped: %s\n", message);
        return 1;
    }
#endif

    FILE* out = fopen(out_path, "wb");
    if (!out)
    {
        fprintf(stderr, "can't write %s\n", out_path);
        return 2;
    }
    fwrite(er_software_framebuffer(), sizeof(uint32_t), (size_t)width * (size_t)height, out);
    fclose(out);
    return 0;
}
