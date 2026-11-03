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

#endif
