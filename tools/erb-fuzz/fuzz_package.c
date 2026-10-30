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
 * The fuzz target: one input is a Flow B package, optionally followed by a script of what the host does to it.
 *
 * The container's CRC is written over the mutated bytes first, or nearly every mutation would stop there and
 * never reach the verifier. The package, the arena and the framebuffer are each allocated at exactly their
 * size, so the sanitizers see an access one byte past any of them. A package that loads runs on the software
 * backend: frames, touches, keys, inputs and a reload, then the error screen when it fails or faults.
 *
 * With ERB_FUZZ_REQUIRE_LOAD set in the environment, a package that doesn't load aborts. CTest runs the seeds
 * this way, so a format change can't quietly leave the fuzzer mutating packages the verifier rejects at once.
 */

#include "er_container.h"
#include "er_error_screen.h"
#include "er_scene.h"
#include "erb_internal.h"
#include "native_renderer.h"
#include "software_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief The container's CRC field, and where the bytes it covers start. */
#define CRC_AT 8u
#define CRC_COVERS_FROM 12u

/** @brief The largest panel and arena a run gets. A package that asks for more is refused as too big. */
#define MAX_PANEL_SIDE 480u
#define MAX_ARENA_BYTES (256u * 1024u)

/** @brief The size used when a package has no panel this harness will make. */
#define FALLBACK_W 240u
#define FALLBACK_H 320u

/** @brief Panels of one package a run with no script loads. */
#define MAX_PANELS_RUN 3u

/** @brief Script steps one input may take: each is an operation and two arguments. */
#define STEP_BYTES 3u
#define MAX_STEPS 64u

/** @brief Each tick, in ms, as a 60 Hz display loop runs. */
#define FRAME_MS 16u

/** @brief What a script step does. An operation past the last wraps around. */
enum
{
    OP_FRAME,        /* a frame of (a * 4) ms */
    OP_LONG_FRAME,   /* a frame of (a * 1000) ms */
    OP_TOUCH_DOWN,   /* at (a, b), as a share of the panel, then a frame */
    OP_TOUCH_MOVE,   /* the same */
    OP_TOUCH_UP,     /* the same */
    OP_TOUCH_CANCEL, /* the same */
    OP_KEY,          /* keycode a, or the character b when a is 0 */
    OP_INPUT_INT,    /* input a set to b - 128 */
    OP_INPUT_BOOL,   /* input a set to b & 1 */
    OP_INPUT_FLOAT,  /* input a set to b - 128, as a float */
    OP_INPUT_STRING, /* input a set to b copies of 'x' */
    OP_WALL_CLOCK,   /* the clock set to (a * 256 + b) seconds */
    OP_RELOAD,       /* the package loaded again over itself */
    OP_UNLOAD,       /* the app unloaded and loaded again */
    OP_COUNT_,
};

/** @brief The host's side of a run with no script: taps across the panel, a drag, typing, inputs and time. */
static const uint8_t s_default_script[] = {
    OP_FRAME,      4,   0,   OP_TOUCH_DOWN,  32,  32,  OP_TOUCH_UP,     32,  32,  OP_TOUCH_DOWN, 128, 32,
    OP_TOUCH_UP,   128, 32,  OP_TOUCH_DOWN,  224, 32,  OP_TOUCH_UP,     224, 32,  OP_TOUCH_DOWN, 32,  128,
    OP_TOUCH_UP,   32,  128, OP_TOUCH_DOWN,  128, 128, OP_TOUCH_UP,     128, 128, OP_TOUCH_DOWN, 224, 128,
    OP_TOUCH_UP,   224, 128, OP_TOUCH_DOWN,  32,  224, OP_TOUCH_UP,     32,  224, OP_TOUCH_DOWN, 128, 224,
    OP_TOUCH_UP,   128, 224, OP_TOUCH_DOWN,  224, 224, OP_TOUCH_MOVE,   128, 224, OP_TOUCH_MOVE, 32,  128,
    OP_TOUCH_UP,   32,  128, OP_KEY,         0,   'a', OP_KEY,          8,   0,   OP_INPUT_INT,  0,   200,
    OP_INPUT_BOOL, 0,   1,   OP_INPUT_FLOAT, 0,   64,  OP_INPUT_STRING, 0,   8,   OP_WALL_CLOCK, 99,  1,
    OP_LONG_FRAME, 1,   0,   OP_FRAME,       4,   0,
};

/** @brief A native function, so a package's import of it resolves. The runtime's tests build packages that use it. */
static bool sensors_read(const int32_t* args, int32_t* result, void* context)
{
    (void)context;
    *result = args[0] == INT32_MAX ? 0 : args[0] + 1;
    return args[0] != INT32_MIN;
}

static const ErbNative s_natives[] = {{"sensors.read", 1, "i:i", sensors_read}};

