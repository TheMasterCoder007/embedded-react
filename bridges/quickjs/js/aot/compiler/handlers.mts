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

import {parseColor, parsePath} from '../../src/embedded-react/svg-ops.js';
import {aotError, withLoc} from './diagnostics.mts';
import {evalStatic} from './static-eval.mts';
import {cScalarType, floatLit} from './c-syntax.mts';
import {
  mix64Error,
  staticInt,
  isTime64,
  constDivisor,
  emitExprWide,
  emitExpr,
  lowerExpr,
  lowerExprWide,
  lowerText,
} from './expressions.mts';
import {isFn} from './collect.mts';
import {
  isAnimatedCall,
  resolveAnim,
  animValues,
  compileAnimateStart,
} from './animations.mts';
import {
  emitVectorPaint,
  svgNum,
  CAP_MAP,
  JOIN_MAP,
  arcEntriesC,
  circleEntriesC,
  geometryOf,
  rectEntriesC,
  lineEntriesC,
} from './svg.mts';
import {printStmts} from './c/statements.mts';
import type * as t from '@babel/types';
import type {Out} from './out.mts';
import type {IrExpr, IrTyped} from './ir/expressions.mts';
import type {IrItemValue, IrStmt, IrTimer} from './ir/statements.mts';
import type {VectorGeometry} from './svg.mts';
import type {
  CExpr,
  CType,
  EffectRecord,
  Env,
  FunctionNode,
  ListState,
  Local,
  PanCallback,
  PanResponderRecord,
  RefRecord,
  ScalarState,
  StateTable,
  StatementContext,
} from './types.mts';

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * The call to a body that set state ends with, to re-apply what reads it. Whether anything reads state is known
 * only once the whole app is emitted, so the call is dropped then if nothing does.
 */
export const APP_UPDATE_CALL = '    app_update();';

/** The constant 0, which a remainder by 0 or ±1 stores. */
const ZERO: IrExpr = {kind: 'number', value: 0, cType: 'int'};

/*----------------------------------------------------------------------------------------------------------------------
 - Implementation
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * Lowers a list-state setter call (`setItems(...)`) to bounded list operations.
 *
 * @param listState  The list state.
 * @param nextList  The setter's argument: the list's next value.
 * @param env  The expression environment.
 *
 * @returns The statements.
 */
function lowerListOp(
  listState: ListState,
  nextList: t.CallExpression['arguments'][number],
  env: Env,
): IrStmt[] {
  const {cap: capacity, struct} = listState;

  // setItems([...items, a, b]) — append; setItems([]) — clear.
  if (nextList.type === 'ArrayExpression') {
    if (nextList.elements.length === 0) {
      return [{kind: 'clearList', list: listState}];
    }

    const [firstElement, ...appendedItems] = nextList.elements;
    if (
      firstElement?.type !== 'SpreadElement' ||
      (firstElement.argument as t.Identifier).name !== listState.name
    ) {
      throw new Error(
        `AOT: a list literal must spread the current list first: [...${listState.name}, item]`,
      );
    }

    // Each appended object literal fills the fields of the next free slot; a full list drops it.
    const statements: IrStmt[] = [];
    for (const item of appendedItems) {
      if (item!.type !== 'ObjectExpression') {
        throw new Error('AOT: appended list items must be object literals');
      }

      const props = new Map(
        (item.properties as t.ObjectProperty[]).map(
          (prop): [string, t.Node] => [
            (prop.key as t.Identifier).name ??
              (prop.key as t.StringLiteral).value,
            prop.value,
          ],
        ),
      );
      const values: IrItemValue[] = [];
      for (const field of struct.fields) {
        const valueNode = props.get(field.key);
        if (!valueNode) continue;
        if (field.kind === 'string') {
          values.push({field, text: lowerText(valueNode, env)});
        } else {
          values.push({
            field,
            value: storeValue(lowerExpr(valueNode, env), field.kind),
          });
        }
      }

      statements.push({kind: 'appendItem', list: listState, values});
    }

    return statements;
  }

  // slice(0, end) keeps the first items by JS rules: a negative end counts back, a large or missing one keeps all.
  if (
    nextList.type === 'CallExpression' &&
    nextList.callee.type === 'MemberExpression' &&
    (nextList.callee.object as t.Identifier).name === listState.name &&
    (nextList.callee.property as t.Identifier).name === 'slice'
  ) {
    const [start, end] = nextList.arguments;
    if (start && staticInt(start, env) !== 0) {
      throw aotError(
        `AOT: only ${listState.name}.slice(0, end) is supported on a list`,
        `a list is a fixed C array and a count, so a slice can only drop items from the end — ` +
          `e.g. ${listState.name}.slice(0, -1) drops the last one.`,
      );
    }

    if (!end) return [];

    // Settle a constant end here, clamped to ±capacity (past which nothing changes), so it needs no 64-bit math.
    const staticEnd = staticInt(end, env);
    if (staticEnd !== null) {
      const clampedEnd = Math.max(-capacity, Math.min(capacity, staticEnd));
      if (clampedEnd === -1) {
        return [{kind: 'dropLastItem', list: listState}];
      }
      if (clampedEnd >= 0) {
        return [{kind: 'keepFirstItems', list: listState, count: clampedEnd}];
      }
      return [
        {
          kind: 'sliceItems',
          list: listState,
          end: {kind: 'number', value: clampedEnd, cType: 'int'},
        },
      ];
    }

    // A runtime end goes through app_slice_len, which applies the same JS rules.
    return [
      {kind: 'sliceItems', list: listState, end: asInt(lowerExpr(end, env))},
    ];
  }

  throw new Error(
    `AOT: unsupported list operation on "${listState.name}" (use [...${listState.name}, ` +
      `item], ${listState.name}.slice(0, n), or [])`,
  );
}

/**
 * Returns a statement node's body list: a BlockStatement's contents, or the lone statement wrapped.
 *
 * @param node  An `if` branch.
 *
 * @returns Its statements.
 */
function blockList(node: t.Statement): t.Statement[] {
  return node.type === 'BlockStatement' ? node.body : [node];
}

