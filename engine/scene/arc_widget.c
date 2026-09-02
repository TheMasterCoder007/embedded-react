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
 - Arc widget (ER_NODE_ARC): node-level glue over the analytic sector rasterizer.
 *
 * One node draws a dial / gauge / progress ring: an optional wide backing band, the track over the whole
 * sweep, the value indicator over [start, value], and a knob at the value end. The value is a first-class
 * animatable property (ER_PROP_ARC_VALUE), and a value change damages only the sub-arc it swept plus the
 * knob's two footprints — not the node box — so a native ramp or a finger drag repaints a sliver.
 ---------------------------------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------------------------------
 - Includes
 ---------------------------------------------------------------------------------------------------------------------*/

#include "arc_widget.h"
#include "arc.h"
#include "image_scaler.h"
#include "renderer_internal.h"
#include "rrect.h"
#include <math.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Macros
 ---------------------------------------------------------------------------------------------------------------------*/

#define ARC_DEG2RAD 0.017453292519943295f
#define ARC_RAD2DEG 57.29577951308232f

/** @brief Minimum touch slop either side of the ring (px) — fingers are wider than a 4 px track. */
#define ARC_HIT_SLOP_MIN 8

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/** @brief Default colours, mirrored from the ERProps docs. */
static inline uint32_t col_or(uint32_t c, uint32_t dflt)
{
    return c ? c : dflt;
}

/**
 * @brief Whether this arc's conic indicator ramp is anchored to the BAND (the lit indicator) rather than
 *        to the whole sweep.
 *
 * A RANGE band ramps across ITSELF, so it always covers exactly what is lit however wide it is; a
 * single-ended arc anchors to the whole sweep instead, so a colour belongs to a position on the dial and
 * does not shift as the value grows.
 *
 * This is the single source of truth for both the paint and the damage. A band-anchored ramp re-maps
 * every pixel between the indicator's ends whenever either end moves, so if the two ever disagreed a
 * value change would leave stale colours behind — keep them reading the same predicate.
 */
static bool arc_grad_band_anchored(const ERArcProps* p)
{
    if (p->gradient_type != ER_GRADIENT_CONIC || p->gradient_stop_count < 2U)
        return false;
    return p->range != 0U;
}

/** @brief The first child a knob-child arc anchors (display:none children are skipped). */
static ERNode* knob_child(const ERNode* n)
{
    uint16_t t = n->first_child_tag;
    while (t != ER_INVALID_TAG)
    {
        ERNode* c = er_get_node(t);
        if (!c)
            return NULL;
        if (c->layout.display != ER_DISPLAY_NONE)
            return c;
        t = c->next_sibling_tag;
    }
    return NULL;
}

