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

#ifndef EMBEDDED_REACT_IMAGE_SCALER_H
#define EMBEDDED_REACT_IMAGE_SCALER_H

#include "er_node_internal.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Renders an Image node's bitmap into the active backend at the given destination rect.
 *
 * The image is looked up in the image registry by name. If not found, nothing is drawn.
 * Scaling mode and tint color are taken from props.
 *
 * When props carries a corner radius and @p box is given, the bitmap is clipped to the rounded rect
 * @p box describes, with the same anti-aliased edge a background with those radii is filled with.
 * Rows no corner arc cuts into take the same paths a square image does.
 *
 * @param[in] props   Image node props carrying the asset name, resize_mode, tint_color and corner radii.
 * @param[in] x       Destination left edge in framebuffer pixels.
 * @param[in] y       Destination top edge in framebuffer pixels.
 * @param[in] width   Destination width in pixels.
 * @param[in] height  Destination height in pixels.
 * @param[in] box     The node's border box, which the corner radii round; NULL draws square corners.
 */
void er_image_render(const ERImageProps* props, int x, int y, int width, int height, const ERRect* box);

#if ERUI_TRANSFORMS_FULL

/**
 * @brief Renders an Image node's bitmap through the node's affine transform, with no transform buffer.
 *
 * Each screen pixel is mapped back through the inverse matrix and the asset is sampled bilinearly in place.
 * The resize mode sets the source crop and the rectangle it fills, as in er_image_render(), and the tint
 * applies per sample. The image fades over one pixel at its edges. Output goes out a row at a time, so the
 * node can be any size.
 *
 * `repeat` and rounded corners are not drawn here: er_node_transforms_directly() keeps those on the
 * transform buffer.
 *
 * @param[in] props                Image node props carrying the asset name, resize_mode and tint_color.
 * @param[in] x                    Content box left edge, in layout space.
 * @param[in] y                    Content box top edge.
 * @param[in] width                Content box width.
 * @param[in] height               Content box height.
 * @param[in] inverse_a            Inverse matrix [0,0], screen to layout.
 * @param[in] inverse_b            Inverse matrix [1,0].
 * @param[in] inverse_c            Inverse matrix [0,1].
 * @param[in] inverse_d            Inverse matrix [1,1].
 * @param[in] inverse_translate_x  Inverse translation X.
 * @param[in] inverse_translate_y  Inverse translation Y.
 * @param[in] clip_x0              Screen clip left edge: the node's screen bounds within the damage clip.
 * @param[in] clip_y0              Screen clip top edge.
 * @param[in] clip_x1              Screen clip right edge (exclusive).
 * @param[in] clip_y1              Screen clip bottom edge (exclusive).
 */
void er_image_render_transformed(const ERImageProps* props,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 float inverse_a,
                                 float inverse_b,
                                 float inverse_c,
                                 float inverse_d,
                                 float inverse_translate_x,
                                 float inverse_translate_y,
                                 int clip_x0,
                                 int clip_y0,
                                 int clip_x1,
                                 int clip_y1);

#endif /* ERUI_TRANSFORMS_FULL */

#endif
