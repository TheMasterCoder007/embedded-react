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

import {NODE_TYPES} from '../style-map.mts';
import {
  flattenSvg,
  parseColor,
  parsePath,
  KAPPA,
  PAINT_STRIDE,
  GRAD_MAX_STOPS,
  scaleVectorArtifact,
} from '../../src/embedded-react/svg-ops.js';
import {aotError} from './diagnostics.mts';
import {evalStatic, withUndefined} from './static-eval.mts';
import {floatLit, cstr} from './c-syntax.mts';
import {asCond, emitExpr} from './expressions.mts';
import {emitColorExpr, collectStyleAssigns} from './style-text.mts';
import {emitRefBind} from './nodes.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {
  EmitOptions,
  Env,
  Scope,
  StateTable,
  VectorGradient,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An SVG element in the shape flattenSvg takes: its tag and its folded props, children included. */
interface SvgElement {
  type: string;
  props: Record<string, unknown>;
}

/** A state-driven attribute: its lowered C expression (null for a `d`, which cannot be one) and its node. */
interface DynamicAttr {
  dyn: string | null;
  node: t.Expression | t.JSXEmptyExpression;
}

/** A gradient attribute (fillGrad / strokeGrad), kept as its expression for gradAttr to read. */
interface GradientAttr {
  gradNode: t.Expression | t.JSXEmptyExpression;
}

/**
 * An SVG element's attributes, as svgAttrs reads them: a folded value (number, string, `true` for a bare
 * attribute), a DynamicAttr, or a GradientAttr.
 */
type SvgAttrs = Record<string, unknown>;

/** A shape's op-tape entries, and the C locals the caller declares ahead of them (a rounded rect's radii). */
export interface VectorGeometry {
  entries: string[];
  locals: string[];
}

/** A gradient descriptor lowered to C-expression fields; `cond` makes the paint's index state-driven. */
interface GradientSpec {
  type: number;
  stops: {color: string; offset: string}[];
  ax: string;
  ay: string;
  bx: string;
  by: string;
  r: string;
  anyDynamic: boolean;
  cond?: string | null;
}

/** A shape's paint as C-expression fields in PAINT_FIELDS order, and whether any is state-driven. */
interface PaintSpec {
  fields: string[];
  anyDynamic: boolean;
  fillGrad: GradientSpec | null;
  strokeGrad: GradientSpec | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** strokeLinecap → ERVectorPaint cap. */
export const CAP_MAP: Record<string, number> = {butt: 0, round: 1, square: 2};
/** strokeLinejoin → ERVectorPaint join. */
export const JOIN_MAP: Record<string, number> = {miter: 0, round: 1, bevel: 2};

/** The ERVectorPaint members, in paint-record order. */
const PAINT_FIELDS = [
  'fill',
  'stroke',
  'stroke_w',
  'miter',
  'cap',
  'join',
  'fill_rule',
  'fill_grad',
  'stroke_grad',
];

/** A C number (sign, decimals, exponent) as a regex source for reading folded literals back. */
const C_NUMBER = '-?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?';

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Yields an <Svg> subtree's shape children in source order, inlining any `<>…</>` in place: a fragment
 * carries no paint and no transform, so it is transparent to the tape (the Flow A walk in flattenSvg
 * treats it the same way). Whitespace between elements and JSX comments are skipped; anything else
 * that has no shape to contribute throws, rather than vanishing from the generated C.
 *
 * @param children  An <Svg>'s (or fragment's) children.
 *
 * @returns The shape elements, in order.
 */
function* svgShapeChildren(
  children: t.JSXElement['children'],
): Generator<t.JSXElement, void, undefined> {
  for (const child of children) {
    if (child.type === 'JSXElement') {
      yield child;
    } else if (child.type === 'JSXFragment') {
      yield* svgShapeChildren(child.children);
    } else if (child.type === 'JSXText') {
      if (child.value.trim()) {
        throw new Error(
          `AOT: <Svg> cannot draw text — remove ${JSON.stringify(child.value.trim())} from the subtree`,
        );
      }
    } else if (child.type === 'JSXExpressionContainer') {
      if (child.expression.type !== 'JSXEmptyExpression') {
        throw new Error(
          'AOT: dynamic <Svg> children ({…}) not yet supported — use literal shape elements',
        );
      }
    } else {
      throw new Error(`AOT: unsupported <Svg> child (${child.type})`);
    }
  }
}

/**
 * Converts an SVG JSX element (Svg/Circle/Path/Rect/Line/Arc/G/…) to flattenSvg's `{type, props}` shape,
 * statically evaluating every attribute; it serves the static path, so an unfoldable attribute throws.
 *
 * @param element  The element.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The element, its subtree included.
 */
function jsxToSvgElement(
  element: t.JSXElement,
  scope: Scope,
): SvgElement | null {
  if (element.type !== 'JSXElement') return null;

  // Fold every attribute; a bare attribute is `true`.
  const type = (element.openingElement.name as t.JSXIdentifier).name;
  const props: Record<string, unknown> = {};
  for (const attr of element.openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') {
      throw new Error(
        'AOT: spread attributes on an <Svg> element not supported',
      );
    }

    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'ref' || name === 'key') continue; // not geometry/paint

    if (attr.value == null) {
      props[name] = true;
    } else if (attr.value.type === 'StringLiteral') {
      props[name] = attr.value.value;
    } else if (attr.value.type === 'JSXExpressionContainer') {
      props[name] = evalStatic(attr.value.expression, scope);
    } else {
      throw new Error(
        `AOT: unsupported <${type}> attribute value for "${name}"`,
      );
    }
  }

  // Convert the shape children the same way, with fragments inlined.
  const children = [];
  for (const child of svgShapeChildren(element.children)) {
    children.push(jsxToSvgElement(child, scope));
  }

  if (children.length) {
    props.children = children;
  }

  return {type, props};
}

/**
 * Emits one ERVectorPaint initializer from a flattenSvg paint record
 * [fill, stroke, w, miter, cap, join, rule, fill_grad, stroke_grad]. fill_grad/stroke_grad (1-based gradient-table
 * indices, 0 = solid) are absent on inline-<Svg> records (7-wide) → zero, and set on a baked <Svg source>.
 *
 * @param paint  The paint record.
 *
 * @returns The C initializer.
 */
