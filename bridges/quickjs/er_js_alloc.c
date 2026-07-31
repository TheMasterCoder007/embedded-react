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
 * er_js_alloc — default JS-heap allocator with working size accounting (see er_js_alloc.h for why).
 */

#include "er_js_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation select: native usable-size call vs. size-prefix shim
 ---------------------------------------------------------------------------------------------------------------------*/

/* -DER_BRIDGE_JS_USABLE_SIZE=native|shim forces one; otherwise pick by platform. The "native" set is
 * exactly the platforms QuickJS's own cutils.h implements, plus Emscripten (whose libc does provide
 * malloc_usable_size — QuickJS just never asks for it, which is why the simulator had no GC either). */
#if !defined(ER_JS_USABLE_SIZE_NATIVE) && !defined(ER_JS_USABLE_SIZE_SHIM)
#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__) || defined(__ANDROID__) ||                             \
    defined(__CYGWIN__) || defined(__FreeBSD__) || defined(__GLIBC__) || defined(__EMSCRIPTEN__)
#define ER_JS_USABLE_SIZE_NATIVE 1
#else
#define ER_JS_USABLE_SIZE_SHIM 1
#endif
#endif

#if defined(ER_JS_USABLE_SIZE_NATIVE)
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#else
#include <malloc.h>
#endif
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Probe allocation size. Large enough that any sane allocator reports >= this many usable bytes. */
#define ER_JS_PROBE_BYTES 1000u

/** @brief Upper bound on a plausible usable_size for the probe: beyond this the answer is not our block. */
#define ER_JS_PROBE_MAX (ER_JS_PROBE_BYTES * 8u)

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — native (platform reports the usable size)
 ---------------------------------------------------------------------------------------------------------------------*/

#if defined(ER_JS_USABLE_SIZE_NATIVE)

/** @brief Zeroed JS-heap allocation. @param opaque Unused. @param count Element count. @param size Element size. */
static void* er_js_native_calloc(void* opaque, size_t count, size_t size)
{
    (void)opaque;
    return calloc(count, size);
}

/** @brief JS-heap allocation. @param opaque Unused. @param size Byte count. */
static void* er_js_native_malloc(void* opaque, size_t size)
{
    (void)opaque;
    return malloc(size);
}

/** @brief Frees a JS-heap allocation. @param opaque Unused. @param ptr Block (may be NULL). */
static void er_js_native_free(void* opaque, void* ptr)
{
    (void)opaque;
    free(ptr);
}

/** @brief Resizes a JS-heap allocation. @param opaque Unused. @param ptr Block. @param size New byte count. */
static void* er_js_native_realloc(void* opaque, void* ptr, size_t size)
{
    (void)opaque;
    return realloc(ptr, size);
}

/** @brief Reports the usable size of a JS-heap allocation. @param ptr Block. */
static size_t er_js_native_usable_size(const void* ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize((void*)ptr);
#else
    return malloc_usable_size((void*)ptr);
#endif
}

static const JSMallocFunctions s_er_js_mf = {
    er_js_native_calloc,
    er_js_native_malloc,
    er_js_native_free,
    er_js_native_realloc,
    er_js_native_usable_size,
};

#else /* ER_JS_USABLE_SIZE_SHIM */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — size-prefix shim (we report the size, so the heap underneath need not)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Per-allocation header: the requested payload size, padded to the platform's widest scalar so the
 *        payload that follows keeps malloc's alignment (8 bytes on ARM32 — QuickJS never needs more than
 *        double/pointer alignment).
 */
typedef union
{
    size_t size;
    void* p;
    double d;
    long long ll;
} ErJsBlockHeader;

#define ER_JS_HDR sizeof(ErJsBlockHeader)

/** @brief Payload pointer for a header. @param h Header. */
static void* er_js_payload(ErJsBlockHeader* h)
{
    return (void*)((uint8_t*)h + ER_JS_HDR);
}

/** @brief Header for a payload pointer. @param ptr Payload (must be non-NULL and ours). */
static ErJsBlockHeader* er_js_header(const void* ptr)
{
    return (ErJsBlockHeader*)(void*)((uint8_t*)(uintptr_t)ptr - ER_JS_HDR);
}

