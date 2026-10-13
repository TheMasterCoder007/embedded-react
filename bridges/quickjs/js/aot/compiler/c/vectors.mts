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
import type {IrVectorPaint, IrVectorTables} from '../ir/vectors.mts';
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