/** @brief Unions a node-local rect into the node's pending vec_dirty rect (or starts one). */
static void union_local_dirty(ERNode* n, int x0, int y0, int x1, int y1)
{
    if (n->vec_has_dirty)
    {
        const int ox0 = n->vec_dirty_x, oy0 = n->vec_dirty_y;
        const int ox1 = ox0 + n->vec_dirty_w, oy1 = oy0 + n->vec_dirty_h;
        if (ox0 < x0)
            x0 = ox0;
        if (oy0 < y0)
            y0 = oy0;
        if (ox1 > x1)
            x1 = ox1;
        if (oy1 > y1)
            y1 = oy1;
    }
    n->vec_dirty_x = (int16_t)x0;
    n->vec_dirty_y = (int16_t)y0;
    n->vec_dirty_w = (int16_t)(x1 - x0);
    n->vec_dirty_h = (int16_t)(y1 - y0);
    n->vec_has_dirty = (x1 > x0 && y1 > y0);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_arc_geom(const ERNode* n, int px, int py, int w, int h, ERArcGeom* g)
{
    const ERArcProps* p = &n->props.arc;

    er_layout_content_box(&n->layout, &px, &py, &w, &h);

    const int side = (w < h) ? w : h;
    const float R = (float)side * 0.5f;

    float width = (float)p->width;
    if (width <= 0.0f)
    {
        width = (float)(side / 10);
        if (width < 2.0f)
            width = 2.0f;
    }
    if (width > R)
        width = R;
    float band = (float)(p->band_width > 0 ? p->band_width : 0);
    if (band > 2.0f * R)
        band = 2.0f * R;

    g->cx = (float)px + (float)w * 0.5f;
    g->cy = (float)py + (float)h * 0.5f;
    g->width = width;
    g->band = band;
    g->half_ext = ((band > width) ? band : width) * 0.5f;
    g->r_mid = R - g->half_ext;
    if (g->r_mid < 0.0f)
        g->r_mid = 0.0f;

    float sweep = p->sweep_angle;
    float a0 = p->start_angle;
    if (sweep <= 0.0f)
    {
        sweep = 270.0f;
        if (a0 == 0.0f)
            a0 = 135.0f;
    }
    if (sweep > 360.0f)
        sweep = 360.0f;
    g->a0 = a0;
    g->sweep = sweep;

    g->min = p->min;
    g->max = (p->max > p->min) ? p->max : (p->min + 100.0f);

    if (p->knob == ER_ARC_KNOB_CIRCLE || p->knob == ER_ARC_KNOB_IMAGE)
    {
        int ks = p->knob_size;
        if (ks <= 0)
            ks = (int)width + 8;
        g->knob_size = ks;
    }
    else
    {
        g->knob_size = 0;
    }
}

float er_arc_value_frac(const ERArcGeom* g, float value)
{
    const float span = g->max - g->min;
    if (span <= 0.0f)
        return 0.0f;
    return clampf((value - g->min) / span, 0.0f, 1.0f);
}

float er_arc_value_angle(const ERArcGeom* g, float value)
{
    return g->a0 + g->sweep * er_arc_value_frac(g, value);
}

void er_arc_knob_center(const ERArcGeom* g, float value, float* kx, float* ky)
{
    const float a = er_arc_value_angle(g, value) * ARC_DEG2RAD;
    *kx = g->cx + g->r_mid * cosf(a);
    *ky = g->cy + g->r_mid * sinf(a);
}

int er_arc_refresh_overhang(ERNode* n)
{
    ERArcGeom g;
    er_arc_geom(n, 0, 0, (int)n->computed.w, (int)n->computed.h, &g);
    int over = 0;
    if (g.knob_size > 0)
    {
        const float reach = (float)g.knob_size * 0.5f + 1.0f; /* + AA fringe */
        const float o = reach - g.half_ext;
        if (o > 0.0f)
            over = (int)ceilf(o);
    }
    n->arc_overhang = (int16_t)over;
    return over;
}

void er_arc_render(ERNode* n, int px, int py, int w, int h)
{
    const ERArcProps* p = &n->props.arc;
    ERArcGeom g;
    er_arc_geom(n, px, py, w, h, &g);
    er_arc_refresh_overhang(n);

    /* Rasterize only where this commit is repainting: the node box (plus the knob overhang) intersected
     * with the active damage scissor — the whole point of the tight value-change damage. */
    const int over = (int)n->arc_overhang;
    int clx0 = px - over, cly0 = py - over, clx1 = px + w + over, cly1 = py + h + over;
    int gx, gy, gw, gh;
    if (er_get_clip_rect(&gx, &gy, &gw, &gh))
    {
        if (gx > clx0)
            clx0 = gx;
        if (gy > cly0)
            cly0 = gy;
        if (gx + gw < clx1)
            clx1 = gx + gw;
        if (gy + gh < cly1)
            cly1 = gy + gh;
    }
    if (clx1 <= clx0 || cly1 <= cly0)
    {
        n->arc_painted_value = n->arc_value;
        return;
    }

    ERArcSector s;
    memset(&s, 0, sizeof(s));
    s.cx = g.cx;
    s.cy = g.cy;
    s.cap = p->cap;
    s.clip_x0 = clx0;
    s.clip_y0 = cly0;
    s.clip_x1 = clx1;
    s.clip_y1 = cly1;
    if (p->segments > 1U)
    {
        s.segments = p->segments;
        s.seg_a0 = g.a0;
        s.seg_a1 = g.a0 + g.sweep;
        s.gap_deg = (p->gap_angle > 0.0f) ? p->gap_angle : 2.0f;
    }

    /* 1. Backing band (wider than the track, centred on the same mid-line). */
    if (g.band > 0.0f)
    {
        s.r_outer = g.r_mid + g.band * 0.5f;
        s.r_inner = g.r_mid - g.band * 0.5f;
        s.a0 = g.a0;
        s.a1 = g.a0 + g.sweep;
        s.color = col_or(p->band_color, 0x33000000U);
        s.grad_type = ER_GRADIENT_NONE;
        er_arc_fill_sector(&s);
    }

    /* 2. Track over the whole sweep. */
    s.r_outer = g.r_mid + g.width * 0.5f;
    s.r_inner = g.r_mid - g.width * 0.5f;
    s.a0 = g.a0;
    s.a1 = g.a0 + g.sweep;
    s.color = col_or(p->track_color, 0xFF3A3A3CU);
    s.grad_type = ER_GRADIENT_NONE;
    er_arc_fill_sector(&s);

    /* 3. Indicator. Single-ended it runs from the sweep start to the value; in RANGE mode it is the band
     *    BETWEEN the two values, so a dual-setpoint dial (a thermostat's AUTO lo..hi) is one node. */
    const float frac = er_arc_value_frac(&g, n->arc_value);
    const float frac0 = p->range ? er_arc_value_frac(&g, n->arc_value_start) : 0.0f;
    if (frac > frac0)
    {
        s.a0 = g.a0 + g.sweep * frac0;
        s.a1 = g.a0 + g.sweep * frac;
        s.color = col_or(p->indicator_color, 0xFF0A84FFU);
        if ((p->gradient_type == ER_GRADIENT_CONIC || p->gradient_type == ER_GRADIENT_RADIAL)
            && p->gradient_stop_count >= 2U)
        {
            s.grad_type = p->gradient_type;
            s.grad_stops = p->gradient_stops;
            s.grad_stop_count = p->gradient_stop_count;
            /* Where the ramp is anchored — see arc_grad_band_anchored(). BAND spans the lit indicator
             * (a thermostat's AUTO warm→cool, always covering exactly what is drawn); SWEEP spans the
             * whole dial (a progress ring's 0-100 scale, colours fixed to positions). */
            const bool band_grad = arc_grad_band_anchored(p);
            s.grad_a0 = band_grad ? s.a0 : g.a0;
            s.grad_a1 = band_grad ? s.a1 : (g.a0 + g.sweep);
        }
        er_arc_fill_sector(&s);
    }

    /* 4. Knob — one at the value end, plus one at the low end in RANGE mode. */
    if (g.knob_size > 0)
    {
        const int ends = p->range ? 2 : 1;
        for (int e = 0; e < ends; e++)
        {
            float kx, ky;
            er_arc_knob_center(&g, (e == 0) ? n->arc_value : n->arc_value_start, &kx, &ky);
            const int ks = g.knob_size;
            const int kx0 = (int)floorf(kx - (float)ks * 0.5f + 0.5f);
            const int ky0 = (int)floorf(ky - (float)ks * 0.5f + 0.5f);
            if (p->knob == ER_ARC_KNOB_CIRCLE)
            {
                const uint32_t fill = col_or(p->knob_color, 0xFFFFFFFFU);
                const uint32_t bc = col_or(p->knob_border_color, col_or(p->indicator_color, 0xFF0A84FFU));
                er_rrect_fill_bordered(fill, bc, (int)p->knob_border_width, kx0, ky0, ks, ks, ks / 2);
            }
            else /* ER_ARC_KNOB_IMAGE */
            {
                ERImageProps ip;
                memset(&ip, 0, sizeof(ip));
                memcpy(ip.image_name, p->image_name, sizeof(ip.image_name));
                ip.resize_mode = ER_RESIZE_CONTAIN;
                er_image_render(&ip, kx0, ky0, ks, ks);
            }
        }
    }

    n->arc_painted_value = n->arc_value;
}

/**
 * @brief Writes ONE end of the arc and records the damage that move implies.
 *
 * Assignment only — the caller has already resolved every constraint, so this never moves the other end
 * and never re-enters. Keeping the write separate from the constraint solving is what makes the push in
 * arc_apply_end() terminate: an earlier version pushed by calling itself, and because the slot was not
 * assigned until the recursion unwound, each level re-read the stale endpoint and bounced back — a
 * min_span wide enough to move BOTH ends recursed until the stack ran out.
 *
 * @param[in,out] n    Arc node.
 * @param[in]     low  true for the RANGE band's low end (arc_value_start), false for the value end.
 * @param[in]     v    Final value for that end.
 *
 * @return true when the stored value actually changed.
 */
static bool arc_set_end(ERNode* n, bool low, float v)
{
    float* const slot = low ? &n->arc_value_start : &n->arc_value;
    const float old = *slot;
    if (v == old)
        return false;
    *slot = v;

    const int w = (int)n->computed.w, h = (int)n->computed.h;
    if (w <= 0 || h <= 0)
        return true; /* not laid out yet: the first paint covers everything */
    ERArcGeom g;
    er_arc_geom(n, 0, 0, w, h, &g);

    /* Damage: normally just the sub-arc this end swept (padded for the AA fringe and a round cap), plus
     * that end's knob in its old and new spots. Both radii of the WIDEST ring, so a band under the
     * indicator is repainted where the track/indicator fringe lands on it. The other end is damaged by
     * its own arc_set_end call when a push moves it too. */
    float a_old = er_arc_value_angle(&g, old);
    float a_new = er_arc_value_angle(&g, v);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    const float pad = 1.5f + ((n->props.arc.cap == ER_ARC_CAP_ROUND) ? g.width * 0.5f : 0.0f);

    /* EXCEPTION: a BAND-anchored conic ramp moves with the indicator, so shifting either end re-maps the
     * colour of every pixel between them — the swept sliver is NOT all that changed. Widen to cover the
     * old band and the new band (they share the end that did not move, so their union runs from it to
     * whichever of the moved end's two positions is further). Without this the rest of the band keeps the
     * previous frame's colours until something forces a full repaint, which on a panel reads as a
     * gradient that only catches up when the finger lifts. A solid, radial, or SWEEP-anchored paint
     * re-maps nothing, so those keep the tight sliver. */
    if (arc_grad_band_anchored(&n->props.arc))
    {
        /* The other end. On a single-ended arc the indicator starts at the sweep start, which never
         * moves. When a push moves BOTH ends, each call reads the other's already-final value, so the
         * two damage rects together still cover the old band and the new one. */
        const float a_fix = n->props.arc.range ? er_arc_value_angle(&g, low ? n->arc_value : n->arc_value_start) : g.a0;
        /* Span the extremes of all three angles, so this is right whichever direction the end moved. */
        float lo_a = a_old, hi_a = a_old;
        if (a_new < lo_a)
            lo_a = a_new;
        if (a_new > hi_a)
            hi_a = a_new;
        if (a_fix < lo_a)
            lo_a = a_fix;
        if (a_fix > hi_a)
            hi_a = a_fix;
        a_old = lo_a;
        a_new = hi_a;
    }
    er_arc_sector_bbox(g.cx, g.cy, g.r_mid - g.half_ext, g.r_mid + g.half_ext, a_old, a_new, pad, &x0, &y0, &x1, &y1);
    if (g.knob_size > 0)
    {
        const float kr = (float)g.knob_size * 0.5f + 1.5f;
        const float ends[2] = {old, v};
        for (int e = 0; e < 2; e++)
        {
            float kx, ky;
            er_arc_knob_center(&g, ends[e], &kx, &ky);
            if (kx - kr < x0)
                x0 = kx - kr;
            if (ky - kr < y0)
                y0 = ky - kr;
            if (kx + kr > x1)
                x1 = kx + kr;
            if (ky + kr > y1)
                y1 = ky + kr;
        }
    }
    union_local_dirty(n, (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1));

    /* An anchored child is a real node at a new position: re-run layout so it is re-anchored and the
     * subtree prune bounds cover its new spot (its move then damages itself through the usual path). */
    if (n->props.arc.knob == ER_ARC_KNOB_CHILD)
        er_request_layout_pass();
    return true;
}

/**
 * @brief Resolves the range constraints for a requested endpoint value, then writes what they allow.
 *
 * RANGE mode: the ends bound each other, so a drag can never invert the band (which would paint a sector
 * running the wrong way round the dial). With no min_span they may meet and a drag stops at its
 * neighbour. With one set, the pair keeps that separation and a drag that closes the gap PUSHES the far
 * end along rather than stopping dead — what a dual-setpoint control wants (a thermostat's minimum
 * heat/cool spread), and what its +/- steppers already do.
 *
 * Both final values are computed here and written by arc_set_end() below, so at most two writes happen
 * and neither can trigger another: no recursion, and nothing reads a half-updated endpoint.
 *
 * @param[in,out] n    Arc node.
 * @param[in]     low  true to move the band's low end (arc_value_start), false for the value end.
 * @param[in]     v    Requested value; clamped to the range and to what min_span leaves free.
 *
 * @return true when either end actually changed.
 */
static bool arc_apply_end(ERNode* n, bool low, float v)
{
    if (!n || n->type != ER_NODE_ARC)
        return false;
    ERArcGeom g;
    er_arc_geom(n, 0, 0, (int)n->computed.w, (int)n->computed.h, &g);
    v = clampf(v, g.min, g.max);

    bool changed = false;
    if (n->props.arc.range)
    {
        /* A min_span wider than the range itself cannot be honoured — clamping it here (rather than
         * letting the endpoint arithmetic below run with it) is what keeps both ends inside [min, max].
         * Left unclamped, a min_span of 200 on a 0..100 arc stored 200 in the high end and -100 in the
         * low one, which then reached onChange and the animation driver as out-of-range values. */
        float ms = n->props.arc.min_span;
        if (ms < 0.0f)
            ms = 0.0f;
        const float span = g.max - g.min;
        if (ms > span)
            ms = span;

        if (ms > 0.0f)
        {
            /* Neither end may sit closer than min_span to the far end of the RANGE, or there would be no
             * room left for its partner. Push the other end first, so this end's damage sees it settled. */
            if (low)
            {
                if (v > g.max - ms)
                    v = g.max - ms;
                if (n->arc_value < v + ms)
                    changed |= arc_set_end(n, false, v + ms);
            }
            else
            {
                if (v < g.min + ms)
                    v = g.min + ms;
                if (n->arc_value_start > v - ms)
                    changed |= arc_set_end(n, true, v - ms);
            }
        }
        else if (low && v > n->arc_value)
        {
            v = n->arc_value;
        }
        else if (!low && v < n->arc_value_start)
        {
            v = n->arc_value_start;
        }
    }

    changed |= arc_set_end(n, low, v);
    return changed;
}

bool er_arc_apply_value(ERNode* n, float value)
{
    return arc_apply_end(n, false, value);
}

bool er_arc_apply_value_start(ERNode* n, float value)
{
    return arc_apply_end(n, true, value);
}

bool er_arc_hit(const ERNode* n, int x, int y)
{
    ERArcGeom g;
    er_arc_geom(n, (int)n->computed.x, (int)n->computed.y, (int)n->computed.w, (int)n->computed.h, &g);
    const float px = (float)x + 0.5f, py = (float)y + 0.5f;
    const float dx = px - g.cx, dy = py - g.cy;
    const float d = sqrtf(dx * dx + dy * dy);

    /* Knob first: a big knob is the natural grab point and may reach past the ring. RANGE mode has one
     * per end. */
    if (g.knob_size > 0)
    {
        const int ends = n->props.arc.range ? 2 : 1;
        const float kr = (float)g.knob_size * 0.5f + 4.0f;
        for (int e = 0; e < ends; e++)
        {
            float kx, ky;
            er_arc_knob_center(&g, (e == 0) ? n->arc_value : n->arc_value_start, &kx, &ky);
            if ((px - kx) * (px - kx) + (py - ky) * (py - ky) <= kr * kr)
                return true;
        }
    }

    float slop = g.half_ext;
    if (g.knob_size > 0 && (float)g.knob_size * 0.5f > slop)
        slop = (float)g.knob_size * 0.5f;
    if (slop < (float)ARC_HIT_SLOP_MIN)
        slop = (float)ARC_HIT_SLOP_MIN;
    if (d < g.r_mid - g.half_ext - slop || d > g.r_mid + g.half_ext + slop)
        return false;
    if (g.sweep >= 360.0f)
        return true;

    /* Inside the sweep (plus the slop expressed as an angle at this radius)? */
    float rel = atan2f(dy, dx) * ARC_RAD2DEG - g.a0;
    rel -= 360.0f * floorf(rel / 360.0f);
    const float eps = (g.r_mid > 1.0f) ? (asinf(clampf(slop / g.r_mid, 0.0f, 1.0f)) * ARC_RAD2DEG) : 180.0f;
    return (rel <= g.sweep + eps) || (rel >= 360.0f - eps);
}

float er_arc_value_at(ERNode* n, int x, int y, bool anti_wrap)
{
    ERArcGeom g;
    er_arc_geom(n, (int)n->computed.x, (int)n->computed.y, (int)n->computed.w, (int)n->computed.h, &g);
    const float dx = (float)x + 0.5f - g.cx, dy = (float)y + 0.5f - g.cy;
    float rel = atan2f(dy, dx) * ARC_RAD2DEG - g.a0;
    rel -= 360.0f * floorf(rel / 360.0f); /* [0, 360) past the start */

    /* A point ON the ring is unambiguous — use it, however far it is from the last sample (a fast finger,
     * or a move coalesced across several frames, legitimately jumps a long way in one dispatch).
     * Only a point in the UNSWEPT GAP is ambiguous, and there the answer is not the geometrically nearer
     * end but the end the finger came FROM: sliding off the max end and on round the gap must hold at max,
     * not snap to min once past the halfway point. A fresh touch has no history, so it takes the nearer. */
    float frac;
    if (rel <= g.sweep)
        frac = rel / g.sweep;
    else if (anti_wrap)
        frac = (n->arc_drag_frac > 0.5f) ? 1.0f : 0.0f;
    else
        frac = ((rel - g.sweep) < (360.0f - rel)) ? 1.0f : 0.0f;
    n->arc_drag_frac = frac;

    float v = g.min + frac * (g.max - g.min);
    const float step = (n->props.arc.step > 0.0f) ? n->props.arc.step : 1.0f;
    v = g.min + floorf((v - g.min) / step + 0.5f) * step;
    return clampf(v, g.min, g.max);
}

bool er_arc_grab_low(const ERNode* n, int x, int y)
{
    if (!n || n->type != ER_NODE_ARC || !n->props.arc.range)
        return false;
    ERArcGeom g;
    er_arc_geom(n, (int)n->computed.x, (int)n->computed.y, (int)n->computed.w, (int)n->computed.h, &g);
    /* Compare in SWEEP FRACTION rather than value units so the choice is the visually nearer knob
     * regardless of the range's scale. */
    const float dx = (float)x + 0.5f - g.cx, dy = (float)y + 0.5f - g.cy;
    float rel = atan2f(dy, dx) * ARC_RAD2DEG - g.a0;
    rel -= 360.0f * floorf(rel / 360.0f);
    const float f = (rel <= g.sweep) ? (rel / g.sweep) : (((rel - g.sweep) < (360.0f - rel)) ? 1.0f : 0.0f);
    const float f_hi = er_arc_value_frac(&g, n->arc_value);
    const float f_lo = er_arc_value_frac(&g, n->arc_value_start);
    return fabsf(f - f_lo) < fabsf(f - f_hi);
}

/**
 * @brief Shifts a whole subtree by (ddx, ddy) — the knob child and everything laid out under it.
 *
 * Walks pre-order through the parent/sibling links rather than an explicit stack, so it has no depth or
 * breadth cap. An earlier version pushed pending children onto a fixed 64-slot array and simply stopped
 * pushing once it filled: a knob subtree bigger than that was left half-moved, split between the value's
 * old and new positions.
 *
 * @param[in,out] root  Subtree root (moved along with its descendants).
 * @param[in]     ddx   X delta in pixels.
 * @param[in]     ddy   Y delta in pixels.
 */
static void arc_shift_subtree(ERNode* root, int16_t ddx, int16_t ddy)
{
    ERNode* m = root;
    while (m)
    {
        m->computed.x = (int16_t)(m->computed.x + ddx);
        m->computed.y = (int16_t)(m->computed.y + ddy);
        m->animated.x = (int16_t)(m->animated.x + ddx);
        m->animated.y = (int16_t)(m->animated.y + ddy);

        ERNode* next = er_get_node(m->first_child_tag);
        if (next)
        {
            m = next;
            continue;
        }
        /* Leaf: move to the next sibling, climbing out of finished branches — but never past the root,
         * whose own siblings are not part of this subtree. */
        while (m != root)
        {
            if (er_get_node(m->next_sibling_tag))
                break;
            m = er_get_node(m->parent_tag);
            if (!m)
                return; /* broken parent link: stop rather than walk off the tree */
        }
        if (m == root)
            return;
        m = er_get_node(m->next_sibling_tag);
    }
}

void er_arc_anchor_child(ERNode* n)
{
    if (!n || n->type != ER_NODE_ARC || n->props.arc.knob != ER_ARC_KNOB_CHILD)
        return;
    ERNode* c = knob_child(n);
    if (!c)
        return;
    ERArcGeom g;
    er_arc_geom(n, (int)n->computed.x, (int)n->computed.y, (int)n->computed.w, (int)n->computed.h, &g);
    float kx, ky;
    er_arc_knob_center(&g, n->arc_value, &kx, &ky);
    const int16_t nx = (int16_t)floorf(kx - (float)c->computed.w * 0.5f + 0.5f);
    const int16_t ny = (int16_t)floorf(ky - (float)c->computed.h * 0.5f + 0.5f);
    const int16_t ddx = (int16_t)(nx - c->computed.x);
    const int16_t ddy = (int16_t)(ny - c->computed.y);
    if (ddx == 0 && ddy == 0)
        return;
    arc_shift_subtree(c, ddx, ddy);
}
