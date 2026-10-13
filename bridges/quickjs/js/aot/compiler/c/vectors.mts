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

import {floatLit} from '../c-syntax.mts';
import {printCondition, printExpr} from './expressions.mts';
import {printPropValue} from './nodes.mts';
import type {
  IrDrawingGradient,
  IrDrawingPaint,
  IrGradientRef,
  IrPaintColor,
  IrStateDrivenVector,
  IrTapeValue,
  IrVectorPaint,
  IrVectorTables,
} from '../ir/vectors.mts';
import type {IrUpdateVector} from '../ir/statements.mts';
import type {VectorGradient} from '../types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The most color stops the engine's ERVectorGradient holds; more are dropped. */
const MAX_GRADIENT_STOPS = 8;

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * One paint as an ERVectorPaint initializer.
 *
 * @param paint  The paint.
 *
 * @returns The C.
 */
export const printVectorPaint = (paint: IrVectorPaint): string =>
  `{ .fill = ${paint.fill}u, .stroke = ${paint.stroke}u, .stroke_w = ` +
  `${floatLit(paint.strokeWidth)}, .miter = ${floatLit(paint.miter)}, .cap = ${paint.cap}, .join ` +
  `= ${paint.join}, .fill_rule = ${paint.fillRule}, .fill_grad = ` +
  `${paint.fillGradient}, .stroke_grad = ${paint.strokeGradient} }`;

/**
 * One gradient as an ERVectorGradient initializer. Stops fill the C ERGradientStop[] positionally; geometry
 * means, per type: linear axis (ax, ay)->(bx, by); radial center (ax, ay) and radius r; conic center (ax, ay)
 * and start angle r.
 *
 * @param gradient  The gradient.
 *
 * @returns The C.
 */
