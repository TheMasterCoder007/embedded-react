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
 * Render-worker fork-join (multi-core plan, Phase 1).
 *
 * Single-worker builds (ERUI_RENDER_WORKERS == 1, the default): er_parallel_for must run the job
 * inline on the calling thread and installing workers must be a no-op.
 *
 * Multi-worker builds: pthread workers service the EmbeddedRenderWorkers contract, and the job
 * proves PER-WORKER CONTEXT ISOLATION through the real engine accessors — every worker sets a
 * worker-distinct draw alpha and clip rect, all workers rendezvous so every set happens before
 * any read-back, then each reads its own state back. Shared contexts would fail deterministically.
 */

#define _POSIX_C_SOURCE 200809L

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Minimal thread-safe test backend (pure per-pixel writes; workers touch disjoint rows)
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 64
#define FB_H 64

static uint32_t s_fb[FB_W * FB_H];

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint8_t a = (uint8_t)((argb >> 24) & 0xFFU);
    if (a == 0U)
        return;
    const uint8_t r = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * a / 255U);
    const uint8_t g = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * a / 255U);
    const uint8_t b = (uint8_t)((uint32_t)(argb & 0xFFU) * a / 255U);
    const uint32_t premul = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            if (col >= 0 && col < FB_W && row >= 0 && row < FB_H)
                s_fb[row * FB_W + col] = premul;
}

static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    if (alpha == 0U)
        return;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* sr = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int bx = x + col;
            const int by = y + row;
            if (bx < 0 || bx >= FB_W || by < 0 || by >= FB_H)
                continue;
            uint32_t sp = sr[col];
            if (alpha < 255U)
                sp = ((uint32_t)((sp >> 24) & 0xFFU) * alpha / 255U << 24)
                     | ((uint32_t)((sp >> 16) & 0xFFU) * alpha / 255U << 16)
                     | ((uint32_t)((sp >> 8) & 0xFFU) * alpha / 255U << 8) | (uint32_t)(sp & 0xFFU) * alpha / 255U;
            const uint8_t sa = (uint8_t)((sp >> 24) & 0xFFU);
            if (sa == 0U)
                continue;
            if (sa == 255U)
            {
                s_fb[by * FB_W + bx] = sp;
                continue;
            }
            const uint32_t d = s_fb[by * FB_W + bx];
            const uint8_t inv = (uint8_t)(255U - sa);
            const uint8_t oa = (uint8_t)(sa + (uint32_t)((d >> 24) & 0xFFU) * inv / 255U);
            const uint8_t or_ = (uint8_t)(((sp >> 16) & 0xFFU) + (uint32_t)((d >> 16) & 0xFFU) * inv / 255U);
            const uint8_t og = (uint8_t)(((sp >> 8) & 0xFFU) + (uint32_t)((d >> 8) & 0xFFU) * inv / 255U);
            const uint8_t ob = (uint8_t)((sp & 0xFFU) + (uint32_t)(d & 0xFFU) * inv / 255U);
            s_fb[by * FB_W + bx] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
        }
    }
}

static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    blend_cb(src, stride, 255U, x, y, w, h, ctx);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Prints a failure message to stderr and returns EXIT_FAILURE. */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Job shared by both build variants
 ---------------------------------------------------------------------------------------------------------------------*/

#define NW ERUI_RENDER_WORKERS

static volatile int s_ran[NW];      /* how many times each worker id ran */
static volatile int s_set_phase[NW]; /* rendezvous: worker w has finished its set phase */
static volatile int s_alpha_ok[NW];
static volatile int s_clip_ok[NW];
static int s_active;                 /* workers participating in the current fork */

