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

#ifndef EMBEDDED_REACT_TRANSFORM_H
#define EMBEDDED_REACT_TRANSFORM_H

#include "er_node_internal.h"
#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Returns true when the node's transform is a pure translation (no scale or rotation).
 *
 * A translate-only transform does not require off-screen scratch compositing; the node and its
 * children are rendered with a simple pixel offset.
 *
 * @param[in] n  Node to inspect.
 *
 * @return true when scale and rotation components are identity.
 */
bool er_transform_is_translate_only(const ERNode* n);

/**
 * @brief Computes the 2D affine matrix for a node's transform props applied at a given position.
 *
 * The forward transform maps layout-space coordinates to screen-space coordinates:
 *   screen_x = a * layout_x + c * layout_y + tx
 *   screen_y = b * layout_x + d * layout_y + ty
 *
 * @param[in]  n          Node whose tp_* fields supply the transform parameters.
 * @param[in]  ref_x      Layout-space X origin of the node (computed.x or render px).
 * @param[in]  ref_y      Layout-space Y origin of the node (computed.y or render py).
 * @param[in]  w          Node width in pixels (used to resolve the pivot origin).
 * @param[in]  h          Node height in pixels.
 * @param[out] a          2×2 matrix element [0,0].
 * @param[out] b          2×2 matrix element [1,0].
 * @param[out] c          2×2 matrix element [0,1].
 * @param[out] d          2×2 matrix element [1,1].
 * @param[out] tx         Affine translation X.
 * @param[out] ty         Affine translation Y.
 */
void er_transform_compute_matrix(
    const ERNode* n, int ref_x, int ref_y, int w, int h, float* a, float* b, float* c, float* d, float* tx, float* ty);

/**
 * @brief Inverts a 2D affine matrix.
 *
 * The inverse maps screen-space coordinates back to layout-space:
 *   layout_x = ia * screen_x + ic * screen_y + itx
 *   layout_y = ib * screen_x + id * screen_y + ity
 *
 * @param[in]  a    Forward matrix element [0,0].
 * @param[in]  b    Forward matrix element [1,0].
 * @param[in]  c    Forward matrix element [0,1].
 * @param[in]  d    Forward matrix element [1,1].
 * @param[in]  tx   Forward translation X.
 * @param[in]  ty   Forward translation Y.
 * @param[out] ia   Inverse matrix element [0,0].
 * @param[out] ib   Inverse matrix element [1,0].
 * @param[out] ic   Inverse matrix element [0,1].
 * @param[out] id   Inverse matrix element [1,1].
 * @param[out] itx  Inverse translation X.
 * @param[out] ity  Inverse translation Y.
 *
 * @return true on success; false when the matrix is singular (e.g. scale = 0).
 */
bool er_transform_invert(float a,
                         float b,
                         float c,
                         float d,
                         float tx,
                         float ty,
                         float* ia,
                         float* ib,
                         float* ic,
                         float* id,
                         float* itx,
                         float* ity);

/**
 * @brief Computes the screen-space axis-aligned bounding box of a transformed rectangle.
 *
 * @param[in]  ref_x   Layout-space X origin.
 * @param[in]  ref_y   Layout-space Y origin.
 * @param[in]  w       Rectangle width.
 * @param[in]  h       Rectangle height.
 * @param[in]  a       Forward matrix [0,0].
 * @param[in]  b       Forward matrix [1,0].
 * @param[in]  c       Forward matrix [0,1].
 * @param[in]  d       Forward matrix [1,1].
 * @param[in]  tx      Forward translation X.
 * @param[in]  ty      Forward translation Y.
 * @param[out] out_x   AABB left edge.
 * @param[out] out_y   AABB top edge.
 * @param[out] out_w   AABB width (always ≥ 0).
 * @param[out] out_h   AABB height (always ≥ 0).
 */
