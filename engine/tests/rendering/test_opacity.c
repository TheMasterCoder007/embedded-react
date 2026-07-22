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

#include "er_scene.h"
#include "native_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 32
#define FB_H 32

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pixel framebuffer and call statistics gathered during a render pass.
 */
typedef struct
{
    uint32_t* fb;    /**< Flat premultiplied ARGB8888 framebuffer; rows are fb_w pixels wide. */
    int fb_w;        /**< Framebuffer width in pixels. */
    int fb_h;        /**< Framebuffer height in pixels. */
    int fill_calls;  /**< Number of fill_rect calls received. */
    int blend_calls; /**< Number of blend_rect calls received. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect: premultiplies the color and writes it into the framebuffer.
 *
 * @param[in] argb  Straight-alpha ARGB8888 fill color.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    t->fill_calls++;

    const uint8_t a = (uint8_t)((argb >> 24) & 0xFFU);
    if (a == 0U)
        return;
    const uint8_t r = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * a / 255U);
    const uint8_t g = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * a / 255U);
    const uint8_t b = (uint8_t)((uint32_t)(argb & 0xFFU) * a / 255U);
    const uint32_t premul = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    for (int row = y; row < y + h; row++)
    {
        for (int col = x; col < x + w; col++)
        {
            if (col >= 0 && col < t->fb_w && row >= 0 && row < t->fb_h)
                t->fb[row * t->fb_w + col] = premul;
        }
    }
}

/**
 * @brief Backend copy_rect: copies premultiplied pixels directly into the framebuffer.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* src_row = (const uint32_t*)((const uint8_t*)src + row * stride);
        for (int col = 0; col < w; col++)
        {
            int px = x + col, py = y + row;
            if (px >= 0 && px < t->fb_w && py >= 0 && py < t->fb_h)
                t->fb[py * t->fb_w + px] = src_row[col];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels into the framebuffer.
 *
 * Scales the source by the global alpha before compositing, matching the behavior the
 * embedded_react scratch system expects from the platform backend.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global alpha scale 0–255.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] w       Width in pixels.
 * @param[in] h       Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    t->blend_calls++;

    for (int row = 0; row < h; row++)
    {
        const uint32_t* src_row = (const uint32_t*)((const uint8_t*)src + row * stride);
        for (int col = 0; col < w; col++)
        {
            int bx = x + col, by = y + row;
            if (bx < 0 || bx >= t->fb_w || by < 0 || by >= t->fb_h)
                continue;

            uint32_t sp = src_row[col];

            /* Scale premultiplied source by global alpha. */
            if (alpha < 255U)
            {
                sp = ((uint32_t)((sp >> 24) & 0xFFU) * alpha / 255U << 24)
                     | ((uint32_t)((sp >> 16) & 0xFFU) * alpha / 255U << 16)
                     | ((uint32_t)((sp >> 8) & 0xFFU) * alpha / 255U << 8) | (uint32_t)(sp & 0xFFU) * alpha / 255U;
            }

            const uint8_t sa = (uint8_t)((sp >> 24) & 0xFFU);
            if (sa == 0U)
                continue;

            if (sa == 255U)
            {
                t->fb[by * t->fb_w + bx] = sp;
            }
            else
            {
                const uint32_t d = t->fb[by * t->fb_w + bx];
                const uint8_t inv_sa = (uint8_t)(255U - sa);
                const uint8_t oa = (uint8_t)(sa + (uint32_t)((d >> 24) & 0xFFU) * inv_sa / 255U);
                const uint8_t or_ = (uint8_t)(((sp >> 16) & 0xFFU) + (uint32_t)((d >> 16) & 0xFFU) * inv_sa / 255U);
                const uint8_t og = (uint8_t)(((sp >> 8) & 0xFFU) + (uint32_t)((d >> 8) & 0xFFU) * inv_sa / 255U);
                const uint8_t ob = (uint8_t)((sp & 0xFFU) + (uint32_t)(d & 0xFFU) * inv_sa / 255U);
                t->fb[by * t->fb_w + bx] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
            }
        }
    }
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Reads a single premultiplied ARGB8888 pixel from the test framebuffer.
 *
 * @param[in] t  TestCtx owning the framebuffer.
 * @param[in] x  X coordinate.
 * @param[in] y  Y coordinate.
 *
 * @return Premultiplied ARGB8888 pixel value at (x, y).
 */
