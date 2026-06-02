// Pure prop-marshalling helpers. No engine / NativeUI dependency, so these are unit-testable in
// plain Node (see src/__tests__/props.unit.test.js). The host config wires them to NativeUI.

// Top-level props NativeUI understands directly (not part of `style`). Everything else style-ish is
// expected inside props.style. Event handlers (on*) are routed separately via setEvent.
export const PASSTHROUGH = [
  'numberOfLines',
  'ellipsizeMode',
  'value',
  'placeholder',
  'placeholderTextColor',
  'editable',
  'animating',
  'visible',
  'resizeMode',
  'tintColor',
  'imageName',
];

/**
 * Flattens RN-style `style` (object or nested array) into the `out` object.
 */
export function flattenStyle(style, out) {
  if (!style) return;
  if (Array.isArray(style)) {
    for (const s of style) flattenStyle(s, out);
  } else {
    Object.assign(out, style);
  }
}

/**
 * Builds the flat prop bag NativeUI.setProps expects: resolved style + passthrough props + text.
 */
export function buildProps(type, props) {
  const flat = {};
  flattenStyle(props.style, flat);
  for (const k of PASSTHROUGH) {
    if (props[k] !== undefined) flat[k] = props[k];
  }
  // <Text>string</Text>: a single string/number child becomes the node's text (see
  // shouldSetTextContent in the host config). Multi-child Text (interpolation/spans) is TODO.
  if (type === 'Text') {
    const c = props.children;
    if (typeof c === 'string' || typeof c === 'number') flat.text = String(c);
  }
  return flat;
}

/**
 * True for an `onXxx` prop whose value is a function (an event handler to route via setEvent).
 */
export function isEventProp(key, value) {
  return (
    key.length > 2 &&
    key[0] === 'o' &&
    key[1] === 'n' &&
    key[2] >= 'A' &&
    key[2] <= 'Z' &&
    typeof value === 'function'
  );
}
