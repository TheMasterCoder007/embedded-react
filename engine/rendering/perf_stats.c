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

#include "er_perf.h"

#if ER_PERF_STATS

#include "image_registry.h" /* image_registry_in_use / IMAGE_REGISTRY_MAX */
#include "vector.h"         /* er_vector_slots_in_use / er_vector_slots_total */

#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t (*s_now_us)(void) = NULL;

static bool s_frame_open = false;
static uint32_t s_frame_start = 0U;
static uint32_t s_phase_start[ER_PERF_PHASE_COUNT];
static bool s_phase_open[ER_PERF_PHASE_COUNT];

static ERPerfFrame s_cur;   /* accumulating: the frame currently open */
static ERPerfFrame s_last;  /* the most recently completed frame */
static ERPerfFrame s_worst; /* the longest frame since the last reset — the spike we are hunting */
static bool s_have_last = false;
static bool s_have_worst = false;
static uint32_t s_next_index = 0U;

static char s_lines[ER_PERF_OVERLAY_LINES][ER_PERF_LINE_MAX];

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Samples the host clock, or 0 when none was installed (timing disabled, counters still work). */
static uint32_t perf_now(void)
{
    return s_now_us ? s_now_us() : 0U;
}

/** @brief Expands a microsecond duration into the two args of a "%u.%u" millisecond field. */
#define ER_PERF_MS_ARGS(us) (unsigned)((us) / 1000U), (unsigned)(((us) % 1000U) / 100U)

/** @brief Whole milliseconds, for the peak line where a tenth of a millisecond is noise. */
#define ER_PERF_MS(us) (unsigned)((us) / 1000U)

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_perf_set_clock(uint32_t (*now_us)(void))
{
    s_now_us = now_us;
}

void er_perf_frame_begin(void)
{
    memset(&s_cur, 0, sizeof(s_cur));
    memset(s_phase_open, 0, sizeof(s_phase_open));
    s_cur.index = s_next_index++;
    s_frame_start = perf_now();
    s_frame_open = true;
}

void er_perf_phase_begin(ERPerfPhase phase)
{
    if (!s_frame_open || (int)phase < 0 || (int)phase >= (int)ER_PERF_PHASE_COUNT)
    {
        return;
    }
    s_phase_start[phase] = perf_now();
    s_phase_open[phase] = true;
}

void er_perf_phase_end(ERPerfPhase phase)
{
    if (!s_frame_open || (int)phase < 0 || (int)phase >= (int)ER_PERF_PHASE_COUNT)
    {
        return;
    }
    if (!s_phase_open[phase])
    {
        return; /* never begun (or already ended): contribute nothing rather than a garbage duration */
    }
    /* Unsigned subtraction, so a 32-bit microsecond wrap between begin and end still yields the true
     * elapsed time — only a single phase longer than ~71 minutes would alias. */
    s_cur.phase_us[phase] += perf_now() - s_phase_start[phase];
    s_phase_open[phase] = false;
}

void er_perf_note_repaint(int x, int y, int w, int h)
{
    if (!s_frame_open || w <= 0 || h <= 0)
    {
        return; /* frame_begin zeroed the counters, so a frame that never notes stays at 0 */
    }
    /* Accumulate: the engine notes each disjoint repaint rect separately. dirty_x/y/w/h grow to the
     * union bounding box; dirty_px sums the areas — exact, since the rects are disjoint, and the
     * honest cost number (two small corners sum small even when their bbox spans the screen). */
    if (s_cur.dirty_px == 0U)
    {
        s_cur.dirty_x = (int32_t)x;
        s_cur.dirty_y = (int32_t)y;
        s_cur.dirty_w = (int32_t)w;
        s_cur.dirty_h = (int32_t)h;
    }
    else
    {
        const int32_t x1 = (s_cur.dirty_x + s_cur.dirty_w > x + w) ? s_cur.dirty_x + s_cur.dirty_w : x + w;
        const int32_t y1 = (s_cur.dirty_y + s_cur.dirty_h > y + h) ? s_cur.dirty_y + s_cur.dirty_h : y + h;
        if ((int32_t)x < s_cur.dirty_x)
            s_cur.dirty_x = (int32_t)x;
        if ((int32_t)y < s_cur.dirty_y)
            s_cur.dirty_y = (int32_t)y;
        s_cur.dirty_w = x1 - s_cur.dirty_x;
        s_cur.dirty_h = y1 - s_cur.dirty_y;
    }
    s_cur.dirty_px += (uint32_t)w * (uint32_t)h;
}

