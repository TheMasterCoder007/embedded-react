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

#ifndef EMBEDDED_REACT_BORDER_SWEEP_H
#define EMBEDDED_REACT_BORDER_SWEEP_H

#include "er_scene.h"
#include <stdint.h>

/**
 * @brief Draws a light sweeping around a rounded-rect border: a ring `width` px thick inside the box,
 * brightest at `phase` (a fraction of the perimeter, clockwise from the top edge's left end) and fading
 * over the `length` of the perimeter behind it. Anti-aliased against both edges of the ring.
 *
 * @param[in] x, y, w, h  The node's box in screen pixels.
 * @param[in] radius      Corner radius of the box (clamped to half its shorter side).
 * @param[in] width       Ring thickness in pixels.
 * @param[in] argb        Straight-alpha colour at the head.
 * @param[in] phase       Head position, 0..1 around the perimeter (wrapped).
 * @param[in] length      Tail length as a fraction of the perimeter, 0..1.
 */
void er_border_sweep_render(
    int x, int y, int w, int h, int radius, int width, uint32_t argb, float phase, float length);

/**
 * @brief Draws a conic gradient seen only through a rounded-rect ring `width` px thick inside the box: the
 * rotating-gradient border. Colours follow the angle around the box centre, CSS conic-gradient style.
 *
 * @param[in] x, y, w, h  The node's box in screen pixels.
 * @param[in] radius      Corner radius of the box.
 * @param[in] width       Ring thickness in pixels.
 * @param[in] from        Start angle in degrees: 0 points up, positive turns clockwise.
 * @param[in] stops       Straight-alpha colour stops at positions 0–1 around the circle, ascending.
 * @param[in] count       Number of stops.
 */
void er_border_conic_render(
    int x, int y, int w, int h, int radius, int width, float from, const ERGradientStop* stops, int count);

/**
 * @brief How far the sweep reaches in from each edge of the box: the bands a phase change repaints.
 *
 * @param[in] radius  Corner radius.
 * @param[in] width   Ring thickness.
 *
 * @return Band thickness in pixels.
 */
int er_border_sweep_reach(int radius, int width);

#endif /* EMBEDDED_REACT_BORDER_SWEEP_H */
