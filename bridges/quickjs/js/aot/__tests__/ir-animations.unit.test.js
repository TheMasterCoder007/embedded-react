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
import {printStmts} from '../compiler/c/statements.mts';
import {Out} from '../compiler/out.mts';

// `.start()` lowers to an animation IR; these check its shape, the chain callbacks it adds, and the C it prints.

/** One int state slot, `n`, with its setter. */
const nState = {
  kind: 'scalar',
  name: 'n',
  cType: 'int',
  isBool: false,
  cField: 'n',
  cMember: 's_state.n',
};
const state = {
  byName: new Map([['n', nState]]),
  bySetter: new Map([['setN', nState]]),
};

/** Lowers a handler body that can drive the animated values a, b and c. */
function lowerBody(source) {
  const statements = parse(source, {sourceType: 'module'}).program.body;
  const animations = new Map(
    ['a', 'b', 'c'].map(name => [
      name,
      {cVar: `s_av_${name}`, initCode: '0.0f'},
    ]),
  );
  const env = {
    state: state.byName,
    locals: new Map(),
    consts: {},
    refs: new Map(),
    animations,
    math64: false,
    caps: {listCap: 8, listStrCap: 48, maxTextSpans: 4},
    found: new Set(),
  };
  const ctx = {stateChanged: false, animIdx: 0, out: new Out(new Map(), null)};
  return {stmts: lowerStmts(statements, env, state, ctx), ctx};
}

/** The one statement a body lowers to. */
function lowerOne(source) {
  const {stmts, ctx} = lowerBody(source);
  expect(stmts).toHaveLength(1);
  return {stmt: stmts[0], ctx};
}

describe('animation IR: flat starts', () => {
  it('lowers a timing to one animation with its folded curve', () => {
    const {stmt, ctx} = lowerOne(
      'Animated.timing(a, {toValue: 1, duration: 300, delay: 50}).start();',
    );
    expect(stmt.kind).toBe('startAnimations');
    expect(stmt.firstConfig).toBe(0);
    expect(stmt.completion).toBeNull();
    expect(stmt.animations).toHaveLength(1);
    const [animation] = stmt.animations;
    expect(animation).toMatchObject({
      handle: 's_av_a',
      curve: {
        kind: 'timing',
        durationMs: 300,
        easing: 'ER_EASE_EASE_IN_OUT',
        bezier: null,
      },
      to: {kind: 'number', value: 1},
      delayMs: 50,
      isLooping: false,
    });
    expect(ctx.animIdx).toBe(1);

    const c = printStmts([stmt], '    ').join('\n');
    expect(c).toContain('cfg0.duration_ms = 300;');
    expect(c).toContain('cfg0.delay_ms = 50;');
    expect(c).toContain('er_anim_value_animate(s_av_a, (float)(1), &cfg0);');
  });

  it('fills spring defaults and keeps bezier control points', () => {
    const {stmt: spring} = lowerOne(
      'Animated.spring(a, {toValue: 2}).start();',
    );
    expect(spring.animations[0].curve).toEqual({
      kind: 'spring',
      stiffness: 200,
      damping: 18,
      mass: 1,
    });

    const {stmt: bezier} = lowerOne(
      'Animated.timing(a, {toValue: 1, easing: Easing.bezier(0.25, 0.1, 0.25, 1)}).start();',
    );
    expect(bezier.animations[0].curve).toMatchObject({
      easing: 'ER_EASE_BEZIER',
      bezier: [0.25, 0.1, 0.25, 1],
    });
  });

  it('gives a decay with no target the value it holds now', () => {
    const {stmt} = lowerOne('Animated.decay(c, {velocity: 0.5}).start();');
    expect(stmt.animations[0].to).toBeNull();
    expect(stmt.animations[0].curve).toEqual({
      kind: 'decay',
      deceleration: 0.998,
      velocity: 0.5,
    });
    expect(printStmts([stmt], '    ').join('\n')).toContain(
      'er_anim_value_animate(s_av_c, (float)(er_anim_value_get(s_av_c)), &cfg0);',
    );
  });

  it('folds a stagger into delays and numbers each config', () => {
    const {stmt, ctx} = lowerOne(
      'Animated.stagger(40, [Animated.timing(a, {toValue: 1}), Animated.timing(b, {toValue: 1}), Animated.timing(c, {toValue: 1})]).start();',
    );
    expect(stmt.animations.map(animation => animation.delayMs)).toEqual([
      0, 40, 80,
    ]);
    expect(ctx.animIdx).toBe(3);
    const c = printStmts([stmt], '    ').join('\n');
    expect(c).toContain('&cfg2);');
  });

  it('runs an endless loop of one timing on the engine loop', () => {
    const {stmt, ctx} = lowerOne(
      'Animated.loop(Animated.timing(a, {toValue: 1, duration: 500})).start();',
    );
    expect(stmt.kind).toBe('startAnimations');
    expect(stmt.animations[0].isLooping).toBe(true);
    expect(ctx.out.animCbs).toHaveLength(0);
  });
});

