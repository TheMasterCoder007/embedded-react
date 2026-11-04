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

/*----------------------------------------------------------------------------------------------------------------------
 - Analytic annular-sector rasterizer (the Arc widget's core).
 *
 * A stroked arc through the generic vector path (ER_VECTOR_OP_ARC) is flattened to a polyline, offset into a stroke
 * outline and scan-converted with sub-scanline coverage — a cost paid again on every frame of a moving dial.
 * An arc is simple enough to evaluate in closed form instead:
 *
 *   - the RING is two circles: per row, four half-chords (each radius ± half a pixel) split the row into
 *     "hole / inner fringe / solid / outer fringe / outside", and only the fringe pixels need a distance —
 *     the rounded-rect fill's row-span idea (rrect.c) applied to a full circle;
 *   - the SWEEP is two half-planes through the centre (an intersection below 180°, a union above), so the
 *     per-pixel wedge test is two cross products whose magnitudes ARE the anti-aliasing distances;
 *   - round CAPS are two discs, segment GAPS are more wedges, a CONIC paint is the pixel's angle indexed
 *     into a colour LUT, a RADIAL paint its distance across the thickness.
 *
 * The half-chords of each radius are cached across nodes and frames (keyed by radius + row phase), so a dial
 * that animates only its value re-derives nothing per row but a table lookup.
 ---------------------------------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------------------------------
 - Includes
 ---------------------------------------------------------------------------------------------------------------------*/

#include "arc.h"
#include "gradient.h"          /* er_gradient_eval_stops / er_gradient_premul (always compiled) */
#include "renderer_internal.h" /* er_blit_blend, er_render_worker_id */
#include <math.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Macros
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Pixels per blended chunk: bounds the per-worker row scratch to 1 KB regardless of arc size. */
#define ARC_ROW_CHUNK 256

/** @brief Colour-ramp entries for a gradient-painted sector (built once per sector call). */
#define ARC_GRADIENT_RAMP_SIZE 128

/** @brief Hard cap on segment count (each gap costs two cross products per pixel). */
#define ARC_MAX_SEGMENTS 32

/** @brief A full turn, half a turn and a quarter turn, in degrees. */
#define ARC_FULL_TURN_DEG 360.0f
#define ARC_HALF_TURN_DEG 180.0f
#define ARC_QUADRANT_DEG 90.0f

/** @brief Past this many degrees the bbox counts every quadrant, since a float stops resolving them. */
#define ARC_BBOX_ANGLE_LIMIT 1e6f

/** @brief 1.0 in the span cache's 8.8 fixed-point half-chords. */
#define ARC_SPAN_FIXED_ONE 256.0f

/** @brief Row fractions closer than this share a span-cache entry. */
#define ARC_SPAN_FRACTION_TOLERANCE (1.0f / 512.0f)

/** @brief A gradient paint needs at least this many stops. */
#define ARC_GRADIENT_MIN_STOPS 2U

/** @brief Radii with a half-chord per row: outer and inner, each plus and minus half a pixel. */
#define ARC_FRINGE_RADII 4

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One cached circle: the half-chord of every row, 8.8 fixed point, indexed by floor(|row offset|).
 *
 * Rows of one sector all share the same fractional offset from the centre (the centre of a layout box is
 * an integer or a half), so the key is (radius, that fraction). A circle drawn at a different row phase
 * simply occupies a second entry.
 */
typedef struct
{
    float radius;       /**< Radius this entry was built for; 0 = empty. */
    float fraction;     /**< Fractional part of |row offset| for every row served. */
    uint32_t stamp;     /**< Last-use tick for LRU replacement. */
    uint16_t row_count; /**< Valid entries in half_chords[] (rows 0..row_count-1 intersect the circle). */
    uint16_t half_chords[ERUI_ARC_MAX_RADIUS + 2];
} ArcSpanEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ArcSpanEntry s_span_cache[ERUI_ARC_SPAN_CACHE];
static uint32_t s_span_tick = 0U;
static uint32_t s_span_misses = 0U;

/** @brief Premultiplied row chunk, one per render worker (the blend source). */
static uint32_t s_row[ERUI_RENDER_WORKERS][ARC_ROW_CHUNK];

