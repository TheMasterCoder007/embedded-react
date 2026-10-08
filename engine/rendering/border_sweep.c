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

#include "border_sweep.h"
#include "renderer_internal.h"
#include <math.h>

#define SWEEP_ROW_MAX 512
#define SWEEP_SQRT2 1.41421356f
#define SWEEP_PI 3.14159265f
/* The head fades in over this much of the perimeter ahead of it, so it has no hard front edge. */
#define SWEEP_LEAD 0.01f

static uint32_t s_row[ERUI_RENDER_WORKERS][SWEEP_ROW_MAX];

/** Signed distance from (px, py) to a rounded rect centred on the origin: negative inside. */
static float sd_rrect(float px, float py, float hw, float hh, float r)
{
    const float qx = fabsf(px) - (hw - r);
    const float qy = fabsf(py) - (hh - r);
    const float ox = qx > 0.0f ? qx : 0.0f;
    const float oy = qy > 0.0f ? qy : 0.0f;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

/**
 * Where (px, py) sits along the ring's centre line, as a fraction of its perimeter: clockwise from the
 * left end of the top edge. `a` and `b` are the half-lengths of the straight edges, `r` the corner radius.
 */
static float perimeter_at(float px, float py, float a, float b, float r)
{
    const float quarter = r * SWEEP_PI * 0.5f;
    const float total = 4.0f * a + 4.0f * b + 4.0f * quarter;
    if (total <= 0.0f)
        return 0.0f;
    float s;
    if (fabsf(px) <= a && py < 0.0f)
        s = px + a;
    else if (px > a && py < -b)
        s = 2.0f * a + r * (atan2f(py + b, px - a) + SWEEP_PI * 0.5f);
    else if (px > 0.0f && fabsf(py) <= b)
        s = 2.0f * a + quarter + (py + b);
    else if (px > a && py > b)
        s = 2.0f * a + 2.0f * b + quarter + r * atan2f(py - b, px - a);
    else if (fabsf(px) <= a && py > 0.0f)
        s = 2.0f * a + 2.0f * b + 2.0f * quarter + (a - px);
    else if (px < -a && py > b)
        s = 4.0f * a + 2.0f * b + 2.0f * quarter + r * (atan2f(py - b, px + a) - SWEEP_PI * 0.5f);
    else if (px < 0.0f && fabsf(py) <= b)
        s = 4.0f * a + 2.0f * b + 3.0f * quarter + (b - py);
    else
        s = 4.0f * a + 4.0f * b + 3.0f * quarter + r * (atan2f(py + b, px + a) + SWEEP_PI);
    return s / total;
}

/** Brightness at perimeter position t: 1 at the head, fading to 0 over `length` behind it. */
static float sweep_alpha(float t, float phase, float length)
{
    float d = phase - t;
    d -= floorf(d); /* distance behind the head, 0..1 */
    if (d > 1.0f - SWEEP_LEAD)
        return (d - (1.0f - SWEEP_LEAD)) / SWEEP_LEAD;
    if (d >= length)
        return 0.0f;
    const float f = 1.0f - d / length;
    return f * f;
}

int er_border_sweep_reach(int radius, int width)
{
    return (radius > width ? radius : width) + 2;
}

/** Colour of the ring at a pixel, from its position relative to the box centre: straight ARGB. */
typedef uint32_t (*RingShader)(float px, float py, const void* ctx);

typedef struct
{
    float a, b, r; /* the ring's centre line: straight half-lengths and corner radius */
    float phase, length;
    uint32_t argb;
} SweepShader;

static uint32_t sweep_shade(float px, float py, const void* ctx)
{
    const SweepShader* s = ctx;
    const float k = sweep_alpha(perimeter_at(px, py, s->a, s->b, s->r), s->phase, s->length);
    const uint32_t a8 = (uint32_t)((float)(s->argb >> 24) * k + 0.5f);
    return (a8 << 24) | (s->argb & 0xFFFFFFU);
}

typedef struct
{
    float from; /* degrees, CSS conic `from`: 0 = up, clockwise */
    const ERGradientStop* stops;
    int count;
} ConicShader;

/** Straight-alpha lerp of two ARGB colours. */
static uint32_t lerp_argb(uint32_t c0, uint32_t c1, float f)
{
    uint32_t out = 0U;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const float v0 = (float)((c0 >> shift) & 0xFFU), v1 = (float)((c1 >> shift) & 0xFFU);
        out |= (uint32_t)(v0 + (v1 - v0) * f + 0.5f) << shift;
    }
    return out;
}

/** The colour at `t` along ascending stops, held flat before the first and after the last. */
static uint32_t stops_at(const ERGradientStop* stops, int count, float t)
{
    if (t <= stops[0].position)
        return stops[0].color;
    for (int i = 1; i < count; i++)
    {
        if (t <= stops[i].position)
        {
            const float span = stops[i].position - stops[i - 1].position;
            return lerp_argb(
                stops[i - 1].color, stops[i].color, span > 0.0f ? (t - stops[i - 1].position) / span : 1.0f);
        }
    }
    return stops[count - 1].color;
}

static uint32_t conic_shade(float px, float py, const void* ctx)
{
    const ConicShader* s = ctx;
    float t = (atan2f(px, -py) * (180.0f / SWEEP_PI) - s->from) / 360.0f;
    t -= floorf(t);
    return stops_at(s->stops, s->count, t);
}

typedef struct
{
    float cx, cy;   /* gradient centre, relative to the box centre */
    float irx, iry; /* reciprocal ellipse radii */
    const ERGradientStop* stops;
    int count;
} RadialShader;

