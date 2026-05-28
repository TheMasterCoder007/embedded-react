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
    return n->tp_scale_x == 0.0f && n->tp_scale_y == 0.0f && n->tp_rotate_z == 0.0f;
}

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
