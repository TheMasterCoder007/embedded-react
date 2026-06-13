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

#include "transform.h"
#include "renderer_internal.h"
#include "scratch_pool.h"
#include <math.h>
#include <string.h>

#ifndef ERUI_SCRATCH_W
#define ERUI_SCRATCH_W 240
#endif
#ifndef ERUI_SCRATCH_H
#define ERUI_SCRATCH_H 240
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

#if ERUI_TRANSFORMS_FULL
/* Source buffer: the untransformed subtree is rendered into this scratch slot. */
static uint32_t s_xform_src[ERUI_SCRATCH_H * ERUI_SCRATCH_W];
/* Destination buffer: holds the inverse-mapped (transformed) output before blending. */
static uint32_t s_xform_dst[ERUI_SCRATCH_H * ERUI_SCRATCH_W];
/* True while a source capture is active. */
static bool s_xform_active = false;

/* Bilinear channel lerp: a,b in [0,255]; t in [0,256]. Result in [0,255]. */
#define ER_LERP_CH(a, b, t) (((uint32_t)(a) * (256u - (t)) + (uint32_t)(b) * (t)) >> 8u)

/**
 * @brief Samples the source scratch with bilinear filtering.
 *
 * Premultiplied ARGB8888 channels are interpolated independently, so no
 * unpremultiply/repremultiply pass is needed.  Out-of-bounds neighbours
 * are treated as fully transparent (0).
 *
 * @param[in] src    Source pixel buffer (row stride = ERUI_SCRATCH_W).
 * @param[in] src_w  Logical width of the source region.
 * @param[in] src_h  Logical height of the source region.
 * @param[in] flx    Sub-pixel X in source-local coordinates.
 * @param[in] fly    Sub-pixel Y in source-local coordinates.
 *
 * @return Bilinearly filtered premultiplied ARGB8888 pixel.
 */
static uint32_t er_bilerp(const uint32_t* src, int src_w, int src_h, float flx, float fly)
{
    const int x0 = (int)floorf(flx);
    const int y0 = (int)floorf(fly);
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;

    const uint32_t wx = (uint32_t)((flx - (float)x0) * 256.0f);
    const uint32_t wy = (uint32_t)((fly - (float)y0) * 256.0f);

    const uint32_t c00 = (x0 >= 0 && x0 < src_w && y0 >= 0 && y0 < src_h) ? src[y0 * ERUI_SCRATCH_W + x0] : 0u;
    const uint32_t c10 = (x1 >= 0 && x1 < src_w && y0 >= 0 && y0 < src_h) ? src[y0 * ERUI_SCRATCH_W + x1] : 0u;
    const uint32_t c01 = (x0 >= 0 && x0 < src_w && y1 >= 0 && y1 < src_h) ? src[y1 * ERUI_SCRATCH_W + x0] : 0u;
    const uint32_t c11 = (x1 >= 0 && x1 < src_w && y1 >= 0 && y1 < src_h) ? src[y1 * ERUI_SCRATCH_W + x1] : 0u;

    const uint32_t out_a = ER_LERP_CH(ER_LERP_CH(c00 >> 24, c10 >> 24, wx), ER_LERP_CH(c01 >> 24, c11 >> 24, wx), wy);
    const uint32_t out_r = ER_LERP_CH(ER_LERP_CH((c00 >> 16) & 0xFFu, (c10 >> 16) & 0xFFu, wx),
                                      ER_LERP_CH((c01 >> 16) & 0xFFu, (c11 >> 16) & 0xFFu, wx),
                                      wy);
    const uint32_t out_g = ER_LERP_CH(ER_LERP_CH((c00 >> 8) & 0xFFu, (c10 >> 8) & 0xFFu, wx),
                                      ER_LERP_CH((c01 >> 8) & 0xFFu, (c11 >> 8) & 0xFFu, wx),
                                      wy);
    const uint32_t out_b =
        ER_LERP_CH(ER_LERP_CH(c00 & 0xFFu, c10 & 0xFFu, wx), ER_LERP_CH(c01 & 0xFFu, c11 & 0xFFu, wx), wy);

    return (out_a << 24u) | (out_r << 16u) | (out_g << 8u) | out_b;
}
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_transform_is_translate_only(const ERNode* n)
{
#if ERUI_3D_TRANSFORMS
    if (n->tp_rotate_x != 0.0f || n->tp_rotate_y != 0.0f || n->tp_perspective != 0.0f)
        return false;
#endif
    return n->tp_scale_x == 0.0f && n->tp_scale_y == 0.0f && n->tp_rotate_z == 0.0f;
}

