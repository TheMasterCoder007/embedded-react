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

#ifndef EMBEDDED_REACT_ARC_WIDGET_H
#define EMBEDDED_REACT_ARC_WIDGET_H

#include "er_node_internal.h"
#include <stdbool.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Resolved geometry of an Arc node inside a given box — every default applied, in pixels / degrees.
 */
typedef struct
{
    float center_x;    /**< Centre X (box space + the origin passed in). */
    float center_y;    /**< Centre Y. */
    float mid_radius;  /**< Radius of the track's mid-line (where the knob centre rides). */
    float half_extent; /**< Half of the widest ring (track or band): the ring's reach either side of mid_radius. */
    float width;       /**< Track + indicator thickness. */
    float band;        /**< Backing band thickness (0 = none). */
    float start_angle; /**< Sweep start angle, degrees clockwise from +X. */
    float sweep;       /**< Sweep extent, degrees in (0, 360]. */
    float min;         /**< Value range start. */
    float max;         /**< Value range end (> min). */
    int knob_size;     /**< Drawn knob diameter (circle / image modes); 0 = no drawn knob. */
} ERArcGeom;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Resolves an Arc node's geometry for a box at (box_x, box_y) of size box_width x box_height.
 *
 * The box passed in is the node's BORDER box; the dial is resolved inside the content box, with the
 * node's style padding taken off first. Every caller therefore inherits the inset — paint, hit test,
 * knob-child placement, damage — without repeating it. A caller passing (0, 0, width, height) gets geometry
 * relative to the border-box origin, padding included, which is the space vector_dirty_* is stored in.
 */
void er_arc_geom(const ERNode* node, int box_x, int box_y, int box_width, int box_height, ERArcGeom* geometry);

/**
 * @brief Maps a value to its sweep fraction [0, 1] under the node's range.
 */
float er_arc_value_frac(const ERArcGeom* geometry, float value);

/**
 * @brief Maps a value to the absolute angle (degrees) of its indicator end.
 */
float er_arc_value_angle(const ERArcGeom* geometry, float value);

/**
 * @brief Centre of the knob for @p value, in the geometry's pixel space.
 */
void er_arc_knob_center(const ERArcGeom* geometry, float value, float* knob_x, float* knob_y);

/**
 * @brief Paints the node: backing band, track, indicator, then the drawn knob. Clips to the active scissor.
 *        Records the painted value so the next value change can bound its damage.
 */
void er_arc_render(ERNode* node, int box_x, int box_y, int width, int height);

/**
 * @brief Recomputes and stores ERNode::arc_overhang from the current props and computed size.
 *
 * Folded into the paint bounds, the damage rects, the last-paint trail and the hit zone, so a knob wider
 * than the ring is not clipped and leaves no trail. NOT honoured on a rotated/scaled arc: that path
 * renders through a `width × height` transform scratch and bounds its damage by the transformed layout box, so an
 * overhanging knob is clipped there (see the Arc section of engine/README.md).
 *
 * @return The new overhang in pixels (>= 0).
 */
int er_arc_refresh_overhang(ERNode* node);

/**
 * @brief Applies a new value (props or animation) and records the damage it implies.
 *
 * Clamps to the range, and when only the value moved, narrows the node's pending damage to the swept
 * sub-arc plus the knob's old and new footprints (via the node's vector_dirty rect). A node whose knob is an
 * anchored child also requests a layout pass so the child is re-anchored and the prune bounds refreshed.
 *
 * @return true when the stored value changed (the caller marks the node dirty); false for a no-op.
 */
bool er_arc_apply_value(ERNode* node, float value);

/**
 * @brief Applies a new low end to a RANGE arc (arc_value_start), with the same clamping and damage rules.
 *
 * @return true when the stored value changed (the caller marks the node dirty).
 */
bool er_arc_apply_value_start(ERNode* node, float value);

/**
 * @brief Picks the end of a RANGE arc a touch point should grab: true for the LOW end (arc_value_start).
 *
 * Compares the touched value against both ends and returns the nearer. Always false for a single-ended arc.
 */
bool er_arc_grab_low(const ERNode* node, int x, int y);

/**
 * @brief Hit test in the node's own layout space: true on the ring band (a few pixels of slop either side,
 *        within the sweep) or on the knob; the centre hole and the unswept gap are transparent.
 */
bool er_arc_hit(const ERNode* node, int x, int y);

/**
 * @brief Value under a touch point (node layout space), quantized to the node's step.
 *
 * A point in the unswept gap resolves to the nearer end. When @p anti_wrap is true (a continuing drag) a
 * jump across the gap — from one end straight to the other — is suppressed by pinning to the end the
 * finger came from, using ERNode::arc_drag_fraction as the reference.
 */
float er_arc_value_at(ERNode* node, int x, int y, bool anti_wrap);

/**
 * @brief Moves the node's first child so its centre sits on the knob point (ER_ARC_KNOB_CHILD). Called
 *        after every layout pass; a no-op for other knob modes.
 */
void er_arc_anchor_child(ERNode* node);

#endif