void er_transform_aabb(int ref_x,
                       int ref_y,
                       int w,
                       int h,
                       float a,
                       float b,
                       float c,
                       float d,
                       float tx,
                       float ty,
                       int* out_x,
                       int* out_y,
                       int* out_w,
                       int* out_h);

/**
 * @brief Maps a single screen-space point to layout space via the inverse affine matrix.
 *
 * @param[in]  ia       Inverse matrix [0,0].
 * @param[in]  ib       Inverse matrix [1,0].
 * @param[in]  ic       Inverse matrix [0,1].
 * @param[in]  id       Inverse matrix [1,1].
 * @param[in]  itx      Inverse translation X.
 * @param[in]  ity      Inverse translation Y.
 * @param[in]  screen_x Screen-space X.
 * @param[in]  screen_y Screen-space Y.
 * @param[out] layout_x Resulting layout-space X.
 * @param[out] layout_y Resulting layout-space Y.
 */
void er_transform_map_point(float ia,
                            float ib,
                            float ic,
                            float id,
                            float itx,
                            float ity,
                            int screen_x,
                            int screen_y,
                            int* layout_x,
                            int* layout_y);

#if ERUI_3D_TRANSFORMS

/**
 * @brief Returns true when the node has any non-zero 3D-specific transform props.
 *
 * Used by the compositor to choose between the 2D affine path and the 3D
 * perspective-homography path. A node may have both 2D and 3D props; when
 * any 3D prop is set, the 3D path handles the entire transform.
 *
 * @param[in] n  Node to inspect.
 *
 * @return true when tp_rotate_x, tp_rotate_y, or tp_perspective is non-zero.
 */
bool er_transform_is_3d(const ERNode* n);

/**
 * @brief Computes a 3×3 homography mapping layout-space source pixels to screen space.
 *
 * The source plane is z=0 in local space. The homography H maps homogeneous
 * source coordinates (u, v, 1) to homogeneous screen coordinates (X, Y, W)
 * such that screen_x = X/W, screen_y = Y/W.
 *
 * The full transform sequence (applied innermost first) is:
 *   Translate to pivot → ScaleXY → RotZ → RotX → RotY →
 *   Translate back + user translate → Perspective(tp_perspective).
 *
 * @param[in]  n      Node whose tp_* fields supply the parameters.
 * @param[in]  ref_x  Layout-space X origin (computed.x after scroll offset).
 * @param[in]  ref_y  Layout-space Y origin.
 * @param[in]  w      Node width in pixels.
 * @param[in]  h      Node height in pixels.
 * @param[out] H      9-element row-major homography: [H00 H01 H02 H10 H11 H12 H20 H21 H22].
 */
void er_transform_compute_homography_3d(const ERNode* n, int ref_x, int ref_y, int w, int h, float H[9]);

/**
 * @brief Inverts a 3×3 homography.
 *
 * @param[in]  H    Input 9-element row-major homography.
 * @param[out] inv  Inverted 9-element row-major homography.
 *
 * @return true on success; false when H is singular (determinant near zero).
 */
bool er_transform_homography_invert(const float H[9], float inv[9]);

/**
 * @brief Computes the screen-space AABB of a source rectangle projected through a homography.
 *
 * Projects the four corners (ref_x,ref_y), (ref_x+w,ref_y), (ref_x+w,ref_y+h),
 * (ref_x,ref_y+h) and returns their bounding box.  Corners that project behind the
 * viewer (W ≤ 0) are skipped; the AABB is (0,0,0,0) when all corners are behind.
 *
 * @param[in]  ref_x   Source rectangle left edge.
 * @param[in]  ref_y   Source rectangle top edge.
 * @param[in]  w       Source rectangle width.
 * @param[in]  h       Source rectangle height.
 * @param[in]  H       9-element row-major forward homography.
 * @param[out] out_x   AABB left edge.
 * @param[out] out_y   AABB top edge.
 * @param[out] out_w   AABB width (always ≥ 0).
 * @param[out] out_h   AABB height (always ≥ 0).
 */