static BitmapFont s_fonts[8];
static GlyphInfo s_glyphs[1024];
static ExtraGlyph s_extras[64];

/** @brief Reads a log line through, so one that runs off its string shows. */
static void log_line(const char* line, void* context)
{
    (void)context;
    volatile size_t length = strlen(line);
    (void)length;
}

/** @brief One frame, as a firmware's display loop runs it. */
static void frame(uint32_t elapsed_ms)
{
    embedded_renderer_tick(elapsed_ms);
    erb_tick(elapsed_ms);
    er_commit();
}

/** @brief What a run needs from step to step. */
typedef struct
{
    const uint8_t* package;
    size_t package_bytes;
    ErbConfig config;
    ErAssetsStorage fonts;
    ErbError error;
} Run;

/** @brief Loads the package, reads the failure's message and shows its screen, as a firmware would. */
static bool load(Run* run)
{
    run->fonts.fonts_used = 0u;
    run->fonts.glyphs_used = 0u;
    run->fonts.extras_used = 0u;
    if (erb_load(run->package, run->package_bytes, &run->config, &run->error))
    {
        return true;
    }
    char message[256];
    char short_message[8];
    erb_error_message(&run->error, message, sizeof(message));
    erb_error_message(&run->error, short_message, sizeof(short_message));
    erb_status_str(run->error.status);
    log_line(message, NULL);
    erb_show_error(&run->error);
    er_commit();
    return false;
}

/** @brief Runs one step of a script. */
static void step(Run* run, uint8_t op, uint8_t a, uint8_t b)
{
    const int x = (int)((uint32_t)a * run->config.screen_w / 256u);
    const int y = (int)((uint32_t)b * run->config.screen_h / 256u);
    switch (op % OP_COUNT_)
    {
        case OP_FRAME:
            frame((uint32_t)a * 4u);
            break;
        case OP_LONG_FRAME:
            frame((uint32_t)a * 1000u);
            break;
        case OP_TOUCH_DOWN:
            embedded_renderer_touch(0, ER_TOUCH_DOWN, x, y);
            frame(FRAME_MS);
            break;
        case OP_TOUCH_MOVE:
            embedded_renderer_touch(0, ER_TOUCH_MOVE, x, y);
            frame(FRAME_MS);
            break;
        case OP_TOUCH_UP:
            embedded_renderer_touch(0, ER_TOUCH_UP, x, y);
            frame(FRAME_MS);
            break;
        case OP_TOUCH_CANCEL:
            embedded_renderer_touch(0, ER_TOUCH_CANCEL, x, y);
            frame(FRAME_MS);
            break;
        case OP_KEY:
        {
            const char text[2] = {(char)(b & 0x7Fu), '\0'};
            embedded_renderer_key(a, a ? NULL : text);
            break;
        }
        case OP_INPUT_INT:
            erb_input_set_int(a, (int32_t)b - 128);
            break;
        case OP_INPUT_BOOL:
            erb_input_set_bool(a, (b & 1u) != 0u);
            break;
        case OP_INPUT_FLOAT:
            erb_input_set_float(a, (float)b - 128.0f);
            break;
        case OP_INPUT_STRING:
        {
            char text[256];
            memset(text, 'x', b);
            text[b] = '\0';
            erb_input_set_string(a, text);
            break;
        }
        case OP_WALL_CLOCK:
            erb_set_wall_clock((int64_t)((uint32_t)a * 256u + b) * 1000);
            break;
        case OP_RELOAD:
            load(run);
            break;
        case OP_UNLOAD:
            erb_unload();
            load(run);
            break;
        default:
            break;
    }
}

/**
 * @brief Finds a panel a package declares, for its size and its arena. Bounds-checked: the package isn't verified.
 *
 * @return How many panels it declares, or 0 when the header can't be read.
 */
static uint32_t read_panels(const ErContainer* container, uint32_t index, uint16_t* w, uint16_t* h, uint32_t* arena)
{
    for (uint32_t section_index = 0; section_index < container->section_count; section_index++)
    {
        const ErContainerSection* section = &container->sections[section_index];
        if (section->type != ERB_SECTION_PROGRAM || section->length < ERB_HEADER_SIZE)
        {
            continue;
        }
        const uint32_t count = erb_u16(section->data + ERB_HEADER_PANEL_COUNT_AT);
        if (count == 0u || section->length - ERB_HEADER_SIZE < count * ERB_PANEL_SIZE)
        {
            return 0u;
        }
        const uint8_t* panel = section->data + ERB_HEADER_SIZE + (index % count) * ERB_PANEL_SIZE;
        *w = erb_u16(panel + ERB_PANEL_WIDTH_AT);
        *h = erb_u16(panel + ERB_PANEL_HEIGHT_AT);
        *arena = erb_u32(panel + ERB_PANEL_ARENA_BYTES_AT);
        return count;
    }
    return 0u;
}