/** @brief One worker's share: set distinct state, rendezvous, read it back. */
static void job(int worker, void* arg)
{
    (void)arg;
    s_ran[worker]++;

    er_set_draw_alpha((uint8_t)(100 + worker));
    er_push_clip_rect(worker * 10, worker * 10 + 1, 5 + worker, 6 + worker);

    /* Rendezvous: every participating worker completes its SET phase before any reads back, so
     * a shared context would deterministically clobber someone's state. */
    s_set_phase[worker] = 1;
    for (int w = 0; w < s_active; w++)
        while (!s_set_phase[w])
        {
        }

    s_alpha_ok[worker] = (er_get_draw_alpha() == (uint8_t)(100 + worker));
    int cx, cy, cw, ch;
    s_clip_ok[worker] = er_get_clip_rect(&cx, &cy, &cw, &ch) && cx == worker * 10 && cy == worker * 10 + 1
                        && cw == 5 + worker && ch == 6 + worker;

    er_pop_clip_rect();
    er_set_draw_alpha(255U);
}

#if ERUI_RENDER_WORKERS > 1

/*----------------------------------------------------------------------------------------------------------------------
 - pthread worker glue (a reference host implementation of the EmbeddedRenderWorkers contract)
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int go;   /* a dispatch is pending */
    int stop; /* shut the worker down */
} Worker;

static Worker s_workers[NW]; /* index 0 unused (worker 0 is the render thread) */
static pthread_mutex_t s_done_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_done_cond = PTHREAD_COND_INITIALIZER;
static int s_done_count = 0;
static int s_dispatched = 0;

static void* worker_main(void* arg)
{
    Worker* w = (Worker*)arg;
    const int idx = (int)(w - s_workers);
    for (;;)
    {
        pthread_mutex_lock(&w->mtx);
        while (!w->go && !w->stop)
            pthread_cond_wait(&w->cond, &w->mtx);
        const int stop = w->stop;
        w->go = 0;
        pthread_mutex_unlock(&w->mtx);
        if (stop)
            return NULL;

        er_render_worker_exec(idx);

        pthread_mutex_lock(&s_done_mtx);
        s_done_count++;
        pthread_cond_signal(&s_done_cond);
        pthread_mutex_unlock(&s_done_mtx);
    }
}

static void host_dispatch(int worker, void* ctx)
{
    (void)ctx;
    Worker* w = &s_workers[worker];
    pthread_mutex_lock(&w->mtx);
    w->go = 1;
    s_dispatched++;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mtx);
}

static void host_sync(void* ctx)
{
    (void)ctx;
    pthread_mutex_lock(&s_done_mtx);
    while (s_done_count < s_dispatched)
        pthread_cond_wait(&s_done_cond, &s_done_mtx);
    s_done_count = 0;
    s_dispatched = 0;
    pthread_mutex_unlock(&s_done_mtx);
}

static int host_worker_id(void)
{
    const pthread_t self = pthread_self();
    for (int k = 1; k < NW; k++)
        if (pthread_equal(self, s_workers[k].thread))
            return k;
    return 0; /* the render thread */
}

#endif /* ERUI_RENDER_WORKERS > 1 */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    embedded_renderer_set_backend(NULL); /* initialises every worker's render contexts */

    /* ---- Inline path: with no workers installed a fork is exactly fn(0). ---- */
    s_active = 1;
    er_parallel_for(job, NULL);
    if (s_ran[0] != 1)
        return fail("uninstalled: job must run exactly once on worker 0");
    if (!s_alpha_ok[0] || !s_clip_ok[0])
        return fail("uninstalled: worker 0 context state readback failed");
    if (er_render_workers_active() != 1)
        return fail("uninstalled: active workers must be 1");

