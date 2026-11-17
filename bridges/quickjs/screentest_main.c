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
 * its rotation, and er_runtime_set_screen_size() changes it under a running app. Needs the parser (evaluates JS
 * source).
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

/** @brief An app that counts the screen changes it hears of, and keeps what `screen` was at each. */
static const char LISTENING_APP[] = "globalThis.changes = [];\n"
                                    "const held = screen;\n"
                                    "NativeUI.setScreenListener(() => changes.push(held.width + 'x' + held.height));\n";

/**
 * @brief Whether the app's `changes` joined is @p expected.
 *
 * @param[in] expected  The sizes it should have heard, comma-separated.
 *
 * @return true when they match.
 */
static bool changes_are(const char* expected)
{
    JSContext* ctx = er_runtime_context();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue changes = JS_GetPropertyStr(ctx, global, "changes");
    JSValue joined = JS_ToString(ctx, changes);
    const char* text = JS_ToCString(ctx, joined);
    const bool same = text && strcmp(text, expected) == 0;
    if (!same)
        printf("  changes are \"%s\"\n", text ? text : "(none)");
    if (text)
        JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, joined);
    JS_FreeValue(ctx, changes);
    JS_FreeValue(ctx, global);
    return same;
}

/** @brief er_runtime_set_screen_size(): the app hears of each new size, with `screen` already changed. */
static void test_live_size(void)
{
    CHECK(!er_runtime_set_screen_size(NAMED_WIDTH, NAMED_HEIGHT), "no runtime, no size to set");

    ErRuntimeConfig config = {0};
    config.screen_scale = 1.0f;
    CHECK(er_set_display_rotation(ER_ROTATION_0, PANEL_WIDTH, PANEL_HEIGHT), "a panel at 0");
    CHECK(er_runtime_init(&config), "er_runtime_init");
    CHECK(er_runtime_load_source(LISTENING_APP, strlen(LISTENING_APP), "App.js"), "the app loads");

    CHECK(er_runtime_set_screen_size(PANEL_WIDTH, PANEL_HEIGHT), "the size it has");
    CHECK(changes_are(""), "is no change");

    CHECK(er_runtime_set_screen_size(NAMED_WIDTH, NAMED_HEIGHT), "a named size");
    CHECK(changes_are("100x50"), "reaches the app, in the screen object it holds");

    /* A quarter turn, with the size left to the engine. */
    CHECK(er_set_display_rotation(ER_ROTATION_90, PANEL_WIDTH, PANEL_HEIGHT), "the panel turns");
    CHECK(er_runtime_set_screen_size(0, 0), "an unset size");
    CHECK(changes_are("100x50,240x320"), "is the display's, turned");

    /* A reset comes back at the new size. */
    CHECK(er_runtime_reset(), "er_runtime_reset");
    static const char check[] = "if (screen.width !== 240 || screen.height !== 320) throw new Error('reset');\n";
    CHECK(er_runtime_load_source(check, strlen(check), "App.js"), "a reset keeps the size");
    er_runtime_shutdown();
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

    test_live_size();

    if (s_failed)
        return 1;
    printf("screen: all tests passed\n");
    return 0;
}
