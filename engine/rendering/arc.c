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
 * A stroked arc through the generic vector path (ER_VOP_ARC) is flattened to a polyline, offset into a stroke
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
#define ARC_GRAD_LUT 128

/** @brief Hard cap on segment count (each gap costs two cross products per pixel). */
#define ARC_MAX_SEGMENTS 32

#define ARC_DEG2RAD 0.017453292519943295f

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One cached circle: the half-chord of every row, 8.8 fixed point, indexed by floor(|dy|).
 *
 * Rows of one sector all share the same fractional offset from the centre (the centre of a layout box is
 * an integer or a half), so the key is (radius, that fraction). A circle drawn at a different row phase
 * simply occupies a second entry.
 */
typedef struct
{
    float r;        /**< Radius this entry was built for; 0 = empty. */
    float frac;     /**< Fractional part of |dy| for every row served. */
    uint32_t stamp; /**< Last-use tick for LRU replacement. */
    uint16_t n;     /**< Valid entries in hc[] (rows 0..n-1 intersect the circle). */
    uint16_t hc[ERUI_ARC_MAX_RADIUS + 2];
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
static uint32_t s_lut[ERUI_RENDER_WORKERS][ARC_GRAD_LUT];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Clamps a coverage to [0, 1]. */
static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/** @brief Scales a premultiplied pixel by an 8-bit coverage (all four channels). */
static inline uint32_t scale_premul(uint32_t p, uint32_t cov)
{
    if (cov >= 255U)
        return p;
    const uint32_t a = (((p >> 24) & 0xFFU) * cov + 127U) / 255U;
    const uint32_t r = (((p >> 16) & 0xFFU) * cov + 127U) / 255U;
    const uint32_t g = (((p >> 8) & 0xFFU) * cov + 127U) / 255U;
    const uint32_t b = ((p & 0xFFU) * cov + 127U) / 255U;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/**
 * @brief Fast atan2 (max error ~0.0015 rad) for the per-pixel conic sampler — same polynomial the vector
 *        rasterizer's conic gradient uses, so both paths quantize an angle identically. Range (-PI, PI].
 */
static inline float fast_atan2(float y, float x)
{
    const float ax = fabsf(x), ay = fabsf(y);
    if (ax < 1e-12f && ay < 1e-12f)
        return 0.0f;
    const float a = (ax > ay) ? (ay / ax) : (ax / ay);
    const float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax)
        r = 1.57079637f - r;
    if (x < 0.0f)
        r = 3.14159274f - r;
    if (y < 0.0f)
        r = -r;
    return r;
}

/** @brief Exact half-chord (no cache). */
static inline float half_chord_exact(float r, float dy)
{
    const float v = r * r - dy * dy;
    return (v <= 0.0f) ? -1.0f : sqrtf(v);
}

/** @brief Finds or builds the span-cache entry for (r, frac); NULL when the radius is too large to cache. */
static ArcSpanEntry* span_entry(float r, float frac)
{
    if (r <= 0.0f || r >= (float)ERUI_ARC_MAX_RADIUS)
        return NULL;
    s_span_tick++;
    ArcSpanEntry* victim = &s_span_cache[0];
    for (int i = 0; i < ERUI_ARC_SPAN_CACHE; i++)
    {
        ArcSpanEntry* e = &s_span_cache[i];
        if (e->r == r && fabsf(e->frac - frac) < (1.0f / 512.0f))
        {
            e->stamp = s_span_tick;
            return e;
        }
        if (e->r == 0.0f || e->stamp < victim->stamp)
            victim = e;
    }
    /* Miss: (re)build the least recently used entry. Rows k = 0.. while k + frac < r. */
    s_span_misses++;
    victim->r = r;
    victim->frac = frac;
    victim->stamp = s_span_tick;
    uint16_t n = 0U;
    while (n < (uint16_t)(ERUI_ARC_MAX_RADIUS + 2))
    {
        const float dy = (float)n + frac;
        const float hc = half_chord_exact(r, dy);
        if (hc < 0.0f)
            break;
        victim->hc[n] = (uint16_t)(hc * 256.0f + 0.5f);
        n++;
    }
    victim->n = n;
    return victim;
}

/** @brief Half-chord of radius @p r at row offset @p dy via a prepared cache entry (or exact when NULL). */
static inline float half_chord(const ArcSpanEntry* e, float r, float dy)
{
    if (!e)
        return half_chord_exact(r, dy);
    const float ady = fabsf(dy);
    const int k = (int)ady;
    if (k >= (int)e->n)
        return -1.0f;
    return (float)e->hc[k] * (1.0f / 256.0f);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

float er_arc_half_chord(float r, float dy)
{
    const float ady = fabsf(dy);
    const float frac = ady - floorf(ady);
    return half_chord(span_entry(r, frac), r, dy);
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

void er_arc_sector_bbox(float cx,
                        float cy,
                        float r_in,
                        float r_out,
                        float a0,
                        float a1,
                        float pad,
                        float* x0,
                        float* y0,
                        float* x1,
                        float* y1)
{
    if (a1 < a0)
    {
        const float t = a0;
        a0 = a1;
        a1 = t;
    }
    if (r_in < 0.0f)
        r_in = 0.0f;
#define ADD(X, Y)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        const float _x = (X), _y = (Y);                                                                                \
        if (_x < *x0)                                                                                                  \
            *x0 = _x;                                                                                                  \
        if (_y < *y0)                                                                                                  \
            *y0 = _y;                                                                                                  \
        if (_x > *x1)                                                                                                  \
            *x1 = _x;                                                                                                  \
        if (_y > *y1)                                                                                                  \
            *y1 = _y;                                                                                                  \
    } while (0)
    const float c0 = cosf(a0 * ARC_DEG2RAD), s0 = sinf(a0 * ARC_DEG2RAD);
    const float c1 = cosf(a1 * ARC_DEG2RAD), s1 = sinf(a1 * ARC_DEG2RAD);
    ADD(cx + r_in * c0, cy + r_in * s0);
    ADD(cx + r_out * c0, cy + r_out * s0);
    ADD(cx + r_in * c1, cy + r_in * s1);
    ADD(cx + r_out * c1, cy + r_out * s1);
    /* Axis extremes inside the sweep: the outer circle reaches its bbox edge there. */
    const float k0 = ceilf(a0 / 90.0f);
    for (float k = k0; k * 90.0f <= a1; k += 1.0f)
    {
        const float a = k * 90.0f * ARC_DEG2RAD;
        ADD(cx + r_out * cosf(a), cy + r_out * sinf(a));
    }
#undef ADD
    *x0 -= pad;
    *y0 -= pad;
    *x1 += pad;
    *y1 += pad;
}

void er_arc_fill_sector(const ERArcSector* s)
{
    if (!s)
        return;
    const float ro = s->r_outer;
    float ri = s->r_inner;
    if (ri < 0.0f)
        ri = 0.0f;
    if (ro <= 0.0f || ri >= ro)
        return;
    float sweep = s->a1 - s->a0;
    if (sweep <= 0.0f)
        return;
    if (sweep > 360.0f)
        sweep = 360.0f;
    const bool full = (sweep >= 360.0f);

    const int wid = er_render_worker_id();
    uint32_t* row = s_row[wid];
    uint32_t* lut = s_lut[wid];

    /* Paint. */
    const bool conic = (s->grad_type == ER_GRADIENT_CONIC && s->grad_stops && s->grad_stop_count >= 2U);
    const bool radial = (s->grad_type == ER_GRADIENT_RADIAL && s->grad_stops && s->grad_stop_count >= 2U);
    const uint32_t solid = er_gradient_premul(s->color);
    if (!conic && !radial && (s->color >> 24) == 0U)
        return;
    if (conic || radial)
        for (int i = 0; i < ARC_GRAD_LUT; i++)
            lut[i] = er_gradient_premul(er_gradient_eval_stops(
                s->grad_stops, (int)s->grad_stop_count, ((float)i + 0.5f) / (float)ARC_GRAD_LUT));
    const float grad_span = (s->grad_a1 != s->grad_a0) ? (s->grad_a1 - s->grad_a0) : 360.0f;
    const float grad_a0 = s->grad_a0;
    const float thick = ro - ri;

    /* Boundary rays. */
    const float a0r = s->a0 * ARC_DEG2RAD, a1r = s->a1 * ARC_DEG2RAD;
    const float c0 = cosf(a0r), sn0 = sinf(a0r), c1 = cosf(a1r), sn1 = sinf(a1r);
    const bool wedge_and = (sweep <= 180.0f);

    /* Round caps: discs of radius hw centred on the mid-radius at each boundary ray. */
    const bool caps = (!full && s->cap == ER_ARC_CAP_ROUND && thick > 0.0f);
    const float hw = thick * 0.5f;
    const float rm = (ro + ri) * 0.5f;
    const float e0x = s->cx + rm * c0, e0y = s->cy + rm * sn0;
    const float e1x = s->cx + rm * c1, e1y = s->cy + rm * sn1;
    const float cap_reach = hw + 1.0f;

    /* Segment gaps: precompute the two rays of every gap. */
    int ngaps = 0;
    float gc0[ARC_MAX_SEGMENTS], gs0[ARC_MAX_SEGMENTS], gc1[ARC_MAX_SEGMENTS], gs1[ARC_MAX_SEGMENTS];
    if (s->segments > 1U && s->gap_deg > 0.0f)
    {
        int nseg = (int)s->segments;
        if (nseg > ARC_MAX_SEGMENTS)
            nseg = ARC_MAX_SEGMENTS;
        const float span = s->seg_a1 - s->seg_a0;
        const float seg_len = (span - (float)(nseg - 1) * s->gap_deg) / (float)nseg;
        if (seg_len > 0.0f)
        {
            for (int k = 1; k < nseg; k++)
            {
                const float g0 = s->seg_a0 + (float)k * (seg_len + s->gap_deg) - s->gap_deg;
                const float g1 = g0 + s->gap_deg;
                gc0[ngaps] = cosf(g0 * ARC_DEG2RAD);
                gs0[ngaps] = sinf(g0 * ARC_DEG2RAD);
                gc1[ngaps] = cosf(g1 * ARC_DEG2RAD);
                gs1[ngaps] = sinf(g1 * ARC_DEG2RAD);
                ngaps++;
            }
        }
    }

    /* Row range. */
    int y_start = (int)floorf(s->cy - ro - 1.0f);
    int y_end = (int)ceilf(s->cy + ro + 1.0f); /* exclusive */
    if (y_start < s->clip_y0)
        y_start = s->clip_y0;
    if (y_end > s->clip_y1)
        y_end = s->clip_y1;
    if (y_start >= y_end)
        return;

    /* Row-span cache entries for the four radii (outer ± 0.5, inner ± 0.5). Rows above and below the centre
     * may sit at different fractional offsets from it (they coincide when the centre is on a pixel edge or
     * centre, the layout-box case), so each radius is looked up once per side — never per row, which with a
     * small cache could thrash a rebuild into every scanline. */
    const float fp0 = 0.5f - s->cy, fn0 = s->cy - 0.5f;
    const float frac_pos = fp0 - floorf(fp0);
    const float frac_neg = fn0 - floorf(fn0);
    const float r_oo = ro + 0.5f, r_oi = ro - 0.5f, r_io = ri + 0.5f, r_ii = ri - 0.5f;
    const ArcSpanEntry* ep[4];
    const ArcSpanEntry* en[4];
    const float rr[4] = {r_oo, r_oi, r_io, r_ii};
    const bool ruse[4] = {true, r_oi > 0.0f, ri > 0.0f, r_ii > 0.0f};
    for (int i = 0; i < 4; i++)
    {
        ep[i] = ruse[i] ? span_entry(rr[i], frac_pos) : NULL;
        en[i] = ruse[i] ? ((frac_neg == frac_pos) ? ep[i] : span_entry(rr[i], frac_neg)) : NULL;
    }

    for (int y = y_start; y < y_end; y++)
    {
        const float dy = (float)y + 0.5f - s->cy;
        const ArcSpanEntry* const* e = (dy < 0.0f) ? en : ep;
        const float h_oo = half_chord(e[0], r_oo, dy);
        if (h_oo < 0.0f)
            continue; /* row misses the circle */
        const float h_oi = ruse[1] ? half_chord(e[1], r_oi, dy) : -1.0f;
        const float h_io = ruse[2] ? half_chord(e[2], r_io, dy) : -1.0f;
        const float h_ii = ruse[3] ? half_chord(e[3], r_ii, dy) : -1.0f;

        /* Column intervals (pixel-centre x): the whole chord, or the two sides of the hole. */
        float ia0, ib0, ia1, ib1;
        int nint;
        if (h_ii > 0.0f)
        {
            ia0 = s->cx - h_oo;
            ib0 = s->cx - h_ii;
            ia1 = s->cx + h_ii;
            ib1 = s->cx + h_oo;
            nint = 2;
        }
        else
        {
            ia0 = s->cx - h_oo;
            ib0 = s->cx + h_oo;
            ia1 = ib1 = 0.0f;
            nint = 1;
        }

        for (int it = 0; it < nint; it++)
        {
            const float xa = (it == 0) ? ia0 : ia1;
            const float xb = (it == 0) ? ib0 : ib1;
            int xs = (int)floorf(xa - 0.5f);
            int xe = (int)ceilf(xb - 0.5f) + 1; /* exclusive */
            if (xs < s->clip_x0)
                xs = s->clip_x0;
            if (xe > s->clip_x1)
                xe = s->clip_x1;
            for (int cx0 = xs; cx0 < xe; cx0 += ARC_ROW_CHUNK)
            {
                int cx1 = cx0 + ARC_ROW_CHUNK;
                if (cx1 > xe)
                    cx1 = xe;
                bool any = false;
                for (int x = cx0; x < cx1; x++)
                {
                    const float dx = (float)x + 0.5f - s->cx;
                    const float adx = fabsf(dx);
                    uint32_t* out = &row[x - cx0];
                    *out = 0U;
                    if (adx > h_oo || adx < h_ii)
                        continue;

                    /* Ring coverage: exact distance only on the two fringes. */
                    float d = -1.0f;
                    float cov = 1.0f;
                    if (adx >= h_oi)
                    {
                        d = sqrtf(dx * dx + dy * dy);
                        cov = clamp01(ro + 0.5f - d);
                    }
                    if (ri > 0.0f && adx <= h_io)
                    {
                        if (d < 0.0f)
                            d = sqrtf(dx * dx + dy * dy);
                        cov *= clamp01(d - ri + 0.5f);
                    }
                    if (cov <= 0.0f)
                        continue;

                    /* Sweep: two half-planes through the centre; the cross products are signed distances. */
                    if (!full)
                    {
                        const float q0 = clamp01(0.5f + (c0 * dy - sn0 * dx));
                        const float q1 = clamp01(0.5f + (sn1 * dx - c1 * dy));
                        const float wc = wedge_and ? (q0 < q1 ? q0 : q1) : (q0 > q1 ? q0 : q1);
                        float c = cov * wc;
                        if (caps)
                        {
                            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                            if (fabsf(px - e0x) <= cap_reach && fabsf(py - e0y) <= cap_reach)
                            {
                                const float dd = sqrtf((px - e0x) * (px - e0x) + (py - e0y) * (py - e0y));
                                const float cc = clamp01(hw + 0.5f - dd);
                                if (cc > c)
                                    c = cc;
                            }
                            if (fabsf(px - e1x) <= cap_reach && fabsf(py - e1y) <= cap_reach)
                            {
                                const float dd = sqrtf((px - e1x) * (px - e1x) + (py - e1y) * (py - e1y));
                                const float cc = clamp01(hw + 0.5f - dd);
                                if (cc > c)
                                    c = cc;
                            }
                        }
                        cov = c;
                        if (cov <= 0.0f)
                            continue;
                    }

                    /* Segment gaps (each a thin wedge, always < 180°: intersection of its two half-planes). */
                    for (int g = 0; g < ngaps; g++)
                    {
                        const float q0 = clamp01(0.5f + (gc0[g] * dy - gs0[g] * dx));
                        const float q1 = clamp01(0.5f + (gs1[g] * dx - gc1[g] * dy));
                        const float gq = q0 < q1 ? q0 : q1;
                        if (gq > 0.0f)
                            cov *= (1.0f - gq);
                    }
                    if (cov <= 0.0f)
                        continue;

                    /* Paint. */
                    uint32_t pm;
                    if (conic)
                    {
                        float ang = fast_atan2(dy, dx) * (1.0f / ARC_DEG2RAD) - grad_a0;
                        ang -= 360.0f * floorf(ang / 360.0f); /* [0, 360) past the ramp's start */
                        /* Outside the ramp (a round cap, the AA fringe of a boundary ray): pin to the nearer
                         * end instead of wrapping the last stop's colour onto the first cap. */
                        if (ang > grad_span)
                            ang = ((ang - grad_span) < (360.0f - ang)) ? grad_span : 0.0f;
                        int idx = (int)((ang / grad_span) * (float)ARC_GRAD_LUT);
                        if (idx < 0)
                            idx = 0;
                        else if (idx >= ARC_GRAD_LUT)
                            idx = ARC_GRAD_LUT - 1;
                        pm = lut[idx];
                    }
                    else if (radial)
                    {
                        if (d < 0.0f)
                            d = sqrtf(dx * dx + dy * dy);
                        int idx = (int)(((d - ri) / thick) * (float)ARC_GRAD_LUT);
                        if (idx < 0)
                            idx = 0;
                        else if (idx >= ARC_GRAD_LUT)
                            idx = ARC_GRAD_LUT - 1;
                        pm = lut[idx];
                    }
                    else
                    {
                        pm = solid;
                    }
                    *out = scale_premul(pm, (uint32_t)(cov * 255.0f + 0.5f));
                    any = true;
                }
                if (any)
                    er_blit_blend(row, (int)(sizeof(uint32_t) * (size_t)(cx1 - cx0)), 255U, cx0, y, cx1 - cx0, 1);
            }
        }
    }
}
