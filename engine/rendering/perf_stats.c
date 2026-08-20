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

#include "image_registry.h" /* image_registry_in_use / ERUI_IMAGE_REGISTRY_MAX */
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
static uint32_t s_sub_start[ER_PERF_RASTER_COUNT];
static bool s_sub_open[ER_PERF_RASTER_COUNT];

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
    memset(s_sub_open, 0, sizeof(s_sub_open));
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

void er_perf_raster_begin(ERPerfRasterSub sub)
{
    if (!s_frame_open || (int)sub < 0 || (int)sub >= (int)ER_PERF_RASTER_COUNT)
    {
        return;
    }
    s_sub_start[sub] = perf_now();
    s_sub_open[sub] = true;
}

void er_perf_raster_end(ERPerfRasterSub sub)
{
    if (!s_frame_open || (int)sub < 0 || (int)sub >= (int)ER_PERF_RASTER_COUNT)
    {
        return;
    }
    if (!s_sub_open[sub])
    {
        return;
    }
    s_cur.raster_us[sub] += perf_now() - s_sub_start[sub];
    s_sub_open[sub] = false;
}

void er_perf_note_blit(uint32_t us, uint32_t px)
{
    if (!s_frame_open)
    {
        return;
    }
    s_cur.raster_us[ER_PERF_RASTER_BLIT] += us;
    s_cur.blit_px += px;
}

uint32_t er_perf_now_us(void)
{
    return perf_now();
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
    for (int i = 0; i < (int)ER_PERF_RASTER_COUNT; i++)
    {
        if (s_sub_open[i])
        {
            s_cur.raster_us[i] += perf_now() - s_sub_start[i];
            s_sub_open[i] = false;
        }
    }

    /* The composite passes were timed as one span WITH the backend blits they emitted inside it;
     * subtract the blit total so the two buckets are disjoint and the split sums to the raster
     * phase. Clamped at zero: with parallel workers the blit total is CPU time summed across
     * workers and can legitimately exceed the composite wall span (see ERPerfRasterSub). */
    if (s_cur.raster_us[ER_PERF_RASTER_RENDER] > s_cur.raster_us[ER_PERF_RASTER_BLIT])
    {
        s_cur.raster_us[ER_PERF_RASTER_RENDER] -= s_cur.raster_us[ER_PERF_RASTER_BLIT];
    }
    else
    {
        s_cur.raster_us[ER_PERF_RASTER_RENDER] = 0U;
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
    s_cur.image_slots_total = (uint16_t)ERUI_IMAGE_REGISTRY_MAX;
    s_cur.vector_slots_overflow = er_vector_slots_overflowed();

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

    if (w->dirty_px >= 10000U)
    {
        snprintf(s_lines[3],
                 sizeof(s_lines[3]),
                 "PKDRT %dx%d %uk",
                 (int)w->dirty_w,
                 (int)w->dirty_h,
                 (unsigned)(w->dirty_px / 1000U));
    }
    else
    {
        snprintf(s_lines[3],
                 sizeof(s_lines[3]),
                 "PKDRT %dx%d %upx",
                 (int)w->dirty_w,
                 (int)w->dirty_h,
                 (unsigned)w->dirty_px);
    }
    /* "!FULL" only when the pool actually turned a node away: "VEC 8/8" alone is legitimate (a screen
     * that exactly fills the pool renders fine), so the counter cannot carry this on its own. */
    snprintf(s_lines[4],
             sizeof(s_lines[4]),
             "VEC %u/%u%s IMG %u/%u",
             (unsigned)l->vector_slots_used,
             (unsigned)l->vector_slots_total,
             l->vector_slots_overflow ? "!FULL" : "",
             (unsigned)l->image_slots_used,
             (unsigned)l->image_slots_total);

    /* The raster split, LAST frame: unlike the PK lines this stays live during a steady-state drag —
     * the case where every frame is equally slow and the peak lines are stuck on the mount frame. W is
     * the pixels handed to the backend; read it against PKDRT/dirty_px for the write amplification.
     * The W field is pre-formatted so each line keeps one literal format string (same k/px switch as
     * the PKDRT line). */
    char blit_last[12];
    char blit_worst[12];
    if (l->blit_px >= 10000U)
        snprintf(blit_last, sizeof(blit_last), "%uk", (unsigned)(l->blit_px / 1000U));
    else
        snprintf(blit_last, sizeof(blit_last), "%upx", (unsigned)l->blit_px);
    if (w->blit_px >= 10000U)
        snprintf(blit_worst, sizeof(blit_worst), "%uk", (unsigned)(w->blit_px / 1000U));
    else
        snprintf(blit_worst, sizeof(blit_worst), "%upx", (unsigned)w->blit_px);

    snprintf(s_lines[5],
             sizeof(s_lines[5]),
             "RST P%u.%u C%u.%u B%u.%u S%u.%u W%s",
             ER_PERF_MS_ARGS(l->raster_us[ER_PERF_RASTER_PREPASS]),
             ER_PERF_MS_ARGS(l->raster_us[ER_PERF_RASTER_RENDER]),
             ER_PERF_MS_ARGS(l->raster_us[ER_PERF_RASTER_BLIT]),
             ER_PERF_MS_ARGS(l->raster_us[ER_PERF_RASTER_SWEEP]),
             blit_last);

    /* The WORST frame's raster split (pairs with the PK lines above), whole milliseconds. */
    snprintf(s_lines[6],
             sizeof(s_lines[6]),
             "PKR P%u C%u B%u S%u W%s",
             ER_PERF_MS(w->raster_us[ER_PERF_RASTER_PREPASS]),
             ER_PERF_MS(w->raster_us[ER_PERF_RASTER_RENDER]),
             ER_PERF_MS(w->raster_us[ER_PERF_RASTER_BLIT]),
             ER_PERF_MS(w->raster_us[ER_PERF_RASTER_SWEEP]),
             blit_worst);

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

void er_perf_raster_begin(ERPerfRasterSub sub)
{
    (void)sub;
}

void er_perf_raster_end(ERPerfRasterSub sub)
{
    (void)sub;
}

void er_perf_note_blit(uint32_t us, uint32_t px)
{
    (void)us;
    (void)px;
}

uint32_t er_perf_now_us(void)
{
    return 0U;
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
