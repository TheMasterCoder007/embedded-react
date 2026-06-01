#ifndef EMBEDDED_REACT_LAYOUT_ANIM_H
#define EMBEDDED_REACT_LAYOUT_ANIM_H

#include "er_node_internal.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Walks the scene tree after er_layout_compute() to start or update layout
 *        animations on nodes whose computed rect changed.
 *
 * For each node whose computed rect differs from its current animated rect: if a pending
 * er_layout_anim_configure_next() config exists, a layout animation is started (or
 * retargeted) that interpolates animated from its current position to computed; otherwise
 * animated is snapped to computed immediately.  Nodes appearing for the first time
 * (prev_computed was zero-size) are always snapped regardless of the pending config.
 *
 * Consumes the pending config after one invocation.
 *
 * @param[in,out] root  Scene root node.
 */
void er_layout_anim_post_layout(ERNode* root);

/**
 * @brief Reports whether a one-shot LayoutAnimation config is awaiting the next commit.
 *
 * er_commit() consults this so that a pending er_layout_anim_configure_next() forces a
 * layout pass even when no layout-affecting prop changed, preserving the "evaluate on the
 * next commit" contract while the layout-dirty fast path skips static frames.
 *
 * @return true when a config set by er_layout_anim_configure_next() has not yet been consumed.
 */
bool er_layout_anim_has_pending(void);

#endif