void er_perf_frame_end(void)
{
    if (!s_frame_open)
    {
        return;
    }

    /* Close anything still running so an unbalanced begin costs its own phase, not the whole frame. */
    for (int i = 0; i < (int)ER_PERF_PHASE_COUNT; i++)
    {
        if (s_phase_open[i])
        {
            s_cur.phase_us[i] += perf_now() - s_phase_start[i];
            s_phase_open[i] = false;
        }
    }

    s_cur.frame_us = perf_now() - s_frame_start;

    /* Whatever the four phases did not cover: input polling, the animation tick, host-side work. Kept
     * as an explicit field so the split always adds up to the frame and nothing hides in the gap. */
    uint32_t phases = 0U;
    for (int i = 0; i < (int)ER_PERF_PHASE_COUNT; i++)
    {
        phases += s_cur.phase_us[i];
    }
    s_cur.other_us = (s_cur.frame_us > phases) ? (s_cur.frame_us - phases) : 0U;

    /* Resource counters are sampled at the frame boundary — they are levels, not per-frame deltas, so
     * the value that matters is the one that held while the frame ran. */
    s_cur.vector_slots_used = (uint16_t)er_vector_slots_in_use();
    s_cur.vector_slots_total = (uint16_t)er_vector_slots_total();
    s_cur.image_slots_used = (uint16_t)image_registry_in_use();
    s_cur.image_slots_total = (uint16_t)IMAGE_REGISTRY_MAX;

    s_last = s_cur;
    s_have_last = true;
    if (!s_have_worst || s_cur.frame_us > s_worst.frame_us)
    {
        s_worst = s_cur;
        s_have_worst = true;
    }
    s_frame_open = false;
}

bool er_perf_get_last(ERPerfFrame* out)
{
    if (out)
    {
        if (s_have_last)
        {
            *out = s_last;
        }
        else
        {
            memset(out, 0, sizeof(*out));
        }
    }
    return s_have_last;
}

bool er_perf_get_worst(ERPerfFrame* out)
{
    if (out)
    {
        if (s_have_worst)
        {
            *out = s_worst;
        }
        else
        {
            memset(out, 0, sizeof(*out));
        }
    }
    return s_have_worst;
}

void er_perf_reset(void)
{
    memset(&s_last, 0, sizeof(s_last));
    memset(&s_worst, 0, sizeof(s_worst));
    s_have_last = false;
    s_have_worst = false;
    s_next_index = 0U;
}

int er_perf_overlay_lines(const char** lines, int max_lines)
{
    if (!lines || max_lines <= 0)
    {
        return 0;
    }

    const ERPerfFrame* l = &s_last;
    const ERPerfFrame* w = &s_worst;

    /* Integer formatting throughout (%u.%u rather than %.1f): newlib-nano, the default on the MCU
     * targets, links a printf with no floating-point support. */
    snprintf(s_lines[0],
             sizeof(s_lines[0]),
             "FRM %u.%u PK %u.%u",
             ER_PERF_MS_ARGS(l->frame_us),
             ER_PERF_MS_ARGS(w->frame_us));
    snprintf(s_lines[1],
             sizeof(s_lines[1]),
             "J%u.%u L%u.%u R%u.%u P%u.%u",
             ER_PERF_MS_ARGS(l->phase_us[ER_PERF_PHASE_JS]),
             ER_PERF_MS_ARGS(l->phase_us[ER_PERF_PHASE_LAYOUT]),
             ER_PERF_MS_ARGS(l->phase_us[ER_PERF_PHASE_RASTER]),
             ER_PERF_MS_ARGS(l->phase_us[ER_PERF_PHASE_PRESENT]));
    snprintf(s_lines[2],
             sizeof(s_lines[2]),
             "PK J%u L%u R%u P%u",
             ER_PERF_MS(w->phase_us[ER_PERF_PHASE_JS]),
             ER_PERF_MS(w->phase_us[ER_PERF_PHASE_LAYOUT]),
             ER_PERF_MS(w->phase_us[ER_PERF_PHASE_RASTER]),
             ER_PERF_MS(w->phase_us[ER_PERF_PHASE_PRESENT]));
    if (l->dirty_px >= 10000U)
    {
        snprintf(s_lines[3],
                 sizeof(s_lines[3]),
                 "DRT %dx%d %uk",
                 (int)l->dirty_w,
                 (int)l->dirty_h,
                 (unsigned)(l->dirty_px / 1000U));
    }
    else
    {
        snprintf(
            s_lines[3], sizeof(s_lines[3]), "DRT %dx%d %upx", (int)l->dirty_w, (int)l->dirty_h, (unsigned)l->dirty_px);
    }
    snprintf(s_lines[4],
             sizeof(s_lines[4]),
             "VEC %u/%u IMG %u/%u",
             (unsigned)l->vector_slots_used,
             (unsigned)l->vector_slots_total,
             (unsigned)l->image_slots_used,
             (unsigned)l->image_slots_total);

    const int n = (max_lines < ER_PERF_OVERLAY_LINES) ? max_lines : ER_PERF_OVERLAY_LINES;
    for (int i = 0; i < n; i++)
    {
        lines[i] = s_lines[i];
    }
    return n;
}

#else /* ER_PERF_STATS == 0 : compiled out */

#include <string.h>

void er_perf_set_clock(uint32_t (*now_us)(void))
{
    (void)now_us;
}

void er_perf_frame_begin(void)
{
}

void er_perf_phase_begin(ERPerfPhase phase)
{
    (void)phase;
}

void er_perf_phase_end(ERPerfPhase phase)
{
    (void)phase;
}

void er_perf_note_repaint(int x, int y, int w, int h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

void er_perf_frame_end(void)
{
}

bool er_perf_get_last(ERPerfFrame* out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

bool er_perf_get_worst(ERPerfFrame* out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

void er_perf_reset(void)
{
}

int er_perf_overlay_lines(const char** lines, int max_lines)
{
    (void)lines;
    (void)max_lines;
    return 0;
}

#endif