static uint32_t radial_shade(float px, float py, const void* ctx)
{
    const RadialShader* s = ctx;
    const float dx = (px - s->cx) * s->irx, dy = (py - s->cy) * s->iry;
    return stops_at(s->stops, s->count, sqrtf(dx * dx + dy * dy));
}

/** Rasterises the anti-aliased ring `width` px inside the rounded box, coloured by `shade`. */
static void ring_render(int x, int y, int w, int h, int radius, int width, RingShader shade, const void* ctx)
{
    const int max_r = (w < h ? w : h) / 2;
    const int r = radius < 0 ? 0 : (radius > max_r ? max_r : radius);
    const float hw = (float)w * 0.5f, hh = (float)h * 0.5f;
    const float bw = (float)width;
    const float inner_r = (float)r > bw ? (float)r - bw : 0.0f;

    int cx0 = x, cy0 = y, cx1 = x + w, cy1 = y + h;
    int gx, gy, gw, gh;
    if (er_get_clip_rect(&gx, &gy, &gw, &gh))
    {
        if (gx > cx0)
            cx0 = gx;
        if (gy > cy0)
            cy0 = gy;
        if (gx + gw < cx1)
            cx1 = gx + gw;
        if (gy + gh < cy1)
            cy1 = gy + gh;
    }
    if (cx0 >= cx1 || cy0 >= cy1)
        return;

    uint32_t* const row = s_row[er_render_worker_id()];
    const int reach = er_border_sweep_reach(r, width);
    for (int sy = cy0; sy < cy1; sy++)
    {
        const int ry = sy - y;
        const bool band = ry < reach || ry >= h - reach;
        /* A row clear of the top and bottom bands holds the ring only near its left and right edges. */
        const int spans[2][2] = {{0, band ? w : reach}, {band ? w : w - reach, w}};
        for (int k = 0; k < (band ? 1 : 2); k++)
        {
            int s0 = x + spans[k][0], s1 = x + spans[k][1];
            if (s0 < cx0)
                s0 = cx0;
            if (s1 > cx1)
                s1 = cx1;
            for (int c0 = s0; c0 < s1; c0 += SWEEP_ROW_MAX)
            {
                const int c1 = (s1 - c0) < SWEEP_ROW_MAX ? s1 : c0 + SWEEP_ROW_MAX;
                bool any = false;
                for (int sx = c0; sx < c1; sx++)
                {
                    const float px = (float)(sx - x) + 0.5f - hw, py = (float)ry + 0.5f - hh;
                    float cov = 0.5f - sd_rrect(px, py, hw, hh, (float)r);
                    cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
                    if (cov > 0.0f)
                    {
                        float in = 0.5f + sd_rrect(px, py, hw - bw, hh - bw, inner_r);
                        cov *= in < 0.0f ? 0.0f : (in > 1.0f ? 1.0f : in);
                    }
                    uint32_t p = 0U;
                    if (cov > 0.0f)
                    {
                        const uint32_t c = shade(px, py, ctx);
                        const uint32_t a8 = (uint32_t)((float)(c >> 24) * cov + 0.5f);
                        if (a8 > 0U)
                        {
                            p = (a8 << 24) | (((((c >> 16) & 0xFFU) * a8 + 127U) / 255U) << 16)
                                | (((((c >> 8) & 0xFFU) * a8 + 127U) / 255U) << 8) | (((c & 0xFFU) * a8 + 127U) / 255U);
                            any = true;
                        }
                    }
                    row[sx - c0] = p;
                }
                if (any)
                    er_blit_blend(row, (c1 - c0) * (int)sizeof(uint32_t), 255, c0, sy, c1 - c0, 1);
            }
        }
    }
}

void er_border_sweep_render(int x, int y, int w, int h, int radius, int width, uint32_t argb, float phase, float length)
{
    if (w <= 0 || h <= 0 || width <= 0 || (argb >> 24) == 0U || length <= 0.0f)
        return;
    const int max_r = (w < h ? w : h) / 2;
    const int r = radius < 0 ? 0 : (radius > max_r ? max_r : radius);
    const float mid = (float)width * 0.5f;
    float cr = (float)r - mid;
    if (cr < 0.0f)
        cr = 0.0f;
    const SweepShader s = {(float)w * 0.5f - mid - cr, (float)h * 0.5f - mid - cr, cr, phase, length, argb};
    ring_render(x, y, w, h, r, width, sweep_shade, &s);
}

void er_border_conic_render(
    int x, int y, int w, int h, int radius, int width, float from, const ERGradientStop* stops, int count)
{
    if (w <= 0 || h <= 0 || width <= 0 || count < 1)
        return;
    const ConicShader s = {from, stops, count};
    ring_render(x, y, w, h, radius, width, conic_shade, &s);
}

void er_border_radial_render(
    int x, int y, int w, int h, int radius, int width, float size, float angle, const ERGradientStop* stops, int count)
{
    if (w <= 0 || h <= 0 || width <= 0 || count < 1 || size <= 0.0f)
        return;
    const float turn = angle / 360.0f - floorf(angle / 360.0f);
    const float pos = turn < 0.5f ? turn * 2.0f : 2.0f - turn * 2.0f;
    const float bw = (float)w * size, bh = (float)h * size;
    /* background-position p puts the background's origin at (box - background) * p; farthest-corner of an
       ellipse centred in its box has radii half the box times sqrt 2. */
    const RadialShader s = {((float)w - bw) * pos + bw * 0.5f - (float)w * 0.5f,
                            ((float)h - bh) * pos + bh * 0.5f - (float)h * 0.5f,
                            1.0f / (bw * 0.5f * SWEEP_SQRT2),
                            1.0f / (bh * 0.5f * SWEEP_SQRT2),
                            stops,
                            count};
    ring_render(x, y, w, h, radius, width, radial_shade, &s);
}
