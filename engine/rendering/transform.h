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
