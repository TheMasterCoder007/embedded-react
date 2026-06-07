// Imperative escape hatch for high-frequency interactive updates.
//
// Going through React on every pointer move is too slow on some devices — the
// reconcile + flatten dominate. For continuous gestures (e.g., dragging a dial), grab a node handle via
// a ref and push updates directly here: it skips React entirely and, for vectors, skips the d-string
// parser too. Commit back to React state when the gesture ends so the declarative tree re-syncs.
import { NativeUI } from '../native-ui.js';
import { shapesToVector } from './svg-ops.js';

/**
 * Imperatively sets an <Svg> node's geometry from primitive shape descriptors (see shapesToVector).
 * @param {number} handle  The node handle from a ref on an <Svg/>.
 * @param {Array}  shapes  Flat array of shape descriptors ({arc}/{circle}/{line}/{rect}/{path} + paint).
 */
export function updateVector(handle, shapes) {
  if (handle == null) return;
  const { ops, paints } = shapesToVector(shapes);
  NativeUI.setVectorOps(handle, ops, paints);
}

/**
 * Imperatively sets a <Text> node's text content WITHOUT disturbing its style (uses a single inherited
 * span, so color/size/etc. from the node's props are preserved). A later React render reverts cleanly.
 * @param {number} handle  The node handle from a ref on a <Text/>.
 * @param {string} text    New text content.
 */
export function updateText(handle, text) {
  if (handle == null) return;
  NativeUI.setTextSpans(handle, [{ text: String(text) }]);
}
