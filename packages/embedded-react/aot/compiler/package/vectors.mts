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

/*
 * <Svg> drawings in the package output, drawn as the C output draws them.
 *
 * A drawing baked at build time is a tape, committed when its node is built. A drawing state drives is a tape
 * too, its constant floats and paints in place: the update copies it into the scratch, works out each float,
 * paint field and gradient field state drives, in the type C works it out in, and commits it, as build_svg<id>()
 * and er_node_set_vector_ops() do. An updateVector call does the same from the code that calls it, then sets the
 * damage hint.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Imports
 ---------------------------------------------------------------------------------------------------------------------*/

import {aotError} from '../diagnostics.mts';
import {MAX_GRADIENT_STOPS} from '../c/vectors.mts';
import {GradientField, MathFn, PaintField} from './format.mts';
import type {Arena, Slot} from './arena.mts';
import type {Register} from './bytecode.mts';
import type {Codegen} from './codegen.mts';
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
import type {ProgramGradient, ProgramPaint, ProgramTape} from './writer.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** The engine's vector opcodes, as their ER_VOP_* names. */
const VECTOR_OPS: Record<string, number> = {
  ER_VOP_SHAPE: 0,
  ER_VOP_MOVE: 1,
  ER_VOP_LINE: 2,
  ER_VOP_QUAD: 3,
  ER_VOP_CUBIC: 4,
  ER_VOP_ARC: 5,
  ER_VOP_CLOSE: 6,
};

const CHAIN_OPS = {'+': 'FADD', '-': 'FSUB', '*': 'FMUL'} as const;

/** C's (float)M_PI. */
const FLOAT_PI = Math.fround(Math.PI);

/** The bound app_vector_dirty keeps a damage hint's edges within. */
const DIRTY_BOUND = 1e9;

/*----------------------------------------------------------------------------------------------------------------------
 - Classes
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The program's tapes, each made once, and the node each updateVector call draws on.
 *
 * @class VectorPlan
 */
export class VectorPlan {
  readonly tapes: ProgramTape[] = [];
  private readonly arena: Arena;
  private readonly made = new Map<object, number>();
  /** Each node ref's node handle, by its C slot, for the updateVector calls. */
  readonly refNodes = new Map<string, Slot>();

  constructor(arena: Arena) {
    this.arena = arena;
  }

  /**
   * A baked drawing's tape.
   *
   * @param tables  The drawing.
   *
   * @returns The tape, or null for an empty drawing, which leaves its node blank.
   */
  tableTape(tables: IrVectorTables): number | null {
    if (!tables.ops.length) {
      return null;
    }
    return this.tape(tables, () => ({
      floats: tables.ops,
      paints: tables.paints.map(bakedPaint),
      gradients: tables.gradients.map(bakedGradient),
    }));
  }

  /**
   * A state-driven drawing's tape: what state drives is 0 until code works it out.
   *
   * @param vector  The drawing.
   *
   * @returns The tape.
   */
  drawingTape(vector: IrStateDrivenVector): number {
    return this.scratchTape(vector, () => ({
      floats: vector.tape.map(constantFloat),
      paints: vector.paints.map(drawingPaint),
      gradients: vector.gradients.map(drawingGradient),
    }));
  }

  /**
   * An updateVector call's tape.
   *
   * @param update  The call.
   *
   * @returns The tape.
   */
  updateTape(update: IrUpdateVector): number {
    return this.scratchTape(update, () => ({
      floats: update.tape.map(constantFloat),
      paints: update.paints.map(bakedPaint),
      gradients: [],
    }));
  }

  /** A tape code copies into the scratch, which must hold it. */
  private scratchTape(key: object, make: () => ProgramTape): number {
    const index = this.tape(key, make);
    this.arena.tapeScratch(this.tapes[index].floats.length);
    return index;
  }

