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

#include "native_renderer.h"
#include "vector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 128
#define FB_H 128

/*----------------------------------------------------------------------------------------------------------------------
 - Test harness (same shape as test_vector.c: capture blit calls into a flat ARGB framebuffer)
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    uint32_t* fb;
    int fb_w;
    int fb_h;
} TestCtx;

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            if (row >= 0 && col >= 0 && row < t->fb_h && col < t->fb_w)
                t->fb[row * t->fb_w + col] = argb;
}

static void copy_cb(const void* src, int s, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

static void blend_cb(const void* src, int s, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)a;
    TestCtx* t = ctx;
    const uint32_t* px_src = src;
    const int pitch = (s > 0) ? s / (int)sizeof(uint32_t) : w;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            const int fy = y + row, fx = x + col;
            if (fy >= 0 && fx >= 0 && fy < t->fb_h && fx < t->fb_w)
                t->fb[fy * t->fb_w + fx] = px_src[row * pitch + col];
        }
}

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test entry point
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Exercises the vector edge cache (ERUI_VECTOR_EDGE_CACHE) through er_vector_render_slot().
 *
 * Verifies that:
 *   - The FIRST render of a static tape does not record (two-touch promotion), the second records, and
 *     the third replays from the cache — with every render producing pixel-identical output, including
 *     the analytic-arc route (counted on replay too).
 *   - A replay under a partial clip paints exactly the reference pixels inside the clip and nothing
 *     outside — the moving-sibling damage-rect case the cache exists for.
 *   - er_vector_store() on the slot invalidates the entry AND disarms the promotion, so a tape that
 *     changes every render (an animated dial) never records or replays.
 *   - er_vector_free() invalidates, so a re-issued slot never replays the previous node's geometry.
 *   - A node whose geometry overflows the rasterizer edge pool falls back to uncached rendering with
 *     unchanged output (and stops re-trying, i.e. builds do not climb per render).
 *
 * With the cache compiled out the counters must stay 0 and er_vector_render_slot() must still paint
 * identically to er_vector_render(); the counter assertions are gated accordingly.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    static uint32_t ref[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    er_vector_reset();
    er_vector_cache_stats_reset();
    er_vector_analytic_arc_count_reset();

    /* A mixed static tape: a filled+stroked polygon, a stroked open path with round joins, and a full
     * circle that routes through the analytic arc core. */
    const float tape[] = {/* shape 0: filled + stroked quad */
                          ER_VOP_SHAPE,
                          0,
                          ER_VOP_MOVE,
                          12,
                          12,
                          ER_VOP_LINE,
                          52,
                          16,
                          ER_VOP_LINE,
                          48,
                          52,
                          ER_VOP_LINE,
                          14,
                          44,
                          ER_VOP_CLOSE,
                          /* shape 1: open zigzag, stroked with round joins */
                          ER_VOP_SHAPE,
                          1,
                          ER_VOP_MOVE,
                          70,
                          20,
                          ER_VOP_LINE,
                          90,
                          40,
                          ER_VOP_LINE,
                          74,
                          58,
                          ER_VOP_LINE,
                          96,
                          74,
                          /* shape 2: full circle -> analytic arc route (fill + stroke) */
                          ER_VOP_SHAPE,
                          2,
                          ER_VOP_ARC,
                          40,
                          90,
                          20,
                          0.0f,
                          6.2832f,
                          0};
    const int n_tape = (int)(sizeof(tape) / sizeof(tape[0]));
    const ERVectorPaint paints[3] = {
        {0xFF3355AAu, 0xFFFFFFFFu, 3.0f, 0.0f, ER_VCAP_BUTT, ER_VJOIN_MITER, ER_VFILL_NONZERO, 0, 0},
        {0x00000000u, 0xFF22CC44u, 5.0f, 0.0f, ER_VCAP_ROUND, ER_VJOIN_ROUND, ER_VFILL_NONZERO, 0, 0},
        {0xFF884422u, 0xFFDDDDDDu, 4.0f, 0.0f, ER_VCAP_BUTT, ER_VJOIN_MITER, ER_VFILL_NONZERO, 0, 0},
    };

    const int slot = er_vector_store(-1, tape, n_tape, paints, 3, NULL, 0);
    if (slot < 0)
        return fail("setup: no storage slot");

    /* --- render #1: legacy path (two-touch: nothing recorded yet); this is the reference frame --- */
    memset(fb, 0, sizeof(fb));
    er_vector_render_slot(slot, 0, 0, 0, 0, FB_W, FB_H);
    memcpy(ref, fb, sizeof(fb));
    const uint32_t arcs_per_render = er_vector_analytic_arc_count();
    if (arcs_per_render == 0)
        return fail("setup: circle did not take the analytic arc route");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_builds() != 0 || er_vector_cache_hits() != 0)
        return fail("render #1 must neither record nor hit (two-touch)");
