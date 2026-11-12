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

/** @brief The largest arena a run gets. A package that asks for more is refused as too big. */
#define MAX_ARENA_BYTES (256u * 1024u)

/** @brief The panels a run loads a package on: its layout checks pick from the size. */
static const uint16_t s_panels[][2] = {
    {240u, 320u}, {320u, 240u}, {800u, 480u}, {240u, 280u}, {128u, 160u}, {480u, 800u}};
#define PANEL_COUNT (sizeof(s_panels) / sizeof(s_panels[0]))

/** @brief Panels a run with no script loads, which reach each layout of the demos. */
#define PANELS_RUN 3u

/** @brief Script steps one input may take: each is an operation and two arguments. */
#define STEP_BYTES 3u
#define MAX_STEPS 64u

/** @brief Each tick, in ms, as a 60 Hz display loop runs. */
#define FRAME_MS 16u

/** @brief The ms each unit of a frame step's argument adds. */
#define SHORT_FRAME_UNIT_MS 4u
#define LONG_FRAME_UNIT_MS 1000u

/** @brief The values a step's byte argument takes, and the one that reads as 0 when it is signed. */
#define ARGUMENT_RANGE 256u
#define ARGUMENT_MIDPOINT 128

/** @brief Keeps a key step's character in ASCII. */
#define ASCII_MASK 0x7Fu

#define MS_PER_SECOND 1000

/** @brief Date.now() as the app sees it when a run starts. */
#define WALL_CLOCK_MS 1767225600000LL

/** @brief The finger every touch step uses. */
#define TOUCH_FINGER 0

/** @brief Room for an error's message, and a short one that cuts it off. */
#define ERROR_MESSAGE_BYTES 256
#define SHORT_MESSAGE_BYTES 8

/** @brief The fonts, glyphs and extra glyphs a package may register. */
#define FONT_SLOTS 8
#define GLYPH_SLOTS 1024
#define EXTRA_GLYPH_SLOTS 64

/** @brief The probe's done event, by its index in s_natives. */
#define PROBE_DONE 10

/** @brief What a script step does with its first and second argument. An operation past the last wraps around. */
enum
{
    OPERATION_FRAME,        /* a frame of first * SHORT_FRAME_UNIT_MS */
    OPERATION_LONG_FRAME,   /* a frame of first * LONG_FRAME_UNIT_MS */
    OPERATION_TOUCH_DOWN,   /* at (first, second), as a share of the panel, then a frame */
    OPERATION_TOUCH_MOVE,   /* the same */
    OPERATION_TOUCH_UP,     /* the same */
    OPERATION_TOUCH_CANCEL, /* the same */
    OPERATION_KEY,          /* keycode first, or the character second when first is 0 */
    OPERATION_INPUT_INT,    /* input first set to second - ARGUMENT_MIDPOINT */
    OPERATION_INPUT_BOOL,   /* input first set to second & 1 */
    OPERATION_INPUT_FLOAT,  /* input first set to second - ARGUMENT_MIDPOINT, as a float */
    OPERATION_INPUT_STRING, /* input first set to second copies of 'x' */
    OPERATION_WALL_CLOCK,   /* the clock set to (first * ARGUMENT_RANGE + second) seconds */
    OPERATION_RELOAD,       /* the package loaded again over itself */
    OPERATION_UNLOAD,       /* the app unloaded and loaded again */
    OPERATION_NATIVE_EVENT, /* native first posted, every value second - ARGUMENT_MIDPOINT */
    OPERATION_COUNT,
};

/** @brief The host's side of a run with no script: taps across the panel, a drag, typing, inputs, time and the
 *         probe's done event (PROBE_DONE). */
