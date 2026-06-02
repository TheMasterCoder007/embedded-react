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

/** Flattens a style (object/array) into a fresh plain object. */
export function flattenStyleObj(style) {
  const out = {};
  flattenStyle(style, out);
  return out;
}

// Style fields the engine honors per inline text span (everything else inherits from the node).
const SPAN_STYLE_KEYS = ['color', 'fontSize', 'fontWeight', 'fontStyle', 'textDecorationLine', 'letterSpacing'];

/**
 * True when a <Text>'s children can be flattened inline — i.e. every leaf is a string/number or a
 * nested <Text> element. A non-Text element (View, a component) makes it false, so the reconciler
 * falls back to mounting children as separate instances.
 */
export function isTextContent(children) {
  if (children == null || children === false || children === true) return true;
  if (typeof children === 'string' || typeof children === 'number') return true;
  if (Array.isArray(children)) return children.every(isTextContent);
  // A React element: only nested <Text> participates in inline flattening (type is the 'Text' tag).
  if (children && children.props !== undefined) return children.type === 'Text';
  return false;
}

/**
 * Walks a <Text>'s children into an ordered list of { text, style } segments. Primitive siblings
 * share the parent's (base) style object by reference; a nested <Text> with its own style produces
 * a new merged style object. Adjacent segments with the same style reference are coalesced — so
 * plain interpolation like `Hi {name}` collapses to a single segment (no spans needed).
 */
export function flattenTextChildren(children, baseStyle) {
  const segments = [];
  const push = (text, style) => {
    if (text === '') return;
    const last = segments[segments.length - 1];
    if (last && last.style === style) last.text += text;
    else segments.push({ text, style });
  };
  const walk = (node, style) => {
    if (node == null || node === false || node === true) return;
    if (typeof node === 'string' || typeof node === 'number') {
      push(String(node), style);
      return;
    }
    if (Array.isArray(node)) {
      for (const c of node) walk(c, style);
      return;
    }
    if (node && node.props !== undefined) {
      // Nested <Text>: merge its style over the inherited one (new object only when it has style,
      // so an unstyled nested <Text> keeps the same reference and still coalesces with siblings).
      const merged = node.props.style ? Object.assign({}, style, flattenStyleObj(node.props.style)) : style;
      walk(node.props.children, merged);
    }
  };
  walk(children, baseStyle);
  return segments;
}

/**
 * Builds the inline-span array for a <Text> (for NativeUI.setTextSpans). Returns [] when the content
 * is uniformly the base style (plain text — no spans needed); otherwise one span per styled segment,
 * each carrying only the span-relevant style keys (the engine inherits the rest from the node).
 */
export function buildTextSpans(props) {
  if (!isTextContent(props.children)) return [];
  const baseStyle = flattenStyleObj(props.style);
  const segments = flattenTextChildren(props.children, baseStyle);
  if (!segments.some((s) => s.style !== baseStyle)) return [];
  return segments.map((s) => {
    const span = { text: s.text };
    for (const k of SPAN_STYLE_KEYS) {
      if (s.style && s.style[k] !== undefined) span[k] = s.style[k];
    }
    return span;
  });
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
  // <Text> content: flatten string/number/nested-<Text> children into the node's full-text string.
  // The engine renders this when no spans are set; with spans (buildTextSpans) it carries the same
  // text for the plain-text fallback. Non-flattenable children fall back to mounted child instances.
  if (type === 'Text' && isTextContent(props.children)) {
    const base = flattenStyleObj(props.style);
    flat.text = flattenTextChildren(props.children, base)
      .map((s) => s.text)
      .join('');
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
