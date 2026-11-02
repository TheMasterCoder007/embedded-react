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
  isBool,
});

/** An expression environment with these state slots and nothing else in reach. */
const envWith = (...slots) => ({
  state: new Map(slots.map(slot => [slot.name, slot])),
  locals: new Map(),
  consts: {},
  refs: new Map(),
  math64: false,
});

const textOf = (source, env = envWith()) =>
  lowerText(parseExpression(source), env);

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