export function emitVectorPaint(paint: number[]): string {
  return (
    `{ .fill = ${paint[0] >>> 0}u, .stroke = ${paint[1] >>> 0}u, .stroke_w = ` +
    `${floatLit(paint[2])}, .miter = ${floatLit(paint[3])}, .cap = ${paint[4] | 0}, .join ` +
    `= ${paint[5] | 0}, .fill_rule = ${paint[6] | 0}, .fill_grad = ` +
    `${(paint[7] || 0) | 0}, .stroke_grad = ${(paint[8] || 0) | 0} }`
  );
}

/**
 * Emits one ERVectorGradient initializer from a baked artifact gradient
 * { type, stops:[{color, offset}], ax, ay, bx, by, r }. Stops fill the C ERGradientStop[] positionally
 * ({color, position}); the engine zero-inits the rest of stops[ER_VGRAD_MAX_STOPS]. Geometry meaning per
 * type: linear axis (ax, ay)->(bx, by); radial center (ax, ay)+radius r; conic center (ax, ay) + start angle r.
 * Stops past the engine's 8 are dropped.
 *
 * @param gradient  The gradient.
 *
 * @returns The C initializer.
 */
function emitVectorGradient(gradient: VectorGradient): string {
  const allStops = gradient.stops || [];
  const keptStops = allStops.length > 8 ? allStops.slice(0, 8) : allStops;
  const stopInits = keptStops
    .map(stop => `{ ${stop.color >>> 0}u, ${floatLit(stop.offset)} }`)
    .join(', ');
  return (
    `{ .type = ${gradient.type | 0}, .stop_count = ${keptStops.length}, .stops = { ${stopInits} }, ` +
    `.ax = ${floatLit(gradient.ax || 0)}, .ay = ${floatLit(gradient.ay || 0)}, .bx = ${floatLit(gradient.bx || 0)}, ` +
    `.by = ${floatLit(gradient.by || 0)}, .r = ${floatLit(gradient.r || 0)} }`
  );
}

/**
 * Static numeric coercion for an SVG attribute value (mirrors svg-ops `num`).
 *
 * @param value  The attribute's value.
 * @param fallback  The value to use when it is not a number.
 *
 * @returns The number.
 */
export const svgNum = (value: unknown, fallback = 0): number => {
  const parsed =
    typeof value === 'number' ? value : parseFloat(value as string);
  return Number.isNaN(parsed) ? fallback : parsed;
};

/**
 * True if an attribute value is a state-driven C expression (vs. a static number/string).
 *
 * @param value  An attribute value from svgAttrs.
 *
 * @returns Whether it is a DynamicAttr.
 */
const isDynamicAttr = (value: unknown): value is DynamicAttr =>
  value != null && typeof value === 'object' && 'dyn' in value;

/**
 * Lowers an SVG coordinate attr to a C float expression (literal when static, cast expr when dynamic).
 *
 * @param attrValue  The attribute's value.
 * @param fallback  The value to use when it is absent or not a number.
 *
 * @returns C for a float.
 */
const attrToCFloat = (attrValue: unknown, fallback = 0): string =>
  isDynamicAttr(attrValue)
    ? `(float)(${attrValue.dyn})`
    : floatLit(svgNum(attrValue, fallback));

/**
 * Reads an SVG element's attributes → { name: number|string|true | {dyn: cExpr} } (state attrs → {dyn}).
 *
 * @param openingElement  The element's opening tag.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 *
 * @returns The attributes, by name.
 */
function svgAttrs(
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  env: Env,
): SvgAttrs {
  const attrs: SvgAttrs = {};
  for (const attr of openingElement.attributes) {
    if (attr.type !== 'JSXAttribute') {
      throw new Error(
        'AOT: spread attributes on an <Svg> element not supported',
      );
    }

    // not geometry/paint
    const name = (attr.name as t.JSXIdentifier).name;
    if (name === 'ref' || name === 'key') continue;

    // Keep a gradient as its expression; gradAttr lowers it, conditions included.
    const valueNode = attr.value;
    if (name === 'fillGrad' || name === 'strokeGrad') {
      if (valueNode == null || valueNode.type !== 'JSXExpressionContainer') {
        throw new Error(`AOT: "${name}" must be an object expression`);
      }

      attrs[name] = {gradNode: valueNode.expression};
      continue;
    }

    // Fold everything else; an expression that does not fold is state-driven.
    if (valueNode == null) {
      attrs[name] = true;
    } else if (valueNode.type === 'StringLiteral') {
      attrs[name] = valueNode.value;
    } else if (valueNode.type === 'JSXExpressionContainer') {
      try {
        attrs[name] = evalStatic(valueNode.expression, withUndefined(scope));
      } catch {
        // Defer a state-driven `d` so pathEntries raises the error naming the fix, not the expression's own error.
        if (name === 'd') {
          attrs[name] = {dyn: null, node: valueNode.expression};
          continue;
        }

        // Keep the node too: fill/stroke lower via emitColorExpr to an ARGB uint, not the numeric `dyn` code.
        attrs[name] = {
          dyn: emitExpr(valueNode.expression, env).code,
          node: valueNode.expression,
        };
      }
    } else {
      throw new Error(`AOT: unsupported SVG attribute value for "${name}"`);
    }
  }

  return attrs;
}

/**
 * Lowers a `{ type, ax, ay, bx, by, r, stops: [{ color, offset }] }` gradient descriptor — the SAME shape
 * Flow A's svg-ops.js takes — to C-expression fields. Every geometry field and every stop may be static
 * or state-driven; `type` and the stop COUNT must be static, since they decide the emitted table's shape.
 *
 * Conic gradients carry the sweep's start angle in `r` (radians, clockwise from the top), so a dial whose
 * ramp must follow a setpoint drives `r` from state — which is exactly why the table can't be const.
 *
 * @param node  The descriptor's object literal.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 * @param attrName  The attribute, for errors.
 *
 * @returns The lowered gradient.
 */