/**
 * Checks a value written to a numeric state or ref slot, whose C type came from its initial value. A 64-bit
 * timestamp written to an int slot is recorded in env.found, and compileWidened compiles again with that
 * slot widened to int64_t. It cannot go into a float or boolean slot (a 64-bit constant can go into a float
 * one), and a widened slot takes no floats.
 *
 * @param value  The lowered value.
 * @param slotType  The slot's C kind.
 * @param isBool  The slot holds a boolean.
 * @param slotCName  The slot's C name, for env.found.
 * @param slotDescription  The slot, for the message.
 * @param env  The expression environment.
 */
function storeCheck(
  value: IrTyped,
  slotType: CType,
  isBool: boolean,
  slotCName: string,
  slotDescription: string,
  env: Env,
): void {
  if (value.cType === 'i64' && slotType !== 'i64') {
    if (slotType === 'int' && !isBool) {
      env.found.add(slotCName);
      return;
    }

    if (value.lit && slotType === 'float') return;

    throw aotError(
      `AOT: a 64-bit time value cannot be stored in ${slotDescription}`,
      'give it a whole-number initial value — useState(0) or useRef(0) — and it widens to hold the timestamp.',
    );
  }

  if (slotType === 'i64' && value.cType === 'float') {
    throw mix64Error();
  }
}

/**
 * `value` for an int destination. A float goes through app_f2i: C's own conversion is undefined behavior for
 * NaN or a value past the int range.
 *
 * @param value  The lowered value.
 *
 * @returns The value as an int.
 */
const asInt = (value: IrExpr): IrExpr =>
  value.cType === 'float'
    ? {kind: 'toInt', operand: value, cType: 'int'}
    : value;

/**
 * `value` for a slot of `slotType`: an int slot takes a float the way asInt does.
 *
 * @param value  The lowered value.
 * @param slotType  The slot's C kind.
 *
 * @returns The value the slot stores.
 */
const storeValue = (value: IrExpr, slotType: CType): IrExpr =>
  slotType === 'int' ? asInt(value) : value;

/**
 * A read of a ref's current value.
 *
 * @param ref  The ref.
 *
 * @returns The read.
 */
const refRead = (ref: RefRecord): IrExpr => ({
  kind: 'ref',
  ref,
  cType: ref.cType as CType,
});

/**
 * Lowers a write of an expression into a scalar state slot: text for a string buffer (so a `+` chain keeps its
 * parts), a plain value otherwise.
 *
 * @param scalarState  The state slot.
 * @param valueNode  The value.
 * @param env  The expression environment.
 *
 * @returns The write.
 */
function lowerScalarSet(
  scalarState: ScalarState,
  valueNode: t.Node,
  env: Env,
): IrStmt {
  if (scalarState.cType !== 'string') {
    // A 64-bit slot takes int `+ - *` worked out in 64 bits.
    const value = lowerExprWide(
      valueNode,
      scalarState.cType === 'i64' ? {...env, math64: true} : env,
    );
    storeCheck(
      value,
      scalarState.cType,
      scalarState.isBool,
      scalarState.cField,
      `state "${scalarState.name}"`,
      env,
    );

    return {
      kind: 'setState',
      state: scalarState,
      value: storeValue(value, scalarState.cType),
    };
  }

  return {
    kind: 'setStateText',
    state: scalarState,
    text: lowerText(valueNode, env),
  };
}

/**
 * Resolves `node` to its ref when it is `<ref>.current` member access on a known value ref.
 *
 * @param node  An assignment or update target.
 * @param env  The expression environment.
 *
 * @returns The ref, marked used, or null when the node is not such an access.
 */
function refTarget(
  node: t.Node | null | undefined,
  env: Env,
): RefRecord | null {
  if (
    node?.type === 'MemberExpression' &&
    !node.computed &&
    node.object.type === 'Identifier' &&
    (node.property as t.Identifier).name === 'current' &&
    env.refs?.has(node.object.name)
  ) {
    const ref = env.refs.get(node.object.name)!;
    ref.used = true;
    return ref;
  }

  return null;
}

/**
 * An imperative updateVector shape `{ arc:[…]|circle:[…]|rect:[x, y,w,h,rx?,ry?]|line:[…]|path:'…', fill, … }`
 * → { entries (op-tape C exprs), locals (C declarations to emit ahead of them), paint (static 7-num
 * record) }. Geometry coords may reference state/refs/event fields (emitExpr); paint must be static.
 * Shares the ...EntriesC geometry with the JSX path. `tag` names any locals and must be unique in the
 * handler block the caller writes them into.
 *
 * @param shapeNode  The shape's object literal.
 * @param env  The expression environment.
 * @param tag  Names the shape's C locals.
 *
 * @returns The shape's op-tape entries, the locals they need, and its paint record.
 */
