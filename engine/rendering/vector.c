/*
 * Vector path rasterizer — anti-aliased fills and strokes for the ER_NODE_VECTOR (Svg) node.
 *
 * Pipeline (clean-room, modelled on the standard scanline-coverage approach used by nanosvg/FreeType):
 *   1. Flatten the op-tape's path ops (lines, quadratic/cubic beziers, circular arcs) into polylines,
 *      grouped into subpaths.
 *   2. FILL: turn every subpath into closed edges and rasterize with the paint's winding rule.
 *   3. STROKE: expand each subpath polyline into outline geometry (segment quads + caps + joins) and
 *      rasterize that as a nonzero fill in the stroke color.
 *   4. Rasterize = per pixel row, take SUBSAMPLES sub-scanlines, find edge crossings, walk them by
 *      winding to get covered spans, accumulate fractional horizontal coverage, then blend the row's
 *      AA spans through er_blit_fill (which composites — incl. into the RGB565 framebuffer).
 *
 * All working buffers are static and bounded (no allocation); the engine is single-threaded so reuse
 * across shapes is safe. Coordinates from the tape are node-local; the node's screen origin (px,py) is
 * added during flattening, and all painting is clipped to the node box.
 */

#include "vector.h"

#include "renderer_internal.h" /* er_blit_fill */

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Tunables
 ---------------------------------------------------------------------------------------------------------------------*/

/* These back STATIC working buffers that the rasterizer touches per-pixel/per-scanline, so on a
 * PSRAM-bss target (ESP32) they must stay in INTERNAL RAM (see the engine component's linker fragment)
 * — sized to fit there. Raise them (and the internal-RAM budget) for very complex paths. */
#ifndef ERUI_VECTOR_MAX_PTS
#define ERUI_VECTOR_MAX_PTS 2048 /**< Flattened polyline vertices per shape. */
#endif
#ifndef ERUI_VECTOR_MAX_SUBPATHS
#define ERUI_VECTOR_MAX_SUBPATHS 256
#endif
#ifndef ERUI_VECTOR_MAX_EDGES
#define ERUI_VECTOR_MAX_EDGES 2048 /**< Edges submitted to one rasterize pass. */
#endif
#ifndef ERUI_VECTOR_MAX_ROW
#define ERUI_VECTOR_MAX_ROW 1024 /**< Max node width in px (coverage row buffer). */
#endif

#ifndef ERUI_MAX_VECTOR_NODES
#define ERUI_MAX_VECTOR_NODES 8 /**< Concurrent vector nodes that can hold geometry. */
#endif
#ifndef ERUI_VECTOR_TAPE_MAX
#define ERUI_VECTOR_TAPE_MAX 1024 /**< Op-tape floats stored per vector node. */
#endif
#ifndef ERUI_VECTOR_PAINTS_MAX
#define ERUI_VECTOR_PAINTS_MAX 16 /**< Paint entries stored per vector node. */
#endif

#define VEC_SUBSAMPLES 5   /**< Vertical AA sub-scanlines per pixel row. */
#define VEC_FLAT_TOL 0.20f /**< Bezier flatness tolerance (px², see test). */
#define VEC_PI 3.14159265358979323846f

/*----------------------------------------------------------------------------------------------------------------------
 - Working state (static, reused per render)
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    float x0, y0, x1, y1; /**< Normalized so y0 <= y1. */
    int dir;              /**< +1 if the edge originally went downward (y increasing), else -1. */
} VecEdge;

typedef struct
{
    int start; /**< Index into s_px/s_py of the first vertex. */
    int count; /**< Vertex count. */
    int closed;
} VecSub;

static float s_px[ERUI_VECTOR_MAX_PTS];
static float s_py[ERUI_VECTOR_MAX_PTS];
static int s_npts;
static VecSub s_sub[ERUI_VECTOR_MAX_SUBPATHS];
static int s_nsub;

static VecEdge s_edges[ERUI_VECTOR_MAX_EDGES];
static int s_nedges;

static float s_cover[ERUI_VECTOR_MAX_ROW]; /**< Per-pixel coverage accumulator for the current row. */

/* Crossing list reused per sub-scanline. */
static float s_cross_x[ERUI_VECTOR_MAX_EDGES];
static int s_cross_d[ERUI_VECTOR_MAX_EDGES];