#if ERUI_3D_TRANSFORMS
bool er_transform_is_3d(const ERNode* n)
{
    return n->tp_rotate_x != 0.0f || n->tp_rotate_y != 0.0f || n->tp_perspective != 0.0f;
}

void er_transform_compute_homography_3d(const ERNode* n, int ref_x, int ref_y, int w, int h, float H[9])
{
    const float sx = (n->tp_scale_x == 0.0f) ? 1.0f : n->tp_scale_x;
    const float sy = (n->tp_scale_y == 0.0f) ? 1.0f : n->tp_scale_y;
    const float rz = n->tp_rotate_z * (float)(3.14159265358979323846 / 180.0);
    const float rx = n->tp_rotate_x * (float)(3.14159265358979323846 / 180.0);
    const float ry = n->tp_rotate_y * (float)(3.14159265358979323846 / 180.0);

    const float cx_t = cosf(rx), sx_t = sinf(rx);
    const float cy_t = cosf(ry), sy_t = sinf(ry);
    const float cz_t = cosf(rz), sz_t = sinf(rz);

    /* Pivot origin in layout space. */
    const float ofx = (n->tp_origin_x < 0.0f) ? 0.5f : n->tp_origin_x;
    const float ofy = (n->tp_origin_y < 0.0f) ? 0.5f : n->tp_origin_y;
    const float ox = (float)ref_x + ofx * (float)w;
    const float oy = (float)ref_y + ofy * (float)h;
    const float tx = n->tp_translate_x;
    const float ty = n->tp_translate_y;
    const float d = n->tp_perspective;

    /* Upper-left 3×3 of M = RotY * RotX * RotZ (columns scaled by sx, sy).
     * Only columns 0 and 1 matter since z = 0 for all source points. */
    /* Column 0 (u0 direction), scaled by sx: */
    const float r00 = sx * (cy_t * cz_t + sx_t * sy_t * sz_t);
    const float r10 = sx * (cx_t * sz_t);
    const float r20 = sx * (-sy_t * cz_t + sx_t * cy_t * sz_t);
    /* Column 1 (v0 direction), scaled by sy: */
    const float r01 = sy * (sy_t * sx_t * cz_t - cy_t * sz_t);
    const float r11 = sy * (cx_t * cz_t);
    const float r21 = sy * (cy_t * sx_t * cz_t + sy_t * sz_t);

    /* Constant term (translation contribution):
     *   X = r00*u0 + r01*v0 + ox + tx   where u0 = u - ox, v0 = v - oy
     *     = r00*u  + r01*v  + (-r00*ox - r01*oy + ox + tx)           */
    const float c0 = -r00 * ox - r01 * oy + ox + tx;
    const float c1 = -r10 * ox - r11 * oy + oy + ty;
    /* W row: W = 1 + Z/d = 1 + (r20*(u-ox) + r21*(v-oy)) / d         */
    const float c2 = 1.0f - r20 * ox / (d > 1e-3f ? d : 1.0f) - r21 * oy / (d > 1e-3f ? d : 1.0f);

    if (d > 1e-3f)
    {
        H[0] = r00;
        H[1] = r01;
        H[2] = c0;
        H[3] = r10;
        H[4] = r11;
        H[5] = c1;
        H[6] = r20 / d;
        H[7] = r21 / d;
        H[8] = c2;
    }
    else
    {
        /* Orthographic (no perspective division). */
        H[0] = r00;
        H[1] = r01;
        H[2] = c0;
        H[3] = r10;
        H[4] = r11;
        H[5] = c1;
        H[6] = 0.0f;
        H[7] = 0.0f;
        H[8] = 1.0f;
    }
}