function imperativeShape(
  shapeNode: t.Node | null | undefined,
  env: Env,
  tag: string,
): VectorGeometry & {paint: number[]} {
  // Index the shape's properties by name.
  if (shapeNode?.type !== 'ObjectExpression') {
    throw new Error('AOT: each updateVector shape must be an object literal');
  }

  const props: Record<string, t.Node> = {};
  for (const prop of shapeNode.properties) {
    if (prop.type !== 'ObjectProperty') {
      throw new Error(
        'AOT: spread/method in an updateVector shape not supported',
      );
    }
    props[
      (prop.key as t.Identifier).name ?? (prop.key as t.StringLiteral).value
    ] = prop.value;
  }

  // Lower whichever geometry key is present; array numbers may be runtime values, a path string must be static.
  const geometryArgs = (key: string, count: number): string[] => {
    if (props[key].type !== 'ArrayExpression') {
      throw new Error(`AOT: updateVector "${key}" must be an array literal`);
    }

    return (props[key] as t.ArrayExpression).elements
      .slice(0, count)
      .map(valueNode => `(float)(${emitExpr(valueNode!, env).code})`);
  };
  let geometry: VectorGeometry;
  if (props.arc) {
    geometry = geometryOf(
      arcEntriesC(
        ...(geometryArgs('arc', 5) as [string, string, string, string, string]),
      ),
    );
  } else if (props.circle) {
    geometry = geometryOf(
      circleEntriesC(
        ...(geometryArgs('circle', 3) as [string, string, string]),
      ),
    );
  } else if (props.rect) {
    // Destructure rather than spread so a 4-element literal cannot slide `tag` into the optional rx slot.
    const [x, y, width, height, rx = null, ry = null] = geometryArgs('rect', 6);
    geometry = geometryOf(rectEntriesC(x, y, width, height, rx, ry, tag));
  } else if (props.line) {
    geometry = geometryOf(
      lineEntriesC(
        ...(geometryArgs('line', 4) as [string, string, string, string]),
      ),
    );
  } else if (props.path) {
    geometry = geometryOf(
      parsePath(String(evalStatic(props.path, env.consts ?? {}))).map(floatLit),
    );
  } else {
    throw new Error(
      'AOT: an updateVector shape needs one of arc / circle / rect / line / path',
    );
  }

  // Fold the paint to its static record: fill, stroke, stroke width, miter limit, cap, join, fill rule.
  const paintValue = (key: string, fallback: unknown): unknown => {
    if (props[key] == null) return fallback;
    try {
      return evalStatic(props[key], env.consts ?? {});
    } catch {
      throw new Error(`AOT: updateVector paint "${key}" must be static`);
    }
  };
  const paint = [
    parseColor(paintValue('fill', 'none')),
    parseColor(paintValue('stroke', 'none')),
    svgNum(paintValue('strokeWidth', 1), 1),
    svgNum(paintValue('miter', 4), 4),
    CAP_MAP[paintValue('cap', 'butt') as string] ?? 0,
    JOIN_MAP[paintValue('join', 'miter') as string] ?? 0,
    paintValue('fillRule', 'nonzero') === 'evenodd' ? 1 : 0,
  ];

  return {...geometry, paint};
}

/**
 * Lowers `updateVector(nodeRef, shapes, [x,y,w,h]?)` to: fill a mutable op-tape, push it to the node, and
 * (optionally) hint the dirty sub-rect — the imperative fast path (drag) that bypasses app_update. The shapes'
 * geometry is still C from the SVG emitter, so the statement carries C lines.
 *
 * @param expr  The updateVector call.
 * @param env  The expression environment.
 * @param ctx  The statement compiler's context.
 *
 * @returns The statement.
 */
function lowerUpdateVector(
  expr: t.CallExpression,
  env: Env,
  ctx: StatementContext,
): IrStmt {
  // The first argument must be a node ref, and the second an array literal of shapes.
  const out = ctx.out;
  const [refArg, shapesArg, dirtyArg] = expr.arguments;
  const ref = refArg?.type === 'Identifier' ? env.refs?.get(refArg.name) : null;
  if (ref?.kind !== 'node') {
    throw new Error(
      'AOT: updateVector(ref, …) first arg must be a node ref (const r = useRef())',
    );
  }

  ref.used = true;
  if (shapesArg?.type !== 'ArrayExpression') {
    throw new Error(
      'AOT: updateVector(ref, shapes, …) shapes must be an array literal',
    );
  }

  // Claim the id first so two updateVector calls in one handler get distinct locals, then lower each shape.
  const vectorId = out.svgN++;
  const entries: string[] = [];
  const decls: string[] = [];
  const paints: number[][] = [];
  for (const shapeNode of shapesArg.elements) {
    const {
      entries: shapeEntries,
      locals,
      paint,
    } = imperativeShape(shapeNode, env, `uv${vectorId}_${paints.length}`);
    decls.push(...locals);
    entries.push('ER_VOP_SHAPE', floatLit(paints.length), ...shapeEntries);
    paints.push(paint);
  }

  // Declare the op-tape and its paint table at file scope, then fill the tape and hand it to the node.
  const opCount = entries.length;
  out.needsMath = true;
  out.vectorData.push(`static float s_uv${vectorId}_ops[${opCount}];`);
  out.vectorData.push(
    `static const ERVectorPaint s_uv${vectorId}_paints[] = ` +
      `{\n${paints.map(paint => '    ' + emitVectorPaint(paint)).join(',\n')}\n};`,
  );
  const lines = [
    ...decls,
    ...entries.map(
      (entry, entryIndex) => `s_uv${vectorId}_ops[${entryIndex}] = ${entry};`,
    ),
  ];
  lines.push(
    `er_node_set_vector_ops(${ref.cVar}, s_uv${vectorId}_ops, ${opCount}, ` +
      `s_uv${vectorId}_paints, ${paints.length}, NULL, 0);`,
  );

  // Hint the dirty sub-rect when the call gives one.
  if (dirtyArg) {
    if (dirtyArg.type !== 'ArrayExpression' || dirtyArg.elements.length < 4) {
      throw new Error(
        'AOT: updateVector dirtyRect must be a [x, y, w, h] array literal',
      );
    }

    const dirtyRect = dirtyArg.elements
      .slice(0, 4)
      .map(edgeNode => emitExpr(edgeNode!, env));
    // A float edge may be NaN; app_vector_dirty then drops the hint, since a zero-width hint would paint nothing.
    const dirtyFn = dirtyRect.some(edge => edge.cType === 'float')
      ? 'app_vector_dirty'
      : 'er_node_set_vector_dirty_rect';
    lines.push(
      `${dirtyFn}(${ref.cVar}, ${dirtyRect.map(edge => edge.code).join(', ')});`,
    );
  }

  return {kind: 'foreign', lines, isIndented: false};
}

/**
 * Lowers setInterval/setTimeout(cb, ms) to a timer, and registers cb as a timer function of its own.
 *
 * @param expr  The setInterval / setTimeout call.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param ctx  The statement compiler's context.
 *
 * @returns The timer.
 */