function gradSpec(
  node: t.Node | null | undefined,
  scope: Scope,
  env: Env,
  attrName: string,
): GradientSpec {
  if (!node || node.type !== 'ObjectExpression') {
    throw new Error(`AOT: "${attrName}" must be an object literal`);
  }

  // Index the descriptor's properties by key.
  const props: Record<string, t.Node> = {};
  for (const property of node.properties) {
    if (property.type !== 'ObjectProperty' || property.computed) {
      throw new Error(`AOT: "${attrName}" takes plain key: value pairs only`);
    }

    props[
      (property.key as t.Identifier).name ??
        (property.key as t.StringLiteral).value
    ] = property.value;
  }

  // A numeric field: fold when it can be, otherwise emit the state-driven expression.
  let anyDynamic = false;
  const floatField = (
    valueNode: t.Node | undefined,
    fallback: number,
  ): string => {
    if (valueNode == null) return floatLit(fallback);
    try {
      return floatLit(evalStatic(valueNode, scope));
    } catch {
      anyDynamic = true;
      return `(float)(${emitExpr(valueNode, env).code})`;
    }
  };

  // The type and the stop count decide the table's shape, so both must be static.
  let type: number;
  try {
    type = (evalStatic(props.type, scope) as number) | 0;
  } catch {
    throw new Error(
      `AOT: "${attrName}.type" must be a compile-time constant (1 linear, 2 radial, 3 conic)`,
    );
  }

  if (type < 1 || type > 3) {
    throw new Error(
      `AOT: "${attrName}.type" must be 1 (linear), 2 (radial) or 3 (conic)`,
    );
  }

  const stopsNode = props.stops;
  if (!stopsNode || stopsNode.type !== 'ArrayExpression') {
    throw new Error(`AOT: "${attrName}.stops" must be an array literal`);
  }

  if (
    stopsNode.elements.length < 2 ||
    stopsNode.elements.length > GRAD_MAX_STOPS
  ) {
    throw new Error(
      `AOT: "${attrName}.stops" needs 2..${GRAD_MAX_STOPS} entries (got ${stopsNode.elements.length})`,
    );
  }

  // Lower each stop: a static color folds to a literal, a state-driven one to an ARGB expression.
  const stops = stopsNode.elements.map(stopNode => {
    if (!stopNode || stopNode.type !== 'ObjectExpression') {
      throw new Error(
        `AOT: each "${attrName}.stops" entry must be an object literal`,
      );
    }

    const stopProps: Record<string, t.Node> = {};
    for (const property of stopNode.properties as t.ObjectProperty[]) {
      stopProps[
        (property.key as t.Identifier).name ??
          (property.key as t.StringLiteral).value
      ] = property.value;
    }

    let color: string;
    try {
      color = `${parseColor(evalStatic(stopProps.color, scope)) >>> 0}u`;
    } catch {
      anyDynamic = true;
      color = emitColorExpr(stopProps.color, env);
    }

    return {color, offset: floatField(stopProps.offset, 0)};
  });

  return {
    type,
    stops,
    ax: floatField(props.ax, 0),
    ay: floatField(props.ay, 0),
    bx: floatField(props.bx, 0),
    by: floatField(props.by, 0),
    r: floatField(props.r, 0),
    anyDynamic,
  };
}

/**
 * Unwraps a gradient attribute, which may be a bare object literal or a CONDITIONAL one:
 * `cond ? {…} : null` (either way round) or `cond && {…}`. The gradient table entry is emitted either
 * way; what the condition drives is the PAINT'S INDEX, which becomes a runtime ternary of N or 0.
 *
 * Without this a gradient applied to a shape that is only sometimes gradient-filled leaks into every
 * other state — the index is a compile-time constant, so "no gradient here" is not expressible by
 * omission: a thermostat dial's Auto ramp would paint over its Cool and Heat modes as well.
 *
 * @param gradientAttr  The attribute, if the shape has it.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 * @param attrName  The attribute, for errors.
 *
 * @returns The lowered gradient and its condition, or null when the shape has none.
 */
function gradAttr(
  gradientAttr: GradientAttr | undefined,
  scope: Scope,
  env: Env,
  attrName: string,
): GradientSpec | null {
  if (!gradientAttr) return null;

  // Peel a conditional down to its object literal, keeping the condition as C.
  let node: t.Node = gradientAttr.gradNode;
  let cond = null;
  const nullish = (branch: t.Node) =>
    branch.type === 'NullLiteral' ||
    (branch.type === 'Identifier' && branch.name === 'undefined');
  if (node.type === 'ConditionalExpression') {
    if (
      node.consequent.type === 'ObjectExpression' &&
      nullish(node.alternate)
    ) {
      cond = asCond(emitExpr(node.test, env));
      node = node.consequent;
    } else if (
      node.alternate.type === 'ObjectExpression' &&
      nullish(node.consequent)
    ) {
      cond = `!(${asCond(emitExpr(node.test, env))})`;
      node = node.alternate;
    } else {
      throw new Error(
        `AOT: a conditional "${attrName}" must be \`cond ? { … } : null\` (one branch an ` +
          `object literal, the other null)`,
      );
    }
  } else if (
    node.type === 'LogicalExpression' &&
    node.operator === '&&' &&
    node.right.type === 'ObjectExpression'
  ) {
    cond = asCond(emitExpr(node.left, env));
    node = node.right;
  }

  // Lower the object literal itself.
  const spec = gradSpec(node, scope, env, attrName);
  spec.cond = cond;
  return spec;
}

/**
 * A `{ .type = …, .stops = { … }, … }` ERVectorGradient initializer from a gradSpec (const tables).
 *
 * @param gradient  The lowered gradient.
 *
 * @returns The C initializer.
 */
function gradInitFromSpec(gradient: GradientSpec): string {
  const stopInits = gradient.stops
    .map(stop => `{ ${stop.color}, ${stop.offset} }`)
    .join(', ');
  return (
    `{ .type = ${gradient.type}, .stop_count = ${gradient.stops.length}, .stops = { ${stopInits} }, ` +
    `.ax = ${gradient.ax}, .ay = ${gradient.ay}, .bx = ${gradient.bx}, .by = ${gradient.by}, .r = ${gradient.r} }`
  );
}

/**
 * Per-field assignments rebuilding one mutable gradient-table entry from state, for build_svgN().
 *
 * @param tableName  The table's C name.
 * @param entryIndex  The entry's index.
 * @param gradient  The lowered gradient.
 *
 * @returns The C statements.
 */
function gradAssigns(
  tableName: string,
  entryIndex: number,
  gradient: GradientSpec,
): string[] {
  const lines = [
    `    ${tableName}[${entryIndex}].type = ${gradient.type};`,
    `    ${tableName}[${entryIndex}].stop_count = ${gradient.stops.length};`,
  ];
  gradient.stops.forEach((stop, stopIndex) => {
    lines.push(
      `    ${tableName}[${entryIndex}].stops[${stopIndex}].color = ${stop.color};`,
    );
    lines.push(
      `    ${tableName}[${entryIndex}].stops[${stopIndex}].position = ${stop.offset};`,
    );
  });

  for (const field of ['ax', 'ay', 'bx', 'by', 'r'] as const) {
    lines.push(
      `    ${tableName}[${entryIndex}].${field} = ${gradient[field]};`,
    );
  }

  return lines;
}