void er_transform_aabb_3d(
    int ref_x, int ref_y, int w, int h, const float H[9], int* out_x, int* out_y, int* out_w, int* out_h);

/**
 * @brief Ends the 3D transform source capture and blits the result through the homography.
 *
 * For each pixel in the destination AABB, back-projects using the inverse homography
 * to find the source UV, then samples the source scratch with bilinear filtering.
 * The resulting image is blended into the current render target.
 *
 * @param[in] src_x   World-space X origin of the source buffer (matches er_transform_source_begin).
 * @param[in] src_y   World-space Y origin.
 * @param[in] src_w   Logical source width.
 * @param[in] src_h   Logical source height.
 * @param[in] inv_H   9-element row-major inverse homography (from er_transform_homography_invert).
 * @param[in] dst_x   Screen-space AABB left edge.
 * @param[in] dst_y   Screen-space AABB top edge.
 * @param[in] dst_w   Screen-space AABB width (must be ≤ ERUI_SCRATCH_W).
 * @param[in] dst_h   Screen-space AABB height (must be ≤ ERUI_SCRATCH_H).
 */
void er_transform_source_end_blit_3d(
    int src_x, int src_y, int src_w, int src_h, const float inv_H[9], int dst_x, int dst_y, int dst_w, int dst_h);

#endif /* ERUI_3D_TRANSFORMS */

/**
 * @brief Begins capturing render output for a full-affine transform into an internal buffer.
 *
 * All subsequent er_blit_* calls route into an off-screen scratch slot until
 * er_transform_source_end_blit() is called.  Any active opacity scratch slots nested
 * inside the transform capture will still blend correctly into the transform source buffer.
 *
 * Only one transform capture may be active at a time.  Returns false and leaves routing
 * unchanged when another capture is already active or the node size exceeds the scratch limits.
 *
 * @param[in] src_x  World-space X origin of the content being captured (= render px).
 * @param[in] src_y  World-space Y origin of the content being captured (= render py).
 * @param[in] w      Content width in pixels (must be ≤ ERUI_SCRATCH_W).
 * @param[in] h      Content height in pixels (must be ≤ ERUI_SCRATCH_H).
 *
 * @return true when capture was started successfully.
 */
bool er_transform_source_begin(int src_x, int src_y, int w, int h);

/**
 * @brief Ends the transform source capture and blits the result through the affine transform.
 *
 * The source scratch is inverse-mapped to produce the output in the destination bounding box,
 * then blended into the current render target (framebuffer or active outer scratch) via
 * er_blit_blend.
 *
 * @param[in] src_x  World-space X origin of the source buffer (must match er_transform_source_begin).
 * @param[in] src_y  World-space Y origin.
 * @param[in] src_w  Logical source width.
 * @param[in] src_h  Logical source height.
 * @param[in] ia     Inverse matrix [0,0].
 * @param[in] ib     Inverse matrix [1,0].
 * @param[in] ic     Inverse matrix [0,1].
 * @param[in] id     Inverse matrix [1,1].
 * @param[in] itx    Inverse translation X.
 * @param[in] ity    Inverse translation Y.
 * @param[in] dst_x  Screen-space AABB left edge.
 * @param[in] dst_y  Screen-space AABB top edge.
 * @param[in] dst_w  Screen-space AABB width (must be ≤ ERUI_SCRATCH_W).
 * @param[in] dst_h  Screen-space AABB height (must be ≤ ERUI_SCRATCH_H).
 */
void er_transform_source_end_blit(int src_x,
                                  int src_y,
                                  int src_w,
                                  int src_h,
                                  float ia,
                                  float ib,
                                  float ic,
                                  float id,
                                  float itx,
                                  float ity,
                                  int dst_x,
                                  int dst_y,
                                  int dst_w,
                                  int dst_h);

#endif /* EMBEDDED_REACT_TRANSFORM_H */