bool er_transform_homography_invert(const float H[9], float inv[9])
{
    /* Cofactors. */
    const float c00 = H[4] * H[8] - H[5] * H[7];
    const float c01 = H[5] * H[6] - H[3] * H[8];
    const float c02 = H[3] * H[7] - H[4] * H[6];
    const float c10 = H[2] * H[7] - H[1] * H[8];
    const float c11 = H[0] * H[8] - H[2] * H[6];
    const float c12 = H[1] * H[6] - H[0] * H[7];
    const float c20 = H[1] * H[5] - H[2] * H[4];
    const float c21 = H[2] * H[3] - H[0] * H[5];
    const float c22 = H[0] * H[4] - H[1] * H[3];

    const float det = H[0] * c00 + H[1] * c01 + H[2] * c02;
    if (det > -1e-7f && det < 1e-7f)
        return false;

    const float inv_det = 1.0f / det;
    inv[0] = c00 * inv_det;
    inv[1] = c10 * inv_det;
    inv[2] = c20 * inv_det;
    inv[3] = c01 * inv_det;
    inv[4] = c11 * inv_det;
    inv[5] = c21 * inv_det;
    inv[6] = c02 * inv_det;
    inv[7] = c12 * inv_det;
    inv[8] = c22 * inv_det;
    return true;
}

void er_transform_aabb_3d(
    int ref_x, int ref_y, int w, int h, const float H[9], int* out_x, int* out_y, int* out_w, int* out_h)
{
    const float cu[4] = {(float)ref_x, (float)(ref_x + w), (float)(ref_x + w), (float)ref_x};
    const float cv[4] = {(float)ref_y, (float)ref_y, (float)(ref_y + h), (float)(ref_y + h)};

    float min_x = 1e30f, max_x = -1e30f, min_y = 1e30f, max_y = -1e30f;
    bool any = false;

    for (int i = 0; i < 4; i++)
    {
        const float Wp = H[6] * cu[i] + H[7] * cv[i] + H[8];
        if (Wp <= 0.0f)
            continue;
        const float sx = (H[0] * cu[i] + H[1] * cv[i] + H[2]) / Wp;
        const float sy = (H[3] * cu[i] + H[4] * cv[i] + H[5]) / Wp;
        if (!any || sx < min_x)
            min_x = sx;
        if (!any || sx > max_x)
            max_x = sx;
        if (!any || sy < min_y)
            min_y = sy;
        if (!any || sy > max_y)
            max_y = sy;
        any = true;
    }

    if (!any)
    {
        *out_x = *out_y = *out_w = *out_h = 0;
        return;
    }

    /* Expand by 1 pixel on all sides.  At near-zero rotation angles the projected
     * corners land very close to integer coordinates; ceil rounding of those values
     * can produce an AABB that is 1 pixel short of the source, causing the last
     * row/column to back-project just outside the source buffer and appear as a
     * transparent gap.  The 1-pixel border always has valid source pixels behind it
     * (the back-projected coordinate is still inside the source), stabilising the
     * apparent size across frames and eliminating the edge-jagging artefact. */
    *out_x = (int)floorf(min_x) - 1;
    *out_y = (int)floorf(min_y) - 1;
    *out_w = (int)ceilf(max_x) - *out_x + 1;
    *out_h = (int)ceilf(max_y) - *out_y + 1;
    if (*out_w < 0)
        *out_w = 0;
    if (*out_h < 0)
        *out_h = 0;
}

