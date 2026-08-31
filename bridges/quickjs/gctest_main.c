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
 * JS-heap accounting regression test — the guard against a silently dead garbage collector.
 *
 * QuickJS triggers its GC (and enforces JS_SetMemoryLimit) purely off js_malloc_usable_size(). Its own
 * default returns 0 on any platform outside Apple/Win32/glibc — bare-metal newlib and Emscripten
 * included — which disables both, and the app then grows until the heap is exhausted. That shipped once
 * (a Flow A STM32F746 host losing ~2.6-6.5 KB per re-render, never recovering), so it gets a test.
 *
 * The failure is platform-dependent but the MECHANISM is not: a JSMallocFunctions whose usable_size
 * returns 0 reproduces it exactly, on any host. So this runs on the Linux CI runner:
 *
 *   - the bridge's default allocator reports real byte sizes (er_js_alloc.c, both modes),
 *   - a broken allocator is detected by er_js_probe_alloc_health / er_runtime_gc_accounting_ok,
 *   - and with working accounting the automatic collector actually reclaims the garbage only it
 *     can reach (reference cycles — refcounting alone frees everything else, so nothing else can
 *     tell a live collector from a dead one).
 *
 * It also covers ErRuntimeConfig.gc_threshold, whose whole point is that the floor survives QuickJS's
 * recompute-after-every-collection — and the manual-collection path a host uses once it raises it.
 *
 * Needs the JS parser, so it is not built for ER_BRIDGE_QUICKJS_LITE.
 */

#include "er_js_alloc.h"
#include "er_runtime.h"
#include "native_renderer.h"
#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Live bytes the growth script allocates (one-byte chars; keep in sync with SRC_GROW). */
#define GROWTH_BYTES 262144

/** @brief Byte-accounting is proven if malloc_size grows by at least this much for that string. */
#define GROWTH_MIN_TRACKED 100000

/** @brief With accounting dead, malloc_size only moves by MALLOC_OVERHEAD per live allocation. */
#define GROWTH_MAX_UNTRACKED 4096

/** @brief Headroom allowed above the pre-churn heap once SRC_CYCLES has run under the automatic GC. */
#define AUTO_GC_MAX_RETAINED (2 * 1024 * 1024)

/** @brief GC floor the threshold scenarios ask for — well above what live x 1.5 would ever settle at. */
#define GC_FLOOR_BYTES (4u * 1024u * 1024u)

/** @brief Growth expected from SRC_CYCLES once automatic collection is switched off entirely. */
#define GC_OFF_MIN_GROWTH (2 * 1024 * 1024)

/** @brief Live bytes SRC_CYCLES may leave behind once the collector is finally allowed to run. */
#define CYCLES_MAX_RETAINED (1024 * 1024)

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_failures = 0;

/**
 * @brief Materialises one live GROWTH_BYTES string — a heap change any byte accounting must see.
 *
 * repeat(), not `s += s`: quickjs-ng concatenation builds a ROPE, and doubling a string by appending it
 * to itself shares both halves, so 256 KB of `length` costs ~1.7 KB of heap. That version measures
 * nothing.
 */
static const char* const SRC_GROW = "globalThis.keep = 'x'.repeat(262144);\n"
                                    "globalThis.keep.length;\n";

/**
 * @brief Churns reference cycles — the only garbage the mark-sweep collector is actually needed for.
 *
 * QuickJS is refcounted first, so plain throwaway objects are freed the instant the loop rebinds the
 * last reference to them — collector or no collector. Churning those measures nothing: this file used
 * to, and the check passed unchanged with the GC switched off (issue #173). A pair that points at each
 * other never reaches refcount zero, so cycles are what accumulate when mark-sweep is not allowed to
 * run (~9 MB here) and what disappear when it is (~30 KB). Every GC claim below is made with these.
 */
static const char* const SRC_CYCLES = "var n = 0;\n"
                                      "for (var i = 0; i < 20000; i++) {\n"
                                      "    var a = { pad: 'x'.repeat(200) };\n"
                                      "    var b = { a: a };\n"
                                      "    a.b = b;\n"
                                      "    n++;\n"
                                      "}\n"
                                      "n;\n";

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — broken allocators (what a host on an unsupported platform effectively gets)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Zeroed allocation. @param opaque Unused. @param count Element count. @param size Element size. */
static void* plain_calloc(void* opaque, size_t count, size_t size)
{
    (void)opaque;
    return calloc(count, size);
}

/** @brief Allocation. @param opaque Unused. @param size Byte count. */
static void* plain_malloc(void* opaque, size_t size)
{
    (void)opaque;
    return malloc(size);
}

/** @brief Free. @param opaque Unused. @param ptr Block. */
static void plain_free(void* opaque, void* ptr)
{
    (void)opaque;
    free(ptr);
}

