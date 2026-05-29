#ifndef EMBEDDED_REACT_SHADOW_H
#define EMBEDDED_REACT_SHADOW_H

#include "er_node_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*----------------------------------------------------------------------------------------------------------------------
     - Functions: Public
     ---------------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Renders a box shadow behind a View-type node.
     *
     * The shadow is blended directly into the current render destination before the node's
     * own background is drawn, so it appears underneath the node.  A two-pass separable box
     * blur (horizontal then vertical) approximates a soft Gaussian shadow.  The blur radius
     * is automatically clamped so the working buffer stays within ERUI_SCRATCH_W ×
     * ERUI_SCRATCH_H pixels.  When the node is larger than the scratch buffer the shadow is
     * silently skipped.
     *
     * When ERUI_SHADOWS is 0 this function compiles to a no-op and adds zero code size.
     *
     * @param[in] vp  View props carrying shadow_color, shadow_offset_x, shadow_offset_y,
     *                shadow_opacity, shadow_radius, and elevation.
     * @param[in] x   Node left edge in framebuffer pixels (after any translate transform).
     * @param[in] y   Node top edge in framebuffer pixels (after any translate transform).
     * @param[in] w   Node width in pixels.
     * @param[in] h   Node height in pixels.
     */
    void er_shadow_render(const ERViewProps* vp, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* EMBEDDED_REACT_SHADOW_H */