static const uint8_t s_default_script[] = {
    OPERATION_FRAME,      4,   0,   OPERATION_TOUCH_DOWN,  32,  32,  OPERATION_TOUCH_UP,     32,  32,
    OPERATION_TOUCH_DOWN, 128, 32,  OPERATION_TOUCH_UP,    128, 32,  OPERATION_TOUCH_DOWN,   224, 32,
    OPERATION_TOUCH_UP,   224, 32,  OPERATION_TOUCH_DOWN,  32,  128, OPERATION_TOUCH_UP,     32,  128,
    OPERATION_TOUCH_DOWN, 128, 128, OPERATION_TOUCH_UP,    128, 128, OPERATION_TOUCH_DOWN,   224, 128,
    OPERATION_TOUCH_UP,   224, 128, OPERATION_TOUCH_DOWN,  32,  224, OPERATION_TOUCH_UP,     32,  224,
    OPERATION_TOUCH_DOWN, 128, 224, OPERATION_TOUCH_UP,    128, 224, OPERATION_TOUCH_DOWN,   224, 224,
    OPERATION_TOUCH_MOVE, 128, 224, OPERATION_TOUCH_MOVE,  32,  128, OPERATION_TOUCH_UP,     32,  128,
    OPERATION_KEY,        0,   'a', OPERATION_KEY,         8,   0,   OPERATION_INPUT_INT,    0,   200,
    OPERATION_INPUT_BOOL, 0,   1,   OPERATION_INPUT_FLOAT, 0,   64,  OPERATION_INPUT_STRING, 0,   8,
    OPERATION_WALL_CLOCK, 99,  1,   OPERATION_LONG_FRAME,  1,   0,   OPERATION_FRAME,        4,   0,
    OPERATION_FRAME,      4,   0,   OPERATION_FRAME,       4,   0,   OPERATION_NATIVE_EVENT, 10,  133,
};

/** @brief A native function, so a package's import of it resolves. The runtime's tests build packages that use it. */
static bool sensors_read(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = arguments[0] == INT32_MAX ? 0 : arguments[0] + 1;
    return arguments[0] != INT32_MIN;
}

/** @brief The probe module the native-calls and native-events frame tests call, so their seeds load. Each wraps
 *         rather than overflows. */
static uint32_t s_probe_count;
static int32_t s_probe_stored;

static bool probe_add(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = (int32_t)((uint32_t)arguments[0] + (uint32_t)arguments[1]);
    return true;
}

static bool probe_half(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    const float half = (float)arguments[0] / 2.0f;
    memcpy(result, &half, sizeof(half));
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
    *result = (int32_t)++s_probe_count;
    return true;
}

static bool probe_store(const int32_t* arguments, int32_t* result, void* context)
{
    (void)result;
    (void)context;
    s_probe_stored = arguments[0];
    return true;
}

static bool probe_stored(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = s_probe_stored;
    return true;
}

/** @brief Posts done from inside the call, which the runtime refuses. */
static bool probe_start(const int32_t* arguments, int32_t* result, void* context)
{
    (void)result;
    (void)context;
    const int32_t values[3] = {arguments[0], arguments[0], arguments[0] & 1};
    erb_native_post(PROBE_DONE, values);
    return true;
}

static bool probe_refused(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)context;
    *result = 0;
    return true;
}

static const ErbNative s_natives[] = {
    {"sensors.read", 1, "i:i", sensors_read},
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
    {"probe.idle", 1, "v:", NULL},
};

static BitmapFont s_fonts[FONT_SLOTS];
static GlyphInfo s_glyphs[GLYPH_SLOTS];
static ExtraGlyph s_extras[EXTRA_GLYPH_SLOTS];

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
    char message[ERROR_MESSAGE_BYTES];
    char short_message[SHORT_MESSAGE_BYTES];
    erb_error_message(&run->error, message, sizeof(message));
    erb_error_message(&run->error, short_message, sizeof(short_message));
    erb_status_name(run->error.status);
    log_line(message, NULL);
    erb_show_error(&run->error);
    er_commit();
    return false;
}