/** @brief Resize. @param opaque Unused. @param ptr Block. @param size New byte count. */
static void* plain_realloc(void* opaque, void* ptr, size_t size)
{
    (void)opaque;
    return realloc(ptr, size);
}

/** @brief What QuickJS's cutils.h fallback does off the supported platforms. @param ptr Block. */
static size_t usable_size_zero(const void* ptr)
{
    (void)ptr;
    return 0;
}

/** @brief A usable_size reading someone else's heap header: an answer, but not about this block. */
static size_t usable_size_constant(const void* ptr)
{
    (void)ptr;
    return 16;
}

static const JSMallocFunctions MF_NO_ACCOUNTING = {plain_calloc, plain_malloc, plain_free, plain_realloc,
                                                   usable_size_zero};

static const JSMallocFunctions MF_IMPLAUSIBLE = {plain_calloc, plain_malloc, plain_free, plain_realloc,
                                                 usable_size_constant};

/** @brief Refuses everything. @param opaque Unused. @param count Element count. @param size Element size. */
static void* failing_calloc(void* opaque, size_t count, size_t size)
{
    (void)opaque;
    (void)count;
    (void)size;
    return NULL;
}

/** @brief Refuses everything. @param opaque Unused. @param size Byte count. */
static void* failing_malloc(void* opaque, size_t size)
{
    (void)opaque;
    (void)size;
    return NULL;
}

/** @brief An out-of-memory host: JS_NewRuntime2 cannot even allocate the JSRuntime, so init fails early. */
static const JSMallocFunctions MF_ALLOCATION_FAILS = {failing_calloc, failing_malloc, plain_free, plain_realloc,
                                                      usable_size_zero};

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — no-op backend (the font registry needs one; nothing is painted here)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief No-op fill. @param argb Color. @param x X. @param y Y. @param w Width. @param h Height. @param ctx Context. */
static void noop_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/** @brief No-op copy. @param src Source. @param stride Stride. @param x X. @param y Y. @param w Width. @param h Height.
 * @param ctx Context. */