/**
 * A shape's paint as C-expr fields (matching PAINT_FIELDS) + whether any is state-driven.
 *   - fill / stroke may be DYNAMIC → lowered via emitColorExpr to an ARGB uint expr (a color string, a
 *     ternary of them, or a folded theme token); static → a baked `0xAARRGGBBu` literal.
 *   - strokeWidth may be DYNAMIC (numeric C expr); static → a float literal.
 *   - cap / join / miterlimit / fillRule must be STATIC (a dynamic one throws a clear error).
 *   - fillGrad / strokeGrad are lowered here; their table indices are filled in by the caller.
 *
 * @param attrs  The shape's attributes.
 * @param env  The expression environment.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The paint, and the shape's gradients.
 */
function paintSpec(attrs: SvgAttrs, env: Env, scope: Scope): PaintSpec {
  // Lower the colors and the stroke width, the only paint attributes that may be state-driven.
  let anyDynamic = false;
  const color = (attrValue: unknown, fallback: string): string => {
    if (isDynamicAttr(attrValue)) {
      anyDynamic = true;
      return emitColorExpr(attrValue.node, env);
    }
    return `${parseColor(attrValue ?? fallback) >>> 0}u`;
  };

  let strokeWidth: string;
  if (isDynamicAttr(attrs.strokeWidth)) {
    anyDynamic = true;
    strokeWidth = `(float)(${attrs.strokeWidth.dyn})`;
  } else {
    strokeWidth = floatLit(svgNum(attrs.strokeWidth, 1));
  }

  // The rest of the paint must be static.
  for (const attrName of [
    'strokeLinecap',
    'strokeLinejoin',
    'strokeMiterlimit',
    'fillRule',
  ]) {
    if (isDynamicAttr(attrs[attrName])) {
      throw new Error(
        `AOT: a state-driven <Svg> "${attrName}" is not supported (only fill / stroke / ` +
          `strokeWidth can be state-driven)`,
      );
    }
  }

  // Assemble the fields in PAINT_FIELDS order.
  const fields = [
    color(attrs.fill, 'black'),
    color(attrs.stroke, 'none'),
    strokeWidth,
    floatLit(svgNum(attrs.strokeMiterlimit, 4)),
    String(CAP_MAP[attrs.strokeLinecap as string] ?? 0),
    String(JOIN_MAP[attrs.strokeLinejoin as string] ?? 0),
    String(attrs.fillRule === 'evenodd' ? 1 : 0),
  ];

  // Placeholder gradient indices keep `fields` aligned with PAINT_FIELDS until the caller lays out the table.
  fields.push('0', '0');

  // Lower the gradients; a conditional one makes the paint index state-driven, so the paint table is mutable.
  const fillGrad = gradAttr(
    attrs.fillGrad as GradientAttr | undefined,
    scope,
    env,
    'fillGrad',
  );
  const strokeGrad = gradAttr(
    attrs.strokeGrad as GradientAttr | undefined,
    scope,
    env,
    'strokeGrad',
  );
  if (
    fillGrad?.anyDynamic ||
    strokeGrad?.anyDynamic ||
    fillGrad?.cond ||
    strokeGrad?.cond
  ) {
    anyDynamic = true;
  }

  return {fields, anyDynamic, fillGrad, strokeGrad};
}

/**
 * A `{ .fill = …, … }` ERVectorPaint initializer from a paintSpec's C-expr fields (used for static paints).
 *
 * @param paint  The paint.
 *
 * @returns The C initializer.
 */
function paintInitFromSpec(paint: PaintSpec): string {
  return `{ ${PAINT_FIELDS.map((field, fieldIndex) => `.${field} = ${paint.fields[fieldIndex]}`).join(', ')} }`;
}

// Per-shape op-tape entries; the ...C helpers take C floats so JSX and updateVector share geometry, as in svg-ops.

/**
 * An arc's op-tape entries. The engine measures angles in radians clockwise from 3 o'clock, so each
 * angle is shifted back a quarter turn and converted.
 *
 * @param cx  C for the center's x.
 * @param cy  C for the center's y.
 * @param radius  C for the radius.
 * @param startDeg  C for the start angle, in degrees clockwise from the top.
 * @param endDeg  C for the end angle, likewise.
 *
 * @returns The entries.
 */
export const arcEntriesC = (
  cx: string,
  cy: string,
  radius: string,
  startDeg: string,
  endDeg: string,
): string[] => {
  const startRad = `((${startDeg} - 90.0f) * (float)M_PI / 180.0f)`;
  const endRad = `((${endDeg} - 90.0f) * (float)M_PI / 180.0f)`;
  return ['ER_VOP_ARC', cx, cy, radius, startRad, endRad, '0.0f'];
};

/**
 * A circle's op-tape entries.
 *
 * @param cx  C for the center's x.
 * @param cy  C for the center's y.
 * @param radius  C for the radius.
 *
 * @returns The entries.
 */
export const circleEntriesC = (
  cx: string,
  cy: string,
  radius: string,
): string[] => [
  'ER_VOP_MOVE',
  `(${cx} + ${radius})`,
  cy,
  'ER_VOP_ARC',
  cx,
  cy,
  radius,
  '0.0f',
  '(2.0f * (float)M_PI)',
  '0.0f',
  'ER_VOP_CLOSE',
];

/**
 * The numeric value of a C float literal (as produced by floatLit/attrToCFloat, or the `(float)(N)` cast
 * the imperative updateVector path emits), or null for a state-driven expr.
 *
 * @param floatExpr  C for a float.
 *
 * @returns Its value, or null when it is not a literal.
 */
const cLiteralValue = (floatExpr: unknown): number | null => {
  const trimmed = String(floatExpr).trim();
  const cast = new RegExp(`^\\(float\\)\\((${C_NUMBER})\\)$`).exec(trimmed);
  if (cast) return parseFloat(cast[1]);
  return new RegExp(`^${C_NUMBER}f$`).test(trimmed)
    ? parseFloat(trimmed)
    : null;
};

/**
 * Normalizes a shape's geometry result. Most shapes are just an op-tape; a rounded <Rect> also returns
 * the `const float` locals the caller must declare ahead of that tape, in the same C block.
 *
 * @param geometry  An op-tape, or a geometry.
 *
 * @returns The geometry.
 */