describe('animation IR: chains', () => {
  it('lowers a sequence to a chain whose step 1 starts from a callback', () => {
    const {stmt, ctx} = lowerOne(
      'Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(30), Animated.spring(a, {toValue: 0})]).start();',
    );
    expect(stmt.kind).toBe('startChain');
    expect(stmt.id).toBe(0);
    expect(stmt.loop).toBeNull();
    expect(stmt.steps.map(step => step.curve.kind)).toEqual([
      'timing',
      'spring',
    ]);
    expect(stmt.steps[1].delayMs).toBe(30);

    // Step 0 starts inline and hands over to step 1's callback, which ends the chain when interrupted.
    expect(printStmts([stmt], '    ').join('\n')).toContain(
      'cfg0_0.on_complete = er_seqcb_0_1;',
    );
    expect(ctx.out.animCbs.map(fn => fn.name)).toEqual(['er_seqcb_0_1']);
    expect(ctx.out.animCbs[0].body.slice(0, 2)).toEqual([
      '    if (!finished)',
      '    {',
    ]);
  });

  it('lowers a counted spring loop with its reset, restart and slots', () => {
    const {stmt, ctx} = lowerOne(
      'Animated.loop(Animated.spring(b, {toValue: 2}), {iterations: 3}).start();',
    );
    expect(stmt.kind).toBe('startChain');
    expect(stmt.loop).toMatchObject({
      iterations: 3,
      resetHandle: 's_av_b',
      restart: {handle: 's_av_b', delayMs: 0},
    });
    expect(ctx.out.effectDecls).toEqual([
      'static int s_loop0_n;',
      'static float s_loop0_from;',
    ]);
    expect(printStmts([stmt], '    ').slice(0, 2)).toEqual([
      '    s_loop0_n = 1;',
      '    s_loop0_from = er_anim_value_get(s_av_b);',
    ]);
    const repeat = ctx.out.animCbs.find(fn => fn.name === 'er_seqcb_0_0');
    expect(repeat.body).toContain('    if (s_loop0_n >= 3)');
    expect(repeat.body).toContain(
      '    er_anim_value_set(s_av_b, s_loop0_from);',
    );
  });

  it("adds a looped sequence's trailing delay to the restart only", () => {
    const {stmt} = lowerOne(
      'Animated.loop(Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.timing(a, {toValue: 0}), Animated.delay(200)])).start();',
    );
    expect(stmt.loop.iterations).toBe(-1);
    expect(stmt.loop.resetHandle).toBeNull();
    expect(stmt.steps[0].delayMs).toBe(0);
    expect(stmt.loop.restart.delayMs).toBe(200);
  });
});