/** @brief JS-heap allocation with a size prefix. @param opaque Unused. @param size Byte count. */
static void* er_js_shim_malloc(void* opaque, size_t size)
{
    (void)opaque;
    if (size == 0 || size > (size_t)-1 - ER_JS_HDR)
    {
        return NULL;
    }
    ErJsBlockHeader* h = (ErJsBlockHeader*)malloc(size + ER_JS_HDR);
    if (!h)
    {
        return NULL;
    }
    h->size = size;
    return er_js_payload(h);
}

/** @brief Zeroed shim allocation. @param opaque Unused. @param count Element count. @param size Element size. */
static void* er_js_shim_calloc(void* opaque, size_t count, size_t size)
{
    /* QuickJS checks the multiply before calling, but this is a public entry point — check anyway. */
    if (count != 0 && size > (size_t)-1 / count)
    {
        return NULL;
    }
    const size_t total = count * size;
    void* p = er_js_shim_malloc(opaque, total);
    if (p)
    {
        memset(p, 0, total);
    }
    return p;
}

/** @brief Frees a shim allocation. @param opaque Unused. @param ptr Payload (may be NULL). */
static void er_js_shim_free(void* opaque, void* ptr)
{
    (void)opaque;
    if (ptr)
    {
        free(er_js_header(ptr));
    }
}

/** @brief Resizes a shim allocation. @param opaque Unused. @param ptr Payload. @param size New byte count. */
static void* er_js_shim_realloc(void* opaque, void* ptr, size_t size)
{
    if (!ptr)
    {
        return er_js_shim_malloc(opaque, size);
    }
    if (size == 0)
    {
        er_js_shim_free(opaque, ptr);
        return NULL;
    }
    if (size > (size_t)-1 - ER_JS_HDR)
    {
        return NULL;
    }
    ErJsBlockHeader* h = (ErJsBlockHeader*)realloc(er_js_header(ptr), size + ER_JS_HDR);
    if (!h)
    {
        return NULL;
    }
    h->size = size;
    return er_js_payload(h);
}

/**
 * @brief Reports the payload size recorded at allocation time.
 *
 * Slightly under-reports versus the heap's true block size (the allocator's own rounding is invisible to
 * us), which is the safe direction: the GC fires marginally early rather than never.
 *
 * @param[in] ptr  Payload pointer.
 *
 * @return Recorded byte count, or 0 for NULL.
 */
static size_t er_js_shim_usable_size(const void* ptr)
{
    return ptr ? er_js_header(ptr)->size : 0u;
}

static const JSMallocFunctions s_er_js_mf = {
    er_js_shim_calloc,
    er_js_shim_malloc,
    er_js_shim_free,
    er_js_shim_realloc,
    er_js_shim_usable_size,
};

#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

const JSMallocFunctions* er_js_default_malloc_functions(void)
{
    return &s_er_js_mf;
}

const char* er_js_usable_size_mode(void)
{
#if defined(ER_JS_USABLE_SIZE_NATIVE)
    return "native";
#else
    return "shim";
#endif
}

ErJsAllocHealth er_js_probe_alloc_health(JSRuntime* rt, size_t* out_reported)
{
    if (out_reported)
    {
        *out_reported = 0;
    }
    if (!rt)
    {
        return ER_JS_ALLOC_OK;
    }

    /* Go through the runtime, not the raw JSMallocFunctions: that is what the GC's accounting uses, and it
       includes QuickJS's silent swap-in of a zero-returning stub when a host leaves js_malloc_usable_size
       NULL. The alloc/free pair is symmetric, so malloc_size is unchanged on return. */
    void* probe = js_malloc_rt(rt, ER_JS_PROBE_BYTES);
    if (!probe)
    {
        return ER_JS_ALLOC_OK;
    }
    const size_t reported = js_malloc_usable_size_rt(rt, probe);
    js_free_rt(rt, probe);

    if (out_reported)
    {
        *out_reported = reported;
    }
    if (reported == 0)
    {
        return ER_JS_ALLOC_NO_ACCOUNTING;
    }
    if (reported < ER_JS_PROBE_BYTES || reported > ER_JS_PROBE_MAX)
    {
        return ER_JS_ALLOC_IMPLAUSIBLE;
    }
    return ER_JS_ALLOC_OK;
}
