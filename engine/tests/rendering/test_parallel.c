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

#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Minimal thread-safe test backend (pure per-pixel writes; workers touch disjoint rows)
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 64
#define FRAMEBUFFER_HEIGHT 64

static uint32_t s_framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];

static void fill_cb(uint32_t argb, int x, int y, int worker_slot, int height, void* ctx)
{
    (void)ctx;
    const uint8_t alpha = (uint8_t)((argb >> 24) & 0xFFU);
    if (alpha == 0U)
        return;
    const uint8_t red = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * alpha / ER_CHANNEL_MAX);
    const uint8_t green = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * alpha / ER_CHANNEL_MAX);
    const uint8_t barrier = (uint8_t)((uint32_t)(argb & 0xFFU) * alpha / ER_CHANNEL_MAX);
    const uint32_t premultiplied = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | barrier;
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + worker_slot; column++)
            if (column >= 0 && column < FRAMEBUFFER_WIDTH && row >= 0 && row < FRAMEBUFFER_HEIGHT)
                s_framebuffer[row * FRAMEBUFFER_WIDTH + column] = premultiplied;
}

static void
blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int worker_slot, int height, void* ctx)
{
    (void)ctx;
    if (alpha == 0U)
        return;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < worker_slot; column++)
        {
            const int pixel_x = x + column;
            const int pixel_y = y + row;
            if (pixel_x < 0 || pixel_x >= FRAMEBUFFER_WIDTH || pixel_y < 0 || pixel_y >= FRAMEBUFFER_HEIGHT)
                continue;
            uint32_t source_pixel = source_row[column];
            if (alpha < ER_ALPHA_OPAQUE)
                source_pixel = ((uint32_t)((source_pixel >> 24) & 0xFFU) * alpha / ER_CHANNEL_MAX << 24)
                               | ((uint32_t)((source_pixel >> 16) & 0xFFU) * alpha / ER_CHANNEL_MAX << 16)
                               | ((uint32_t)((source_pixel >> 8) & 0xFFU) * alpha / ER_CHANNEL_MAX << 8)
                               | (uint32_t)(source_pixel & 0xFFU) * alpha / ER_CHANNEL_MAX;
            const uint8_t source_alpha = (uint8_t)((source_pixel >> 24) & 0xFFU);
            if (source_alpha == 0U)
                continue;
            if (source_alpha == ER_ALPHA_OPAQUE)
            {
                s_framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] = source_pixel;
                continue;
            }
            const uint32_t destination_pixel = s_framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x];
            const uint8_t inverse_alpha = (uint8_t)(ER_CHANNEL_MAX - source_alpha);
            const uint8_t out_alpha =
                (uint8_t)(source_alpha
                          + (uint32_t)((destination_pixel >> 24) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            const uint8_t out_red =
                (uint8_t)(((source_pixel >> 16) & 0xFFU)
                          + (uint32_t)((destination_pixel >> 16) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            const uint8_t out_green =
                (uint8_t)(((source_pixel >> 8) & 0xFFU)
                          + (uint32_t)((destination_pixel >> 8) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            const uint8_t out_blue =
                (uint8_t)((source_pixel & 0xFFU)
                          + (uint32_t)(destination_pixel & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            s_framebuffer[pixel_y * FRAMEBUFFER_WIDTH + pixel_x] =
                ((uint32_t)out_alpha << 24) | ((uint32_t)out_red << 16) | ((uint32_t)out_green << 8) | out_blue;
        }
    }
}

static void copy_cb(const void* source, int stride, int x, int y, int worker_slot, int height, void* ctx)
{
    blend_cb(source, stride, 255U, x, y, worker_slot, height, ctx);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Prints a failure message to stderr and returns EXIT_FAILURE. */
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Job shared by both build variants
 ---------------------------------------------------------------------------------------------------------------------*/

#define WORKER_COUNT ERUI_RENDER_WORKERS

/* A portable generation-counting barrier (pthread_barrier_t is absent on some platforms, e.g.
 * macOS). Every access is under the mutex, so the rendezvous is well-defined under the C memory
 * model — no volatile spin. The generation flip lets one instance be reused across forks. */
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int arrived;
    unsigned generation;
} TestBarrier;

static TestBarrier s_job_barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

/** @brief Blocks until `thread_count` threads have called it; then all proceed. */
static void barrier_wait(TestBarrier* barrier, int thread_count)
{
    pthread_mutex_lock(&barrier->mutex);
    const unsigned generation = barrier->generation;
    if (++barrier->arrived >= thread_count)
    {
        barrier->arrived = 0;
        barrier->generation++;
        pthread_cond_broadcast(&barrier->condition);
    }
    else
    {
        while (generation == barrier->generation)
            pthread_cond_wait(&barrier->condition, &barrier->mutex);
    }
    pthread_mutex_unlock(&barrier->mutex);
}

/* Each worker writes only its own index; the main thread reads these only AFTER er_parallel_for's
 * sync()/join, which establishes happens-before — so plain ints, no volatile, no data race. */
static int s_ran[WORKER_COUNT]; /* how many times each worker id ran */
static int s_alpha_ok[WORKER_COUNT];
static int s_clip_ok[WORKER_COUNT];
static int s_active; /* workers participating in the current fork (barrier participant count) */

/** @brief One worker's share: set distinct state, rendezvous, read it back. */
static void job(int worker, void* context)
{
    (void)context;
    s_ran[worker]++;

    er_set_draw_alpha((uint8_t)(100 + worker));
    er_push_clip_rect(worker * 10, worker * 10 + 1, 5 + worker, 6 + worker);

    /* Rendezvous: every participating worker completes its SET phase before any reads back, so
     * a shared context would deterministically clobber someone's state. */
    barrier_wait(&s_job_barrier, s_active);

    s_alpha_ok[worker] = (er_get_draw_alpha() == (uint8_t)(100 + worker));
    int clip_x, clip_y, clip_width, clip_height;
    s_clip_ok[worker] = er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height) && clip_x == worker * 10
                        && clip_y == worker * 10 + 1 && clip_width == 5 + worker && clip_height == 6 + worker;

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
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int pending; /* a dispatch is pending */
    int stop;    /* shut the worker down */
} Worker;

static Worker s_workers[WORKER_COUNT]; /* index 0 unused (worker 0 is the render thread) */
static pthread_mutex_t s_done_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_done_condition = PTHREAD_COND_INITIALIZER;
static int s_done_count = 0;
static int s_dispatched = 0;

static void* worker_main(void* argument)
{
    Worker* worker_slot = (Worker*)argument;
    const int worker_index = (int)(worker_slot - s_workers);
    for (;;)
    {
        pthread_mutex_lock(&worker_slot->mutex);
        while (!worker_slot->pending && !worker_slot->stop)
            pthread_cond_wait(&worker_slot->condition, &worker_slot->mutex);
        const int stop = worker_slot->stop;
        worker_slot->pending = 0;
        pthread_mutex_unlock(&worker_slot->mutex);
        if (stop)
            return NULL;

        er_render_worker_execute(worker_index);

        pthread_mutex_lock(&s_done_mutex);
        s_done_count++;
        pthread_cond_signal(&s_done_condition);
        pthread_mutex_unlock(&s_done_mutex);
    }
}

static void host_dispatch(int worker, void* ctx)
{
    (void)ctx;
    Worker* worker_slot = &s_workers[worker];
    pthread_mutex_lock(&worker_slot->mutex);
    worker_slot->pending = 1;
    s_dispatched++;
    pthread_cond_signal(&worker_slot->condition);
    pthread_mutex_unlock(&worker_slot->mutex);
}

static void host_sync(void* ctx)
{
    (void)ctx;
    pthread_mutex_lock(&s_done_mutex);
    while (s_done_count < s_dispatched)
        pthread_cond_wait(&s_done_condition, &s_done_mutex);
    s_done_count = 0;
    s_dispatched = 0;
    pthread_mutex_unlock(&s_done_mutex);
}

static int host_worker_id(void)
{
    const pthread_t self = pthread_self();
    for (int worker_index = 1; worker_index < WORKER_COUNT; worker_index++)
        if (pthread_equal(self, s_workers[worker_index].thread))
            return worker_index;
    return 0; /* the render thread */
}

/** @brief Props with every layout field AUTO and full opacity, the base every node below starts from. */
static ERProps auto_props(void)
{
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.padding_horizontal = props.padding_vertical = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/**
 * @brief Empties the scene and installs a full-framebuffer root.
 *
 * @return The root.
 */
static ERNode* new_scene(void)
{
    er_reset();
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = auto_props();
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    root_props.background_color = 0xFF101828U;
    er_node_set_props(root, &root_props);
    er_tree_set_root(root);
    return root;
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
    for (int worker_index = 1; worker_index < WORKER_COUNT; worker_index++)
    {
        pthread_mutex_init(&s_workers[worker_index].mutex, NULL);
        pthread_cond_init(&s_workers[worker_index].condition, NULL);
        s_workers[worker_index].pending = 0;
        s_workers[worker_index].stop = 0;
        if (pthread_create(&s_workers[worker_index].thread, NULL, worker_main, &s_workers[worker_index]) != 0)
            return fail("pthread_create failed");
    }

    EmbeddedRenderWorkers hooks = {
        .count = WORKER_COUNT,
        .dispatch = host_dispatch,
        .sync = host_sync,
        .worker_id = host_worker_id,
        .context = NULL,
    };
    embedded_renderer_set_workers(&hooks);
    if (er_render_workers_active() != WORKER_COUNT)
        return fail("installed: active workers must equal the build cap");

    for (int worker_slot = 0; worker_slot < WORKER_COUNT; worker_slot++)
    {
        s_ran[worker_slot] = 0;
        s_alpha_ok[worker_slot] = 0;
        s_clip_ok[worker_slot] = 0;
    }
    s_active = WORKER_COUNT;
    er_parallel_for(job, NULL);

    for (int worker_slot = 0; worker_slot < WORKER_COUNT; worker_slot++)
    {
        if (s_ran[worker_slot] != 1)
            return fail("parallel: every worker must run its share exactly once");
        if (!s_alpha_ok[worker_slot])
            return fail("parallel: draw-alpha context bled between workers");
        if (!s_clip_ok[worker_slot])
            return fail("parallel: clip-stack context bled between workers");
    }

    /* ---- Fork again: the machinery (and the barrier) must be reusable frame after frame. ---- */
    er_parallel_for(job, NULL);
    for (int worker_slot = 0; worker_slot < WORKER_COUNT; worker_slot++)
        if (s_ran[worker_slot] != 2)
            return fail("parallel: second fork must run every worker again");

    /* ================================================================================
     * Full-commit equivalence: a scene rendered by the sliced parallel fork must be
     * byte-identical to the same scene rendered single-core. The scene straddles the
     * slice boundary with an opacity group and a scaled transform so cross-slice
     * composites and captures are exercised, not just plain fills.
     * ================================================================================ */
    {
        static const EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
        embedded_renderer_set_backend(&backend); /* re-inits worker contexts; workers stay installed */

        ERNode* root = new_scene();
        const ERProps base = auto_props(); /* AUTO-initialised template for the children */

        /* A 40px-tall tile whose color changes drive the partial-damage round below. */
        ERNode* tile = er_node_create(ER_NODE_VIEW);
        ERProps tile_props = base;
        tile_props.position = ER_POSITION_ABSOLUTE;
        tile_props.left = 2;
        tile_props.top = 2;
        tile_props.width = 14;
        tile_props.height = 40;
        tile_props.background_color = 0xFFB91C1CU;
        tile_props.border_radius = 4;
        er_node_set_props(tile, &tile_props);
        er_tree_append_child(root, tile);

        /* Translucent group straddling the slice boundary (rows 20-59), with a nested
         * translucent child: cross-slice strip composites at depth 2. */
        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps group_props = base;
        group_props.position = ER_POSITION_ABSOLUTE;
        group_props.left = 20;
        group_props.top = 20;
        group_props.width = 30;
        group_props.height = 40;
        group_props.opacity = 128U;
        group_props.background_color = 0xFF0E7490U;
        er_node_set_props(group, &group_props);
        ERNode* inner = er_node_create(ER_NODE_VIEW);
        ERProps inner_props = base;
        inner_props.position = ER_POSITION_ABSOLUTE;
        inner_props.left = 6;
        inner_props.top = 10;
        inner_props.width = 18;
        inner_props.height = 20;
        inner_props.opacity = 200U;
        inner_props.background_color = 0xFFF59E0BU;
        inner_props.border_radius = 6;
        er_node_set_props(inner, &inner_props);
        er_tree_append_child(group, inner);
        er_tree_append_child(root, group);

        /* A scaled node straddling the boundary: cross-slice transform capture + emit
         * (a no-op scale on TRANSLATE_ONLY builds — equivalence holds either way). */
        ERNode* scaled = er_node_create(ER_NODE_VIEW);
        ERProps scaled_props = base;
        scaled_props.position = ER_POSITION_ABSOLUTE;
        scaled_props.left = 52;
        scaled_props.top = 26;
        scaled_props.width = 8;
        scaled_props.height = 12;
        scaled_props.background_color = 0xFF15803DU;
        scaled_props.transform_scale_x = 1.5f;
        scaled_props.transform_scale_y = 1.5f;
        er_node_set_props(scaled, &scaled_props);
        er_tree_append_child(root, scaled);

        const uint32_t forks_before = er_parallel_frames();
        er_commit(); /* full repaint, sliced across the workers */
        if (er_parallel_frames() != forks_before + 1)
            return fail("equivalence: first commit did not take the parallel fork");

        static uint32_t parallel_framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
        memcpy(parallel_framebuffer, s_framebuffer, sizeof(s_framebuffer));

        /* Same scene, single-core. */
        embedded_renderer_set_workers(NULL);
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        er_force_full_repaint();
        er_commit();
        if (er_parallel_frames() != forks_before + 1)
            return fail("equivalence: serial commit must not fork");
        if (memcmp(parallel_framebuffer, s_framebuffer, sizeof(s_framebuffer)) != 0)
            return fail("equivalence: parallel and serial framebuffers differ (full repaint)");

        /* ---- Partial damage: a prop change rendered parallel vs serial. ---- */
        embedded_renderer_set_workers(&hooks);
        tile_props.background_color = 0xFF7E22CEU;
        er_node_set_props(tile, &tile_props);
        er_commit(); /* damage-clipped (~40 rows), still forks */
        if (er_parallel_frames() != forks_before + 2)
            return fail("equivalence: damage-clipped commit did not take the parallel fork");
        memcpy(parallel_framebuffer, s_framebuffer, sizeof(s_framebuffer));

        embedded_renderer_set_workers(NULL);
        tile_props.background_color = 0xFFB91C1CU; /* back... */
        er_node_set_props(tile, &tile_props);
        er_commit();
        tile_props.background_color = 0xFF7E22CEU; /* ...and forward again, serial this time */
        er_node_set_props(tile, &tile_props);
        er_commit();
        if (memcmp(parallel_framebuffer, s_framebuffer, sizeof(s_framebuffer)) != 0)
            return fail("equivalence: parallel and serial framebuffers differ (partial damage)");

        embedded_renderer_set_workers(&hooks); /* restore for the uninstall test below */
    }

    /* ---- A reset ends the old scene's hold on single-core rendering. ---- */
    {
        ERNode* root = new_scene();
        ERNode* arc = er_node_create(ER_NODE_ARC);
        ERProps arc_props = auto_props();
        arc_props.position = ER_POSITION_ABSOLUTE;
        arc_props.left = 10;
        arc_props.top = 10;
        arc_props.width = 40;
        arc_props.height = 40;
        arc_props.arc_width = 6;
        arc_props.arc_max = 100.0f;
        arc_props.arc_value = 50.0f;
        er_node_set_props(arc, &arc_props);
        er_tree_append_child(root, arc);

        const uint32_t forks = er_parallel_frames();
        er_commit();
        if (er_parallel_frames() != forks)
            return fail("reset: a scene with an Arc must render single-core, or the scenario proves nothing");

        new_scene();
        er_commit();
        if (er_parallel_frames() != forks + 1)
            return fail("reset: a scene without an Arc still rendered single-core after a reset from one with");
    }

    /* ---- Uninstall: back to the exact single-core path. ---- */
    embedded_renderer_set_workers(NULL);
    if (er_render_workers_active() != 1)
        return fail("uninstall: active workers must return to 1");
    s_active = 1;
    er_parallel_for(job, NULL);
    if (s_ran[0] != 3)
        return fail("uninstall: job must run inline on worker 0 again");

    for (int worker_index = 1; worker_index < WORKER_COUNT; worker_index++)
    {
        pthread_mutex_lock(&s_workers[worker_index].mutex);
        s_workers[worker_index].stop = 1;
        pthread_cond_signal(&s_workers[worker_index].condition);
        pthread_mutex_unlock(&s_workers[worker_index].mutex);
        pthread_join(s_workers[worker_index].thread, NULL);
    }
    printf("PASS: fork-join across %d workers with isolated contexts\n", WORKER_COUNT);
#else
    printf("PASS: single-worker build runs fork-join inline\n");
#endif

    return EXIT_SUCCESS;
}