void er_transform_source_end_blit_3d(
    int src_x, int src_y, int src_w, int src_h, const float inv_H[9], int dst_x, int dst_y, int dst_w, int dst_h)
{
#if ERUI_TRANSFORMS_FULL
    if (!s_xform_active)
        return;

    er_scratch_pop_base();
    s_xform_active = false;

    if (dst_w <= 0 || dst_h <= 0 || dst_w > ERUI_SCRATCH_W || dst_h > ERUI_SCRATCH_H)
        return;

    memset(s_xform_dst, 0, (size_t)dst_w * (size_t)dst_h * sizeof(uint32_t));

    for (int oy = 0; oy < dst_h; oy++)
    {
        const float sy_f = (float)(dst_y + oy);
        uint32_t* dst_row = s_xform_dst + oy * dst_w;

        for (int ox = 0; ox < dst_w; ox++)
        {
            const float sx_f = (float)(dst_x + ox);

            /* Back-project screen point through the inverse homography. */
            const float Wp = inv_H[6] * sx_f + inv_H[7] * sy_f + inv_H[8];
            if (Wp <= 0.0f)
                continue;
            const float flx = (inv_H[0] * sx_f + inv_H[1] * sy_f + inv_H[2]) / Wp - (float)src_x;
            const float fly = (inv_H[3] * sx_f + inv_H[4] * sy_f + inv_H[5]) / Wp - (float)src_y;

            if (flx < -1.0f || flx >= (float)src_w || fly < -1.0f || fly >= (float)src_h)
                continue;

            dst_row[ox] = er_bilerp(s_xform_src, src_w, src_h, flx, fly);
        }
    }

    er_blit_blend(s_xform_dst, dst_w * (int)sizeof(uint32_t), 255U, dst_x, dst_y, dst_w, dst_h);
#else
    (void)src_x;
    (void)src_y;
    (void)src_w;
    (void)src_h;
    (void)inv_H;
    (void)dst_x;
    (void)dst_y;
    (void)dst_w;
    (void)dst_h;
#endif
}
#endif /* ERUI_3D_TRANSFORMS */

void er_transform_compute_matrix(
    const ERNode* n, int ref_x, int ref_y, int w, int h, float* a, float* b, float* c, float* d, float* tx, float* ty)
{
    const float sx = (n->tp_scale_x == 0.0f) ? 1.0f : n->tp_scale_x;
    const float sy = (n->tp_scale_y == 0.0f) ? 1.0f : n->tp_scale_y;
    const float theta = n->tp_rotate_z * (float)(3.14159265358979323846 / 180.0);
    const float cos_t = cosf(theta);
    const float sin_t = sinf(theta);

    /* 2×2 rotation/scale matrix:
     *   x' = sx*cos(θ)*x − sy*sin(θ)*y
     *   y' = sx*sin(θ)*x + sy*cos(θ)*y  */
    *a = sx * cos_t;
    *b = sx * sin_t;
    *c = -sy * sin_t;
    *d = sy * cos_t;

    /* Transform origin in layout/render space. */
    const float ofx = (n->tp_origin_x < 0.0f) ? 0.5f : n->tp_origin_x;
    const float ofy = (n->tp_origin_y < 0.0f) ? 0.5f : n->tp_origin_y;
    const float ox = (float)ref_x + ofx * (float)w;
    const float oy = (float)ref_y + ofy * (float)h;

    /* Total translation: pivot back + explicit translate − M * pivot. */
    *tx = ox + n->tp_translate_x - (*a * ox + *c * oy);
    *ty = oy + n->tp_translate_y - (*b * ox + *d * oy);
}

bool er_transform_invert(float a,
                         float b,
                         float c,
                         float d,
                         float tx,
                         float ty,
                         float* ia,
                         float* ib,
                         float* ic,
                         float* id,
                         float* itx,
                         float* ity)
{
    const float det = a * d - b * c;
    if (det > -1e-6f && det < 1e-6f)
        return false;

    const float inv = 1.0f / det;
    *ia = d * inv;
    *ib = -b * inv;
    *ic = -c * inv;
    *id = a * inv;
    /* Inverse translation: itx = (c*ty − d*tx) / det, ity = (b*tx − a*ty) / det */
    *itx = (c * ty - d * tx) * inv;
    *ity = (b * tx - a * ty) * inv;
    return true;
}

