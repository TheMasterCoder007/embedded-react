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

import type {VectorGradient} from '../types.mts';
import type {IrExpr} from './expressions.mts';
import type {IrPropValue} from './nodes.mts';
import type {IrUpdateVector} from './statements.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Interfaces
 ---------------------------------------------------------------------------------------------------------------------*/

/** One shape's paint, as the engine stores it. */
export interface IrVectorPaint {
  /** ARGB fill and stroke colors. */
  fill: number;
  stroke: number;
  strokeWidth: number;
  miter: number;
  /** The cap, join and fill rule, as the engine numbers them. */
  cap: number;
  join: number;
  fillRule: number;
  /** 1-based indexes into the drawing's gradient table; 0 paints a solid color. */
  fillGradient: number;
  strokeGradient: number;
}

/**
 * A drawing baked at build time into const tables: an op-tape (opcodes and coordinates, as floats), one paint
 * per shape, and the gradients the paints refer to. An empty tape draws nothing and has no tables.
 */
export interface IrVectorTables {
  /** Numbers the drawing's tables (`s_svg<id>_…`). */
  id: number;
  ops: number[];
  paints: IrVectorPaint[];
  gradients: VectorGradient[];
}

/**
 * One float slot of a state-driven drawing's op-tape, recomputed on every update. Each combining form is one
 * piece of shape geometry: a sum or difference of coordinates, a scaled handle, an angle, a clamped radius.
 */
export type IrTapeValue =
  /** An opcode, an ER_VOP_* name. */
  | {kind: 'op'; op: string}
  | {kind: 'number'; value: number}
  /** A state-driven attribute, read as a float. */
  | {kind: 'float'; value: IrExpr}
  /** A local the drawing declares ahead of its tape. */
  | {kind: 'local'; name: string}
  /** `first`, then each operator and operand in turn, left to right. */
  | {
      kind: 'chain';
      first: IrTapeValue;
      rest: {op: '+' | '-' | '*'; value: IrTapeValue}[];
    }
  /** An angle in degrees clockwise from the top, as the engine's radians clockwise from 3 o'clock. */
  | {kind: 'radians'; degrees: IrTapeValue}
  /** A whole turn, in radians. */
  | {kind: 'fullTurn'}
  /** A corner radius clamped to half the side it rounds. */
  | {kind: 'clampRadius'; radius: IrTapeValue; side: IrTapeValue};

/** A shape's tape slots, and the locals declared ahead of them (a rounded rect's radii). */
export interface IrGeometry {
  entries: IrTapeValue[];
  locals: {name: string; value: IrTapeValue}[];
}

/** A paint color: a constant ARGB, or one state chooses (a color, or a ternary of them). */
export type IrPaintColor =
  | {kind: 'static'; argb: number}
  | {kind: 'state'; value: IrPropValue};

/** A gradient of a state-driven drawing: any stop color or geometry field may follow state. */
export interface IrDrawingGradient {
  /** 1 linear, 2 radial, 3 conic. */
  type: number;
  stops: {color: IrPaintColor; offset: IrTapeValue}[];
  ax: IrTapeValue;
  ay: IrTapeValue;
  bx: IrTapeValue;
  by: IrTapeValue;
  r: IrTapeValue;
}

/** A paint's gradient: a 1-based index into the drawing's table (0 for none), used only while `when` holds. */
export interface IrGradientRef {
  index: number;
  when: {test: IrExpr; negate: boolean} | null;
}

/** One shape's paint in a state-driven drawing. */
export interface IrDrawingPaint {
  fill: IrPaintColor;
  stroke: IrPaintColor;
  strokeWidth: IrTapeValue;
  miter: number;
  cap: number;
  join: number;
  fillRule: number;
  fillGradient: IrGradientRef;
  strokeGradient: IrGradientRef;
}

/**
 * A drawing state drives: its op-tape is rebuilt by build_svg<id>() at boot and on every update, and so are its
 * paints and gradients when anything in them follows state. Otherwise they stay const tables.
 */
export interface IrStateDrivenVector {
  /** Numbers the drawing's tables and builder (`s_svg<id>_…`, `build_svg<id>`). */
  id: number;
  locals: {name: string; value: IrTapeValue}[];
  tape: IrTapeValue[];
  paints: IrDrawingPaint[];
  gradients: IrDrawingGradient[];
}

/**
 * A drawing's file-scope tables, in the order the app declares them: baked tables, a state-driven drawing
 * (whose builder is declared with them), or an updateVector call's tape and paints.
 */
export type IrVectorData =
  | {kind: 'tables'; tables: IrVectorTables}
  | {kind: 'drawing'; vector: IrStateDrivenVector}
  | {kind: 'updateVector'; update: IrUpdateVector};