#endif

    /* --- render #2: records; output must not change --- */
    memset(fb, 0, sizeof(fb));
    er_vector_render_slot(slot, 0, 0, 0, 0, FB_W, FB_H);
    if (memcmp(fb, ref, sizeof(fb)) != 0)
        return fail("recording render differs from the reference frame");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_builds() != 1)
        return fail("render #2 did not record");
    if (er_vector_cache_hits() != 0)
        return fail("render #2 must not count as a hit");
#endif

    /* --- render #3: replays; output must not change, and the arc must still route analytically --- */
    memset(fb, 0, sizeof(fb));
    er_vector_render_slot(slot, 0, 0, 0, 0, FB_W, FB_H);
    if (memcmp(fb, ref, sizeof(fb)) != 0)
        return fail("replayed render differs from the reference frame");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_hits() != 1)
        return fail("render #3 did not replay from the cache");
    if (er_vector_analytic_arc_count() != 3 * arcs_per_render)
        return fail("replay did not route the circle through the analytic arc core");
#endif

    /* --- partial clip replay: exactly the reference pixels inside the clip, nothing outside --- */
    {
        const int cx0 = 30, cy0 = 24, cx1 = 84, cy1 = 70;
        memset(fb, 0, sizeof(fb));
        er_vector_render_slot(slot, 0, 0, cx0, cy0, cx1, cy1);
        for (int y = 0; y < FB_H; y++)
            for (int x = 0; x < FB_W; x++)
            {
                const int inside = (x >= cx0 && x < cx1 && y >= cy0 && y < cy1);
                const uint32_t want = inside ? ref[y * FB_W + x] : 0u;
                if (fb[y * FB_W + x] != want)
                    return fail(inside ? "partial-clip replay differs from the reference inside the clip"
                                       : "partial-clip replay painted outside the clip");
            }
    }

    /* --- er_vector_store() invalidates: new geometry must paint, and an every-render tape update
     *     (an animated node) must never record --- */
    {
        const float tape2[] = {ER_VOP_SHAPE,
                               0,
                               ER_VOP_MOVE,
                               100,
                               100,
                               ER_VOP_LINE,
                               120,
                               100,
                               ER_VOP_LINE,
                               120,
                               120,
                               ER_VOP_LINE,
                               100,
                               120,
                               ER_VOP_CLOSE};
        const ERVectorPaint p2 = {0xFFFF0000u, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_NONZERO, 0, 0};
#if ERUI_VECTOR_EDGE_CACHE
        const uint32_t builds_before = er_vector_cache_builds();
        const uint32_t hits_before = er_vector_cache_hits();
#endif
        for (int i = 0; i < 4; i++)
        {
            /* Alternate the two tapes so every render sees a fresh store — the animated pattern. */
            const int use2 = i & 1;
            if (er_vector_store(slot,
                                use2 ? tape2 : tape,
                                use2 ? (int)(sizeof(tape2) / sizeof(tape2[0])) : n_tape,
                                use2 ? &p2 : paints,
                                use2 ? 1 : 3,
                                NULL,
                                0)
                != slot)
                return fail("re-store changed the slot");
            memset(fb, 0, sizeof(fb));
            er_vector_render_slot(slot, 0, 0, 0, 0, FB_W, FB_H);
            if (use2)
            {
                if (fb[110 * FB_W + 110] != 0xFFFF0000u)
                    return fail("post-store render did not paint the new geometry");
                if (fb[90 * FB_W + 40] != 0u)
                    return fail("post-store render painted the OLD geometry (stale cache)");
            }
            else if (memcmp(fb, ref, sizeof(fb)) != 0)
                return fail("post-store render of the original tape differs from the reference");
        }
#if ERUI_VECTOR_EDGE_CACHE
        if (er_vector_cache_builds() != builds_before)
            return fail("an every-render tape update recorded (two-touch failed to protect it)");
        if (er_vector_cache_hits() != hits_before)
            return fail("an every-render tape update replayed a stale entry");
#endif
    }

    /* --- er_vector_free(): a re-issued slot must not replay the previous node's geometry --- */
    {
        /* Re-establish a cached entry for the original tape first. */
        if (er_vector_store(slot, tape, n_tape, paints, 3, NULL, 0) != slot)
            return fail("re-store changed the slot");
        er_vector_render_slot(slot, 0, 0, 0, 0, FB_W, FB_H); /* arm */
        er_vector_render_slot(slot, 0, 0, 0, 0, FB_W, FB_H); /* record */
        er_vector_free(slot);
        const float tape3[] = {ER_VOP_SHAPE,
                               0,
                               ER_VOP_MOVE,
                               4,
                               4,
                               ER_VOP_LINE,
                               24,
                               4,
                               ER_VOP_LINE,
                               24,
                               24,
                               ER_VOP_LINE,
                               4,
                               24,
                               ER_VOP_CLOSE};
        const ERVectorPaint p3 = {0xFF00FF00u, 0, 0.0f, 0.0f, 0, 0, ER_VFILL_NONZERO, 0, 0};
        const int slot2 = er_vector_store(-1, tape3, (int)(sizeof(tape3) / sizeof(tape3[0])), &p3, 1, NULL, 0);
        if (slot2 != slot)
            return fail("freed slot was not re-issued (test setup assumption)");
        memset(fb, 0, sizeof(fb));
        er_vector_render_slot(slot2, 0, 0, 0, 0, FB_W, FB_H);
        if (fb[10 * FB_W + 10] != 0xFF00FF00u)
            return fail("re-issued slot did not paint its own geometry");
        if (fb[90 * FB_W + 40] != 0u)
            return fail("re-issued slot replayed the freed node's geometry");
    }

    /* --- a moved node (new origin) misses, re-records at the new position, and paints correctly --- */
    {
        er_vector_free(slot);
        const int slot3 = er_vector_store(-1, tape, n_tape, paints, 3, NULL, 0);
        if (slot3 < 0)
            return fail("no slot for the move test");
        er_vector_render_slot(slot3, 0, 0, 0, 0, FB_W, FB_H); /* arm */
        er_vector_render_slot(slot3, 0, 0, 0, 0, FB_W, FB_H); /* record at (0,0) */
        memset(fb, 0, sizeof(fb));
        er_vector_render_slot(slot3, 6, 3, 0, 0, FB_W, FB_H); /* moved: must NOT replay (0,0) edges */
        for (int y = 0; y < FB_H - 3; y++)
            for (int x = 0; x < FB_W - 6; x++)
                if (fb[(y + 3) * FB_W + (x + 6)] != ref[y * FB_W + x])
                    return fail("moved-origin render is not the reference shifted by the move");
        er_vector_free(slot3);
    }

    /* --- geometry too big to record: identical output every render, and no cache churn --- */
    {
        /* A sharply-reversing zigzag with round joins: every join fans many triangles, overflowing the
         * rasterizer edge pool — the recording must abort (and block), never truncating harder than
         * the legacy build does. */
        static float big[512];
        int n = 0;
        big[n++] = ER_VOP_SHAPE;
        big[n++] = 0;
        big[n++] = ER_VOP_MOVE;
        big[n++] = 4.0f;
        big[n++] = 60.0f;
        for (int i = 0; i < 160; i++)
        {
            big[n++] = ER_VOP_LINE;
            big[n++] = 5.0f + (float)(i % 2) * 110.0f;
            big[n++] = 60.0f + (float)i * 0.05f;
        }
        const ERVectorPaint pb = {
            0x00000000u, 0xFF8080FFu, 6.0f, 0.0f, ER_VCAP_ROUND, ER_VJOIN_ROUND, ER_VFILL_NONZERO, 0, 0};
        const int slotb = er_vector_store(-1, big, n, &pb, 1, NULL, 0);
        if (slotb < 0)
            return fail("no slot for the overflow test");
#if ERUI_VECTOR_EDGE_CACHE
        const uint32_t builds_before = er_vector_cache_builds();
#endif
        static uint32_t first[FB_W * FB_H];
        memset(fb, 0, sizeof(fb));
        er_vector_render_slot(slotb, 0, 0, 0, 0, FB_W, FB_H);
        memcpy(first, fb, sizeof(fb));
        for (int i = 0; i < 3; i++)
        {
            memset(fb, 0, sizeof(fb));
            er_vector_render_slot(slotb, 0, 0, 0, 0, FB_W, FB_H);
            if (memcmp(fb, first, sizeof(fb)) != 0)
                return fail("overflowing node renders inconsistently across frames");
        }
#if ERUI_VECTOR_EDGE_CACHE
        if (er_vector_cache_builds() != builds_before)
            return fail("overflowing node published a cache entry");
#endif
        er_vector_free(slotb);
    }

    /* --- empty / invalid slots are safe no-ops --- */
    er_vector_render_slot(-1, 0, 0, 0, 0, FB_W, FB_H);
    er_vector_render_slot(1234, 0, 0, 0, 0, FB_W, FB_H);

    er_vector_reset();
    printf("OK\n");
    return EXIT_SUCCESS;
}