  private tape(key: object, make: () => ProgramTape): number {
    const existing = this.made.get(key);
    if (existing !== undefined) {
      return existing;
    }
    const index = this.tapes.push(make()) - 1;
    this.made.set(key, index);
    return index;
  }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tables
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A baked paint as the tape holds it.
 *
 * @param paint  The paint.
 *
 * @returns The record.
 */
const bakedPaint = (paint: IrVectorPaint): ProgramPaint => ({
  fill: paint.fill >>> 0,
  stroke: paint.stroke >>> 0,
  strokeWidth: paint.strokeWidth,
  miter: paint.miter,
  cap: paint.cap,
  join: paint.join,
  fillRule: paint.fillRule,
  fillGradient: paint.fillGradient,
  strokeGradient: paint.strokeGradient,
});

/**
 * A baked gradient as the tape holds it, as printVectorGradient prints it. The check pass keeps a <Svg>'s own
 * gradients to 2 to 8 stops, but an <Svg source> file's may have fewer, which the runtime doesn't load.
 *
 * @param gradient  The gradient.
 *
 * @returns The record.
 */
function bakedGradient(gradient: VectorGradient): ProgramGradient {
  const stops = (gradient.stops || []).slice(0, MAX_GRADIENT_STOPS);
  if (stops.length < 2) {
    throw aotError(
      `AOT: an <Svg> gradient has ${stops.length} stop${stops.length === 1 ? '' : 's'}, but the package output needs at least two`,
      'give the gradient in the SVG file two stops, or paint a solid color.',
    );
  }
  return {
    type: gradient.type | 0,
    stops: stops.map(stop => ({
      color: stop.color >>> 0,
      position: stop.offset,
    })),
    ax: gradient.ax || 0,
    ay: gradient.ay || 0,
    bx: gradient.bx || 0,
    by: gradient.by || 0,
    radius: gradient.r || 0,
  };
}

/**
 * Whether a tape slot is the same on every update.
 *
 * @param value  The slot.
 *
 * @returns Whether it is.
 */
const isConstant = (value: IrTapeValue): boolean =>
  value.kind === 'op' || value.kind === 'number' || value.kind === 'fullTurn';

/**
 * A tape slot as the tape holds it: its value when it is constant, else 0.
 *
 * @param value  The slot.
 *
 * @returns The float.
 */
function constantFloat(value: IrTapeValue): number {
  switch (value.kind) {
    case 'op':
      return VECTOR_OPS[value.op];
    case 'number':
      return value.value;
    case 'fullTurn':
      return Math.fround(2 * FLOAT_PI);
    default:
      return 0;
  }
}

const constantColor = (color: IrPaintColor): number =>
  color.kind === 'static' ? color.argb >>> 0 : 0;

const constantGradient = (ref: IrGradientRef): number =>
  ref.when ? 0 : ref.index;

/**
 * A state-driven drawing's paint as the tape holds it, what state drives left 0.
 *
 * @param paint  The paint.
 *
 * @returns The record.
 */
const drawingPaint = (paint: IrDrawingPaint): ProgramPaint => ({
  fill: constantColor(paint.fill),
  stroke: constantColor(paint.stroke),
  strokeWidth: constantFloat(paint.strokeWidth),
  miter: paint.miter,
  cap: paint.cap,
  join: paint.join,
  fillRule: paint.fillRule,
  fillGradient: constantGradient(paint.fillGradient),
  strokeGradient: constantGradient(paint.strokeGradient),
});

/**
 * A state-driven drawing's gradient as the tape holds it, what state drives left 0.
 *
 * @param gradient  The gradient.
 *
 * @returns The record.
 */
const drawingGradient = (gradient: IrDrawingGradient): ProgramGradient => ({
  type: gradient.type,
  stops: gradient.stops.map(stop => ({
    color: constantColor(stop.color),
    position: constantFloat(stop.offset),
  })),
  ax: constantFloat(gradient.ax),
  ay: constantFloat(gradient.ay),
  bx: constantFloat(gradient.bx),
  by: constantFloat(gradient.by),
  radius: constantFloat(gradient.r),
});

/*----------------------------------------------------------------------------------------------------------------------
 - Code
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * A tape slot worked out as C works out its float.
 *
 * @param codegen  The function's codegen.
 * @param value  The slot.
 * @param locals  The drawing's locals, worked out already.
 *
 * @returns A float register. A local's is its own: never write to it.
 */
function tapeValue(
  codegen: Codegen,
  value: IrTapeValue,
  locals: Map<string, Register>,
): Register {
  const {fn} = codegen;
  const result = (): Register => fn.register('float');
  switch (value.kind) {
    case 'op':
    case 'number':
    case 'fullTurn':
      return codegen.load('float', constantFloat(value));
    case 'float':
      return codegen.valueAs(value.value, 'float');
    case 'local': {
      const local = locals.get(value.name);
      if (!local) {
        throw new Error(`AOT internal: no tape local "${value.name}"`);
      }
      return local;
    }
    case 'chain': {
      // Left to right, as C reads the chain.
      let sum = tapeValue(codegen, value.first, locals);
      for (const term of value.rest) {
        const next = result();
        fn.abc(
          CHAIN_OPS[term.op],
          next.index,
          sum.index,
          tapeValue(codegen, term.value, locals).index,
        );
        sum = next;
      }
      return sum;
    }
    case 'radians': {
      // ((degrees - 90.0f) * (float)M_PI / 180.0f)
      const radians = result();
      fn.abc(
        'FSUB',
        radians.index,
        tapeValue(codegen, value.degrees, locals).index,
        codegen.load('float', 90).index,
      );
      fn.abc(
        'FMUL',
        radians.index,
        radians.index,
        codegen.load('float', FLOAT_PI).index,
      );
      fn.abc(
        'FDIV',
        radians.index,
        radians.index,
        codegen.load('float', 180).index,
      );
      return radians;
    }
    case 'clampRadius': {
      // fminf(fmaxf(radius, 0.0f), side * 0.5f): fmaxf and fminf pass over a NaN, where FMATH would keep it.
      const radius = tapeValue(codegen, value.radius, locals);
      const zero = codegen.load('float', 0);
      const above = fn.register('bool');
      fn.abc('FLT', above.index, zero.index, radius.index);
      const atLeastZero = codegen.choose(
        above,
        () => radius,
        () => zero,
        'float',
      );
      const half = result();
      fn.abc(
        'FMUL',
        half.index,
        tapeValue(codegen, value.side, locals).index,
        codegen.load('float', 0.5).index,
      );
      const below = fn.register('bool');
      fn.abc('FLT', below.index, half.index, atLeastZero.index);
      return codegen.choose(
        below,
        () => half,
        () => atLeastZero,
        'float',
      );
    }
  }
}

/**
 * Works out a drawing's locals, each after the ones before it.
 *
 * @param codegen  The function's codegen.
 * @param locals  The locals.
 *
 * @returns Each one's register, by name.
 */
function drawingLocals(
  codegen: Codegen,
  locals: {name: string; value: IrTapeValue}[],
): Map<string, Register> {
  const registers = new Map<string, Register>();
  for (const local of locals) {
    registers.set(local.name, tapeValue(codegen, local.value, registers));
  }
  return registers;
}

/**
 * Sets each tape slot state drives in the scratch.
 *
 * @param codegen  The function's codegen.
 * @param tape  The tape's slots.
 * @param locals  The drawing's locals.
 */
function setTape(
  codegen: Codegen,
  tape: IrTapeValue[],
  locals: Map<string, Register>,
): void {
  tape.forEach((value, slot) => {
    if (!isConstant(value)) {
      codegen.scoped(() =>
        codegen.fn.abx('VSET', tapeValue(codegen, value, locals).index, slot),
      );
    }
  });
}

/**
 * Sets each paint and gradient field state drives in the scratch, as build_svg<id>() writes them.
 *
 * @param codegen  The function's codegen.
 * @param vector  The drawing.
 * @param locals  The drawing's locals.
 *
 * @returns Whether it set any, so the commit takes the scratch's paints.
 */
function setPaints(
  codegen: Codegen,
  vector: IrStateDrivenVector,
  locals: Map<string, Register>,
): boolean {
  const {fn} = codegen;
  let set = false;
  const field = (
    op: 'VPAINT' | 'VGRAD',
    index: number,
    code: number,
    value: () => Register,
  ): void => {
    set = true;
    codegen.scoped(() => fn.abc(op, value().index, index, code));
  };
  const color = (paintColor: IrPaintColor & {kind: 'state'}) => () =>
    codegen.convert(codegen.propRegister(paintColor.value), 'int');
  const float = (value: IrTapeValue) => () => tapeValue(codegen, value, locals);

  vector.paints.forEach((paint, index) => {
    if (paint.fill.kind === 'state') {
      field('VPAINT', index, PaintField.fill, color(paint.fill));
    }
    if (paint.stroke.kind === 'state') {
      field('VPAINT', index, PaintField.stroke, color(paint.stroke));
    }
    if (!isConstant(paint.strokeWidth)) {
      field('VPAINT', index, PaintField.strokeWidth, float(paint.strokeWidth));
    }
    const gradients: [IrGradientRef, number][] = [
      [paint.fillGradient, PaintField.fillGradient],
      [paint.strokeGradient, PaintField.strokeGradient],
    ];
    for (const [ref, code] of gradients) {
      const {when} = ref;
      if (when) {
        // ((test) ? index : 0), or the test negated.
        field('VPAINT', index, code, () =>
          codegen.choose(
            codegen.condition(when.test),
            () => codegen.load('int', when.negate ? 0 : ref.index),
            () => codegen.load('int', when.negate ? ref.index : 0),
            'int',
          ),
        );
      }
    }
  });

  vector.gradients.forEach((gradient, index) => {
    const geometry: [IrTapeValue, number][] = [
      [gradient.ax, GradientField.ax],
      [gradient.ay, GradientField.ay],
      [gradient.bx, GradientField.bx],
      [gradient.by, GradientField.by],
      [gradient.r, GradientField.radius],
    ];
    for (const [value, code] of geometry) {
      if (!isConstant(value)) {
        field('VGRAD', index, code, float(value));
      }
    }
    gradient.stops.forEach((stop, stopIndex) => {
      const at = GradientField.stops + 2 * stopIndex;
      if (stop.color.kind === 'state') {
        field('VGRAD', index, at, color(stop.color));
      }
      if (!isConstant(stop.offset)) {
        field('VGRAD', index, at + 1, float(stop.offset));
      }
    });
  });
  return set;
}

/**
 * Builds a state-driven drawing from state and commits it to its node, as build_svg<id>() and
 * er_node_set_vector_ops() do.
 *
 * @param codegen  The update function's codegen.
 * @param vector  The drawing.
 * @param plan  The program's tapes.
 * @param node  Loads the node's handle.
 */
export function commitDrawing(
  codegen: Codegen,
  vector: IrStateDrivenVector,
  plan: VectorPlan,
  node: () => Register,
): void {
  const {fn} = codegen;
  const tape = plan.drawingTape(vector);
  codegen.scoped(() => {
    fn.abx('VTAPE', 0, tape);
    const locals = drawingLocals(codegen, vector.locals);
    setTape(codegen, vector.tape, locals);
    const ownPaints = setPaints(codegen, vector, locals);
    fn.abx(ownPaints ? 'VCOMMITP' : 'VCOMMIT', node().index, tape);
  });
}

/**
 * An updateVector call: its tape worked out and handed to the node, then the damage hint, as the C output runs it.
 *
 * @param codegen  The function's codegen.
 * @param update  The call.
 * @param plan  The program's tapes.
 */
export function updateVector(
  codegen: Codegen,
  update: IrUpdateVector,
  plan: VectorPlan,
): void {
  const {fn} = codegen;
  // A ref no node holds draws nothing, as the C output's NULL node does.
  const handle = plan.refNodes.get(update.node);
  if (!handle) {
    return;
  }
  const tape = plan.updateTape(update);
  codegen.scoped(() => {
    fn.abx('VTAPE', 0, tape);
    const locals = drawingLocals(codegen, update.locals);
    setTape(codegen, update.tape, locals);
    const node = fn.register('node');
    fn.abx('GET64', node.index, handle.index);
    fn.abx('VCOMMIT', node.index, tape);
    const {dirty} = update;
    if (!dirty) {
      return;
    }
    if (dirty.some(edge => edge.type.kind === 'float')) {
      floatDirty(codegen, node, dirty);
      return;
    }
    const edges = fn.consecutive(['int', 'int', 'int', 'int']);
    dirty.forEach((edge, index) =>
      codegen.scoped(() =>
        fn.abc('MOVE', edges[index].index, codegen.valueAs(edge, 'int').index),
      ),
    );
    fn.abc('VDIRTY', node.index, edges[0].index);
  });
}

/**
 * A damage hint worked out in floats, as app_vector_dirty() does: none when an edge isn't finite, else rounded
 * out to whole pixels, each edge bounded.
 *
 * @param codegen  The function's codegen.
 * @param node  The node.
 * @param dirty  The x, y, width and height.
 */
function floatDirty(
  codegen: Codegen,
  node: Register,
  dirty: IrUpdateVector['dirty'] & object,
): void {
  const {fn} = codegen;
  const skip = fn.label();
  const [x, y, width, height] = dirty.map(edge =>
    codegen.valueAs(edge, 'float'),
  );
  // A value is finite when it less itself is 0.
  const zero = codegen.load('float', 0);
  for (const edge of [x, y, width, height]) {
    codegen.scoped(() => {
      const difference = fn.register('float');
      const finite = fn.register('bool');
      fn.abc('FSUB', difference.index, edge.index, edge.index);
      fn.abc('FEQ', finite.index, difference.index, zero.index);
      fn.branch('JF', skip, finite);
    });
  }
  const sum = (left: Register, right: Register): Register => {
    const total = fn.register('float');
    fn.abc('FADD', total.index, left.index, right.index);
    return total;
  };
  const low = codegen.load('float', -DIRTY_BOUND);
  const high = codegen.load('float', DIRTY_BOUND);
  const corners = (
    [
      [x, MathFn.floor],
      [y, MathFn.floor],
      [sum(x, width), MathFn.ceil],
      [sum(y, height), MathFn.ceil],
    ] as const
  ).map(([value, rounding]) => {
    const edge = fn.register('float');
    fn.abc('FMATH', edge.index, value.index, value.index, rounding);
    fn.abc('FMATH', edge.index, edge.index, low.index, MathFn.max);
    fn.abc('FMATH', edge.index, edge.index, high.index, MathFn.min);
    return codegen.convert(edge, 'int');
  });
  const rect = fn.consecutive(['int', 'int', 'int', 'int']);
  fn.abc('MOVE', rect[0].index, corners[0].index);
  fn.abc('MOVE', rect[1].index, corners[1].index);
  fn.abc('SUB', rect[2].index, corners[2].index, corners[0].index);
  fn.abc('SUB', rect[3].index, corners[3].index, corners[1].index);
  fn.abc('VDIRTY', node.index, rect[0].index);
  fn.place(skip);
}