/*----------------------------------------------------------------------------------------------------------------------
 - Per-node storage pool
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    bool used;
    float ops[ERUI_VECTOR_TAPE_MAX];
    int n_ops;
    ERVectorPaint paints[ERUI_VECTOR_PAINTS_MAX];
    int n_paints;
} VecSlot;

static VecSlot s_slots[ERUI_MAX_VECTOR_NODES];

int er_vector_store(int slot, const float* ops, int n_ops, const ERVectorPaint* paints, int n_paints)
{
    if (slot < 0)
    {
        for (int i = 0; i < ERUI_MAX_VECTOR_NODES; i++)
        {
            if (!s_slots[i].used)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return -1; /* pool exhausted; node renders nothing */
    }
    if (slot >= ERUI_MAX_VECTOR_NODES)
        return -1;

    VecSlot* s = &s_slots[slot];
    s->used = true;
    int no = (n_ops < 0) ? 0 : (n_ops > ERUI_VECTOR_TAPE_MAX ? ERUI_VECTOR_TAPE_MAX : n_ops);
    if (ops && no > 0)
        memcpy(s->ops, ops, (size_t)no * sizeof(float));
    s->n_ops = no;
    int np = (n_paints < 0) ? 0 : (n_paints > ERUI_VECTOR_PAINTS_MAX ? ERUI_VECTOR_PAINTS_MAX : n_paints);
    if (paints && np > 0)
        memcpy(s->paints, paints, (size_t)np * sizeof(ERVectorPaint));
    s->n_paints = np;
    return slot;
}

void er_vector_free(int slot)
{
    if (slot >= 0 && slot < ERUI_MAX_VECTOR_NODES)
        s_slots[slot].used = false;
}

const float* er_vector_slot_ops(int slot, int* n_ops)
{
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || !s_slots[slot].used)
    {
        if (n_ops)
            *n_ops = 0;
        return 0;
    }
    if (n_ops)
        *n_ops = s_slots[slot].n_ops;
    return s_slots[slot].ops;
}

const ERVectorPaint* er_vector_slot_paints(int slot, int* n_paints)
{
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || !s_slots[slot].used)
    {
        if (n_paints)
            *n_paints = 0;
        return 0;
    }
    if (n_paints)
        *n_paints = s_slots[slot].n_paints;
    return s_slots[slot].paints;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Geometry building
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Appends a vertex to the current subpath (last entry of s_sub). */
static void pt_add(float x, float y)
{
    if (s_npts >= ERUI_VECTOR_MAX_PTS || s_nsub == 0)
        return;
    /* Drop exact duplicates (zero-length segments add nothing and bloat the edge list). */
    VecSub* sp = &s_sub[s_nsub - 1];
    if (sp->count > 0)
    {
        const int last = sp->start + sp->count - 1;
        if (s_px[last] == x && s_py[last] == y)
            return;
    }
    s_px[s_npts] = x;
    s_py[s_npts] = y;
    s_npts++;
    sp->count++;
}

/** @brief Starts a new subpath at (x,y). */
static void sub_begin(float x, float y)
{
    if (s_nsub >= ERUI_VECTOR_MAX_SUBPATHS)
        return;
    s_sub[s_nsub].start = s_npts;
    s_sub[s_nsub].count = 0;
    s_sub[s_nsub].closed = 0;
    s_nsub++;
    pt_add(x, y);
}

/** @brief Recursively flattens a cubic bezier into line vertices. */
static void flatten_cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int depth)
{
    if (depth > 12)
    {
        pt_add(x3, y3);
        return;
    }
    const float dx = x3 - x0;
    const float dy = y3 - y0;
    const float d1 = fabsf((x1 - x3) * dy - (y1 - y3) * dx);
    const float d2 = fabsf((x2 - x3) * dy - (y2 - y3) * dx);
    if ((d1 + d2) * (d1 + d2) < VEC_FLAT_TOL * (dx * dx + dy * dy))
    {
        pt_add(x3, y3);
        return;
    }
    const float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
    const float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
    const float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
    const float xa = (x01 + x12) * 0.5f, ya = (y01 + y12) * 0.5f;
    const float xb = (x12 + x23) * 0.5f, yb = (y12 + y23) * 0.5f;
    const float xm = (xa + xb) * 0.5f, ym = (ya + yb) * 0.5f;
    flatten_cubic(x0, y0, x01, y01, xa, ya, xm, ym, depth + 1);
    flatten_cubic(xm, ym, xb, yb, x23, y23, x3, y3, depth + 1);
}

