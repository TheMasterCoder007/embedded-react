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
 * Screen test: a runtime whose host leaves the size unset takes `screen` from the engine's display, turned by
 * its rotation. Needs the parser (evaluates JS source).
 */

#include "er_runtime.h"
#include "er_scene.h"
#include "host_harness.h"
#include "native_renderer.h"

#include <stdio.h>
#include <string.h>

/** @brief The panel as it scans; mounted at 90 it runs a 240x320 app. */
#define PANEL_WIDTH 320
#define PANEL_HEIGHT 240

/** @brief Sizes a host names itself. */
#define NAMED_WIDTH 100
#define NAMED_HEIGHT 50

static int s_failed = 0;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL: %s\n", (message));                                                                           \
            s_failed = 1;                                                                                              \
        }                                                                                                              \
    } while (0)

/**
 * @brief Brings a runtime up with the given size and checks the `screen` it shows the app.
 *
 * @param[in] config_width   ErRuntimeConfig.screen_width.
 * @param[in] config_height  ErRuntimeConfig.screen_height.
 * @param[in] check          JS that throws when `screen` is wrong.
 *
 * @return true when the app ran without throwing.
 */
static bool screen_matches(int config_width, int config_height, const char* check)
{
    ErRuntimeConfig config = {0};
    config.screen_width = config_width;
    config.screen_height = config_height;
    config.screen_scale = 1.0f;
    if (!er_runtime_init(&config))
    {
        printf("FAIL: er_runtime_init\n");
        return false;
    }
    const bool ran = er_runtime_load_source(check, strlen(check), "App.js");
    if (!ran)
        printf("  %s\n", er_runtime_last_error());
    er_runtime_shutdown();
    return ran;
}

int main(void)
{
    static const EmbeddedRenderBackend backend = ER_HOST_NOOP_BACKEND;
    embedded_renderer_set_backend(&backend);

    CHECK(er_set_display_rotation(ER_ROTATION_90, PANEL_WIDTH, PANEL_HEIGHT), "a panel mounted at 90");
    CHECK(screen_matches(0,
                         0,
                         "if (screen.width !== 240 || screen.height !== 320)\n"
                         "  throw new Error('screen is ' + screen.width + 'x' + screen.height);\n"),
          "an unset size is the display's, turned");
    CHECK(screen_matches(NAMED_WIDTH,
                         NAMED_HEIGHT,
                         "if (screen.width !== 100 || screen.height !== 50)\n"
                         "  throw new Error('screen is ' + screen.width + 'x' + screen.height);\n"),
          "a size the host names wins");

    if (s_failed)
        return 1;
    printf("screen: all tests passed\n");
    return 0;
}