/** @brief Gradient colour ramp, one per render worker, rebuilt per gradient-painted sector. */
static uint32_t s_ramp[ERUI_RENDER_WORKERS][ARC_GRADIENT_RAMP_SIZE];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Clamps a coverage to [0, 1]. */
static inline float clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/** @brief Exact half-chord (no cache). */
static inline float half_chord_exact(float radius, float row_offset)
{
    const float chord_squared = radius * radius - row_offset * row_offset;
    return (chord_squared <= 0.0f) ? -1.0f : sqrtf(chord_squared);
}

/** @brief Finds or builds the span-cache entry for (radius, fraction); NULL when the radius is too large to cache. */
static ArcSpanEntry* span_entry(float radius, float fraction)
{
    if (radius <= 0.0f || radius >= (float)ERUI_ARC_MAX_RADIUS)
        return NULL;
    s_span_tick++;
    ArcSpanEntry* victim = &s_span_cache[0];
    for (int entry_index = 0; entry_index < ERUI_ARC_SPAN_CACHE; entry_index++)
    {
        ArcSpanEntry* entry = &s_span_cache[entry_index];
        if (entry->radius == radius && fabsf(entry->fraction - fraction) < ARC_SPAN_FRACTION_TOLERANCE)
        {
            entry->stamp = s_span_tick;
            return entry;
        }
        if (entry->radius == 0.0f || entry->stamp < victim->stamp)
            victim = entry;
    }
    /* Miss: (re)build the least recently used entry. Rows 0.. while row + fraction < radius. */
    s_span_misses++;
    victim->radius = radius;
    victim->fraction = fraction;
    victim->stamp = s_span_tick;
    uint16_t row_count = 0U;
    while (row_count < (uint16_t)(ERUI_ARC_MAX_RADIUS + 2))
    {
        const float row_offset = (float)row_count + fraction;
        const float chord = half_chord_exact(radius, row_offset);
        if (chord < 0.0f)
            break;
        victim->half_chords[row_count] = (uint16_t)(chord * ARC_SPAN_FIXED_ONE + 0.5f);
        row_count++;
    }
    victim->row_count = row_count;
    return victim;
}

/** @brief Half-chord of radius @p radius at row offset @p row_offset via a prepared cache entry (or exact when NULL).
 */
static inline float half_chord(const ArcSpanEntry* entry, float radius, float row_offset)
{
    if (!entry)
        return half_chord_exact(radius, row_offset);
    const float abs_row_offset = fabsf(row_offset);
    const int row = (int)abs_row_offset;
    if (row >= (int)entry->row_count)
        return -1.0f;
    return (float)entry->half_chords[row] * (1.0f / ARC_SPAN_FIXED_ONE);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

float er_arc_half_chord(float radius, float row_offset)
{
    const float abs_row_offset = fabsf(row_offset);
    const float fraction = abs_row_offset - floorf(abs_row_offset);
    return half_chord(span_entry(radius, fraction), radius, row_offset);
}

void er_arc_span_cache_reset(void)
{
    memset(s_span_cache, 0, sizeof(s_span_cache));
    s_span_tick = 0U;
    s_span_misses = 0U;
}

uint32_t er_arc_span_cache_misses(void)
{
    return s_span_misses;
}

void er_arc_sector_bbox(float center_x,
                        float center_y,
                        float inner_radius,
                        float outer_radius,
                        float start_deg,
                        float end_deg,
                        float padding,
                        float* x0,
                        float* y0,
                        float* x1,
                        float* y1)
{
    if (end_deg < start_deg)
    {
        const float swap = start_deg;
        start_deg = end_deg;
        end_deg = swap;
    }
    if (inner_radius < 0.0f)
        inner_radius = 0.0f;
#define ADD(new_x, new_y)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        const float point_x = (new_x), point_y = (new_y);                                                              \
        if (point_x < *x0)                                                                                             \
            *x0 = point_x;                                                                                             \
        if (point_y < *y0)                                                                                             \
            *y0 = point_y;                                                                                             \
        if (point_x > *x1)                                                                                             \
            *x1 = point_x;                                                                                             \
        if (point_y > *y1)                                                                                             \
            *y1 = point_y;                                                                                             \
    } while (0)
    const float start_cos = cosf(start_deg * ER_DEG2RAD), start_sin = sinf(start_deg * ER_DEG2RAD);
    const float end_cos = cosf(end_deg * ER_DEG2RAD), end_sin = sinf(end_deg * ER_DEG2RAD);
    ADD(center_x + inner_radius * start_cos, center_y + inner_radius * start_sin);
    ADD(center_x + outer_radius * start_cos, center_y + outer_radius * start_sin);
    ADD(center_x + inner_radius * end_cos, center_y + inner_radius * end_sin);
    ADD(center_x + outer_radius * end_cos, center_y + outer_radius * end_sin);
    /* Axis extremes inside the sweep: the outer circle reaches its bbox edge there. The quadrants are counted in
     * a float, which stops changing past 2^24, so an angle that far out (a <Dial> prop is app math), a NaN or
     * infinite one, or a sweep past a turn takes all four. */
    if (end_deg - start_deg <= ARC_FULL_TURN_DEG && fabsf(start_deg) < ARC_BBOX_ANGLE_LIMIT)
    {
        const float first_quadrant = ceilf(start_deg / ARC_QUADRANT_DEG);
        for (float quadrant = first_quadrant; quadrant * ARC_QUADRANT_DEG <= end_deg; quadrant += 1.0f)
        {
            const float angle = quadrant * ARC_QUADRANT_DEG * ER_DEG2RAD;
            ADD(center_x + outer_radius * cosf(angle), center_y + outer_radius * sinf(angle));
        }
    }
    else
    {
        ADD(center_x + outer_radius, center_y);
        ADD(center_x - outer_radius, center_y);
        ADD(center_x, center_y + outer_radius);
        ADD(center_x, center_y - outer_radius);
    }
#undef ADD
    *x0 -= padding;
    *y0 -= padding;
    *x1 += padding;
    *y1 += padding;
}

