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
