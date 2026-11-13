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
import {buildsText, lowerText} from '../compiler/expressions.mts';

// Text lowers to IR parts first; these check the parts.

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
  math64: undefined,
});

const textOf = (source, environment = envWith()) =>
  lowerText(parseExpression(source), environment);

describe('text lowers to IR parts', () => {
  it('renders a boolean on its own as nothing, as React does', () => {
    const text = textOf('flag', envWith(scalar('flag', 'int', true)));
    expect(text.parts).toEqual([{kind: 'literal', text: ''}]);
  });

  it('refuses a ternary that concatenates in a branch', () => {
    expect(() =>
      textOf(
        "on ? 'n=' + count : 'off'",
        envWith(scalar('on', 'int', true), scalar('count', 'int')),
      ),
    ).toThrow(/cannot concatenate inside its test or a branch/);
  });
});

describe('template literals and string helpers', () => {
  const environment = () => ({
    ...envWith(
      scalar('count', 'int'),
      scalar('level', 'float'),
      scalar('name', 'string'),
      scalar('on', 'int', true),
    ),
    refs: new Map([
      ['box', {kind: 'node', slotName: 'box', slotKind: 'node', used: true}],
    ]),
  });
  const kinds = text =>
    text.parts.map(part =>
      part.kind === 'literal'
        ? part.text
        : {[part.kind]: part.value.state?.name ?? part.value.kind},
    );

  it('splits a template literal into its strings and values, printing a boolean as a word', () => {
    expect(
      kinds(textOf('`${count} of ${name}, ${on}`', environment())),
    ).toEqual([
      '',
      {value: 'count'},
      ' of ',
      {value: 'name'},
      ', ',
      {booleanWord: 'on'},
      '',
    ]);
  });

  it('folds a constant template and constant helpers to literal text', () => {
    expect(textOf('`v${1 + 1}: ${(1.25).toFixed(1)}`').parts).toEqual([
      {kind: 'literal', text: 'v2: 1.3'},
    ]);
    expect(textOf("String(7).padStart(3, '0')").parts).toEqual([
      {kind: 'literal', text: '007'},
    ]);
  });

  it('prints String(x) as a `+` operand would', () => {
    expect(kinds(textOf('String(on)', environment()))).toEqual([
      {booleanWord: 'on'},
    ]);
  });

  it('keeps toFixed digits and padStart calls on the value they print', () => {
    const [fixed] = textOf('level.toFixed(2)', environment()).parts;
    expect(fixed).toMatchObject({kind: 'value', digits: 2});
    const [padded] = textOf(
      "count.toFixed(1).padStart(6).padStart(8, '*')",
      environment(),
    ).parts;
    expect(padded).toMatchObject({
      digits: 1,
      edits: [
        {kind: 'pad', width: 6, fill: ' '},
        {kind: 'pad', width: 8, fill: '*'},
      ],
    });
  });

  it('pads a runtime boolean printed by String()', () => {
    expect(textOf("String(on).padStart(5, '0')", environment()).parts).toEqual([
      {
        kind: 'booleanWord',
        value: expect.anything(),
        edits: [{kind: 'pad', width: 5, fill: '0'}],
      },
    ]);
  });

  it('folds slice, toUpperCase and includes on a constant', () => {
    expect(textOf("'Hello'.slice(1, -1)").parts).toEqual([
      {kind: 'literal', text: 'ell'},
    ]);
    expect(textOf("'straße'.toUpperCase()").parts).toEqual([
      {kind: 'literal', text: 'STRASSE'},
    ]);
    expect(textOf("`${'abc'.includes('b')}`").parts).toEqual([
      {kind: 'literal', text: 'true'},
    ]);
  });

  it('keeps slice on the value it cuts, cutting a constant position as JS does', () => {
    const position = value => ({kind: 'number', value});
    expect(textOf('name.slice(1, -1)', environment()).parts).toMatchObject([
      {
        kind: 'value',
        edits: [{kind: 'slice', start: position(1), end: position(-1)}],
      },
    ]);
    expect(textOf('name.slice(-3)', environment()).parts).toMatchObject([
      {edits: [{start: position(-3), end: position(0x7fff)}]},
    ]);
    expect(textOf('name.slice(1.7, 99999)', environment()).parts).toMatchObject(
      [{edits: [{start: position(1), end: position(0x7fff)}]}],
    );
    expect(textOf('name.slice(count)', environment()).parts).toMatchObject([
      {edits: [{start: {kind: 'state'}, end: position(0x7fff)}]},
    ]);
  });

  it('applies the helpers on a piece in the order they are called', () => {
    const [piece] = textOf(
      "name.slice(0, 3).padStart(5, '-').toUpperCase()",
      environment(),
    ).parts;
    expect(piece.edits.map(edit => edit.kind)).toEqual([
      'slice',
      'pad',
      'upper',
    ]);
  });

  it('upper-cases literal text now and each value with letters as it prints', () => {
    const parts = textOf(
      '`id ${name}: ${count}, ${level}, ${on}`.toUpperCase()',
      environment(),
    ).parts;
    expect(
      parts.map(part => part.text ?? part.edits?.[0]?.kind ?? null),
    ).toEqual(['ID ', 'upper', ': ', null, ', ', 'upper', ', ', 'upper', '']);
  });

  it('prints includes as a boolean word in a template', () => {
    expect(
      textOf("`${name.includes('a')}`", environment()).parts,
    ).toMatchObject([
      {kind: 'literal', text: ''},
      {
        kind: 'booleanWord',
        value: {
          kind: 'stringIncludes',
          haystack: {kind: 'state'},
          needle: {kind: 'string', value: 'a'},
        },
      },
      {kind: 'literal', text: ''},
    ]);
  });

  it("leaves a list's slice to the list", () => {
    const lists = {
      ...environment(),
      state: new Map([
        ...environment().state,
        ['items', {kind: 'list', name: 'items'}],
      ]),
    };
    expect(buildsText(parseExpression('items.slice(0, 2)'), lists)).toBe(false);
    expect(buildsText(parseExpression('name.slice(1)'), lists)).toBe(true);
  });

  it('leaves String() alone where a runtime value named String hides the global', () => {
    const shadowed = envWith(
      scalar('String', 'string'),
      scalar('count', 'int'),
    );
    expect(() => textOf('String(7)', shadowed)).toThrow();
    expect(() => textOf('`${String(count)}`', shadowed)).toThrow();
  });

  it.each([
    ['name.toFixed(1)', /toFixed is only supported on a number/],
    ['on.toFixed(1)', /toFixed is only supported on a number/],
    ['box.current.toFixed(1)', /toFixed is only supported on a number/],
    ['level.toFixed(count)', /toFixed's digits must be a compile-time/],
    ['level.toFixed(21)', /toFixed\(21\) is out of range/],
    ["count.padStart(2, '0')", /padStart is only supported on a string/],
    ['`${count}:${count}`.padStart(5)', /padStart can only pad one value/],
    ["name.padStart(4, 'ab')", /fill must be one printable ASCII character/],
    ['name.padStart(300)', /longer than any text/],
    ['count.slice(1)', /slice is only supported on a string/],
    ['`${name}-${name}`.slice(1)', /slice can only cut one value/],
    ['name.slice(level)', /slice's start and end must be whole numbers/],
    ["name.slice('a')", /slice's start and end must be numbers/],
    ["'abc'.slice('a')", /slice's start and end must be numbers/],
    ['count.toFixed(2).slice(1)', /cannot cut a whole number's toFixed/],
    ['count.toUpperCase()', /toUpperCase is only supported on a string/],
    ["`${name.includes('a', 1)}`", /includes' position is not supported/],
    ["`${'abc'.includes('b', 1)}`", /includes' position is not supported/],
    ['`${name.includes(count)}`', /includes only searches for a string/],
    ["`${'abc'.includes(1)}`", /includes only searches for a string/],
    ["`${count.includes('a')}`", /includes is only supported on a string/],
    ["`${`${name}!`.includes('a')}`", /not one built in place/],
    [
      "on ? level.toFixed(1) : '--'",
      /cannot concatenate inside its test or a branch/,
    ],
  ])('refuses %s', (source, message) => {
    expect(() => textOf(source, environment())).toThrow(message);
  });
});
