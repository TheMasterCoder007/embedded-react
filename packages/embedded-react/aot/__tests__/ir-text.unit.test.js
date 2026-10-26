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
import {printText, formatArgs} from '../compiler/c/text.mts';

// Text lowers to IR parts first; these check the parts, and the printf format the C printer makes of them.

/** A scalar state slot, as collectState records one. */
const scalar = (name, cType, isBool = false) => ({
  kind: 'scalar',
  name,
  cType,
  isBool,
  cMember: `s_state.${name}`,
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
  it('splits a string concatenation into literal text and values', () => {
    const text = textOf("'n=' + count", envWith(scalar('count', 'int')));
    expect(text.parts).toMatchObject([
      {kind: 'literal', text: 'n='},
      {kind: 'value', value: {kind: 'state'}},
    ]);
    expect(printText(text)).toEqual({format: 'n=%d', args: ['s_state.count']});
  });

  it("adds before it appends, as JS does with `count + 1 + 'ms'`", () => {
    const text = textOf("count + 1 + 'ms'", envWith(scalar('count', 'int')));
    expect(text.parts).toMatchObject([
      {kind: 'value', value: {kind: 'helper', helper: 'add'}},
      {kind: 'literal', text: 'ms'},
    ]);
    expect(printText(text)).toEqual({
      format: '%dms',
      args: ['app_add(s_state.count, 1)'],
    });
  });

  it('prints a boolean operand of `+` as true or false', () => {
    const text = textOf("'on: ' + flag", envWith(scalar('flag', 'int', true)));
    expect(text.parts[1]).toMatchObject({kind: 'booleanWord'});
    expect(printText(text)).toEqual({
      format: 'on: %s',
      args: ['((s_state.flag) ? "true" : "false")'],
    });
  });

  it('renders a boolean on its own as nothing, as React does', () => {
    const text = textOf('flag', envWith(scalar('flag', 'int', true)));
    expect(text.parts).toEqual([{kind: 'literal', text: ''}]);
  });

  it('prints each kind of value with the spec that fits it', () => {
    const env = envWith(
      scalar('level', 'float'),
      scalar('ms', 'i64'),
      scalar('label', 'string'),
    );
    expect(printText(textOf('level', env))).toEqual({
      format: '%s',
      args: ['app_ftoa((char[16]){0}, s_state.level)'],
    });
    expect(printText(textOf('ms', env))).toEqual({
      format: '%lld',
      args: ['(long long)(s_state.ms)'],
    });
    expect(printText(textOf('label', env))).toEqual({
      format: '%s',
      args: ['s_state.label'],
    });
  });

  it('escapes a literal `%` in the format', () => {
    const text = textOf("'100% ' + count", envWith(scalar('count', 'int')));
    expect(printText(text).format).toBe('100%% %d');
  });

  it("keeps constant text in snprintf's `%s` form", () => {
    expect(formatArgs(printText(textOf("'100%'")))).toBe('"%s", "100%"');
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