function lowerTimer(
  expr: t.CallExpression,
  env: Env,
  state: StateTable,
  ctx: StatementContext,
): IrTimer {
  const cb = expr.arguments[0];
  if (!isFn(cb)) {
    throw aotError(
      'AOT: a setInterval/setTimeout callback must be an inline function',
      'pass an inline arrow, e.g. setInterval(() => setTick((t) => t + 1), 1000).',
    );
  }

  // Any delay is accepted here; the printer converts a timestamp or float one the way Flow A's setTimeout does.
  const delay = expr.arguments[1]
    ? lowerExprWide(expr.arguments[1], env)
    : null;
  const isRepeating = (expr.callee as t.Identifier).name === 'setInterval';

  // Reserve the timer's slot before compiling its body, which may add timers of its own.
  const slot = ctx.out.timerFns.length;
  const timerFnName = `er_timer_fn_${slot}`;
  ctx.out.usesTimers = true;
  ctx.out.timerFns.push({name: timerFnName, body: null});
  ctx.out.timerFns[slot].body = compileHandler(cb, env, state, ctx.out);
  return {delay, isRepeating, fn: timerFnName};
}

/**
 * Inlines a handler-statement call to a helper / useCallback: binds the call's args to the helper's params
 * as locals, then lowers the helper body here in the current env/state/ctx. Guards against recursion.
 *
 * @param helperName  The helper's name.
 * @param helper  The helper.
 * @param args  The call's arguments.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param ctx  The statement compiler's context.
 *
 * @returns The helper's body as statements.
 */
function inlineHelperCall(
  helperName: string,
  helper: FunctionNode,
  args: t.CallExpression['arguments'],
  env: Env,
  state: StateTable,
  ctx: StatementContext,
): IrStmt[] {
  // A helper already being inlined further up this call chain is recursive.
  ctx.inlining = ctx.inlining ?? new Set();
  if (ctx.inlining.has(helperName)) {
    throw aotError(
      `AOT: helper "${helperName}" is recursive — can't be inlined into a handler`,
      'handlers are flattened to straight-line C, so a helper that calls itself (directly ' +
        'or via another helper) has no base case to unroll. Move recursive logic out of the ' +
        'handler, or precompute the value.',
    );
  }

  // Bind each passed argument to its parameter as a local holding the argument's C expression.
  const locals = new Map<string, Local>(env.locals);
  helper.params.forEach((param, paramIndex) => {
    if (param.type !== 'Identifier') {
      throw aotError(
        `AOT: helper "${helperName}" must take simple (identifier) params to be inlined`,
        'a helper called from a handler must use plain positional params (e.g. `(a, b) => …`) ' +
          '— destructuring or default params in the signature are not supported.',
      );
    }

    if (args[paramIndex]) {
      const argExpr = emitExprWide(args[paramIndex], env);
      locals.set(param.name, {
        code: argExpr.code,
        cType: argExpr.cType,
        isBool: argExpr.isBool,
        lit: argExpr.lit,
      });
    }
  });

  // An expression body is a single statement.
  const body = helper.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];

  // The body is spliced into the caller, so reject its `return`, even in an effect body where one is allowed.
  const outerAllowReturn = ctx.allowReturn;
  ctx.inlining.add(helperName);
  ctx.allowReturn = false;
  try {
    return lowerStmts(statements, {...env, locals}, state, ctx);
  } finally {
    ctx.allowReturn = outerAllowReturn;
    ctx.inlining.delete(helperName);
  }
}

/**
 * Lowers one handler ExpressionStatement: a state setter, ref mutation, timer, updateVector, helper call, or
 * Animated.*(...).start() / .stop() (see lowerHandlerExpr, the located entry point).
 *
 * @param expr  The statement's expression.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param ctx  The statement compiler's context.
 *
 * @returns The statements.
 */
