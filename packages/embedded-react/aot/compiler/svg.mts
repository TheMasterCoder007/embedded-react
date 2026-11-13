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

import {settledOf} from './typing.mts';
import {check, checkNumber} from './check.mts';
import {lowerStyle, NODE_TYPES} from '../style-map.mts';
import {
  flattenSvg,
  parseColor,
  parsePath,
  KAPPA,
  PAINT_PROPS,
  PAINT_STRIDE,
  scaleVectorArtifact,
  VOP_ARC,
  VOP_CUBIC,
  VOP_LINE,
  VOP_MOVE,
  VOP_QUAD,
  VOP_SHAPE,
} from '../../src/embedded-react/svg-ops.js';
import {aotError, currentLoc} from './diagnostics.mts';
import {
  bootSlotOf,
  bootSourceText,
  buildValueOf,
  containsBoot,
  foldBoot,
  isBoot,
} from './boot.mts';
import {evalStatic, withUndefined} from './static-eval.mts';
import {lowerExpr} from './expressions.mts';
import {
  lowerColorValue,
  bootStyleAssign,
  collectStyleAssigns,
  styleWrites,
} from './style-text.mts';
import {checkInterpolations, lowerRefBinds} from './nodes.mts';
import {
  at,
  checkAttributes,
  checkNoChildren,
  elementSpec,
  svgShapeSpec,
} from './elements/props.mts';
import {MAX_GRADIENT_STOPS} from './ir/vectors.mts';
import type * as t from '@babel/types';
import type {AotError} from './diagnostics.mts';
import type {Out} from './out.mts';
import type {IrNode} from './ir/nodes.mts';
import type {IrExpr} from './ir/expressions.mts';
import {FLOAT} from './ir/types.mts';
import type {DynAssign} from './types.mts';
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
import type {
  EmitOptions,
  Env,
  Scope,
  StateTable,
  StaticAssign,
} from './types.mts';

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

