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

import {describe, it, expect} from 'vitest';
import {parseExpression} from '@babel/parser';
import {
  lowerExprWide,
  lowerSource,
  typeSource,
} from '../compiler/expressions.mts';
import {printExpr} from '../compiler/c/expressions.mts';

// Expressions lower to the IR first; these check the IR itself, and the C its printer makes of it.

/** A scalar state slot, as collectState records one. */
const scalar = (name, cType, isBool = false) => ({
  kind: 'scalar',
  name,
  cType,
  isBool,
  cMember: `s_state.${name}`,
});

/** A list state slot with its runtime count. */
const list = name => ({
  kind: 'list',
  name,
  countMember: `s_${name}_count`,
});

/** An expression environment with these state slots and nothing else in reach. */
const envWith = (...slots) => ({
  state: new Map(slots.map(slot => [slot.name, slot])),
  locals: new Map(),
  consts: {},
  refs: new Map(),
  math64: false,
});

const lower = (source, env = envWith()) =>
  lowerExprWide(parseExpression(source), env);

describe('expressions lower to the IR', () => {
  it('lowers whole-number `+` to the saturating 32-bit helper', () => {
    const expr = lower('count + 1', envWith(scalar('count', 'int')));
    expect(expr).toMatchObject({
      kind: 'helper',
      helper: 'add',
      bits: 32,
      type: {kind: 'int'},
    });
    expect(printExpr(expr)).toBe('app_add(s_state.count, 1)');
  });

  it('keeps a timestamp difference in 64 bits', () => {
    const expr = lower('Date.now() - start', envWith(scalar('start', 'i64')));
    expect(expr).toMatchObject({
      kind: 'helper',
      helper: 'sub',
      bits: 64,
      type: {kind: 'int64', source: 'time'},
    });
    expect(expr.args[0]).toMatchObject({kind: 'clock', clock: 'Date'});
    expect(printExpr(expr)).toBe('app_sub64(app_date_now(), s_state.start)');
  });

  it('narrows a timestamp remainder by a small constant to an int', () => {
    const expr = lower('ms % 1000', envWith(scalar('ms', 'i64')));
    expect(expr).toMatchObject({kind: 'narrowRemainder', type: {kind: 'int'}});
    expect(printExpr(expr)).toBe('((int)(s_state.ms % 1000))');
  });

  it('divides in floats, as JS does', () => {
    const expr = lower('count / 2', envWith(scalar('count', 'int')));
    expect(expr).toMatchObject({kind: 'floatDivide', type: {kind: 'float'}});
    expect(printExpr(expr)).toBe('((float)(s_state.count) / (float)(2))');
  });

  it('rounds a float through the float function and back to an int', () => {
    const expr = lower('Math.floor(level)', envWith(scalar('level', 'float')));
    expect(expr).toMatchObject({kind: 'toInt', type: {kind: 'int'}});
    expect(expr.operand).toMatchObject({kind: 'floatMath', fn: 'floor'});
    expect(printExpr(expr)).toBe('app_f2i(floorf((float)(s_state.level)))');
  });

  it('leaves an int as it is under Math.round', () => {
    const expr = lower('Math.round(count)', envWith(scalar('count', 'int')));
    expect(expr).toMatchObject({kind: 'wholeRound', type: {kind: 'int'}});
    expect(printExpr(expr)).toBe('s_state.count');
  });

  it('compares strings for equality', () => {
    const expr = lower("mode === 'on'", envWith(scalar('mode', 'string')));
    expect(expr).toMatchObject({
      kind: 'stringEquals',
      op: '==',
      type: {kind: 'bool'},
    });
    expect(printExpr(expr)).toBe('(strcmp(s_state.mode, "on") == 0)');
  });

  it('folds `flag === 1` to false, since a boolean never equals a number in JS', () => {
    const expr = lower('flag === 1', envWith(scalar('flag', 'int', true)));
    expect(expr).toMatchObject({
      kind: 'boolean',
      value: false,
      type: {kind: 'bool'},
    });
  });

  it('keeps the operand `||` returns when it is 64-bit', () => {
    const expr = lower('last || Date.now()', envWith(scalar('last', 'i64')));
    expect(expr).toMatchObject({
      kind: 'pick',
      op: '||',
      type: {kind: 'int64', source: 'time'},
    });
    expect(printExpr(expr)).toBe(
      '(s_state.last ? s_state.last : app_date_now())',
    );
  });

  it('folds whole-number constants exactly past the int range', () => {
    const expr = lower('2147483647 + 1');
    expect(expr).toMatchObject({
      kind: 'number',
      value: 2147483648,
      type: {kind: 'int64', source: 'number'},
    });
  });

  it('spells the smallest int without overflowing the literal', () => {
    expect(printExpr(lower('-2147483648'))).toBe('(-2147483647 - 1)');
  });

  it("reads a list's runtime count for `.length`", () => {
    const expr = lower('items.length', envWith(list('items')));
    expect(expr).toMatchObject({kind: 'listLength', type: {kind: 'int'}});
    expect(printExpr(expr)).toBe('s_items_count');
  });

  it('refuses a Math name that is not a function it lowers', () => {
    expect(() =>
      lower('Math.toString(count)', envWith(scalar('count', 'int'))),
    ).toThrow(/unsupported Math\.toString/);
  });

  it("reads an onLayout rect field from the engine's rect", () => {
    const expr = lower('e.layout.width', {...envWith(), event: 'e'});
    expect(expr).toMatchObject({
      kind: 'layout',
      field: 'width',
      type: {kind: 'int'},
    });
    expect(printExpr(expr)).toBe('data->layout_rect.w');
  });

  it('refuses an onLayout name that is not a rect field', () => {
    expect(() =>
      lower('e.layout.toString', {...envWith(), event: 'e'}),
    ).toThrow(/unknown onLayout rect field "toString"/);
  });
});

