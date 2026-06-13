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

#ifndef EMBEDDED_REACT_LAYOUT_ENGINE_H
#define EMBEDDED_REACT_LAYOUT_ENGINE_H

#include "er_node_internal.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Runs the Yoga-compatible flexbox layout pass on the subtree rooted at root_tag.
 *
 * Recursively resolves all flex-grow, flex-shrink, wrapping, alignment, justify-content,
 * and absolute-position constraints, writing computed x/y/w/h into each node's
 * ERLayoutRect. The root node is given screen origin (0, 0).
 *
 * @param[in] root_tag  Tag of the root node to start the layout pass from.
 * @param[in] w         Width allocated to the root node in pixels.
 * @param[in] h         Height allocated to the root node in pixels.
 */
void er_layout_compute(uint16_t root_tag, int16_t w, int16_t h);

#endif