export const geometryOf = (
  geometry: string[] | VectorGeometry,
): VectorGeometry =>
  Array.isArray(geometry) ? {entries: geometry, locals: []} : geometry;

/**
 * A corner radius clamped to half its side, mirroring svg-ops rectRadii. Folded to a literal when both
 * the radius and the side are static — the usual case, since only x/y/width tend to be state-driven.
 *
 * @param radius  C for the radius.
 * @param side  C for the side it is clamped against.
 *
 * @returns C for the clamped radius.
 */
const clampRadiusC = (radius: string, side: string): string => {
  const radiusValue = cLiteralValue(radius);
  if (radiusValue != null && radiusValue <= 0) return '0.0f';

  const sideValue = cLiteralValue(side);
  if (radiusValue != null && sideValue != null) {
    return floatLit(Math.max(0, Math.min(radiusValue, sideValue / 2)));
  }

  return `fminf(fmaxf(${radius}, 0.0f), (${side}) * 0.5f)`;
};

/**
 * A square-cornered rectangle's op-tape entries.
 *
 * @param x  C for the left edge.
 * @param y  C for the top edge.
 * @param width  C for the width.
 * @param height  C for the height.
 *
 * @returns The entries.
 */
const sharpRectEntriesC = (
  x: string,
  y: string,
  width: string,
  height: string,
): string[] => [
  'ER_VOP_MOVE',
  x,
  y,
  'ER_VOP_LINE',
  `(${x} + ${width})`,
  y,
  'ER_VOP_LINE',
  `(${x} + ${width})`,
  `(${y} + ${height})`,
  'ER_VOP_LINE',
  x,
  `(${y} + ${height})`,
  'ER_VOP_CLOSE',
];

/**
 * A rectangle's op-tape entries. rx/ry are null when the rect has no corner radius at all. Corners are
 * cubics, not ER_VOP_ARC, for the same reason svg-ops uses them: a corner must start at EXACTLY the preceding
 * line's endpoint. `tag` names the locals and must be unique within the caller's C block.
 *
 * @param x  C for the left edge.
 * @param y  C for the top edge.
 * @param width  C for the width.
 * @param height  C for the height.
 * @param rx  C for the horizontal corner radius, or null.
 * @param ry  C for the vertical corner radius, or null.
 * @param tag  Names the C locals a state-driven radius needs.
 *
 * @returns The entries, and the locals to declare ahead of them.
 */
export const rectEntriesC = (
  x: string,
  y: string,
  width: string,
  height: string,
  rx: string | null = null,
  ry: string | null = null,
  tag = '',
): VectorGeometry => {
  // A negative literal radius means `auto` (use the other one, as browsers do); a state-driven radius only clamps.
  const isNegativeLiteral = (radius: string | null) => {
    const value = cLiteralValue(radius);
    return value != null && value < 0;
  };
  if (isNegativeLiteral(rx)) {
    rx = null;
  }
  if (isNegativeLiteral(ry)) {
    ry = null;
  }
  if (rx == null && ry == null) {
    return {entries: sharpRectEntriesC(x, y, width, height), locals: []};
  }

  // A missing radius takes the other's value, each clamps to half its side, and a zero radius squares the corners.
  let radiusX = clampRadiusC((rx ?? ry)!, width);
  let radiusY = clampRadiusC((ry ?? rx)!, height);
  if (cLiteralValue(radiusX) === 0 || cLiteralValue(radiusY) === 0) {
    return {entries: sharpRectEntriesC(x, y, width, height), locals: []};
  }

  // Bind an unfolded clamp to a local rather than repeat it ten times in the tape; a folded radius stays inline.
  const locals = [];
  if (cLiteralValue(radiusX) == null) {
    locals.push(`const float rx_${tag} = ${radiusX};`);
    radiusX = `rx_${tag}`;
  }
  if (cLiteralValue(radiusY) == null) {
    locals.push(`const float ry_${tag} = ${radiusY};`);
    radiusY = `ry_${tag}`;
  }

  // Trace clockwise from the top edge's start: each side a line, each corner a cubic with KAPPA-length handles.
  const kappa = floatLit(KAPPA);
  const handleX = `(${radiusX} * ${kappa})`;
  const handleY = `(${radiusY} * ${kappa})`;
  const innerLeft = `(${x} + ${radiusX})`;
  const innerRight = `(${x} + ${width} - ${radiusX})`;
  const innerTop = `(${y} + ${radiusY})`;
  const innerBottom = `(${y} + ${height} - ${radiusY})`;
  const right = `(${x} + ${width})`;
  const bottom = `(${y} + ${height})`;
  const entries = [
    'ER_VOP_MOVE',
    innerLeft,
    y,
    'ER_VOP_LINE',
    innerRight,
    y,
    'ER_VOP_CUBIC',
    `(${innerRight} + ${handleX})`,
    y,
    right,
    `(${innerTop} - ${handleY})`,
    right,
    innerTop,
    'ER_VOP_LINE',
    right,
    innerBottom,
    'ER_VOP_CUBIC',
    right,
    `(${innerBottom} + ${handleY})`,
    `(${innerRight} + ${handleX})`,
    bottom,
    innerRight,
    bottom,
    'ER_VOP_LINE',
    innerLeft,
    bottom,
    'ER_VOP_CUBIC',
    `(${innerLeft} - ${handleX})`,
    bottom,
    x,
    `(${innerBottom} + ${handleY})`,
    x,
    innerBottom,
    'ER_VOP_LINE',
    x,
    innerTop,
    'ER_VOP_CUBIC',
    x,
    `(${innerTop} - ${handleY})`,
    `(${innerLeft} - ${handleX})`,
    y,
    innerLeft,
    y,
    'ER_VOP_CLOSE',
  ];

  return {entries, locals};
};

/**
 * A line's op-tape entries.
 *
 * @param x1  C for the start's x.
 * @param y1  C for the start's y.
 * @param x2  C for the end's x.
 * @param y2  C for the end's y.
 *
 * @returns The entries.
 */
export const lineEntriesC = (
  x1: string,
  y1: string,
  x2: string,
  y2: string,
): string[] => ['ER_VOP_MOVE', x1, y1, 'ER_VOP_LINE', x2, y2];

