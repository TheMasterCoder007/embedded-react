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
static uint32_t s_subphase_start[ER_PERF_RASTER_COUNT];
static bool s_subphase_open[ER_PERF_RASTER_COUNT];

/* The JS sub-split is a nesting stack rather than a set of independent spans: the bridge's marks sit
 * inside one another (marshalling inside the mutation pass inside the batched render), and each
 * begin/end charges the time since the last mark to whichever bucket is innermost. That yields
 * exclusive times directly, with no subtraction pass to get wrong. 8 is far above the three levels
 * the bridge actually nests; s_js_overflow keeps a begin dropped at the ceiling from unbalancing the
 * stack when its end arrives. */
#define ER_PERF_JS_STACK_MAX 8

/* Overlay number formatting: microseconds print as milliseconds, and pixel counts from this size up as thousands. */
#define ER_PERF_US_PER_MS 1000U
#define ER_PERF_US_PER_TENTH_MS 100U
#define ER_PERF_KILO 1000U
#define ER_PERF_KILO_THRESHOLD 10000U
#define ER_PERF_FIELD_BYTES 12 /* one pre-formatted pixel count */
static uint8_t s_js_stack[ER_PERF_JS_STACK_MAX];
static int s_js_depth = 0;
static int s_js_overflow = 0;
static uint32_t s_js_mark = 0U; /* clock at the last bucket switch */

static ERPerfFrame s_current; /* accumulating: the frame currently open */
static ERPerfFrame s_last;    /* the most recently completed frame */
static ERPerfFrame s_worst;   /* the longest frame since the last reset — the spike we are hunting */
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
#define ER_PERF_MS_ARGS(microseconds)                                                                                  \
    (unsigned)((microseconds) / ER_PERF_US_PER_MS),                                                                    \
        (unsigned)(((microseconds) % ER_PERF_US_PER_MS) / ER_PERF_US_PER_TENTH_MS)

/** @brief Whole milliseconds, for the peak line where a tenth of a millisecond is noise. */
#define ER_PERF_MS(microseconds) (unsigned)((microseconds) / ER_PERF_US_PER_MS)

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_perf_set_clock(uint32_t (*now_us)(void))
{
    s_now_us = now_us;
}

void er_perf_frame_begin(void)
{
    memset(&s_current, 0, sizeof(s_current));
    memset(s_phase_open, 0, sizeof(s_phase_open));
    memset(s_subphase_open, 0, sizeof(s_subphase_open));
    s_js_depth = 0;
    s_js_overflow = 0;
    s_current.index = s_next_index++;
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
    s_current.phase_us[phase] += perf_now() - s_phase_start[phase];
    s_phase_open[phase] = false;
}

void er_perf_raster_begin(ERPerfRasterSubphase subphase)
{
    if (!s_frame_open || (int)subphase < 0 || (int)subphase >= (int)ER_PERF_RASTER_COUNT)
    {
        return;
    }
    s_subphase_start[subphase] = perf_now();
    s_subphase_open[subphase] = true;
}

void er_perf_raster_end(ERPerfRasterSubphase subphase)
{
    if (!s_frame_open || (int)subphase < 0 || (int)subphase >= (int)ER_PERF_RASTER_COUNT)
    {
        return;
    }
    if (!s_subphase_open[subphase])
    {
        return;
    }
    s_current.raster_us[subphase] += perf_now() - s_subphase_start[subphase];
    s_subphase_open[subphase] = false;
}

void er_perf_js_begin(ERPerfJsSubphase subphase)
{
    if (!s_frame_open || (int)subphase < 0 || (int)subphase >= (int)ER_PERF_JS_COUNT)
    {
        return;
    }
    if (s_js_depth >= ER_PERF_JS_STACK_MAX)
    {
        s_js_overflow++; /* remembered so the matching end() pops nothing rather than someone else's frame */
        return;
    }
    const uint32_t now = perf_now();
    if (s_js_depth > 0)
    {
        s_current.js_us[s_js_stack[s_js_depth - 1]] += now - s_js_mark;
    }
    s_js_mark = now;
    s_js_stack[s_js_depth++] = (uint8_t)subphase;
}

void er_perf_js_end(ERPerfJsSubphase subphase)
{
    if (!s_frame_open)
    {
        return;
    }
    if (s_js_overflow > 0)
    {
        s_js_overflow--;
        return;
    }
    if (s_js_depth == 0 || s_js_stack[s_js_depth - 1] != (uint8_t)subphase)
    {
        return; /* never begun, or crossed with another span: contribute nothing rather than misattribute */
    }
    const uint32_t now = perf_now();
    s_current.js_us[subphase] += now - s_js_mark;
    s_js_mark = now;
    s_js_depth--;
}

