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

// Imperative escape hatch for high-frequency interactive updates.
//
// Going through React on every pointer move is too slow on some devices — the
// reconcile + flatten dominate. For continuous gestures (e.g., dragging a dial), grab a node handle via
// a ref and push updates directly here: it skips React entirely and, for vectors, skips the d-string
// parser too. Commit back to React state when the gesture ends so the declarative tree re-syncs.
import {NativeUI} from '../native-ui.mts';
import {
  shapesToVector,
  warnVectorCaps,
  encodeVectorGradients,
} from './svg-ops.mts';
import type {KeyboardConfig, ScrollPosition, VectorShape} from './index.d.ts';

type Handle = number | null | undefined;

/**
 * Imperatively sets an <Svg> node's geometry from primitive shape descriptors (see shapesToVector).
 * `handle` is the node handle from a ref on an <Svg/>. `shapes` is a flat array of shape descriptors
 * ({arc}/{circle}/{line}/{rect}/{path} + paint). `dirtyRect` is an optional node-local [x, y, w, h]
 * bounding the change; when given, only that region is repainted and flushed (a big win for small updates
 * on a large vector node). Omit it to repaint the whole node box.
 */
export function updateVector(
  handle: Handle,
  shapes: VectorShape[],
  dirtyRect?: [number, number, number, number],
): void {
  if (handle == null) return;
  const {ops, paints, gradients} = shapesToVector(shapes);
  warnVectorCaps(
    ops.length,
    paints.length,
    NativeUI.maxVectorOps,
    NativeUI.maxVectorPaints,
    gradients.length,
    NativeUI.maxVectorGrads,
  );
  NativeUI.setVectorOps(
    handle,
    ops,
    paints,
    gradients.length ? encodeVectorGradients(gradients) : undefined,
    dirtyRect,
  );
}

/**
 * Imperatively sets a <Text> node's text content WITHOUT disturbing its style (uses a single inherited
 * span, so color/size/etc. from the node's props are preserved). A later React render reverts cleanly.
 * `handle` is the node handle from a ref on a <Text/>.
 */
export function updateText(handle: Handle, text: string | number): void {
  if (handle == null) return;
  NativeUI.setTextSpans(handle, [{text: String(text)}]);
}

/**
 * Scrolls a <ScrollView> and returns where it landed. The offset is clamped to the scroll range, and an
 * axis left out stays where it is, so scrollTo(handle) only reads the position.
 * `handle` is the node handle from a ref on a <ScrollView/>, and `offset` is in pixels. Returns the offset
 * and the largest offset on each axis, or undefined when the handle is not a ScrollView.
 */
export function scrollTo(
  handle: Handle,
  offset?: {x?: number; y?: number},
): ScrollPosition | undefined {
  if (handle == null) return undefined;
  return NativeUI.scrollTo(handle, offset?.x, offset?.y);
}

/**
 * Customizes the on-screen software keyboard's appearance and/or layout. Only effective when the engine was
 * built with the keyboard enabled (ERUI_ONSCREEN_KEYBOARD=1); a no-op otherwise. The config is a plain
 * object of colors/sizes plus an optional `layers` array; omit `layers` to keep the built-in QWERTY:
 *
 *   setKeyboardConfig({ keyColor: '#fff', labelColor: '#111', fontSize: 20,
 *     layers: [ [ [ { char: 'a' }, ... ],                                    // a row
 *                 [ { label: 'shift', layer: 1, highlight: true }, ... ] ],  // a row with a layer-switch key
 *               ... ] });                                                    // more layers
 *
 * Key shapes: { char } types it; { char: ' ', span } a space bar; { label, layer } switches layer;
 * { label, backspace } / { label, done }; optional `span` (grid columns) and `highlight` (lit on its layer).
 * Pass nothing/null to restore the default.
 */
export function setKeyboardConfig(config?: KeyboardConfig | null): void {
  NativeUI.setKeyboardConfig(config || null);
}