/** @brief Appends a circular arc, sampled so the chord error stays sub-pixel. */
static void append_arc(float cx, float cy, float r, float a0, float a1, int ccw)
{
    if (r < 0.0f)
        r = -r;
    float da = a1 - a0;
    if (ccw)
    {
        while (da > 0.0f)
            da -= 2.0f * VEC_PI;
    }
    else
    {
        while (da < 0.0f)
            da += 2.0f * VEC_PI;
    }
    /* Step angle so r*(1-cos(step/2)) < ~0.2px. */
    float step = (r > 0.5f) ? 2.0f * acosf(1.0f - 0.2f / r) : VEC_PI;
    if (step <= 0.0f || step != step) /* NaN/0 guard */
        step = 0.2f;
    int n = (int)ceilf(fabsf(da) / step);
    if (n < 1)
        n = 1;
    for (int i = 0; i <= n; i++)
    {
        const float t = a0 + da * ((float)i / (float)n);
        pt_add(cx + r * cosf(t), cy + r * sinf(t));
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Edge list + rasterization
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Adds a non-horizontal edge to the rasterizer edge list (normalized so y0 <= y1). */
static void edge_add(float x0, float y0, float x1, float y1)
{
    if (y0 == y1 || s_nedges >= ERUI_VECTOR_MAX_EDGES)
        return;
    VecEdge* e = &s_edges[s_nedges++];
    if (y0 < y1)
    {
        e->x0 = x0;
        e->y0 = y0;
        e->x1 = x1;
        e->y1 = y1;
        e->dir = 1;
    }
    else
    {
        e->x0 = x1;
        e->y0 = y1;
        e->x1 = x0;
        e->y1 = y0;
        e->dir = -1;
    }
}

/** @brief Adds coverage for a covered horizontal span [xs,xe] (screen px) at the given weight. */
static void cover_span(float xs, float xe, float weight, int clipx0, int width)
{
    xs -= (float)clipx0;
    xe -= (float)clipx0;
    if (xs < 0.0f)
        xs = 0.0f;
    if (xe > (float)width)
        xe = (float)width;
    if (xe <= xs)
        return;
    int ix0 = (int)floorf(xs);
    int ix1 = (int)floorf(xe);
    if (ix0 == ix1)
    {
        s_cover[ix0] += weight * (xe - xs);
        return;
    }
    s_cover[ix0] += weight * ((float)(ix0 + 1) - xs);
    for (int x = ix0 + 1; x < ix1; x++)
        s_cover[x] += weight;
    if (ix1 < width)
        s_cover[ix1] += weight * (xe - (float)ix1);
}

/** @brief Returns true when winding @p acc counts as "inside" under the given fill rule. */
static int rule_inside(int acc, int evenodd)
{
    return evenodd ? (acc & 1) : (acc != 0);
}

/** @brief Blends the accumulated coverage row into the target, coalescing equal-alpha runs. */
static void emit_row(uint32_t color, int iy, int clipx0, int width)
{
    const uint32_t pa = (color >> 24) & 0xFFU;
    const uint32_t rgb = color & 0x00FFFFFFU;
    int x = 0;
    while (x < width)
    {
        float c = s_cover[x];
        if (c <= 0.0f)
        {
            x++;
            continue;
        }
        if (c > 1.0f)
            c = 1.0f;
        const uint32_t a = (pa * (uint32_t)(c * 255.0f + 0.5f) + 127U) / 255U;
        /* Coalesce a run of pixels with the same quantized alpha. */
        int run = 1;
        while (x + run < width)
        {
            float c2 = s_cover[x + run];
            if (c2 > 1.0f)
                c2 = 1.0f;
            const uint32_t a2 = c2 <= 0.0f ? 0U : (pa * (uint32_t)(c2 * 255.0f + 0.5f) + 127U) / 255U;
            if (a2 != a)
                break;
            run++;
        }
        if (a != 0U)
            er_blit_fill((a << 24) | rgb, clipx0 + x, iy, run, 1);
        x += run;
    }
}

/** @brief qsort comparator: orders edges top-to-bottom by their (normalized) top y. */
static int edge_cmp_y0(const void* pa, const void* pb)
{
    const float ya = ((const VecEdge*)pa)->y0;
    const float yb = ((const VecEdge*)pb)->y0;
    return (ya < yb) ? -1 : (ya > yb) ? 1 : 0;
}

/* Indices of edges crossing the current scanline (the active-edge table). */
static int s_active[ERUI_VECTOR_MAX_EDGES];

/** @brief Rasterizes the current edge list into the clip box with anti-aliasing. */
static void rasterize(uint32_t color, int evenodd, int clipx0, int clipy0, int clipx1, int clipy1)
{
    if (s_nedges == 0 || ((color >> 24) & 0xFFU) == 0U)
        return;
    const int width = clipx1 - clipx0;
    if (width <= 0 || width > ERUI_VECTOR_MAX_ROW)
        return;

    /* Sort edges top-to-bottom so each scanline only tests the few edges actually crossing it (an
     * active-edge table) instead of all of them — the difference between O(edges*rows) and ~O(rows),
     * which is what makes a long arc stroke cheap to rasterize. */
    qsort(s_edges, (size_t)s_nedges, sizeof(VecEdge), edge_cmp_y0);

    float fy1 = -1e30f;
    for (int i = 0; i < s_nedges; i++)
        if (s_edges[i].y1 > fy1)
            fy1 = s_edges[i].y1;
    int ymin = (int)floorf(s_edges[0].y0); /* sorted: edge 0 has the smallest y0 */
    int ymax = (int)ceilf(fy1);
    if (ymin < clipy0)
        ymin = clipy0;
    if (ymax > clipy1)
        ymax = clipy1;

    const float w = 1.0f / (float)VEC_SUBSAMPLES;
    int next = 0;     /* next edge (in y0 order) not yet activated */
    int n_active = 0; /* count in s_active */

    for (int iy = ymin; iy < ymax; iy++)
    {
        /* Activate edges whose top is within/above this row; retire edges that ended above it. */
        const float row_bottom = (float)(iy + 1);
        while (next < s_nedges && s_edges[next].y0 < row_bottom)
            s_active[n_active++] = next++;
        for (int a = 0; a < n_active;)
        {
            if (s_edges[s_active[a]].y1 <= (float)iy)
                s_active[a] = s_active[--n_active];
            else
                a++;
        }

        memset(s_cover, 0, (size_t)width * sizeof(float));
        for (int s = 0; s < VEC_SUBSAMPLES; s++)
        {
            const float sy = (float)iy + ((float)s + 0.5f) * w;
            /* Collect crossings of this sub-scanline from the active set only. */
            int m = 0;
            for (int a = 0; a < n_active; a++)
            {
                const VecEdge* e = &s_edges[s_active[a]];
                if (sy < e->y0 || sy >= e->y1)
                    continue;
                const float t = (sy - e->y0) / (e->y1 - e->y0);
                s_cross_x[m] = e->x0 + (e->x1 - e->x0) * t;
                s_cross_d[m] = e->dir;
                m++;
            }
            if (m < 2)
                continue;
            /* Insertion sort crossings by x (m is small in practice). */
            for (int a = 1; a < m; a++)
            {
                const float kx = s_cross_x[a];
                const int kd = s_cross_d[a];
                int b = a - 1;
                while (b >= 0 && s_cross_x[b] > kx)
                {
                    s_cross_x[b + 1] = s_cross_x[b];
                    s_cross_d[b + 1] = s_cross_d[b];
                    b--;
                }
                s_cross_x[b + 1] = kx;
                s_cross_d[b + 1] = kd;
            }
            /* Walk crossings, accumulating winding; emit covered spans. */
            int acc = 0;
            for (int k = 0; k < m - 1; k++)
            {
                acc += s_cross_d[k];
                if (rule_inside(acc, evenodd))
                    cover_span(s_cross_x[k], s_cross_x[k + 1], w, clipx0, width);
            }
        }
        emit_row(color, iy, clipx0, width);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Fill + stroke
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Builds closed edges from every subpath and rasterizes the fill. */
static void fill_shape(uint32_t color, int evenodd, int cx0, int cy0, int cx1, int cy1)
{
    s_nedges = 0;
    for (int si = 0; si < s_nsub; si++)
    {
        const VecSub* sp = &s_sub[si];
        if (sp->count < 2)
            continue;
        for (int i = 0; i < sp->count - 1; i++)
            edge_add(s_px[sp->start + i], s_py[sp->start + i], s_px[sp->start + i + 1], s_py[sp->start + i + 1]);
        /* Fill implicitly closes every subpath. */
        edge_add(s_px[sp->start + sp->count - 1], s_py[sp->start + sp->count - 1], s_px[sp->start], s_py[sp->start]);
    }
    rasterize(color, evenodd, cx0, cy0, cx1, cy1);
}

/** @brief Adds a filled convex quad (4 corners, CCW or CW) as edges. */
static void add_quad(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
{
    edge_add(ax, ay, bx, by);
    edge_add(bx, by, cx, cy);
    edge_add(cx, cy, dx, dy);
    edge_add(dx, dy, ax, ay);
}

/** @brief Adds a filled disc (used for round caps/joins) approximated by an n-gon. */
static void add_disc(float cx, float cy, float r)
{
    if (r <= 0.0f)
        return;
    int n = (int)(r * 1.5f);
    if (n < 8)
        n = 8;
    if (n > 28)
        n = 28;
    float px = cx + r, py = cy;
    for (int i = 1; i <= n; i++)
    {
        const float t = 2.0f * VEC_PI * (float)i / (float)n;
        const float qx = cx + r * cosf(t);
        const float qy = cy + r * sinf(t);
        edge_add(px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

/** @brief Adds a triangle as edges (for bevel/miter joins). */
static void add_tri(float ax, float ay, float bx, float by, float cx, float cy)
{
    edge_add(ax, ay, bx, by);
    edge_add(bx, by, cx, cy);
    edge_add(cx, cy, ax, ay);
}

/**
 * @brief Builds stroke outline geometry for one subpath and adds it to the edge list.
 *
 * Each segment becomes a quad of width @p sw; interior vertices get a join and open endpoints get a
 * cap. Round joins/caps use discs, bevel uses a triangle, miter extends to the intersection (bounded
 * by the miter limit, else falls back to bevel). The pieces overlap and are unioned by the nonzero
 * fill rule, so no single clean outline polygon is needed.
 */
static void stroke_subpath(const VecSub* sp, float sw, int cap, int join, float miter)
{
    const int n = sp->count;
    if (n < 1)
        return;
    const float r = sw * 0.5f;
    const float* X = &s_px[sp->start];
    const float* Y = &s_py[sp->start];

    if (n == 1)
    {
        if (cap == ER_VCAP_ROUND)
            add_disc(X[0], Y[0], r);
        return;
    }

    const int closed = sp->closed;

    /* Segment quads. */
    for (int i = 0; i < n - 1; i++)
    {
        float ux = X[i + 1] - X[i];
        float uy = Y[i + 1] - Y[i];
        const float len = sqrtf(ux * ux + uy * uy);
        if (len < 1e-6f)
            continue;
        ux /= len;
        uy /= len;
        const float nx = -uy * r, ny = ux * r; /* left normal * r */
        float ax = X[i], ay = Y[i], bx = X[i + 1], by = Y[i + 1];
        /* Square cap: extend the open ends by r along the segment direction. */
        if (cap == ER_VCAP_SQUARE && !closed)
        {
            if (i == 0)
            {
                ax -= ux * r;
                ay -= uy * r;
            }
            if (i == n - 2)
            {
                bx += ux * r;
                by += uy * r;
            }
        }
        add_quad(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny);
    }

    /* Joins at interior vertices (and the wrap vertex for closed paths). */
    const int jlast = closed ? n - 1 : n - 2;
    for (int i = 1; i <= jlast; i++)
    {
        const int ip = i;
        const int prev = i - 1;
        const int next = (i == n - 1) ? 1 : i + 1; /* for closed wrap, segment after P[0] */
        float u0x = X[ip] - X[prev], u0y = Y[ip] - Y[prev];
        float u1x = X[next] - X[ip], u1y = Y[next] - Y[ip];
        float l0 = sqrtf(u0x * u0x + u0y * u0y);
        float l1 = sqrtf(u1x * u1x + u1y * u1y);
        if (l0 < 1e-6f || l1 < 1e-6f)
            continue;
        u0x /= l0;
        u0y /= l0;
        u1x /= l1;
        u1y /= l1;

        /* Skip joins between near-collinear segments: the adjacent segment quads already cover the
         * seam (their sub-pixel gap is hidden by AA). THIS IS THE BIG PERF WIN on smooth flattened
         * curves — an arc has many vertices that would otherwise each emit join geometry. */
        if (u0x * u1x + u0y * u1y > 0.9995f)
            continue;

        /* Outer side normals + which side carries the gap. */
        const float n0x = -u0y * r, n0y = u0x * r;
        const float n1x = -u1y * r, n1y = u1x * r;
        const float cross = u0x * u1y - u0y * u1x;
        const float s = (cross < 0.0f) ? 1.0f : -1.0f; /* pick the outer offsets */
        const float p0x = X[ip] + s * n0x, p0y = Y[ip] + s * n0y;
        const float p1x = X[ip] + s * n1x, p1y = Y[ip] + s * n1y;

        if (join == ER_VJOIN_ROUND)
        {
            /* Fan the outer gap with triangles back to the vertex — a true round join in a handful of
             * edges (one triangle for a gentle bend), not a full disc per vertex. */
            const float a0 = atan2f(p0y - Y[ip], p0x - X[ip]);
            const float a1 = atan2f(p1y - Y[ip], p1x - X[ip]);
            float da = a1 - a0;
            while (da > VEC_PI)
                da -= 2.0f * VEC_PI;
            while (da < -VEC_PI)
                da += 2.0f * VEC_PI;
            int steps = (int)ceilf(fabsf(da) / 0.4f); /* ~23 degrees per step */
            if (steps < 1)
                steps = 1;
            float pax = p0x, pay = p0y;
            for (int k = 1; k <= steps; k++)
            {
                const float t = a0 + da * ((float)k / (float)steps);
                const float pbx = X[ip] + r * cosf(t);
                const float pby = Y[ip] + r * sinf(t);
                add_tri(X[ip], Y[ip], pax, pay, pbx, pby);
                pax = pbx;
                pay = pby;
            }
            continue;
        }
        if (join == ER_VJOIN_MITER)
        {
            /* Miter point = intersection of the two outer offset lines. */
            const float denom = u0x * u1y - u0y * u1x;
            if (fabsf(denom) > 1e-6f)
            {
                const float t = ((p1x - p0x) * u1y - (p1y - p0y) * u1x) / denom;
                const float mx = p0x + u0x * t;
                const float my = p0y + u0y * t;
                const float mdx = mx - X[ip], mdy = my - Y[ip];
                const float mlen = sqrtf(mdx * mdx + mdy * mdy);
                if (mlen <= miter * r)
                {
                    add_tri(X[ip], Y[ip], p0x, p0y, mx, my);
                    add_tri(X[ip], Y[ip], mx, my, p1x, p1y);
                    continue;
                }
            }
            /* Too sharp -> fall through to bevel. */
        }
        add_tri(X[ip], Y[ip], p0x, p0y, p1x, p1y); /* bevel */
    }

    /* Caps at open endpoints. */
    if (!closed && cap == ER_VCAP_ROUND)
    {
        add_disc(X[0], Y[0], r);
        add_disc(X[n - 1], Y[n - 1], r);
    }
}

/** @brief Builds stroke geometry for all subpaths and rasterizes it. */
static void stroke_shape(uint32_t color, float sw, int cap, int join, float miter, int cx0, int cy0, int cx1, int cy1)
{
    if (sw <= 0.0f)
        return;
    s_nedges = 0;
    for (int si = 0; si < s_nsub; si++)
        if (s_sub[si].count >= 1)
            stroke_subpath(&s_sub[si], sw, cap, join, miter);
    rasterize(color, 0 /* stroke always nonzero */, cx0, cy0, cx1, cy1);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public entry
 ---------------------------------------------------------------------------------------------------------------------*/

void er_vector_render(
    const float* ops, int n_ops, const ERVectorPaint* paints, int n_paints, int px, int py, int w, int h)
{
    if (!ops || n_ops <= 0)
        return;
    const int cx0 = px, cy0 = py, cx1 = px + w, cy1 = py + h;

    int i = 0;
    while (i < n_ops)
    {
        if (ops[i] != ER_VOP_SHAPE)
        {
            i++; /* tape must start with SHAPE; skip stray values defensively */
            continue;
        }
        i++; /* consume SHAPE opcode */
        const int pidx = (i < n_ops) ? (int)ops[i++] : 0;

        /* Build this shape's subpaths until the next SHAPE or end of tape. */
        s_npts = 0;
        s_nsub = 0;
        float cur_x = 0.0f, cur_y = 0.0f;
        while (i < n_ops && ops[i] != ER_VOP_SHAPE)
        {
            const float op = ops[i++];
            if (op == ER_VOP_MOVE)
            {
                cur_x = (float)px + ops[i++];
                cur_y = (float)py + ops[i++];
                sub_begin(cur_x, cur_y);
            }
            else if (op == ER_VOP_LINE)
            {
                cur_x = (float)px + ops[i++];
                cur_y = (float)py + ops[i++];
                pt_add(cur_x, cur_y);
            }
            else if (op == ER_VOP_QUAD)
            {
                const float qcx = (float)px + ops[i++];
                const float qcy = (float)py + ops[i++];
                const float ex = (float)px + ops[i++];
                const float ey = (float)py + ops[i++];
                /* Elevate the quadratic to a cubic and reuse the cubic flattener. */
                const float c1x = cur_x + (2.0f / 3.0f) * (qcx - cur_x);
                const float c1y = cur_y + (2.0f / 3.0f) * (qcy - cur_y);
                const float c2x = ex + (2.0f / 3.0f) * (qcx - ex);
                const float c2y = ey + (2.0f / 3.0f) * (qcy - ey);
                flatten_cubic(cur_x, cur_y, c1x, c1y, c2x, c2y, ex, ey, 0);
                cur_x = ex;
                cur_y = ey;
            }
            else if (op == ER_VOP_CUBIC)
            {
                const float c1x = (float)px + ops[i++];
                const float c1y = (float)py + ops[i++];
                const float c2x = (float)px + ops[i++];
                const float c2y = (float)py + ops[i++];
                const float ex = (float)px + ops[i++];
                const float ey = (float)py + ops[i++];
                flatten_cubic(cur_x, cur_y, c1x, c1y, c2x, c2y, ex, ey, 0);
                cur_x = ex;
                cur_y = ey;
            }
            else if (op == ER_VOP_ARC)
            {
                const float acx = (float)px + ops[i++];
                const float acy = (float)py + ops[i++];
                const float ar = ops[i++];
                const float a0 = ops[i++];
                const float a1 = ops[i++];
                const int ccw = (ops[i++] != 0.0f);
                if (s_nsub == 0)
                    sub_begin(acx + ar * cosf(a0), acy + ar * sinf(a0));
                append_arc(acx, acy, ar, a0, a1, ccw);
                cur_x = acx + ar * cosf(a1);
                cur_y = acy + ar * sinf(a1);
            }
            else if (op == ER_VOP_CLOSE)
            {
                if (s_nsub > 0)
                {
                    s_sub[s_nsub - 1].closed = 1;
                    cur_x = s_px[s_sub[s_nsub - 1].start];
                    cur_y = s_py[s_sub[s_nsub - 1].start];
                }
            }
            else
            {
                /* Unknown opcode: stop to avoid desync. */
                break;
            }
        }

        /* Apply the shape's paint: fill first, then stroke (SVG paint order). */
        const ERVectorPaint* pt = (pidx >= 0 && pidx < n_paints) ? &paints[pidx] : 0;
        if (pt)
        {
            if (((pt->fill >> 24) & 0xFFU) != 0U)
                fill_shape(pt->fill, pt->fill_rule == ER_VFILL_EVENODD, cx0, cy0, cx1, cy1);
            if (((pt->stroke >> 24) & 0xFFU) != 0U && pt->stroke_w > 0.0f)
                stroke_shape(pt->stroke,
                             pt->stroke_w,
                             pt->cap,
                             pt->join,
                             pt->miter > 0.0f ? pt->miter : 4.0f,
                             cx0,
                             cy0,
                             cx1,
                             cy1);
        }
    }
}
