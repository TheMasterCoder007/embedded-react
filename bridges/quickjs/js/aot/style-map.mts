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

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import type {StaticAssign} from './compiler/types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** A table from an RN enum value (e.g. `'flex-start'`) to its ER_* C constant. */
type EnumTable = Record<string, string>;

/** Lowers one style value to the ERProps writes it stands for. */
type StyleLowering = (value: unknown) => StaticAssign[];

/**
 * A style key whose value can be state-driven, and how app_update() lowers its runtime value: `num` assigns a
 * C number, `opacity` scales 0–1 to 0–255, `color` takes a (ternary of) color literal(s), and `enum` maps a
 * (ternary of) string literal(s) through `table`.
 */
export type DynamicStyleField =
  | {field: string; kind: 'num' | 'opacity' | 'color'}
  | {field: string; kind: 'enum'; table: EnumTable};

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** A few CSS/RN named colors (extend as demos need them); everything else must be hex. */
const NAMED_COLORS: Record<string, number> = {
  transparent: 0x00000000,
  white: 0xffffffff,
  black: 0xff000000,
  red: 0xffff0000,
  green: 0xff00ff00,
  blue: 0xff0000ff,
};

// RN enum values → ER_* constants, shared by the static (KEYS) and state-driven (ENUM_FIELDS) lowerings.
const ALIGN: EnumTable = {
  auto: 'ER_ALIGN_AUTO',
  'flex-start': 'ER_ALIGN_FLEX_START',
  center: 'ER_ALIGN_CENTER',
  'flex-end': 'ER_ALIGN_FLEX_END',
  stretch: 'ER_ALIGN_STRETCH',
};
const JUSTIFY: EnumTable = {
  'flex-start': 'ER_JUSTIFY_FLEX_START',
  center: 'ER_JUSTIFY_CENTER',
  'flex-end': 'ER_JUSTIFY_FLEX_END',
  'space-between': 'ER_JUSTIFY_SPACE_BETWEEN',
  'space-around': 'ER_JUSTIFY_SPACE_AROUND',
  'space-evenly': 'ER_JUSTIFY_SPACE_EVENLY',
};
const FLEX_DIRECTION: EnumTable = {
  column: 'ER_FLEX_COL',
  row: 'ER_FLEX_ROW',
  'row-reverse': 'ER_FLEX_ROW_REVERSE',
  'column-reverse': 'ER_FLEX_COL_REVERSE',
};
const POSITION: EnumTable = {
  relative: 'ER_POS_RELATIVE',
  absolute: 'ER_POS_ABSOLUTE',
};
const DISPLAY: EnumTable = {flex: 'ER_DISPLAY_FLEX', none: 'ER_DISPLAY_NONE'};

/** Maps a JSX host tag to its ERNodeType enum, for the components the generic node emitter handles. */
export const NODE_TYPES: Record<string, string> = {
  View: 'ER_NODE_VIEW',
  Text: 'ER_NODE_TEXT',
  Pressable: 'ER_NODE_PRESSABLE',
  TouchableOpacity: 'ER_NODE_PRESSABLE',
  Image: 'ER_NODE_IMAGE',
  ScrollView: 'ER_NODE_SCROLL_VIEW',
};

/** Numeric style keys that can be state-driven and the ERProps field each one writes. */
const NUM_FIELDS: Record<string, string> = {
  width: 'width',
  height: 'height',
  minWidth: 'min_width',
  maxWidth: 'max_width',
  minHeight: 'min_height',
  maxHeight: 'max_height',
  padding: 'padding',
  paddingHorizontal: 'padding_horizontal',
  paddingVertical: 'padding_vertical',
  paddingLeft: 'padding_left',
  paddingTop: 'padding_top',
  paddingRight: 'padding_right',
  paddingBottom: 'padding_bottom',
  margin: 'margin',
  marginHorizontal: 'margin_horizontal',
  marginVertical: 'margin_vertical',
  marginLeft: 'margin_left',
  marginTop: 'margin_top',
  marginRight: 'margin_right',
  marginBottom: 'margin_bottom',
  gap: 'gap',
  rowGap: 'row_gap',
  columnGap: 'column_gap',
  flexGrow: 'flex_grow',
  flexShrink: 'flex_shrink',
  borderRadius: 'border_radius',
  borderTopLeftRadius: 'border_top_left_radius',
  borderTopRightRadius: 'border_top_right_radius',
  borderBottomRightRadius: 'border_bottom_right_radius',
  borderBottomLeftRadius: 'border_bottom_left_radius',
  borderWidth: 'border_width',
  zIndex: 'z_index',
  fontSize: 'font_size',
  lineHeight: 'line_height',
  letterSpacing: 'letter_spacing',
};

