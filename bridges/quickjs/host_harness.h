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

#ifndef EMBEDDED_REACT_HOST_HARNESS_H
#define EMBEDDED_REACT_HOST_HARNESS_H

/*
 * Shared scaffolding for the bridge's host-side executables (smoke, runtest, splittest, gctest, compile).
 * Each is its own tiny main() and each grew its own copy of the same two things; a "variant" that skipped
 * the null terminator is exactly the kind of difference that reads as intentional and is not.
 *
 * Deliberately depends on nothing but the C library — compile_main links only QuickJS, with no engine
 * headers on its include path, so the no-op render callbacks below spell their signatures in plain C
 * types and leave the EmbeddedRenderBackend struct literal to the caller.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Reads a whole file into a newly allocated buffer, with a null terminator appended.
 *
 * The terminator is always there, and is not counted in @p out_length — so the same call serves a JS source
 * file read as a string and a binary container read as bytes.
 *
 * @param[in]  path        Filesystem path to read.
 * @param[out] out_length  Receives the byte length, excluding the appended terminator.
 *
 * @return Heap buffer the caller must free(), or NULL if the file could not be read.
 */
static inline void* er_host_read_file(const char* path, size_t* out_length)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0)
    {
        fclose(file);
        return NULL;
    }
    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer)
    {
        fclose(file);
        return NULL;
    }
    const size_t bytes_read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[bytes_read] = '\0';
    *out_length = bytes_read;
    return buffer;
}

/** @brief No-op fill callback: a headless harness runs the engine with nothing to draw on. */
static inline void er_host_noop_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/** @brief No-op copy callback. */
static inline void er_host_noop_copy(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/** @brief No-op blend callback. */
static inline void
er_host_noop_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/** @brief Initialiser for an EmbeddedRenderBackend that draws nothing (no wait / frame / ctx hooks). */
#define ER_HOST_NOOP_BACKEND {er_host_noop_fill, er_host_noop_copy, er_host_noop_blend, NULL, NULL, NULL}

#endif
