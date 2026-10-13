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
import {
  printAnimationCallback,
  printStaticDecl,
} from '../compiler/c/functions.mts';
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

/** The on_complete callbacks a body added, as C functions. */
const callbacksOf = ctx => ctx.out.animCbs.flatMap(printAnimationCallback);

/** Lowers a handler body that can drive the animated values a, b and c. */
function lowerBody(source) {
  const statements = parse(source, {sourceType: 'module'}).program.body;
  const animations = new Map(
    ['a', 'b', 'c'].map(name => [name, {cVar: `s_av_${name}`, initial: 0}]),
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
    expect(ctx.out.animCbs).toEqual([{kind: 'chain', chain: stmt}]);
    expect(callbacksOf(ctx).map(fn => fn.name)).toEqual(['er_seqcb_0_1']);
    expect(callbacksOf(ctx)[0].body.slice(0, 2)).toEqual([
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
    expect(ctx.out.effectDecls).toEqual([{kind: 'chain', chain: stmt}]);
    expect(printStaticDecl(ctx.out.effectDecls[0], 48)).toEqual([
      'static int s_loop0_n;',
      'static float s_loop0_from;',
    ]);
    expect(printStmts([stmt], '    ').slice(0, 2)).toEqual([
      '    s_loop0_n = 1;',
      '    s_loop0_from = er_anim_value_get(s_av_b);',
    ]);
    const repeat = callbacksOf(ctx).find(fn => fn.name === 'er_seqcb_0_0');
    expect(repeat.body).toContain('    if (s_loop0_n >= 3)');
    expect(repeat.body).toContain(
      '    er_anim_value_set(s_av_b, s_loop0_from);',
    );
  });

  it("waits out a counted loop's trailing delay after its last pass too", () => {
    const {stmt, ctx} = lowerOne(
      'Animated.loop(Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(300)]), {iterations: 1}).start(() => setN(1));',
    );
    expect(stmt.loop.finalWait).toEqual({delayMs: 300, owner: 0});
    const repeat = callbacksOf(ctx).find(fn => fn.name === 'er_seqcb_1_0');
    expect(repeat.body).toContain('    if (s_loop1_n >= 1)');
    expect(repeat.body).toContain(
      '        app_wait_start(0, 300, er_donecb_0);',
    );
    expect(repeat.body).not.toContain('        er_donecb_0(true, NULL);');
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
    expect(ctx.out.animCbs).toEqual([
      {kind: 'completion', completion: stmt.completion},
    ]);
    expect(callbacksOf(ctx)[0].body.at(-1)).toBe('    app_update();');
  });

  it('completes a zero-iteration loop at once', () => {
    const {stmt} = lowerOne(
      'Animated.loop(Animated.timing(a, {toValue: 1}), {iterations: 0}).start(() => setN(2));',
    );
    expect(stmt).toMatchObject({
      kind: 'completeAfter',
      delayMs: 0,
      owner: null,
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
        owner: 0,
      });
      expect(printStmts([stmt], '    ')).toEqual([
        `    app_wait_start(0, ${delayMs}, er_donecb_0);`,
      ]);
      expect(ctx.out.timerFns).toHaveLength(0);
    }
  });

  it("waits out a sequence's trailing delay before completing", () => {
    const {stmt, ctx} = lowerOne(
      'Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(40), Animated.delay(60)]).start(() => setN(1));',
    );
    expect(stmt.completionWait).toEqual({
      delayMs: 100,
      callback: 'er_seqcb_1_1',
      owner: 0,
    });

    // The last step hands over to the wait, which ends at once if the step was interrupted.
    expect(printStmts([stmt], '    ').join('\n')).toContain(
      'cfg1_0.on_complete = er_seqcb_1_1;',
    );
    const wait = callbacksOf(ctx).find(fn => fn.name === 'er_seqcb_1_1');
    expect(wait.body).toEqual([
      '    if (!finished)',
      '    {',
      '        er_donecb_0(false, NULL);',
      '        return;',
      '    }',
      '    app_wait_start(0, 100, er_donecb_0);',
    ]);
  });

  it('completes a flat start when a delay beside its timing ends', () => {
    const {stmt, ctx} = lowerOne(
      'Animated.stagger(100, [Animated.delay(50), Animated.timing(b, {toValue: 1, duration: 100, delay: 20}), Animated.delay(400)]).start(() => setN(1));',
    );
    expect(stmt.completionWait).toEqual({
      delayMs: 380,
      callback: 'er_waitcb_1',
      owner: 0,
    });
    expect(printStmts([stmt], '    ').join('\n')).toContain(
      'cfg0.on_complete = er_waitcb_1;',
    );
    expect(ctx.out.animCbs.map(callback => callback.kind)).toEqual([
      'completion',
      'completionWait',
    ]);
    expect(callbacksOf(ctx).map(fn => fn.name)).toEqual([
      'er_donecb_0',
      'er_waitcb_1',
    ]);
  });

  it('rejects a callback on a spring or decay beside a delay that runs past its start', () => {
    for (const source of [
      'Animated.parallel([Animated.spring(a, {toValue: 1}), Animated.delay(500)]).start(() => setN(1));',
      'Animated.stagger(100, [Animated.decay(c, {velocity: 1}), Animated.delay(50), Animated.delay(60)]).start(() => setN(1));',
    ]) {
      expect(() => lowerBody(source)).toThrow(
        /beside a longer Animated\.delay is not yet supported/,
      );
    }

    // A delay that is over before the spring starts leaves nothing to wait for, and no callback is fine too.
    for (const source of [
      'Animated.stagger(100, [Animated.delay(50), Animated.spring(a, {toValue: 1})]).start(() => setN(1));',
      'Animated.parallel([Animated.spring(a, {toValue: 1}), Animated.delay(500)]).start();',
    ]) {
      expect(lowerOne(source).stmt.completionWait).toBeNull();
    }
  });

  it('adds no wait when the animation outlasts every delay, or nothing waits on it', () => {
    for (const source of [
      'Animated.parallel([Animated.timing(a, {toValue: 1, duration: 600}), Animated.delay(500)]).start(() => setN(1));',
      'Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(100)]).start();',
    ]) {
      const {stmt, ctx} = lowerOne(source);
      expect(stmt.completionWait).toBeNull();
      expect(ctx.out.timerFns).toHaveLength(0);
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

describe('animation IR: stopping a wait', () => {
  it('gives each composition written apart its own owner, even when they are alike', () => {
    const {stmts, ctx} = lowerBody(
      'Animated.delay(500).start(() => setN(1)); Animated.delay(500).start(() => setN(2)); Animated.timing(a, {toValue: 1}).stop(); Animated.delay(500).stop();',
    );
    expect(stmts[0].owner).toBe(0);
    expect(stmts[1].owner).toBe(1);
    expect(stmts[2]).toMatchObject({
      kind: 'stopAnimations',
      handles: ['s_av_a'],
      waitOwner: null,
    });

    // A stop of a third delay names an owner of its own, which nothing starts (assembly drops the stop).
    expect(stmts[3]).toMatchObject({handles: [], waitOwner: 2});
    expect([...ctx.out.animWaits.values()]).toEqual([
      {owner: 0, isStarted: true},
      {owner: 1, isStarted: true},
      {owner: 2, isStarted: false},
    ]);
  });

  it('detaches the waits before freezing values, and reports them after', () => {
    const {stmts} = lowerBody(
      'const pulse = Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(300)]); pulse.start(() => setN(1)); pulse.stop();',
    );
    expect(printStmts([stmts[1]], '    ')).toEqual([
      '    app_wait_detach(0);',
      '    er_anim_value_set(s_av_a, er_anim_value_get(s_av_a));',
      '    app_wait_report(0);',
    ]);
  });

  it('gives a stop that comes first the owner a later start waits under', () => {
    const {stmts, ctx} = lowerBody(
      'const pulse = Animated.sequence([Animated.timing(a, {toValue: 1}), Animated.delay(300)]); pulse.stop(); pulse.start(() => setN(1));',
    );
    const [stop, start] = stmts;
    expect(stop.waitOwner).toBe(0);
    expect(start.completionWait.owner).toBe(0);
    expect([...ctx.out.animWaits.values()]).toEqual([
      {owner: 0, isStarted: true},
    ]);
  });
});
