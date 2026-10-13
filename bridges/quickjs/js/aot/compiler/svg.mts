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

import {check, checkNumber} from './check.mts';
import {NODE_TYPES} from '../style-map.mts';
import {
  flattenSvg,
  parseColor,
  parsePath,
  KAPPA,
  PAINT_STRIDE,
  scaleVectorArtifact,
} from '../../src/embedded-react/svg-ops.js';
import {aotError} from './diagnostics.mts';
import {evalStatic, withUndefined} from './static-eval.mts';
import {lowerExpr} from './expressions.mts';
import {lowerColorValue, collectStyleAssigns} from './style-text.mts';
import {lowerRefBinds} from './nodes.mts';
import {MAX_GRADIENT_STOPS, printTapeValue} from './c/vectors.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {IrNode} from './ir/nodes.mts';
import type {IrExpr} from './ir/expressions.mts';
import type {
  IrDrawingGradient,
  IrDrawingPaint,
  IrGeometry,
  IrGradientRef,
  IrPaintColor,
  IrStateDrivenVector,
  IrTapeValue,
  IrVectorPaint,
  IrVectorTables,
} from './ir/vectors.mts';
import type {EmitOptions, Env, Scope, StateTable} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** An SVG element in the shape flattenSvg takes: its tag and its folded props, children included. */
interface SvgElement {
  type: string;
  props: Record<string, unknown>;
}

