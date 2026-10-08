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
import {lowerExprWide} from '../compiler/expressions.mts';
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
      cType: 'int',
    });
    expect(printExpr(expr)).toBe('app_add(s_state.count, 1)');
  });

  it('keeps a timestamp difference in 64 bits', () => {
    const expr = lower('Date.now() - start', envWith(scalar('start', 'i64')));
    expect(expr).toMatchObject({
      kind: 'helper',
      helper: 'sub',
      bits: 64,
      cType: 'i64',
      wide: false,
    });
    expect(expr.args[0]).toMatchObject({kind: 'clock', clock: 'Date'});
    expect(printExpr(expr)).toBe('app_sub64(app_date_now(), s_state.start)');
  });

  it('narrows a timestamp remainder by a small constant to an int', () => {
    const expr = lower('ms % 1000', envWith(scalar('ms', 'i64')));
    expect(expr).toMatchObject({kind: 'narrowRemainder', cType: 'int'});
    expect(printExpr(expr)).toBe('((int)(s_state.ms % 1000))');
  });

  it('divides in floats, as JS does', () => {
    const expr = lower('count / 2', envWith(scalar('count', 'int')));
    expect(expr).toMatchObject({kind: 'floatDivide', cType: 'float'});
    expect(printExpr(expr)).toBe('((float)(s_state.count) / (float)(2))');
  });

  it('rounds a float through the float function and back to an int', () => {
    const expr = lower('Math.floor(level)', envWith(scalar('level', 'float')));
    expect(expr).toMatchObject({kind: 'toInt', cType: 'int'});
    expect(expr.operand).toMatchObject({kind: 'floatMath', fn: 'floor'});
    expect(printExpr(expr)).toBe('app_f2i(floorf((float)(s_state.level)))');
  });

  it('leaves an int as it is under Math.round', () => {
    const expr = lower('Math.round(count)', envWith(scalar('count', 'int')));
    expect(expr).toMatchObject({kind: 'wholeRound', cType: 'int'});
    expect(printExpr(expr)).toBe('s_state.count');
  });

  it('compares strings for equality', () => {
    const expr = lower("mode === 'on'", envWith(scalar('mode', 'string')));
    expect(expr).toMatchObject({
      kind: 'stringEquals',
      op: '==',
      isBool: true,
    });
    expect(printExpr(expr)).toBe('(strcmp(s_state.mode, "on") == 0)');
  });

  it('folds `flag === 1` to false, since a boolean never equals a number in JS', () => {
    const expr = lower('flag === 1', envWith(scalar('flag', 'int', true)));
    expect(expr).toMatchObject({kind: 'boolean', value: false, isBool: true});
  });

  it('keeps the operand `||` returns when it is 64-bit', () => {
    const expr = lower('last || Date.now()', envWith(scalar('last', 'i64')));
    expect(expr).toMatchObject({kind: 'pick', op: '||', cType: 'i64'});
    expect(printExpr(expr)).toBe(
      '(s_state.last ? s_state.last : app_date_now())',
    );
  });

  it('folds whole-number constants exactly past the int range', () => {
    const expr = lower('2147483647 + 1');
    expect(expr).toMatchObject({
      kind: 'number',
      value: 2147483648,
      cType: 'i64',
      lit: true,
    });
  });

  it('spells the smallest int without overflowing the literal', () => {
    expect(printExpr(lower('-2147483648'))).toBe('(-2147483647 - 1)');
  });

  it("reads a list's runtime count for `.length`", () => {
    const expr = lower('items.length', envWith(list('items')));
    expect(expr).toMatchObject({kind: 'listLength', cType: 'int'});
    expect(printExpr(expr)).toBe('s_items_count');
  });

  it('refuses a Math name that is not a function it lowers', () => {
    expect(() =>
      lower('Math.toString(count)', envWith(scalar('count', 'int'))),
    ).toThrow(/unsupported Math\.toString/);
  });
});