describe('animation IR: completions', () => {
  it('lowers the callback body to statements and re-applies state after it', () => {
    const {stmt, ctx} = lowerOne(
      'Animated.timing(a, {toValue: 1}).start(({finished}) => { if (finished) { setN(1); } });',
    );
    expect(stmt.completion.name).toBe('er_donecb_0');
    expect(stmt.completion.updatesState).toBe(true);
    expect(stmt.completion.body).toHaveLength(1);
    expect(stmt.completion.body[0]).toMatchObject({
      kind: 'if',
      consequent: [{kind: 'setState', state: nState}],
    });

    // The animation is the only one, so it carries the completion; a start sets no state itself.
    expect(printStmts([stmt], '    ').join('\n')).toContain(
      'cfg0.on_complete = er_donecb_0;',
    );
    expect(ctx.stateChanged).toBe(false);
    expect(ctx.out.animCbs[0].body.at(-1)).toBe('    app_update();');
  });

  it('completes a zero-iteration loop at once', () => {
    const {stmt} = lowerOne(
      'Animated.loop(Animated.timing(a, {toValue: 1}), {iterations: 0}).start(() => setN(2));',
    );
    expect(stmt).toMatchObject({
      kind: 'completeAfter',
      delayMs: 0,
      timerFn: null,
    });
    expect(printStmts([stmt], '    ')).toEqual([
      '    er_donecb_0(true, NULL);',
    ]);
  });

  it('completes an empty composition at once, as JS does', () => {
    for (const source of [
      'Animated.sequence([]).start(() => setN(1));',
      'Animated.parallel([]).start(() => setN(1));',
      'Animated.stagger(50, []).start(() => setN(1));',
    ]) {
      const {stmt, ctx} = lowerOne(source);
      expect(stmt).toMatchObject({kind: 'completeAfter', delayMs: 0});
      expect(ctx.out.timerFns).toHaveLength(0);
    }
  });

  it('completes delays alone after they pass, through a timer', () => {
    const cases = [
      ['Animated.delay(200)', 200],
      ['Animated.sequence([Animated.delay(100), Animated.delay(50)])', 150],
      ['Animated.parallel([Animated.delay(100), Animated.delay(300)])', 300],
      ['Animated.stagger(40, [Animated.delay(10), Animated.delay(10)])', 50],
      [
        'Animated.loop(Animated.sequence([Animated.delay(30)]), {iterations: 3})',
        90,
      ],
    ];
    for (const [animation, delayMs] of cases) {
      const {stmt, ctx} = lowerOne(`${animation}.start(() => setN(1));`);
      expect(stmt).toMatchObject({
        kind: 'completeAfter',
        delayMs,
        timerFn: 'er_timer_fn_0',
      });
      expect(printStmts([stmt], '    ')).toEqual([
        `    er_timer_add(${delayMs}, false, er_timer_fn_0);`,
      ]);
      expect(ctx.out.usesTimers).toBe(true);
      expect(ctx.out.timerFns).toEqual([
        {name: 'er_timer_fn_0', body: ['    er_donecb_0(true, NULL);']},
      ]);
    }
  });

  it('never completes an endless loop of delays, but still references its completion', () => {
    const {stmt} = lowerOne(
      'Animated.loop(Animated.sequence([Animated.delay(30)])).start(() => setN(1));',
    );
    expect(stmt).toMatchObject({kind: 'startAnimations', animations: []});
    expect(printStmts([stmt], '    ')).toEqual(['    (void)er_donecb_0;']);
  });

  it('numbers a chain after its completion, whose own starts come first', () => {
    const {stmt} = lowerOne(
      'Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.timing(b, {toValue: 1})]).start(() => { Animated.sequence([Animated.timing(c, {toValue: 1}), Animated.timing(c, {toValue: 0})]).start(); });',
    );
    expect(stmt.completion.name).toBe('er_donecb_0');
    expect(stmt.completion.body[0]).toMatchObject({kind: 'startChain', id: 1});
    expect(stmt.id).toBe(2);
  });

  it('starts nothing for an empty sequence', () => {
    const {stmt} = lowerOne('Animated.sequence([]).start();');
    expect(stmt).toMatchObject({kind: 'startAnimations', animations: []});
    expect(printStmts([stmt], '    ')).toEqual([]);
  });
});
