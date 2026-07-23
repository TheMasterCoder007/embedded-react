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
 * Render-worker fork-join (multi-core plan, Phase 1). The engine owns the job model; the HOST
 * owns the threads — installed as an EmbeddedRenderWorkers hook set (dispatch/sync/worker_id).
 * With the default ERUI_RENDER_WORKERS == 1 build, or with no workers installed, everything in
 * here collapses to a plain inline call on the render thread: no locks, no signalling, no
 * behavioral difference from the single-core engine.
 *
 * Memory-ordering note (the engine is C99 and has no atomics): the pending-job fields are
 * written by the render thread BEFORE dispatch() and read by workers only AFTER their wake-up —
 * the host's dispatch/sync primitives are required to order memory like a semaphore (see the
 * EmbeddedRenderWorkers contract), which is what makes that hand-off safe.
 */

#include "native_renderer.h"
#include "renderer_internal.h"

#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

#if ERUI_RENDER_WORKERS > 1
static EmbeddedRenderWorkers s_workers;   /* copied at install; count == 0 → none installed */
static ERParallelFn s_job_fn = NULL;      /* pending forked job (valid between dispatch and sync) */
static void* s_job_arg = NULL;
static bool s_forking = false;            /* true while a fork-join is in flight (re-entrancy guard) */
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void embedded_renderer_set_workers(const EmbeddedRenderWorkers* workers)
{
#if ERUI_RENDER_WORKERS > 1
    if (!workers || workers->count <= 1 || !workers->dispatch || !workers->sync || !workers->worker_id)
    {
        s_workers.count = 0; /* single-core */
        return;
    }
    s_workers = *workers;
    if (s_workers.count > ERUI_RENDER_WORKERS)
        s_workers.count = ERUI_RENDER_WORKERS; /* clamp to the build cap */
#else
    (void)workers; /* single-worker build: the renderer always runs single-core */
#endif
}

#if ERUI_RENDER_WORKERS > 1
int er_render_worker_id(void)
{
    /* Outside a fork-join every thread that may legally call render code IS worker 0 (the
     * render thread), so skip the hook — this also keeps captures/tests correct when render
     * code runs before any workers are installed. */
    if (!s_forking || s_workers.count == 0)
        return 0;
    return s_workers.worker_id();
}
#endif

int er_render_workers_active(void)
{
#if ERUI_RENDER_WORKERS > 1
    return (s_workers.count > 1) ? s_workers.count : 1;
#else
    return 1;
#endif
}

void er_render_worker_exec(int worker)
{
#if ERUI_RENDER_WORKERS > 1
    if (s_job_fn && worker > 0 && worker < er_render_workers_active())
        s_job_fn(worker, s_job_arg);
#else
    (void)worker;
#endif
}

void er_parallel_for(ERParallelFn fn, void* arg)
{
#if ERUI_RENDER_WORKERS > 1
    const int n = er_render_workers_active();
    if (n > 1 && !s_forking)
    {
        s_forking = true;
        s_job_fn = fn;
        s_job_arg = arg;
        /* Remote workers FIRST, own share last: signalling a same-core worker before the rest
         * lets it preempt this dispatch loop and silently serialize the job (measured in the
         * Phase 0.5 spike — every workload came back exactly 1.00x). */
        for (int k = n - 1; k >= 1; k--)
            s_workers.dispatch(k, s_workers.ctx);
        fn(0, arg);
        s_workers.sync(s_workers.ctx);
        s_job_fn = NULL;
        s_forking = false;
        return;
    }
#endif
    fn(0, arg);
}