static uint32_t px(const TestCtx* t, int x, int y)
{
    return t->fb[y * t->fb_w + x];
}

/**
 * @brief Returns an ERProps with all int16_t layout fields set to ER_LAYOUT_AUTO.
 *
 * @return ERProps with all layout sentinels initialised and opacity set to 255.
 */
static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/**
 * @brief Zeros the framebuffer and resets call statistics.
 *
 * @param[in,out] t  TestCtx to reset.
 */
static void reset(TestCtx* t)
{
    memset(t->fb, 0, (size_t)t->fb_w * (size_t)t->fb_h * sizeof(uint32_t));
    t->fill_calls = 0;
    t->blend_calls = 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests opacity compositing via the scene graph API.
 *
 * Verifies:
 *  - opacity=255 on a View does not use scratch (direct fill_rect).
 *  - opacity=0 on a View renders nothing into the framebuffer.
 *  - opacity=128 on a View composites correctly: red over white → expected pink pixel.
 *  - Nested opacity: inner opacity=128 red inside outer opacity=128 container over white.
 *  - opacity on a leaf View with no children composites the background fill.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t fb[FB_W * FB_H];
    TestCtx tc = {fb, FB_W, FB_H, 0, 0};
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    /* -----------------------------------------------------------------------
     * opacity=255: no scratch path, background fills directly via fill_rect.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFF0000FFU; /* opaque blue */
        er_node_set_props(root, &rp);
        er_tree_set_root(root);
        er_commit();

        /* The opaque fill must reach the backend directly — no blend call. */
        if (tc.blend_calls != 0)
            return fail("opacity=255: blend_rect should not be called");
        /* Pixel should be premultiplied blue: A=255 R=0 G=0 B=255 → 0xFF0000FF. */
        if (px(&tc, 0, 0) != 0xFF0000FFU)
            return fail("opacity=255: pixel should be blue");

        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * opacity=0: View renders nothing (scratch allocated but alpha=0 blend is a no-op).
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0x00000000U; /* transparent root */
        er_node_set_props(root, &rp);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.width = 8;
        cp.height = 8;
        cp.background_color = 0xFFFF0000U; /* red */
        cp.opacity = 0U;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* alpha=0 blend produces no visible change; framebuffer stays at zero. */
        if (px(&tc, 0, 0) != 0x00000000U)
            return fail("opacity=0: pixel should remain transparent");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * opacity=128: red (0xFFFF0000) View over white root.
     *
     * Expected compositing math:
     *   - Root fill: 0xFFFFFFFF (white, premultiplied) written via fill_rect.
     *   - Scratch render of child: scratch ← 0xFFFF0000 (premultiplied red, A=255).
     *   - er_scratch_pop_blend at alpha=128:
     *       backend blend_rect receives src=0xFFFF0000 premul, global_alpha=128.
     *       Backend scales: A = 255*128/255 = 128, R = 255*128/255 = 128, G=0, B=0
     *       → scaled premul = 0x80800000.
     *       Source-over onto dst=0xFFFFFFFF:
     *         inv_sa = 255 - 128 = 127
     *         out.A = 128 + 255*127/255 = 255
     *         out.R = 128 + 255*127/255 = 255
     *         out.G =   0 + 255*127/255 = 127
     *         out.B =   0 + 255*127/255 = 127
     *       → 0xFFFF7F7F.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* child = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.width = 4;
        cp.height = 4;
        cp.background_color = 0xFFFF0000U; /* red */
        cp.opacity = 128U;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Inside the child region: pink (red composited over white at 50%). */
        const uint32_t inside = px(&tc, 0, 0);
        const uint8_t a = (uint8_t)((inside >> 24) & 0xFFU);
        const uint8_t r = (uint8_t)((inside >> 16) & 0xFFU);
        const uint8_t g = (uint8_t)((inside >> 8) & 0xFFU);
        const uint8_t b = (uint8_t)(inside & 0xFFU);
        if (a != 255U)
            return fail("opacity=128: alpha channel should be 255");
        if (r != 255U)
            return fail("opacity=128: red channel should be 255");
        if (g != 127U)
            return fail("opacity=128: green channel should be 127");
        if (b != 127U)
            return fail("opacity=128: blue channel should be 127");

        /* Outside the child region: still white. */
        if (px(&tc, 8, 8) != 0xFFFFFFFFU)
            return fail("opacity=128: pixel outside child should remain white");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Leaf node opacity: a View with no children and opacity=128.
     * Same expected result as above: red at 128 over white → 0xFFFF7F7F.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* leaf = er_node_create(ER_NODE_VIEW);
        ERProps lp = props_default();
        lp.width = 4;
        lp.height = 4;
        lp.background_color = 0xFFFF0000U; /* red */
        lp.opacity = 128U;
        er_node_set_props(leaf, &lp);

        er_tree_append_child(root, leaf);
        er_tree_set_root(root);
        er_commit();

        const uint32_t pixel = px(&tc, 0, 0);
        if ((uint8_t)((pixel >> 16) & 0xFFU) != 255U)
            return fail("leaf opacity=128: red channel should be 255");
        if ((uint8_t)((pixel >> 8) & 0xFFU) != 127U)
            return fail("leaf opacity=128: green channel should be 127");
        if ((uint8_t)(pixel & 0xFFU) != 127U)
            return fail("leaf opacity=128: blue channel should be 127");

        er_tree_remove_child(root, leaf);
        er_node_destroy(leaf);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Nested opacity: outer=128, inner=255 red, over white.
     *
     * Inner node renders fully opaque into the outer scratch slot.
     * Outer scratch blends at 128 → same result as the single-node case: 0xFFFF7F7F.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* outer = er_node_create(ER_NODE_VIEW);
        ERProps op = props_default();
        op.width = 8;
        op.height = 8;
        op.background_color = 0x00000000U; /* transparent container */
        op.opacity = 128U;
        er_node_set_props(outer, &op);

        ERNode* inner = er_node_create(ER_NODE_VIEW);
        ERProps ip = props_default();
        ip.width = 8;
        ip.height = 8;
        ip.background_color = 0xFFFF0000U; /* red, fully opaque */
        ip.opacity = 255U;
        er_node_set_props(inner, &ip);

        er_tree_append_child(outer, inner);
        er_tree_append_child(root, outer);
        er_tree_set_root(root);
        er_commit();

        /* Outer container is transparent; inner fills it red at full opacity.
         * The combined scratch (pure red) is blended at 128 over white → 0xFFFF7F7F. */
        const uint32_t pixel = px(&tc, 0, 0);
        if ((uint8_t)((pixel >> 16) & 0xFFU) != 255U)
            return fail("nested opacity: red channel should be 255");
        if ((uint8_t)((pixel >> 8) & 0xFFU) != 127U)
            return fail("nested opacity: green channel should be 127");
        if ((uint8_t)(pixel & 0xFFU) != 127U)
            return fail("nested opacity: blue channel should be 127");

        er_tree_remove_child(outer, inner);
        er_tree_remove_child(root, outer);
        er_node_destroy(inner);
        er_node_destroy(outer);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Banded compositing across strip seams: a tall translucent group with two
     * fully-overlapping opaque children.
     *
     * The 16×24 group is taller than a small ERUI_SCRATCH_BAND_H strip, so it is
     * composited in multiple band passes. Group semantics require the composite
     * (blue — the topmost opaque child) to be blended ONCE at alpha 128 over
     * white: 0xFF7F7FFF on every row. Per-primitive alpha (red@128 then blue@128)
     * or a double blend at a band seam would produce different values, and any
     * seam artefact would break row-to-row equality.
     *
     * With the default full-height strip this degenerates to the single-pass
     * path and must produce the identical result.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps gp = props_default();
        gp.width = 16;
        gp.height = 24;
        gp.background_color = 0x00000000U; /* transparent container */
        gp.opacity = 128U;
        er_node_set_props(group, &gp);

        ERNode* red = er_node_create(ER_NODE_VIEW);
        ERProps rd = props_default();
        rd.position = 1; /* ER_POS_ABSOLUTE */
        rd.left = 0;
        rd.top = 0;
        rd.width = 16;
        rd.height = 24;
        rd.background_color = 0xFFFF0000U; /* red, fully opaque */
        er_node_set_props(red, &rd);

        ERNode* blue = er_node_create(ER_NODE_VIEW);
        ERProps bl = props_default();
        bl.position = 1; /* ER_POS_ABSOLUTE */
        bl.left = 0;
        bl.top = 0;
        bl.width = 16;
        bl.height = 24;
        bl.background_color = 0xFF0000FFU; /* blue, fully opaque, on top of red */
        er_node_set_props(blue, &bl);

        er_tree_append_child(group, red);
        er_tree_append_child(group, blue);
        er_tree_append_child(root, group);
        er_tree_set_root(root);
        er_commit();

        /* Expected: blue premul at alpha 128 (0x80000080) over white →
         * r = 0 + 255*127/255 = 127, g = 127, b = 128 + 255*127/255 = 255. */
        const uint32_t top_px = px(&tc, 2, 2);
        if ((uint8_t)((top_px >> 16) & 0xFFU) != 127U)
            return fail("banded overlap: red channel should be 127 (group blend, not per-child)");
        if ((uint8_t)((top_px >> 8) & 0xFFU) != 127U)
            return fail("banded overlap: green channel should be 127");
        if ((uint8_t)(top_px & 0xFFU) != 255U)
            return fail("banded overlap: blue channel should be 255");

        /* Every row of the group must be identical — any band-seam double blend,
         * gap, or missed row shows up as a differing pixel. */
        for (int row = 0; row < 24; row++)
        {
            if (px(&tc, 2, row) != top_px)
                return fail("banded overlap: rows must be identical across band seams");
        }
        /* Just outside the group: untouched white. */
        if (px(&tc, 2, 24) != 0xFFFFFFFFU)
            return fail("banded overlap: pixel below the group should stay white");
        if (px(&tc, 16, 2) != 0xFFFFFFFFU)
            return fail("banded overlap: pixel right of the group should stay white");

        er_tree_remove_child(group, red);
        er_tree_remove_child(group, blue);
        er_tree_remove_child(root, group);
        er_node_destroy(red);
        er_node_destroy(blue);
        er_node_destroy(group);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * Scratch-pool exhaustion falls back to multiplied alpha.
     *
     * Five nested translucent containers (opacity 128 each) exceed
     * ERUI_MAX_OPACITY_DEPTH=4: the innermost group cannot get a scratch slot
     * and must fall back to multiplying its opacity into the child's draw.
     * The red leaf then carries alpha 128·(128/255)^4 = 8 when it reaches the
     * framebuffer → over white: r=255, g=b=247.
     *
     * The old fallback rendered the leaf at FULL opacity inside level 4's
     * composite → effective alpha 16 → g=b=239, so this check also guards
     * against regressing to opacity-dropping.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* level[5];
        for (int i = 0; i < 5; i++)
        {
            level[i] = er_node_create(ER_NODE_VIEW);
            ERProps lp = props_default();
            lp.width = 8;
            lp.height = 8;
            lp.background_color = 0x00000000U; /* transparent container */
            lp.opacity = 128U;
            er_node_set_props(level[i], &lp);
            if (i > 0)
                er_tree_append_child(level[i - 1], level[i]);
        }

        ERNode* leaf = er_node_create(ER_NODE_VIEW);
        ERProps fp = props_default();
        fp.width = 8;
        fp.height = 8;
        fp.background_color = 0xFFFF0000U; /* red, fully opaque */
        er_node_set_props(leaf, &fp);
        er_tree_append_child(level[4], leaf);

        er_tree_append_child(root, level[0]);
        er_tree_set_root(root);
        er_commit();

        const uint32_t pixel = px(&tc, 2, 2);
        const int g = (int)((pixel >> 8) & 0xFFU);
        if ((uint8_t)((pixel >> 16) & 0xFFU) != 255U)
            return fail("depth fallback: red channel should be 255");
        if (g < 245 || g > 249)
            return fail("depth fallback: 5th nesting level must still dim (multiplied-alpha fallback)");

        er_tree_remove_child(level[4], leaf);
        er_node_destroy(leaf);
        for (int i = 4; i > 0; i--)
        {
            er_tree_remove_child(level[i - 1], level[i]);
            er_node_destroy(level[i]);
        }
        er_tree_remove_child(root, level[0]);
        er_node_destroy(level[0]);
        er_node_destroy(root);
    }

