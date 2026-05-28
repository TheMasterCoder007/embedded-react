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

            /* Inverse-transform screen point → layout space. */
            const int lx = (int)(ia * sx + ic * sy + itx) - src_x;
            const int ly = (int)(ib * sx + id * sy + ity) - src_y;

            if (lx < 0 || lx >= src_w || ly < 0 || ly >= src_h)
                continue;

            dst_row[ox] = s_xform_src[ly * ERUI_SCRATCH_W + lx];
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