// The JSX path: each shape's attributes resolve to C floats via attrToCFloat.
const arcEntries = (attrs: SvgAttrs) =>
  arcEntriesC(
    attrToCFloat(attrs.cx),
    attrToCFloat(attrs.cy),
    attrToCFloat(attrs.r),
    attrToCFloat(attrs.startAngle),
    attrToCFloat(attrs.endAngle),
  );
const circleEntries = (attrs: SvgAttrs) =>
  circleEntriesC(
    attrToCFloat(attrs.cx),
    attrToCFloat(attrs.cy),
    attrToCFloat(attrs.r),
  );
const rectEntries = (attrs: SvgAttrs, tag: string) =>
  rectEntriesC(
    attrToCFloat(attrs.x),
    attrToCFloat(attrs.y),
    attrToCFloat(attrs.width),
    attrToCFloat(attrs.height),
    attrs.rx == null ? null : attrToCFloat(attrs.rx),
    attrs.ry == null ? null : attrToCFloat(attrs.ry),
    tag,
  );
const lineEntries = (attrs: SvgAttrs) =>
  lineEntriesC(
    attrToCFloat(attrs.x1),
    attrToCFloat(attrs.y1),
    attrToCFloat(attrs.x2),
    attrToCFloat(attrs.y2),
  );
const pathEntries = (attrs: SvgAttrs) => {
  if (attrs.d == null) return [];
  if (isDynamicAttr(attrs.d)) {
    throw new Error(
      'AOT: a state-driven <Path d=…> is not yet supported (use Arc/Circle/Rect/Line for dynamic shapes)',
    );
  }
  return parsePath(String(attrs.d)).map(floatLit); // opcodes are encoded as float values 0..6, like coords
};

/** The shapes a state-driven <Svg> can hold, by tag. */
const SHAPE_ENTRIES: Record<
  string,
  (attrs: SvgAttrs, tag: string) => string[] | VectorGeometry
> = {
  Arc: arcEntries,
  Circle: circleEntries,
  Rect: rectEntries,
  Line: lineEntries,
  Path: pathEntries,
};

/**
 * True if any attribute anywhere in the <Svg> subtree references state (→ the state-driven path).
 *
 * @param element  The <Svg>.
 * @param scope  Compile-time constants in reach.
 *
 * @returns Whether an attribute does not fold.
 */
function svgHasDynamic(element: t.JSXElement, scope: Scope): boolean {
  let hasDynamic = false;
  const walk = (node: t.Node): void => {
    // A fragment holds no attributes of its own, but its shapes' attributes still count.
    if (node.type === 'JSXFragment') {
      for (const child of node.children) {
        walk(child);
      }
      return;
    }

    if (node.type !== 'JSXElement') return;

    for (const attr of node.openingElement.attributes) {
      if (
        attr.type === 'JSXAttribute' &&
        (attr.name as t.JSXIdentifier).name !== 'ref' &&
        (attr.name as t.JSXIdentifier).name !== 'key' &&
        attr.value?.type === 'JSXExpressionContainer'
      ) {
        try {
          evalStatic(attr.value.expression, scope);
        } catch {
          hasDynamic = true;
        }
      }
    }

    for (const child of node.children) {
      walk(child);
    }
  };

  walk(element);
  return hasDynamic;
}

/**
 * Emits the vector node's box: create + props + width/height + optional style={}.
 *
 * @param nodeId  The node's variable.
 * @param width  Its width, when a number.
 * @param height  Its height, when a number.
 * @param openingElement  The <Svg>'s opening tag, for its style and ref.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 */
function emitSvgBox(
  nodeId: string,
  width: unknown,
  height: unknown,
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  out: Out,
  env: Env,
): void {
  const {staticAssigns} = collectStyleAssigns(openingElement, scope, env);
  out.build.push(
    `    ${nodeId} = er_node_create(ER_NODE_VECTOR);`,
    `    er_props_default(&p);`,
  );

  if (typeof width === 'number') {
    out.build.push(`    p.width = (int16_t)${Math.round(width)};`);
  }

  if (typeof height === 'number') {
    out.build.push(`    p.height = (int16_t)${Math.round(height)};`);
  }

  for (const assign of staticAssigns) {
    out.build.push(`    p.${assign.field} = ${assign.expr};`);
  }

  out.build.push(`    er_node_set_props(${nodeId}, &p);`);
  emitRefBind(nodeId, openingElement, out, env);
}

/**
 * <Svg> → ER_NODE_VECTOR. Static subtree → is a baked const op-tape, converted by flattenSvg (the same
 * converter Flow A uses); any state-driven attr → a symbolic op-tape rebuilt by a generated build_svgN() at
 * build time and on every app_update. <Svg source> draws a .svg baked ahead of time (bakeSvgArtifacts).
 *
 * @param element  The <Svg>.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param opts  How the element is placed.
 *
 * @returns The node's variable.
 */
export function emitSvg(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
  opts: EmitOptions,
): string {
  if (opts.displayCode) {
    throw new Error(
      'AOT: an <Svg> inside a dynamic conditional is not yet supported',
    );
  }

  // The vector box takes static style only, so reject `visible` rather than silently ignoring a hide prop.
  if (
    element.openingElement.attributes.some(
      attr =>
        attr.type === 'JSXAttribute' &&
        attr.name &&
        attr.name.name === 'visible',
    )
  ) {
    throw aotError(
      'AOT: `visible` on an <Svg> is not supported',
      'wrap it: <View visible={…}><Svg …/></View> — the View carries the hide and the whole subtree goes with it.',
    );
  }

  // Pick the path: a baked .svg file, a state-driven tape, or a static one.
  const sourceAttr = element.openingElement.attributes.find(
    (candidate): candidate is t.JSXAttribute =>
      candidate.type === 'JSXAttribute' &&
      candidate.name &&
      candidate.name.name === 'source',
  );
  if (sourceAttr) return emitSvgSource(element, sourceAttr, scope, out, env);

  return svgHasDynamic(element, scope)
    ? emitSvgDynamic(element, scope, out, env, state)
    : emitSvgStatic(element, scope, out, env);
}

/**
 * Static <Svg>: reuse flattenSvg (full feature set: viewBox, <G>, Path) and bake const arrays.
 *
 * @param element  The <Svg>.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 *
 * @returns The node's variable.
 */