/** A gradient attribute (fillGradient / strokeGradient), kept as its expression for gradAttr to read. */
interface GradientAttr {
  gradientNode: t.Expression | t.JSXEmptyExpression;
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

/** SVG's stroke-miterlimit when a shape sets none. */
export const DEFAULT_MITER_LIMIT = 4;

/** Gradient types: 1 linear, 2 radial, 3 conic. */
const GRADIENT_LINEAR = 1;
const GRADIENT_CONIC = 3;

/** Where each field sits in a flattenSvg paint record. */
const PAINT_RECORD = {
  fill: 0,
  stroke: 1,
  strokeWidth: 2,
  miter: 3,
  cap: 4,
  join: 5,
  fillRule: 6,
  fillGradient: 7,
  strokeGradient: 8,
} as const;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Refuses a child an <Svg> cannot draw, an attribute its shape does not declare, and children on a shape
 * other than a <G>.
 *
 * @param child  The child element.
 */
function checkShape(child: t.JSXElement): void {
  const nameNode = child.openingElement.name;
  const tag = nameNode.type === 'JSXIdentifier' ? nameNode.name : null;
  const spec = tag == null ? undefined : svgShapeSpec(tag);
  if (!spec) {
    throw at(
      aotError(
        `AOT: <${tag ?? 'this element'}> is not a shape an <Svg> draws`,
        'an <Svg> draws Path, Circle, Ellipse, Rect, Line and Arc, grouped by G. A component is not ' +
          'rendered inside one: inline its shapes.',
      ),
      child,
    );
  }

  checkAttributes(spec, child.openingElement);
  if (spec.children === 'none') {
    checkNoChildren(spec, child.children);
  }
}

/**
 * Refuses a constant `fillGradient` / `strokeGradient` the engine cannot draw as written: the same type and stop rules
 * a state-driven one meets (see lowerGradient). `null` is no gradient.
 *
 * @param name  The attribute.
 * @param value  Its folded value.
 */
function checkStaticGradient(name: string, value: unknown): void {
  if (value == null) return;

  const gradient = value as {type?: unknown; stops?: unknown};
  if (
    typeof value !== 'object' ||
    !Number.isInteger(gradient.type) ||
    (gradient.type as number) < GRADIENT_LINEAR ||
    (gradient.type as number) > GRADIENT_CONIC
  ) {
    throw aotError(
      `AOT: "${name}.type" must be 1 (linear), 2 (radial) or 3 (conic)`,
    );
  }

  if (!Array.isArray(gradient.stops)) {
    throw aotError(`AOT: "${name}.stops" must be an array`);
  }

  check({kind: 'gradientStops', attribute: name, count: gradient.stops.length});
  for (const stop of gradient.stops) {
    if (stop == null || typeof stop !== 'object') {
      throw aotError(`AOT: each "${name}.stops" entry must be an object`);
    }
  }
}

/**
 * Yields an <Svg> subtree's shape children in source order, inlining any `<>…</>` in place: a fragment
 * carries no paint and no transform, so it is transparent to the tape (the Flow A walk in flattenSvg
 * treats it the same way). Whitespace between elements and JSX comments are skipped; anything else
 * that has no shape to contribute throws, rather than vanishing from the package.
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
      checkShape(child);
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

/** A drawing's value worked out from `screen`, which a tape can't hold: a compile error, not a state-driven value. */
class SvgScreenError extends Error {
  aotLoc?: {line: number; column: number};
  aotHint?: string;
}

/**
 * Folds a value of a drawing. A drawing is baked once, so what it draws can't change with the panel: it fails the
 * build, naming the expression, instead of adding a layout for every size.
 *
 * @param node  The expression.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The value.
 */
function evalDrawing(node: t.Node, scope: Scope): unknown {
  const value = evalStatic(node, scope, 'boot');
  if (!containsBoot(value)) return value;

  const error = new SvgScreenError(
    `AOT: an <Svg> shape can't be calculated from \`screen\`: ${bootSourceText(node) ?? 'this value'}`,
  );
  error.aotHint =
    'draw in a viewBox\'s units and size the <Svg> instead: <Svg viewBox="0 0 100 100" width={screen.width} …> ' +
    'scales on every panel.';
  if (node.loc) error.aotLoc = node.loc.start;
  throw error;
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
  for (const attribute of element.openingElement.attributes) {
    if (attribute.type !== 'JSXAttribute') {
      throw new Error(
        'AOT: spread attributes on an <Svg> element not supported',
      );
    }

    const name = (attribute.name as t.JSXIdentifier).name;
    if (name === 'ref' || name === 'key' || name === 'style') continue; // the box's, not the drawing's

    if (attribute.value == null) {
      props[name] = true;
    } else if (attribute.value.type === 'StringLiteral') {
      props[name] = attribute.value.value;
    } else if (attribute.value.type === 'JSXExpressionContainer') {
      props[name] =
        type === 'Svg' && (name === 'width' || name === 'height')
          ? evalStatic(attribute.value.expression, scope, 'boot')
          : evalDrawing(attribute.value.expression, scope);
    } else {
      throw new Error(
        `AOT: unsupported <${type}> attribute value for "${name}"`,
      );
    }
  }

  // A baked gradient takes the same checks as a state-driven one.
  for (const name of ['fillGradient', 'strokeGradient']) {
    checkStaticGradient(name, props[name]);
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
 * A flattenSvg paint record [fill, stroke, w, miter, cap, join, rule, fill_gradient, stroke_gradient] as a paint.
 * fill_gradient/stroke_gradient (1-based gradient-table indices, 0 = solid) are absent on inline-<Svg> records (7-wide)
 * and set on a baked <Svg source>.
 *
 * @param record  The paint record.
 *
 * @returns The paint.
 */
export const vectorPaintOf = (record: number[]): IrVectorPaint => ({
  fill: record[PAINT_RECORD.fill] >>> 0,
  stroke: record[PAINT_RECORD.stroke] >>> 0,
  strokeWidth: record[PAINT_RECORD.strokeWidth],
  miter: record[PAINT_RECORD.miter],
  cap: record[PAINT_RECORD.cap] | 0,
  join: record[PAINT_RECORD.join] | 0,
  fillRule: record[PAINT_RECORD.fillRule] | 0,
  fillGradient: (record[PAINT_RECORD.fillGradient] || 0) | 0,
  strokeGradient: (record[PAINT_RECORD.strokeGradient] || 0) | 0,
});

/**
 * Refuses a paint whose widths aren't finite, where the paint is lowered rather than when its table is.
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
 * Refuses baked tables holding a number that isn't finite, in the order the tables are written: the ops, the
 * paints, then each gradient's kept stops, and geometry. An empty tape has no tables to check.
 *
 * @param tables  The drawing.
 */
function checkVectorTables(tables: IrVectorTables): void {
  if (!tables.ops.length) return;
  for (const word of tables.ops) {
    checkNumber(word);
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
 * True if an attribute value is a state-driven expression (vs. a static number/string).
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
 * @param environment  The expression environment.
 *
 * @returns The attributes, by name.
 */
function svgAttrs(
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  environment: Env,
): SvgAttrs {
  const attributes: SvgAttrs = {};
  const tagName = (openingElement.name as t.JSXIdentifier).name;
  for (const attribute of openingElement.attributes) {
    if (attribute.type !== 'JSXAttribute') {
      throw new Error(
        'AOT: spread attributes on an <Svg> element not supported',
      );
    }

    // Not geometry or paint: the box's style and ref are lowerSvgBox's.
    const name = (attribute.name as t.JSXIdentifier).name;
    if (name === 'ref' || name === 'key' || name === 'style') continue;

    // Keep a gradient as its expression; gradAttr lowers it, conditions included.
    const valueNode = attribute.value;
    if (name === 'fillGradient' || name === 'strokeGradient') {
      if (valueNode == null || valueNode.type !== 'JSXExpressionContainer') {
        throw new Error(`AOT: "${name}" must be an object expression`);
      }

      attributes[name] = {gradientNode: valueNode.expression};
      continue;
    }

    // Fold everything else; an expression that does not fold is state-driven.
    if (valueNode == null) {
      attributes[name] = true;
    } else if (valueNode.type === 'StringLiteral') {
      attributes[name] = valueNode.value;
    } else if (valueNode.type === 'JSXExpressionContainer') {
      try {
        attributes[name] =
          tagName === 'Svg' && (name === 'width' || name === 'height')
            ? evalStatic(valueNode.expression, withUndefined(scope), 'boot')
            : evalDrawing(valueNode.expression, withUndefined(scope));
      } catch (error) {
        if (error instanceof SvgScreenError) throw error;
        // Defer a state-driven `d` so pathEntries raises the error naming the fix, not the expression's own error.
        if (name === 'd') {
          attributes[name] = {value: null, node: valueNode.expression};
          continue;
        }

        // Keep the node too: fill/stroke lower to an ARGB color from it, not from the numeric value.
        attributes[name] = {
          value: lowerExpr(valueNode.expression, environment),
          node: valueNode.expression,
        };
      }
    } else {
      throw new Error(`AOT: unsupported SVG attribute value for "${name}"`);
    }
  }

  return attributes;
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
 * @param environment  The expression environment.
 * @param attributeName  The attribute, for errors.
 *
 * @returns The gradient.
 */
function lowerGradient(
  node: t.Node | null | undefined,
  scope: Scope,
  environment: Env,
  attributeName: string,
): IrDrawingGradient {
  if (!node || node.type !== 'ObjectExpression') {
    throw new Error(`AOT: "${attributeName}" must be an object literal`);
  }

  // Index the descriptor's properties by key.
  const props: Record<string, t.Node> = {};
  for (const property of node.properties) {
    if (property.type !== 'ObjectProperty' || property.computed) {
      throw new Error(
        `AOT: "${attributeName}" takes plain key: value pairs only`,
      );
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
      value = evalDrawing(valueNode, scope);
    } catch (error) {
      if (error instanceof SvgScreenError) throw error;
      return {kind: 'float', value: lowerExpr(valueNode, environment)};
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
      `AOT: "${attributeName}.type" must be a compile-time constant (1 linear, 2 radial, 3 conic)`,
    );
  }

  if (type < GRADIENT_LINEAR || type > GRADIENT_CONIC) {
    throw new Error(
      `AOT: "${attributeName}.type" must be 1 (linear), 2 (radial) or 3 (conic)`,
    );
  }

  const stopsNode = props.stops;
  if (!stopsNode || stopsNode.type !== 'ArrayExpression') {
    throw new Error(`AOT: "${attributeName}.stops" must be an array literal`);
  }

  check({
    kind: 'gradientStops',
    attribute: attributeName,
    count: stopsNode.elements.length,
  });

  // Lower each stop: a static color folds to its ARGB, a state-driven one to the color state chooses.
  const stops = stopsNode.elements.map(stopNode => {
    if (!stopNode || stopNode.type !== 'ObjectExpression') {
      throw new Error(
        `AOT: each "${attributeName}.stops" entry must be an object literal`,
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
        argb: parseColor(evalDrawing(stopProps.color, scope)) >>> 0,
      };
    } catch (error) {
      if (error instanceof SvgScreenError) throw error;
      color = {
        kind: 'state',
        value: lowerColorValue(stopProps.color, environment),
      };
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
 * @param gradientAttribute  The attribute, if the shape has it.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment.
 * @param attributeName  The attribute, for errors.
 *
 * @returns The gradient and its condition, or null when the shape has none.
 */
function lowerGradientAttr(
  gradientAttribute: GradientAttr | undefined,
  scope: Scope,
  environment: Env,
  attributeName: string,
): ShapeGradient | null {
  if (!gradientAttribute) return null;

  // Peel a conditional down to its object literal, keeping the condition.
  let node: t.Node = gradientAttribute.gradientNode;
  let when: ShapeGradient['when'] = null;
  const nullish = (branch: t.Node) =>
    branch.type === 'NullLiteral' ||
    (branch.type === 'Identifier' && branch.name === 'undefined');
  if (node.type === 'ConditionalExpression') {
    if (
      node.consequent.type === 'ObjectExpression' &&
      nullish(node.alternate)
    ) {
      when = {test: lowerExpr(node.test, environment), negate: false};
      node = node.consequent;
    } else if (
      node.alternate.type === 'ObjectExpression' &&
      nullish(node.consequent)
    ) {
      when = {test: lowerExpr(node.test, environment), negate: true};
      node = node.alternate;
    } else {
      throw new Error(
        `AOT: a conditional "${attributeName}" must be \`cond ? { … } : null\` (one branch an ` +
          `object literal, the other null)`,
      );
    }
  } else if (
    node.type === 'LogicalExpression' &&
    node.operator === '&&' &&
    node.right.type === 'ObjectExpression'
  ) {
    when = {test: lowerExpr(node.left, environment), negate: false};
    node = node.right;
  }

  return {
    gradient: lowerGradient(node, scope, environment, attributeName),
    when,
  };
}

/**
 * Lowers a shape's paint and the gradients it uses:
 *   - fill / stroke may be state-driven (a color string, a ternary of them, or a folded theme token);
 *   - strokeWidth may be state-driven;
 *   - cap / join / miterlimit / fillRule must be static (a state-driven one throws a clear error);
 *   - fillGradient / strokeGradient are lowered here; the caller gives them places in the drawing's table.
 *
 * @param attributes  The shape's attributes.
 * @param environment  The expression environment.
 * @param scope  Compile-time constants in reach.
 *
 * @returns The paint, and the shape's gradients.
 */
function lowerShapePaint(
  attributes: SvgAttrs,
  environment: Env,
  scope: Scope,
): ShapePaint {
  // Colors and the stroke width are the only paint attributes that may be state-driven.
  const color = (attributeValue: unknown, fallback: string): IrPaintColor =>
    isDynamicAttr(attributeValue)
      ? {
          kind: 'state',
          value: lowerColorValue(attributeValue.node, environment),
        }
      : {kind: 'static', argb: parseColor(attributeValue ?? fallback) >>> 0};

  const strokeWidth: IrTapeValue = isDynamicAttr(attributes.strokeWidth)
    ? {kind: 'float', value: attributes.strokeWidth.value!}
    : checkedTapeNumber(svgNum(attributes.strokeWidth, 1));

  // The rest of the paint must be static.
  for (const attributeName of [
    'strokeLinecap',
    'strokeLinejoin',
    'strokeMiterlimit',
    'fillRule',
  ]) {
    if (isDynamicAttr(attributes[attributeName])) {
      throw new Error(
        `AOT: a state-driven <Svg> "${attributeName}" is not supported (only fill / stroke / ` +
          `strokeWidth can be state-driven)`,
      );
    }
  }

  const fill = color(attributes.fill, 'black');
  const stroke = color(attributes.stroke, 'none');
  const miter = svgNum(attributes.strokeMiterlimit, DEFAULT_MITER_LIMIT);
  checkNumber(miter);
  const paint = {
    fill,
    stroke,
    strokeWidth,
    miter,
    cap: CAP_MAP[attributes.strokeLinecap as string] ?? 0,
    join: JOIN_MAP[attributes.strokeLinejoin as string] ?? 0,
    fillRule: attributes.fillRule === 'evenodd' ? 1 : 0,
  };
  const fillGradient = lowerGradientAttr(
    attributes.fillGradient as GradientAttr | undefined,
    scope,
    environment,
    'fillGradient',
  );
  const strokeGradient = lowerGradientAttr(
    attributes.strokeGradient as GradientAttr | undefined,
    scope,
    environment,
    'strokeGradient',
  );

  return {paint, fillGradient, strokeGradient};
}

// Per-shape op-tape geometry, shared by the JSX path and the imperative updateVector path, as in svg-ops.

/** A number as a tape slot. */
const tapeNumber = (value: number): IrTapeValue => ({kind: 'number', value});

/** The zero slot. */
const TAPE_ZERO = tapeNumber(0);

/**
 * A number from the app as a tape slot, checked now, where the shape is lowered.
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
 * @param operation  The ER_VECTOR_OP_* name.
 *
 * @returns The slot.
 */
const tapeOp = (operation: string): IrTapeValue => ({kind: 'op', operation});

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
  rest: rest.map(([operator, value]) => ({operator, value})),
});

/**
 * The value of a slot that holds a known number, or null when state drives it.
 *
 * @param value  The slot, if there is one.
 *
 * @returns Its value, or null.
 */
function tapeLiteral(value: IrTapeValue | null): number | null {
  if (value?.kind === 'number') return value.value;
  if (value?.kind !== 'float') return null;
  // One the typing pass has yet to type is no literal: lowering types a literal at once.
  const settled = settledOf(value.value);
  return settled?.kind === 'number' ? settled.value : null;
}

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
  tapeOp('ER_VECTOR_OP_ARC'),
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
  tapeOp('ER_VECTOR_OP_MOVE'),
  tapeChain(cx, ['+', radius]),
  cy,
  tapeOp('ER_VECTOR_OP_ARC'),
  cx,
  cy,
  radius,
  TAPE_ZERO,
  {kind: 'fullTurn'},
  TAPE_ZERO,
  tapeOp('ER_VECTOR_OP_CLOSE'),
];

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
  tapeOp('ER_VECTOR_OP_MOVE'),
  x,
  y,
  tapeOp('ER_VECTOR_OP_LINE'),
  tapeChain(x, ['+', width]),
  y,
  tapeOp('ER_VECTOR_OP_LINE'),
  tapeChain(x, ['+', width]),
  tapeChain(y, ['+', height]),
  tapeOp('ER_VECTOR_OP_LINE'),
  x,
  tapeChain(y, ['+', height]),
  tapeOp('ER_VECTOR_OP_CLOSE'),
];

/**
 * A rectangle's tape slots. rx/ry are null when the rect has no corner radius at all. Corners are cubics, not
 * ER_VECTOR_OP_ARC, for the same reason svg-ops uses them: a corner must start at EXACTLY the preceding line's
 * endpoint. `tag` names the locals and must be unique within the caller's block.
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
    tapeOp('ER_VECTOR_OP_MOVE'),
    innerLeft,
    y,
    tapeOp('ER_VECTOR_OP_LINE'),
    innerRight,
    y,
    tapeOp('ER_VECTOR_OP_CUBIC'),
    tapeChain(innerRight, ['+', handleX]),
    y,
    right,
    tapeChain(innerTop, ['-', handleY]),
    right,
    innerTop,
    tapeOp('ER_VECTOR_OP_LINE'),
    right,
    innerBottom,
    tapeOp('ER_VECTOR_OP_CUBIC'),
    right,
    tapeChain(innerBottom, ['+', handleY]),
    tapeChain(innerRight, ['+', handleX]),
    bottom,
    innerRight,
    bottom,
    tapeOp('ER_VECTOR_OP_LINE'),
    innerLeft,
    bottom,
    tapeOp('ER_VECTOR_OP_CUBIC'),
    tapeChain(innerLeft, ['-', handleX]),
    bottom,
    x,
    tapeChain(innerBottom, ['+', handleY]),
    x,
    innerBottom,
    tapeOp('ER_VECTOR_OP_LINE'),
    x,
    innerTop,
    tapeOp('ER_VECTOR_OP_CUBIC'),
    x,
    tapeChain(innerTop, ['-', handleY]),
    tapeChain(innerLeft, ['-', handleX]),
    y,
    innerLeft,
    y,
    tapeOp('ER_VECTOR_OP_CLOSE'),
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
  tapeOp('ER_VECTOR_OP_MOVE'),
  x1,
  y1,
  tapeOp('ER_VECTOR_OP_LINE'),
  x2,
  y2,
];

/**
 * An SVG coordinate attribute as a tape slot: a number when static, the state-driven value read as a float.
 *
 * @param attributeValue  The attribute's value.
 * @param fallback  The value to use when it is absent or not a number.
 *
 * @returns The slot.
 */
const attrTape = (attributeValue: unknown, fallback = 0): IrTapeValue =>
  isDynamicAttr(attributeValue)
    ? {kind: 'float', value: attributeValue.value!}
    : checkedTapeNumber(svgNum(attributeValue, fallback));

// The JSX path: each shape's attributes resolve to tape slots via attrTape.
const arcShape = (attributes: SvgAttrs): IrGeometry => ({
  entries: arcTape(
    attrTape(attributes.cx),
    attrTape(attributes.cy),
    attrTape(attributes.r),
    attrTape(attributes.startAngle),
    attrTape(attributes.endAngle),
  ),
  locals: [],
});
const circleShape = (attributes: SvgAttrs): IrGeometry => ({
  entries: circleTape(
    attrTape(attributes.cx),
    attrTape(attributes.cy),
    attrTape(attributes.r),
  ),
  locals: [],
});
const rectShape = (attributes: SvgAttrs, tag: string): IrGeometry =>
  rectTape(
    attrTape(attributes.x),
    attrTape(attributes.y),
    attrTape(attributes.width),
    attrTape(attributes.height),
    attributes.rx == null ? null : attrTape(attributes.rx),
    attributes.ry == null ? null : attrTape(attributes.ry),
    tag,
  );
const lineShape = (attributes: SvgAttrs): IrGeometry => ({
  entries: lineTape(
    attrTape(attributes.x1),
    attrTape(attributes.y1),
    attrTape(attributes.x2),
    attrTape(attributes.y2),
  ),
  locals: [],
});
const pathShape = (attributes: SvgAttrs): IrGeometry => {
  if (attributes.d == null) return {entries: [], locals: []};
  if (isDynamicAttr(attributes.d)) {
    throw new Error(
      'AOT: a state-driven <Path d=…> is not yet supported (use Arc/Circle/Rect/Line for dynamic shapes)',
    );
  }
  // Opcodes are encoded as float values 0..6, like coords.
  return {
    entries: parsePath(String(attributes.d)).map((value: number) =>
      checkedTapeNumber(value),
    ),
    locals: [],
  };
};

/** The shapes a state-driven <Svg> can hold, by tag. */
const SHAPE_TAPES: Record<
  string,
  (attributes: SvgAttrs, tag: string) => IrGeometry
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

    for (const attribute of node.openingElement.attributes) {
      if (
        attribute.type === 'JSXAttribute' &&
        (attribute.name as t.JSXIdentifier).name !== 'ref' &&
        (attribute.name as t.JSXIdentifier).name !== 'key' &&
        (attribute.name as t.JSXIdentifier).name !== 'style' &&
        attribute.value?.type === 'JSXExpressionContainer'
      ) {
        try {
          evalStatic(attribute.value.expression, scope, 'boot');
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
 * An <Svg> width or height as pixels.
 *
 * @param field  Which one.
 * @param value  Its folded value.
 * @param scaled  Whether the drawing is scaled to it, which takes pixels.
 *
 * @returns The pixels.
 */
function svgPixels(
  field: 'width' | 'height',
  value: unknown,
  scaled: boolean,
): number {
  const pixels = typeof value === 'number' ? value : NaN;
  if (!Number.isFinite(pixels)) {
    throw aotError(
      `AOT: <Svg ${field}> must be a number of pixels${scaled ? '' : ' or a percentage'} ` +
        `(got ${JSON.stringify(value) ?? String(value)})`,
      scaled
        ? 'a viewBox or a baked source is scaled to the box, so the box takes pixels; size a wrapping ' +
            "<View> with style={{width: '50%'}} for a share of the parent."
        : `write the pixels as a number (${field}={100}): Flow A ignores a numeric string here.`,
    );
  }

  return pixels;
}

/**
 * The ERProps writes for an <Svg> width or height: pixels as an int16, or a percentage of the parent when the
 * drawing is not scaled to the box. Absent, it writes nothing.
 *
 * @param field  Which one.
 * @param value  Its folded value, or the state-driven one.
 * @param scaled  Whether the drawing is scaled to the box (a viewBox or a baked source).
 *
 * @returns The writes.
 */
function svgBoxSize(
  field: 'width' | 'height',
  value: unknown,
  scaled: boolean,
): StaticAssign[] {
  if (value == null || svgBootSize(field, value)) return [];
  if (isBoot(value)) return svgBoxSize(field, foldBoot(value), scaled);
  if (isDynamicAttr(value)) {
    throw aotError(
      `AOT: a state-driven <Svg ${field}> is not supported`,
      `give the <Svg> a constant ${field} and drive the size of a wrapping <View> from state instead.`,
    );
  }

  if (!scaled && typeof value === 'string' && value.trim().endsWith('%')) {
    return lowerStyle({[field]: value});
  }

  return [
    {
      field,
      value: Math.round(svgPixels(field, value, scaled)),
    },
  ];
}

/**
 * An <Svg> width or height worked out at boot, as its runtime write. A drawing with a viewBox then scales on every
 * panel with no layout of its own. A value boot can't work out is taken at the build's panel.
 *
 * @param field  Which one.
 * @param value  Its value.
 *
 * @returns The write, or null when it is not one.
 */
function svgBootSize(
  field: 'width' | 'height',
  value: unknown,
): DynAssign | null {
  return isBoot(value) ? bootStyleAssign(field, value) : null;
}

/**
 * An <Svg>'s width and height, as the writes of those worked out at boot.
 *
 * @param width  Its width.
 * @param height  Its height.
 *
 * @returns The writes.
 */
function svgBootSizes(width: unknown, height: unknown): DynAssign[] {
  return [svgBootSize('width', width), svgBootSize('height', height)].filter(
    (size): size is DynAssign => size !== null,
  );
}

/**
 * Lowers the box an <Svg> draws in: a node of `type` with its size, then its static style, its animated
 * transform and its refs. No state drives the box: it is not a node the update rewrites.
 *
 * @param nodeId  The node's variable.
 * @param type  The engine node type: a vector node, or an image for a rasterized .svg.
 * @param width  Its width writes (see svgBoxSize).
 * @param height  Its height writes.
 * @param openingElement  The <Svg>'s opening tag, for its style and ref.
 * @param scope  Compile-time constants in reach.
 * @param environment  The expression environment.
 * @param bootSizes  Its width and height writes worked out at boot.
 *
 * @returns The node, with its refs wired.
 */
function lowerSvgBox(
  nodeId: string,
  type: string,
  width: StaticAssign[],
  height: StaticAssign[],
  openingElement: t.JSXOpeningElement,
  scope: Scope,
  environment: Env,
  bootSizes: DynAssign[] = [],
): IrNode {
  const {staticAssigns, dynamicAssigns, binds} = collectStyleAssigns(
    openingElement,
    scope,
    environment,
    elementSpec('Svg')!,
  );
  // A value worked out at boot is set once, so only one state drives is refused.
  const readsOnlyBoot = (assign: DynAssign): boolean =>
    (assign.value.kind === 'dimension' || assign.value.kind === 'opacity') &&
    settledOf(assign.value.value)?.kind === 'boot';
  if (!dynamicAssigns.every(readsOnlyBoot)) {
    throw aotError(
      'AOT: a state-driven style on an <Svg> is not supported',
      'put the changing style on a wrapping <View>: <View style={…}><Svg …/></View>.',
    );
  }

  // A style width or height wins over the attribute, in pixels or as a percentage of the parent.
  const size = [...width, ...height].filter(
    assign =>
      !styleWrites(staticAssigns, [], assign.field.replace(/_percent$/, '')),
  );
  const node: IrNode = {
    id: nodeId,
    location: currentLoc(),
    type,
    constantProps: [...size, ...staticAssigns],
    stateProps: [
      ...bootSizes.filter(
        assign =>
          !styleWrites(
            staticAssigns,
            dynamicAssigns,
            assign.field.replace(/_percent$/, ''),
          ),
      ),
      ...dynamicAssigns,
    ],
    placeholder: null,
    text: null,
    image: null,
    spans: null,
    wiring: [
      ...binds.map((bind, index) => ({kind: 'bind' as const, bind, index})),
      ...lowerRefBinds(openingElement, environment).map(slot => ({
        kind: 'ref' as const,
        slot,
      })),
    ],
    children: [],
  };
  checkInterpolations(node);
  return node;
}

/**
 * <Svg> → ER_NODE_VECTOR. Static subtree → is a baked const op-tape, converted by flattenSvg (the same
 * converter Flow A uses); any state-driven attr → a symbolic op-tape rebuilt at
 * build time and on every update. <Svg source> draws a .svg baked ahead of time (bakeSvgArtifacts).
 *
 * @param element  The <Svg>.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 * @param options  How the element is placed.
 *
 * @returns The node.
 */
export function emitSvg(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
  options: EmitOptions,
): IrNode {
  if (options.show) {
    throw new Error(
      'AOT: an <Svg> inside a dynamic conditional is not yet supported',
    );
  }

  // Pick the path: a baked .svg file, a state-driven tape, or a static one.
  const sourceAttribute = element.openingElement.attributes.find(
    (candidate): candidate is t.JSXAttribute =>
      candidate.type === 'JSXAttribute' &&
      candidate.name &&
      candidate.name.name === 'source',
  );
  if (sourceAttribute)
    return emitSvgSource(element, sourceAttribute, scope, output, environment);

  return svgHasDynamic(element, scope)
    ? emitSvgDynamic(element, scope, output, environment, state)
    : emitSvgStatic(element, scope, output, environment);
}

/**
 * How much a drawing scales along one side: the box over the viewBox.
 *
 * @param size  The box's side: a number, or a boot value.
 * @param view  The viewBox's side.
 *
 * @returns The scale, or null when the side has no value boot can work out.
 */
function viewBoxScale(size: unknown, view: number): IrTapeValue | null {
  if (!isBoot(size)) return tapeNumber(svgNum(size, view) / view);
  const slot = bootSlotOf(size);
  if (!slot || (slot.type.kind !== 'int' && slot.type.kind !== 'float')) {
    return null;
  }

  return {
    kind: 'float',
    value: {
      kind: 'floatDivide',
      left: {kind: 'boot', slot, type: slot.type},
      right: {kind: 'number', value: view, type: FLOAT},
      type: FLOAT,
    },
  };
}

/**
 * A viewBox drawing sized at boot: drawn in the viewBox's units and scaled once, from the real size, as a drawing
 * state drives is rebuilt. So it takes one layout on every panel, where baking it would take one layout per size.
 *
 * @param element  The <Svg>.
 * @param svgElement  The <Svg>, folded.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 *
 * @returns The node, or null when no viewBox drawing is sized at boot.
 */
function emitSvgScaled(
  element: t.JSXElement,
  svgElement: SvgElement,
  scope: Scope,
  output: Out,
  environment: Env,
): IrNode | null {
  const {width, height, viewBox} = svgElement.props;
  if (!viewBox || !width || !height || (!isBoot(width) && !isBoot(height))) {
    return null;
  }

  const view = String(viewBox)
    .trim()
    .split(/[\s,]+/)
    .map(parseFloat);
  if (view.length !== 4 || !(view[2] > 0) || !(view[3] > 0)) return null;
  const scaleX = viewBoxScale(width, view[2]);
  const scaleY = viewBoxScale(height, view[3]);
  if (!scaleX || !scaleY) return null;

  const {ops, paints, gradients} = flattenSvg({
    ...svgElement.props,
    width: view[2],
    height: view[3],
  });
  const sideX: IrTapeValue = {kind: 'local', name: 'scale_x'};
  const sideY: IrTapeValue = {kind: 'local', name: 'scale_y'};
  const along = (value: number, side: IrTapeValue): IrTapeValue =>
    tapeChain(tapeNumber(value), ['*', side]);
  const operationNames: Record<number, string> = {
    [VOP_MOVE]: 'ER_VECTOR_OP_MOVE',
    [VOP_LINE]: 'ER_VECTOR_OP_LINE',
    [VOP_QUAD]: 'ER_VECTOR_OP_QUAD',
    [VOP_CUBIC]: 'ER_VECTOR_OP_CUBIC',
    [VOP_ARC]: 'ER_VECTOR_OP_ARC',
  };
  const tape: IrTapeValue[] = [];
  for (let index = 0; index < ops.length;) {
    const operation = ops[index++];
    if (operation === VOP_SHAPE) {
      tape.push(tapeOp('ER_VECTOR_OP_SHAPE'), tapeNumber(ops[index++]));
    } else if (operation === VOP_ARC) {
      tape.push(
        tapeOp(operationNames[operation]),
        along(ops[index++], sideX),
        along(ops[index++], sideY),
        along(ops[index++], sideX),
        tapeNumber(ops[index++]),
        tapeNumber(ops[index++]),
      );
    } else if (operation === VOP_MOVE || operation === VOP_LINE) {
      tape.push(
        tapeOp(operationNames[operation]),
        along(ops[index++], sideX),
        along(ops[index++], sideY),
      );
    } else if (operation === VOP_QUAD || operation === VOP_CUBIC) {
      tape.push(tapeOp(operationNames[operation]));
      for (let point = 0; point < (operation === VOP_QUAD ? 2 : 3); point++) {
        tape.push(along(ops[index++], sideX), along(ops[index++], sideY));
      }
    } else {
      tape.push(tapeOp('ER_VECTOR_OP_CLOSE'));
    }
  }

  // A stroke scales by the two sides' mean, as flattenSvg's does.
  const sideStroke = tapeChain(tapeChain(sideX, ['+', sideY]), [
    '*',
    tapeNumber(0.5),
  ]);
  const nodeId = `n${output.allocateNodeId()}`;
  const vector: IrStateDrivenVector = {
    id: output.svgCount++,
    locals: [
      {name: 'scale_x', value: scaleX},
      {name: 'scale_y', value: scaleY},
    ],
    tape,
    paints: paintsOf(paints).map(paint => ({
      fill: {kind: 'static', argb: paint.fill},
      stroke: {kind: 'static', argb: paint.stroke},
      strokeWidth: tapeChain(tapeNumber(paint.strokeWidth), ['*', sideStroke]),
      miter: paint.miter,
      cap: paint.cap,
      join: paint.join,
      fillRule: paint.fillRule,
      fillGradient: {index: paint.fillGradient, when: null},
      strokeGradient: {index: paint.strokeGradient, when: null},
    })),
    gradients: (gradients as IrVectorTables['gradients']).map(gradient => ({
      type: gradient.type,
      stops: (gradient.stops || []).map(stop => ({
        color: {kind: 'static' as const, argb: stop.color >>> 0},
        offset: tapeNumber(stop.offset),
      })),
      ax: tapeNumber(gradient.ax || 0),
      ay: tapeNumber(gradient.ay || 0),
      bx: tapeNumber(gradient.bx || 0),
      by: tapeNumber(gradient.by || 0),
      r: tapeNumber(gradient.r || 0),
    })),
  };
  output.vectors.push({kind: 'drawing', vector});

  const node = lowerSvgBox(
    nodeId,
    'ER_NODE_VECTOR',
    svgBoxSize('width', width, true),
    svgBoxSize('height', height, true),
    element.openingElement,
    scope,
    environment,
    svgBootSizes(width, height),
  );
  node.wiring.push({kind: 'stateDrivenVector', vector});
  return node;
}

/**
 * Static <Svg>: reuse flattenSvg (full feature set: viewBox, <G>, Path) and bake const arrays.
 *
 * @param element  The <Svg>.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 *
 * @returns The node.
 */
function emitSvgStatic(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
): IrNode {
  // Flatten the subtree and bake its ops and paints into const tables.
  const svgElement = jsxToSvgElement(element, scope)!;
  const scaled = emitSvgScaled(element, svgElement, scope, output, environment);
  if (scaled) return scaled;
  const {ops, paints, gradients} = flattenSvg({
    ...svgElement.props,
    width: buildValueOf(svgElement.props.width),
    height: buildValueOf(svgElement.props.height),
  });
  const nodeId = `n${output.allocateNodeId()}`;
  const tables: IrVectorTables = {
    id: output.svgCount++,
    ops: Array.from(ops),
    paints: paintsOf(paints),
    gradients,
  };
  checkVectorTables(tables);
  output.vectors.push({kind: 'tables', tables});

  // Create the vector node and point it at the tables.
  const node = lowerSvgBox(
    nodeId,
    'ER_NODE_VECTOR',
    svgBoxSize(
      'width',
      svgElement.props.width,
      svgElement.props.viewBox != null,
    ),
    svgBoxSize(
      'height',
      svgElement.props.height,
      svgElement.props.viewBox != null,
    ),
    element.openingElement,
    scope,
    environment,
    svgBootSizes(svgElement.props.width, svgElement.props.height),
  );
  node.wiring.push({kind: 'vectorTables', tables});
  return node;
}

/**
 * Baked <Svg source={importedSvg}>: the CLI pre-bakes the .svg to a vector artifact (ops/paints/GRADIENTS) *
 * (bakeSvgArtifacts → options.svgArtifacts) since compilePackage is I/O-free. Scaled to the static
 * width/height at compile time, then emitted as const tables. This is the path that carries gradients into
 * Flow B.
 *
 * @param element  The <Svg>.
 * @param sourceAttribute  Its `source` attribute.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 *
 * @returns The node.
 */
function emitSvgSource(
  element: t.JSXElement,
  sourceAttribute: t.JSXAttribute,
  scope: Scope,
  output: Out,
  environment: Env,
): IrNode {
  // Resolve the source to its imported .svg's baked artifact.
  const expression =
    sourceAttribute.value &&
    sourceAttribute.value.type === 'JSXExpressionContainer'
      ? sourceAttribute.value.expression
      : null;
  if (!expression || expression.type !== 'Identifier') {
    throw new Error(
      'AOT: <Svg source> must reference an imported .svg (source={importedSvg})',
    );
  }

  const svgImport = environment.svgImports.get(expression.name);
  if (!svgImport) {
    throw new Error(
      `AOT: <Svg source={${expression.name}}> — no matching \`import ${expression.name} from '...svg'\``,
    );
  }

  const artifact = environment.svgArtifacts[svgImport.name];
  if (!artifact) {
    throw new Error(
      `AOT: vector artifact for "${svgImport.name}" was not baked (internal: opts.svgArtifacts missing it)`,
    );
  }

  // Scale the artifact at compile time to a static width/height, defaulting to its intrinsic size.
  const numericAttr = (
    attributeName: 'width' | 'height',
    fallback: number,
  ): number => {
    const attribute = element.openingElement.attributes.find(
      (candidate): candidate is t.JSXAttribute =>
        candidate.type === 'JSXAttribute' &&
        candidate.name &&
        candidate.name.name === attributeName,
    );
    if (!attribute) return fallback;

    let value: unknown = true; // a bare attribute
    if (attribute.value?.type === 'StringLiteral') {
      value = attribute.value.value;
    } else if (attribute.value?.type === 'JSXExpressionContainer') {
      try {
        value = evalStatic(attribute.value.expression, scope);
      } catch {
        throw at(
          aotError('AOT: <Svg source> width/height must be a static number'),
          attribute,
        );
      }
    }

    try {
      return svgPixels(attributeName, value, true);
    } catch (error) {
      throw at(error as AotError, attribute);
    }
  };

  // The drawing is scaled to the box, so a style size wins over the attribute, as in Flow A.
  const {staticAssigns: style} = collectStyleAssigns(
    element.openingElement,
    scope,
    environment,
    elementSpec('Svg')!,
  );
  const boxSize = (field: 'width' | 'height', fallback: number): number => {
    if (style.some(assign => assign.field === `${field}_percent`)) {
      throw aotError(
        `AOT: a percentage style ${field} on an <Svg source> is not supported`,
        'the drawing is scaled to the box at build time, when the parent size is unknown; give the ' +
          `<Svg> its ${field} in pixels.`,
      );
    }

    const styled = style.find(assign => assign.field === field);
    return styled ? Number(styled.value) : numericAttr(field, fallback);
  };

  // Raster fallback: a .svg with unsupported features was baked to a PNG; register it and emit an Image node.
  const width = boxSize('width', artifact.width);
  const height = boxSize('height', artifact.height);
  if (artifact.kind === 'raster') {
    if (artifact.png) {
      output.images.set(artifact.name, artifact.png);
    }

    const node = lowerSvgBox(
      `n${output.allocateNodeId()}`,
      NODE_TYPES.Image,
      svgBoxSize('width', width, true),
      svgBoxSize('height', height, true),
      element.openingElement,
      scope,
      environment,
    );
    node.image = {kind: 'static', name: artifact.name};
    return node;
  }

  // Scale the vector artifact to the box and bake its ops, paints and gradients into const tables.
  const scaled = scaleVectorArtifact(artifact, width, height);
  const nodeId = `n${output.allocateNodeId()}`;
  const tables: IrVectorTables = {
    id: output.svgCount++,
    ops: Array.from(scaled.ops),
    paints: paintsOf(scaled.paints),
    gradients: scaled.gradients || [],
  };
  checkVectorTables(tables);
  output.vectors.push({kind: 'tables', tables});

  // Create the vector node and point it at the tables.
  const node = lowerSvgBox(
    nodeId,
    'ER_NODE_VECTOR',
    svgBoxSize('width', width, true),
    svgBoxSize('height', height, true),
    element.openingElement,
    scope,
    environment,
  );
  node.wiring.push({kind: 'vectorTables', tables});
  return node;
}

/**
 * State-driven <Svg> (flat Arc/Circle/Rect/Line/static-Path; no viewBox/<G>): emit a mutable op-tape
 * that is recomputed from state, at build and on each update.
 *
 * @param element  The <Svg>.
 * @param scope  Compile-time constants in reach.
 * @param output  Everything emitted so far.
 * @param environment  The expression environment.
 * @param state  The component's state table.
 *
 * @returns The node.
 */
function emitSvgDynamic(
  element: t.JSXElement,
  scope: Scope,
  output: Out,
  environment: Env,
  state: StateTable,
): IrNode {
  // The root's attributes give the box; this path has no viewBox transform.
  const rootAttributes = svgAttrs(element.openingElement, scope, environment);
  if (rootAttributes.viewBox != null) {
    throw new Error(
      'AOT: a viewBox on a state-driven <Svg> is not yet supported — size shapes in the width/height space',
    );
  }

  // Lower each shape to tape slots (after an ER_VECTOR_OP_SHAPE marker naming its paint) and its paint.
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

    const attributes = svgAttrs(child.openingElement, scope, environment);
    // The builder is this <Svg>'s own function, so the shape index alone keeps a local's name unique.
    const shape = lowerShape(attributes, `s${shapePaints.length}`);
    if (!shape.entries.length) continue;
    locals.push(...shape.locals);
    tape.push(
      tapeOp('ER_VECTOR_OP_SHAPE'),
      tapeNumber(shapePaints.length),
      ...shape.entries,
    );
    // The root's paint cascades to each shape, as flattenSvg does.
    const paintAttributes = {...attributes};
    for (const name of PAINT_PROPS)
      paintAttributes[name] ??= rootAttributes[name];
    shapePaints.push(lowerShapePaint(paintAttributes, environment, scope));
  }

  // Allocate the node and this <Svg>'s table names.
  const nodeId = `n${output.allocateNodeId()}`;
  const svgId = output.svgCount++;

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

  // Declare the tables and the builder, which rewrites the tape (and anything in the paints state drives).
  output.vectors.push({kind: 'drawing', vector});

  // Create the vector node, build its tape once, and register it for the update to rebuild.
  const node = lowerSvgBox(
    nodeId,
    'ER_NODE_VECTOR',
    svgBoxSize('width', rootAttributes.width, false),
    svgBoxSize('height', rootAttributes.height, false),
    element.openingElement,
    scope,
    environment,
    svgBootSizes(rootAttributes.width, rootAttributes.height),
  );
  node.wiring.push({kind: 'stateDrivenVector', vector});

  return node;
}