/** A state-driven attribute: its lowered value (null for a `d`, which cannot be one) and its node. */
interface DynamicAttr {
  value: IrExpr | null;
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

/** A gradient a shape uses, and the condition it is used under (`cond ? {…} : null`), if any. */
interface ShapeGradient {
  gradient: IrDrawingGradient;
  when: {test: IrExpr; negate: boolean} | null;
}

/** A shape's paint before its gradients have places in the drawing's table, and the gradients it uses. */
interface ShapePaint {
  paint: Omit<IrDrawingPaint, 'fillGradient' | 'strokeGradient'>;
  fillGradient: ShapeGradient | null;
  strokeGradient: ShapeGradient | null;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** strokeLinecap → ERVectorPaint cap. */
export const CAP_MAP: Record<string, number> = {butt: 0, round: 1, square: 2};
/** strokeLinejoin → ERVectorPaint join. */
export const JOIN_MAP: Record<string, number> = {miter: 0, round: 1, bevel: 2};

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
 * A flattenSvg paint record [fill, stroke, w, miter, cap, join, rule, fill_grad, stroke_grad] as a paint.
 * fill_grad/stroke_grad (1-based gradient-table indices, 0 = solid) are absent on inline-<Svg> records (7-wide)
 * and set on a baked <Svg source>.
 *
 * @param record  The paint record.
 *
 * @returns The paint.
 */
export const vectorPaintOf = (record: number[]): IrVectorPaint => ({
  fill: record[0] >>> 0,
  stroke: record[1] >>> 0,
  strokeWidth: record[2],
  miter: record[3],
  cap: record[4] | 0,
  join: record[5] | 0,
  fillRule: record[6] | 0,
  fillGradient: (record[7] || 0) | 0,
  strokeGradient: (record[8] || 0) | 0,
});

/**
 * Refuses a paint whose widths C cannot write, where the paint is lowered rather than when its table is.
 *
 * @param paints  The paints, in table order.
 */
export function checkVectorPaints(paints: IrVectorPaint[]): void {
  for (const paint of paints) {
    checkNumber(paint.strokeWidth);
    checkNumber(paint.miter);
  }
}

/**
 * Refuses baked tables holding a number C cannot write, in the order the tables are written: the ops, the
 * paints, then each gradient's kept stops, and geometry. An empty tape has no tables to check.
 *
 * @param tables  The drawing.
 */
function checkVectorTables(tables: IrVectorTables): void {
  if (!tables.ops.length) return;
  for (const op of tables.ops) {
    checkNumber(op);
  }
  checkVectorPaints(tables.paints);
  for (const gradient of tables.gradients) {
    for (const stop of (gradient.stops || []).slice(0, MAX_GRADIENT_STOPS)) {
      checkNumber(stop.offset);
    }
    const {ax, ay, bx, by, r} = gradient;
    for (const field of [ax, ay, bx, by, r]) {
      checkNumber(field || 0);
    }
  }
}

/**
 * A drawing's flat paint records as paints, one per shape.
 *
 * @param records  The records, PAINT_STRIDE numbers each.
 *
 * @returns The paints.
 */
function paintsOf(records: ArrayLike<number>): IrVectorPaint[] {
  const values = Array.from(records);
  return Array.from({length: values.length / PAINT_STRIDE}, (_, paintIndex) => {
    const start = paintIndex * PAINT_STRIDE;
    return vectorPaintOf(values.slice(start, start + PAINT_STRIDE));
  });
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
  value != null && typeof value === 'object' && 'node' in value;

/**
 * Reads an SVG element's attributes → { name: number|string|true | DynamicAttr } (state attrs → DynamicAttr).
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
          attrs[name] = {value: null, node: valueNode.expression};
          continue;
        }

        // Keep the node too: fill/stroke lower to an ARGB color from it, not from the numeric value.
        attrs[name] = {
          value: lowerExpr(valueNode.expression, env),
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
 * Flow A's svg-ops.js takes. Every geometry field and every stop may be static or state-driven; `type` and the
 * stop COUNT must be static, since they decide the emitted table's shape.
 *
 * Conic gradients carry the sweep's start angle in `r` (radians, clockwise from the top), so a dial whose
 * ramp must follow a setpoint drives `r` from state — which is exactly why the table can't be const.
 *
 * @param node  The descriptor's object literal.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 * @param attrName  The attribute, for errors.
 *
 * @returns The gradient.
 */
function lowerGradient(
  node: t.Node | null | undefined,
  scope: Scope,
  env: Env,
  attrName: string,
): IrDrawingGradient {
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

  // A numeric field: a number when it folds to one (which the check pass checks), otherwise the state-driven value.
  const floatField = (
    valueNode: t.Node | undefined,
    fallback: number,
  ): IrTapeValue => {
    if (valueNode == null) return tapeNumber(fallback);
    let value: unknown;
    try {
      value = evalStatic(valueNode, scope);
    } catch {
      return {kind: 'float', value: lowerExpr(valueNode, env)};
    }

    checkNumber(value);
    return tapeNumber(Number(value));
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

  check({
    kind: 'gradientStops',
    attribute: attrName,
    count: stopsNode.elements.length,
  });

  // Lower each stop: a static color folds to its ARGB, a state-driven one to the color state chooses.
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

    let color: IrPaintColor;
    try {
      color = {
        kind: 'static',
        argb: parseColor(evalStatic(stopProps.color, scope)) >>> 0,
      };
    } catch {
      color = {kind: 'state', value: lowerColorValue(stopProps.color, env)};
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
  };
}

/**
 * Unwraps a gradient attribute, which may be a bare object literal or a CONDITIONAL one:
 * `cond ? {…} : null` (either way round) or `cond && {…}`. The gradient table entry is emitted either
 * way; what the condition drives is the PAINT'S INDEX, which is the entry only while the condition holds.
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
 * @returns The gradient and its condition, or null when the shape has none.
 */
function lowerGradientAttr(
  gradientAttr: GradientAttr | undefined,
  scope: Scope,
  env: Env,
  attrName: string,
): ShapeGradient | null {
  if (!gradientAttr) return null;

  // Peel a conditional down to its object literal, keeping the condition.
  let node: t.Node = gradientAttr.gradNode;
  let when: ShapeGradient['when'] = null;
  const nullish = (branch: t.Node) =>
    branch.type === 'NullLiteral' ||
    (branch.type === 'Identifier' && branch.name === 'undefined');
  if (node.type === 'ConditionalExpression') {
    if (
      node.consequent.type === 'ObjectExpression' &&
      nullish(node.alternate)
    ) {
      when = {test: lowerExpr(node.test, env), negate: false};
      node = node.consequent;
    } else if (
      node.alternate.type === 'ObjectExpression' &&
      nullish(node.consequent)
    ) {
      when = {test: lowerExpr(node.test, env), negate: true};
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
    when = {test: lowerExpr(node.left, env), negate: false};
    node = node.right;
  }

  return {gradient: lowerGradient(node, scope, env, attrName), when};
}

/**
 * Lowers a shape's paint and the gradients it uses:
 *   - fill / stroke may be state-driven (a color string, a ternary of them, or a folded theme token);
 *   - strokeWidth may be state-driven;
 *   - cap / join / miterlimit / fillRule must be static (a state-driven one throws a clear error);
 *   - fillGrad / strokeGrad are lowered here; the caller gives them places in the drawing's table.
 *
 * @param attrs  The shape's attributes.
 * @param env  The expression environment.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The paint, and the shape's gradients.
 */
function lowerShapePaint(attrs: SvgAttrs, env: Env, scope: Scope): ShapePaint {
  // Colors and the stroke width are the only paint attributes that may be state-driven.
  const color = (attrValue: unknown, fallback: string): IrPaintColor =>
    isDynamicAttr(attrValue)
      ? {kind: 'state', value: lowerColorValue(attrValue.node, env)}
      : {kind: 'static', argb: parseColor(attrValue ?? fallback) >>> 0};

  const strokeWidth: IrTapeValue = isDynamicAttr(attrs.strokeWidth)
    ? {kind: 'float', value: attrs.strokeWidth.value!}
    : checkedTapeNumber(svgNum(attrs.strokeWidth, 1));

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

  const fill = color(attrs.fill, 'black');
  const stroke = color(attrs.stroke, 'none');
  const miter = svgNum(attrs.strokeMiterlimit, 4);
  checkNumber(miter);
  const paint = {
    fill,
    stroke,
    strokeWidth,
    miter,
    cap: CAP_MAP[attrs.strokeLinecap as string] ?? 0,
    join: JOIN_MAP[attrs.strokeLinejoin as string] ?? 0,
    fillRule: attrs.fillRule === 'evenodd' ? 1 : 0,
  };
  const fillGradient = lowerGradientAttr(
    attrs.fillGrad as GradientAttr | undefined,
    scope,
    env,
    'fillGrad',
  );
  const strokeGradient = lowerGradientAttr(
    attrs.strokeGrad as GradientAttr | undefined,
    scope,
    env,
    'strokeGrad',
  );

  return {paint, fillGradient, strokeGradient};
}

// Per-shape op-tape geometry, shared by the JSX path and the imperative updateVector path, as in svg-ops.

/** A number as a tape slot. */
const tapeNumber = (value: number): IrTapeValue => ({kind: 'number', value});

/** The zero slot. */
const TAPE_ZERO = tapeNumber(0);

/**
 * A number from the app as a tape slot, checked now that C can write it, where the shape is lowered.
 *
 * @param value  The number.
 *
 * @returns The slot.
 */
export const checkedTapeNumber = (value: number): IrTapeValue => {
  checkNumber(value);
  return tapeNumber(value);
};

/**
 * An opcode as a tape slot.
 *
 * @param op  The ER_VOP_* name.
 *
 * @returns The slot.
 */
const tapeOp = (op: string): IrTapeValue => ({kind: 'op', op});

/**
 * `first`, then each operator and operand in turn.
 *
 * @param first  The first operand.
 * @param rest  Each operator and the operand after it.
 *
 * @returns The slot.
 */
const tapeChain = (
  first: IrTapeValue,
  ...rest: ['+' | '-' | '*', IrTapeValue][]
): IrTapeValue => ({
  kind: 'chain',
  first,
  rest: rest.map(([op, value]) => ({op, value})),
});

/**
 * The value of a slot that reads as a C float literal, or null when state drives it. This reads the slot's C
 * text, the way the geometry folded before it was IR, so the generated C stays the same.
 *
 * @param value  The slot, if there is one.
 *
 * @returns Its value, or null.
 */
const tapeLiteral = (value: IrTapeValue | null): number | null =>
  value == null ? null : cLiteralValue(printTapeValue(value));

/**
 * An arc's tape slots. The engine measures angles in radians clockwise from 3 o'clock, so each angle is
 * converted from degrees clockwise from the top.
 *
 * @param cx  The center's x.
 * @param cy  The center's y.
 * @param radius  The radius.
 * @param startDeg  The start angle, in degrees clockwise from the top.
 * @param endDeg  The end angle, likewise.
 *
 * @returns The slots.
 */
export const arcTape = (
  cx: IrTapeValue,
  cy: IrTapeValue,
  radius: IrTapeValue,
  startDeg: IrTapeValue,
  endDeg: IrTapeValue,
): IrTapeValue[] => [
  tapeOp('ER_VOP_ARC'),
  cx,
  cy,
  radius,
  {kind: 'radians', degrees: startDeg},
  {kind: 'radians', degrees: endDeg},
  TAPE_ZERO,
];

/**
 * A circle's tape slots.
 *
 * @param cx  The center's x.
 * @param cy  The center's y.
 * @param radius  The radius.
 *
 * @returns The slots.
 */
export const circleTape = (
  cx: IrTapeValue,
  cy: IrTapeValue,
  radius: IrTapeValue,
): IrTapeValue[] => [
  tapeOp('ER_VOP_MOVE'),
  tapeChain(cx, ['+', radius]),
  cy,
  tapeOp('ER_VOP_ARC'),
  cx,
  cy,
  radius,
  TAPE_ZERO,
  {kind: 'fullTurn'},
  TAPE_ZERO,
  tapeOp('ER_VOP_CLOSE'),
];

/**
 * The numeric value of a C float literal (as produced by floatLit, or the `(float)(N)` cast the imperative
 * updateVector path emits), or null for a state-driven expr.
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
 * A corner radius clamped to half its side, mirroring svg-ops rectRadii. Folded to a number when both the
 * radius and the side are known — the usual case, since only x/y/width tend to be state-driven.
 *
 * @param radius  The radius.
 * @param side  The side it is clamped against.
 *
 * @returns The clamped radius.
 */
const clampRadius = (radius: IrTapeValue, side: IrTapeValue): IrTapeValue => {
  const radiusValue = tapeLiteral(radius);
  if (radiusValue != null && radiusValue <= 0) return TAPE_ZERO;

  const sideValue = tapeLiteral(side);
  if (radiusValue != null && sideValue != null) {
    return tapeNumber(Math.max(0, Math.min(radiusValue, sideValue / 2)));
  }

  return {kind: 'clampRadius', radius, side};
};

/**
 * A square-cornered rectangle's tape slots.
 *
 * @param x  The left edge.
 * @param y  The top edge.
 * @param width  The width.
 * @param height  The height.
 *
 * @returns The slots.
 */
const sharpRectTape = (
  x: IrTapeValue,
  y: IrTapeValue,
  width: IrTapeValue,
  height: IrTapeValue,
): IrTapeValue[] => [
  tapeOp('ER_VOP_MOVE'),
  x,
  y,
  tapeOp('ER_VOP_LINE'),
  tapeChain(x, ['+', width]),
  y,
  tapeOp('ER_VOP_LINE'),
  tapeChain(x, ['+', width]),
  tapeChain(y, ['+', height]),
  tapeOp('ER_VOP_LINE'),
  x,
  tapeChain(y, ['+', height]),
  tapeOp('ER_VOP_CLOSE'),
];

/**
 * A rectangle's tape slots. rx/ry are null when the rect has no corner radius at all. Corners are cubics, not
 * ER_VOP_ARC, for the same reason svg-ops uses them: a corner must start at EXACTLY the preceding line's
 * endpoint. `tag` names the locals and must be unique within the caller's C block.
 *
 * @param x  The left edge.
 * @param y  The top edge.
 * @param width  The width.
 * @param height  The height.
 * @param rx  The horizontal corner radius, or null.
 * @param ry  The vertical corner radius, or null.
 * @param tag  Names the locals a state-driven radius needs.
 *
 * @returns The slots, and the locals to declare ahead of them.
 */
export const rectTape = (
  x: IrTapeValue,
  y: IrTapeValue,
  width: IrTapeValue,
  height: IrTapeValue,
  rx: IrTapeValue | null,
  ry: IrTapeValue | null,
  tag: string,
): IrGeometry => {
  // A negative literal radius means `auto` (use the other one, as browsers do); a state-driven radius only clamps.
  const isNegativeLiteral = (radius: IrTapeValue | null) => {
    const value = tapeLiteral(radius);
    return value != null && value < 0;
  };
  if (isNegativeLiteral(rx)) {
    rx = null;
  }
  if (isNegativeLiteral(ry)) {
    ry = null;
  }
  if (rx == null && ry == null) {
    return {entries: sharpRectTape(x, y, width, height), locals: []};
  }

  // A missing radius takes the other's value, each clamps to half its side, and a zero radius squares the corners.
  let radiusX = clampRadius((rx ?? ry)!, width);
  let radiusY = clampRadius((ry ?? rx)!, height);
  if (tapeLiteral(radiusX) === 0 || tapeLiteral(radiusY) === 0) {
    return {entries: sharpRectTape(x, y, width, height), locals: []};
  }

  // Bind an unfolded clamp to a local rather than repeat it ten times in the tape; a folded radius stays inline.
  const locals: IrGeometry['locals'] = [];
  if (tapeLiteral(radiusX) == null) {
    locals.push({name: `rx_${tag}`, value: radiusX});
    radiusX = {kind: 'local', name: `rx_${tag}`};
  }
  if (tapeLiteral(radiusY) == null) {
    locals.push({name: `ry_${tag}`, value: radiusY});
    radiusY = {kind: 'local', name: `ry_${tag}`};
  }

  // Trace clockwise from the top edge's start: each side a line, each corner a cubic with KAPPA-length handles.
  const kappa = tapeNumber(KAPPA);
  const handleX = tapeChain(radiusX, ['*', kappa]);
  const handleY = tapeChain(radiusY, ['*', kappa]);
  const innerLeft = tapeChain(x, ['+', radiusX]);
  const innerRight = tapeChain(x, ['+', width], ['-', radiusX]);
  const innerTop = tapeChain(y, ['+', radiusY]);
  const innerBottom = tapeChain(y, ['+', height], ['-', radiusY]);
  const right = tapeChain(x, ['+', width]);
  const bottom = tapeChain(y, ['+', height]);
  const entries = [
    tapeOp('ER_VOP_MOVE'),
    innerLeft,
    y,
    tapeOp('ER_VOP_LINE'),
    innerRight,
    y,
    tapeOp('ER_VOP_CUBIC'),
    tapeChain(innerRight, ['+', handleX]),
    y,
    right,
    tapeChain(innerTop, ['-', handleY]),
    right,
    innerTop,
    tapeOp('ER_VOP_LINE'),
    right,
    innerBottom,
    tapeOp('ER_VOP_CUBIC'),
    right,
    tapeChain(innerBottom, ['+', handleY]),
    tapeChain(innerRight, ['+', handleX]),
    bottom,
    innerRight,
    bottom,
    tapeOp('ER_VOP_LINE'),
    innerLeft,
    bottom,
    tapeOp('ER_VOP_CUBIC'),
    tapeChain(innerLeft, ['-', handleX]),
    bottom,
    x,
    tapeChain(innerBottom, ['+', handleY]),
    x,
    innerBottom,
    tapeOp('ER_VOP_LINE'),
    x,
    innerTop,
    tapeOp('ER_VOP_CUBIC'),
    x,
    tapeChain(innerTop, ['-', handleY]),
    tapeChain(innerLeft, ['-', handleX]),
    y,
    innerLeft,
    y,
    tapeOp('ER_VOP_CLOSE'),
  ];

  return {entries, locals};
};

/**
 * A line's tape slots.
 *
 * @param x1  The start's x.
 * @param y1  The start's y.
 * @param x2  The end's x.
 * @param y2  The end's y.
 *
 * @returns The slots.
 */
export const lineTape = (
  x1: IrTapeValue,
  y1: IrTapeValue,
  x2: IrTapeValue,
  y2: IrTapeValue,
): IrTapeValue[] => [
  tapeOp('ER_VOP_MOVE'),
  x1,
  y1,
  tapeOp('ER_VOP_LINE'),
  x2,
  y2,
];

/**
 * An SVG coordinate attribute as a tape slot: a number when static, the state-driven value read as a float.
 *
 * @param attrValue  The attribute's value.
 * @param fallback  The value to use when it is absent or not a number.
 *
 * @returns The slot.
 */
const attrTape = (attrValue: unknown, fallback = 0): IrTapeValue =>
  isDynamicAttr(attrValue)
    ? {kind: 'float', value: attrValue.value!}
    : checkedTapeNumber(svgNum(attrValue, fallback));

// The JSX path: each shape's attributes resolve to tape slots via attrTape.
const arcShape = (attrs: SvgAttrs): IrGeometry => ({
  entries: arcTape(
    attrTape(attrs.cx),
    attrTape(attrs.cy),
    attrTape(attrs.r),
    attrTape(attrs.startAngle),
    attrTape(attrs.endAngle),
  ),
  locals: [],
});
const circleShape = (attrs: SvgAttrs): IrGeometry => ({
  entries: circleTape(
    attrTape(attrs.cx),
    attrTape(attrs.cy),
    attrTape(attrs.r),
  ),
  locals: [],
});
const rectShape = (attrs: SvgAttrs, tag: string): IrGeometry =>
  rectTape(
    attrTape(attrs.x),
    attrTape(attrs.y),
    attrTape(attrs.width),
    attrTape(attrs.height),
    attrs.rx == null ? null : attrTape(attrs.rx),
    attrs.ry == null ? null : attrTape(attrs.ry),
    tag,
  );
const lineShape = (attrs: SvgAttrs): IrGeometry => ({
  entries: lineTape(
    attrTape(attrs.x1),
    attrTape(attrs.y1),
    attrTape(attrs.x2),
    attrTape(attrs.y2),
  ),
  locals: [],
});
const pathShape = (attrs: SvgAttrs): IrGeometry => {
  if (attrs.d == null) return {entries: [], locals: []};
  if (isDynamicAttr(attrs.d)) {
    throw new Error(
      'AOT: a state-driven <Path d=…> is not yet supported (use Arc/Circle/Rect/Line for dynamic shapes)',
    );
  }
  // Opcodes are encoded as float values 0..6, like coords.
  return {
    entries: parsePath(String(attrs.d)).map((value: number) =>
      checkedTapeNumber(value),
    ),
    locals: [],
  };
};

/** The shapes a state-driven <Svg> can hold, by tag. */
const SHAPE_TAPES: Record<
  string,
  (attrs: SvgAttrs, tag: string) => IrGeometry
> = {
  Arc: arcShape,
  Circle: circleShape,
  Rect: rectShape,
  Line: lineShape,
  Path: pathShape,
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
 * Lowers the box an <Svg> draws in: a node of `type` sized to a numeric width/height, then its static style,
 * then its refs. Width and height are written as int16 casts, ahead of the style.
 *
 * @param nodeId  The node's variable.
 * @param type  The engine node type: a vector node, or an image for a rasterized .svg.
 * @param width  Its width, when a number.
 * @param height  Its height, when a number.
 * @param openingElement  The <Svg>'s opening tag, for its style and ref.
 * @param scope  Compile-time constants in reach.
 * @param env  The expression environment.
 *
 * @returns The node, with its refs wired.
 */
function lowerSvgBox(
  nodeId: string,
  type: string,
  width: unknown,
  height: unknown,
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  env: Env,
): IrNode {
  const {staticAssigns} = collectStyleAssigns(openingElement, scope, env);
  const size = [];
  if (typeof width === 'number') {
    size.push({field: 'width', expr: `(int16_t)${Math.round(width)}`});
  }

  if (typeof height === 'number') {
    size.push({field: 'height', expr: `(int16_t)${Math.round(height)}`});
  }

  return {
    id: nodeId,
    type,
    constantProps: [...size, ...staticAssigns],
    stateProps: [],
    placeholder: null,
    text: null,
    image: null,
    spans: null,
    wiring: lowerRefBinds(openingElement, env).map(slot => ({
      kind: 'ref',
      slot,
    })),
    children: [],
  };
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
 * @returns The node.
 */
export function emitSvg(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
  opts: EmitOptions,
): IrNode {
  if (opts.show) {
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
 * @returns The node.
 */
function emitSvgStatic(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
): IrNode {
  // Flatten the subtree and bake its ops and paints into const tables.
  const svgElement = jsxToSvgElement(element, scope)!;
  const {ops, paints} = flattenSvg(svgElement.props);
  const nodeId = `n${out.allocateNodeId()}`;
  const tables: IrVectorTables = {
    id: out.svgN++,
    ops: Array.from(ops),
    paints: paintsOf(paints),
    gradients: [],
  };
  checkVectorTables(tables);
  out.vectors.push({kind: 'tables', tables});

  // Create the vector node and point it at the tables.
  const node = lowerSvgBox(
    nodeId,
    'ER_NODE_VECTOR',
    svgElement.props.width,
    svgElement.props.height,
    element.openingElement,
    scope,
    env,
  );
  node.wiring.push({kind: 'vectorTables', tables});
  return node;
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
 * @returns The node.
 */
function emitSvgSource(
  element: t.JSXElement,
  sourceAttr: t.JSXAttribute,
  scope: Scope,
  out: Out,
  env: Env,
): IrNode {
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

    const node = lowerSvgBox(
      `n${out.allocateNodeId()}`,
      NODE_TYPES.Image,
      width,
      height,
      element.openingElement,
      scope,
      env,
    );
    node.image = {kind: 'static', name: artifact.name, resetOnUpdate: false};
    return node;
  }

  // Scale the vector artifact to the box and bake its ops, paints and gradients into const tables.
  const scaled = scaleVectorArtifact(artifact, width, height);
  const nodeId = `n${out.allocateNodeId()}`;
  const tables: IrVectorTables = {
    id: out.svgN++,
    ops: Array.from(scaled.ops),
    paints: paintsOf(scaled.paints),
    gradients: scaled.gradients || [],
  };
  checkVectorTables(tables);
  out.vectors.push({kind: 'tables', tables});

  // Create the vector node and point it at the tables.
  const node = lowerSvgBox(
    nodeId,
    'ER_NODE_VECTOR',
    width,
    height,
    element.openingElement,
    scope,
    env,
  );
  node.wiring.push({kind: 'vectorTables', tables});
  return node;
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
 * @returns The node.
 */
function emitSvgDynamic(
  element: t.JSXElement,
  scope: Scope,
  out: Out,
  env: Env,
  state: StateTable,
): IrNode {
  // The root's attributes give the box; this path has no viewBox transform.
  const rootAttrs = svgAttrs(element.openingElement, scope, env);
  if (rootAttrs.viewBox != null) {
    throw new Error(
      'AOT: a viewBox on a state-driven <Svg> is not yet supported — size shapes in the width/height space',
    );
  }

  // Lower each shape to tape slots (after an ER_VOP_SHAPE marker naming its paint) and its paint.
  const tape: IrTapeValue[] = [];
  const locals: IrGeometry['locals'] = []; // declared ahead of the tape (rounded-rect radii)
  const shapePaints: ShapePaint[] = [];
  for (const child of svgShapeChildren(element.children)) {
    const type = (child.openingElement.name as t.JSXIdentifier).name;
    const lowerShape = SHAPE_TAPES[type];
    if (!lowerShape) {
      throw new Error(
        `AOT: <${type}> is not a supported shape in a state-driven <Svg> (no <G>/viewBox yet)`,
      );
    }

    const attrs = svgAttrs(child.openingElement, scope, env);
    // build_svgN() is this <Svg>'s own function, so the shape index alone keeps a local's name unique.
    const shape = lowerShape(attrs, `s${shapePaints.length}`);
    if (!shape.entries.length) continue;
    locals.push(...shape.locals);
    tape.push(
      tapeOp('ER_VOP_SHAPE'),
      tapeNumber(shapePaints.length),
      ...shape.entries,
    );
    shapePaints.push(lowerShapePaint(attrs, env, scope));
  }

  // Allocate the node and this <Svg>'s table names.
  const nodeId = `n${out.allocateNodeId()}`;
  const svgId = out.svgN++;
  out.needsMath = true; // build_svg uses cosf/sinf/M_PI for arcs

  // Gather the gradients into one 1-based table per <Svg>, fill then stroke, shape by shape.
  const gradients: IrDrawingGradient[] = [];
  const place = (shapeGradient: ShapeGradient | null): IrGradientRef =>
    shapeGradient
      ? {
          index: gradients.push(shapeGradient.gradient),
          when: shapeGradient.when,
        }
      : {index: 0, when: null};
  const vector: IrStateDrivenVector = {
    id: svgId,
    locals,
    tape,
    paints: shapePaints.map(shapePaint => ({
      ...shapePaint.paint,
      fillGradient: place(shapePaint.fillGradient),
      strokeGradient: place(shapePaint.strokeGradient),
    })),
    gradients,
  };

  // Declare the tables and build_svgN(), which rewrites the tape (and anything in the paints state drives).
  out.vectors.push({kind: 'drawing', vector});

  // Create the vector node, build its tape once, and register it for app_update() to rebuild.
  const node = lowerSvgBox(
    nodeId,
    'ER_NODE_VECTOR',
    rootAttrs.width,
    rootAttrs.height,
    element.openingElement,
    scope,
    env,
  );
  node.wiring.push({kind: 'stateDrivenVector', vector});

  return node;
}