function emitSvgStatic(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
): string {
  // Flatten the subtree and bake its ops and paints into const tables.
  const svgElement = jsxToSvgElement(element, scope)!;
  const {ops, paints} = flattenSvg(svgElement.props);
  const nodeId = `n${out.allocateNodeId()}`;
  const svgId = out.svgN++;
  const paintCount = paints.length / PAINT_STRIDE;
  if (ops.length) {
    out.vectorData.push(
      `static const float s_svg${svgId}_ops[] = {\n    ${Array.from(ops, floatLit).join(', ')}\n};`,
    );
    const paintRows = Array.from({length: paintCount}, (_, paintIndex) => {
      const start = paintIndex * PAINT_STRIDE;
      return (
        '    ' + emitVectorPaint(paints.slice(start, start + PAINT_STRIDE))
      );
    });
    out.vectorData.push(
      `static const ERVectorPaint s_svg${svgId}_paints[] = {\n${paintRows.join(',\n')}\n};`,
    );
  }

  // Create the vector node and point it at the tables.
  emitSvgBox(
    nodeId,
    svgElement.props.width,
    svgElement.props.height,
    element.openingElement,
    scope,
    out,
    env,
  );
  if (ops.length) {
    out.build.push(
      `    er_node_set_vector_ops(${nodeId}, s_svg${svgId}_ops, ${ops.length}, ` +
        `s_svg${svgId}_paints, ${paintCount}, NULL, 0);`,
    );
  }

  return nodeId;
}

/**
 * Baked <Svg source={importedSvg}>: the CLI pre-bakes the .svg to a vector artifact (ops/paints/GRADIENTS) *
 * (bakeSvgArtifacts → opts.svgArtifacts) since compileSource is I/O-free. Scaled to the static
 * width/height at compile time, then emitted as const tables. This is the path that carries gradients into
 * Flow B.
 *
 * @param element  The <Svg>.
 * @param sourceAttr  Its `source` attribute.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 *
 * @returns The node's variable.
 */
function emitSvgSource(
  element: t.JSXElement,
  sourceAttr: t.JSXAttribute,
  scope: Scope,
  out: Out,
  env: Env,
): string {
  // Resolve the source to its imported .svg's baked artifact.
  const expr =
    sourceAttr.value && sourceAttr.value.type === 'JSXExpressionContainer'
      ? sourceAttr.value.expression
      : null;
  if (!expr || expr.type !== 'Identifier') {
    throw new Error(
      'AOT: <Svg source> must reference an imported .svg (source={importedSvg})',
    );
  }

  const svgImport = env.svgImports.get(expr.name);
  if (!svgImport) {
    throw new Error(
      `AOT: <Svg source={${expr.name}}> — no matching \`import ${expr.name} from '...svg'\``,
    );
  }

  const artifact = env.svgArtifacts[svgImport.name];
  if (!artifact) {
    throw new Error(
      `AOT: vector artifact for "${svgImport.name}" was not baked (internal: opts.svgArtifacts missing it)`,
    );
  }

  // Scale the artifact at compile time to a static width/height, defaulting to its intrinsic size.
  const numericAttr = (attrName: string, fallback: number): number => {
    const attr = element.openingElement.attributes.find(
      (candidate): candidate is t.JSXAttribute =>
        candidate.type === 'JSXAttribute' &&
        candidate.name &&
        candidate.name.name === attrName,
    );
    if (!attr || attr.value == null) return fallback;

    if (attr.value.type === 'StringLiteral') {
      return svgNum(attr.value.value, fallback);
    }

    if (attr.value.type === 'JSXExpressionContainer') {
      try {
        return svgNum(evalStatic(attr.value.expression, scope), fallback);
      } catch {
        throw new Error(
          'AOT: <Svg source> width/height must be a static number',
        );
      }
    }

    return fallback;
  };

  // Raster fallback: a .svg with unsupported features was baked to a PNG; register it and emit an Image node.
  const width = numericAttr('width', artifact.width);
  const height = numericAttr('height', artifact.height);
  if (artifact.kind === 'raster') {
    if (artifact.png) {
      out.images.set(artifact.name, artifact.png);
    }

    const imageNodeId = `n${out.allocateNodeId()}`;
    const {staticAssigns} = collectStyleAssigns(
      element.openingElement,
      scope,
      env,
    );
    out.build.push(
      `    ${imageNodeId} = er_node_create(${NODE_TYPES.Image});`,
      `    er_props_default(&p);`,
    );

    if (typeof width === 'number') {
      out.build.push(`    p.width = (int16_t)${Math.round(width)};`);
    }

    if (typeof height === 'number') {
      out.build.push(`    p.height = (int16_t)${Math.round(height)};`);
    }

    for (const assign of staticAssigns) {
      out.build.push(`    p.${assign.field} = ${assign.expr};`);
    }

    out.build.push(
      `    snprintf(p.image_name, sizeof(p.image_name), "%s", ${cstr(artifact.name)});`,
    );

    out.build.push(`    er_node_set_props(${imageNodeId}, &p);`);
    emitRefBind(imageNodeId, element.openingElement, out, env);
    return imageNodeId;
  }

  // Scale the vector artifact to the box and bake its ops, paints and gradients into const tables.
  const scaled = scaleVectorArtifact(artifact, width, height);
  const ops = scaled.ops;
  const paints = scaled.paints;
  const gradients: VectorGradient[] = scaled.gradients || [];

  const nodeId = `n${out.allocateNodeId()}`;
  const svgId = out.svgN++;
  const paintCount = paints.length / PAINT_STRIDE;
  if (ops.length) {
    out.vectorData.push(
      `static const float s_svg${svgId}_ops[] = {\n    ${Array.from(ops, floatLit).join(', ')}\n};`,
    );

    const paintRows = Array.from({length: paintCount}, (_, paintIndex) => {
      const start = paintIndex * PAINT_STRIDE;
      return (
        '    ' + emitVectorPaint(paints.slice(start, start + PAINT_STRIDE))
      );
    });

    out.vectorData.push(
      `static const ERVectorPaint s_svg${svgId}_paints[] = {\n${paintRows.join(',\n')}\n};`,
    );

    if (gradients.length) {
      out.vectorData.push(
        `static const ERVectorGradient s_svg${svgId}_grads[] = ` +
          `{\n${gradients.map(gradient => '    ' + emitVectorGradient(gradient)).join(',\n')}\n};`,
      );
    }
  }

  // Create the vector node and point it at the tables.
  emitSvgBox(nodeId, width, height, element.openingElement, scope, out, env);
  if (ops.length) {
    const gradsRef = gradients.length ? `s_svg${svgId}_grads` : 'NULL';
    out.build.push(
      `    er_node_set_vector_ops(${nodeId}, s_svg${svgId}_ops, ${ops.length}, ` +
        `s_svg${svgId}_paints, ${paintCount}, ${gradsRef}, ${gradients.length});`,
    );
  }

  return nodeId;
}