#if ERUI_TRANSFORMS_FULL
    /* -----------------------------------------------------------------------
     * Transform inside a banded opacity group.
     *
     * A rotated child inside a tall translucent group is the subtle banded case:
     * its transform-source capture must stay COMPLETE during every band pass
     * (band tiles are not clip rects), while the transformed output lands only
     * in the strip being composited. A 90° rotation of an 8×8 red square about
     * its centre reproduces the same square, so the result must match an
     * unrotated red child blended at the group alpha: ~0xFFFF7F7F on every row,
     * with no gaps at band seams.
     * ---------------------------------------------------------------------- */
    reset(&tc);
    {
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = FB_W;
        rp.height = FB_H;
        rp.background_color = 0xFFFFFFFFU; /* white */
        er_node_set_props(root, &rp);

        ERNode* group = er_node_create(ER_NODE_VIEW);
        ERProps gp = props_default();
        gp.width = 16;
        gp.height = 24;
        gp.background_color = 0x00000000U; /* transparent container */
        gp.opacity = 128U;
        er_node_set_props(group, &gp);

        ERNode* card = er_node_create(ER_NODE_VIEW);
        ERProps cp = props_default();
        cp.position = 1; /* ER_POS_ABSOLUTE */
        cp.left = 4;
        cp.top = 8; /* spans band seams for any small ERUI_SCRATCH_BAND_H */
        cp.width = 8;
        cp.height = 8;
        cp.background_color = 0xFFFF0000U; /* red, fully opaque */
        cp.transform_rotate_z = 90.0f;
        cp.transform_origin_x = -1.0f; /* centre pivot: square maps onto itself */
        cp.transform_origin_y = -1.0f;
        er_node_set_props(card, &cp);

        er_tree_append_child(group, card);
        er_tree_append_child(root, group);
        er_tree_set_root(root);
        er_commit();

        /* Interior of the rotated square, straddling seam rows: red at group alpha
         * over white ≈ (255,127,127). Allow ±2 for bilinear float jitter at 90°. */
        const int probe_x = 8;
        const int probe_rows[3] = {10, 12, 13};
        for (int i = 0; i < 3; i++)
        {
            const uint32_t p = px(&tc, probe_x, probe_rows[i]);
            const int r = (int)((p >> 16) & 0xFFU);
            const int g = (int)((p >> 8) & 0xFFU);
            const int b = (int)(p & 0xFFU);
            if (r < 253 || g < 125 || g > 129 || b < 125 || b > 129)
                return fail("banded transform: interior of rotated child should be red at group alpha on all rows");
        }
        /* Outside the group: untouched white. */
        if (px(&tc, 2, 25) != 0xFFFFFFFFU)
            return fail("banded transform: pixel below the group should stay white");

        er_tree_remove_child(group, card);
        er_tree_remove_child(root, group);
        er_node_destroy(card);
        er_node_destroy(group);
        er_node_destroy(root);
    }
#endif /* ERUI_TRANSFORMS_FULL */

    return EXIT_SUCCESS;
}
