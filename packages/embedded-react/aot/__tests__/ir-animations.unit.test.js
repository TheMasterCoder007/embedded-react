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
import {parse} from '@babel/parser';
import {lowerStmts} from '../compiler/handlers.mts';

import {Out} from '../compiler/out.mts';

// `.start()` lowers to an animation IR; these check its shape and the chain callbacks it adds.

/** One int state slot, `n`, with its setter. */
const nState = {
  kind: 'scalar',
  name: 'n',
  slotKind: 'int',
  isBoolean: false,
  slotName: 'n',
};
const state = {
  byName: new Map([['n', nState]]),
  bySetter: new Map([['setN', nState]]),
};

/** Lowers a handler body that can drive the animated values a, b and c. */
function lowerBody(source) {
  const statements = parse(source, {sourceType: 'module'}).program.body;
  const animations = new Map(
    ['a', 'b', 'c'].map(name => [name, {slotName: `anim_${name}`, initial: 0}]),
  );
  const environment = {
    state: state.byName,
    locals: new Map(),
    constants: {},
    refs: new Map(),
    animations,
    math64: false,
    caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
    stores: [],
  };
  const context = {stateChanged: false, output: new Out(new Map(), null)};
  return {
    statements: lowerStmts(statements, environment, state, context),
    context,
  };
}

/** The one statement a body lowers to. */
function lowerOne(source) {
  const {statements, context} = lowerBody(source);
  expect(statements).toHaveLength(1);
  return {statement: statements[0], context};
}

describe('animation IR: flat starts', () => {
  it('fills spring defaults and keeps bezier control points', () => {
    const {statement: spring} = lowerOne(
      'Animated.spring(a, {toValue: 2}).start();',
    );
    expect(spring.animations[0].curve).toEqual({
      kind: 'spring',
      stiffness: 200,
      damping: 18,
      mass: 1,
    });

    const {statement: bezier} = lowerOne(
      'Animated.timing(a, {toValue: 1, easing: Easing.bezier(0.25, 0.1, 0.25, 1)}).start();',
    );
    expect(bezier.animations[0].curve).toMatchObject({
      easing: 'ER_EASE_BEZIER',
      bezier: [0.25, 0.1, 0.25, 1],
    });
  });

  it('runs an endless loop of one timing on the engine loop', () => {
    const {statement, context} = lowerOne(
      'Animated.loop(Animated.timing(a, {toValue: 1, duration: 500})).start();',
    );
    expect(statement.kind).toBe('startAnimations');
    expect(statement.animations[0].isLooping).toBe(true);
    expect(context.output.animationCallbacks).toHaveLength(0);
  });
});

describe('animation IR: chains', () => {
  it("adds a looped sequence's trailing delay to the restart only", () => {
    const {statement} = lowerOne(
      'Animated.loop(Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.timing(a, {toValue: 0}), Animated.delay(200)])).start();',
    );
    expect(statement.loop.iterations).toBe(-1);
    expect(statement.loop.resetHandle).toBeNull();
    expect(statement.steps[0].delayMs).toBe(0);
    expect(statement.loop.restart.delayMs).toBe(200);
  });
});

describe('animation IR: completions', () => {
  it('completes an empty composition at once, as JS does', () => {
    for (const source of [
      'Animated.sequence([]).start(() => setN(1));',
      'Animated.parallel([]).start(() => setN(1));',
      'Animated.stagger(50, []).start(() => setN(1));',
    ]) {
      const {statement, context} = lowerOne(source);
      expect(statement).toMatchObject({kind: 'completeAfter', delayMs: 0});
      expect(context.output.timerFunctions).toHaveLength(0);
    }
  });

  it('adds no wait when the animation outlasts every delay, or nothing waits on it', () => {
    for (const source of [
      'Animated.parallel([Animated.timing(a, {toValue: 1, duration: 600}), Animated.delay(500)]).start(() => setN(1));',
      'Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(100)]).start();',
    ]) {
      const {statement, context} = lowerOne(source);
      expect(statement.completionWait).toBeNull();
      expect(context.output.timerFunctions).toHaveLength(0);
    }
  });

  it('numbers a chain after its completion, whose own starts come first', () => {
    const {statement} = lowerOne(
      'Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.timing(b, {toValue: 1})]).start(() => { Animated.sequence([Animated.timing(c, {toValue: 1}), Animated.timing(c, {toValue: 0})]).start(); });',
    );
    expect(statement.completion.name).toBe('done_0');
    expect(statement.completion.body[0]).toMatchObject({
      kind: 'startChain',
      id: 1,
    });
    expect(statement.id).toBe(2);
  });
});

describe('animation IR: stopping a wait', () => {
  it('gives each composition written apart its own owner, even when they are alike', () => {
    const {statements, context} = lowerBody(
      'Animated.delay(500).start(() => setN(1)); Animated.delay(500).start(() => setN(2)); Animated.timing(a, {toValue: 1}).stop(); Animated.delay(500).stop();',
    );
    expect(statements[0].owner).toBe(0);
    expect(statements[1].owner).toBe(1);
    expect(statements[2]).toMatchObject({
      kind: 'stopAnimations',
      handles: ['anim_a'],
      waitOwner: null,
    });

    // A stop of a third delay names an owner of its own, which nothing starts (assembly drops the stop).
    expect(statements[3]).toMatchObject({handles: [], waitOwner: 2});
    expect([...context.output.animationWaits.values()]).toEqual([
      {owner: 0, isStarted: true},
      {owner: 1, isStarted: true},
      {owner: 2, isStarted: false},
    ]);
  });

  it('gives a stop that comes first the owner a later start waits under', () => {
    const {statements, context} = lowerBody(
      'const pulse = Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(300)]); pulse.stop(); pulse.start(() => setN(1));',
    );
    const [stop, start] = statements;
    expect(stop.waitOwner).toBe(0);
    expect(start.completionWait.owner).toBe(0);
    expect([...context.output.animationWaits.values()]).toEqual([
      {owner: 0, isStarted: true},
    ]);
  });
});