/**
 * State-driven <Svg> (flat Arc/Circle/Rect/Line/static-Path; no viewBox/<G>): emit a mutable op-tape
 * + build_svgN() that recomputes it from state, called at build and re-called on each app_update.
 *
 * @param element  The <Svg>.
 * @param scope  Compile-time constants in reach.
 * @param out  Everything emitted so far.
 * @param env  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node's variable.
 */
function emitSvgDynamic(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
): string {
  // The root's attributes give the box; this path has no viewBox transform.
  const rootAttrs = svgAttrs(element.openingElement, scope, env);
  if (rootAttrs.viewBox != null) {
    throw new Error(
      'AOT: a viewBox on a state-driven <Svg> is not yet supported — size shapes in the width/height space',
    );
  }

  // Lower each shape to op-tape entries (after an ER_VOP_SHAPE marker naming its paint) and a paint spec.
  const entries = [];
  const shapeLocals = []; // C locals the shapes need declared ahead of the tape (rounded-rect radii)
  const paintSpecs = [];
  for (const child of svgShapeChildren(element.children)) {
    const type = (child.openingElement.name as t.JSXIdentifier).name;
    const lowerShape = SHAPE_ENTRIES[type];
    if (!lowerShape) {
      throw new Error(
        `AOT: <${type}> is not a supported shape in a state-driven <Svg> (no <G>/viewBox yet)`,
      );
    }

    const attrs = svgAttrs(child.openingElement, scope, env);
    // build_svgN() is this <Svg>'s own function, so the shape index alone keeps a local's name unique.
    const shape = geometryOf(lowerShape(attrs, `s${paintSpecs.length}`));
    if (!shape.entries.length) continue;
    shapeLocals.push(...shape.locals);
    entries.push('ER_VOP_SHAPE', floatLit(paintSpecs.length), ...shape.entries);
    paintSpecs.push(paintSpec(attrs, env, scope));
  }

  // Allocate the node and this <Svg>'s table names.
  const nodeId = `n${out.allocateNodeId()}`;
  const svgId = out.svgN++;
  const opCount = entries.length;
  const paintCount = paintSpecs.length;
  const hasDynamicPaint = paintSpecs.some(paint => paint.anyDynamic);
  out.needsMath = true; // build_svg uses cosf/sinf/M_PI for arcs

  // Flatten gradients into one 1-based table per <Svg>; done here as only the <Svg> knows each gradient's index.
  const gradients: GradientSpec[] = [];
  for (const paint of paintSpecs) {
    const gradientIndexField = (gradient: GradientSpec | null) => {
      if (!gradient) return '0';
      const tableIndex = gradients.push(gradient); // push returns the new length = the 1-based index
      return gradient.cond
        ? `((${gradient.cond}) ? ${tableIndex} : 0)`
        : String(tableIndex);
    };
    paint.fields[7] = gradientIndexField(paint.fillGrad);
    paint.fields[8] = gradientIndexField(paint.strokeGrad);
  }

  // Declare the tables: the op-tape is always mutable, the gradients and paints only when state drives them.
  const hasDynamicGradient = gradients.some(gradient => gradient.anyDynamic);
  out.vectorData.push(`static float s_svg${svgId}_ops[${opCount}];`);
  if (gradients.length) {
    // Mutable when any field is state-driven, e.g., a conic ramp whose start angle follows a setpoint.
    if (hasDynamicGradient) {
      out.vectorData.push(
        `static ERVectorGradient s_svg${svgId}_grads[${gradients.length}];`,
      );
    } else {
      out.vectorData.push(
        `static const ERVectorGradient s_svg${svgId}_grads[] = ` +
          `{\n${gradients.map(gradient => '    ' + gradInitFromSpec(gradient)).join(',\n')}\n};`,
      );
    }
  }

  // Dynamic paint → a MUTABLE paint table (re)filled by build_svg from state each update; else a const table.
  if (hasDynamicPaint) {
    out.vectorData.push(
      `static ERVectorPaint s_svg${svgId}_paints[${paintCount}];`,
    );
  } else {
    out.vectorData.push(
      `static const ERVectorPaint s_svg${svgId}_paints[] = ` +
        `{\n${paintSpecs.map(paint => '    ' + paintInitFromSpec(paint)).join(',\n')}\n};`,
    );
  }

  // Emit build_svgN(), which rewrites the tape, and any mutable paints and gradients, from state.
  const builderLines = [
    ...shapeLocals.map(local => `    ${local}`),
    ...entries.map(
      (entry, entryIndex) => `    s_svg${svgId}_ops[${entryIndex}] = ${entry};`,
    ),
  ];
  if (hasDynamicPaint) {
    paintSpecs.forEach((paint, paintIndex) =>
      paint.fields.forEach((field, fieldIndex) =>
        builderLines.push(
          `    s_svg${svgId}_paints[${paintIndex}].${PAINT_FIELDS[fieldIndex]} = ${field};`,
        ),
      ),
    );
  }

  if (hasDynamicGradient) {
    gradients.forEach((gradient, gradientIndex) =>
      builderLines.push(
        ...gradAssigns(`s_svg${svgId}_grads`, gradientIndex, gradient),
      ),
    );
  }

  out.vectorBuilders.push(
    `static void build_svg${svgId}(void)\n{\n${builderLines.join('\n')}\n}`,
  );

  // Create the vector node, build its tape once, and register it for app_update() to rebuild.
  emitSvgBox(
    nodeId,
    rootAttrs.width,
    rootAttrs.height,
    element.openingElement,
    scope,
    out,
    env,
  );

  out.build.push(
    `    build_svg${svgId}();`,
    `    er_node_set_vector_ops(${nodeId}, s_svg${svgId}_ops, ${opCount}, ` +
      `s_svg${svgId}_paints, ${paintCount}, ` +
      `${gradients.length ? `s_svg${svgId}_grads` : 'NULL'}, ${gradients.length});`,
    `    s_${nodeId} = ${nodeId};`,
  );
  out.handles.push(nodeId);
  out.svgUpdates.push({
    id: svgId,
    len: opCount,
    nPaints: paintCount,
    nGrads: gradients.length,
    nodeVar: `s_${nodeId}`,
  });

  return nodeId;
}