function lowerHandlerExprImpl(
  expr: t.Expression,
  env: Env,
  state: StateTable,
  ctx: StatementContext,
): IrStmt[] {
  // updateVector(ref, shapes, dirtyRect?) — imperative vector redraw (no app_update).
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'Identifier' &&
    expr.callee.name === 'updateVector'
  ) {
    return [lowerUpdateVector(expr, env, ctx)];
  }

  // setInterval / setTimeout(cb, ms) → register a host-tick timer (the returned id is discarded here).
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'Identifier' &&
    (expr.callee.name === 'setInterval' || expr.callee.name === 'setTimeout')
  ) {
    return [
      {
        kind: 'startTimer',
        timer: lowerTimer(expr, env, state, ctx),
        idLocal: null,
      },
    ];
  }

  // clearInterval / clearTimeout(id) → deactivate the timer slot.
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'Identifier' &&
    (expr.callee.name === 'clearInterval' ||
      expr.callee.name === 'clearTimeout')
  ) {
    return [{kind: 'clearTimer', id: asInt(lowerExpr(expr.arguments[0], env))}];
  }

  // `ref.current = expr` / `ref.current += expr` — a value ref write; does NOT trigger a re-render.
  if (expr.type === 'AssignmentExpression') {
    const ref = refTarget(expr.left, env);
    if (!ref) {
      throw new Error(
        'AOT: the only assignment allowed in a handler is `ref.current = ...`',
      );
    }

    // Lower the right side in the ref's width and check it fits; a 64-bit `%=` keeps its constant divisor.
    const refEnv = ref.cType === 'i64' ? {...env, math64: true} : env;
    const valueExpr = lowerExprWide(expr.right, refEnv);
    if (
      expr.operator === '/=' &&
      (ref.cType === 'i64' || isTime64(valueExpr))
    ) {
      throw aotError(
        'AOT: `/=` on a 64-bit time value is not supported',
        'JS division gives a fraction; write ref.current = Math.floor(ref.current / n) for whole units.',
      );
    }

    const wideModDivisor =
      expr.operator === '%=' && (ref.cType === 'i64' || isTime64(valueExpr))
        ? constDivisor(expr.right, env)
        : null;
    storeCheck(
      valueExpr,
      ref.cType as CType,
      false,
      ref.cVar,
      `ref "${((expr.left as t.MemberExpression).object as t.Identifier).name}"`,
      env,
    );

    // Lower whole-ref `+= -= *=`, float `/=` and float `%=` as the binary op to share its checked and float math.
    const isWholeRef = ref.cType === 'int' || ref.cType === 'i64';
    const binaryOp = (
      {'+=': '+', '-=': '-', '*=': '*'} as Record<string, string>
    )[expr.operator];
    const isFloatMod =
      expr.operator === '%=' &&
      (ref.cType === 'float' || valueExpr.cType === 'float');
    if (
      (isWholeRef &&
        (binaryOp ||
          (expr.operator === '/=' && valueExpr.cType === 'float'))) ||
      isFloatMod
    ) {
      const resultExpr = lowerExprWide(
        {
          type: 'BinaryExpression',
          operator: binaryOp ?? expr.operator.slice(0, -1),
          left: expr.left,
          right: expr.right,
          loc: expr.loc,
        } as t.BinaryExpression,
        refEnv,
      );

      return [
        {
          kind: 'writeRef',
          ref,
          op: '=',
          value: storeValue(resultExpr, ref.cType as CType),
        },
      ];
    }

    // Int `/=` and `%=` use app_div/app_mod for JS's answer; a constant divisor other than 0 or -1 needs no check.
    if (
      ref.cType === 'int' &&
      valueExpr.cType === 'int' &&
      (expr.operator === '/=' || expr.operator === '%=')
    ) {
      const staticDivisor = staticInt(expr.right, env);
      if (
        expr.operator === '/=' &&
        (staticDivisor === null || staticDivisor === 0 || staticDivisor === -1)
      ) {
        return [
          {
            kind: 'writeRef',
            ref,
            op: '=',
            value: {
              kind: 'helper',
              helper: 'div',
              bits: 32,
              args: [refRead(ref), valueExpr],
              cType: 'int',
            },
          },
        ];
      }

      if (
        expr.operator === '%=' &&
        (staticDivisor === 0 || staticDivisor === -1)
      ) {
        return [{kind: 'writeRef', ref, op: '=', value: ZERO}];
      }

      if (expr.operator === '%=' && staticDivisor === null) {
        return [
          {
            kind: 'writeRef',
            ref,
            op: '=',
            value: {
              kind: 'helper',
              helper: 'mod',
              bits: 32,
              args: [refRead(ref), valueExpr],
              cType: 'int',
            },
          },
        ];
      }
    }

    // A 64-bit `%=` by 0 or ±1 stores 0: no remainder, JS's NaN as 0, and no C overflow on INT64_MIN % -1.
    if (wideModDivisor === 0 || wideModDivisor === 1 || wideModDivisor === -1) {
      return [{kind: 'writeRef', ref, op: '=', value: ZERO}];
    }

    // Everything else is a plain assignment or compound assignment.
    return [
      {
        kind: 'writeRef',
        ref,
        op: expr.operator,
        value:
          expr.operator === '='
            ? storeValue(valueExpr, ref.cType as CType)
            : valueExpr,
      },
    ];
  }

  // `ref.current++` / `ref.current--`; a whole-number ref steps through the checked `+ 1` / `- 1`.
  if (expr.type === 'UpdateExpression') {
    const ref = refTarget(expr.argument, env);
    if (!ref) {
      throw new Error(
        'AOT: the only ++/-- allowed in a handler is on `ref.current`',
      );
    }

    if (ref.cType === 'int' || ref.cType === 'i64') {
      return [
        {
          kind: 'writeRef',
          ref,
          op: '=',
          value: {
            kind: 'helper',
            helper: expr.operator === '++' ? 'add' : 'sub',
            bits: ref.cType === 'i64' ? 64 : 32,
            args: [refRead(ref), {kind: 'number', value: 1, cType: 'int'}],
            cType: ref.cType,
          },
        },
      ];
    }

    return [{kind: 'stepRef', ref, op: expr.operator}];
  }

  // `anim.stop()` freezes each driven value; the cancel reports !finished, which ends any sequence or loop chain.
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'MemberExpression' &&
    (expr.callee.property as t.Identifier).name === 'stop' &&
    isAnimatedCall(resolveAnim(expr.callee.object, env))
  ) {
    return [
      {
        kind: 'stopAnimations',
        handles: [...animValues(expr.callee.object, env)],
      },
    ];
  }

  // Animated.*(…).start(), atomic or composed, is native-driven, so it sets no state and needs no app_update.
  if (
    expr.type === 'CallExpression' &&
    expr.callee.type === 'MemberExpression' &&
    (expr.callee.property as t.Identifier).name === 'start'
  ) {
    return [
      {
        kind: 'foreign',
        lines: compileAnimateStart(expr, env, state, ctx),
        isIndented: true,
      },
    ];
  }

  // What is left must be a plain call: to a helper or a state setter.
  if (expr.type !== 'CallExpression' || expr.callee.type !== 'Identifier') {
    throw aotError(
      'AOT: a handler statement must be a state setter, a ref write, or Animated.timing/spring(...).start()',
      'each statement in a handler must be one of: setX(value) / setX(prev => …), a ' +
        '`ref.current = …` write, an `updateVector(…)` call, or `Animated.timing|spring(v, ' +
        '…).start()`. Wrap conditional logic in `if (…) { … }`.',
    );
  }

  // A call to a helper / useCallback (e.g. `reset();`) → inline its body here so handlers can compose logic.
  const helperFn =
    env.helpers?.get(expr.callee.name) ?? env.callbacks?.get(expr.callee.name);
  if (helperFn) {
    return inlineHelperCall(
      expr.callee.name,
      helperFn,
      expr.arguments,
      env,
      state,
      ctx,
    );
  }

  // A state setter. Flag the state change, which tells the caller to re-apply state once the body is done.
  const targetState = state.bySetter.get(expr.callee.name);
  if (!targetState) {
    throw aotError(
      `AOT: "${expr.callee.name}" is not a known state setter`,
      `a handler can only call a setter from this component's own useState (e.g. setCount), ` +
        `a ref write, updateVector(…), or Animated…start(). "${expr.callee.name}" isn't one ` +
        `of those — arbitrary functions (fetch, console.*, helpers) can't be lowered to C.`,
    );
  }

  ctx.stateChanged = true;
  const setterArg = expr.arguments[0];
  if (targetState.kind === 'list') {
    return lowerListOp(targetState, setterArg, env);
  }

  // A scalar setter takes either an updater function or the new value itself.
  if (
    setterArg &&
    (setterArg.type === 'ArrowFunctionExpression' ||
      setterArg.type === 'FunctionExpression')
  ) {
    // setState(prev => expr): bind the param to the current value, assign the result.
    const prevParam = (setterArg.params[0] as t.Identifier | undefined)?.name;
    const locals = new Map<string, Local>(env.locals);
    if (prevParam) {
      locals.set(prevParam, {
        code: targetState.cMember,
        cType: targetState.cType,
      });
    }

    if (setterArg.body.type === 'BlockStatement') {
      throw new Error(
        'AOT: updater function must be a single expression (for now)',
      );
    }

    return [lowerScalarSet(targetState, setterArg.body, {...env, locals})];
  }

  return [lowerScalarSet(targetState, setterArg, env)];
}

