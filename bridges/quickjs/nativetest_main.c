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
 * Native module test: `__erNative` finds the firmware's natives by name, version and signature, converts arguments
 * and results by the signature, and throws when a call is wrong or a native fails. er_runtime_native_post() hands an
 * event to `__erNativeEvent`, converted by its signature, and refuses one it can't take. Needs the parser (evaluates
 * JS source).
 */

#include "er_runtime.h"
#include "host_harness.h"
#include "native_renderer.h"

#include <stdio.h>
#include <string.h>

/** @brief Screen the test runtime is configured for. */
#define TEST_SCREEN_WIDTH 320
#define TEST_SCREEN_HEIGHT 240

/** @brief What test.poke stores when the test starts. */
#define POKED_NOTHING (-1)

/** @brief What the script pokes. */
#define POKED_VALUE 7

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

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

/** @brief Room for one script. */
#define SCRIPT_BYTES 2048u

/** @brief Runs a script in a function of its own, so its names don't outlive it. It throws when a check fails. */
static bool run(const char* body)
{
    static char source[SCRIPT_BYTES];
    snprintf(source, sizeof(source), "(() => {\n%s})();\n", body);
    const bool ok = er_runtime_load_source(source, strlen(source), "native-test.js");
    if (!ok)
    {
        printf("  %s\n", er_runtime_last_error());
    }
    return ok;
}

/*----------------------------------------------------------------------------------------------------------------------
 - The natives
 ---------------------------------------------------------------------------------------------------------------------*/

static float cell_float(int32_t cell)
{
    float value;
    memcpy(&value, &cell, sizeof(value));
    return value;
}

static int32_t float_cell(float value)
{
    int32_t cell;
    memcpy(&cell, &value, sizeof(cell));
    return cell;
}

static bool native_add(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = arguments[0] + arguments[1];
    return true;
}

static bool native_half(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = float_cell(cell_float(arguments[0]) / 2.0f);
    return true;
}

static bool native_not(const int32_t* arguments, int32_t* result, void* context)
{
    (void)context;
    *result = !arguments[0];
    return true;
}

static bool native_poke(const int32_t* arguments, int32_t* result, void* context)
{
    (void)result;
    *(int32_t*)context = arguments[0];
    return true;
}

static bool native_fail(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)result;
    (void)context;
    return false;
}

/** @brief The firmware's event, by its index in s_natives. */
#define NATIVE_READING 5

/** @brief What er_runtime_native_post() said when test.post posted from inside its call. */
static bool s_posted_inside = true;

static bool native_post(const int32_t* arguments, int32_t* result, void* context)
{
    (void)arguments;
    (void)result;
    (void)context;
    const int32_t values[3] = {1, 0, 1};
    s_posted_inside = er_runtime_native_post(NATIVE_READING, values);
    return true;
}

static const ErbNative s_natives[] = {
    {"test.add", 1, "i:ii", native_add},
    {"test.half", 1, "f:f", native_half},
    {"test.not", 1, "b:b", native_not},
    {"test.poke", 1, "v:i", native_poke},
    {"test.fail", 1, "v:", native_fail},
    [NATIVE_READING] = {"test.reading", 1, "v:ifb", NULL},
    {"test.post", 1, "v:", native_post},
};

static int32_t s_poked = POKED_NOTHING;

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The script's check(): throws with the message when the condition is false. */
#define SCRIPT_CHECK "function check(condition, message) { if (!condition) throw new Error(message); }\n"

static void test_find(void)
{
    CHECK(run(SCRIPT_CHECK "check(__erNative.find('test.add', 1, 'i:ii') === 0, 'finds test.add');\n"
                           "check(__erNative.find('test.fail', 1, 'v:') === 4, 'finds test.fail');\n"
                           "check(__erNative.find('test.add', 2, 'i:ii') === -1, 'another version is missing');\n"
                           "check(__erNative.find('test.add', 1, 'i:if') === -1, 'another signature is missing');\n"
                           "check(__erNative.find('test.sub', 1, 'i:ii') === -1, 'another name is missing');\n"),
          "natives are found by name, version and signature");
}

static void test_conversions(void)
{
    CHECK(run(SCRIPT_CHECK
              "const add = __erNative.find('test.add', 1, 'i:ii');\n"
              "check(__erNative.call(add, 2.9, -1) === 1, 'an int is truncated');\n"
              "check(__erNative.call(add, 2 ** 32 + 5, 0) === 5, 'and wraps');\n"
              "const half = __erNative.find('test.half', 1, 'f:f');\n"
              "check(__erNative.call(half, 0.1) === Math.fround(0.1) / 2, 'a number goes through a float');\n"
              "const not = __erNative.find('test.not', 1, 'b:b');\n"
              "check(__erNative.call(not, 0) === true, 'a falsy value is false');\n"
              "check(__erNative.call(not, 'yes') === false, 'a truthy one is true');\n"
              "const poke = __erNative.find('test.poke', 1, 'v:i');\n"
              "check(__erNative.call(poke, 7) === undefined, 'void returns undefined');\n"),
          "arguments and results convert by the signature");
    CHECK(s_poked == POKED_VALUE, "a native gets the runtime's native context");
}