/**
 * Enum style keys that can be state-driven: the value (a string literal or a ternary of them) lowers to the
 * matching ER_* constant via its table. Changing one in app_update re-runs layout (props_hash covers it).
 */
const ENUM_FIELDS: Record<string, {field: string; table: EnumTable}> = {
  flexDirection: {field: 'flex_direction', table: FLEX_DIRECTION},
  alignItems: {field: 'align_items', table: ALIGN},
  alignSelf: {field: 'align_self', table: ALIGN},
  justifyContent: {field: 'justify_content', table: JUSTIFY},
  position: {field: 'position', table: POSITION},
  display: {field: 'display', table: DISPLAY},
};

/**
 * Every style key whose value can be state-driven, with its ERProps field and how app_update() lowers it (see
 * DynamicStyleField). Keys absent here (such as the `flex` shorthand, `flexBasis`, the four insets and
 * `fontWeight`) can only be static.
 */
export const DYN_FIELDS: Record<string, DynamicStyleField> = {
  ...Object.fromEntries(
    Object.entries(NUM_FIELDS).map(([styleKey, field]) => [
      styleKey,
      {field, kind: 'num' as const},
    ]),
  ),
  ...Object.fromEntries(
    Object.entries(ENUM_FIELDS).map(([styleKey, enumField]) => [
      styleKey,
      {field: enumField.field, kind: 'enum' as const, table: enumField.table},
    ]),
  ),
  backgroundColor: {field: 'background_color', kind: 'color'},
  color: {field: 'color', kind: 'color'},
  borderColor: {field: 'border_color', kind: 'color'},
  opacity: {field: 'opacity', kind: 'opacity'},
};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Parses a color string to a packed ARGB8888 unsigned int (matching ERProps color fields).
 *
 * @param color  '#rgb' | '#rrggbb' | '#rrggbbaa' (RN order) | a named color.
 *
 * @returns ARGB8888 as an unsigned 32-bit integer.
 */
export function parseColorValue(color: unknown): number {
  if (typeof color !== 'string') {
    throw new Error(`color must be a string, got ${typeof color}`);
  }

  // Accept a named color, else a hex string, expanding the 3-digit form to 6.
  const normalized = color.trim().toLowerCase();
  if (normalized in NAMED_COLORS) return NAMED_COLORS[normalized] >>> 0;
  const hexMatch = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(normalized);
  if (!hexMatch) {
    throw new Error(
      `unsupported color "${color}" (use #rgb / #rrggbb / #rrggbbaa or a named color)`,
    );
  }

  // #rgb → #rrggbb
  let hex = hexMatch[1];
  if (hex.length === 3) {
    hex = hex
      .split('')
      .map(digit => digit + digit)
      .join('');
  }

  // Split out the channels; a 6-digit color is opaque.
  let red: number, green: number, blue: number, alpha: number;
  if (hex.length === 6) {
    alpha = 0xff;
    red = parseInt(hex.slice(0, 2), 16);
    green = parseInt(hex.slice(2, 4), 16);
    blue = parseInt(hex.slice(4, 6), 16);
  } else {
    // RN 8-digit is #rrggbbaa (alpha last).
    red = parseInt(hex.slice(0, 2), 16);
    green = parseInt(hex.slice(2, 4), 16);
    blue = parseInt(hex.slice(4, 6), 16);
    alpha = parseInt(hex.slice(6, 8), 16);
  }

  return ((alpha << 24) | (red << 16) | (green << 8) | blue) >>> 0;
}

/**
 * Formats an ARGB int as a C unsigned hex literal, e.g. 0xFF0F172Au.
 *
 * @param color  A color string (see parseColorValue).
 *
 * @returns Its C literal.
 */
export function colorLiteral(color: unknown): string {
  return `0x${parseColorValue(color).toString(16).toUpperCase().padStart(8, '0')}u`;
}

/**
 * Looks an enum style value up in its table.
 *
 * @param table  The RN value → ER_* constant table.
 * @param name  The style key, for the error.
 *
 * @returns A lookup that throws on a value the table does not have.
 */
const enumKey =
  (table: EnumTable, name: string) =>
  (value: unknown): string => {
    const constant = table[value as string];
    if (!constant) {
      throw new Error(
        `${name}: unsupported value "${value}" (one of ${Object.keys(table).join(', ')})`,
      );
    }

    return constant;
  };

