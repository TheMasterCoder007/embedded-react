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

#ifndef ER_JS_ALLOC_H
#define ER_JS_ALLOC_H

/*
 * er_js_alloc — the JS-heap allocator the bridge installs when a host supplies none.
 *
 * WHY THIS EXISTS. QuickJS drives its garbage collector entirely off byte accounting: every
 * allocation adds js_malloc_usable_size(ptr) + MALLOC_OVERHEAD to JSMallocState.malloc_size, and the
 * GC only runs when that crosses malloc_gc_threshold. QuickJS's own default (cutils.h
 * js__malloc_usable_size) implements __APPLE__, _WIN32 and the glibc/Linux/BSD family, and returns 0
 * for EVERYTHING ELSE — including bare-metal arm-none-eabi/newlib and Emscripten. With that default,
 * malloc_size degenerates to 8 bytes per live allocation, so:
 *
 *   - the GC effectively never runs (JS garbage grows until the heap is exhausted), and
 *   - JS_SetMemoryLimit (ErRuntimeConfig.memory_limit) can no longer cap the heap in bytes.
 *
 * Both failures are silent and platform-dependent, and look exactly like a leak in the app or engine.
 * So the bridge never uses the QuickJS default: er_runtime always creates its runtime with
 * JS_NewRuntime2, passing either the host's JSMallocFunctions or the ones below.
 *
 * TWO IMPLEMENTATIONS, chosen at compile time (override with -DER_BRIDGE_JS_USABLE_SIZE=native|shim):
 *
 *   native — Apple / Win32 / glibc-family / Emscripten: plain malloc + the platform's usable-size
 *            call. Zero overhead, identical to what QuickJS does on those hosts today.
 *   shim   — everywhere else (bare metal): malloc with a size prefix, so usable_size is answered from
 *            our own header. Costs one pointer-aligned word per allocation (8 bytes on ARM32,
 *            on par with QuickJS's own MALLOC_OVERHEAD) and is correct no matter which heap the
 *            firmware wired behind malloc.
 *
 * The shim is deliberately the bare-metal DEFAULT even though newlib does declare
 * malloc_usable_size(): a host that replaces malloc with its own heap (tlsf, FreeRTOS, a static pool)
 * usually leaves newlib's malloc_usable_size in place, which would then read a header that isn't
 * there. Hosts that know their heap can pass the zero-overhead call via
 * ErRuntimeConfig.malloc_functions (see examples/esp32/esp32-s3/main/main.c) or force
 * -DER_BRIDGE_JS_USABLE_SIZE=native.
 */

#include "quickjs.h"

#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Verdict from er_js_probe_alloc_health — is the runtime's byte accounting usable? */
typedef enum
{
    ER_JS_ALLOC_OK = 0,        /**< usable_size reports a plausible byte count: GC + memory_limit work. */
    ER_JS_ALLOC_NO_ACCOUNTING, /**< usable_size reported 0: GC and memory_limit are both dead. */
    ER_JS_ALLOC_IMPLAUSIBLE,   /**< usable_size answered, but nowhere near the requested size — typically a
                                    host whose malloc was replaced (tlsf/pool) while usable_size was not,
                                    so it is reading a header that does not belong to the block. */
} ErJsAllocHealth;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Returns the bridge's default JS-heap allocator (never NULL) — malloc/calloc/realloc/free plus a
 *        js_malloc_usable_size that actually reports bytes on every supported platform.
 *
 * Used by er_runtime_init when ErRuntimeConfig.malloc_functions is NULL. The returned pointer has static
 * storage duration, so it satisfies QuickJS's "must outlive the runtime" requirement.
 *
 * @return Static JSMallocFunctions with working size accounting.
 */
const JSMallocFunctions* er_js_default_malloc_functions(void);

/** @brief Returns which usable-size implementation was compiled in: "native" or "shim" (for logs/tests). */
const char* er_js_usable_size_mode(void);

/**
 * @brief Checks whether @p rt can actually account for heap bytes, by allocating a probe block through the
 *        runtime's own allocator and asking it for the block's usable size.
 *
 * This tests exactly what the GC sees — including QuickJS's silent substitution of a zero-returning stub
 * when a host passes a JSMallocFunctions with a NULL js_malloc_usable_size. Cheap enough to run at init
 * (one allocation). If the probe allocation itself fails, the result is ER_JS_ALLOC_OK: nothing can be
 * concluded about accounting, and a runtime that cannot allocate 1 KB at boot has a different problem.
 *
 * @param[in]  rt            Runtime to probe.
 * @param[out] out_reported  Optional: receives the size the runtime reported for the probe block.
 *
 * @return The verdict (see ErJsAllocHealth).
 */
ErJsAllocHealth er_js_probe_alloc_health(JSRuntime* rt, size_t* out_reported);

#endif
