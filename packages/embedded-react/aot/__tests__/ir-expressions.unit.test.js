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

// Expressions lower to the IR first; these check the IR itself.

/** A scalar state slot, as collectState records one. */
const scalar = (name, slotKind, isBool = false) => ({
  kind: 'scalar',
  name,
  slotKind,
  isBoolean: isBool,
});

/** An expression environment with these state slots and nothing else in reach. */
const envWith = (...slots) => ({
  state: new Map(slots.map(slot => [slot.name, slot])),
  locals: new Map(),
  constants: {},
  refs: new Map(),
  math64: false,
});

const lower = (source, environment = envWith()) =>
  lowerExprWide(parseExpression(source), environment);

describe('expressions lower to the IR', () => {
  it('folds `flag === 1` to false, since a boolean never equals a number in JS', () => {
    const expression = lower(
      'flag === 1',
      envWith(scalar('flag', 'int', true)),
    );
    expect(expression).toMatchObject({
      kind: 'boolean',
      value: false,
      type: {kind: 'bool'},
    });
  });

  it('folds whole-number constants exactly past the int range', () => {
    const expression = lower('2147483647 + 1');
    expect(expression).toMatchObject({
      kind: 'number',
      value: 2147483648,
      type: {kind: 'int64', source: 'number'},
    });
  });

  it('refuses a Math name that is not a function it lowers', () => {
    expect(() =>
      lower('Math.toString(count)', envWith(scalar('count', 'int'))),
    ).toThrow(/unsupported Math\.toString/);
  });

  it('refuses an onLayout name that is not a rect field', () => {
    expect(() =>
      lower('e.layout.toString', {...envWith(), event: 'e', layout: true}),
    ).toThrow(/unknown onLayout rect field "toString"/);
  });
});

describe('expressions: the source and the typing pass', () => {
  /** An expression's source and its type, over the given state. */
  const both = (source, environment = envWith()) =>
    lowerSource(parseExpression(source), environment);

  it('records what JS asked for, and types it apart', () => {
    const {source, expression} = both(
      'count + 1',
      envWith(scalar('count', 'int')),
    );
    expect(source).toMatchObject({
      kind: 'binary',
      operator: '+',
      left: {kind: 'state'},
      right: {kind: 'number', value: 1},
      divisor: {constant: 1},
    });
    expect(source).not.toHaveProperty('type');
    expect(typeSource(source, false)).toEqual(expression);
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
    const environment = envWith(scalar('a', 'int'), scalar('ms', 'i64'));
    const {source, expression} = both('Math.floor(a / 3)', environment);
    expect(source).toMatchObject({
      kind: 'math',
      mathFunction: 'floor',
      exactDivision: true,
      callArguments: [{kind: 'binary', operator: '/'}],
    });
    expect(expression).toMatchObject({
      kind: 'helper',
      helper: 'floordiv',
      bits: 32,
    });
    expect(both('Math.floor(ms / 1000)', environment).expression).toMatchObject(
      {
        helper: 'floordiv',
        bits: 64,
        type: {kind: 'int64', source: 'time'},
      },
    );
  });

  it('reports a typing error at the expression it is about', () => {
    const environment = envWith(scalar('ms', 'i64'), scalar('n', 'int'));
    let error;
    try {
      both('1 + Math.floor(ms / n)', environment);
    } catch (caught) {
      error = caught;
    }
    expect(error.message).toBe(
      'AOT: a 64-bit time value can only be divided by a constant',
    );
    expect(error.aotLoc).toMatchObject({line: 1, column: 20});
  });
});