/**
 * A pixel dimension as C.
 *
 * @param value  The style value; must be a finite number.
 *
 * @returns It rounded to a whole pixel.
 */
const dimension = (value: unknown): string => {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new Error(
      `expected a numeric dimension, got ${JSON.stringify(value)}`,
    );
  }

  return String(Math.round(value));
};

/**
 * A dimension that may be a percentage. `'50%'` → the engine's float `*_pct` field (% of the parent);
 * a number → the pixel field. Used for width / height / flexBasis and the four insets, all of which the
 * engine resolves at layout.
 *
 * @param pxField  ERProps pixel field (e.g. 'width').
 * @param pctField  ERProps percentage field (e.g. 'width_pct').
 *
 * @returns The lowering for that key.
 */
const pctOrPx =
  (pxField: string, pctField: string): StyleLowering =>
  value => {
    if (typeof value === 'string' && value.trim().endsWith('%')) {
      const percent = parseFloat(value);
      if (!Number.isFinite(percent)) {
        throw new Error(
          `expected a percentage like '50%', got ${JSON.stringify(value)}`,
        );
      }

      // The engine reads a 0.0 percentage as "not set", so lower 0% to the pixel field, where 0 means the same.
      if (percent === 0) return [{field: pxField, expr: '0'}];

      // A valid C float literal needs a decimal point — `50f` is a syntax error, `50.0f` is not.
      const floatLiteral = Number.isInteger(percent)
        ? `${percent}.0f`
        : `${percent}f`;

      return [{field: pctField, expr: floatLiteral}];
    }

    return [{field: pxField, expr: dimension(value)}];
  };

/**
 * Per-style-key lowering. Each entry maps a style value to one or more { field, expr } ERProps writes
 * (field = ERProps C member, expr = C source for the value). `flex` expands to several fields.
 */
