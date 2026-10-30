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
 * Error screen test: a JS exception nothing caught is logged and shows the engine's error screen, and a
 * reset or a good load takes it down. Needs the parser (evaluates JS source).
 */

#include "er_error_screen.h"
#include "er_runtime.h"
#include "host_harness.h"
#include "native_renderer.h"

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

#define LOG_SIZE 2048u
#define TIMER_MS 100u

static int s_failed = 0;
static char s_log[LOG_SIZE];

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL: %s\n", (message));                                                                           \
            s_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

/** @brief The log sink: keeps every line, so a test can look for one. */
static void keep_line(const char* line)
{
    const size_t used = strlen(s_log);
    snprintf(s_log + used, sizeof(s_log) - used, "%s\n", line);
}

static bool load(const char* source)
{
    s_log[0] = '\0';
    return er_runtime_load_source(source, strlen(source), "App.js");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static void test_failed_load(void)
{
    CHECK(!load("throw new Error('boot failed');"), "a throwing app doesn't load");
    CHECK(strstr(er_runtime_last_error(), "boot failed") != NULL, "its message is kept");
    CHECK(strstr(s_log, "JS exception: Error: boot failed") != NULL, "and logged");
    er_runtime_show_error();
    CHECK(er_error_screen_active(), "the host shows it");
    er_runtime_reset();
    CHECK(!er_error_screen_active(), "a reset takes it down");
}

static void test_uncaught_timer(void)
{
    CHECK(load("setTimeout(() => { throw new Error('tick failed'); }, 100);"), "a timer app loads");
    CHECK(!er_error_screen_active(), "with no error up");
    embedded_renderer_tick(TIMER_MS);
    er_runtime_pump();
    CHECK(er_error_screen_active(), "the throwing timer shows the error screen");
    CHECK(strstr(er_runtime_last_error(), "tick failed") != NULL, "with its message");
    CHECK(strstr(s_log, "JS timer callback exception: Error: tick failed") != NULL, "and logs where it threw");

    CHECK(load("globalThis.fixed = true;"), "a good app loads");
    CHECK(!er_error_screen_active(), "and takes it down");
}

static void test_message(void)
{
    er_runtime_show_message("No config loaded", "flash a config", NULL);
    CHECK(er_error_screen_active(), "a message shows on the error screen");
    er_runtime_reset();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    static const EmbeddedRenderBackend backend = ER_HOST_NOOP_BACKEND;
    embedded_renderer_set_backend(&backend);
    ErRuntimeConfig config = {0};
    config.screen_width = 320;
    config.screen_height = 240;
    config.screen_scale = 1.0f;
    config.log = keep_line;
    if (!er_runtime_init(&config))
    {
        printf("FAIL: er_runtime_init\n");
        return 1;
    }

    test_failed_load();
    test_uncaught_timer();
    test_message();

    er_runtime_shutdown();
    if (s_failed)
    {
        return 1;
    }
    printf("error screen: all tests passed\n");
    return 0;
}