/** lowerHandlerExprImpl, with the expression's source location attached to any AOT error it throws. */
const lowerHandlerExpr = withLoc(lowerHandlerExprImpl);

/**
 * Lowers a list of handler statements to the IR. This one-statement lowering runs event handlers, effect bodies
 * (useEffect), timer callbacks, and inlined helper calls. Supports: `const x = expr` (a local, visible to later
 * statements), `if (cond) {...} else {...}`, state setters (list setters included), ref writes, timers,
 * updateVector() and Animated.*(...).start(). `ctx` accumulates `stateChanged` (→ trailing app_update),
 * `animIdx` (unique ERAnimConfig locals) and `usedReturn` (an early `return` was lowered, so the body needs a C
 * function of its own). `ctx.allowReturn` and `ctx.bodyList` mark which body a `return` may exit and which
 * statement of it is the tail.
 *
 * @param statements  The statements.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param ctx  The statement compiler's context.
 *
 * @returns The lowered statements.
 */
export function lowerStmts(
  statements: t.Statement[],
  env: Env,
  state: StateTable,
  ctx: StatementContext,
): IrStmt[] {
  const lowered: IrStmt[] = [];
  for (
    let statementIndex = 0;
    statementIndex < statements.length;
    statementIndex++
  ) {
    const statement = statements[statementIndex];

    // `const x = …` declares a local that the statements after it can read.
    if (statement.type === 'VariableDeclaration') {
      for (const decl of statement.declarations) {
        if (decl.id.type !== 'Identifier') {
          throw new Error(
            'AOT: destructuring a handler local is not supported',
          );
        }

        if (!decl.init) {
          throw new Error('AOT: a handler local must have an initializer');
        }

        // `const anim = Animated.…` holds no C value; `anim.start()`/`anim.stop()` compile the animation it names.
        if (isAnimatedCall(decl.init)) {
          const locals = new Map(env.locals);
          locals.delete(decl.id.name);
          env = {
            ...env,
            locals,
            animLocals: new Map(env.animLocals).set(decl.id.name, decl.init),
          };
          continue;
        }

        // A dep-driven effect's cleanup outlives the call, so the body's locals become file-scope slots.
        const isHoisted = Boolean(ctx.hoist && statements === ctx.bodyList);
        const cName = isHoisted
          ? `${ctx.hoist!.prefix}${decl.id.name}`
          : `l_${decl.id.name}`;

        // `const id = setInterval/setTimeout(…)` is an int timer-id local, so a later clearInterval(id) resolves.
        if (
          decl.init.type === 'CallExpression' &&
          decl.init.callee.type === 'Identifier' &&
          (decl.init.callee.name === 'setInterval' ||
            decl.init.callee.name === 'setTimeout')
        ) {
          // The id is only needed for a later clear*(); a mount effect drops its cleanup, so mark it used.
          if (isHoisted) {
            ctx.hoist!.decls.push(`static int ${cName};`);
          }
          lowered.push({
            kind: 'startTimer',
            timer: lowerTimer(decl.init, env, state, ctx),
            idLocal: {name: cName, isHoisted},
          });
          env = {
            ...env,
            locals: new Map(env.locals).set(decl.id.name, {
              code: cName,
              cType: 'int',
            }),
          };

          continue;
        }

        // Any other initializer must lower to a number, a boolean or a string.
        const initExpr = lowerExprWide(decl.init, env);
        if (initExpr.cType === 'string') {
          // Copy into a buffer of its own; aliasing a state slot would hide a self-read like `setLabel(t + '!')`.
          if (isHoisted) {
            ctx.hoist!.decls.push(
              `static char ${cName}[${env.caps.listStrCap}];`,
            );
          }
        } else {
          if (
            initExpr.cType !== 'int' &&
            initExpr.cType !== 'float' &&
            initExpr.cType !== 'i64'
          ) {
            throw aotError(
              'AOT: a handler local must hold a number, a boolean or a string',
              'bind the value itself — e.g. `const id = item.id` — rather than a list item or a node ref.',
            );
          }

          if (isHoisted) {
            ctx.hoist!.decls.push(
              `static ${cScalarType(initExpr.cType)} ${cName};`,
            );
          }
        }

        lowered.push({
          kind: 'declareLocal',
          name: cName,
          init: initExpr,
          isHoisted,
          stringCap: env.caps.listStrCap,
        });

        // Later statements read the name as the C local.
        env = {
          ...env,
          locals: new Map(env.locals).set(decl.id.name, {
            code: cName,
            cType: initExpr.cType,
            isBool: initExpr.isBool,
          }),
        };
      }

      continue;
    }

    // A tail `return` is the cleanup, dropped on mount and armed if dep-driven; an earlier one is a guard.
    if (statement.type === 'ReturnStatement') {
      if (!ctx.allowReturn) {
        const error = aotError(
          'AOT: `return` is only supported inside a useEffect body',
          'a handler, timer, animation or inlined-helper body cannot return early — flatten the logic into if/else.',
        );
        if (statement.loc) {
          error.aotLoc = statement.loc.start;
        }

        throw error;
      }

      const isTail =
        statements === ctx.bodyList && statementIndex === statements.length - 1;
      if (ctx.depDriven && isFn(statement.argument) && !isTail) {
        const error = aotError(
          'AOT: a useEffect cleanup must be the last statement of the effect body',
          'a cleanup returned from inside an `if` would be dropped. Return `undefined` from the ' +
            'guard and put the single `return () => …` at the end of the body.',
        );

        if (statement.loc) {
          error.aotLoc = statement.loc.start;
        }

        throw error;
      }

      if (isTail) {
        if (ctx.cleanup && isFn(statement.argument)) {
          ctx.cleanup.emit(statement.argument, env);
          lowered.push({kind: 'armCleanup', flag: ctx.cleanup.armed});
        }

        continue;
      }

      ctx.usedReturn = true;
      lowered.push({kind: 'return'});
      continue;
    }

    // `if (…) { … } else { … }` lowers each branch in order: the test, then the consequent, then the alternate.
    if (statement.type === 'IfStatement') {
      const test = lowerExprWide(statement.test, env);
      const consequent = lowerStmts(
        blockList(statement.consequent),
        env,
        state,
        ctx,
      );
      const alternate = statement.alternate
        ? lowerStmts(blockList(statement.alternate), env, state, ctx)
        : null;
      lowered.push({kind: 'if', test, consequent, alternate});
      continue;
    }

    // Anything else must be an expression statement: a setter call, a ref write, a timer, and so on.
    if (statement.type !== 'ExpressionStatement') {
      throw aotError(
        `AOT: unsupported statement "${statement.type}" in event handler`,
        'a handler supports only `const x = …` locals, `if (…) { … } else { … }`, and ' +
          'expression statements (setters / ref writes / updateVector / Animated…start). Loops ' +
          '(for/while), switch and try/catch are not lowered — precompute values or flatten the ' +
          'logic into if/else.',
      );
    }

    lowered.push(...lowerHandlerExpr(statement.expression, env, state, ctx));
  }

  return lowered;
}