const KEYS: Record<string, StyleLowering> = {
  // Layout
  width: pctOrPx('width', 'width_pct'),
  height: pctOrPx('height', 'height_pct'),
  flexBasis: pctOrPx('flex_basis', 'flex_basis_pct'),
  minWidth: width => [{field: 'min_width', expr: dimension(width)}],
  maxWidth: width => [{field: 'max_width', expr: dimension(width)}],
  minHeight: height => [{field: 'min_height', expr: dimension(height)}],
  maxHeight: height => [{field: 'max_height', expr: dimension(height)}],
  padding: padding => [{field: 'padding', expr: dimension(padding)}],
  paddingHorizontal: padding => [
    {field: 'padding_horizontal', expr: dimension(padding)},
  ],
  paddingVertical: padding => [
    {field: 'padding_vertical', expr: dimension(padding)},
  ],
  paddingLeft: padding => [{field: 'padding_left', expr: dimension(padding)}],
  paddingTop: padding => [{field: 'padding_top', expr: dimension(padding)}],
  paddingRight: padding => [{field: 'padding_right', expr: dimension(padding)}],
  paddingBottom: padding => [
    {field: 'padding_bottom', expr: dimension(padding)},
  ],
  margin: margin => [{field: 'margin', expr: dimension(margin)}],
  marginHorizontal: margin => [
    {field: 'margin_horizontal', expr: dimension(margin)},
  ],
  marginVertical: margin => [
    {field: 'margin_vertical', expr: dimension(margin)},
  ],
  marginLeft: margin => [{field: 'margin_left', expr: dimension(margin)}],
  marginTop: margin => [{field: 'margin_top', expr: dimension(margin)}],
  marginRight: margin => [{field: 'margin_right', expr: dimension(margin)}],
  marginBottom: margin => [{field: 'margin_bottom', expr: dimension(margin)}],
  gap: gap => [{field: 'gap', expr: dimension(gap)}],
  rowGap: gap => [{field: 'row_gap', expr: dimension(gap)}],
  columnGap: gap => [{field: 'column_gap', expr: dimension(gap)}],
  flexGrow: grow => [{field: 'flex_grow', expr: dimension(grow)}],
  flexShrink: shrink => [{field: 'flex_shrink', expr: dimension(shrink)}],
  flexDirection: direction => [
    {
      field: 'flex_direction',
      expr: enumKey(FLEX_DIRECTION, 'flexDirection')(direction),
    },
  ],
  alignItems: alignment => [
    {field: 'align_items', expr: enumKey(ALIGN, 'alignItems')(alignment)},
  ],
  alignSelf: alignment => [
    {field: 'align_self', expr: enumKey(ALIGN, 'alignSelf')(alignment)},
  ],
  justifyContent: justification => [
    {
      field: 'justify_content',
      expr: enumKey(JUSTIFY, 'justifyContent')(justification),
    },
  ],
  // Positioning: `position: 'absolute'` takes a node out of flow; left/top/right/bottom are its anchors.
  position: position => [
    {field: 'position', expr: enumKey(POSITION, 'position')(position)},
  ],
  display: display => [
    {field: 'display', expr: enumKey(DISPLAY, 'display')(display)},
  ],
  left: pctOrPx('left', 'left_pct'),
  top: pctOrPx('top', 'top_pct'),
  right: pctOrPx('right', 'right_pct'),
  bottom: pctOrPx('bottom', 'bottom_pct'),
  // `flex: n` → grow=n, shrink=1, basis=0 (RN semantics; matches native_ui_bridge apply_flex).
  flex: flexValue => {
    const factor = Number(flexValue);
    if (factor > 0) {
      return [
        {field: 'flex_grow', expr: dimension(factor)},
        {field: 'flex_shrink', expr: '1'},
        {field: 'flex_basis', expr: '0'},
      ];
    }
    if (factor === 0) {
      return [
        {field: 'flex_grow', expr: '0'},
        {field: 'flex_shrink', expr: '0'},
      ];
    }
    return [
      {field: 'flex_grow', expr: '0'},
      {field: 'flex_shrink', expr: '1'},
    ];
  },

  // View visual
  backgroundColor: color => [
    {field: 'background_color', expr: colorLiteral(color)},
  ],
  borderRadius: radius => [{field: 'border_radius', expr: dimension(radius)}],
  // Per-corner radii; the engine reads 0 in one of these as "use borderRadius" (same as Flow A).
  borderTopLeftRadius: radius => [
    {field: 'border_top_left_radius', expr: dimension(radius)},
  ],
  borderTopRightRadius: radius => [
    {field: 'border_top_right_radius', expr: dimension(radius)},
  ],
  borderBottomRightRadius: radius => [
    {field: 'border_bottom_right_radius', expr: dimension(radius)},
  ],
  borderBottomLeftRadius: radius => [
    {field: 'border_bottom_left_radius', expr: dimension(radius)},
  ],
  borderWidth: width => [{field: 'border_width', expr: dimension(width)}],
  borderColor: color => [{field: 'border_color', expr: colorLiteral(color)}],
  opacity: opacity => [
    {
      field: 'opacity',
      expr: String(Math.round(Math.max(0, Math.min(1, Number(opacity))) * 255)),
    },
  ],
  zIndex: zIndex => [{field: 'z_index', expr: dimension(zIndex)}],

  // Text
  color: color => [{field: 'color', expr: colorLiteral(color)}],
  fontSize: size => [{field: 'font_size', expr: dimension(size)}],
  fontWeight: weight => [
    {
      field: 'font_weight',
      expr: weight === 'bold' || Number(weight) >= 600 ? '1' : '0',
    },
  ],
  lineHeight: lineHeight => [
    {field: 'line_height', expr: dimension(lineHeight)},
  ],
  letterSpacing: spacing => [
    {field: 'letter_spacing', expr: dimension(spacing)},
  ],
};

/**
 * Whether the static lowering knows `key` at all (vs. knowing it but rejecting its value).
 *
 * @param key  A style key.
 *
 * @returns Whether lowerStyle has a lowering for it.
 */
export const isStyleKey = (key: string): boolean => Object.hasOwn(KEYS, key);

/** Every style key the AOT lowers statically — listed in the "unsupported style key" diagnostic. */
export const STYLE_KEYS = Object.keys(KEYS);

/**
 * Lowers one flattened style object to a list of ERProps field assignments. This is the build-time mirror
 * of what native_ui_bridge.c does at runtime in Flow A: the generated C (e.g.
 * `p.background_color = 0xFF0F172Au;`) builds the same prop bags as Flow A, from the same defaults
 * (er_props_default), so both render the same pixels. An unsupported key throws, so the compiler fails
 * loudly instead of silently dropping a style.
 *
 * @param style  Flattened style (plain key→value; values already statically resolved).
 *
 * @returns ERProps writes in declaration order.
 */
export function lowerStyle(style: Record<string, unknown>): StaticAssign[] {
  const assigns = [];
  for (const [key, value] of Object.entries(style)) {
    if (value === undefined || value === null) continue;

    const lowering = Object.hasOwn(KEYS, key) ? KEYS[key] : undefined;
    if (!lowering) {
      throw new Error(
        `AOT: unsupported style key "${key}" (not yet lowered to ERProps)`,
      );
    }

    assigns.push(...lowering(value));
  }

  return assigns;
}
