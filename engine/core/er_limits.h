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

#ifndef EMBEDDED_REACT_ER_LIMITS_H
#define EMBEDDED_REACT_ER_LIMITS_H

/*----------------------------------------------------------------------------------------------------------------------
 - Build-time pool and buffer sizes that more than one module must agree on.
 *
 * The engine CMake (and each board component) defines these on the command line; the #ifndef fallbacks here
 * are what a consumer compiling the sources directly gets. They live in one header because the modules that
 * read them index the SAME arrays: a value written per-file drifts silently the moment one copy is bumped —
 * layout stops covering the upper node range, or a shadow renders into a scratch slot sized by someone else.
 *
 * Per-module knobs stay with their module (the vector pools in vector.c, the arc span cache in arc.c).
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Scene-graph node pool size.
 *
 * Shared by the pool itself (scene/compositor.c), the layout scratch arrays (layout/layout_engine.c) and the
 * hit-test walks (scene/hit_test.c) — all three index nodes by tag over the same range.
 */
#ifndef ERUI_MAX_NODES
#define ERUI_MAX_NODES 512
#endif

/**
 * @brief Opacity-composite scratch width in pixels (the widest node that can be captured offscreen).
 *
 * rendering/scratch_pool.c owns the allocation; rendering/shadow.c and rendering/transform.c size their own
 * working buffers to match and refuse anything larger.
 */
#ifndef ERUI_SCRATCH_W
#define ERUI_SCRATCH_W 240
#endif

/** @brief Scratch height in pixels; also the default for the band strip and the transform source. */
#ifndef ERUI_SCRATCH_H
#define ERUI_SCRATCH_H 240
#endif

/** @brief Opacity-composite strip height; smaller trades RAM for extra banded passes over a tall subtree. */
#ifndef ERUI_SCRATCH_BAND_H
#define ERUI_SCRATCH_BAND_H ERUI_SCRATCH_H
#endif

/** @brief Nested offscreen-composite layers the scratch pool can hold at once. */
#ifndef ERUI_MAX_OPACITY_DEPTH
#define ERUI_MAX_OPACITY_DEPTH 4
#endif

/* The transform source defaults to the scratch dims so a consumer that sizes only ERUI_SCRATCH_W/H keeps its
 * old transform budget; set ERUI_XFORM_W/H to decouple them (useful when the strips are screen-wide). */
#ifndef ERUI_XFORM_W
#define ERUI_XFORM_W ERUI_SCRATCH_W
#endif
#ifndef ERUI_XFORM_H
#define ERUI_XFORM_H ERUI_SCRATCH_H
#endif

/**
 * @brief Static row-buffer width for the per-row rasterizers.
 *
 * rendering/image_scaler.c and rendering/gradient.c both assemble one row here, but they do NOT handle an
 * over-wide row alike: the image scaler chunks across it, while the gradient renders only the first
 * ERUI_MAX_IMG_ROW_PIXELS columns and truncates the rest. So size this to cover the widest GRADIENT on the
 * screen, not just the widest scaled image — below that, wide gradients lose their right-hand side.
 */
#ifndef ERUI_MAX_IMG_ROW_PIXELS
#define ERUI_MAX_IMG_ROW_PIXELS 800
#endif

#endif