/**
 * Lowers handler statements and prints them as C lines (see lowerStmts).
 *
 * @param statements  The statements.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param ctx  The statement compiler's context.
 * @param indent  The statements' indentation.
 *
 * @returns The C lines.
 */
export function compileStmts(
  statements: t.Statement[],
  env: Env,
  state: StateTable,
  ctx: StatementContext,
  indent: string,
): string[] {
  return printStmts(lowerStmts(statements, env, state, ctx), indent);
}

/**
 * Compiles a useEffect (App or child) into C. Two shapes:
 *  - `useEffect(fn, [])` — MOUNT-ONCE: body runs after the initial app_update (in er_app_build).
 *  - `useEffect(fn, [dep…])` — DEP-DRIVEN: body becomes a file-scope `er_effect_N()`; runs once at mount,
 *    then again from app_update whenever a SCALAR dep changes (compared against a stored prev). The body is
 *    compiled WITHOUT a trailing app_update — it runs INSIDE app_update / at mount, so re-applying state it
 *    sets happens on the next app_update (one-frame), and it can never re-enter app_update (no infinite loop).
 *    A dep-driven `return () => …` becomes `er_effect_N_cleanup()`, run at the top of the next re-run.
 *
 * @param effect  The effect.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param out  Everything emitted so far; the effect's C is added to it.
 */
export function compileEffect(
  effect: EffectRecord,
  env: Env,
  state: StateTable,
  out: Out,
): void {
  // An expression body is a single statement.
  const body = effect.fn.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];

  // A mount-once effect (no dependency list, or `[]`) runs once, from er_app_build.
  const isMount =
    !effect.deps ||
    (effect.deps.type === 'ArrayExpression' &&
      effect.deps.elements.length === 0);
  if (isMount) {
    const ctx: StatementContext = {
      stateChanged: false,
      animIdx: 0,
      out,
      allowReturn: true,
      bodyList: statements,
    };
    const lines = compileStmts(statements, env, state, ctx, '    ');
    if (!ctx.usedReturn) {
      if (ctx.stateChanged) {
        lines.push(APP_UPDATE_CALL);
      }
      out.mountEffects.push(...lines);

      return;
    }

    // With an early `return` the body gets its own function, so it cannot skip later mount effects.
    const effectName = `er_effect_${out.effN++}`;
    out.effectFns.push({name: effectName, body: lines});
    out.mountEffects.push(`    ${effectName}();`);
    if (ctx.stateChanged) {
      out.mountEffects.push(APP_UPDATE_CALL);
    }

    return;
  }

  // A dep-driven effect gets its own C function; each dep must be scalar, so a change shows against a stored copy.
  if (effect.deps!.type !== 'ArrayExpression') {
    throw aotError(
      'AOT: a useEffect dependency list must be an array literal',
      'pass `[]` (run once) or `[a, b]` (re-run when a/b change).',
    );
  }
  const effectId = out.effN++;
  const effectName = `er_effect_${effectId}`;
  const deps = (effect.deps as t.ArrayExpression).elements.map(depNode => {
    if (!depNode) {
      throw aotError('AOT: a useEffect dependency must be an expression');
    }

    const depExpr = emitExprWide(depNode, env);
    if (!['int', 'float', 'i64', 'string'].includes(depExpr.cType)) {
      throw aotError(
        'AOT: useEffect dependencies must be scalar (number / bool / string)',
        'depend on scalar state values; object/array dependencies are not yet supported.',
      );
    }

    return depExpr;
  });

  // React runs the previous cleanup before a re-run; an `armed` flag records whether the last run reached it.
  const lastStatement = statements[statements.length - 1];
  const hasCleanup =
    lastStatement?.type === 'ReturnStatement' && isFn(lastStatement.argument);
  const armedFlag = `s_eff${effectId}_armed`;
  const ctx: StatementContext = {
    stateChanged: false,
    animIdx: 0,
    out,
    allowReturn: true,
    bodyList: statements,
    depDriven: true,
  };

  // With a cleanup, hoist the body's locals and compile the tail `return () => …` into `<effect>_cleanup()`.
  if (hasCleanup) {
    ctx.hoist = {prefix: `s_eff${effectId}_l_`, decls: out.effectDecls};
    ctx.cleanup = {
      armed: armedFlag,
      emit: (cleanupFn, cleanupEnv) => {
        const cleanupBody = cleanupFn.body;
        const cleanupStatements: t.Statement[] =
          cleanupBody.type === 'BlockStatement'
            ? cleanupBody.body
            : [{type: 'ExpressionStatement', expression: cleanupBody}];
        const cleanupCtx: StatementContext = {
          stateChanged: false,
          animIdx: 0,
          out,
        };
        out.effectFns.push({
          name: `${effectName}_cleanup`,
          body: compileStmts(
            cleanupStatements,
            cleanupEnv,
            state,
            cleanupCtx,
            '    ',
          ),
        });
      },
    };

    out.effectDecls.push(`static int ${armedFlag};`);
  }

  // The effect function runs the last run's cleanup first, if that run armed it, then the body.
  const bodyLines = compileStmts(statements, env, state, ctx, '    ');
  out.effectFns.push({
    name: effectName,
    body: hasCleanup
      ? [
          `    if (${armedFlag})`,
          '    {',
          `        ${armedFlag} = 0;`,
          `        ${effectName}_cleanup();`,
          '    }',
          ...bodyLines,
        ]
      : bodyLines,
  });

  // A static "previous value" per dep; snapshot at mount, then app_update detects changes against it.
  deps.forEach((dep, depIndex) =>
    out.effectDecls.push(
      dep.cType === 'string'
        ? `static char s_eff${effectId}_d${depIndex}[${env.caps.listStrCap}];`
        : `static ${cScalarType(dep.cType)} s_eff${effectId}_d${depIndex};`,
    ),
  );
  const snapshotDep = (depIndex: number, dep: CExpr) =>
    dep.cType === 'string'
      ? `snprintf(s_eff${effectId}_d${depIndex}, sizeof(s_eff${effectId}_d${depIndex}), "%s", ${dep.code})`
      : `s_eff${effectId}_d${depIndex} = ${dep.code}`;

  // At mount, run the effect once and snapshot each dep.
  out.mountEffects.push(
    `    ${effectName}();`,
    ...deps.map((dep, depIndex) => `    ${snapshotDep(depIndex, dep)};`),
  );

  // In app_update, re-snapshot each dep that changed and re-run the effect once if any did.
  const changeCheck = ['    {', '        int er_changed = 0;'];
  deps.forEach((dep, depIndex) => {
    if (dep.cType === 'string') {
      changeCheck.push(
        `        if (strcmp(s_eff${effectId}_d${depIndex}, ${dep.code}) != 0) { ` +
          `${snapshotDep(depIndex, dep)}; er_changed = 1; }`,
      );
    } else {
      const cType = cScalarType(dep.cType);
      changeCheck.push(
        `        ${cType} er_d${depIndex} = ${dep.code}; if (er_d${depIndex} != ` +
          `s_eff${effectId}_d${depIndex}) { s_eff${effectId}_d${depIndex} = er_d${depIndex}; ` +
          `er_changed = 1; }`,
      );
    }
  });

  changeCheck.push(`        if (er_changed) ${effectName}();`, '    }');
  out.depEffects.push(changeCheck.join('\n'));
}