static void test_wrong_calls(void)
{
    CHECK(run(SCRIPT_CHECK "function message(call) { try { call(); } catch (error) { return error.message; } }\n"
                           "const add = __erNative.find('test.add', 1, 'i:ii');\n"
                           "check(message(() => __erNative.call(add, 1)) === 'test.add() takes 2 arguments, not 1',\n"
                           "      'a call with the wrong argument count throws');\n"
                           "check(message(() => __erNative.call(99)) === 'no native function 99',\n"
                           "      'an index past the natives throws');\n"
                           "const fail = __erNative.find('test.fail', 1, 'v:');\n"
                           "check(message(() => __erNative.call(fail)) === 'native module failed: test.fail',\n"
                           "      'a native that fails throws, naming it');\n"),
          "a wrong call or a failing native throws");
}

static void test_events(void)
{
    CHECK(run(SCRIPT_CHECK
              "check(__erNative.findEvent('test.reading', 1, 'v:ifb') === 5, 'finds test.reading');\n"
              "check(__erNative.find('test.reading', 1, 'v:ifb') === -1, 'an event is no function');\n"
              "check(__erNative.findEvent('test.add', 1, 'i:ii') === -1, 'nor a function an event');\n"
              "check(__erNative.findEvent('test.reading', 2, 'v:ifb') === -1, 'another version');\n"
              "globalThis.__erNativeEvent = (index, values) => { globalThis.heard = [index, ...values]; };\n"),
          "events are found by name, version and signature");

    const int32_t values[3] = {3, float_cell(0.1f), 7};
    CHECK(er_runtime_native_post(NATIVE_READING, values), "a post is taken");
    CHECK(run(SCRIPT_CHECK "check(heard.length === 4 && heard[0] === 5, 'the index');\n"
                           "check(heard[1] === 3 && heard[2] === Math.fround(0.1) && heard[3] === true,\n"
                           "      'and the values, converted by the signature');\n"),
          "the dispatcher gets the event's index and values");

    CHECK(run(SCRIPT_CHECK "const post = __erNative.find('test.post', 1, 'v:');\n"
                           "globalThis.heard = null;\n"
                           "__erNative.call(post);\n"
                           "check(heard === null, 'nothing is heard');\n"),
          "a post from inside a native call runs nothing");
    CHECK(!s_posted_inside, "and is refused");
    CHECK(!er_runtime_native_post(0, values), "a native function isn't posted");
    CHECK(!er_runtime_native_post(99, values), "nor an index past the natives");

    CHECK(run("globalThis.__erNativeEvent = () => { throw new Error('boom'); };\n"), "a dispatcher that throws");
    CHECK(er_runtime_native_post(NATIVE_READING, values) && strstr(er_runtime_last_error(), "boom"),
          "is reported, and the post still taken");
}

static void test_reset(void)
{
    CHECK(er_runtime_reset(), "the runtime resets");
    const int32_t values[3] = {0, 0, 0};
    CHECK(!er_runtime_native_post(NATIVE_READING, values), "a post with no app loaded is refused");
    CHECK(run(SCRIPT_CHECK "check(__erNative.find('test.add', 1, 'i:ii') === 0, 'finds test.add');\n"),
          "a reset keeps the natives");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    static const EmbeddedRenderBackend backend = ER_HOST_NOOP_BACKEND;
    embedded_renderer_set_backend(&backend);
    ErRuntimeConfig config = {0};
    config.screen_width = TEST_SCREEN_WIDTH;
    config.screen_height = TEST_SCREEN_HEIGHT;
    config.screen_scale = 1.0f;
    config.natives = s_natives;
    config.native_count = sizeof(s_natives) / sizeof(s_natives[0]);
    config.native_context = &s_poked;
    if (!er_runtime_init(&config))
    {
        printf("FAIL: er_runtime_init\n");
        return 1;
    }

    const int32_t values[3] = {0, 0, 0};
    CHECK(!er_runtime_native_post(NATIVE_READING, values), "a post before an app loads is refused");
    test_find();
    test_conversions();
    test_wrong_calls();
    test_events();
    test_reset();

    er_runtime_shutdown();
    if (s_failed)
    {
        return 1;
    }
    printf("native modules: all tests passed\n");
    return 0;
}