#if ERUI_RENDER_WORKERS > 1
    /* ---- Real threads: install pthread workers and fork across all of them. ---- */
    for (int k = 1; k < NW; k++)
    {
        pthread_mutex_init(&s_workers[k].mtx, NULL);
        pthread_cond_init(&s_workers[k].cond, NULL);
        s_workers[k].go = 0;
        s_workers[k].stop = 0;
        if (pthread_create(&s_workers[k].thread, NULL, worker_main, &s_workers[k]) != 0)
            return fail("pthread_create failed");
    }

    EmbeddedRenderWorkers hooks = {
        .count = NW,
        .dispatch = host_dispatch,
        .sync = host_sync,
        .worker_id = host_worker_id,
        .ctx = NULL,
    };
    embedded_renderer_set_workers(&hooks);
    if (er_render_workers_active() != NW)
        return fail("installed: active workers must equal the build cap");

    for (int w = 0; w < NW; w++)
    {
        s_ran[w] = 0;
        s_set_phase[w] = 0;
        s_alpha_ok[w] = 0;
        s_clip_ok[w] = 0;
    }
    s_active = NW;
    er_parallel_for(job, NULL);

    for (int w = 0; w < NW; w++)
    {
        if (s_ran[w] != 1)
            return fail("parallel: every worker must run its share exactly once");
        if (!s_alpha_ok[w])
            return fail("parallel: draw-alpha context bled between workers");
        if (!s_clip_ok[w])
            return fail("parallel: clip-stack context bled between workers");
    }

    /* ---- Fork again: the machinery must be reusable frame after frame. ---- */
    for (int w = 0; w < NW; w++)
        s_set_phase[w] = 0;
    er_parallel_for(job, NULL);
    for (int w = 0; w < NW; w++)
        if (s_ran[w] != 2)
            return fail("parallel: second fork must run every worker again");

    /* ================================================================================
     * Full-commit equivalence: a scene rendered by the sliced parallel fork must be
     * byte-identical to the same scene rendered single-core. The scene straddles the
     * slice boundary with an opacity group and a scaled transform so cross-slice
     * composites and captures are exercised, not just plain fills.
     * ================================================================================ */
    {
        static const EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
        embedded_renderer_set_backend(&be); /* re-inits worker contexts; workers stay installed */

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = {0};
        rp.left = rp.top = rp.right = rp.bottom = ER_LAYOUT_AUTO;
        rp.min_width = rp.max_width = rp.min_height = rp.max_height = ER_LAYOUT_AUTO;
        rp.padding = rp.padding_left = rp.padding_top = ER_LAYOUT_AUTO;
        rp.padding_right = rp.padding_bottom = ER_LAYOUT_AUTO;
        rp.padding_horizontal = rp.padding_vertical = ER_LAYOUT_AUTO;
        rp.margin = rp.margin_left = rp.margin_top = ER_LAYOUT_AUTO;
        rp.margin_right = rp.margin_bottom = ER_LAYOUT_AUTO;
        rp.gap = rp.row_gap = rp.column_gap = ER_LAYOUT_AUTO;
        rp.flex_basis = ER_LAYOUT_AUTO;
        rp.opacity = 255U;
        const ERProps base = rp; /* AUTO-initialised template for the children */
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFF101828U;
        er_node_set_props(root, &rp);
        er_tree_set_root(root);

        /* A 40px-tall tile whose color changes drive the partial-damage round below. */
        ERNode* tile = er_node_create(ER_NODE_VIEW);
        ERProps tp = base;
        tp.position = ER_POS_ABSOLUTE;
        tp.left = 2;
        tp.top = 2;
        tp.width = 14;
        tp.height = 40;
        tp.background_color = 0xFFB91C1CU;
        tp.border_radius = 4;
        er_node_set_props(tile, &tp);
        er_tree_append_child(root, tile);

        /* Translucent group straddling the slice boundary (rows 20-59), with a nested
         * translucent child: cross-slice strip composites at depth 2. */
        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps gp = base;
        gp.position = ER_POS_ABSOLUTE;
        gp.left = 20;
        gp.top = 20;
        gp.width = 30;
        gp.height = 40;
        gp.opacity = 128U;
        gp.background_color = 0xFF0E7490U;
        er_node_set_props(group, &gp);
        ERNode* inner = er_node_create(ER_NODE_VIEW);
        ERProps ip = base;
        ip.position = ER_POS_ABSOLUTE;
        ip.left = 6;
        ip.top = 10;
        ip.width = 18;
        ip.height = 20;
        ip.opacity = 200U;
        ip.background_color = 0xFFF59E0BU;
        ip.border_radius = 6;
        er_node_set_props(inner, &ip);
        er_tree_append_child(group, inner);
        er_tree_append_child(root, group);

        /* A scaled node straddling the boundary: cross-slice transform capture + emit
         * (a no-op scale on TRANSLATE_ONLY builds — equivalence holds either way). */
        ERNode* scaled = er_node_create(ER_NODE_VIEW);
        ERProps sp2 = base;
        sp2.position = ER_POS_ABSOLUTE;
        sp2.left = 52;
        sp2.top = 26;
        sp2.width = 8;
        sp2.height = 12;
        sp2.background_color = 0xFF15803DU;
        sp2.transform_scale_x = 1.5f;
        sp2.transform_scale_y = 1.5f;
        er_node_set_props(scaled, &sp2);
        er_tree_append_child(root, scaled);

        const uint32_t forks_before = er_parallel_frames();
        er_commit(); /* full repaint, sliced across the workers */
        if (er_parallel_frames() != forks_before + 1)
            return fail("equivalence: first commit did not take the parallel fork");

        static uint32_t fb_parallel[FB_W * FB_H];
        memcpy(fb_parallel, s_fb, sizeof(s_fb));

        /* Same scene, single-core. */
        embedded_renderer_set_workers(NULL);
        memset(s_fb, 0, sizeof(s_fb));
        er_force_full_repaint();
        er_commit();
        if (er_parallel_frames() != forks_before + 1)
            return fail("equivalence: serial commit must not fork");
        if (memcmp(fb_parallel, s_fb, sizeof(s_fb)) != 0)
            return fail("equivalence: parallel and serial framebuffers differ (full repaint)");

        /* ---- Partial damage: a prop change rendered parallel vs serial. ---- */
        embedded_renderer_set_workers(&hooks);
        tp.background_color = 0xFF7E22CEU;
        er_node_set_props(tile, &tp);
        er_commit(); /* damage-clipped (~40 rows), still forks */
        if (er_parallel_frames() != forks_before + 2)
            return fail("equivalence: damage-clipped commit did not take the parallel fork");
        memcpy(fb_parallel, s_fb, sizeof(s_fb));

        embedded_renderer_set_workers(NULL);
        tp.background_color = 0xFFB91C1CU; /* back... */
        er_node_set_props(tile, &tp);
        er_commit();
        tp.background_color = 0xFF7E22CEU; /* ...and forward again, serial this time */
        er_node_set_props(tile, &tp);
        er_commit();
        if (memcmp(fb_parallel, s_fb, sizeof(s_fb)) != 0)
            return fail("equivalence: parallel and serial framebuffers differ (partial damage)");

        embedded_renderer_set_workers(&hooks); /* restore for the uninstall test below */
    }

    /* ---- Uninstall: back to the exact single-core path. ---- */
    embedded_renderer_set_workers(NULL);
    if (er_render_workers_active() != 1)
        return fail("uninstall: active workers must return to 1");
    s_active = 1;
    s_set_phase[0] = 0;
    er_parallel_for(job, NULL);
    if (s_ran[0] != 3)
        return fail("uninstall: job must run inline on worker 0 again");

    for (int k = 1; k < NW; k++)
    {
        pthread_mutex_lock(&s_workers[k].mtx);
        s_workers[k].stop = 1;
        pthread_cond_signal(&s_workers[k].cond);
        pthread_mutex_unlock(&s_workers[k].mtx);
        pthread_join(s_workers[k].thread, NULL);
    }
    printf("PASS: fork-join across %d workers with isolated contexts\n", NW);
#else
    printf("PASS: single-worker build runs fork-join inline\n");
#endif

    return EXIT_SUCCESS;
}
