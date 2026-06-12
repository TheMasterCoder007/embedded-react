// style-map — lowers RN style keys to ERProps C-struct field assignments for the Flow B AOT compiler.
//
// This is the build-time mirror of what native_ui_bridge.c does at runtime in Flow A: it turns a
// flattened style object into ERProps field writes. The AOT compiler emits those writes as C source
// (e.g. `p.background_color = 0xFF0F172Au;`) so the generated program builds byte-identical prop bags
// to Flow A — same defaults (er_props_default), same field values → pixel-identical rendering.
//
// Scope: the minimal subset for the first vertical slice (the scaffold counter). Unsupported keys
// throw with a clear message so the compiler fails loudly instead of silently dropping a style — the
// supported set grows demo by demo.

/** A few CSS/RN named colors (extend as demos need them); everything else must be hex. */
const NAMED_COLORS = {
  transparent: 0x00000000,
  white: 0xffffffff,
  black: 0xff000000,
  red: 0xffff0000,
  green: 0xff00ff00,
  blue: 0xff0000ff,
};

/**
 * Parses a color string to a packed ARGB8888 unsigned int (matching ERProps color fields).
 *
 * @param {string} s  '#rgb' | '#rrggbb' | '#rrggbbaa' (RN order) | a named color.
 * @returns {number} ARGB8888 as an unsigned 32-bit integer.
 */
export function parseColorValue(s) {
  if (typeof s !== 'string') throw new Error(`color must be a string, got ${typeof s}`);
  const key = s.trim().toLowerCase();
  if (key in NAMED_COLORS) return NAMED_COLORS[key] >>> 0;
  const m = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(key);
  if (!m) throw new Error(`unsupported color "${s}" (use #rgb / #rrggbb / #rrggbbaa or a named color)`);
  let hex = m[1];
  if (hex.length === 3) hex = hex.split('').map((c) => c + c).join(''); // #rgb → #rrggbb
  let r, g, b, a;
  if (hex.length === 6) {
    a = 0xff;
    r = parseInt(hex.slice(0, 2), 16);
    g = parseInt(hex.slice(2, 4), 16);
    b = parseInt(hex.slice(4, 6), 16);
  } else {
    // RN 8-digit is #rrggbbaa (alpha last).
    r = parseInt(hex.slice(0, 2), 16);
    g = parseInt(hex.slice(2, 4), 16);
    b = parseInt(hex.slice(4, 6), 16);
    a = parseInt(hex.slice(6, 8), 16);
  }
  return (((a << 24) | (r << 16) | (g << 8) | b) >>> 0);
}

/** Formats an ARGB int as a C unsigned hex literal, e.g. 0xFF0F172Au. */
export function colorLiteral(s) {
  return `0x${parseColorValue(s).toString(16).toUpperCase().padStart(8, '0')}u`;
}

const ALIGN = {
  auto: 'ER_ALIGN_AUTO',
  'flex-start': 'ER_ALIGN_FLEX_START',
  center: 'ER_ALIGN_CENTER',
  'flex-end': 'ER_ALIGN_FLEX_END',
  stretch: 'ER_ALIGN_STRETCH',
};
const JUSTIFY = {
  'flex-start': 'ER_JUSTIFY_FLEX_START',
  center: 'ER_JUSTIFY_CENTER',
  'flex-end': 'ER_JUSTIFY_FLEX_END',
  'space-between': 'ER_JUSTIFY_SPACE_BETWEEN',
  'space-around': 'ER_JUSTIFY_SPACE_AROUND',
  'space-evenly': 'ER_JUSTIFY_SPACE_EVENLY',
};
const FLEX_DIRECTION = {
  column: 'ER_FLEX_COL',
  row: 'ER_FLEX_ROW',
  'row-reverse': 'ER_FLEX_ROW_REVERSE',
  'column-reverse': 'ER_FLEX_COL_REVERSE',
};
const POSITION = { relative: 'ER_POS_RELATIVE', absolute: 'ER_POS_ABSOLUTE' };

const enumKey = (table, name) => (v) => {
  const c = table[v];
  if (!c) throw new Error(`${name}: unsupported value "${v}" (one of ${Object.keys(table).join(', ')})`);
  return c;
};
const dim = (v) => {
  if (typeof v !== 'number' || !Number.isFinite(v)) throw new Error(`expected a numeric dimension, got ${JSON.stringify(v)}`);
  return String(Math.round(v));
};