void er_arc_fill_sector(const ERArcSector* sector)
{
    if (!sector)
        return;
    const float outer_radius = sector->outer_radius;
    float inner_radius = sector->inner_radius;
    if (inner_radius < 0.0f)
        inner_radius = 0.0f;
    /* NaN or infinite geometry (app math behind an <Svg> arc or a <Dial> prop) has nothing to draw, and would
     * reach the int casts below as NaN. */
    if (!(fabsf(sector->center_x) < INFINITY && fabsf(sector->center_y) < INFINITY && outer_radius < INFINITY
          && inner_radius == inner_radius && fabsf(sector->start_deg) < INFINITY && fabsf(sector->end_deg) < INFINITY))
        return;
    if (outer_radius <= 0.0f || inner_radius >= outer_radius)
        return;
    float sweep = sector->end_deg - sector->start_deg;
    if (sweep <= 0.0f)
        return;
    if (sweep > ARC_FULL_TURN_DEG)
        sweep = ARC_FULL_TURN_DEG;
    const bool full_circle = (sweep >= ARC_FULL_TURN_DEG);

    const int worker = er_render_worker_id();
    uint32_t* row = s_row[worker];
    uint32_t* ramp = s_ramp[worker];

    /* Paint. */
    const bool conic = (sector->gradient_type == ER_GRADIENT_CONIC && sector->gradient_stops
                        && sector->gradient_stop_count >= ARC_GRADIENT_MIN_STOPS);
    const bool radial = (sector->gradient_type == ER_GRADIENT_RADIAL && sector->gradient_stops
                         && sector->gradient_stop_count >= ARC_GRADIENT_MIN_STOPS);
    const uint32_t solid = er_gradient_premul(sector->color);
    if (!conic && !radial && (sector->color >> 24) == 0U)
        return;
    if (conic || radial)
        for (int ramp_index = 0; ramp_index < ARC_GRADIENT_RAMP_SIZE; ramp_index++)
            ramp[ramp_index] =
                er_gradient_premul(er_gradient_eval_stops(sector->gradient_stops,
                                                          (int)sector->gradient_stop_count,
                                                          ((float)ramp_index + 0.5f) / (float)ARC_GRADIENT_RAMP_SIZE));
    const float gradient_span = (sector->gradient_end_deg != sector->gradient_start_deg)
                                    ? (sector->gradient_end_deg - sector->gradient_start_deg)
                                    : ARC_FULL_TURN_DEG;
    const float gradient_start = sector->gradient_start_deg;
    const float thickness = outer_radius - inner_radius;

    /* Boundary rays. */
    const float start_rad = sector->start_deg * ER_DEG2RAD, end_rad = sector->end_deg * ER_DEG2RAD;
    const float start_cos = cosf(start_rad), start_sin = sinf(start_rad), end_cos = cosf(end_rad),
                end_sin = sinf(end_rad);
    const bool wedge_intersects = (sweep <= ARC_HALF_TURN_DEG);

    /* Round caps: discs of radius cap_radius centred on the mid-radius at each boundary ray. */
    const bool has_caps = (!full_circle && sector->cap == ER_ARC_CAP_ROUND && thickness > 0.0f);
    const float cap_radius = thickness * 0.5f;
    const float mid_radius = (outer_radius + inner_radius) * 0.5f;
    const float start_cap_x = sector->center_x + mid_radius * start_cos,
                start_cap_y = sector->center_y + mid_radius * start_sin;
    const float end_cap_x = sector->center_x + mid_radius * end_cos,
                end_cap_y = sector->center_y + mid_radius * end_sin;
    const float cap_reach = cap_radius + 1.0f;

    /* Segment gaps: precompute the two rays of every gap. */
    int gap_count = 0;
    float gap_start_cos[ARC_MAX_SEGMENTS], gap_start_sin[ARC_MAX_SEGMENTS], gap_end_cos[ARC_MAX_SEGMENTS],
        gap_end_sin[ARC_MAX_SEGMENTS];
    if (sector->segments > 1U && sector->gap_deg > 0.0f)
    {
        int segment_count = (int)sector->segments;
        if (segment_count > ARC_MAX_SEGMENTS)
            segment_count = ARC_MAX_SEGMENTS;
        const float span = sector->segment_end_deg - sector->segment_start_deg;
        const float segment_length = (span - (float)(segment_count - 1) * sector->gap_deg) / (float)segment_count;
        if (segment_length > 0.0f)
        {
            for (int segment_index = 1; segment_index < segment_count; segment_index++)
            {
                const float gap_start = sector->segment_start_deg
                                        + (float)segment_index * (segment_length + sector->gap_deg) - sector->gap_deg;
                const float gap_end = gap_start + sector->gap_deg;
                gap_start_cos[gap_count] = cosf(gap_start * ER_DEG2RAD);
                gap_start_sin[gap_count] = sinf(gap_start * ER_DEG2RAD);
                gap_end_cos[gap_count] = cosf(gap_end * ER_DEG2RAD);
                gap_end_sin[gap_count] = sinf(gap_end * ER_DEG2RAD);
                gap_count++;
            }
        }
    }

    /* Row range, clamped to the clip in float first: a huge radius or centre would run past the int range. */
    const float top_y = sector->center_y - outer_radius - 1.0f, bottom_y = sector->center_y + outer_radius + 1.0f;
    const int y_start = top_y > (float)sector->clip_y0
                            ? (top_y < (float)sector->clip_y1 ? (int)floorf(top_y) : sector->clip_y1)
                            : sector->clip_y0;
    const int y_end = /* exclusive */
        bottom_y < (float)sector->clip_y1 ? (bottom_y > (float)sector->clip_y0 ? (int)ceilf(bottom_y) : sector->clip_y0)
                                          : sector->clip_y1;
    if (y_start >= y_end)
        return;

    /* Row-span cache entries for the four radii (outer ± 0.5, inner ± 0.5). Rows above and below the centre
     * may sit at different fractional offsets from it (they coincide when the centre is on a pixel edge or
     * centre, the layout-box case), so each radius is looked up once per side — never per row, which with a
     * small cache could thrash a rebuild into every scanline. */
    const float offset_below = 0.5f - sector->center_y, offset_above = sector->center_y - 0.5f;
    const float fraction_below = offset_below - floorf(offset_below);
    const float fraction_above = offset_above - floorf(offset_above);
    const float outer_plus_half = outer_radius + 0.5f, outer_minus_half = outer_radius - 0.5f,
                inner_plus_half = inner_radius + 0.5f, inner_minus_half = inner_radius - 0.5f;
    const ArcSpanEntry* entries_below[ARC_FRINGE_RADII];
    const ArcSpanEntry* entries_above[ARC_FRINGE_RADII];
    const float radii[ARC_FRINGE_RADII] = {outer_plus_half, outer_minus_half, inner_plus_half, inner_minus_half};
    const bool radius_used[ARC_FRINGE_RADII] = {
        true, outer_minus_half > 0.0f, inner_radius > 0.0f, inner_minus_half > 0.0f};
    for (int radius_index = 0; radius_index < ARC_FRINGE_RADII; radius_index++)
    {
        entries_below[radius_index] =
            radius_used[radius_index] ? span_entry(radii[radius_index], fraction_below) : NULL;
        entries_above[radius_index] =
            radius_used[radius_index]
                ? ((fraction_above == fraction_below) ? entries_below[radius_index]
                                                      : span_entry(radii[radius_index], fraction_above))
                : NULL;
    }

    for (int y = y_start; y < y_end; y++)
    {
        const float row_offset = (float)y + 0.5f - sector->center_y;
        const ArcSpanEntry* const* entries = (row_offset < 0.0f) ? entries_above : entries_below;
        const float chord_outer_plus = half_chord(entries[0], outer_plus_half, row_offset);
        if (chord_outer_plus < 0.0f)
            continue; /* row misses the circle */
        const float chord_outer_minus = radius_used[1] ? half_chord(entries[1], outer_minus_half, row_offset) : -1.0f;
        const float chord_inner_plus = radius_used[2] ? half_chord(entries[2], inner_plus_half, row_offset) : -1.0f;
        const float chord_inner_minus = radius_used[3] ? half_chord(entries[3], inner_minus_half, row_offset) : -1.0f;

        /* Column intervals (pixel-centre x): the whole chord, or the two sides of the hole. */
        float interval_start0, interval_end0, interval_start1, interval_end1;
        int interval_count;
        if (chord_inner_minus > 0.0f)
        {
            interval_start0 = sector->center_x - chord_outer_plus;
            interval_end0 = sector->center_x - chord_inner_minus;
            interval_start1 = sector->center_x + chord_inner_minus;
            interval_end1 = sector->center_x + chord_outer_plus;
            interval_count = 2;
        }
        else
        {
            interval_start0 = sector->center_x - chord_outer_plus;
            interval_end0 = sector->center_x + chord_outer_plus;
            interval_start1 = interval_end1 = 0.0f;
            interval_count = 1;
        }

        for (int interval_index = 0; interval_index < interval_count; interval_index++)
        {
            const float interval_start = (interval_index == 0) ? interval_start0 : interval_start1;
            const float interval_end = (interval_index == 0) ? interval_end0 : interval_end1;
            /* Clamped to the clip in float first, as the rows are. */
            const float first_center = interval_start - 0.5f, last_center = interval_end - 0.5f;
            const int column_start =
                first_center > (float)sector->clip_x0
                    ? (first_center < (float)sector->clip_x1 ? (int)floorf(first_center) : sector->clip_x1)
                    : sector->clip_x0;
            const int column_end = /* exclusive */
                last_center < (float)(sector->clip_x1 - 1)
                    ? (last_center > (float)(sector->clip_x0 - 1) ? (int)ceilf(last_center) + 1 : sector->clip_x0)
                    : sector->clip_x1;
            for (int chunk_start = column_start; chunk_start < column_end; chunk_start += ARC_ROW_CHUNK)
            {
                int chunk_end = chunk_start + ARC_ROW_CHUNK;
                if (chunk_end > column_end)
                    chunk_end = column_end;
                bool painted_any = false;
                for (int x = chunk_start; x < chunk_end; x++)
                {
                    const float column_offset = (float)x + 0.5f - sector->center_x;
                    const float abs_column_offset = fabsf(column_offset);
                    uint32_t* out = &row[x - chunk_start];
                    *out = 0U;
                    if (abs_column_offset > chord_outer_plus || abs_column_offset < chord_inner_minus)
                        continue;

                    /* Ring coverage: exact distance only on the two fringes. */
                    float distance = -1.0f;
                    float coverage = 1.0f;
                    if (abs_column_offset >= chord_outer_minus)
                    {
                        distance = sqrtf(column_offset * column_offset + row_offset * row_offset);
                        coverage = clamp01(outer_radius + 0.5f - distance);
                    }
                    if (inner_radius > 0.0f && abs_column_offset <= chord_inner_plus)
                    {
                        if (distance < 0.0f)
                            distance = sqrtf(column_offset * column_offset + row_offset * row_offset);
                        coverage *= clamp01(distance - inner_radius + 0.5f);
                    }
                    if (coverage <= 0.0f)
                        continue;

                    /* Sweep: two half-planes through the centre; the cross products are signed distances. */
                    if (!full_circle)
                    {
                        const float start_side = clamp01(0.5f + (start_cos * row_offset - start_sin * column_offset));
                        const float end_side = clamp01(0.5f + (end_sin * column_offset - end_cos * row_offset));
                        const float wedge_coverage = wedge_intersects ? (start_side < end_side ? start_side : end_side)
                                                                      : (start_side > end_side ? start_side : end_side);
                        float sweep_coverage = coverage * wedge_coverage;
                        if (has_caps)
                        {
                            const float pixel_x = (float)x + 0.5f, pixel_y = (float)y + 0.5f;
                            if (fabsf(pixel_x - start_cap_x) <= cap_reach && fabsf(pixel_y - start_cap_y) <= cap_reach)
                            {
                                const float cap_distance = sqrtf((pixel_x - start_cap_x) * (pixel_x - start_cap_x)
                                                                 + (pixel_y - start_cap_y) * (pixel_y - start_cap_y));
                                const float cap_coverage = clamp01(cap_radius + 0.5f - cap_distance);
                                if (cap_coverage > sweep_coverage)
                                    sweep_coverage = cap_coverage;
                            }
                            if (fabsf(pixel_x - end_cap_x) <= cap_reach && fabsf(pixel_y - end_cap_y) <= cap_reach)
                            {
                                const float cap_distance = sqrtf((pixel_x - end_cap_x) * (pixel_x - end_cap_x)
                                                                 + (pixel_y - end_cap_y) * (pixel_y - end_cap_y));
                                const float cap_coverage = clamp01(cap_radius + 0.5f - cap_distance);
                                if (cap_coverage > sweep_coverage)
                                    sweep_coverage = cap_coverage;
                            }
                        }
                        coverage = sweep_coverage;
                        if (coverage <= 0.0f)
                            continue;
                    }

                    /* Segment gaps (each a thin wedge, always < 180°: intersection of its two half-planes). */
                    for (int gap_index = 0; gap_index < gap_count; gap_index++)
                    {
                        const float start_side = clamp01(
                            0.5f + (gap_start_cos[gap_index] * row_offset - gap_start_sin[gap_index] * column_offset));
                        const float end_side = clamp01(
                            0.5f + (gap_end_sin[gap_index] * column_offset - gap_end_cos[gap_index] * row_offset));
                        const float gap_coverage = start_side < end_side ? start_side : end_side;
                        if (gap_coverage > 0.0f)
                            coverage *= (1.0f - gap_coverage);
                    }
                    if (coverage <= 0.0f)
                        continue;

                    /* Paint. */
                    uint32_t premultiplied;
                    if (conic)
                    {
                        float angle = er_fast_atan2(row_offset, column_offset) * ER_RAD2DEG - gradient_start;
                        angle -=
                            ARC_FULL_TURN_DEG * floorf(angle / ARC_FULL_TURN_DEG); /* [0, 360) past the ramp's start */
                        /* Outside the ramp (a round cap, the AA fringe of a boundary ray): pin to the nearer
                         * end instead of wrapping the last stop's colour onto the first cap. */
                        if (angle > gradient_span)
                            angle = ((angle - gradient_span) < (ARC_FULL_TURN_DEG - angle)) ? gradient_span : 0.0f;
                        int ramp_index = (int)((angle / gradient_span) * (float)ARC_GRADIENT_RAMP_SIZE);
                        if (ramp_index < 0)
                            ramp_index = 0;
                        else if (ramp_index >= ARC_GRADIENT_RAMP_SIZE)
                            ramp_index = ARC_GRADIENT_RAMP_SIZE - 1;
                        premultiplied = ramp[ramp_index];
                    }
                    else if (radial)
                    {
                        if (distance < 0.0f)
                            distance = sqrtf(column_offset * column_offset + row_offset * row_offset);
                        int ramp_index = (int)(((distance - inner_radius) / thickness) * (float)ARC_GRADIENT_RAMP_SIZE);
                        if (ramp_index < 0)
                            ramp_index = 0;
                        else if (ramp_index >= ARC_GRADIENT_RAMP_SIZE)
                            ramp_index = ARC_GRADIENT_RAMP_SIZE - 1;
                        premultiplied = ramp[ramp_index];
                    }
                    else
                    {
                        premultiplied = solid;
                    }
                    *out = er_px_scale_premul(premultiplied, (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
                    painted_any = true;
                }
                if (painted_any)
                    er_blit_blend(row,
                                  (int)(sizeof(uint32_t) * (size_t)(chunk_end - chunk_start)),
                                  ER_ALPHA_OPAQUE,
                                  chunk_start,
                                  y,
                                  chunk_end - chunk_start,
                                  1);
            }
        }
    }
}
