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

#ifndef EMBEDDED_REACT_GRADIENT_H
#define EMBEDDED_REACT_GRADIENT_H

#include "er_node_internal.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Renders a gradient background into a rectangular region of the framebuffer.
 *
 * Dispatches to the linear or radial rasterizer based on vp->gradient_type.  Each rasterizer
 * writes one row of premultiplied ARGB8888 pixels at a time and flushes it via er_blit_blend.
 *
 * The fill is rectangular — border_radius clipping is NOT applied.  When a View also carries a
 * border, the border strokes (which are rounded) are rendered on top of the gradient by the
 * caller after this function returns.
 *
 * When ERUI_GRADIENT is 0 this function compiles to a no-op.
 * When ERUI_GRADIENT_RADIAL is 0 the radial path is excluded but the linear path remains.
 *
 * @param[in] vp  View props carrying gradient_type, gradient_angle, gradient_stop_count, and
 *                gradient_stops.  Nothing is drawn when vp is NULL or gradient_type ==
 *                ER_GRADIENT_NONE.
 * @param[in] x   Destination left edge in framebuffer pixels.
 * @param[in] y   Destination top edge in framebuffer pixels.
 * @param[in] w   Destination width in pixels.
 * @param[in] h   Destination height in pixels.
 */
void er_gradient_render(const ERViewProps* vp, int x, int y, int w, int h);

#endif /* EMBEDDED_REACT_GRADIENT_H */
