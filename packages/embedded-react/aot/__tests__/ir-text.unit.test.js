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
import {lowerText} from '../compiler/expressions.mts';

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
  math64: false,
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
      pads: [
        {width: 6, fill: ' '},
        {width: 8, fill: '*'},
      ],
    });
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
    [
      "on ? level.toFixed(1) : '--'",
      /cannot concatenate inside its test or a branch/,
    ],
  ])('refuses %s', (source, message) => {
    expect(() => textOf(source, environment())).toThrow(message);
  });
});
