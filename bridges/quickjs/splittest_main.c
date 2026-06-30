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
#include "native_renderer.h"
#include "quickjs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ST_SCREEN_W 480
#define ST_SCREEN_H 320

/** @brief No-op render backend — the engine's paint path runs without a window. */
static void noop_fill(uint32_t a, int x, int y, int w, int h, void* c)
{
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
}
static void noop_copy(const void* s, int st, int x, int y, int w, int h, void* c)
{
    (void)s;
    (void)st;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
}
static void noop_blend(const void* s, int st, uint8_t a, int x, int y, int w, int h, void* c)
{
    (void)s;
    (void)st;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
}

/** @brief Runtime log sink → stdout (one line per call). */
static void log_line(const char* line)
{
    printf("%s\n", line);
}

/** @brief Reads a whole file into a malloc'd buffer. @param path p. @param out_len receives length. */
static uint8_t* read_file(const char* path, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
    {
        fclose(f);
        return NULL;
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    *out_len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

/** @brief Reads globalThis.__probe.{build,n}. @return 0 on success, -1 if __probe is missing. */
static int read_probe(char* build_out, size_t build_cap, int32_t* n_out)
{
    JSContext* ctx = er_runtime_context();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue probe = JS_GetPropertyStr(ctx, global, "__probe");
    int rc = -1;
    if (JS_IsObject(probe))
    {
        JSValue build = JS_GetPropertyStr(ctx, probe, "build");
        const char* bs = JS_ToCString(ctx, build);
        if (bs)
        {
            snprintf(build_out, build_cap, "%s", bs);
            JS_FreeCString(ctx, bs);
            JSValue nv = JS_GetPropertyStr(ctx, probe, "n");
            JS_ToInt32(ctx, n_out, nv);
            JS_FreeValue(ctx, nv);
            rc = 0;
        }
        JS_FreeValue(ctx, build);
    }
    JS_FreeValue(ctx, probe);
    JS_FreeValue(ctx, global);
    return rc;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <boot.erpkg> <reloaded.erpkg>\n", argv[0]);
        return 2;
    }

    size_t boot_len = 0, frame_len = 0;
    uint8_t* boot = read_file(argv[1], &boot_len);
    uint8_t* frame = read_file(argv[2], &frame_len);
    if (!boot || !frame)
    {
        fprintf(stderr, "could not read container(s)\n");
        return 2;
    }

    static const EmbeddedRenderBackend backend = {noop_fill, noop_copy, noop_blend, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    ErRuntimeConfig cfg = {0};
    cfg.screen_width = ST_SCREEN_W;
    cfg.screen_height = ST_SCREEN_H;
    cfg.screen_scale = 1.0f;
    cfg.install_persist = true; /* so usePersistentState has a store to survive in */
    cfg.log = log_line;
    if (!er_runtime_init(&cfg))
    {
        fprintf(stderr, "er_runtime_init failed\n");
        return 1;
    }

    int failed = 0;
    char build[32] = {0};
    int32_t n = 0;

    /* 1) Boot the SPLIT container: vendor runs first (installs require()), then the app mounts. */
    ErContainerStatus s1 = er_runtime_load_container(boot, boot_len);
    if (s1 != ER_CONTAINER_OK)
    {
        printf("FAIL: boot load -> %s\n", er_runtime_container_status_str(s1));
        return 1;
    }
    er_runtime_pump();
    if (read_probe(build, sizeof build, &n) != 0)
    {
        printf("FAIL: no __probe after boot (app did not mount)\n");
        return 1;
    }
    printf("after boot:         build=%s n=%d\n", build, n);
    if (strcmp(build, "boot") != 0 || n != 41)
    {
        printf("FAIL: expected build=boot n=41 after boot\n");
        failed = 1;
    }

    /* 2) Soft reload through the REAL firmware apply path: er_hotreload_apply sees no vendor section and
     *    takes the soft path (no reset). The resident vendor must be reused (require() still resolves) and
     *    the persisted 'n' must survive. */
    ErContainerStatus s2 = er_hotreload_apply(frame, frame_len);
    if (s2 != ER_CONTAINER_OK)
    {
        printf("FAIL: app-only soft reload -> %s\n", er_runtime_container_status_str(s2));
        return 1;
    }
    er_runtime_pump();
    if (read_probe(build, sizeof build, &n) != 0)
    {
        printf("FAIL: no __probe after soft reload\n");
        return 1;
    }
    printf("after soft reload:  build=%s n=%d\n", build, n);
    if (strcmp(build, "reloaded") != 0)
    {
        printf("FAIL: app-only frame did not run the new code (vendor not resident?)\n");
        failed = 1;
    }
    if (n != 41)
    {
        printf("FAIL: persisted state lost across soft reload (n=%d, expected 41)\n", n);
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
