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
 * @param[in] props  Image node props carrying the asset name, resize_mode, and tint_color.
 * @param[in] x     Destination left edge in framebuffer pixels.
 * @param[in] y     Destination top edge in framebuffer pixels.
 * @param[in] w     Destination width in pixels.
 * @param[in] h     Destination height in pixels.
 */
void er_image_render(const ERImageProps* props, int x, int y, int w, int h);

#endif