void er_perf_note_blit(uint32_t elapsed_us, uint32_t pixels)
{
    if (!s_frame_open)
    {
        return;
    }
    s_current.raster_us[ER_PERF_RASTER_BLIT] += elapsed_us;
    s_current.blit_pixels += pixels;
}

uint32_t er_perf_now_us(void)
{
    return perf_now();
}

void er_perf_note_repaint(int x, int y, int width, int height)
{
    if (!s_frame_open || width <= 0 || height <= 0)
    {
        return; /* frame_begin zeroed the counters, so a frame that never notes stays at 0 */
    }
    /* Accumulate: the engine notes each disjoint repaint rect separately. dirty_x/y/w/h grow to the
     * union bounding box; dirty_pixels sums the areas — exact, since the rects are disjoint, and the
     * honest cost number (two small corners sum small even when their bbox spans the screen). */
    if (s_current.dirty_pixels == 0U)
    {
        s_current.dirty_x = (int32_t)x;
        s_current.dirty_y = (int32_t)y;
        s_current.dirty_width = (int32_t)width;
        s_current.dirty_height = (int32_t)height;
    }
    else
    {
        const int32_t x1 = (s_current.dirty_x + s_current.dirty_width > x + width)
                               ? s_current.dirty_x + s_current.dirty_width
                               : x + width;
        const int32_t y1 = (s_current.dirty_y + s_current.dirty_height > y + height)
                               ? s_current.dirty_y + s_current.dirty_height
                               : y + height;
        if ((int32_t)x < s_current.dirty_x)
            s_current.dirty_x = (int32_t)x;
        if ((int32_t)y < s_current.dirty_y)
            s_current.dirty_y = (int32_t)y;
        s_current.dirty_width = x1 - s_current.dirty_x;
        s_current.dirty_height = y1 - s_current.dirty_y;
    }
    s_current.dirty_pixels += (uint32_t)width * (uint32_t)height;
}

