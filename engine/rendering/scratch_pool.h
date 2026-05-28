#ifndef EMBEDDED_REACT_SCRATCH_POOL_H
#define EMBEDDED_REACT_SCRATCH_POOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*----------------------------------------------------------------------------------------------------------------------
     - Functions: Public
     ---------------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Allocates a scratch slot and begins routing blit calls into it.
     *
     * All er_blit_fill / er_blit_copy / er_blit_blend calls made after this return write into
     * an off-screen premultiplied ARGB8888 slot rather than the real framebuffer.  The slot is
     * pre-cleared to transparent (0x00000000).  Calls must be balanced with
     * er_scratch_pop_blend().
     *
     * Up to ERUI_MAX_OPACITY_DEPTH nested slots may be active simultaneously.  A push fails
     * (returning false) when the pool is exhausted or the node is wider than ERUI_SCRATCH_W
     * or taller than ERUI_SCRATCH_H.  On failure blit routing is unchanged and no matching
     * er_scratch_pop_blend() is required.
     *
     * @param[in] x  World-space left edge of the node being composited.
     * @param[in] y  World-space top edge of the node being composited.
     * @param[in] w  Node width in pixels.
     * @param[in] h  Node height in pixels.
     *
     * @return true if a slot was allocated and blit redirection is now active.
     */
    bool er_scratch_push(int x, int y, int w, int h);

    /**
     * @brief Blends the top scratch slot onto the destination and releases it.
     *
     * Restores blit routing to whatever was active before the matching er_scratch_push()
     * call (the real framebuffer, or the next outer scratch slot for nested opacity), then
     * alpha-blends the accumulated scratch content over that destination at the given opacity.
     *
     * @param[in] alpha  Global opacity 0–255 (255 = fully opaque).
     * @param[in] x     World-space left edge (must match the er_scratch_push() call).
     * @param[in] y     World-space top edge (must match the er_scratch_push() call).
     * @param[in] w     Width in pixels (must match the er_scratch_push() call).
     * @param[in] h     Height in pixels (must match the er_scratch_push() call).
     */
    void er_scratch_pop_blend(uint8_t alpha, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* EMBEDDED_REACT_SCRATCH_POOL_H */