/** @brief Runs one step of a script. */
static void step(Run* run, uint8_t operation, uint8_t first_argument, uint8_t second_argument)
{
    const int x = (int)((uint32_t)first_argument * run->config.screen_width / ARGUMENT_RANGE);
    const int y = (int)((uint32_t)second_argument * run->config.screen_height / ARGUMENT_RANGE);
    switch (operation % OPERATION_COUNT)
    {
        case OPERATION_FRAME:
            frame((uint32_t)first_argument * SHORT_FRAME_UNIT_MS);
            break;
        case OPERATION_LONG_FRAME:
            frame((uint32_t)first_argument * LONG_FRAME_UNIT_MS);
            break;
        case OPERATION_TOUCH_DOWN:
            embedded_renderer_touch(TOUCH_FINGER, ER_TOUCH_DOWN, x, y);
            frame(FRAME_MS);
            break;
        case OPERATION_TOUCH_MOVE:
            embedded_renderer_touch(TOUCH_FINGER, ER_TOUCH_MOVE, x, y);
            frame(FRAME_MS);
            break;
        case OPERATION_TOUCH_UP:
            embedded_renderer_touch(TOUCH_FINGER, ER_TOUCH_UP, x, y);
            frame(FRAME_MS);
            break;
        case OPERATION_TOUCH_CANCEL:
            embedded_renderer_touch(TOUCH_FINGER, ER_TOUCH_CANCEL, x, y);
            frame(FRAME_MS);
            break;
        case OPERATION_KEY:
        {
            const char text[2] = {(char)(second_argument & ASCII_MASK), '\0'};
            embedded_renderer_key(first_argument, first_argument ? NULL : text);
            break;
        }
        case OPERATION_INPUT_INT:
            erb_input_set_int(first_argument, (int32_t)second_argument - ARGUMENT_MIDPOINT);
            break;
        case OPERATION_INPUT_BOOL:
            erb_input_set_bool(first_argument, (second_argument & 1u) != 0u);
            break;
        case OPERATION_INPUT_FLOAT:
            erb_input_set_float(first_argument, (float)second_argument - (float)ARGUMENT_MIDPOINT);
            break;
        case OPERATION_INPUT_STRING:
        {
            char text[ARGUMENT_RANGE];
            memset(text, 'x', second_argument);
            text[second_argument] = '\0';
            erb_input_set_string(first_argument, text);
            break;
        }
        case OPERATION_WALL_CLOCK:
            erb_set_wall_clock((int64_t)((uint32_t)first_argument * ARGUMENT_RANGE + second_argument) * MS_PER_SECOND);
            break;
        case OPERATION_RELOAD:
            load(run);
            break;
        case OPERATION_UNLOAD:
            erb_unload();
            load(run);
            break;
        case OPERATION_NATIVE_EVENT:
        {
            int32_t values[ERB_NATIVE_MAX_PARAMETERS];
            for (uint32_t value = 0; value < ERB_NATIVE_MAX_PARAMETERS; value++)
            {
                values[value] = (int32_t)second_argument - ARGUMENT_MIDPOINT;
            }
            erb_native_post(first_argument % (sizeof(s_natives) / sizeof(s_natives[0])), values);
            break;
        }
        default:
            break;
    }
}

/**
 * @brief The arena a package says it needs at most. Bounds-checked: the package isn't verified.
 *
 * @return The bytes, or 0 when the header can't be read.
 */
static uint32_t read_arena(const ErContainer* container)
{
    for (uint32_t section_index = 0; section_index < container->section_count; section_index++)
    {
        const ErContainerSection* section = &container->sections[section_index];
        if (section->type == ERB_SECTION_PROGRAM && section->length >= ERB_HEADER_SIZE)
        {
            return erb_u32(section->data + ERB_HEADER_ARENA_BYTES_AT);
        }
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
    const uint16_t width = s_panels[panel % PANEL_COUNT][0];
    const uint16_t height = s_panels[panel % PANEL_COUNT][1];
    uint32_t arena_bytes = scanned ? read_arena(container) : 0u;
    if (arena_bytes > MAX_ARENA_BYTES)
    {
        arena_bytes = MAX_ARENA_BYTES;
    }
    void* arena = malloc(arena_bytes ? arena_bytes : 1u);
    if (!arena || !er_software_backend_init(width, height))
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
    run.config.screen_width = width;
    run.config.screen_height = height;
    run.config.natives = s_natives;
    run.config.native_count = sizeof(s_natives) / sizeof(s_natives[0]);
    run.config.fonts = &run.fonts;
    run.config.log = log_line;

    erb_set_wall_clock(WALL_CLOCK_MS);
    const bool loaded = load(&run);
    if (!loaded && getenv("ERB_FUZZ_REQUIRE_LOAD"))
    {
        char message[ERROR_MESSAGE_BYTES];
        erb_error_message(&run.error, message, sizeof(message));
        fprintf(stderr, "a seed didn't load (%ux%u): %s\n", width, height, message);
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
    ErContainer input_container;
    const bool scanned = er_container_scan(data, size, &input_container) == ER_CONTAINER_OK;
    const size_t package_bytes = scanned ? input_container.size : size;

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
        for (uint32_t panel = 0; panel < PANELS_RUN; panel++)
        {
            run_panel(package, package_bytes, &container, scanned, panel, s_default_script, sizeof(s_default_script));
        }
    }

    free(package);
    return 0;
}