void er_transform_aabb(int ref_x,
                       int ref_y,
                       int w,
                       int h,
                       float a,
                       float b,
                       float c,
                       float d,
                       float tx,
                       float ty,
                       int* out_x,
                       int* out_y,
                       int* out_w,
                       int* out_h)
{
    /* Screen-space positions of the four corners. */
    const float cx[4] = {(float)ref_x, (float)(ref_x + w), (float)(ref_x + w), (float)ref_x};
    const float cy[4] = {(float)ref_y, (float)ref_y, (float)(ref_y + h), (float)(ref_y + h)};

    float min_x, max_x, min_y, max_y;
    min_x = max_x = a * cx[0] + c * cy[0] + tx;
    min_y = max_y = b * cx[0] + d * cy[0] + ty;

    for (int i = 1; i < 4; i++)
    {
        const float sx = a * cx[i] + c * cy[i] + tx;
        const float sy = b * cx[i] + d * cy[i] + ty;
        if (sx < min_x)
            min_x = sx;
        if (sx > max_x)
            max_x = sx;
        if (sy < min_y)
            min_y = sy;
        if (sy > max_y)
            max_y = sy;
    }

    *out_x = (int)floorf(min_x);
    *out_y = (int)floorf(min_y);
    *out_w = (int)ceilf(max_x) - *out_x;
    *out_h = (int)ceilf(max_y) - *out_y;
    if (*out_w < 0)
        *out_w = 0;
    if (*out_h < 0)
        *out_h = 0;
}

void er_transform_map_point(float ia,
                            float ib,
                            float ic,
                            float id,
                            float itx,
                            float ity,
                            int screen_x,
                            int screen_y,
                            int* layout_x,
                            int* layout_y)
{
    *layout_x = (int)(ia * (float)screen_x + ic * (float)screen_y + itx);
    *layout_y = (int)(ib * (float)screen_x + id * (float)screen_y + ity);
}

bool er_transform_source_begin(int src_x, int src_y, int w, int h)
{
#if ERUI_TRANSFORMS_FULL
    if (s_xform_active)
        return false;
    if (w <= 0 || h <= 0 || w > ERUI_SCRATCH_W || h > ERUI_SCRATCH_H)
        return false;

    memset(s_xform_src, 0, (size_t)ERUI_SCRATCH_W * (size_t)ERUI_SCRATCH_H * sizeof(uint32_t));
    er_scratch_push_base(s_xform_src, ERUI_SCRATCH_W, ERUI_SCRATCH_H, src_x, src_y);
    s_xform_active = true;
    return true;
#else
    (void)src_x;
    (void)src_y;
    (void)w;
    (void)h;
    return false;
#endif
}

void er_transform_source_end_blit(int src_x,
                                  int src_y,
                                  int src_w,
                                  int src_h,
                                  float ia,
                                  float ib,
                                  float ic,
                                  float id,
                                  float itx,
                                  float ity,
                                  int dst_x,
                                  int dst_y,
                                  int dst_w,
                                  int dst_h)
{
#if ERUI_TRANSFORMS_FULL
    if (!s_xform_active)
        return;

    er_scratch_pop_base();
    s_xform_active = false;

    if (dst_w <= 0 || dst_h <= 0 || dst_w > ERUI_SCRATCH_W || dst_h > ERUI_SCRATCH_H)
        return;

    /* Build output buffer: for each screen-space pixel in the destination bounding box,
     * inverse-map to the source scratch and sample nearest-neighbor. */
    memset(s_xform_dst, 0, (size_t)dst_w * (size_t)dst_h * sizeof(uint32_t));

    for (int oy = 0; oy < dst_h; oy++)
    {
        const float sy = (float)(dst_y + oy);
        uint32_t* dst_row = s_xform_dst + oy * dst_w;

        for (int ox = 0; ox < dst_w; ox++)
        {
            const float sx = (float)(dst_x + ox);

            /* Inverse-transform screen point → source-local sub-pixel coordinates. */
            const float flx = (ia * sx + ic * sy + itx) - (float)src_x;
            const float fly = (ib * sx + id * sy + ity) - (float)src_y;

            /* Coarse bounds reject: skip if all four bilinear neighbours are outside. */
            if (flx < -1.0f || flx >= (float)src_w || fly < -1.0f || fly >= (float)src_h)
                continue;

            dst_row[ox] = er_bilerp(s_xform_src, src_w, src_h, flx, fly);
        }
    }

    er_blit_blend(s_xform_dst, dst_w * (int)sizeof(uint32_t), 255U, dst_x, dst_y, dst_w, dst_h);
#else
    (void)src_x;
    (void)src_y;
    (void)src_w;
    (void)src_h;
    (void)ia;
    (void)ib;
    (void)ic;
    (void)id;
    (void)itx;
    (void)ity;
    (void)dst_x;
    (void)dst_y;
    (void)dst_w;
    (void)dst_h;
#endif
}