/** @brief Loads one panel of the package, runs the script when it loads, and tears everything down. */
static void run_panel(const uint8_t* package,
                      size_t package_bytes,
                      const ErContainer* container,
                      bool scanned,
                      uint32_t panel,
                      const uint8_t* script,
                      size_t script_bytes)
{
    uint16_t w = FALLBACK_W;
    uint16_t h = FALLBACK_H;
    uint32_t arena_bytes = 0u;
    if (scanned)
    {
        read_panels(container, panel, &w, &h, &arena_bytes);
    }
    if (w == 0u || h == 0u || w > MAX_PANEL_SIDE || h > MAX_PANEL_SIDE)
    {
        w = FALLBACK_W;
        h = FALLBACK_H;
    }
    if (arena_bytes > MAX_ARENA_BYTES)
    {
        arena_bytes = MAX_ARENA_BYTES;
    }
    void* arena = malloc(arena_bytes ? arena_bytes : 1u);
    if (!arena || !er_software_backend_init(w, h))
    {
        abort();
    }

    Run run;
    memset(&run, 0, sizeof(run));
    run.package = package;
    run.package_bytes = package_bytes;
    run.fonts.fonts = s_fonts;
    run.fonts.font_count = sizeof(s_fonts) / sizeof(s_fonts[0]);
    run.fonts.glyphs = s_glyphs;
    run.fonts.glyph_count = sizeof(s_glyphs) / sizeof(s_glyphs[0]);
    run.fonts.extras = s_extras;
    run.fonts.extra_count = sizeof(s_extras) / sizeof(s_extras[0]);
    run.config.arena = arena;
    run.config.arena_bytes = arena_bytes;
    run.config.screen_w = w;
    run.config.screen_h = h;
    run.config.natives = s_natives;
    run.config.native_count = sizeof(s_natives) / sizeof(s_natives[0]);
    run.config.fonts = &run.fonts;
    run.config.log = log_line;

    erb_set_wall_clock(1767225600000LL);
    const bool loaded = load(&run);
    if (!loaded && getenv("ERB_FUZZ_REQUIRE_LOAD"))
    {
        char message[256];
        erb_error_message(&run.error, message, sizeof(message));
        fprintf(stderr, "a seed didn't load (%ux%u): %s\n", w, h, message);
        abort();
    }
    if (loaded)
    {
        erb_input_find("fuzz");
        er_commit();
        for (size_t offset = 0; offset + STEP_BYTES <= script_bytes; offset += STEP_BYTES)
        {
            step(&run, script[offset], script[offset + 1u], script[offset + 2u]);
        }
    }

    /* Everything back as it was, so one input can't change how the next runs. */
    erb_unload();
    er_error_screen_hide();
    er_reset();
    er_keyboard_set_config(NULL);
    er_image_clear();
    er_font_clear();
    er_software_backend_destroy();
    free(arena);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    ErContainer scan;
    const bool scanned = er_container_scan(data, size, &scan) == ER_CONTAINER_OK;
    const size_t package_bytes = scanned ? scan.size : size;

    uint8_t* package = malloc(package_bytes ? package_bytes : 1u);
    if (!package)
    {
        abort();
    }
    memcpy(package, data, package_bytes);

    ErContainer container;
    memset(&container, 0, sizeof(container));
    if (scanned)
    {
        const uint32_t crc = er_crc32(package + CRC_COVERS_FROM, package_bytes - CRC_COVERS_FROM);
        package[CRC_AT] = (uint8_t)crc;
        package[CRC_AT + 1u] = (uint8_t)(crc >> 8);
        package[CRC_AT + 2u] = (uint8_t)(crc >> 16);
        package[CRC_AT + 3u] = (uint8_t)(crc >> 24);
        er_container_scan(package, package_bytes, &container);
    }

    const uint8_t* script = data + package_bytes;
    size_t script_bytes = size - package_bytes;
    if (script_bytes > MAX_STEPS * STEP_BYTES)
    {
        script_bytes = MAX_STEPS * STEP_BYTES;
    }
    if (script_bytes >= STEP_BYTES)
    {
        /* A script's first byte picks the panel. The rest is its steps. */
        run_panel(package, package_bytes, &container, scanned, script[0], script + 1u, script_bytes - 1u);
    }
    else
    {
        uint16_t w;
        uint16_t h;
        uint32_t arena;
        const uint32_t declared = scanned ? read_panels(&container, 0u, &w, &h, &arena) : 0u;
        const uint32_t panels = declared > MAX_PANELS_RUN ? MAX_PANELS_RUN : declared;
        for (uint32_t panel = 0; panel < (panels ? panels : 1u); panel++)
        {
            run_panel(package, package_bytes, &container, scanned, panel, s_default_script, sizeof(s_default_script));
        }
    }

    free(package);
    return 0;
}