static void noop_copy(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/** @brief No-op blend. @param src Source. @param stride Stride. @param a Alpha. @param x X. @param y Y. @param w Width.
 * @param h Height. @param ctx Context. */
static void noop_blend(const void* src, int stride, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — harness
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Lines the gc_threshold sanity warning has emitted through the log sink (scenario 6). */
static int s_gc_warns = 0;

/** @brief Log sink that counts the gc_threshold-vs-memory_limit warning (and stays quiet otherwise). */
static void counting_log(const char* line)
{
    if (strstr(line, "gc_threshold is not below memory_limit"))
    {
        s_gc_warns++;
    }
}

/** @brief Records one assertion. @param ok Result. @param what Description printed either way. */
static void check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
    {
        s_failures++;
    }
}

/** @brief Returns the runtime's tracked heap size in bytes. @param rt Runtime. */
static int64_t tracked_bytes(JSRuntime* rt)
{
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(rt, &usage);
    return usage.malloc_size;
}

/** @brief Evaluates a script, reporting any exception. @param ctx Context. @param src Source. @param name Trace name. */
static bool run_js(JSContext* ctx, const char* src, const char* name)
{
    JSValue result = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    const bool ok = !JS_IsException(result);
    if (!ok)
    {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "JS exception in %s: %s\n", name, msg ? msg : "(unknown)");
        if (msg)
        {
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
    return ok;
}

/**
 * @brief Runs SRC_GROW under @p mf and returns how many bytes the runtime's accounting noticed.
 *
 * @param[in] mf  Allocator to build the runtime with.
 *
 * @return malloc_size delta across the script, or -1 if the script did not run.
 */
static int64_t measure_growth(const JSMallocFunctions* mf)
{
    JSRuntime* rt = JS_NewRuntime2(mf, NULL);
    if (!rt)
    {
        return -1;
    }
    JSContext* ctx = er_js_new_context(rt, ER_JS_INTRINSIC_EVAL);
    if (!ctx)
    {
        JS_FreeRuntime(rt);
        return -1;
    }

    /* Collect on both sides so the delta is the LIVE string and nothing else: without the first
       collection the baseline still holds context-setup garbage, which an automatic GC part-way through
       the script would reclaim — hiding the string's growth behind someone else's shrink. */
    JS_RunGC(rt);
    const int64_t before = tracked_bytes(rt);
    const bool ok = run_js(ctx, SRC_GROW, "<grow>");
    JS_RunGC(rt);
    const int64_t delta = tracked_bytes(rt) - before;

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ok ? delta : -1;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Exercises the heap-accounting probe, the default allocator, and real GC reclamation.
 *
 * @return 0 when every check passes, 1 otherwise.
 */
int main(void)
{
    static const EmbeddedRenderBackend backend = {noop_fill, noop_copy, noop_blend, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    printf("JS heap accounting mode: %s\n", er_js_usable_size_mode());

    /* --- 1. The probe classifies each allocator correctly ------------------------------------------ */
    {
        size_t reported = 0;

        JSRuntime* good = JS_NewRuntime2(er_js_default_malloc_functions(), NULL);
        check(er_js_probe_alloc_health(good, &reported) == ER_JS_ALLOC_OK, "probe: bridge default -> OK");
        check(reported >= 1000, "probe: bridge default reports >= the requested bytes");
        JS_FreeRuntime(good);

        JSRuntime* dead = JS_NewRuntime2(&MF_NO_ACCOUNTING, NULL);
        check(er_js_probe_alloc_health(dead, &reported) == ER_JS_ALLOC_NO_ACCOUNTING,
              "probe: usable_size returning 0 -> NO_ACCOUNTING");
        JS_FreeRuntime(dead);

        JSRuntime* wrong = JS_NewRuntime2(&MF_IMPLAUSIBLE, NULL);
        check(er_js_probe_alloc_health(wrong, &reported) == ER_JS_ALLOC_IMPLAUSIBLE,
              "probe: usable_size answering about another heap -> IMPLAUSIBLE");
        JS_FreeRuntime(wrong);

        /* A NULL usable_size is the same defect wearing a different hat: QuickJS silently substitutes a
           zero-returning stub, so the probe must see through the substitution. */
        JSMallocFunctions mf_null = MF_NO_ACCOUNTING;
        mf_null.js_malloc_usable_size = NULL;
        JSRuntime* null_us = JS_NewRuntime2(&mf_null, NULL);
        check(er_js_probe_alloc_health(null_us, &reported) == ER_JS_ALLOC_NO_ACCOUNTING,
              "probe: NULL usable_size -> NO_ACCOUNTING (QuickJS's silent stub)");
        JS_FreeRuntime(null_us);
    }

    /* --- 2. The default allocator accounts in bytes; the broken one does not ----------------------- */
    {
        const int64_t tracked = measure_growth(er_js_default_malloc_functions());
        char msg[128];
        snprintf(msg, sizeof msg, "accounting: %d KB live string tracked as %d KB (bridge default)",
                 GROWTH_BYTES / 1024, (int)(tracked / 1024));
        check(tracked >= GROWTH_MIN_TRACKED, msg);

        /* The defect itself, pinned down: same script, same heap traffic, invisible to the GC. */
        const int64_t untracked = measure_growth(&MF_NO_ACCOUNTING);
        check(untracked >= 0 && untracked < GROWTH_MAX_UNTRACKED,
              "accounting: the same string is invisible when usable_size returns 0 (the reported bug)");
    }

    /* --- 3. With accounting live, the AUTOMATIC collector runs and reclaims cyclic garbage --------- */
    {
        JSRuntime* rt = JS_NewRuntime2(er_js_default_malloc_functions(), NULL);
        JSContext* ctx = er_js_new_context(rt, ER_JS_INTRINSIC_EVAL);

        JS_RunGC(rt); /* baseline = the live set, so the growth measured below is the churn's alone */
        const int64_t before = tracked_bytes(rt);
        const bool ok = run_js(ctx, SRC_CYCLES, "<cycles>");
        const int64_t after = tracked_bytes(rt); /* NOT collected first: the automatic GC must have run */

        char msg[128];
        snprintf(msg, sizeof msg, "gc: cyclic garbage churned, heap grew %d KB (bounded)",
                 (int)((after - before) / 1024));
        check(ok && (after - before) < AUTO_GC_MAX_RETAINED, msg);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    /* --- 4. End to end through er_runtime, which is what hosts actually call ----------------------- */
    {
        ErRuntimeConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.screen_width = 240;
        cfg.screen_height = 240;

        /* malloc_functions = NULL is the path every host copies from the README. It must be safe on
           EVERY platform, which is why er_runtime never calls JS_NewRuntime(). */
        check(er_runtime_init(&cfg), "er_runtime: init with malloc_functions = NULL");
        check(er_runtime_gc_accounting_ok(), "er_runtime: default allocator reports working accounting");
        er_runtime_shutdown();

        /* A host that supplies its own broken allocator still gets caught (and warned about) at boot. */
        cfg.malloc_functions = &MF_NO_ACCOUNTING;
        check(er_runtime_init(&cfg), "er_runtime: init with a host allocator that has no accounting");
        check(!er_runtime_gc_accounting_ok(), "er_runtime: broken host allocator is reported, not ignored");
        er_runtime_shutdown();

        /* The verdict outlives the runtime it describes, so a later init that fails BEFORE probing must
           not still be answering for the broken runtime above. This init dies inside JS_NewRuntime2 (its
           allocator refuses the JSRuntime itself), so nothing is probed. */
        cfg.malloc_functions = &MF_ALLOCATION_FAILS;
        check(!er_runtime_init(&cfg), "er_runtime: init fails when the JS heap cannot be allocated");
        check(er_runtime_gc_accounting_ok(), "er_runtime: a failed init does not report the previous runtime's verdict");
    }

    /* --- 5. The GC floor survives QuickJS's recompute-after-every-collection ----------------------- */
    {
        ErRuntimeConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.screen_width = 240;
        cfg.screen_height = 240;

        /* Control: with no floor, the threshold is whatever the last collection left behind — live x 1.5,
           which on a small live set is a fraction of the floor the next case asks for. */
        check(er_runtime_init(&cfg), "gc floor: init without a threshold");
        check(er_runtime_load_source(SRC_CYCLES, strlen(SRC_CYCLES), "<cycles>"), "gc floor: control app ran");
        er_runtime_pump();
        const size_t unfloored = er_runtime_gc_threshold();
        check(unfloored < GC_FLOOR_BYTES, "gc floor: unconfigured, the threshold settles near the live set");
        er_runtime_shutdown();

        cfg.gc_threshold = GC_FLOOR_BYTES;
        check(er_runtime_init(&cfg), "gc floor: init with a threshold");
        check(er_runtime_gc_threshold() == GC_FLOOR_BYTES, "gc floor: the configured value is applied at init");
        check(er_runtime_load_source(SRC_CYCLES, strlen(SRC_CYCLES), "<cycles>"),
              "gc floor: app ran under the floor");
        er_runtime_pump(); /* what a host does every frame — and what re-asserts the floor */
        char msg[128];
        snprintf(msg, sizeof msg, "gc floor: still %u KB after collections, not %u KB (the recompute is undone)",
                 (unsigned)(er_runtime_gc_threshold() / 1024), (unsigned)(unfloored / 1024));
        check(er_runtime_gc_threshold() >= GC_FLOOR_BYTES, msg);

        /* Turning automatic collection off is the extreme of the same knob: the host then owns the pause
           and places it somewhere it does not show. Both halves have to work for that to be usable. */
        er_runtime_set_gc_threshold((size_t)-1);
        JSRuntime* rt = JS_GetRuntime(er_runtime_context());
        JS_RunGC(rt);
        const int64_t before = tracked_bytes(rt);
        check(er_runtime_load_source(SRC_CYCLES, strlen(SRC_CYCLES), "<cycles>"),
              "gc off: app ran with the GC disabled");
        er_runtime_pump();
        const int64_t peak = tracked_bytes(rt);
        check(peak - before > GC_OFF_MIN_GROWTH, "gc off: cyclic garbage accumulates, as asked");

        er_runtime_run_gc();
        const int64_t collected = tracked_bytes(rt);
        snprintf(msg, sizeof msg, "gc off: er_runtime_run_gc reclaimed %d KB of it",
                 (int)((peak - collected) / 1024));
        check(collected - before < CYCLES_MAX_RETAINED, msg);
        check(er_runtime_gc_threshold() == (size_t)-1, "gc off: a manual collection does not re-arm the schedule");

        /* Coming back from a disabled collector has to write a real threshold: nothing would ever run to
           recompute one, so a host that only stopped re-applying the floor would never collect again. */
        er_runtime_set_gc_threshold(0);
        check(er_runtime_gc_threshold() > 0 && er_runtime_gc_threshold() < GC_FLOOR_BYTES,
              "gc off: dropping the floor hands the schedule back to QuickJS");
        er_runtime_shutdown();
    }

    /* --- 6. The threshold-vs-limit sanity warning fires from the setter, not just from init --------- */
    {
        ErRuntimeConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.screen_width = 240;
        cfg.screen_height = 240;
        cfg.log = counting_log;
        cfg.memory_limit = 1024 * 1024;

        s_gc_warns = 0;
        check(er_runtime_init(&cfg), "gc warn: init with a memory limit and no floor");
        check(s_gc_warns == 0, "gc warn: a floor of 0 does not warn");

        /* Raising the floor past the limit at RUNTIME disarms the automatic collector just as
           thoroughly as doing it in the config, so the setter must warn the same way init does. */
        er_runtime_set_gc_threshold(2 * 1024 * 1024);
        check(s_gc_warns == 1, "gc warn: the setter warns when the floor is not below memory_limit");
        er_runtime_set_gc_threshold(256 * 1024);
        check(s_gc_warns == 1, "gc warn: a floor back under the limit is quiet");
        er_runtime_shutdown();
    }

    if (s_failures)
    {
        printf("\n%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