export function printVectorGradient(gradient: VectorGradient): string {
  const allStops = gradient.stops || [];
  const keptStops =
    allStops.length > MAX_GRADIENT_STOPS
      ? allStops.slice(0, MAX_GRADIENT_STOPS)
      : allStops;
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
 * A baked drawing's const tables: its op-tape, its paints, and its gradients when it has any. An empty tape
 * has none.
 *
 * @param tables  The drawing.
 *
 * @returns The C declarations.
 */
export function printVectorTables(tables: IrVectorTables): string[] {
  if (!tables.ops.length) {
    return [];
  }

  const {id} = tables;
  const declarations = [
    `static const float s_svg${id}_ops[] = {\n    ${tables.ops.map(op => floatLit(op)).join(', ')}\n};`,
    `static const ERVectorPaint s_svg${id}_paints[] = {\n` +
      `${tables.paints.map(paint => '    ' + printVectorPaint(paint)).join(',\n')}\n};`,
  ];
  if (tables.gradients.length) {
    declarations.push(
      `static const ERVectorGradient s_svg${id}_grads[] = ` +
        `{\n${tables.gradients.map(gradient => '    ' + printVectorGradient(gradient)).join(',\n')}\n};`,
    );
  }

  return declarations;
}

/**
 * Points a vector node at its baked tables; an empty tape leaves the node blank.
 *
 * @param nodeId  The node's variable.
 * @param tables  The drawing.
 *
 * @returns The C lines.
 */
export function printVectorOps(
  nodeId: string,
  tables: IrVectorTables,
): string[] {
  if (!tables.ops.length) {
    return [];
  }

  const {id, gradients} = tables;
  const gradsRef = gradients.length ? `s_svg${id}_grads` : 'NULL';
  return [
    `    er_node_set_vector_ops(${nodeId}, s_svg${id}_ops, ${tables.ops.length}, ` +
      `s_svg${id}_paints, ${tables.paints.length}, ${gradsRef}, ${gradients.length});`,
  ];
}

/**
 * One op-tape slot as the C float it is.
 *
 * @param value  The slot.
 *
 * @returns The C.
 */
export function printTapeValue(value: IrTapeValue): string {
  switch (value.kind) {
    case 'op':
      return value.op;
    case 'number':
      return floatLit(value.value);
    case 'float':
      return `(float)(${printExpr(value.value)})`;
    case 'local':
      return value.name;
    case 'chain':
      return `(${[
        printTapeValue(value.first),
        ...value.rest.map(term => `${term.op} ${printTapeValue(term.value)}`),
      ].join(' ')})`;
    case 'radians':
      return `((${printTapeValue(value.degrees)} - 90.0f) * (float)M_PI / 180.0f)`;
    case 'fullTurn':
      return '(2.0f * (float)M_PI)';
    case 'clampRadius':
      return `fminf(fmaxf(${printTapeValue(value.radius)}, 0.0f), (${printTapeValue(value.side)}) * 0.5f)`;
  }
}

/**
 * A paint color as C: a constant ARGB, or the color state chooses.
 *
 * @param color  The color.
 *
 * @returns The C.
 */
const printPaintColor = (color: IrPaintColor): string =>
  color.kind === 'static' ? `${color.argb}u` : printPropValue(color.value);

/**
 * A paint's gradient index as C: 0 for none, the index, or the index while its condition holds.
 *
 * @param ref  The gradient.
 *
 * @returns The C.
 */
function printGradientRef(ref: IrGradientRef): string {
  if (!ref.index || !ref.when) {
    return String(ref.index);
  }

  const test = printCondition(ref.when.test);
  return `((${ref.when.negate ? `!(${test})` : test}) ? ${ref.index} : 0)`;
}

/**
 * Whether a gradient follows state: a stop color or a geometry field does.
 *
 * @param gradient  The gradient.
 *
 * @returns Whether it does.
 */
const isGradientStateDriven = (gradient: IrDrawingGradient): boolean =>
  gradient.stops.some(
    stop => stop.color.kind === 'state' || stop.offset.kind !== 'number',
  ) ||
  [gradient.ax, gradient.ay, gradient.bx, gradient.by, gradient.r].some(
    field => field.kind !== 'number',
  );

/**
 * Whether a paint follows state: a color, the stroke width, or a gradient it uses (or its condition) does.
 *
 * @param paint  The paint.
 * @param gradients  The drawing's gradients.
 *
 * @returns Whether it does.
 */
function isPaintStateDriven(
  paint: IrDrawingPaint,
  gradients: IrDrawingGradient[],
): boolean {
  const followsState = (ref: IrGradientRef) =>
    ref.index > 0 &&
    (ref.when !== null || isGradientStateDriven(gradients[ref.index - 1]));
  return (
    paint.fill.kind === 'state' ||
    paint.stroke.kind === 'state' ||
    paint.strokeWidth.kind !== 'number' ||
    followsState(paint.fillGradient) ||
    followsState(paint.strokeGradient)
  );
}

/**
 * A paint's ERVectorPaint fields, in struct order.
 *
 * @param paint  The paint.
 *
 * @returns Each field's name and C value.
 */
const paintFields = (paint: IrDrawingPaint): [string, string][] => [
  ['fill', printPaintColor(paint.fill)],
  ['stroke', printPaintColor(paint.stroke)],
  ['stroke_w', printTapeValue(paint.strokeWidth)],
  ['miter', floatLit(paint.miter)],
  ['cap', String(paint.cap)],
  ['join', String(paint.join)],
  ['fill_rule', String(paint.fillRule)],
  ['fill_grad', printGradientRef(paint.fillGradient)],
  ['stroke_grad', printGradientRef(paint.strokeGradient)],
];

/**
 * A gradient's ERVectorGradient fields after its stops, in struct order.
 *
 * @param gradient  The gradient.
 *
 * @returns Each field's name and C value.
 */
const gradientGeometry = (gradient: IrDrawingGradient): [string, string][] =>
  (['ax', 'ay', 'bx', 'by', 'r'] as const).map(field => [
    field,
    printTapeValue(gradient[field]),
  ]);

/**
 * A state-driven drawing's tables and its build_svg<id>() function. The op-tape is always mutable; the
 * gradients and paints are const tables unless state drives something in them, in which case the builder
 * rewrites them too.
 *
 * @param vector  The drawing.
 *
 * @returns The table declarations, and the builder function.
 */
export function printStateDrivenVector(vector: IrStateDrivenVector): {
  data: string[];
  builder: string;
} {
  const {id, gradients, paints} = vector;
  const hasStatePaint = paints.some(paint =>
    isPaintStateDriven(paint, gradients),
  );
  const hasStateGradient = gradients.some(isGradientStateDriven);

  const data = [`static float s_svg${id}_ops[${vector.tape.length}];`];
  if (gradients.length) {
    if (hasStateGradient) {
      data.push(
        `static ERVectorGradient s_svg${id}_grads[${gradients.length}];`,
      );
    } else {
      const inits = gradients.map(gradient => {
        const stops = gradient.stops
          .map(
            stop =>
              `{ ${printPaintColor(stop.color)}, ${printTapeValue(stop.offset)} }`,
          )
          .join(', ');
        const geometry = gradientGeometry(gradient)
          .map(([field, value]) => `.${field} = ${value}`)
          .join(', ');
        return (
          `    { .type = ${gradient.type}, .stop_count = ${gradient.stops.length}, ` +
          `.stops = { ${stops} }, ${geometry} }`
        );
      });
      data.push(
        `static const ERVectorGradient s_svg${id}_grads[] = {\n${inits.join(',\n')}\n};`,
      );
    }
  }

  if (hasStatePaint) {
    data.push(`static ERVectorPaint s_svg${id}_paints[${paints.length}];`);
  } else {
    const inits = paints.map(
      paint =>
        `    { ${paintFields(paint)
          .map(([field, value]) => `.${field} = ${value}`)
          .join(', ')} }`,
    );
    data.push(
      `static const ERVectorPaint s_svg${id}_paints[] = {\n${inits.join(',\n')}\n};`,
    );
  }

  // The builder: the locals, every tape slot, then the paints and gradients state drives.
  const lines = [
    ...vector.locals.map(
      local =>
        `    const float ${local.name} = ${printTapeValue(local.value)};`,
    ),
    ...vector.tape.map(
      (value, slot) =>
        `    s_svg${id}_ops[${slot}] = ${printTapeValue(value)};`,
    ),
  ];
  if (hasStatePaint) {
    paints.forEach((paint, paintIndex) => {
      for (const [field, value] of paintFields(paint)) {
        lines.push(`    s_svg${id}_paints[${paintIndex}].${field} = ${value};`);
      }
    });
  }
  if (hasStateGradient) {
    gradients.forEach((gradient, entry) => {
      const prefix = `    s_svg${id}_grads[${entry}]`;
      lines.push(
        `${prefix}.type = ${gradient.type};`,
        `${prefix}.stop_count = ${gradient.stops.length};`,
      );
      gradient.stops.forEach((stop, stopIndex) => {
        lines.push(
          `${prefix}.stops[${stopIndex}].color = ${printPaintColor(stop.color)};`,
          `${prefix}.stops[${stopIndex}].position = ${printTapeValue(stop.offset)};`,
        );
      });
      for (const [field, value] of gradientGeometry(gradient)) {
        lines.push(`${prefix}.${field} = ${value};`);
      }
    });
  }

  return {
    data,
    builder: `static void build_svg${id}(void)\n{\n${lines.join('\n')}\n}`,
  };
}

/**
 * Builds a state-driven drawing once, points its node at the tables, and keeps the node's handle for app_update.
 *
 * @param nodeId  The node's variable.
 * @param vector  The drawing.
 *
 * @returns The C lines.
 */
export function printStateDrivenVectorWiring(
  nodeId: string,
  vector: IrStateDrivenVector,
): string[] {
  const {id, gradients} = vector;
  return [
    `    build_svg${id}();`,
    `    er_node_set_vector_ops(${nodeId}, s_svg${id}_ops, ${vector.tape.length}, ` +
      `s_svg${id}_paints, ${vector.paints.length}, ` +
      `${gradients.length ? `s_svg${id}_grads` : 'NULL'}, ${gradients.length});`,
    `    s_${nodeId} = ${nodeId};`,
  ];
}

/**
 * An updateVector call's file-scope tables: its mutable op-tape and its const paints.
 *
 * @param update  The call.
 *
 * @returns The C declarations.
 */
export const printUpdateVectorData = (update: IrUpdateVector): string[] => [
  `static float s_uv${update.id}_ops[${update.tape.length}];`,
  `static const ERVectorPaint s_uv${update.id}_paints[] = ` +
    `{\n${update.paints.map(paint => '    ' + printVectorPaint(paint)).join(',\n')}\n};`,
];

/**
 * An updateVector call as statements: its locals, every tape slot, the hand-over to the node, and the dirty
 * hint. A float edge may be NaN, so it goes through app_vector_dirty, which drops such a hint (a zero-width
 * hint would paint nothing).
 *
 * @param update  The call.
 *
 * @returns The C lines, without their indent.
 */
export function printUpdateVector(update: IrUpdateVector): string[] {
  const {id, node, tape} = update;
  const lines = [
    ...update.locals.map(
      local => `const float ${local.name} = ${printTapeValue(local.value)};`,
    ),
    ...tape.map(
      (value, slot) => `s_uv${id}_ops[${slot}] = ${printTapeValue(value)};`,
    ),
    `er_node_set_vector_ops(${node}, s_uv${id}_ops, ${tape.length}, ` +
      `s_uv${id}_paints, ${update.paints.length}, NULL, 0);`,
  ];
  if (update.dirty) {
    const dirtyFn = update.dirty.some(edge => edge.cType === 'float')
      ? 'app_vector_dirty'
      : 'er_node_set_vector_dirty_rect';
    lines.push(
      `${dirtyFn}(${node}, ${update.dirty.map(printExpr).join(', ')});`,
    );
  }

  return lines;
}