/**
 * A dimension that may be a percentage. `'50%'` → the engine's float `*_pct` field (% of the parent);
 * a number → the pixel field. Used for width / height / flexBasis, which the engine resolves at layout.
 *
 * @param {string} pxField   ERProps pixel field (e.g. 'width').
 * @param {string} pctField  ERProps percentage field (e.g. 'width_pct').
 */
const pctOrPx = (pxField, pctField) => (v) => {
  if (typeof v === 'string' && v.trim().endsWith('%')) {
    const n = parseFloat(v);
    if (!Number.isFinite(n)) throw new Error(`expected a percentage like '50%', got ${JSON.stringify(v)}`);
    // A valid C float literal needs a decimal point — `50f` is a syntax error, `50.0f` is not.
    const lit = Number.isInteger(n) ? `${n}.0f` : `${n}f`;
    return [{ field: pctField, expr: lit }];
  }
  return [{ field: pxField, expr: dim(v) }];
};

/**
 * Per-style-key lowering. Each entry maps a style value to one or more { field, expr } ERProps writes
 * (field = ERProps C member, expr = C source for the value). `flex` expands to several fields.
 */
const KEYS = {
  // Layout
  width: pctOrPx('width', 'width_pct'),
  height: pctOrPx('height', 'height_pct'),
  flexBasis: pctOrPx('flex_basis', 'flex_basis_pct'),
  minWidth: (v) => [{ field: 'min_width', expr: dim(v) }],
  maxWidth: (v) => [{ field: 'max_width', expr: dim(v) }],
  minHeight: (v) => [{ field: 'min_height', expr: dim(v) }],
  maxHeight: (v) => [{ field: 'max_height', expr: dim(v) }],
  padding: (v) => [{ field: 'padding', expr: dim(v) }],
  paddingHorizontal: (v) => [{ field: 'padding_horizontal', expr: dim(v) }],
  paddingVertical: (v) => [{ field: 'padding_vertical', expr: dim(v) }],
  paddingLeft: (v) => [{ field: 'padding_left', expr: dim(v) }],
  paddingTop: (v) => [{ field: 'padding_top', expr: dim(v) }],
  paddingRight: (v) => [{ field: 'padding_right', expr: dim(v) }],
  paddingBottom: (v) => [{ field: 'padding_bottom', expr: dim(v) }],
  margin: (v) => [{ field: 'margin', expr: dim(v) }],
  marginHorizontal: (v) => [{ field: 'margin_horizontal', expr: dim(v) }],
  marginVertical: (v) => [{ field: 'margin_vertical', expr: dim(v) }],
  marginLeft: (v) => [{ field: 'margin_left', expr: dim(v) }],
  marginTop: (v) => [{ field: 'margin_top', expr: dim(v) }],
  marginRight: (v) => [{ field: 'margin_right', expr: dim(v) }],
  marginBottom: (v) => [{ field: 'margin_bottom', expr: dim(v) }],
  gap: (v) => [{ field: 'gap', expr: dim(v) }],
  rowGap: (v) => [{ field: 'row_gap', expr: dim(v) }],
  columnGap: (v) => [{ field: 'column_gap', expr: dim(v) }],
  flexGrow: (v) => [{ field: 'flex_grow', expr: dim(v) }],
  flexShrink: (v) => [{ field: 'flex_shrink', expr: dim(v) }],
  flexDirection: (v) => [{ field: 'flex_direction', expr: enumKey(FLEX_DIRECTION, 'flexDirection')(v) }],
  alignItems: (v) => [{ field: 'align_items', expr: enumKey(ALIGN, 'alignItems')(v) }],
  alignSelf: (v) => [{ field: 'align_self', expr: enumKey(ALIGN, 'alignSelf')(v) }],
  justifyContent: (v) => [{ field: 'justify_content', expr: enumKey(JUSTIFY, 'justifyContent')(v) }],
  // Positioning: `position: 'absolute'` takes a node out of flow; left/top/right/bottom are its anchors.
  position: (v) => [{ field: 'position', expr: enumKey(POSITION, 'position')(v) }],
  left: (v) => [{ field: 'left', expr: dim(v) }],
  top: (v) => [{ field: 'top', expr: dim(v) }],
  right: (v) => [{ field: 'right', expr: dim(v) }],
  bottom: (v) => [{ field: 'bottom', expr: dim(v) }],
  // `flex: n` → grow=n, shrink=1, basis=0 (RN semantics; matches native_ui_bridge apply_flex).
  flex: (v) => {
    const n = Number(v);
    if (n > 0) return [{ field: 'flex_grow', expr: dim(n) }, { field: 'flex_shrink', expr: '1' }, { field: 'flex_basis', expr: '0' }];
    if (n === 0) return [{ field: 'flex_grow', expr: '0' }, { field: 'flex_shrink', expr: '0' }];
    return [{ field: 'flex_grow', expr: '0' }, { field: 'flex_shrink', expr: '1' }];
  },

  // View visual
  backgroundColor: (v) => [{ field: 'background_color', expr: colorLiteral(v) }],
  borderRadius: (v) => [{ field: 'border_radius', expr: dim(v) }],
  borderWidth: (v) => [{ field: 'border_width', expr: dim(v) }],
  borderColor: (v) => [{ field: 'border_color', expr: colorLiteral(v) }],
  opacity: (v) => [{ field: 'opacity', expr: String(Math.round(Math.max(0, Math.min(1, Number(v))) * 255)) }],
  zIndex: (v) => [{ field: 'z_index', expr: dim(v) }],

  // Text
  color: (v) => [{ field: 'color', expr: colorLiteral(v) }],
  fontSize: (v) => [{ field: 'font_size', expr: dim(v) }],
  fontWeight: (v) => [{ field: 'font_weight', expr: v === 'bold' || Number(v) >= 600 ? '1' : '0' }],
  lineHeight: (v) => [{ field: 'line_height', expr: dim(v) }],
  letterSpacing: (v) => [{ field: 'letter_spacing', expr: dim(v) }],
};

