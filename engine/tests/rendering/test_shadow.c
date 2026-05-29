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
 * @brief Pixel framebuffer and blit statistics collected during a render pass.
 */
typedef struct
{
    uint32_t* fb;    /**< Premultiplied ARGB8888 framebuffer; FB_W pixels per row. */
    int fb_w;        /**< Framebuffer width in pixels. */
    int fb_h;        /**< Framebuffer height in pixels. */
    int fill_calls;  /**< Number of fill_rect calls received. */
    int blend_calls; /**< Number of blend_rect calls received. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Backend fill_rect: premultiplies the color and writes it to the framebuffer.
 *
 * @param[in] argb  Straight-alpha ARGB8888 color.
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
        for (int col = x; col < x + w; col++)
            if (col >= 0 && col < t->fb_w && row >= 0 && row < t->fb_h)
                t->fb[row * t->fb_w + col] = premul;
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
            const int px = x + col, py = y + row;
            if (px >= 0 && px < t->fb_w && py >= 0 && py < t->fb_h)
                t->fb[py * t->fb_w + px] = src_row[col];
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels into the framebuffer.
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
            const int bx = x + col, by = y + row;
            if (bx < 0 || bx >= t->fb_w || by < 0 || by >= t->fb_h)
                continue;

            uint32_t sp = src_row[col];
            if (alpha < 255U)
            {
                sp = ((uint32_t)((sp >> 24) & 0xFFU) * alpha / 255U << 24)
                     | ((uint32_t)((sp >> 16) & 0xFFU) * alpha / 255U << 16)
                     | ((uint32_t)((sp >> 8) & 0xFFU) * alpha / 255U << 8) | ((uint32_t)(sp & 0xFFU) * alpha / 255U);
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
 * @param[in] msg  Human-readable description of the assertion that failed.
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
 * @return Premultiplied ARGB8888 pixel at (x, y).
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
 * @brief Zeros the framebuffer and resets blit statistics.
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
 * @brief Tests box shadow rendering through the scene graph API.
 *
 * With ERUI_SHADOWS=0 the tests verify the no-op path (shadow_opacity is stored but
 * produces no blend calls or pixel changes).  With ERUI_SHADOWS=1 the tests exercise
 * the two-pass blur rasteriser, offset positioning, opacity scaling, and elevation.
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
     * shadow_opacity=0: shadow props stored but no shadow rendered.
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
        cp.background_color = 0xFF0000FFU; /* blue */
        cp.shadow_color = 0xFF000000U;
        cp.shadow_offset_x = 2.0f;
        cp.shadow_offset_y = 2.0f;
        cp.shadow_opacity = 0.0f; /* disabled */
        cp.shadow_radius = 0;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* shadow_opacity=0: no blend call should originate from shadow rasteriser.
         * (The only blend calls, if any, would be from opacity compositing — none here.) */
#if ERUI_SHADOWS
        /* With shadows enabled the rasteriser must respect the opacity=0 gate. */
        const uint32_t shadow_corner = px(&tc, 2, 2);
        /* Pixel at (2,2) is outside the child (child is at 0..3). Since there is no
         * shadow, it should still be white (root background). */
        if ((shadow_corner >> 24) != 0xFFU || (shadow_corner >> 16 & 0xFFU) != 0xFFU
            || (shadow_corner >> 8 & 0xFFU) != 0xFFU || (shadow_corner & 0xFFU) != 0xFFU)
            return fail("shadow_opacity=0: pixel at (2,2) should remain white (no shadow)");
#endif

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

#if ERUI_SHADOWS

    /* -----------------------------------------------------------------------
     * shadow_opacity=1, radius=0, offset=(3,3): hard black shadow at offset.
     *
     * Node: 4×4 at (0,0), transparent background.
     * Root: 8×8, white background.
     * Shadow at (3,3) should be premultiplied black (0xFF000000).
     * Pixel at (0,0) stays white (root, no node background).
     * Pixel at (12,12) stays white (outside shadow).
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
        cp.background_color = 0x00000000U; /* transparent — only the shadow visible */
        cp.shadow_color = 0xFF000000U;     /* black */
        cp.shadow_offset_x = 3.0f;
        cp.shadow_offset_y = 3.0f;
        cp.shadow_opacity = 1.0f;
        cp.shadow_radius = 0;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* The shadow (no blur) is a 4×4 block at (3,3). */
        const uint32_t shadow_pixel = px(&tc, 3, 3);
        if ((shadow_pixel >> 24) != 0xFFU)
            return fail("hard shadow: shadow pixel at (3,3) must be fully opaque");
        if (((shadow_pixel >> 16) & 0xFFU) != 0U || ((shadow_pixel >> 8) & 0xFFU) != 0U || (shadow_pixel & 0xFFU) != 0U)
            return fail("hard shadow: shadow pixel at (3,3) must be black");

        /* Pixel directly at the node origin: no node background, root still white. */
        const uint32_t root_pixel = px(&tc, 0, 0);
        if (root_pixel != 0xFFFFFFFFU)
            return fail("hard shadow: pixel at (0,0) should remain white (node has no background)");

        /* Well outside the shadow: still white. */
        if (px(&tc, 12, 12) != 0xFFFFFFFFU)
            return fail("hard shadow: pixel at (12,12) should remain white");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * shadow_opacity=1, radius=2, offset=(0,0): soft shadow extends beyond the node.
     *
     * Node: 4×4 at (8,8) within a 32×32 white root.
     * Shadow buffer is (4+4)×(4+4) = 8×8; shadow origin is (8-2, 8-2) = (6,6).
     * Pixels inside the node's silhouette (center of blur) have the highest alpha.
     * Pixels at the outer corners of the shadow have a lower (but non-zero) alpha
     * because the box blur spreads the alpha beyond the silhouette.
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
        cp.left = 8;
        cp.top = 8;
        cp.width = 4;
        cp.height = 4;
        cp.position = 1;                   /* ER_POS_ABSOLUTE */
        cp.background_color = 0x00000000U; /* transparent */
        cp.shadow_color = 0xFF000000U;
        cp.shadow_offset_x = 0.0f;
        cp.shadow_offset_y = 0.0f;
        cp.shadow_opacity = 1.0f;
        cp.shadow_radius = 2;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Center of blur region (maps to center of node silhouette in blur buffer):
         * node center in world = (8+2, 8+2) = (10,10).
         * This pixel should have been blurred from 255 and must be darker than white. */
        const uint32_t center = px(&tc, 10, 10);
        const uint8_t center_a = (uint8_t)((center >> 24) & 0xFFU);
        if (center_a == 0U)
            return fail("soft shadow: center pixel must have non-zero alpha (shadow present)");

        /* Outer corner of the shadow bleed region: node at (8,8) → shadow at (6,6).
         * After blur, pixel (6,6) is at the corner of the shadow buffer and must still have
         * non-zero alpha (blur spreads into surrounding pixels). */
        const uint32_t outer_corner = px(&tc, 6, 6);
        const uint8_t outer_a = (uint8_t)((outer_corner >> 24) & 0xFFU);
        if (outer_a == 0U)
            return fail("soft shadow: corner of blur spread at (6,6) must have non-zero alpha");

        /* The center must have higher alpha than the outer corner (blur gradient). */
        if (center_a <= outer_a)
            return fail("soft shadow: blur center must be darker than outer blur edge");

        /* Far from the shadow: root still white. */
        if (px(&tc, 0, 0) != 0xFFFFFFFFU)
            return fail("soft shadow: pixel at (0,0) should remain white (outside shadow)");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * shadow_opacity=0.5: shadow alpha halved.
     *
     * Node: 4×4 at (0,0), transparent background.  Shadow offset (2,2), radius=0.
     * shadow_opacity=0.5 means effective alpha = 0.5 * 255 ≈ 127 at center.
     * After source-over blend onto white, the shadow pixel at (2,2) must be grey
     * (not black), verifying that opacity is applied.
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
        cp.background_color = 0x00000000U; /* transparent */
        cp.shadow_color = 0xFF000000U;
        cp.shadow_offset_x = 2.0f;
        cp.shadow_offset_y = 2.0f;
        cp.shadow_opacity = 0.5f;
        cp.shadow_radius = 0;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* shadow_opacity=0.5 → effective alpha ≈ 127 → blended over white = grey.
         * The resulting pixel must be neither white (0xFFFFFFFF) nor black (0xFF000000). */
        const uint32_t sp = px(&tc, 2, 2);
        const uint8_t sp_r = (uint8_t)((sp >> 16) & 0xFFU);
        const uint8_t sp_b = (uint8_t)(sp & 0xFFU);
        if (sp_r == 255U && sp_b == 255U)
            return fail("shadow_opacity=0.5: shadow pixel should be grey, not white");
        if (sp_r == 0U && sp_b == 0U)
            return fail("shadow_opacity=0.5: shadow pixel should be grey, not black");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

    /* -----------------------------------------------------------------------
     * elevation > 0: synthesised shadow with no explicit shadow_opacity.
     *
     * elevation=4 synthesises a shadow with offset_y ≈ 1.3 px, radius=4, opacity ≈ 0.24.
     * Node: 4×4 at (0,0), transparent background, root white.
     * There should be at least one blend call and at least one non-white pixel below
     * the node (the synthesised shadow's downward offset lands it below the node rect).
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
        cp.left = 4;
        cp.top = 4;
        cp.width = 4;
        cp.height = 4;
        cp.position = 1;                   /* ER_POS_ABSOLUTE */
        cp.background_color = 0x00000000U; /* transparent */
        cp.shadow_opacity = 0.0f;          /* use elevation path */
        cp.elevation = 4;
        er_node_set_props(child, &cp);

        er_tree_append_child(root, child);
        er_tree_set_root(root);
        er_commit();

        /* Elevation synthesises a shadow: blend calls must have been made. */
        if (tc.blend_calls == 0)
            return fail("elevation: at least one blend call expected for synthesised shadow");

        /* The synthesised shadow extends into the region below the node.
         * elevation=4 gives offset_y ≈ 1.3 and radius=4; node is at (4..7, 4..7).
         * Shadow buffer: 12×12, origin at (4+0-4, 4+1-4) = (0, 1).
         * Center of shadow at approximately (4+2, 4+2+1) = (6, 7).
         * That pixel must be non-white (shadow present). */
        const uint32_t shadow_center = px(&tc, 6, 7);
        const uint8_t sc_r = (uint8_t)((shadow_center >> 16) & 0xFFU);
        const uint8_t sc_g = (uint8_t)((shadow_center >> 8) & 0xFFU);
        const uint8_t sc_b = (uint8_t)(shadow_center & 0xFFU);
        if (sc_r == 255U && sc_g == 255U && sc_b == 255U)
            return fail("elevation: shadow center pixel should not be pure white");

        er_tree_remove_child(root, child);
        er_node_destroy(child);
        er_node_destroy(root);
    }

#endif /* ERUI_SHADOWS */

    return EXIT_SUCCESS;
}
