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
