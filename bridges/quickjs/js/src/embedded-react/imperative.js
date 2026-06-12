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
 * @param {number} handle      The node handle from a ref on an <Svg/>.
 * @param {Array}  shapes      Flat array of shape descriptors ({arc}/{circle}/{line}/{rect}/{path} + paint).
 * @param {Array}  [dirtyRect] Optional node-local [x, y, w, h] bounding the change; when given, only that
 *                             region is repainted and flushed (a big win for small updates on a large
 *                             vector node). Omit to repaint the whole node box.
 */
export function updateVector(handle, shapes, dirtyRect) {
  if (handle == null) return;
  const { ops, paints } = shapesToVector(shapes);
  NativeUI.setVectorOps(handle, ops, paints, dirtyRect);
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

/**
 * Customises the on-screen software keyboard's appearance and/or layout. Only effective when the engine was
 * built with the keyboard enabled (ERUI_ONSCREEN_KEYBOARD=1); a no-op otherwise. The config is a plain
 * object of colours/sizes plus an optional `layers` array; omit `layers` to keep the built-in QWERTY:
 *
 *   setKeyboardConfig({ keyColor: '#fff', labelColor: '#111', fontSize: 20,
 *     layers: [ [ [ { char: 'a' }, ... ],                                    // a row
 *                 [ { label: 'shift', layer: 1, highlight: true }, ... ] ],  // a row with a layer-switch key
 *               ... ] });                                                    // more layers
 *
 * Key shapes: { char } types it; { char: ' ', span } a space bar; { label, layer } switches layer;
 * { label, backspace } / { label, done }; optional `span` (grid columns) and `highlight` (lit on its layer).
 * Pass nothing/null to restore the default.
 * @param {object} [config]  Keyboard config, or null for the built-in default.
 */
export function setKeyboardConfig(config) {
  NativeUI.setKeyboardConfig(config || null);
}
