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
 * Desktop round-trip test for the vendor/app split + soft reload — the device hot-reload mechanism, run
 * headlessly in real QuickJS so it's validated before flashing.
 *
 * Loads a SPLIT boot container (vendor section + app section) exactly as a device boots one, then loads an
 * app-only frame (no vendor) WITHOUT a reset — the "soft" reload. It asserts, via a globalThis.__probe the
 * fixtures publish, that:
 *   1. the boot container runs the vendor first (installs the require() registry) then the app;
 *   2. the app-only frame mounts against the STILL-RESIDENT vendor (require() resolves with no re-eval);
 *   3. usePersistentState survives the soft reload (the seeded value is read back by the new app).
 *
 * Usage: er-bridge-quickjs-splittest <boot.erpkg> <reloaded.erpkg>  (containers built by split-roundtrip.mjs)
 */

#include "er_hotreload.h"
#include "er_runtime.h"
#include "host_harness.h"
#include "native_renderer.h"
#include "quickjs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ST_SCREEN_WIDTH 480
#define ST_SCREEN_HEIGHT 320

/** @brief Room for the fixture's build name. */
#define ST_BUILD_NAME_BYTES 32

/** @brief The value the boot fixture persists as `n`. */
#define ST_PERSISTED_VALUE 41

/** @brief Runtime log sink → stdout (one line per call). */
static void log_line(const char* line)
{
    printf("%s\n", line);
}

/** @brief Reads globalThis.__probe.{build,n}. @return 0 on success, -1 if __probe is missing. */
static int read_probe(char* build_out, size_t build_capacity, int32_t* persisted_out)
{
    JSContext* ctx = er_runtime_context();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue probe = JS_GetPropertyStr(ctx, global, "__probe");
    int status = -1;
    if (JS_IsObject(probe))
    {
        JSValue build = JS_GetPropertyStr(ctx, probe, "build");
        const char* build_text = JS_ToCString(ctx, build);
        if (build_text)
        {
            snprintf(build_out, build_capacity, "%s", build_text);
            JS_FreeCString(ctx, build_text);
            JSValue persisted_value = JS_GetPropertyStr(ctx, probe, "n");
            JS_ToInt32(ctx, persisted_out, persisted_value);
            JS_FreeValue(ctx, persisted_value);
            status = 0;
        }
        JS_FreeValue(ctx, build);
    }
    JS_FreeValue(ctx, probe);
    JS_FreeValue(ctx, global);
    return status;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <boot.erpkg> <reloaded.erpkg>\n", argv[0]);
        return 2;
    }

    size_t boot_length = 0, frame_length = 0;
    uint8_t* boot = er_host_read_file(argv[1], &boot_length);
    uint8_t* frame = er_host_read_file(argv[2], &frame_length);
    if (!boot || !frame)
    {
        fprintf(stderr, "could not read container(s)\n");
        return 2;
    }

    static const EmbeddedRenderBackend backend = ER_HOST_NOOP_BACKEND;
    embedded_renderer_set_backend(&backend);

    ErRuntimeConfig config = {0};
    config.screen_width = ST_SCREEN_WIDTH;
    config.screen_height = ST_SCREEN_HEIGHT;
    config.screen_scale = 1.0f;
    config.install_persist = true; /* so usePersistentState has a store to survive in */
    config.log = log_line;
    if (!er_runtime_init(&config))
    {
        fprintf(stderr, "er_runtime_init failed\n");
        return 1;
    }

    int failed = 0;
    char build[ST_BUILD_NAME_BYTES] = {0};
    int32_t persisted = 0;

    /* 1) Boot the SPLIT container: vendor runs first (installs require()), then the app mounts. */
    ErContainerStatus boot_status = er_runtime_load_container(boot, boot_length);
    if (boot_status != ER_CONTAINER_OK)
    {
        printf("FAIL: boot load -> %s\n", er_runtime_container_status_str(boot_status));
        return 1;
    }
    er_runtime_pump();
    if (read_probe(build, sizeof build, &persisted) != 0)
    {
        printf("FAIL: no __probe after boot (app did not mount)\n");
        return 1;
    }
    printf("after boot:         build=%s n=%d\n", build, persisted);
    if (strcmp(build, "boot") != 0 || persisted != ST_PERSISTED_VALUE)
    {
        printf("FAIL: expected build=boot n=%d after boot\n", ST_PERSISTED_VALUE);
        failed = 1;
    }

    /* 2) Soft reload through the REAL firmware apply path: er_hotreload_apply sees no vendor section and
     *    takes the soft path (no reset). The resident vendor must be reused (require() still resolves) and
     *    the persisted 'n' must survive. */
    ErContainerStatus reload_status = er_hotreload_apply(frame, frame_length);
    if (reload_status != ER_CONTAINER_OK)
    {
        printf("FAIL: app-only soft reload -> %s\n", er_runtime_container_status_str(reload_status));
        return 1;
    }
    er_runtime_pump();
    if (read_probe(build, sizeof build, &persisted) != 0)
    {
        printf("FAIL: no __probe after soft reload\n");
        return 1;
    }
    printf("after soft reload:  build=%s n=%d\n", build, persisted);
    if (strcmp(build, "reloaded") != 0)
    {
        printf("FAIL: app-only frame did not run the new code (vendor not resident?)\n");
        failed = 1;
    }
    if (persisted != ST_PERSISTED_VALUE)
    {
        printf("FAIL: persisted state lost across soft reload (n=%d, expected %d)\n", persisted, ST_PERSISTED_VALUE);
        failed = 1;
    }

    if (!failed)
    {
        printf("PASS: split boot + app-only soft reload (vendor resident, persist survived)\n");
    }

    er_runtime_shutdown();
    free(boot);
    free(frame);
    return failed;
}