describe('expressions: the source and the typing pass', () => {
  /** An expression's source and its type, over the given state. */
  const both = (source, env = envWith()) =>
    lowerSource(parseExpression(source), env);

  it('records what JS asked for, and types it apart', () => {
    const {source, expr} = both('count + 1', envWith(scalar('count', 'int')));
    expect(source).toMatchObject({
      kind: 'binary',
      op: '+',
      left: {kind: 'state'},
      right: {kind: 'number', value: 1},
      divisor: {constant: 1},
    });
    expect(source).not.toHaveProperty('type');
    expect(typeSource(source, false)).toEqual(expr);
  });

  it('types the same source again as 64-bit math beside a time', () => {
    const {source} = both('count + 1', envWith(scalar('count', 'int')));
    expect(typeSource(source, false)).toMatchObject({
      kind: 'helper',
      bits: 32,
      type: {kind: 'int'},
    });
    expect(typeSource(source, true)).toMatchObject({
      kind: 'helper',
      bits: 64,
      type: {kind: 'int64', source: 'math'},
    });
  });

  it('keeps a division under Math.floor apart, so whole numbers divide exactly', () => {
    const env = envWith(scalar('a', 'int'), scalar('ms', 'i64'));
    const {source, expr} = both('Math.floor(a / 3)', env);
    expect(source).toMatchObject({
      kind: 'math',
      fn: 'floor',
      exactDivision: true,
      args: [{kind: 'binary', op: '/'}],
    });
    expect(expr).toMatchObject({kind: 'helper', helper: 'floordiv', bits: 32});
    expect(both('Math.floor(ms / 1000)', env).expr).toMatchObject({
      helper: 'floordiv',
      bits: 64,
      type: {kind: 'int64', source: 'time'},
    });
  });

  it('reports a typing error at the expression it is about', () => {
    const env = envWith(scalar('ms', 'i64'), scalar('n', 'int'));
    let error;
    try {
      both('1 + Math.floor(ms / n)', env);
    } catch (caught) {
      error = caught;
    }
    expect(error.message).toBe(
      'AOT: a 64-bit time value can only be divided by a constant',
    );
    expect(error.aotLoc).toMatchObject({line: 1, column: 20});
  });
});