void er_perf_frame_end(void)
{
    if (!s_frame_open)
    {
        return;
    }

    /* Close anything still running so an unbalanced begin costs its own phase, not the whole frame. */
    for (int phase_index = 0; phase_index < (int)ER_PERF_PHASE_COUNT; phase_index++)
    {
        if (s_phase_open[phase_index])
        {
            s_current.phase_us[phase_index] += perf_now() - s_phase_start[phase_index];
            s_phase_open[phase_index] = false;
        }
    }
    for (int subphase_index = 0; subphase_index < (int)ER_PERF_RASTER_COUNT; subphase_index++)
    {
        if (s_subphase_open[subphase_index])
        {
            s_current.raster_us[subphase_index] += perf_now() - s_subphase_start[subphase_index];
            s_subphase_open[subphase_index] = false;
        }
    }

    /* Same for the JS stack: charge what the innermost bucket has run so far and unwind, so a bridge
     * call that threw part-way through costs its own bucket rather than the whole frame.
     *
     * ONE timestamp for the whole unwind, deliberately. The outer buckets were paused when their
     * child opened, so no time is theirs to claim; re-reading the clock per pop would hand each of
     * them a slice of this loop's own cost instead of zero. */
    if (s_js_depth > 0)
    {
        const uint32_t now = perf_now();
        s_current.js_us[s_js_stack[s_js_depth - 1]] += now - s_js_mark;
        s_js_mark = now;
        s_js_depth = 0;
    }
    s_js_overflow = 0;

    /* The reconciler commits from inside the pump, so that er_commit() ran inside the host's JS span
     * AND reported itself as LAYOUT + RASTER. Counted in both, the phases sum past the frame and
     * other_us collapses to 0. Take it out of JS — LAYOUT + RASTER is where commit time belongs, and
     * js_us[COMMIT] still says the pump was what asked for it. */
    if (s_current.phase_us[ER_PERF_PHASE_JS] > s_current.js_us[ER_PERF_JS_COMMIT])
    {
        s_current.phase_us[ER_PERF_PHASE_JS] -= s_current.js_us[ER_PERF_JS_COMMIT];
    }
    else
    {
        s_current.phase_us[ER_PERF_PHASE_JS] = 0U;
    }

    /* The composite passes were timed as one span WITH the backend blits they emitted inside it;
     * subtract the blit total so the two buckets are disjoint and the split sums to the raster
     * phase. Clamped at zero: with parallel workers the blit total is CPU time summed across
     * workers and can legitimately exceed the composite wall span (see ERPerfRasterSubphase). */
    if (s_current.raster_us[ER_PERF_RASTER_RENDER] > s_current.raster_us[ER_PERF_RASTER_BLIT])
    {
        s_current.raster_us[ER_PERF_RASTER_RENDER] -= s_current.raster_us[ER_PERF_RASTER_BLIT];
    }
    else
    {
        s_current.raster_us[ER_PERF_RASTER_RENDER] = 0U;
    }

    s_current.frame_us = perf_now() - s_frame_start;

    /* Whatever the four phases did not cover: input polling, the animation tick, host-side work. Kept
     * as an explicit field so the split always adds up to the frame and nothing hides in the gap. */
    uint32_t phases = 0U;
    for (int phase_index = 0; phase_index < (int)ER_PERF_PHASE_COUNT; phase_index++)
    {
        phases += s_current.phase_us[phase_index];
    }
    s_current.other_us = (s_current.frame_us > phases) ? (s_current.frame_us - phases) : 0U;

    /* Resource counters are sampled at the frame boundary — they are levels, not per-frame deltas, so
     * the value that matters is the one that held while the frame ran. */
    s_current.vector_slots_used = (uint16_t)er_vector_slots_in_use();
    s_current.vector_slots_total = (uint16_t)er_vector_slots_total();
    s_current.image_slots_used = (uint16_t)image_registry_in_use();
    s_current.image_slots_total = (uint16_t)ERUI_IMAGE_REGISTRY_MAX;
    s_current.vector_slots_overflow = er_vector_slots_overflowed();

    s_last = s_current;
    s_have_last = true;
    if (!s_have_worst || s_current.frame_us > s_worst.frame_us)
    {
        s_worst = s_current;
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

    const ERPerfFrame* last = &s_last;
    const ERPerfFrame* worst = &s_worst;

    /* Integer formatting throughout (%u.%u rather than %.1f): newlib-nano, the default on the MCU
     * targets, links a printf with no floating-point support. */
    snprintf(s_lines[0],
             sizeof(s_lines[0]),
             "FRM %u.%u PK %u.%u",
             ER_PERF_MS_ARGS(last->frame_us),
             ER_PERF_MS_ARGS(worst->frame_us));
    snprintf(s_lines[1],
             sizeof(s_lines[1]),
             "J%u.%u L%u.%u R%u.%u P%u.%u",
             ER_PERF_MS_ARGS(last->phase_us[ER_PERF_PHASE_JS]),
             ER_PERF_MS_ARGS(last->phase_us[ER_PERF_PHASE_LAYOUT]),
             ER_PERF_MS_ARGS(last->phase_us[ER_PERF_PHASE_RASTER]),
             ER_PERF_MS_ARGS(last->phase_us[ER_PERF_PHASE_PRESENT]));
    snprintf(s_lines[2],
             sizeof(s_lines[2]),
             "PK J%u L%u R%u P%u",
             ER_PERF_MS(worst->phase_us[ER_PERF_PHASE_JS]),
             ER_PERF_MS(worst->phase_us[ER_PERF_PHASE_LAYOUT]),
             ER_PERF_MS(worst->phase_us[ER_PERF_PHASE_RASTER]),
             ER_PERF_MS(worst->phase_us[ER_PERF_PHASE_PRESENT]));

    if (worst->dirty_pixels >= ER_PERF_KILO_THRESHOLD)
    {
        snprintf(s_lines[3],
                 sizeof(s_lines[3]),
                 "PKDRT %dx%d %uk",
                 (int)worst->dirty_width,
                 (int)worst->dirty_height,
                 (unsigned)(worst->dirty_pixels / ER_PERF_KILO));
    }
    else
    {
        snprintf(s_lines[3],
                 sizeof(s_lines[3]),
                 "PKDRT %dx%d %upx",
                 (int)worst->dirty_width,
                 (int)worst->dirty_height,
                 (unsigned)worst->dirty_pixels);
    }
    /* "!FULL" only when the pool actually turned a node away: "VEC 8/8" alone is legitimate (a screen
     * that exactly fills the pool renders fine), so the counter cannot carry this on its own. */
    snprintf(s_lines[4],
             sizeof(s_lines[4]),
             "VEC %u/%u%s IMG %u/%u",
             (unsigned)last->vector_slots_used,
             (unsigned)last->vector_slots_total,
             last->vector_slots_overflow ? "!FULL" : "",
             (unsigned)last->image_slots_used,
             (unsigned)last->image_slots_total);

    /* The raster split, LAST frame: unlike the PK lines this stays live during a steady-state drag —
     * the case where every frame is equally slow and the peak lines are stuck on the mount frame. W is
     * the pixels handed to the backend; read it against PKDRT/dirty_pixels for the write amplification.
     * The W field is pre-formatted so each line keeps one literal format string (same k/px switch as
     * the PKDRT line). */
    char blit_last[ER_PERF_FIELD_BYTES];
    char blit_worst[ER_PERF_FIELD_BYTES];
    if (last->blit_pixels >= ER_PERF_KILO_THRESHOLD)
        snprintf(blit_last, sizeof(blit_last), "%uk", (unsigned)(last->blit_pixels / ER_PERF_KILO));
    else
        snprintf(blit_last, sizeof(blit_last), "%upx", (unsigned)last->blit_pixels);
    if (worst->blit_pixels >= ER_PERF_KILO_THRESHOLD)
        snprintf(blit_worst, sizeof(blit_worst), "%uk", (unsigned)(worst->blit_pixels / ER_PERF_KILO));
    else
        snprintf(blit_worst, sizeof(blit_worst), "%upx", (unsigned)worst->blit_pixels);

    snprintf(s_lines[5],
             sizeof(s_lines[5]),
             "RST P%u.%u C%u.%u B%u.%u S%u.%u W%s",
             ER_PERF_MS_ARGS(last->raster_us[ER_PERF_RASTER_PREPASS]),
             ER_PERF_MS_ARGS(last->raster_us[ER_PERF_RASTER_RENDER]),
             ER_PERF_MS_ARGS(last->raster_us[ER_PERF_RASTER_BLIT]),
             ER_PERF_MS_ARGS(last->raster_us[ER_PERF_RASTER_SWEEP]),
             blit_last);

    /* The WORST frame's raster split (pairs with the PK lines above), whole milliseconds. */
    snprintf(s_lines[6],
             sizeof(s_lines[6]),
             "PKR P%u C%u B%u S%u W%s",
             ER_PERF_MS(worst->raster_us[ER_PERF_RASTER_PREPASS]),
             ER_PERF_MS(worst->raster_us[ER_PERF_RASTER_RENDER]),
             ER_PERF_MS(worst->raster_us[ER_PERF_RASTER_BLIT]),
             ER_PERF_MS(worst->raster_us[ER_PERF_RASTER_SWEEP]),
             blit_worst);

    /* The JS split, last frame then peak. D/R/M are slices of the JS phase; C is the commit the pump
     * drove — reported here (not in JS) because its time lives in the L and R fields above. */
    snprintf(s_lines[7],
             sizeof(s_lines[7]),
             "JSS D%u.%u R%u.%u M%u.%u C%u.%u",
             ER_PERF_MS_ARGS(last->js_us[ER_PERF_JS_DISPATCH]),
             ER_PERF_MS_ARGS(last->js_us[ER_PERF_JS_RECONCILE]),
             ER_PERF_MS_ARGS(last->js_us[ER_PERF_JS_MARSHAL]),
             ER_PERF_MS_ARGS(last->js_us[ER_PERF_JS_COMMIT]));
    snprintf(s_lines[8],
             sizeof(s_lines[8]),
             "PKJ D%u R%u M%u C%u",
             ER_PERF_MS(worst->js_us[ER_PERF_JS_DISPATCH]),
             ER_PERF_MS(worst->js_us[ER_PERF_JS_RECONCILE]),
             ER_PERF_MS(worst->js_us[ER_PERF_JS_MARSHAL]),
             ER_PERF_MS(worst->js_us[ER_PERF_JS_COMMIT]));

    const int line_count = (max_lines < ER_PERF_OVERLAY_LINES) ? max_lines : ER_PERF_OVERLAY_LINES;
    for (int line_index = 0; line_index < line_count; line_index++)
    {
        lines[line_index] = s_lines[line_index];
    }
    return line_count;
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

void er_perf_raster_begin(ERPerfRasterSubphase subphase)
{
    (void)subphase;
}

void er_perf_raster_end(ERPerfRasterSubphase subphase)
{
    (void)subphase;
}

void er_perf_js_begin(ERPerfJsSubphase subphase)
{
    (void)subphase;
}

void er_perf_js_end(ERPerfJsSubphase subphase)
{
    (void)subphase;
}

void er_perf_note_blit(uint32_t elapsed_us, uint32_t pixels)
{
    (void)elapsed_us;
    (void)pixels;
}

uint32_t er_perf_now_us(void)
{
    return 0U;
}

void er_perf_note_repaint(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
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