/**
 * Lowers one flattened style object to a list of ERProps field assignments.
 *
 * @param {object} style  Flattened style (plain key→value; values already statically resolved).
 * @returns {Array<{field:string, expr:string}>} ERProps writes in declaration order.
 */
export function lowerStyle(style) {
  const out = [];
  for (const [key, value] of Object.entries(style)) {
    if (value === undefined || value === null) continue;
    const fn = KEYS[key];
    if (!fn) throw new Error(`AOT: unsupported style key "${key}" (not yet lowered to ERProps)`);
    out.push(...fn(value));
  }
  return out;
}

/** Maps a JSX component tag to its ERNodeType enum (the subset the slice supports). */
export const NODE_TYPES = {
  View: 'ER_NODE_VIEW',
  Text: 'ER_NODE_TEXT',
  Pressable: 'ER_NODE_PRESSABLE',
  Image: 'ER_NODE_IMAGE',
  ScrollView: 'ER_NODE_SCROLL_VIEW',
};

// Style keys whose value can be DYNAMIC (driven by state). Each maps to its ERProps field + a kind the
// AOT compiler uses to lower a runtime expression: 'num' → assign a C numeric expression directly;
// 'opacity' → scale a 0–1 expression to 0–255; 'color' → a (ternary of) color literal(s). Keys absent
// here (enums like flexDirection, the `flex` shorthand) can only be static for now.
const NUM_FIELDS = {
  width: 'width', height: 'height', minWidth: 'min_width', maxWidth: 'max_width', minHeight: 'min_height', maxHeight: 'max_height',
  padding: 'padding', paddingHorizontal: 'padding_horizontal', paddingVertical: 'padding_vertical',
  paddingLeft: 'padding_left', paddingTop: 'padding_top', paddingRight: 'padding_right', paddingBottom: 'padding_bottom',
  margin: 'margin', marginHorizontal: 'margin_horizontal', marginVertical: 'margin_vertical',
  marginLeft: 'margin_left', marginTop: 'margin_top', marginRight: 'margin_right', marginBottom: 'margin_bottom',
  gap: 'gap', rowGap: 'row_gap', columnGap: 'column_gap', flexGrow: 'flex_grow', flexShrink: 'flex_shrink',
  borderRadius: 'border_radius', borderWidth: 'border_width', zIndex: 'z_index',
  fontSize: 'font_size', lineHeight: 'line_height', letterSpacing: 'letter_spacing',
};

export const DYN_FIELDS = {
  ...Object.fromEntries(Object.entries(NUM_FIELDS).map(([k, f]) => [k, { field: f, kind: 'num' }])),
  backgroundColor: { field: 'background_color', kind: 'color' },
  color: { field: 'color', kind: 'color' },
  borderColor: { field: 'border_color', kind: 'color' },
  opacity: { field: 'opacity', kind: 'opacity' },
};