/**
 * Compiles an event handler to the lines of its C function. Its first parameter is the event, whose touch
 * fields read EREventData; a PanResponder callback's second is RN's gestureState (see panGestureField).
 *
 * @param fnNode  The handler.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param out  Everything emitted so far.
 * @param pan  The PanResponder the handler belongs to, if any.
 *
 * @returns The function body's C lines, ending with app_update() when it set state.
 */
export function compileHandler(
  fnNode: PanCallback,
  env: Env,
  state: StateTable,
  out: Out,
  pan: PanResponderRecord | null = null,
): string[] {
  // An expression body is a single statement.
  const body = fnNode.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];

  // The first parameter is the event (fields map to EREventData); a PanResponder's second is RN's gestureState.
  const eventParam =
    fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const gestureParam =
    pan && fnNode.params[1]?.type === 'Identifier'
      ? fnNode.params[1].name
      : null;
  let handlerEnv = env;
  if (eventParam) {
    handlerEnv = {...handlerEnv, event: eventParam};
  }

  if (gestureParam) {
    handlerEnv = {...handlerEnv, gesture: gestureParam, pan};
  }

  // Compile the body, then re-apply state-dependent props once if it set state.
  const ctx: StatementContext = {stateChanged: false, animIdx: 0, out};
  const lines = compileStmts(statements, handlerEnv, state, ctx, '    ');
  if (ctx.stateChanged) {
    lines.push(APP_UPDATE_CALL);
  }

  return lines;
}

/**
 * Compiles a value-callback (e.g., Switch onValueChange) — binds its first param to `valueCode`, not an event.
 *
 * @param fnNode  The callback.
 * @param valueCode  C for the value its first parameter receives.
 * @param env  The expression environment.
 * @param state  The component's state table.
 * @param out  Everything emitted so far.
 * @param cType  The value's C kind.
 * @param secondValueCode  C for a second parameter's value (a range <Dial>'s low end), if any.
 * @param isBool  The value is a boolean, so text prints it as true/false.
 *
 * @returns The function body's C lines, ending with app_update() when it set state.
 */
export function compileValueHandler(
  fnNode: FunctionNode,
  valueCode: string,
  env: Env,
  state: StateTable,
  out: Out,
  cType: CType = 'int',
  secondValueCode: string | null = null,
  isBool = false,
): string[] {
  // A <Switch> hands its callback a boolean; carry that so `'on=' + v` prints true/false, not 1/0.
  const valueParam =
    fnNode.params[0]?.type === 'Identifier' ? fnNode.params[0].name : null;
  const locals = new Map<string, Local>(env.locals);
  if (valueParam) {
    locals.set(valueParam, {code: valueCode, cType, isBool});
  }

  // A RANGE <Dial>'s second parameter binds the same way, lowering to data->value_start with no object on device.
  const secondParam =
    fnNode.params[1]?.type === 'Identifier' ? fnNode.params[1].name : null;
  if (secondParam && secondValueCode) {
    locals.set(secondParam, {code: secondValueCode, cType});
  }

  // Compile the body (an expression body is a single statement), then re-apply state once if it set any.
  const ctx: StatementContext = {stateChanged: false, animIdx: 0, out};
  const body = fnNode.body;
  const statements: t.Statement[] =
    body.type === 'BlockStatement'
      ? body.body
      : [{type: 'ExpressionStatement', expression: body}];
  const lines = compileStmts(statements, {...env, locals}, state, ctx, '    ');
  if (ctx.stateChanged) {
    lines.push(APP_UPDATE_CALL);
  }

  return lines;
}
